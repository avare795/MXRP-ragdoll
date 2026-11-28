/*
 * objects.c - objets de la piste pour marcher dessus (voir objects.h)
 *
 * Disposition d'un objet ODE (dxGeom, ODE simple précision, 64 bits) attendue, vérifiée sur
 * l'objet du relief avant usage : pointeur de table virtuelle, type (+8), corps (+24),
 * position et rotation finales (+40), boîte englobante (+56), puis next / tome (liste de
 * l'espace) et l'espace parent. Les décalages sont retrouvés sur l'objet du relief (type 9) :
 * rien n'est codé en dur sans vérification.
 */
#include "objects.h"
#include "terrain.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

static ObjLogFn g_block_log = NULL;
static int g_block_n = 0;

typedef struct {
    float c[3], R[9], h[3]; /* centre, rotation (lignes), demi-côtés */
    unsigned flags, cat, col; /* drapeaux ODE (+12), catégories (+120) et collisions (+124) */
    uintptr_t space, data; /* espace ODE parent, données (+16) */
} Box;

typedef struct {
    float *tri;   /* 9 floats par triangle (monde) */
    unsigned char *wall; /* 1 : mur (trop raide pour marcher dessus, mais il bloque) */
    int ntri;
    Box *box;
    int nbox;
    float x0, z0, cell;
    int nx, nz;
    int *start;   /* nx*nz+1 */
    int *items;   /* >= 0 : triangle ; < 0 : boîte ~i */
} Db;

static Db *volatile g_db = NULL;
static Db *g_old[4];
static volatile LONG g_busy = 0;
static uintptr_t g_scanned = 0;
static ObjLogFn L = NULL;

/* v1.8.87 : obstacles ODE ajoutés (bottes de foin) : relief, son espace, table d'une boîte du jeu */
static volatile uintptr_t g_hf_geom = 0, g_hf_space = 0, g_box_vt = 0;
static volatile uintptr_t g_ob_pl = 0; /* v1.8.95 : relief de la piste où nos boîtes ont été mises */
static void obstacles_drop(void);

/* décalages trouvés sur l'objet du relief */
static int A_OFF, N_OFF, P_OFF, B_OFF, SZ, PS_OFF, FIRST_OFF;

static int rd(uintptr_t a, void *out, size_t n)
{
    SIZE_T got = 0;
    if (a < 0x10000 || a > 0x7FFFFFFFFFFFull)
        return 0;
    return ReadProcessMemory(GetCurrentProcess(), (void *)a, out, n, &got) && got == n;
}

static uintptr_t rp(uintptr_t a)
{
    uint64_t v = 0;
    if (!rd(a, &v, 8))
        return 0;
    return v > 0x10000 && v < 0x7FFFFFFFFFFFull ? (uintptr_t)v : 0;
}

static int gtype(uintptr_t g)
{
    int32_t t = -1;
    if (!rd(g + 8, &t, 4))
        return -1;
    return t;
}

static int is_space(int t) { return t >= 10 && t <= 13; }

static int geom_ok(uintptr_t g, float aabb[6])
{
    float a[6];
    int t = gtype(g), i;
    if (t < 0 || t > 15)
        return 0;
    if (is_space(t))
        return 1;
    if (!rd(g + A_OFF, a, sizeof(a)))
        return 0;
    for (i = 0; i < 6; ++i)
        if (!isfinite(a[i]) || fabsf(a[i]) > 1e6f)
            return 0;
    if (a[0] > a[1] || a[2] > a[3] || a[4] > a[5])
        return 0;
    if (aabb)
        memcpy(aabb, a, sizeof(a));
    return 1;
}

static void dump(const char *what, uintptr_t a, int len)
{
    uint32_t w[64];
    int i;
    if (!L || len > 256 || !rd(a, w, (size_t)len))
        return;
    for (i = 0; i < len / 4; i += 8)
        L("objets : %s +%03x : %08x %08x %08x %08x %08x %08x %08x %08x", what, i * 4, w[i], w[i + 1], w[i + 2],
          w[i + 3], w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
}

/* ---------- disposition des objets ODE, retrouvée sur l'objet du relief ---------- */
static int detect(uintptr_t ptrloc, uintptr_t aabb, uintptr_t *hf, uintptr_t *space)
{
    uintptr_t g = 0;
    int off;
    float ab[6];
    {
        /* disposition habituelle d'abord (+56), puis autour */
        static const int k_try[] = { 56, 48, 64, 40, 72, 32, 80, 88, 96, 104, 112, 120, 128, 136, 144, 24 };
        int i;
        for (i = 0; i < (int)(sizeof(k_try) / sizeof(k_try[0])); ++i)
            if (gtype(aabb - k_try[i]) == 9) {
                g = aabb - k_try[i];
                A_OFF = k_try[i];
                break;
            }
    }
    if (!g) {
        if (L)
            L("objets : objet ODE du relief pas reconnu (type 9 introuvable)");
        dump("relief", ptrloc - 0xC0, 0xC8);
        return 0;
    }
    SZ = (int)(ptrloc - g);
    if (!geom_ok(g, ab))
        return 0;
    /* liste de l'espace : next puis tome, *tome == nous */
    N_OFF = 0;
    /* v0.40.39 : dans le jeu, next / tome / espace parent sont AVANT la boîte englobante
       (ODE récent : +56 next, +64 tome, +88 espace, +96 boîte, taille 128) */
    for (off = 16; off + 16 <= SZ; off += 8) {
        uintptr_t t;
        if (off + 8 >= A_OFF && off < A_OFF + 24)
            continue;
        t = rp(g + off + 8);
        if (t && rp(t) == g) {
            N_OFF = off;
            break;
        }
    }
    /* position finale : pointe vers pos (4 floats) puis rotation 3x4 */
    P_OFF = 0;
    for (off = 16; off + 8 <= A_OFF; off += 8) {
        uintptr_t p = rp(g + off);
        float f[16];
        if (!p || !rd(p, f, sizeof(f)))
            continue;
        if (f[0] >= ab[0] - 1.0f && f[0] <= ab[1] + 1.0f && f[2] >= ab[4] - 1.0f && f[2] <= ab[5] + 1.0f &&
            fabsf(f[4] * f[4] + f[5] * f[5] + f[6] * f[6] - 1.0f) < 0.01f &&
            fabsf(f[8] * f[8] + f[9] * f[9] + f[10] * f[10] - 1.0f) < 0.01f) {
            P_OFF = off;
            break;
        }
    }
    B_OFF = P_OFF >= 32 ? P_OFF - 16 : 24;
    PS_OFF = 0;
    *space = 0;
    if (N_OFF)
        for (off = 16; off + 8 <= SZ; off += 8) {
            uintptr_t p;
            if ((off >= A_OFF && off < A_OFF + 24) || off == N_OFF || off == N_OFF + 8)
                continue;
            p = rp(g + off);
            if (p && is_space(gtype(p))) {
                PS_OFF = off;
                *space = p;
                break;
            }
        }
    if (L)
        L("objets : objet ODE du relief %p, taille %d, boite +%d, liste +%d, position +%d, corps +%d, espace +%d (%p)",
          (void *)g, SZ, A_OFF, N_OFF, P_OFF, B_OFF, PS_OFF, (void *)*space);
    if (!N_OFF || !PS_OFF) {
        dump("relief", g, SZ < 256 ? SZ : 256);
        return 0;
    }
    *hf = g;
    return 1;
}

/* ---------- copie des surfaces ---------- */
typedef struct {
    float *tri;
    unsigned char *wall;
    int ntri, captri;
    Box *box;
    int nbox, capbox;
    int types[16], dyn, bad_box, bad_mesh, dumped;
    unsigned dyn_cat, dyn_col; int dyn_logged; /* v0.40.83 : filtres de collision des objets mobiles (moto) */
} Build;

static void xf(const float pos[3], const float R[12], const float v[3], float out[3])
{
    int i;
    for (i = 0; i < 3; ++i)
        out[i] = R[i * 4] * v[0] + R[i * 4 + 1] * v[1] + R[i * 4 + 2] * v[2] + pos[i];
}

static void get_posr(uintptr_t g, float pos[3], float R[12])
{
    float f[16];
    uintptr_t p = P_OFF ? rp(g + P_OFF) : 0;
    if (p && rd(p, f, sizeof(f))) {
        memcpy(pos, f, 12);
        memcpy(R, f + 4, 48);
        return;
    }
    memset(pos, 0, 12);
    memset(R, 0, 48);
    R[0] = R[5] = R[10] = 1.0f;
}

static void add_box(Build *b, uintptr_t g, const float ab[6])
{
    float pos[3], R[12], s[3];
    int d, i;
    get_posr(g, pos, R);
    for (d = 0; d <= 32; d += 4) {
        int off = SZ + d; /* côtés juste après l'objet ODE de base */
        int ok = 1;
        if (!rd(g + off, s, sizeof(s)) || !(s[0] > 0.0f && s[1] > 0.0f && s[2] > 0.0f && s[0] < 1e4f && s[1] < 1e4f &&
                                           s[2] < 1e4f))
            continue;
        for (i = 0; i < 3 && ok; ++i) {
            float e = 0.5f * (fabsf(R[i * 4]) * s[0] + fabsf(R[i * 4 + 1]) * s[1] + fabsf(R[i * 4 + 2]) * s[2]);
            float want = 0.5f * (ab[i * 2 + 1] - ab[i * 2]);
            ok = fabsf(e - want) < 0.05f + 0.02f * want;
        }
        if (!ok)
            continue;
        if (b->nbox >= b->capbox) {
            int nc = b->capbox ? b->capbox * 2 : 1024;
            Box *nb = (Box *)realloc(b->box, (size_t)nc * sizeof(Box));
            if (!nb)
                return;
            b->box = nb;
            b->capbox = nc;
        }
        {
            Box *x = &b->box[b->nbox++];
            memcpy(x->c, pos, 12);
            x->flags = x->cat = x->col = 0;
            rd(g + 12, &x->flags, 4);
            rd(g + 120, &x->cat, 4);
            rd(g + 124, &x->col, 4);
            x->space = PS_OFF ? rp(g + PS_OFF) : 0;
            x->data = rp(g + 16);
            {
                /* v0.40.84 : contenu brut d'un cube de 1 m et d'une autre boîte (diagnostic murs invisibles) */
                int unit = fabsf(s[0] - 1.0f) < 0.01f && fabsf(s[1] - 1.0f) < 0.01f && fabsf(s[2] - 1.0f) < 0.01f;
                static int dumped_unit = 0, dumped_other = 0;
                if (g_block_log && ((unit && dumped_unit < 1) || (!unit && dumped_other < 2))) {
                    unsigned w[48];
                    int q;
                    if (unit) ++dumped_unit; else ++dumped_other;
                    if (rd(g, w, sizeof(w)))
                        for (q = 0; q < 48; q += 8)
                            g_block_log("objets : boite %s %.2fx%.2fx%.2f +%03d : %08x %08x %08x %08x %08x %08x %08x %08x",
                                        unit ? "cube" : "autre", s[0], s[1], s[2], q * 4, w[q], w[q + 1], w[q + 2], w[q + 3],
                                        w[q + 4], w[q + 5], w[q + 6], w[q + 7]);
                }
            }
            for (i = 0; i < 3; ++i) {
                x->R[i * 3] = R[i * 4];
                x->R[i * 3 + 1] = R[i * 4 + 1];
                x->R[i * 3 + 2] = R[i * 4 + 2];
                x->h[i] = 0.5f * s[i];
            }
        }
        return;
    }
    ++b->bad_box;
}

/* maillage : nb triangles, nb sommets, triangles, sommets, pas des triangles, pas des sommets */
static int take_mesh(Build *b, const float pos[3], const float R[12], const float ab[6], uint32_t nt, uint32_t nv,
                     uintptr_t pt, uintptr_t pv, uint32_t st, uint32_t sv)
{
    uint32_t *ib;
    uint8_t *vb;
    uint32_t i, k;
    int add = 0;
    int i16 = st == 6; /* indices sur 16 bits */
    if (nt < 1 || nt > 4000000 || nv < 3 || nv > 4000000 || (st < 12 && !i16) || st > 64 || sv < 12 || sv > 64)
        return 0;
    /* quelques triangles d'essai avant de tout copier */
    for (k = 0; k < 3; ++k) {
        uint32_t ix[3];
        uint16_t h3[3];
        uint32_t t = k == 0 ? 0 : k == 1 ? nt / 2 : nt - 1;
        int j;
        if (i16) {
            if (!rd(pt + (uintptr_t)t * st, h3, 6))
                return 0;
            ix[0] = h3[0];
            ix[1] = h3[1];
            ix[2] = h3[2];
        } else if (!rd(pt + (uintptr_t)t * st, ix, 12))
            return 0;
        for (j = 0; j < 3; ++j) {
            float v[3], w[3];
            if (ix[j] >= nv || !rd(pv + (uintptr_t)ix[j] * sv, v, 12))
                return 0;
            xf(pos, R, v, w);
            if (w[0] < ab[0] - 1.0f || w[0] > ab[1] + 1.0f || w[1] < ab[2] - 1.0f || w[1] > ab[3] + 1.0f ||
                w[2] < ab[4] - 1.0f || w[2] > ab[5] + 1.0f)
                return 0;
        }
    }
    ib = (uint32_t *)malloc((size_t)nt * st);
    vb = (uint8_t *)malloc((size_t)nv * sv);
    if (!ib || !vb || !rd(pt, ib, (size_t)nt * st) || !rd(pv, vb, (size_t)nv * sv)) {
        free(ib);
        free(vb);
        return 0;
    }
    for (i = 0; i < nt; ++i) {
        uint32_t ix[3];
        float w[3][3], e1[3], e2[3], n[3], nl;
        int j, bad = 0;
        if (i16) {
            const uint16_t *h = (const uint16_t *)((const uint8_t *)ib + (size_t)i * st);
            ix[0] = h[0];
            ix[1] = h[1];
            ix[2] = h[2];
        } else
            memcpy(ix, (const uint8_t *)ib + (size_t)i * st, 12);
        for (j = 0; j < 3; ++j) {
            if (ix[j] >= nv) {
                bad = 1;
                break;
            }
            xf(pos, R, (const float *)(vb + (size_t)ix[j] * sv), w[j]);
        }
        if (bad)
            continue;
        for (j = 0; j < 3; ++j) {
            e1[j] = w[1][j] - w[0][j];
            e2[j] = w[2][j] - w[0][j];
        }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-8f)
            continue;
        if (b->ntri >= b->captri) {
            int nc = b->captri ? b->captri * 2 : 65536;
            float *nb;
            if (nc > 3000000)
                break;
            nb = (float *)realloc(b->tri, (size_t)nc * 9 * sizeof(float));
            if (!nb)
                break;
            b->tri = nb;
            {
                unsigned char *nw = (unsigned char *)realloc(b->wall, (size_t)nc);
                if (!nw)
                    break;
                b->wall = nw;
            }
            b->captri = nc;
        }
        memcpy(b->tri + (size_t)b->ntri * 9, w, 36);
        b->wall[b->ntri] = fabsf(n[1]) < 0.5f * nl; /* plus de 60° : mur */
        ++b->ntri;
        ++add;
    }
    free(ib);
    free(vb);
    return add > 0 || nt > 0;
}

static int try_mesh_at(Build *b, uintptr_t base, int len, const float pos[3], const float R[12], const float ab[6])
{
    uint8_t buf[0x600];
    int o;
    if (len > (int)sizeof(buf) || !rd(base, buf, (size_t)len))
        return 0;
    /* OPCODE du jeu (vu dans le journal, v0.40.41) : nb triangles, nb sommets, pas, pas, puis
       des pointeurs un peu plus loin ; on essaie chaque paire de pointeurs (validée par
       quelques triangles dans la boîte englobante) */
    for (o = 0; o + 16 <= len; o += 4) {
        uint32_t nt, nv, st, sv;
        int a, c;
        memcpy(&nt, buf + o, 4);
        memcpy(&nv, buf + o + 4, 4);
        if (nt < 1 || nt > 4000000 || nv < 3 || nv > 4000000)
            continue;
        memcpy(&st, buf + o + 8, 4);
        memcpy(&sv, buf + o + 12, 4);
        if (!((st == 12 || st == 6) && (sv == 12 || sv == 16)))
            continue;
        for (a = (o + 16 + 7) & ~7; a + 8 <= len && a < o + 0x60; a += 8)
            for (c = (o + 16 + 7) & ~7; c + 8 <= len && c < o + 0x60; c += 8) {
                uint64_t pt, pv;
                if (a == c)
                    continue;
                memcpy(&pt, buf + a, 8);
                memcpy(&pv, buf + c, 8);
                if (pt < 0x10000 || pt > 0x7FFFFFFFFFFFull || pv < 0x10000 || pv > 0x7FFFFFFFFFFFull)
                    continue;
                if (take_mesh(b, pos, R, ab, nt, nv, (uintptr_t)pt, (uintptr_t)pv, st, sv) ||
                    take_mesh(b, pos, R, ab, nv, nt, (uintptr_t)pt, (uintptr_t)pv, st, sv)) {
                    if (L)
                        L("objets : maillage lu (+%x : %u triangles, %u sommets, pointeurs +%x +%x)", o, nt, nv, a, c);
                    return 1;
                }
            }
    }
    /* ODE récent (dxTriDataBase) : sommets, pas, nombre, puis indices, pas, nombre */
    for (o = 0; o + 32 <= len; o += 8) {
        uint64_t pv, pi;
        int32_t sv, st;
        uint32_t nv, nt;
        memcpy(&pv, buf + o, 8);
        memcpy(&sv, buf + o + 8, 4);
        memcpy(&nv, buf + o + 12, 4);
        memcpy(&pi, buf + o + 16, 8);
        memcpy(&st, buf + o + 24, 4);
        memcpy(&nt, buf + o + 28, 4);
        if (pv < 0x10000 || pv > 0x7FFFFFFFFFFFull || pi < 0x10000 || pi > 0x7FFFFFFFFFFFull)
            continue;
        if (sv < 12 || sv > 64 || (st != 6 && (st < 12 || st > 64)))
            continue;
        if (take_mesh(b, pos, R, ab, nt, nv, (uintptr_t)pi, (uintptr_t)pv, (uint32_t)st, (uint32_t)sv))
            return 1;
    }
    for (o = 0; o + 32 <= len; o += 8) {
        uint32_t nt, nv, st, sv;
        uint64_t p1, p2;
        memcpy(&nt, buf + o, 4);
        memcpy(&nv, buf + o + 4, 4);
        if (nt < 1 || nt > 4000000 || nv < 3 || nv > 4000000)
            continue;
        memcpy(&p1, buf + o + 8, 8);
        memcpy(&p2, buf + o + 16, 8);
        if (p1 < 0x10000 || p1 > 0x7FFFFFFFFFFFull || p2 < 0x10000 || p2 > 0x7FFFFFFFFFFFull)
            continue;
        memcpy(&st, buf + o + 24, 4);
        memcpy(&sv, buf + o + 28, 4);
        if (!(st == 12 && (sv == 12 || sv == 16))) {
            st = 12;
            sv = 12;
        }
        if (take_mesh(b, pos, R, ab, nt, nv, (uintptr_t)p1, (uintptr_t)p2, st, sv) ||
            take_mesh(b, pos, R, ab, nt, nv, (uintptr_t)p2, (uintptr_t)p1, st, sv) ||
            take_mesh(b, pos, R, ab, nt, nv, (uintptr_t)p1, (uintptr_t)p2, 12, 16))
            return 1;
    }
    return 0;
}

static void add_mesh(Build *b, uintptr_t g, const float ab[6])
{
    float pos[3], R[12];
    int off, o2;
    get_posr(g, pos, R);
    for (off = SZ; off < SZ + 0x100; off += 8) {
        uintptr_t d = rp(g + off);
        if (!d)
            continue;
        if (try_mesh_at(b, d, 0x600, pos, R, ab))
            return;
        for (o2 = 0; o2 < 0x200; o2 += 8) {
            uintptr_t d2 = rp(d + o2);
            if (d2 && d2 != g && try_mesh_at(b, d2, 0x400, pos, R, ab))
                return;
        }
    }
    if (try_mesh_at(b, g + SZ, 0x200, pos, R, ab))
        return;
    ++b->bad_mesh;
    if (b->dumped < 2) {
        uintptr_t d = rp(g + SZ + 0x20);
        ++b->dumped;
        if (L)
            L("objets : maillage non lu %p, boite %.1f %.1f / %.1f %.1f / %.1f %.1f", (void *)g, ab[0], ab[1], ab[2], ab[3],
              ab[4], ab[5]);
        dump("maillage", g + SZ, 0x40);
        if (d)
            dump("donnees", d, 0x100);
    }
}

static void visit(Build *b, uintptr_t sp, int depth, int *count)
{
    uintptr_t g = rp(sp + FIRST_OFF);
    while (g && *count < 200000) {
        float ab[6];
        int t;
        ++*count;
        if (!geom_ok(g, ab))
            break;
        t = gtype(g);
        b->types[t & 15]++;
        if (is_space(t)) {
            if (depth < 6)
                visit(b, g, depth + 1, count);
        } else if (rp(g + B_OFF)) {
            unsigned fl = 0, ca = 0, co = 0;
            if (t == 1 && !g_box_vt)
                g_box_vt = rp(g); /* v1.8.93 : boîte de la moto (pistes sans boîte, ex. Motopark) */
            ++b->dyn; /* attaché à un corps : moto, pilote */
            rd(g + 12, &fl, 4);
            rd(g + 120, &ca, 4);
            rd(g + 124, &co, 4);
            b->dyn_cat |= ca;
            b->dyn_col |= co;
            if (g_block_log && b->dyn_logged++ < 6)
                g_block_log("objets : objet mobile type %d, drapeaux %x, categories %x, collisions %x", t, fl, ca, co);
        } else if (t == 1) {
            if (!g_box_vt)
                g_box_vt = rp(g); /* table virtuelle d'une boîte ODE du jeu (dxBox) */
            add_box(b, g, ab);
        } else if (t == 8) {
            add_mesh(b, g, ab);
        }
        g = rp(g + N_OFF);
    }
}

/* ---------- grille de recherche ---------- */
static void item_bounds(const Build *b, int it, float *x0, float *x1, float *z0, float *z1)
{
    if (it >= 0) {
        const float *t = b->tri + (size_t)it * 9;
        *x0 = fminf(t[0], fminf(t[3], t[6]));
        *x1 = fmaxf(t[0], fmaxf(t[3], t[6]));
        *z0 = fminf(t[2], fminf(t[5], t[8]));
        *z1 = fmaxf(t[2], fmaxf(t[5], t[8]));
    } else {
        const Box *x = &b->box[~it];
        float ex = fabsf(x->R[0]) * x->h[0] + fabsf(x->R[1]) * x->h[1] + fabsf(x->R[2]) * x->h[2];
        float ez = fabsf(x->R[6]) * x->h[0] + fabsf(x->R[7]) * x->h[1] + fabsf(x->R[8]) * x->h[2];
        *x0 = x->c[0] - ex;
        *x1 = x->c[0] + ex;
        *z0 = x->c[2] - ez;
        *z1 = x->c[2] + ez;
    }
}

static Db *make_db(Build *b)
{
    Db *d = (Db *)calloc(1, sizeof(Db));
    float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
    int i, n = b->ntri + b->nbox, pass;
    long long refs = 0;
    if (!d)
        return NULL;
    for (i = 0; i < n; ++i) {
        float a, bb, c, e;
        item_bounds(b, i < b->ntri ? i : ~(i - b->ntri), &a, &bb, &c, &e);
        x0 = fminf(x0, a);
        x1 = fmaxf(x1, bb);
        z0 = fminf(z0, c);
        z1 = fmaxf(z1, e);
    }
    d->cell = 2.0f;
    while ((x1 - x0) / d->cell > 4000.0f || (z1 - z0) / d->cell > 4000.0f)
        d->cell *= 2.0f;
    d->x0 = x0;
    d->z0 = z0;
    d->nx = n ? (int)((x1 - x0) / d->cell) + 1 : 1;
    d->nz = n ? (int)((z1 - z0) / d->cell) + 1 : 1;
    d->start = (int *)calloc((size_t)d->nx * d->nz + 1, sizeof(int));
    if (!d->start) {
        free(d);
        return NULL;
    }
    for (pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            long long c;
            int k, sum = 0;
            for (k = 0; k < d->nx * d->nz; ++k) {
                c = d->start[k];
                d->start[k] = sum;
                sum += (int)c;
            }
            d->start[d->nx * d->nz] = sum;
            d->items = (int *)malloc((size_t)(sum ? sum : 1) * sizeof(int));
            if (!d->items) {
                free(d->start);
                free(d);
                return NULL;
            }
            refs = 0;
        }
        for (i = 0; i < n; ++i) {
            int it = i < b->ntri ? i : ~(i - b->ntri);
            float a, bb, c, e;
            int cx0, cx1, cz0, cz1, cx, cz;
            item_bounds(b, it, &a, &bb, &c, &e);
            cx0 = (int)((a - x0) / d->cell);
            cx1 = (int)((bb - x0) / d->cell);
            cz0 = (int)((c - z0) / d->cell);
            cz1 = (int)((e - z0) / d->cell);
            if ((long long)(cx1 - cx0 + 1) * (cz1 - cz0 + 1) > 4096)
                continue; /* trop grand : ignoré */
            for (cz = cz0; cz <= cz1; ++cz)
                for (cx = cx0; cx <= cx1; ++cx) {
                    int k = cz * d->nx + cx;
                    if (cx < 0 || cz < 0 || cx >= d->nx || cz >= d->nz)
                        continue;
                    if (pass == 1)
                        d->items[d->start[k]] = it;
                    ++d->start[k];
                    ++refs;
                }
        }
        if (pass == 1) {
            /* start[k] a avancé jusqu'à la fin de la case k : on le remet au début */
            int k;
            for (k = d->nx * d->nz - 1; k > 0; --k)
                d->start[k] = d->start[k - 1];
            d->start[0] = 0;
        }
    }
    (void)refs;
    d->tri = b->tri;
    d->wall = b->wall;
    d->ntri = b->ntri;
    d->box = b->box;
    d->nbox = b->nbox;
    return d;
}

static void free_db(Db *d)
{
    if (!d)
        return;
    free(d->tri);
    free(d->wall);
    free(d->box);
    free(d->start);
    free(d->items);
    free(d);
}

static uintptr_t g_job_pl, g_job_ab;

static volatile uintptr_t g_root = 0;
static int g_col_diag = 0, g_col_diag_n = 0; /* espace ODE racine (relief, objets, moto) */

/* v0.41.02 : objets ODE attachés à ces corps (la moto) : collisions coupées ou remises
   (drapeau ODE « actif », 0x10). Retourne le nombre d'objets changés. */
static int collide_walk(uintptr_t sp, const uintptr_t *bodies, int n, int on, int depth, int *count)
{
    uintptr_t g = rp(sp + FIRST_OFF);
    int done = 0;
    while (g && *count < 20000) {
        int t, k;
        ++*count;
        if (!geom_ok(g, NULL))
            break;
        t = gtype(g);
        if (is_space(t)) {
            if (depth < 6)
                done += collide_walk(g, bodies, n, on, depth + 1, count);
        } else {
            uintptr_t body = rp(g + B_OFF);
            if (g_col_diag && L && g_col_diag_n < 30) {
                /* diagnostic : où ce geom garde-t-il un pointeur vers une pièce de la moto ? */
                uintptr_t w[24];
                int o, j;
                ++g_col_diag_n;
                if (rd(g, w, sizeof(w)))
                    for (o = 0; o < 24; ++o)
                        for (j = 0; j < n; ++j)
                            if (w[o] == bodies[j] || (w[o] > bodies[j] - 0x200 && w[o] < bodies[j] + 0x200))
                                L("objets : geom %p type %d : +%d = %p (piece %p, ecart %d)", (void *)g, t, o * 8,
                                  (void *)w[o], (void *)bodies[j], (int)(w[o] - bodies[j]));
                if (body)
                    L("objets : geom %p type %d, corps (+%d) %p", (void *)g, t, B_OFF, (void *)body);
            }
            /* v0.41.11 : vu en jeu, l'objet de collision pointe 8 octets avant l'adresse de la
               pièce utilisée par le plugin (début du dxBody ODE) */
            for (k = 0; body && k < n; ++k)
                if (bodies[k] == body || bodies[k] == body + 8) {
                    unsigned fl = 0, nf;
                    if (rd(g + 12, &fl, 4) && fl < 0x100) {
                        nf = on ? (fl | 0x10u) : (fl & ~0x10u);
                        if (nf != fl) {
                            SIZE_T w = 0;
                            if (WriteProcessMemory(GetCurrentProcess(), (void *)(g + 12), &nf, 4, &w) && w == 4)
                                ++done;
                        }
                    }
                    break;
                }
        }
        g = rp(g + N_OFF);
    }
    return done;
}

int objects_body_collide(const uintptr_t *bodies, int n, int on)
{
    int count = 0, r, k;
    if (!g_root || !FIRST_OFF || n <= 0)
        return -1;
    r = collide_walk(g_root, bodies, n, on, 0, &count);
    if (r == 0 && !on && !g_col_diag && L && 0) {
        g_col_diag = 1;
        for (k = 0; k < n; ++k)
            L("objets : piece de la moto %d = %p", k, (void *)bodies[k]);
        count = 0;
        collide_walk(g_root, bodies, n, on, 0, &count);
        L("objets : %d objets parcourus", count);
    }
    return r;
}


/* v1.8.87 : diagnostic des espaces ODE (pour y ajouter les bottes de foin comme obstacles) */
static void space_diag(uintptr_t sp, int depth, uintptr_t hf)
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL), vt = rp(sp), g;
    int32_t cnt = -1;
    unsigned fl = 0, ca = 0, co = 0;
    int n = 0, nb = 0, nbody = 0, has_hf = 0;
    uint64_t v[12];
    int k;
    rd(sp + 128, &cnt, 4);
    rd(sp + 12, &fl, 4);
    rd(sp + 120, &ca, 4);
    rd(sp + 124, &co, 4);
    for (g = rp(sp + FIRST_OFF); g && n < 5000; g = rp(g + N_OFF)) {
        ++n;
        if (rp(g + B_OFF))
            ++nbody;
        if (gtype(g) == 1)
            ++nb;
        if (g == hf)
            has_hf = 1;
    }
    memset(v, 0, sizeof(v));
    rd(vt, v, sizeof(v));
    L("objets : diag espace %p prof %d type %d, %d objets (compte %d), %d boites, %d sur corps%s, drapeaux %x, cat %x col %x, parent %p",
      (void *)sp, depth, gtype(sp), n, cnt, nb, nbody, has_hf ? ", RELIEF" : "", fl, ca, co, (void *)rp(sp + PS_OFF));
    L("objets : diag espace %p table +%llx : %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx", (void *)sp,
      (unsigned long long)(vt - base), (unsigned long long)(v[0] - base), (unsigned long long)(v[1] - base),
      (unsigned long long)(v[2] - base), (unsigned long long)(v[3] - base), (unsigned long long)(v[4] - base),
      (unsigned long long)(v[5] - base), (unsigned long long)(v[6] - base), (unsigned long long)(v[7] - base),
      (unsigned long long)(v[8] - base), (unsigned long long)(v[9] - base));
    dump("espace", sp + 128, 64);
    for (g = rp(sp + FIRST_OFF), k = 0; g && k < 5000; g = rp(g + N_OFF), ++k)
        if (is_space(gtype(g)) && depth < 4)
            space_diag(g, depth + 1, hf);
}

static DWORD WINAPI scan_thread(LPVOID arg)
{
    uintptr_t hf = 0, sp = 0, root, cur;
    DWORD t0 = GetTickCount();
    Build b;
    int count = 0, i;
    (void)arg;
    memset(&b, 0, sizeof(b));
    if (!detect(g_job_pl, g_job_ab, &hf, &sp))
        goto done;
    /* début de la liste : on remonte par tome jusqu'au champ « premier » de l'espace */
    cur = hf;
    FIRST_OFF = 0;
    for (i = 0; i < 300000; ++i) {
        uintptr_t t = rp(cur + N_OFF + 8), prev;
        if (!t)
            break;
        if (t >= sp && t < sp + 0x400) {
            FIRST_OFF = (int)(t - sp);
            break;
        }
        prev = t - N_OFF;
        if (!geom_ok(prev, NULL) || rp(prev + N_OFF) != cur)
            break;
        cur = prev;
    }
    if (!FIRST_OFF) {
        if (L)
            L("objets : debut de la liste de l'espace pas trouve");
        dump("espace", sp, 0x100);
        goto done;
    }
    root = sp;
    for (i = 0; i < 8; ++i) {
        uintptr_t p = rp(root + PS_OFF);
        if (!p || !is_space(gtype(p)))
            break;
        root = p;
    }
    g_root = root;
    visit(&b, root, 0, &count);
    /* v1.8.95 : nos boîtes vont à la racine de la physique (espace racine, filtres ouverts) : en
       ligne, l'espace du relief n'est jamais testé contre la moto (filtres des espaces) */
    if (g_hf_geom && g_hf_geom != hf)
        obstacles_drop(); /* autre relief : autre monde ODE */
    g_hf_space = root;
    g_hf_geom = hf;
    g_ob_pl = g_job_pl;
    if (L) {
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        unsigned fl = 0, ca = 0, co = 0;
        rd(hf + 12, &fl, 4);
        rd(hf + 120, &ca, 4);
        rd(hf + 124, &co, 4);
        L("objets : diag relief %p table +%llx, drapeaux %x, cat %x col %x, donnees %p, espace %p, premier +%d",
          (void *)hf, (unsigned long long)(rp(hf) - base), fl, ca, co, (void *)rp(hf + 16), (void *)rp(hf + PS_OFF), FIRST_OFF);
        if (rp(hf + 16))
            dump("donnees du relief", rp(hf + 16), 64);
        space_diag(root, 0, hf);
    }
    if (L)
        L("objets : %d objets ODE (boites %d, maillages %d, spheres %d, capsules %d, cylindres %d, convexes %d, "
          "espaces %d, relief %d), %d attaches a un corps",
          count, b.types[1], b.types[8], b.types[0], b.types[2], b.types[3], b.types[6],
          b.types[10] + b.types[11] + b.types[12] + b.types[13], b.types[9], b.dyn);
    {
        /* v0.40.83 : la moto ne bute pas dans certains objets (murs invisibles a pied) : on
           garde seulement les boites avec lesquelles la moto entre en collision (filtres ODE :
           categories de l'une & collisions de l'autre) */
        int kept = 0, dropped = 0, j, off_dis = 0, off_mark = 0;
        int sane = b.dyn_cat && b.dyn_col && b.dyn_cat != 0xFFFFFFFFu;
        /* v0.40.85 : vu dans le jeu (Copperhead Canyon), les murs invisibles sont des cubes de
           exactement 1 m, alignés tous les 2 m, rangés seuls dans leur espace ODE : des
           repères (limites de piste, détection), que la moto traverse. On ignore aussi les
           objets désactivés (drapeau ODE « actif » absent : le jeu ne les teste pas). */
        uintptr_t msp[16];
        int nmsp = 0, q;
        {
            uintptr_t sp[32];
            int nsp = 0, cnt[32] = { 0 }, cube[32] = { 0 };
            for (j = 0; j < b.nbox; ++j) {
                const Box *x = &b.box[j];
                int unit = fabsf(x->h[0] - 0.5f) < 0.005f && fabsf(x->h[1] - 0.5f) < 0.005f && fabsf(x->h[2] - 0.5f) < 0.005f;
                for (q = 0; q < nsp && sp[q] != x->space; ++q)
                    ;
                if (q == nsp && nsp < 32)
                    sp[nsp++] = x->space;
                if (q < 32) {
                    ++cnt[q];
                    cube[q] += unit;
                }
            }
            for (q = 0; q < nsp; ++q)
                if (sp[q] && cnt[q] >= 6 && cube[q] == cnt[q] && nmsp < 16)
                    msp[nmsp++] = sp[q];
        }
        for (j = 0; j < b.nbox; ++j) {
            const Box *x = &b.box[j];
            int coll = (x->cat & b.dyn_col) || (x->col & b.dyn_cat), mark = 0;
            for (q = 0; q < nmsp; ++q)
                mark |= x->space == msp[q];
            if (x->flags < 0x100 && !(x->flags & 0x10)) {
                ++off_dis;
                ++dropped;
            } else if (mark) {
                ++off_mark;
                ++dropped;
            } else if (!sane || coll)
                b.box[kept++] = *x;
            else
                ++dropped;
        }
        if (L)
            L("objets : %d boites ignorees : %d desactivees, %d reperes (cubes de 1 m alignes)", dropped, off_dis, off_mark);
        if (L)
            for (q = 0; q < nmsp; ++q) {
                unsigned sf = 0;
                rd(msp[q] + 12, &sf, 4);
                L("objets : espace de reperes %p (drapeaux %x)", (void *)msp[q], sf);
            }
        if (L) {
            /* v0.40.84 : boîtes par espace ODE (les cubes traversés par la moto sont peut-être à part) */
            uintptr_t sp[16];
            int nsp = 0, cnt[16] = { 0 }, cube[16] = { 0 }, q;
            for (j = 0; j < b.nbox; ++j) {
                const Box *x = &b.box[j];
                int unit = fabsf(x->h[0] - 0.5f) < 0.01f && fabsf(x->h[1] - 0.5f) < 0.01f && fabsf(x->h[2] - 0.5f) < 0.01f;
                for (q = 0; q < nsp && sp[q] != x->space; ++q)
                    ;
                if (q == nsp && nsp < 16)
                    sp[nsp++] = x->space;
                if (q < 16) {
                    ++cnt[q];
                    cube[q] += unit;
                }
            }
            for (q = 0; q < nsp; ++q)
                L("objets : espace %p : %d boites dont %d cubes de 1 m", (void *)sp[q], cnt[q], cube[q]);
        }
        if (L)
            L("objets : moto categories %x collisions %x : %d boites gardees, %d ignorees (la moto les traverse)%s",
              b.dyn_cat, b.dyn_col, kept, dropped, sane ? "" : " (filtre non applique)");
        b.nbox = kept;
    }
    {
        Db *d = make_db(&b);
        if (d) {
            for (i = 3; i > 0; --i)
                g_old[i] = g_old[i - 1];
            free_db(g_old[3]);
            g_old[3] = NULL;
            g_old[0] = g_db;
            g_db = d;
            b.tri = NULL;
            b.wall = NULL;
            b.box = NULL;
        }
        if (L && d)
            for (i = 0; i < d->nbox && i < 6; ++i)
                L("objets : boite %d en (%.1f %.1f %.1f), %.2f x %.2f x %.2f m, drapeaux %x, categories %x, collisions %x", i,
                  d->box[i].c[0], d->box[i].c[1], d->box[i].c[2], 2 * d->box[i].h[0], 2 * d->box[i].h[1], 2 * d->box[i].h[2],
                  d->box[i].flags, d->box[i].cat, d->box[i].col);
        if (L)
            L("objets : %d boites et %d triangles pour marcher dessus (%d boites, %d maillages non lus) en %.2f s",
              b.nbox, b.ntri, b.bad_box, b.bad_mesh, (GetTickCount() - t0) / 1000.0f);
    }
done:
    free(b.tri);
    free(b.wall);
    free(b.box);
    InterlockedExchange(&g_busy, 0);
    return 0;
}

void objects_forget(void)
{
    g_root = 0; /* espace ODE de la piste précédente : plus jamais parcouru */
    g_scanned = 0; /* v1.8.95 : relu (nos boîtes restent : même monde tant que l'événement continue) */
}

/* v1.8.95 : nouvel événement (session) : le jeu recrée sa physique ; nos boîtes sont oubliées */
void objects_obstacles_reset(void)
{
    obstacles_drop();
    g_scanned = 0;
    g_root = 0;
}

void objects_poll(ObjLogFn log)
{
    uintptr_t pl = 0, ab = 0;
    g_block_log = log;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 1000)
        return;
    last = now;
    if (g_ob_pl && (!terrain_geom_info(&pl, &ab) || pl != g_ob_pl))
        obstacles_drop(); /* autre relief : nos boîtes sont libérées par le jeu avec son espace */
    if (!terrain_geom_info(&pl, &ab)) {
        if (g_db) {
            g_old[1] = g_old[0];
            g_old[0] = g_db;
            g_db = NULL; /* autre piste : libéré plus tard */
        }
        g_scanned = 0;
        g_root = 0;
        return;
    }
    if (pl != g_scanned)
        g_root = 0;
    if (pl == g_scanned || InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return;
    g_scanned = pl;
    L = log;
    g_job_pl = pl;
    g_job_ab = ab;
    {
        HANDLE t = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            InterlockedExchange(&g_busy, 0);
    }
}


/* ---------- v1.8.87 : obstacles (bottes de foin) dans la physique du jeu ----------
   ODE 0.13 simple précision (chemin de compilation dans l'exe) ; tous les espaces de la piste sont
   des espaces simples (type 10, vu en jeu). Une boîte (dxBox, 144 octets : objet de base de 128
   puis ses côtés) est copiée d'une boîte du jeu (même table virtuelle), allouée avec le malloc
   du jeu (MSVCR90) : quand le jeu détruit l'espace (autre piste), il la libère lui-même. On
   l'ajoute en tête de l'espace du relief comme dxSpace::add, et on la marque déplacée comme
   dGeomMoved. Jamais retirée : désactivée (drapeau ODE « actif ») quand elle ne sert plus. */
#define GF_DIRTY 1
#define GF_AABB_BAD 4
#define GF_PLACEABLE 8
#define GF_ENABLED 16
#define OBST_MAX 1024 /* v1.8.119 : 512 et plus : rampes */
static struct {
    uintptr_t g, posr, space, hf;
    float c[3], R[9], h[3];
    int on;
    DWORD checked; /* v1.8.99 : dernière vérification (étalées sur la seconde) */
} g_ob[OBST_MAX];
static ObjLogFn g_ob_log = NULL;
static int g_ob_made = 0;

static int wr(uintptr_t a, const void *v, size_t n)
{
    SIZE_T w = 0;
    if (a < 0x10000 || a > 0x7FFFFFFFFFFFull)
        return 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)a, v, n, &w) && w == n;
}
static int wp(uintptr_t a, uintptr_t v)
{
    uint64_t x = v;
    return wr(a, &x, 8);
}

static void obstacles_drop(void)
{
    int i;
    for (i = 0; i < OBST_MAX; ++i)
        g_ob[i].g = 0; /* (libérées par le jeu avec leur espace : on n'y touche plus) */
    g_hf_geom = 0;
    g_hf_space = 0;
    g_ob_pl = 0;
}

/* dGeomMoved : la boîte et ses espaces parents marqués « sales », remis en tête de leur liste */
static void geom_moved(uintptr_t g)
{
    int guard = 0;
    uintptr_t parent = rp(g + PS_OFF);
    unsigned fl = 0;
    while (parent && guard++ < 8) {
        uintptr_t nx, tm, first;
        rd(g + 12, &fl, 4);
        if (fl & GF_DIRTY)
            break;
        fl |= GF_DIRTY | GF_AABB_BAD;
        wr(g + 12, &fl, 4);
        /* dxSpace::dirty : retiré de la liste, remis en tête */
        nx = rp(g + N_OFF);
        tm = rp(g + N_OFF + 8);
        if (tm) {
            if (nx)
                wp(nx + N_OFF + 8, tm);
            wp(tm, nx);
        }
        first = rp(parent + FIRST_OFF);
        wp(g + N_OFF, first);
        wp(g + N_OFF + 8, parent + FIRST_OFF);
        if (first)
            wp(first + N_OFF + 8, g + N_OFF);
        wp(parent + FIRST_OFF, g);
        g = parent;
        parent = rp(g + PS_OFF);
    }
    for (guard = 0; g && guard < 8; ++guard) {
        rd(g + 12, &fl, 4);
        fl |= GF_DIRTY | GF_AABB_BAD;
        wr(g + 12, &fl, 4);
        g = rp(g + PS_OFF);
    }
}

static void *game_malloc(size_t n)
{
    static void *(*fn)(size_t) = NULL;
    if (!fn) {
        HMODULE m = GetModuleHandleA("MSVCR90.dll");
        if (m)
            fn = (void *(*)(size_t))GetProcAddress(m, "malloc");
    }
    return fn ? fn(n) : NULL;
}

/* la boîte i est-elle toujours la nôtre, dans l'espace du relief de cette piste ? */
static int ob_alive(int i)
{
    uintptr_t g = g_ob[i].g;
    return g && g_hf_geom && g_ob[i].hf == g_hf_geom && g_box_vt && rp(g) == g_box_vt && gtype(g) == 1 &&
           rp(g + PS_OFF) == g_ob[i].space && rp(g + P_OFF) == g_ob[i].posr;
}

static uintptr_t ob_make(int i)
{
    uintptr_t sp = g_hf_space, g, posr;
    uint8_t buf[144];
    float pr[16];
    int32_t cnt = 0;
    if (!sp || !g_box_vt || !FIRST_OFF || N_OFF != 56 || PS_OFF != 88 || P_OFF != 40 || SZ != 128 ||
        gtype(sp) != 10) {
        if (g_ob_log && g_ob_made < 3) {
            ++g_ob_made;
            g_ob_log("objets : obstacle pas cree : espace %p (type %d), boite %p, premier +%d, liste +%d, parent +%d, position +%d, taille %d",
                     (void *)sp, sp ? gtype(sp) : -1, (void *)g_box_vt, FIRST_OFF, N_OFF, PS_OFF, P_OFF, SZ);
        }
        return 0;
    }
    g = (uintptr_t)game_malloc(144);
    posr = (uintptr_t)game_malloc(64);
    if (!g || !posr) {
        if (g_ob_log && g_ob_made < 3) {
            ++g_ob_made;
            g_ob_log("objets : obstacle pas cree : malloc du jeu (MSVCR90) %s", GetModuleHandleA("MSVCR90.dll") ? "trouve" : "pas trouve");
        }
        return 0; /* (au pire 144 octets perdus) */
    }
    memset(buf, 0, sizeof(buf));
    *(uint64_t *)(buf + 0) = g_box_vt;
    *(int32_t *)(buf + 8) = 1; /* dBoxClass */
    *(uint32_t *)(buf + 12) = GF_DIRTY | GF_AABB_BAD | GF_PLACEABLE | GF_ENABLED;
    *(uint64_t *)(buf + 40) = posr;
    *(uint64_t *)(buf + 88) = sp;
    /* catégorie des objets de la piste ; v1.8.95 : collisions avec les motos et pilotes, hors ligne
       (catégories 0x100, 0x200) comme en ligne (0x400, 0x800) */
    *(uint32_t *)(buf + 120) = 2;
    *(uint32_t *)(buf + 124) = 0xF00;
    *(float *)(buf + 128) = 2.0f * g_ob[i].h[0];
    *(float *)(buf + 132) = 2.0f * g_ob[i].h[1];
    *(float *)(buf + 136) = 2.0f * g_ob[i].h[2];
    memset(pr, 0, sizeof(pr));
    pr[0] = g_ob[i].c[0];
    pr[1] = g_ob[i].c[1];
    pr[2] = g_ob[i].c[2];
    {
        int r, c;
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c)
                pr[4 + r * 4 + c] = g_ob[i].R[r * 3 + c];
    }
    memcpy((void *)posr, pr, 64);
    {
        /* dxSpace::add : en tête de la liste, compte + 1, énumération oubliée, puis dGeomMoved(espace) */
        uintptr_t first = rp(sp + FIRST_OFF);
        *(uint64_t *)(buf + 56) = first;
        *(uint64_t *)(buf + 64) = sp + FIRST_OFF;
        memcpy((void *)g, buf, sizeof(buf));
        if (first)
            wp(first + N_OFF + 8, g + N_OFF);
        wp(sp + FIRST_OFF, g);
        rd(sp + FIRST_OFF - 8, &cnt, 4);
        ++cnt;
        wr(sp + FIRST_OFF - 8, &cnt, 4);
        wp(sp + FIRST_OFF + 24, 0); /* current_geom */
        {
            /* l'espace du relief et ses parents : sales */
            unsigned fl = 0;
            rd(sp + 12, &fl, 4);
            if (!(fl & GF_DIRTY))
                geom_moved(sp);
        }
    }
    g_ob[i].g = g;
    g_ob[i].posr = posr;
    g_ob[i].space = sp;
    g_ob[i].hf = g_hf_geom;
    if (g_ob_log && g_ob_made++ < 6)
        g_ob_log("objets : obstacle %d ajoute a la physique (%p, %.2f x %.2f x %.2f m) en (%.1f %.1f %.1f)", i, (void *)g,
                 2 * g_ob[i].h[0], 2 * g_ob[i].h[1], 2 * g_ob[i].h[2], g_ob[i].c[0], g_ob[i].c[1], g_ob[i].c[2]);
    return g;
}

int objects_obstacle(int i, int on, const float c[3], const float R[9], const float h[3])
{
    unsigned fl = 0;
    int moved;
    if (i < 0 || i >= OBST_MAX)
        return 0;
    {
        /* v1.8.95 : jamais d'écriture si le relief a changé (monde ODE libéré) */
        uintptr_t pl = 0, ab = 0;
        if (g_ob_pl && (!terrain_geom_info(&pl, &ab) || pl != g_ob_pl)) {
            obstacles_drop();
            return 0;
        }
    }
    /* v1.8.99 : boîte déjà en place et pas bougée : rien à lire ni à écrire (avant : 4 lectures
       mémoire du système par boîte et par appel, soit des centaines par image avec beaucoup de bottes).
       Sa validité est revérifiée une fois par seconde. */
    if (on && g_ob[i].g && g_ob[i].on && !memcmp(g_ob[i].c, c, 12) && !memcmp(g_ob[i].R, R, 36) &&
        !memcmp(g_ob[i].h, h, 12) && g_ob[i].hf == g_hf_geom && GetTickCount() - g_ob[i].checked < 1000)
        return 1;
    if (g_ob[i].g && !ob_alive(i))
        g_ob[i].g = 0; /* plus la nôtre (autre piste) */
    g_ob[i].checked = GetTickCount() - (DWORD)((i * 37) % 900);
    if (!on) {
        if (g_ob[i].g && g_ob[i].on && rd(g_ob[i].g + 12, &fl, 4)) {
            fl &= ~(unsigned)GF_ENABLED;
            wr(g_ob[i].g + 12, &fl, 4);
        }
        g_ob[i].on = 0;
        return 1;
    }
    moved = !g_ob[i].on || memcmp(g_ob[i].c, c, 12) || memcmp(g_ob[i].R, R, 36) || memcmp(g_ob[i].h, h, 12);
    memcpy(g_ob[i].c, c, 12);
    memcpy(g_ob[i].R, R, 36);
    if (g_ob[i].g && memcmp(g_ob[i].h, h, 12)) {
        float sd[3] = { 2.0f * h[0], 2.0f * h[1], 2.0f * h[2] };
        wr(g_ob[i].g + 128, sd, 12);
    }
    memcpy(g_ob[i].h, h, 12);
    if (!g_ob[i].g) {
        if (!ob_make(i))
            return 0;
        g_ob[i].on = 1;
        return 1;
    }
    if (moved) {
        float pr[16];
        int r, k;
        memset(pr, 0, sizeof(pr));
        memcpy(pr, c, 12);
        for (r = 0; r < 3; ++r)
            for (k = 0; k < 3; ++k)
                pr[4 + r * 4 + k] = R[r * 3 + k];
        wr(g_ob[i].posr, pr, 64);
        if (rd(g_ob[i].g + 12, &fl, 4) && !(fl & GF_ENABLED)) {
            fl |= GF_ENABLED;
            wr(g_ob[i].g + 12, &fl, 4);
        }
        geom_moved(g_ob[i].g);
    }
    g_ob[i].on = 1;
    return 1;
}

void objects_obstacle_log(ObjLogFn log)
{
    g_ob_log = log;
}

/* v1.8.120 : relief du jeu changé (rampes) : sa boîte englobante sera recalculée par ODE */
void objects_relief_moved(void)
{
    uintptr_t g = g_hf_geom;
    unsigned fl = 0;
    if (!g || !rd(g + 12, &fl, 4))
        return;
    geom_moved(g);
    if (rd(g + 12, &fl, 4)) {
        fl |= GF_DIRTY | GF_AABB_BAD;
        wr(g + 12, &fl, 4);
    }
}

int objects_hf_data_off(uintptr_t hdr)
{
    uintptr_t g = g_hf_geom;
    int off;
    if (!g || !hdr)
        return -1;
    for (off = 8; off < 0x300; off += 8)
        if (rp(g + (uintptr_t)off) == hdr)
            return off;
    return -1;
}

int objects_geom_dirty(uintptr_t g)
{
    unsigned fl = 0;
    if (!g || !geom_ok(g, NULL))
        return 0;
    geom_moved(g);
    if (rd(g + 12, &fl, 4)) {
        fl |= GF_DIRTY | GF_AABB_BAD;
        wr(g + 12, &fl, 4);
    }
    return 1;
}

int objects_obstacle_ready(void)
{
    static DWORD last = 0;
    int ok = g_hf_space && g_box_vt && FIRST_OFF;
    if (!ok && g_ob_log && GetTickCount() - last > 15000) {
        last = GetTickCount();
        g_ob_log("objets : obstacles pas prets : espace racine %p, table de boite %p, premier +%d", (void *)g_hf_space,
                 (void *)g_box_vt, FIRST_OFF);
    }
    return ok;
}

int objects_ready(void)
{
    return g_db != NULL;
}

static int box_top(const Box *x, float px, float pz, float ymax, float *h)
{
    float lo = -1e30f, hi = 1e30f;
    int i;
    for (i = 0; i < 3; ++i) {
        /* coordonnée locale i le long de la verticale (x = px, z = pz) : a + b y */
        float a = x->R[0 * 3 + i] * (px - x->c[0]) - x->R[1 * 3 + i] * x->c[1] + x->R[2 * 3 + i] * (pz - x->c[2]);
        float b = x->R[1 * 3 + i];
        if (fabsf(b) < 1e-6f) {
            if (fabsf(a) > x->h[i])
                return 0;
        } else {
            float y1 = (-x->h[i] - a) / b, y2 = (x->h[i] - a) / b;
            if (y1 > y2) {
                float t = y1;
                y1 = y2;
                y2 = t;
            }
            lo = fmaxf(lo, y1);
            hi = fminf(hi, y2);
        }
    }
    if (lo > hi || hi > ymax)
        return 0;
    *h = hi;
    return 1;
}

int objects_height(float x, float z, float ymax, float *h)
{
    Db *d = g_db;
    int cx, cz, k, e, found = 0;
    float best = -1e30f;
    if (!d)
        return 0;
    cx = (int)floorf((x - d->x0) / d->cell);
    cz = (int)floorf((z - d->z0) / d->cell);
    if (cx < 0 || cz < 0 || cx >= d->nx || cz >= d->nz)
        return 0;
    k = cz * d->nx + cx;
    e = d->start[k + 1];
    for (int j = d->start[k]; j < e; ++j) {
        int it = d->items[j];
        float y;
        if (it >= 0) {
            const float *t = d->tri + (size_t)it * 9;
            float x1 = t[3] - t[0], z1 = t[5] - t[2], x2 = t[6] - t[0], z2 = t[8] - t[2];
            float px = x - t[0], pz = z - t[2], den = x1 * z2 - x2 * z1, u, v;
            if (d->wall[it] || fabsf(den) < 1e-9f)
                continue;
            u = (px * z2 - x2 * pz) / den;
            v = (x1 * pz - px * z1) / den;
            if (u < 0.0f || v < 0.0f || u + v > 1.0f)
                continue;
            y = t[1] + u * (t[4] - t[1]) + v * (t[7] - t[1]);
            if (y > ymax)
                continue;
        } else if (!box_top(&d->box[~it], x, z, ymax, &y)) {
            continue;
        }
        if (y > best) {
            best = y;
            found = 1;
        }
    }
    if (found)
        *h = best;
    return found;
}

/* point (px, pz) à moins de r du segment a-b (plan horizontal) */
static int near_seg(float px, float pz, float ax, float az, float bx, float bz, float r)
{
    float dx = bx - ax, dz = bz - az, l = dx * dx + dz * dz, t = 0.0f, ex, ez;
    if (l > 1e-12f) {
        t = ((px - ax) * dx + (pz - az) * dz) / l;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    }
    ex = ax + t * dx - px;
    ez = az + t * dz - pz;
    return ex * ex + ez * ez < r * r;
}

/* le triangle coupe-t-il le plan y = h près de (x, z) ? */
static int tri_cut(const float *t, float x, float z, float h, float r)
{
    float d[3], p[2][2];
    int i, n = 0;
    for (i = 0; i < 3; ++i)
        d[i] = t[i * 3 + 1] - h;
    if ((d[0] > 0 && d[1] > 0 && d[2] > 0) || (d[0] < 0 && d[1] < 0 && d[2] < 0))
        return 0;
    for (i = 0; i < 3 && n < 2; ++i) {
        int j = (i + 1) % 3;
        if ((d[i] <= 0) != (d[j] <= 0)) {
            float k = d[i] / (d[i] - d[j]);
            p[n][0] = t[i * 3] + k * (t[j * 3] - t[i * 3]);
            p[n][1] = t[i * 3 + 2] + k * (t[j * 3 + 2] - t[i * 3 + 2]);
            ++n;
        }
    }
    return n == 2 && near_seg(x, z, p[0][0], p[0][1], p[1][0], p[1][1], r);
}

static int box_hit(const Box *x, float px, float py, float pz, float r)
{
    int i;
    for (i = 0; i < 3; ++i) {
        float l = x->R[0 * 3 + i] * (px - x->c[0]) + x->R[1 * 3 + i] * (py - x->c[1]) + x->R[2 * 3 + i] * (pz - x->c[2]);
        if (fabsf(l) > x->h[i] + r)
            return 0;
    }
    return 1;
}

/* point le plus proche de p sur le triangle t (Ericson, Real-Time Collision Detection 5.1.5) */
static void closest_tri(const float *t, const float p[3], float out[3])
{
    const float *a = t, *b = t + 3, *c = t + 6;
    float ab[3], ac[3], ap[3], bp[3], cp[3], d1, d2, d3, d4, d5, d6, va, vb, vc, v, w, den;
    int i;
    for (i = 0; i < 3; ++i) {
        ab[i] = b[i] - a[i];
        ac[i] = c[i] - a[i];
        ap[i] = p[i] - a[i];
        bp[i] = p[i] - b[i];
        cp[i] = p[i] - c[i];
    }
#define D3(u, q) ((u)[0] * (q)[0] + (u)[1] * (q)[1] + (u)[2] * (q)[2])
    d1 = D3(ab, ap); d2 = D3(ac, ap);
    if (d1 <= 0 && d2 <= 0) { memcpy(out, a, 12); return; }
    d3 = D3(ab, bp); d4 = D3(ac, bp);
    if (d3 >= 0 && d4 <= d3) { memcpy(out, b, 12); return; }
    vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        v = d1 / (d1 - d3);
        for (i = 0; i < 3; ++i) out[i] = a[i] + v * ab[i];
        return;
    }
    d5 = D3(ab, cp); d6 = D3(ac, cp);
    if (d6 >= 0 && d5 <= d6) { memcpy(out, c, 12); return; }
    vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        w = d2 / (d2 - d6);
        for (i = 0; i < 3; ++i) out[i] = a[i] + w * ac[i];
        return;
    }
    va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (i = 0; i < 3; ++i) out[i] = b[i] + w * (c[i] - b[i]);
        return;
    }
#undef D3
    den = 1.0f / (va + vb + vc);
    v = vb * den;
    w = vc * den;
    for (i = 0; i < 3; ++i) out[i] = a[i] + ab[i] * v + ac[i] * w;
}

/* v0.41.27 : pilote au sol contre un objet (boîte, mur) : déplacement pour sortir la sphère
   (p, r) de l'objet le plus enfoncé. Avant, seul le dessus des objets comptait : couché, le
   pilote glissait à travers les murs et les boîtes. */
int objects_push(const float p[3], float r, float push[3])
{
    Db *d = g_db;
    int cx0, cx1, cz0, cz1, cx, cz, found = 0;
    float best = 0.0f;
    push[0] = push[1] = push[2] = 0.0f;
    if (!d)
        return 0;
    cx0 = (int)floorf((p[0] - r - d->x0) / d->cell);
    cx1 = (int)floorf((p[0] + r - d->x0) / d->cell);
    cz0 = (int)floorf((p[2] - r - d->z0) / d->cell);
    cz1 = (int)floorf((p[2] + r - d->z0) / d->cell);
    for (cz = cz0; cz <= cz1; ++cz)
        for (cx = cx0; cx <= cx1; ++cx) {
            int k, j;
            if (cx < 0 || cz < 0 || cx >= d->nx || cz >= d->nz)
                continue;
            k = cz * d->nx + cx;
            for (j = d->start[k]; j < d->start[k + 1]; ++j) {
                int it = d->items[j], i;
                if (it < 0) {
                    const Box *bx = &d->box[~it];
                    float l[3], pen = 1e9f, dir = 0.0f;
                    int ax = -1;
                    for (i = 0; i < 3; ++i) {
                        float q;
                        l[i] = bx->R[0 * 3 + i] * (p[0] - bx->c[0]) + bx->R[1 * 3 + i] * (p[1] - bx->c[1]) +
                               bx->R[2 * 3 + i] * (p[2] - bx->c[2]);
                        q = bx->h[i] + r - fabsf(l[i]);
                        if (q <= 0.0f) {
                            ax = -2;
                            break;
                        }
                        if (q < pen) {
                            pen = q;
                            ax = i;
                            dir = l[i] < 0.0f ? -1.0f : 1.0f;
                        }
                    }
                    if (ax < 0 || pen <= best)
                        continue;
                    best = pen;
                    found = 1;
                    for (i = 0; i < 3; ++i)
                        push[i] = bx->R[i * 3 + ax] * dir * pen; /* colonne ax de R = axe local en monde */
                } else if (d->wall[it]) {
                    const float *t = d->tri + (size_t)it * 9;
                    float c[3], v[3], dl, pen;
                    closest_tri(t, p, c);
                    for (i = 0; i < 3; ++i)
                        v[i] = p[i] - c[i];
                    dl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (dl >= r || dl < 1e-5f)
                        continue;
                    pen = r - dl;
                    if (pen <= best)
                        continue;
                    best = pen;
                    found = 1;
                    for (i = 0; i < 3; ++i)
                        push[i] = v[i] / dl * pen;
                }
            }
        }
    return found;
}

int objects_blocked(float x, float z, float feet, float r)
{
    Db *d = g_db;
    /* tranches au-dessus des pieds, tous les 10 cm (la marche de 45 cm passe dessous) */
    static const float k_h[] = { 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.35f, 1.5f, 1.65f };
    static const float k_o[5][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    int o;
    int cx0, cx1, cz0, cz1, cx, cz;
    float top;
    if (!d)
        return 0;
    /* une surface trop haute pour une marche mais sous la tête : on bute dedans (bord du ponton) */
    for (o = 0; o < 5; ++o)
        if (objects_height(x + k_o[o][0] * r, z + k_o[o][1] * r, feet + 1.8f, &top) && top > feet + 0.47f) {
            if (g_block_log && (g_block_n++ % 120) == 0)
                g_block_log("objets : bloque par un bord trop haut en (%.1f %.1f), pieds %.2f, dessus %.2f", x, z, feet, top);
            return 1;
        }
    cx0 = (int)floorf((x - r - d->x0) / d->cell);
    cx1 = (int)floorf((x + r - d->x0) / d->cell);
    cz0 = (int)floorf((z - r - d->z0) / d->cell);
    cz1 = (int)floorf((z + r - d->z0) / d->cell);
    for (cz = cz0; cz <= cz1; ++cz)
        for (cx = cx0; cx <= cx1; ++cx) {
            int k, j;
            if (cx < 0 || cz < 0 || cx >= d->nx || cz >= d->nz)
                continue;
            k = cz * d->nx + cx;
            for (j = d->start[k]; j < d->start[k + 1]; ++j) {
                int it = d->items[j], s;
                for (s = 0; s < (int)(sizeof(k_h) / sizeof(k_h[0])); ++s) {
                    if (it >= 0) {
                        if (d->wall[it] && tri_cut(d->tri + (size_t)it * 9, x, z, feet + k_h[s], r)) {
                            if (g_block_log && (g_block_n++ % 120) == 0)
                                g_block_log("objets : bloque par le maillage (triangle %d) en (%.1f %.1f), pieds %.2f", it, x, z, feet);
                            return 1;
                        }
                    } else if (box_hit(&d->box[~it], x, feet + k_h[s], z, r)) {
                        const Box *bx = &d->box[~it];
                        if (g_block_log && (g_block_n++ % 120) == 0)
                            g_block_log("objets : bloque par la boite %d (%.1f %.1f %.1f, %.2f x %.2f x %.2f m, espace %p, donnees %p) en (%.1f %.1f)",
                                        ~it, bx->c[0], bx->c[1], bx->c[2], 2 * bx->h[0], 2 * bx->h[1], 2 * bx->h[2], (void *)bx->space,
                                        (void *)bx->data, x, z);
                        return 1;
                    }
                }
            }
        }
    return 0;
}

/* v1.8.146 : rampes en maillage (voir objects.h) */
#include "gamever.h"
#define RMESH_MAX 512
typedef struct {
    uintptr_t g, d, sp;
    float *v;
    int *ix;
} RMesh;
static RMesh g_rm[RMESH_MAX];
static int g_nrm = 0, g_rm_fail = 0, g_rm_logged = 0;
static uintptr_t g_rm_hf = 0;
/* v1.8.146 (2e essai) : les rampes dans l'espace du relief (marque « sol » 0x20000) : le jeu traite leurs contacts
   comme la terre (matière lue dans la grille, roues sur le relief) : roues toujours sans contact en haut, bruit de
   sable. Maintenant : notre propre espace, marqué comme les maillages des pistes (passerelles : 0x20), avec une
   table de matières à nous (matière 0 de maillage : surface dure). */
static uintptr_t g_rm_space = 0, g_rm_space_hf = 0;
static struct {
    int32_t tag, pad;
    void *track;
} g_rm_sd;
static uint8_t g_rm_track[0x800];
static int32_t g_rm_mat[4096];
typedef void *(*FnTmDataCreate)(void);
typedef void (*FnTmBuild)(void *, const void *, int, int, const void *, int, int);
typedef void *(*FnTmCreate)(void *, void *, void *, void *, void *);
typedef void (*FnVoidP)(void *);

void objects_ramp_meshes_clear(void)
{
    const GameOde *o = gamever_ode(NULL);
    int i, alive = g_rm_hf && g_rm_hf == g_hf_geom;
    for (i = 0; i < g_nrm; ++i) {
        RMesh *m = &g_rm[i];
        /* même piste et toujours dans son espace : détruit ; sinon le jeu l'a déjà libéré avec sa physique */
        if (alive && m->g && o->destroy && rp(m->g + PS_OFF) == m->sp) {
            ((FnVoidP)o->destroy)((void *)m->g);
            if (m->d && o->data_destroy)
                ((FnVoidP)o->data_destroy)((void *)m->d);
            free(m->v);
            free(m->ix);
        } else if (alive) {
            free(m->v);
            free(m->ix);
        }
        /* (autre piste : données du maillage et sommets gardés, jamais relus : quelques ko) */
        memset(m, 0, sizeof(*m));
    }
    g_nrm = 0;
    g_rm_fail = 0;
    g_rm_hf = 0;
}

/* v1.8.152 : murs des rampes (côtés, dos) : comme les murs des pistes (WLL : marque 0x40, catégorie 2) : les roues
   (cylindres de la moto) et le pilote les touchent */
static uintptr_t g_rw_space = 0, g_rw_space_hf = 0;
static struct {
    int32_t tag, pad;
    void *track;
} g_rw_sd;

static int mesh_add(const float *v, int nv, const int *ix, int nt, int wall)
{
    const GameOde *o = gamever_ode(L);
    uintptr_t hf = g_hf_geom, sp, g, d;
    RMesh *m;
    unsigned one = 1, zero = 0;
    if (nv <= 0 || nt <= 0 || nt > 4096)
        return 0;
    if (g_nrm >= RMESH_MAX || !o->create || !hf || !PS_OFF) {
        g_rm_fail = 1;
        return 0;
    }
    if (!wall && g_rm_space && g_rm_space_hf == hf)
        sp = g_rm_space;
    else if (wall && g_rw_space && g_rw_space_hf == hf)
        sp = g_rw_space;
    else {
        uintptr_t rs = rp(hf + PS_OFF), ts = rs ? rp(rs + PS_OFF) : 0; /* espace du relief, puis celui de la piste */
        unsigned one = 1, zero = 0;
        sp = 0;
        if (o->space_create && ts && is_space(gtype(ts)))
            sp = (uintptr_t)((void *(*)(void *))o->space_create)((void *)ts);
        if (!sp || !is_space(gtype(sp))) {
            if (L)
                L("rampes : espace des maillages pas cree (espace de la piste %p)", (void *)ts);
            g_rm_fail = 1;
            return 0;
        }
        *(int32_t **)(g_rm_track + 0x600) = g_rm_mat; /* matière de chaque triangle : 0 */
        *(int32_t **)(g_rm_track + 0x630) = g_rm_mat; /* (murs) */
        if (wall) {
            unsigned two = 2;
            g_rw_sd.tag = 0x40;
            g_rw_sd.track = g_rm_track;
            wr(sp + 120, &two, 4);
            wr(sp + 124, &zero, 4);
            wp(sp + 16, (uintptr_t)&g_rw_sd);
            g_rw_space = sp;
            g_rw_space_hf = hf;
        } else {
            g_rm_sd.tag = 0x20;
            g_rm_sd.track = g_rm_track;
            wr(sp + 120, &one, 4);
            wr(sp + 124, &zero, 4);
            wp(sp + 16, (uintptr_t)&g_rm_sd);
            g_rm_space = sp;
            g_rm_space_hf = hf;
        }
        if (L)
            L("rampes : espace des %s cree %p dans l'espace de la piste %p (marque 0x%x, cat %d col 0)", wall ? "murs" : "maillages",
              (void *)sp, (void *)ts, wall ? 0x40 : 0x20, wall ? 2 : 1);
    }
    if (g_rm_hf && g_rm_hf != hf)
        objects_ramp_meshes_clear();
    m = &g_rm[g_nrm];
    memset(m, 0, sizeof(*m));
    m->v = (float *)malloc((size_t)nv * 12);
    m->ix = (int *)malloc((size_t)nt * 12);
    if (!m->v || !m->ix) {
        free(m->v);
        free(m->ix);
        g_rm_fail = 1;
        return 0;
    }
    memcpy(m->v, v, (size_t)nv * 12);
    memcpy(m->ix, ix, (size_t)nt * 12);
    d = (uintptr_t)((FnTmDataCreate)o->data_create)();
    if (!d) {
        free(m->v);
        free(m->ix);
        g_rm_fail = 1;
        return 0;
    }
    ((FnTmBuild)o->build_single)((void *)d, m->v, 12, nv, m->ix, nt * 3, 12);
    g = (uintptr_t)((FnTmCreate)o->create)((void *)sp, (void *)d, NULL, NULL, NULL);
    if (!g) {
        ((FnVoidP)o->data_destroy)((void *)d);
        free(m->v);
        free(m->ix);
        g_rm_fail = 1;
        return 0;
    }
    /* filtres : sol (catégorie 1) ou mur (catégorie 2), collisions 0 */
    {
        unsigned ca = wall ? 2u : one, co = zero;
        wr(g + 120, &ca, 4);
        wr(g + 124, &co, 4);
    }
    m->g = g;
    m->d = d;
    m->sp = sp;
    g_rm_hf = hf;
    ++g_nrm;
    if (L && g_rm_logged < 4) {
        unsigned tag = 0, ca = 0, co = 0;
        uintptr_t sd = rp(sp + 16);
        ++g_rm_logged;
        if (sd)
            rd(sd, &tag, 4);
        rd(g + 120, &ca, 4);
        rd(g + 124, &co, 4);
        L("rampes : maillage %d dans la physique (%d sommets, %d triangles), espace %p (type %d, marque %x), cat %x col %x, objet ODE type %d",
          g_nrm - 1, nv, nt, (void *)sp, gtype(sp), tag, ca, co, gtype(g));
    }
    return 1;
}

int objects_ramp_mesh_add(const float *v, int nv, const int *ix, int nt)
{
    return mesh_add(v, nv, ix, nt, 0);
}

int objects_ramp_wall_add(const float *v, int nv, const int *ix, int nt)
{
    return mesh_add(v, nv, ix, nt, 1);
}

void objects_ramp_spaces(uintptr_t *top, uintptr_t *wall)
{
    *top = g_rm_space;
    *wall = g_rw_space;
}

int objects_ramp_meshes_stale(void)
{
    return g_rm_fail || (g_nrm > 0 && g_rm_hf != g_hf_geom);
}

int objects_ramp_meshes_count(void)
{
    return g_nrm;
}

void objects_ramp_meshes_log(ObjLogFn log)
{
    if (log && !L)
        L = log;
}
#else
int objects_blocked(float x, float z, float feet, float r) { (void)x; (void)z; (void)feet; (void)r; return 0; }
void objects_poll(ObjLogFn log) { (void)log; }
void objects_forget(void) {}
int objects_height(float x, float z, float ymax, float *h) { (void)x; (void)z; (void)ymax; (void)h; return 0; }
int objects_ready(void) { return 0; }
int objects_push(const float p[3], float r, float push[3]) { (void)p; (void)r; push[0] = push[1] = push[2] = 0.0f; return 0; }
int objects_body_collide(const uintptr_t *bodies, int n, int on) { (void)bodies; (void)n; (void)on; return -1; }
int objects_obstacle(int i, int on, const float c[3], const float R[9], const float h[3]) { (void)i; (void)on; (void)c; (void)R; (void)h; return 0; }
int objects_obstacle_ready(void) { return 0; }
void objects_relief_moved(void) {}
int objects_hf_data_off(uintptr_t hdr) { (void)hdr; return -1; }
int objects_geom_dirty(uintptr_t g) { (void)g; return 0; }
void objects_obstacles_reset(void) {}
void objects_obstacle_log(ObjLogFn log) { (void)log; }
void objects_ramp_meshes_clear(void) {}
int objects_ramp_mesh_add(const float *v, int nv, const int *ix, int nt) { (void)v; (void)nv; (void)ix; (void)nt; return 0; }
int objects_ramp_wall_add(const float *v, int nv, const int *ix, int nt) { (void)v; (void)nv; (void)ix; (void)nt; return 0; }
void objects_ramp_spaces(uintptr_t *top, uintptr_t *wall) { *top = *wall = 0; }
int objects_ramp_meshes_stale(void) { return 0; }
int objects_ramp_meshes_count(void) { return 0; }
void objects_ramp_meshes_log(ObjLogFn log) { (void)log; }
#endif

/*
 * groundhook.c - v1.8.148 : voir groundhook.h
 */
#include "groundhook.h"
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>

/* surfaces sous un point : appel du relief dans la fonction de la liste (le @), puis matière du relief (+0x26) */
static const char k_gh_sig[] =
    "4c 8d 44 24 48 48 8d 94 24 ?? ?? 00 00 41 0f 28 dc 48 8b cb f3 44 0f 11 5c 24 20 @e8 ?? ?? ?? ?? 83 f8 01 0f 85 ?? ?? 00 00 "
    "4c 8d 44 24 44 48 8d 54 24 40 41 0f 28 dc 48 8b cb f3 44 0f 11 5c 24 20 e8";

/* liste des surfaces dans les données de la piste : nombre (+0x648, 10 au plus), puis 24 octets par surface :
   hauteur, normale, matière, sous-matière */
#define GH_COUNT 0x648
#define GH_LIST 0x64c
#define GH_MAX 10

typedef int (*HfHitFn)(void *td, float *h, float *n, float x, float z);
typedef int (*HfMatFn)(void *td, int *m, int *m2, float x, float z);

static HfHitFn g_hf = NULL;
static uint8_t *volatile g_td = NULL; /* données de la piste (vues au premier appel) */
typedef void *(*GameAllocFn)(unsigned n);
typedef void (*GameFreeFn)(void *p);
static GameAllocFn g_alloc = NULL;
static GameFreeFn g_free = NULL;
static const char k_alloc_sig[] = "8b 93 e0 05 00 00 8d 0c 52 c1 e1 02 @e8 ?? ?? ?? ?? 8b 8b e0 05 00 00";
static const char k_free_sig[] =
    "48 89 5c 24 08 57 48 83 ec 20 48 8b d9 48 8b 89 e8 05 00 00 @e8 ?? ?? ?? ?? 48 8b 8b f8 05 00 00 33 ff 48 89 bb e8 05 00 00";
static HfMatFn g_mat = NULL;
static GroundRampFn g_fn = NULL;
/* v1.8.153 : contacts */
typedef int (*NearFn)(void *self, void *o1, void *o2, void *c, void *out);
static NearFn g_near = NULL;
static volatile uintptr_t g_sp_top = 0, g_sp_wall = 0;
static volatile unsigned long g_cs[6]; /* dessus : vus, refusés, gardés ; murs : vus, refusés, gardés */
static const char k_near_sig[] =
    "48 63 c5 4d 8b c5 49 8b d6 48 8d 0c c0 48 8d 44 24 30 48 03 c9 48 89 44 24 20 48 8d 5c cc 40 49 8b cf 4c 8b cb @e8 ?? ?? ?? ?? 83 f8 ff 0f 84";

static uintptr_t gspace(void *g)
{
    return g ? *(uintptr_t *)((uint8_t *)g + 88) : 0;
}

static volatile int g_cd_n = 0;
static struct {
    int k, r, c1, c2;
    unsigned cat1, cat2, t1, t2;
    float pos[3], nrm[3], depth;
} g_cd[8];

static unsigned gtag(void *g)
{
    uintptr_t sp = gspace(g), d;
    if (!sp)
        return 0;
    d = *(uintptr_t *)(sp + 16);
    return d ? *(unsigned *)d : 0;
}

static int near_hook(void *self, void *o1, void *o2, void *c, void *out)
{
    int r = g_near(self, o1, o2, c, out), k = -1;
    uintptr_t a = gspace(o1), b = gspace(o2);
    if (g_sp_top && (a == g_sp_top || b == g_sp_top))
        k = 0;
    else if (g_sp_wall && (a == g_sp_wall || b == g_sp_wall))
        k = 3;
    if (k >= 0) {
        ++g_cs[k];
        ++g_cs[k + (r == -1 ? 1 : 2)];
        if (g_cd_n < 8 && (k == 3 || g_cd_n < 2)) {
            int i = g_cd_n;
            const float *cg = (const float *)((uint8_t *)c + 0x40);
            g_cd[i].k = k;
            g_cd[i].r = r;
            g_cd[i].c1 = *(int *)((uint8_t *)o1 + 8);
            g_cd[i].c2 = *(int *)((uint8_t *)o2 + 8);
            g_cd[i].cat1 = *(unsigned *)((uint8_t *)o1 + 0x78);
            g_cd[i].cat2 = *(unsigned *)((uint8_t *)o2 + 0x78);
            g_cd[i].t1 = gtag(o1);
            g_cd[i].t2 = gtag(o2);
            memcpy(g_cd[i].pos, cg, 12);
            memcpy(g_cd[i].nrm, cg + 4, 12);
            g_cd[i].depth = cg[8];
            g_cd_n = i + 1;
        }
    }
    return r;
}

void groundhook_contact_watch(uintptr_t sp_top, uintptr_t sp_wall)
{
    g_sp_top = sp_top;
    g_sp_wall = sp_wall;
}

void groundhook_contact_stats(unsigned long out[6])
{
    int i;
    for (i = 0; i < 6; ++i)
        out[i] = g_cs[i];
}

void groundhook_contact_log(void (*log)(const char *fmt, ...))
{
    int i, n = g_cd_n;
    for (i = 0; i < n && log; ++i)
        log("rampes : contact %s : objets classe %d/%d cat %x/%x marque %x/%x, rendu %d, en (%.2f %.2f %.2f) normale (%.2f %.2f %.2f) "
            "profondeur %.3f",
            g_cd[i].k ? "mur" : "dessus", g_cd[i].c1, g_cd[i].c2, g_cd[i].cat1, g_cd[i].cat2, g_cd[i].t1, g_cd[i].t2, g_cd[i].r,
            g_cd[i].pos[0], g_cd[i].pos[1], g_cd[i].pos[2], g_cd[i].nrm[0], g_cd[i].nrm[1], g_cd[i].nrm[2], g_cd[i].depth);
    g_cd_n = 0;
}
static volatile unsigned long g_hits = 0, g_calls = 0;

static int parse(const char *s, uint8_t *b, uint8_t *m, int *at)
{
    int n = 0;
    *at = -1;
    while (*s && n < 128) {
        unsigned v = 0;
        while (*s == ' ')
            ++s;
        if (!*s)
            break;
        if (*s == '@') {
            *at = n;
            ++s;
            continue;
        }
        if (s[0] == '?') {
            b[n] = 0;
            m[n++] = 0;
        } else {
            int k;
            for (k = 0; k < 2; ++k) {
                char c = s[k];
                v = v * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
            }
            b[n] = (uint8_t)v;
            m[n++] = 1;
        }
        s += 2;
    }
    return n;
}

static uint8_t *find_once(uint8_t *lo, uint8_t *hi, const char *sig)
{
    uint8_t b[128], m[128];
    int at, n = parse(sig, b, m, &at), k, hits = 0;
    uint8_t *p, *hit = NULL;
    if (n < 16 || at < 0)
        return NULL;
    for (p = lo; p + n <= hi; ++p) {
        if (p[0] != b[0])
            continue;
        for (k = 1; k < n; ++k)
            if (m[k] && p[k] != b[k])
                break;
        if (k == n) {
            hit = p + at;
            if (++hits > 1)
                return NULL;
        }
    }
    return hits == 1 ? hit : NULL;
}

static uint8_t *alloc_near(uint8_t *near_addr)
{
    SYSTEM_INFO si;
    uintptr_t gran, start, addr;
    const uintptr_t range = 0x70000000;
    GetSystemInfo(&si);
    gran = si.dwAllocationGranularity;
    start = ((uintptr_t)near_addr) & ~(gran - 1);
    for (addr = start; addr > gran && start - addr < range; addr -= gran) {
        void *p = VirtualAlloc((void *)addr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p)
            return (uint8_t *)p;
    }
    for (addr = start + gran; addr - start < range; addr += gran) {
        void *p = VirtualAlloc((void *)addr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p)
            return (uint8_t *)p;
    }
    return NULL;
}

static int hf_hit_hook(void *td, float *h, float *n, float x, float z)
{
    int r;
    int *cnt;
    g_td = (uint8_t *)td;
    r = g_hf(td, h, n, x, z);
    cnt = (int *)((uint8_t *)td + GH_COUNT);
    float rh, rn[3];
    ++g_calls;
    /* (une place gardée pour le relief, ajouté juste après par le jeu) */
    if (g_fn && *cnt >= 0 && *cnt < GH_MAX - 1 && g_fn(x, z, &rh, rn)) {
        uint8_t *e = (uint8_t *)td + GH_LIST + *cnt * 24;
        int m = 0, m2 = 0;
        if (g_mat)
            g_mat(td, &m, &m2, x, z); /* matière du relief à cet endroit (la rampe roule comme la piste) */
        memcpy(e, &rh, 4);
        memcpy(e + 4, rn, 12);
        memcpy(e + 16, &m, 4);
        memcpy(e + 20, &m2, 4);
        ++*cnt;
        ++g_hits;
    }
    return r;
}

int groundhook_install(GroundRampFn fn, void (*log)(const char *fmt, ...))
{
    static int done = 0, ok = 0;
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL), *site = NULL, *tr;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    int i;
    int32_t rel;
    uint64_t dest = (uint64_t)(uintptr_t)&hf_hit_hook;
    DWORD old, tmp;
    g_fn = fn;
    if (done)
        return ok;
    done = 1;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (!memcmp(sec->Name, ".text", 5)) {
            site = find_once(base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize, k_gh_sig);
            break;
        }
    if (!site || site[0] != 0xE8 || site[0x26] != 0xE8) {
        if (log)
            log("rampes : appel des surfaces sous les roues PAS trouve (roues sans rampe au-dessus du relief)");
        return 0;
    }
    memcpy(&rel, site + 1, 4);
    g_hf = (HfHitFn)(void *)(site + 5 + rel);
    memcpy(&rel, site + 0x27, 4);
    g_mat = (HfMatFn)(void *)(site + 0x26 + 5 + rel);
    tr = alloc_near(site);
    if (!tr) {
        if (log)
            log("rampes : surfaces sous les roues : pas de memoire pres du jeu");
        return 0;
    }
    /* mov rax, hf_hit_hook ; jmp rax */
    tr[0] = 0x48;
    tr[1] = 0xB8;
    memcpy(tr + 2, &dest, 8);
    tr[10] = 0xFF;
    tr[11] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), tr, 12);
    rel = (int32_t)((intptr_t)tr - (intptr_t)(site + 5));
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    ok = 1;
    for (i = 0, sec = IMAGE_FIRST_SECTION(nt); i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (!memcmp(sec->Name, ".text", 5)) {
            uint8_t *lo = base + sec->VirtualAddress, *hi = lo + sec->Misc.VirtualSize, *a, *f;
            a = find_once(lo, hi, k_alloc_sig);
            if (!a) {
                /* (2 fois dans le jeu : la 1re) */
                uint8_t b[128], m[128];
                int at, n = parse(k_alloc_sig, b, m, &at), k;
                uint8_t *p;
                for (p = lo; p + n <= hi && !a; ++p) {
                    for (k = 0; k < n; ++k)
                        if (m[k] && p[k] != b[k])
                            break;
                    if (k == n)
                        a = p + at;
                }
            }
            f = find_once(lo, hi, k_free_sig);
            if (a && f && a[0] == 0xE8 && f[0] == 0xE8) {
                memcpy(&rel, a + 1, 4);
                g_alloc = (GameAllocFn)(void *)(a + 5 + rel);
                memcpy(&rel, f + 1, 4);
                g_free = (GameFreeFn)(void *)(f + 5 + rel);
            }
            break;
        }
    {
        /* v1.8.153 : appel du tri des contacts ODE (diagnostic des rampes) */
        uint8_t *ns = NULL, *t2;
        for (i = 0, sec = IMAGE_FIRST_SECTION(nt); i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            if (!memcmp(sec->Name, ".text", 5)) {
                ns = find_once(base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize, k_near_sig);
                break;
            }
        if (ns && ns[0] == 0xE8 && (t2 = alloc_near(ns)) != NULL) {
            uint64_t d2 = (uint64_t)(uintptr_t)&near_hook;
            memcpy(&rel, ns + 1, 4);
            g_near = (NearFn)(void *)(ns + 5 + rel);
            t2[0] = 0x48;
            t2[1] = 0xB8;
            memcpy(t2 + 2, &d2, 8);
            t2[10] = 0xFF;
            t2[11] = 0xE0;
            FlushInstructionCache(GetCurrentProcess(), t2, 12);
            rel = (int32_t)((intptr_t)t2 - (intptr_t)(ns + 5));
            if (VirtualProtect(ns, 5, PAGE_EXECUTE_READWRITE, &old)) {
                memcpy(ns + 1, &rel, 4);
                VirtualProtect(ns, 5, old, &tmp);
                FlushInstructionCache(GetCurrentProcess(), ns, 5);
            }
        }
        if (log)
            log("rampes : contacts ODE %s", g_near ? "surveilles" : "PAS trouves");
    }
    if (log)
        log("rampes : memoire du jeu %s (+0x%llX +0x%llX)", g_alloc && g_free ? "trouvee" : "PAS trouvee",
            (unsigned long long)(g_alloc ? (uint8_t *)(void *)g_alloc - base : 0),
            (unsigned long long)(g_free ? (uint8_t *)(void *)g_free - base : 0));
    if (log)
        log("rampes : surfaces sous les roues interceptees (jeu+0x%llX, relief +0x%llX, matiere +0x%llX)",
            (unsigned long long)(site - base), (unsigned long long)((uint8_t *)(void *)g_hf - base),
            (unsigned long long)((uint8_t *)(void *)g_mat - base));
    return 1;
}
/* ---- v1.8.149 : rampes dans les maillages de la piste ---- */
#define TD_NV 0x5e0
#define TD_V 0x5e8
#define TD_NT 0x5f0
#define TD_I 0x5f8
#define TD_M 0x600
#define TD_M2 0x608
#define TD_NN 0x638
#define TD_N 0x640
typedef struct {
    float mn[3], mx[3];
    int start, count, left, right;
} Node; /* 40 octets, numéros des enfants à partir de 1 (0 : aucun) */

#define GR_MAXR 216
#define GR_MAXT 80
#define GR_MAXV 160
static struct {
    float v[GR_MAXV * 3];
    int ix[GR_MAXT * 3];
    int nv, nt;
} g_gr[GR_MAXR];
static int g_ngr = 0;

/* piste d'origine (pointeurs du jeu), et les nôtres en place */
static uint8_t *g_otd = NULL;
static void *g_ov, *g_oi, *g_om, *g_om2, *g_on;
static void *g_cv, *g_ci, *g_cm, *g_cm2, *g_cn;
static struct {
    void *p;
    DWORD t;
} g_trash[64];

static void trash(void *p)
{
    int i;
    if (!p)
        return;
    for (i = 0; i < 64; ++i)
        if (!g_trash[i].p) {
            g_trash[i].p = p;
            g_trash[i].t = GetTickCount();
            return;
        }
    /* (plein : gardé, quelques ko perdus) */
}

void groundhook_poll(void)
{
    int i;
    for (i = 0; i < 64; ++i)
        if (g_trash[i].p && GetTickCount() - g_trash[i].t > 5000) {
            if (g_free)
                g_free(g_trash[i].p);
            g_trash[i].p = NULL;
        }
}

int groundhook_ready(void)
{
    return g_td && g_alloc && g_free;
}

int groundhook_stale(void)
{
    uint8_t *td = g_td;
    return td && g_cn && (td != g_otd || *(void **)(td + TD_N) != g_cn);
}

void groundhook_ramps_begin(void)
{
    g_ngr = 0;
}

int groundhook_ramps_add(const float *v, int nv, const int *ix, int nt)
{
    if (g_ngr >= GR_MAXR || nv <= 0 || nt <= 0 || nv > GR_MAXV || nt > GR_MAXT)
        return 0;
    memcpy(g_gr[g_ngr].v, v, (size_t)nv * 12);
    memcpy(g_gr[g_ngr].ix, ix, (size_t)nt * 12);
    g_gr[g_ngr].nv = nv;
    g_gr[g_ngr].nt = nt;
    ++g_ngr;
    return 1;
}

static void bound(Node *n, const float *v, int nv)
{
    int i, k;
    for (i = 0; i < nv; ++i)
        for (k = 0; k < 3; ++k) {
            if (v[i * 3 + k] < n->mn[k])
                n->mn[k] = v[i * 3 + k];
            if (v[i * 3 + k] > n->mx[k])
                n->mx[k] = v[i * 3 + k];
        }
}

/* arbre des rampes [a, b) : rend le numéro (à partir de 1) de son nœud */
static int build(Node *nodes, int *nn, int a, int b, const int *tstart)
{
    int me = (*nn)++, k;
    Node *n = &nodes[me];
    for (k = 0; k < 3; ++k) {
        n->mn[k] = 1e30f;
        n->mx[k] = -1e30f;
    }
    n->start = n->count = n->left = n->right = 0;
    if (b - a == 1) {
        bound(n, g_gr[a].v, g_gr[a].nv);
        n->start = tstart[a];
        n->count = g_gr[a].nt;
    } else {
        int m = (a + b) / 2, l = build(nodes, nn, a, m, tstart), r = build(nodes, nn, m, b, tstart);
        n = &nodes[me];
        for (k = 0; k < 3; ++k) {
            n->mn[k] = nodes[l - 1].mn[k] < nodes[r - 1].mn[k] ? nodes[l - 1].mn[k] : nodes[r - 1].mn[k];
            n->mx[k] = nodes[l - 1].mx[k] > nodes[r - 1].mx[k] ? nodes[l - 1].mx[k] : nodes[r - 1].mx[k];
        }
        n->left = l;
        n->right = r;
    }
    return me + 1;
}

int groundhook_ramps_commit(void (*log)(const char *fmt, ...))
{
    uint8_t *td = g_td;
    int onv, ont, onn, nv, nt, nn, i, k, tstart[GR_MAXR], vstart[GR_MAXR], mat = 0, mat2 = 0;
    float *V;
    int *I, *M, *M2;
    Node *N;
    if (!td || !g_alloc || !g_free)
        return 0;
    /* autre piste chargée (le jeu a remplacé les tableaux) : nouvelle origine */
    if (td != g_otd || (*(void **)(td + TD_N) != g_cn && *(void **)(td + TD_N) != g_on) ||
        (*(void **)(td + TD_V) != g_cv && *(void **)(td + TD_V) != g_ov)) {
        g_otd = td;
        g_ov = *(void **)(td + TD_V);
        g_oi = *(void **)(td + TD_I);
        g_om = *(void **)(td + TD_M);
        g_om2 = *(void **)(td + TD_M2);
        g_on = *(void **)(td + TD_N);
        g_cv = g_ci = g_cm = g_cm2 = g_cn = NULL;
    }
    onv = *(int *)(td + TD_NV);
    ont = *(int *)(td + TD_NT);
    onn = *(int *)(td + TD_NN);
    if (onv < 0 || ont < 0 || onn < 0 || onv > 10000000 || ont > 10000000 || onn > 10000000)
        return 0;
    if (!g_on)
        onn = 0;
    if (g_ngr == 0) {
        /* piste d'origine remise (arbre d'abord, puis les tableaux) */
        if (g_cn) {
            *(void **)(td + TD_N) = g_on;
            MemoryBarrier();
            *(void **)(td + TD_V) = g_ov;
            *(void **)(td + TD_I) = g_oi;
            *(void **)(td + TD_M) = g_om;
            *(void **)(td + TD_M2) = g_om2;
            trash(g_cv);
            trash(g_ci);
            trash(g_cm);
            trash(g_cm2);
            trash(g_cn);
            g_cv = g_ci = g_cm = g_cm2 = g_cn = NULL;
            if (log)
                log("rampes : maillages de la piste remis d'origine");
        }
        return 1;
    }
    nv = onv;
    nt = ont;
    for (i = 0; i < g_ngr; ++i) {
        vstart[i] = nv;
        tstart[i] = nt;
        nv += g_gr[i].nv;
        nt += g_gr[i].nt;
    }
    nn = (onn ? onn + 1 : 0) + 2 * g_ngr;
    V = (float *)g_alloc((unsigned)nv * 12);
    I = (int *)g_alloc((unsigned)nt * 12);
    M = (int *)g_alloc((unsigned)nt * 4);
    M2 = (int *)g_alloc((unsigned)nt * 4);
    N = (Node *)g_alloc((unsigned)nn * (unsigned)sizeof(Node));
    if (!V || !I || !M || !M2 || !N) {
        if (V) g_free(V);
        if (I) g_free(I);
        if (M) g_free(M);
        if (M2) g_free(M2);
        if (N) g_free(N);
        return 0;
    }
    if (onv && g_ov)
        memcpy(V, g_ov, (size_t)onv * 12);
    if (ont && g_oi)
        memcpy(I, g_oi, (size_t)ont * 12);
    if (ont && g_om)
        memcpy(M, g_om, (size_t)ont * 4);
    if (ont && g_om2)
        memcpy(M2, g_om2, (size_t)ont * 4);
    /* matière des rampes : celle du premier triangle de la piste (passerelle), sinon celle du relief */
    if (ont && g_om && g_om2) {
        mat = ((int *)g_om)[0];
        mat2 = ((int *)g_om2)[0];
    } else if (g_mat && g_ngr) {
        g_mat(td, &mat, &mat2, g_gr[0].v[0], g_gr[0].v[2]);
    }
    for (i = 0; i < g_ngr; ++i) {
        memcpy(V + (size_t)vstart[i] * 3, g_gr[i].v, (size_t)g_gr[i].nv * 12);
        for (k = 0; k < g_gr[i].nt * 3; ++k)
            I[(size_t)tstart[i] * 3 + k] = g_gr[i].ix[k] + vstart[i];
        for (k = 0; k < g_gr[i].nt; ++k) {
            M[tstart[i] + k] = mat;
            M2[tstart[i] + k] = mat2;
        }
    }
    {
        int cnt;
        if (onn) {
            /* nœud 1 : racine nouvelle (ancienne racine et nos rampes) ; ancienne racine recopiée à la fin */
            int ours;
            memcpy(N, g_on, (size_t)onn * sizeof(Node));
            N[onn] = N[0];
            cnt = onn + 1;
            ours = build(N, &cnt, 0, g_ngr, tstart);
            for (k = 0; k < 3; ++k) {
                N[0].mn[k] = N[onn].mn[k] < N[ours - 1].mn[k] ? N[onn].mn[k] : N[ours - 1].mn[k];
                N[0].mx[k] = N[onn].mx[k] > N[ours - 1].mx[k] ? N[onn].mx[k] : N[ours - 1].mx[k];
            }
            N[0].start = N[0].count = 0;
            N[0].left = ours; /* (les rampes d'abord : le jeu ne regarde pas l'autre côté s'il a trouvé) */
            N[0].right = onn + 1;
        } else {
            cnt = 0;
            build(N, &cnt, 0, g_ngr, tstart);
        }
        /* tableaux d'abord (les anciens sont au début), puis l'arbre */
        trash(g_cv);
        trash(g_ci);
        trash(g_cm);
        trash(g_cm2);
        *(void **)(td + TD_V) = V;
        *(void **)(td + TD_I) = I;
        *(void **)(td + TD_M) = M;
        *(void **)(td + TD_M2) = M2;
        MemoryBarrier();
        *(void **)(td + TD_N) = N;
        trash(g_cn);
        g_cv = V;
        g_ci = I;
        g_cm = M;
        g_cm2 = M2;
        g_cn = N;
        if (log)
            log("rampes : %d rampe(s) dans les maillages de la piste (%d triangles d'origine, %d noeuds -> %d, matiere %d/%d)",
                g_ngr, ont, onn, cnt, mat, mat2);
    }
    return 1;
}
#else
int groundhook_install(GroundRampFn fn, void (*log)(const char *fmt, ...)) { (void)fn; (void)log; return 0; }
void groundhook_ramps_begin(void) {}
int groundhook_ramps_add(const float *v, int nv, const int *ix, int nt) { (void)v; (void)nv; (void)ix; (void)nt; return 0; }
int groundhook_ramps_commit(void (*log)(const char *fmt, ...)) { (void)log; return 0; }
void groundhook_poll(void) {}
int groundhook_stale(void) { return 0; }
void groundhook_contact_watch(uintptr_t sp_top, uintptr_t sp_wall) { (void)sp_top; (void)sp_wall; }
void groundhook_contact_stats(unsigned long out[6]) { int i; for (i = 0; i < 6; ++i) out[i] = 0; }
void groundhook_contact_log(void (*log)(const char *fmt, ...)) { (void)log; }
int groundhook_ready(void) { return 0; }
#endif

unsigned long groundhook_hits(void) { return g_hits; }
unsigned long groundhook_calls(void) { return g_calls; }

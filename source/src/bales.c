/*
 * bales.c - v1.8.82 : bottes de foin (voir bales.h)
 *
 * Une botte est un bloc lourd posé au sol : elle ne bascule pas ; elle glisse un peu quand une moto
 * la percute, frotte au sol et s'arrête vite ; elle suit la pente sous elle, ou se pose sur une
 * autre botte. Les bottes se repoussent entre elles (rectangles vus de dessus).
 */
#include "bales.h"
#include "objinst.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bale_model.inc"
#include "furn_model.inc" /* v1.8.178 : mobilier (table, chaises de camping) : variantes après les bottes */
#define NVAR (BALE_VARIANTS + FURN_VARIANTS)
#define NTEX (BALE_TEXS + FURN_TEXS)
#define IS_FURN(v) ((v) >= BALE_VARIANTS)
static const float *vdim(int v) { return IS_FURN(v) ? k_furn_dim[v - BALE_VARIANTS] : k_bale_dim[v]; }
static int vtris(int v) { return IS_FURN(v) ? k_furn_tris[v - BALE_VARIANTS] : k_bale_tris[v]; }
static int vtris_low(int v) { return IS_FURN(v) ? k_furn_tris[v - BALE_VARIANTS] : k_bale_tris_low[v]; }
static const float (*vmesh(int v))[9] { return IS_FURN(v) ? k_furn_m[v - BALE_VARIANTS] : k_bale_m[v]; }
static const float (*vmesh_low(int v))[9] { return IS_FURN(v) ? k_furn_m[v - BALE_VARIANTS] : k_bale_low[v]; }
/* texture de la variante (rang parmi celles des bottes puis du mobilier) */
static int vtex(int v) { return IS_FURN(v) ? BALE_TEXS + (int)k_furn_m[v - BALE_VARIANTS][0][8] : (int)k_bale_m[v][0][8]; }

typedef struct {
    int used;
    int var;
    float pos[3];   /* centre du dessous */
    float yaw;      /* e1 = (sin yaw, 0, cos yaw) : largeur ; e2 = (cos yaw, 0, -sin yaw) : longueur */
    float n[3];     /* haut de la botte (pente) */
    float v[2];     /* vitesse (x, z) */
    float vy;       /* chute (posée en l'air, ou botte du dessous partie) */
    float w;        /* rotation (rad/s) */
    float spawn[4]; /* x, y, z, cap au moment où elle a été posée */
    float still;
    float idle, ref[3]; /* v1.8.102 : temps sans vrai déplacement, et position de référence */
    int held;       /* v1.8.84 : portée par le pilote (pas de physique, pas d'obstacle) */
} Bale;

static Bale g_b[BALES_MAX];
static Bale g_disp[BALES_MAX];
static const Bale *g_src = g_b;
static int g_vsel[2] = { 0, BALE_VARIANTS }; /* v1.8.179 : variante choisie : bottes, mobilier */
static int g_cat = 0;                        /* v1.8.179 : 0 bottes, 1 mobilier (page de la roue) */
#define CAT_OF(v) (IS_FURN(v) ? 1 : 0)
#define IN_CAT(b) (CAT_OF((b)->var) == g_cat)
static BalesLogFn g_log = 0;

/* lourdes : part de la vitesse de la moto qu'elles prennent au choc */
/* v1.8.84 : la ronde est impossible à bouger ; la petite (lourde) se pousse un peu et se porte */
static const float k_take[NVAR] = { 0.0f, 0.30f, 0.35f, 0.55f, 0.55f }; /* v1.8.86 : 0,45 ; v1.8.89 : 0,30 (trop violente) */
/* frottement au sol (m/s²) : la petite glisse plus loin */
static const float k_fric[NVAR] = { 6.0f, 4.5f, 6.0f, 5.0f, 5.0f };
static const float k_invm[NVAR] = { 0.0f, 1.0f, 0.8f, 1.6f, 1.6f }; /* v1.8.188 : chaise bois retirée ; v1.8.189 : chaise rouge */
#define PUSHABLE(b) ((b)->var >= 1) /* v1.8.178 : la petite botte et le mobilier */
static const char *const k_name[NVAR] = { "Ronde", "Petite", "Table", "Chaise", "Chaise rouge" };

static float dim_hx(int v) { return vdim(v)[0]; }
static float dim_hy(int v) { return vdim(v)[1]; }
static float dim_h(int v) { return vdim(v)[2]; }
static int dim_round(int v) { return vdim(v)[3] > 0.5f; }

void bales_init(BalesLogFn log)
{
    g_log = log;
    bales_clear();
}

void bales_clear(void)
{
    memset(g_b, 0, sizeof(g_b));
}

static void wake_all(void);
/* v1.8.181 : chaises : s'asseoir */
static int g_ignore = -1;
void bales_set_ignore(int i) { g_ignore = i; }
/* v1.8.182 : avant de la chaise dans son repère (e1, e2) ; 0 si ce n'est pas une chaise */
int bales_chair_front(int v, float *fx, float *fy)
{
    if (v < BALE_VARIANTS || v >= NVAR || k_furn_seat[v - BALE_VARIANTS][2] <= 0.0f)
        return 0;
    *fx = k_furn_seat[v - BALE_VARIANTS][0];
    *fy = k_furn_seat[v - BALE_VARIANTS][1];
    return 1;
}
int bales_nearest_chair(float x, float z, float maxd)
{
    int i, best = -1;
    float bd = maxd * maxd;
    for (i = 0; i < BALES_MAX; ++i) {
        const Bale *b = &g_b[i];
        float dx, dz;
        if (!b->used || b->held || !IS_FURN(b->var) || k_furn_seat[b->var - BALE_VARIANTS][2] <= 0.0f)
            continue;
        dx = b->pos[0] - x;
        dz = b->pos[2] - z;
        if (dx * dx + dz * dz < bd) {
            bd = dx * dx + dz * dz;
            best = i;
        }
    }
    return best;
}
/* centre de l'assise (monde) et direction de l'avant (horizontale) ; 0 si ce n'est plus une chaise */
int bales_chair_seat(int i, float seat[3], float front[3])
{
    const Bale *b;
    const float *sd;
    float s, c, e1[3], e2[3], fl;
    int j;
    if (i < 0 || i >= BALES_MAX || !g_b[i].used || !IS_FURN(g_b[i].var))
        return 0;
    b = &g_b[i];
    sd = k_furn_seat[b->var - BALE_VARIANTS];
    if (sd[2] <= 0.0f)
        return 0;
    s = sinf(b->yaw);
    c = cosf(b->yaw);
    /* repère comme le dessin (bale_inst) : e1 = (sin, 0, cos), e2 = n x e1 (sol plat : (cos, 0, -sin) ou opposé) */
    e1[0] = s;
    e1[1] = 0.0f;
    e1[2] = c;
    e2[0] = b->n[1] * e1[2] - b->n[2] * e1[1];
    e2[1] = b->n[2] * e1[0] - b->n[0] * e1[2];
    e2[2] = b->n[0] * e1[1] - b->n[1] * e1[0];
    for (j = 0; j < 3; ++j)
        front[j] = e1[j] * sd[0] + e2[j] * sd[1];
    front[1] = 0.0f;
    fl = sqrtf(front[0] * front[0] + front[2] * front[2]);
    if (fl < 1e-4f)
        return 0;
    front[0] /= fl;
    front[2] /= fl;
    for (j = 0; j < 3; ++j)
        seat[j] = b->pos[j] + front[j] * sd[3] + b->n[j] * sd[2];
    return 1;
}

/* v1.8.179 : page de la roue : 0 bottes de foin, 1 mobilier */
void bales_set_cat(int c) { g_cat = c ? 1 : 0; }
void bales_cat_range(int c, int *first, int *n)
{
    *first = c ? BALE_VARIANTS : 0;
    *n = c ? FURN_VARIANTS : BALE_VARIANTS;
}
int bales_count_cat(void)
{
    int i, n = 0;
    for (i = 0; i < BALES_MAX; ++i)
        n += g_b[i].used && IN_CAT(&g_b[i]);
    return n;
}
void bales_clear_cat(void)
{
    int i;
    for (i = 0; i < BALES_MAX; ++i)
        if (g_b[i].used && IN_CAT(&g_b[i]))
            g_b[i].used = 0;
    wake_all();
}

int bales_count(void)
{
    int i, n = 0;
    for (i = 0; i < BALES_MAX; ++i)
        n += g_b[i].used;
    return n;
}

int bales_variants(void)
{
    return NVAR;
}

void bales_set_variant(int v)
{
    if (v >= 0 && v < NVAR)
        g_vsel[CAT_OF(v)] = v;
}

int bales_variant(void)
{
    return g_vsel[g_cat];
}

unsigned long bales_variant_rgb(int v)
{
    return v >= 0 && v < NVAR ? (IS_FURN(v) ? k_furn_rgb[v - BALE_VARIANTS] : k_bale_var_rgb[v]) : 0x50B0D8UL;
}

const char *bales_variant_name(int v)
{
    return v >= 0 && v < NVAR ? k_name[v] : "Botte";
}

void bales_variant_size(int v, float *hx, float *hy, float *h)
{
    if (v < 0 || v >= NVAR)
        v = 0;
    *hx = dim_hx(v);
    *hy = dim_hy(v);
    *h = dim_h(v);
}

/* repère à plat (collisions) */
static void axes(const Bale *b, float e1[2], float e2[2])
{
    float s = sinf(b->yaw), c = cosf(b->yaw);
    e1[0] = s;
    e1[1] = c;
    e2[0] = c;
    e2[1] = -s;
}

/* point (x, z) dans le repère de la botte */
static void to_local(const Bale *b, float x, float z, float *lx, float *ly)
{
    float e1[2], e2[2], dx = x - b->pos[0], dz = z - b->pos[2];
    axes(b, e1, e2);
    *lx = dx * e1[0] + dz * e1[1];
    *ly = dx * e2[0] + dz * e2[1];
}

/* v1.8.91 : (x, z) assez près pour tester (cercle qui contient la botte, agrandi de r) ? */
static int near_xz(const Bale *b, float x, float z, float r)
{
    float dx = x - b->pos[0], dz = z - b->pos[2], R = dim_hx(b->var) + dim_hy(b->var) + r;
    return dx * dx + dz * dz <= R * R;
}

/* dans l'empreinte agrandie de r ? */
static int inside(const Bale *b, float lx, float ly, float r)
{
    float hx = dim_hx(b->var), hy = dim_hy(b->var);
    if (dim_round(b->var))
        return lx * lx + ly * ly <= (hx + r) * (hx + r);
    return fabsf(lx) <= hx + r && fabsf(ly) <= hy + r;
}

/* toutes les bottes (les nôtres, puis celles des autres joueurs) */
#define RPEERS 8
typedef struct {
    uint32_t id;
    double seen;
    Bale b[BALES_MAX];
} RPeer;
static RPeer g_rp[RPEERS];
static volatile long g_rbusy = 0;
static double g_rnow = 0.0;

static int all_count(void)
{
    return BALES_MAX * (1 + RPEERS);
}
static const Bale *all_get(int k)
{
    if (k < BALES_MAX)
        return g_b[k].used && !g_b[k].held ? &g_b[k] : NULL;
    k -= BALES_MAX;
    {
        const RPeer *p = &g_rp[k / BALES_MAX];
        const Bale *b = &p->b[k % BALES_MAX];
        if (!p->id || g_rnow - p->seen > 3.0 || !b->used || g_rbusy)
            return NULL;
        return b;
    }
}

/* support sous (x, z) : sol, ou dessus d'une autre botte (pas plus haut que ymax) */
/* v1.8.182 : mobilier : le relief seul (sans les objets de la piste : table penchée, chaise en l'air) */
static BalesGroundFn g_furn_ground = NULL;
void bales_set_furn_ground(BalesGroundFn fn) { g_furn_ground = fn; }

static float support(const Bale *self, float x, float z, float ymax, BalesGroundFn ground, int *on_bale,
                     float n_out[3])
{
    float h = -1e9f, g;
    int k;
    *on_bale = 0;
    if (self && IS_FURN(self->var) && g_furn_ground) {
        /* v1.8.183 : le mobilier reste au sol (jamais posé sur une autre botte ou un meuble : chaise penchée
           à moitié sur la table) */
        if (ground(x, z, &g))
            return g;
        return h;
    }
    if (ground && ground(x, z, &g))
        h = g;
    for (k = 0; k < all_count(); ++k) {
        const Bale *o = all_get(k);
        float lx, ly, top;
        if (!o || o == self)
            continue;
        if (!near_xz(o, x, z, 0.0f))
            continue;
        to_local(o, x, z, &lx, &ly);
        if (!inside(o, lx, ly, 0.0f))
            continue;
        top = o->pos[1] + dim_h(o->var);
        if (top <= ymax && top > h) {
            h = top;
            *on_bale = 1;
            if (n_out)
                memcpy(n_out, o->n, sizeof(float) * 3);
        }
    }
    return h;
}

/* haut de la botte : plan du sol sous son empreinte */
static void ground_normal(Bale *b, BalesGroundFn ground)
{
    float e1[2], e2[2], hx = dim_hx(b->var), hy = dim_hy(b->var), ha, hb, hc, hd;
    float d1[3], d2[3], n[3], l, hm;
    int furn = IS_FURN(b->var);
    if (furn) {
        /* v1.8.183 : le mobilier reste droit (table penchée sur les bosses) */
        b->n[0] = b->n[2] = 0.0f;
        b->n[1] = 1.0f;
        return;
    }
    axes(b, e1, e2);
    if (furn && g_furn_ground)
        ground = g_furn_ground;
    if (!ground || !ground(b->pos[0] + e1[0] * hx, b->pos[2] + e1[1] * hx, &ha) ||
        !ground(b->pos[0] - e1[0] * hx, b->pos[2] - e1[1] * hx, &hb) ||
        !ground(b->pos[0] + e2[0] * hy, b->pos[2] + e2[1] * hy, &hc) ||
        !ground(b->pos[0] - e2[0] * hy, b->pos[2] - e2[1] * hy, &hd)) {
        b->n[0] = b->n[2] = 0.0f;
        b->n[1] = 1.0f;
        return;
    }
    if (furn) {
        /* v1.8.182 : un coin bien plus haut ou plus bas que le milieu (bord, objet) : ignoré */
        hm = (ha + hb + hc + hd) * 0.25f;
        if (ground(b->pos[0], b->pos[2], &hm)) {
            if (fabsf(ha - hm) > 0.25f) ha = hm;
            if (fabsf(hb - hm) > 0.25f) hb = hm;
            if (fabsf(hc - hm) > 0.25f) hc = hm;
            if (fabsf(hd - hm) > 0.25f) hd = hm;
        }
    }
    d1[0] = e1[0] * 2.0f * hx;
    d1[1] = ha - hb;
    d1[2] = e1[1] * 2.0f * hx;
    d2[0] = e2[0] * 2.0f * hy;
    d2[1] = hc - hd;
    d2[2] = e2[1] * 2.0f * hy;
    /* n = d1 x d2 (vers le haut) */
    n[0] = d1[1] * d2[2] - d1[2] * d2[1];
    n[1] = d1[2] * d2[0] - d1[0] * d2[2];
    n[2] = d1[0] * d2[1] - d1[1] * d2[0];
    if (n[1] < 0.0f) {
        n[0] = -n[0];
        n[1] = -n[1];
        n[2] = -n[2];
    }
    l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l < 1e-6f || n[1] / l < (furn ? 0.966f : 0.5f)) { /* v1.8.182 : mobilier : 15 degrés au plus */
        b->n[0] = b->n[2] = 0.0f;
        b->n[1] = 1.0f;
        return;
    }
    b->n[0] = n[0] / l;
    b->n[1] = n[1] / l;
    b->n[2] = n[2] / l;
}

int bales_add(float x, float y, float z, float yaw, BalesGroundFn ground)
{
    int i, on;
    for (i = 0; i < BALES_MAX; ++i)
        if (!g_b[i].used)
            break;
    if (i == BALES_MAX)
        return -1;
    memset(&g_b[i], 0, sizeof(g_b[i]));
    g_b[i].var = g_vsel[g_cat];
    g_b[i].pos[0] = x;
    g_b[i].pos[2] = z;
    g_b[i].yaw = yaw;
    {
        /* posée sur le sol, ou sur une botte déjà là */
        float n[3] = { 0.0f, 1.0f, 0.0f }, s = support(&g_b[i], x, z, y + 1.6f, ground, &on, n);
        g_b[i].pos[1] = s > -1e8f ? s : y;
        if (on)
            memcpy(g_b[i].n, n, sizeof(n));
        else
            ground_normal(&g_b[i], ground);
    }
    g_b[i].spawn[0] = x;
    g_b[i].spawn[1] = g_b[i].pos[1];
    g_b[i].spawn[2] = z;
    g_b[i].spawn[3] = yaw;
    g_b[i].used = 1;
    if (g_log)
        g_log("bottes : botte %d posee (%s) en (%.1f %.1f %.1f)%s", i, k_name[g_b[i].var], x, g_b[i].pos[1], z,
              on ? " sur une autre botte" : "");
    return i;
}

static void wake_all(void);
int bales_remove_nearest(float x, float z, float maxd)
{
    int i, best = -1;
    float bd = maxd * maxd;
    for (i = 0; i < BALES_MAX; ++i) {
        float dx, dz;
        if (!g_b[i].used || g_b[i].held || !IN_CAT(&g_b[i]))
            continue;
        dx = g_b[i].pos[0] - x;
        dz = g_b[i].pos[2] - z;
        if (dx * dx + dz * dz < bd) {
            bd = dx * dx + dz * dz;
            best = i;
        }
    }
    if (best < 0)
        return 0;
    g_b[best].used = 0;
    wake_all();
    return 1;
}

int bales_moved_count(void)
{
    int i, n = 0;
    for (i = 0; i < BALES_MAX; ++i) {
        const Bale *b = &g_b[i];
        if (b->used && IN_CAT(b) && (fabsf(b->pos[0] - b->spawn[0]) + fabsf(b->pos[2] - b->spawn[2]) > 0.05f ||
                        fabsf(b->yaw - b->spawn[3]) > 0.02f))
            ++n;
    }
    return n;
}

void bales_reset_spawn(void)
{
    int i;
    for (i = 0; i < BALES_MAX; ++i) {
        Bale *b = &g_b[i];
        if (!b->used || b->held || !IN_CAT(b))
            continue;
        b->pos[0] = b->spawn[0];
        b->pos[1] = b->spawn[1];
        b->pos[2] = b->spawn[2];
        b->yaw = b->spawn[3];
        b->v[0] = b->v[1] = b->vy = b->w = 0.0f;
        b->still = b->idle = 0.0f;
    }
}

int bales_carry_nearest(float x, float z, float maxd)
{
    int i, best = -1;
    float bd = maxd * maxd;
    for (i = 0; i < BALES_MAX; ++i) {
        float dx, dz;
        if (!g_b[i].used || g_b[i].held || !PUSHABLE(&g_b[i]) || !IN_CAT(&g_b[i]))
            continue;
        dx = g_b[i].pos[0] - x;
        dz = g_b[i].pos[2] - z;
        if (dx * dx + dz * dz < bd) {
            bd = dx * dx + dz * dz;
            best = i;
        }
    }
    return best;
}

void bales_hold(int i, float x, float y, float z, float yaw)
{
    Bale *b;
    if (i < 0 || i >= BALES_MAX || !g_b[i].used)
        return;
    b = &g_b[i];
    b->held = 1;
    b->pos[0] = x;
    b->pos[1] = y;
    b->pos[2] = z;
    b->yaw = yaw;
    b->n[0] = b->n[2] = 0.0f;
    b->n[1] = 1.0f;
    b->v[0] = b->v[1] = b->vy = b->w = 0.0f;
    b->still = b->idle = 0.0f;
}

void bales_hold_end(int i)
{
    if (i >= 0 && i < BALES_MAX)
        g_b[i].held = 0; /* elle tombe sur ce qu'il y a dessous */
}

void bales_set_spawn(int i, float x, float y, float z, float yaw)
{
    if (i < 0 || i >= BALES_MAX || !g_b[i].used)
        return;
    g_b[i].spawn[0] = x;
    g_b[i].spawn[1] = y;
    g_b[i].spawn[2] = z;
    g_b[i].spawn[3] = yaw;
}

static void wake_all(void)
{
    int i;
    for (i = 0; i < BALES_MAX; ++i)
        g_b[i].still = g_b[i].idle = 0.0f; /* (une botte posée dessus doit retomber) */
}

void bales_remove(int i)
{
    if (i >= 0 && i < BALES_MAX)
        g_b[i].used = 0;
    wake_all();
}

int bales_get_var(int i)
{
    return i >= 0 && i < BALES_MAX && g_b[i].used ? g_b[i].var : -1;
}

int bales_get(int i, float pos[3], float *yaw)
{
    if (i < 0 || i >= BALES_MAX || !g_b[i].used)
        return 0;
    if (pos)
        memcpy(pos, g_b[i].pos, sizeof(float) * 3);
    if (yaw)
        *yaw = g_b[i].yaw;
    return 1;
}

/* deux bottes au même niveau qui se chevauchent (vu de dessus) : séparées (rectangles, axes séparateurs) */
static void separate(Bale *a, Bale *b)
{
    float ax[2][2], bx[2][2], ah[2], bh[2], d[2], best = 1e9f, bn[2] = { 0.0f, 0.0f };
    int k;
    if (a->pos[1] + dim_h(a->var) < b->pos[1] + 0.1f || b->pos[1] + dim_h(b->var) < a->pos[1] + 0.1f)
        return; /* l'une sur l'autre */
    if ((b->pos[0] - a->pos[0]) * (b->pos[0] - a->pos[0]) + (b->pos[2] - a->pos[2]) * (b->pos[2] - a->pos[2]) > 9.0f)
        return; /* v1.8.100 : loin : avant les sinus */
    axes(a, ax[0], ax[1]);
    axes(b, bx[0], bx[1]);
    ah[0] = dim_hx(a->var);
    ah[1] = dim_round(a->var) ? dim_hx(a->var) : dim_hy(a->var);
    bh[0] = dim_hx(b->var);
    bh[1] = dim_round(b->var) ? dim_hx(b->var) : dim_hy(b->var);
    d[0] = b->pos[0] - a->pos[0];
    d[1] = b->pos[2] - a->pos[2];
    if (d[0] * d[0] + d[1] * d[1] > 9.0f)
        return;
    for (k = 0; k < 4; ++k) {
        const float *L = k < 2 ? ax[k] : bx[k - 2];
        float ra = ah[0] * fabsf(ax[0][0] * L[0] + ax[0][1] * L[1]) + ah[1] * fabsf(ax[1][0] * L[0] + ax[1][1] * L[1]);
        float rb = bh[0] * fabsf(bx[0][0] * L[0] + bx[0][1] * L[1]) + bh[1] * fabsf(bx[1][0] * L[0] + bx[1][1] * L[1]);
        float dd = d[0] * L[0] + d[1] * L[1], o = ra + rb - fabsf(dd);
        if (o <= 0.0f)
            return; /* axe séparateur : pas de contact */
        if (o < best) {
            best = o;
            bn[0] = dd < 0.0f ? -L[0] : L[0];
            bn[1] = dd < 0.0f ? -L[1] : L[1];
        }
    }
    /* selon les masses (la ronde ne bouge pas) ; les vitesses le long du contact sont partagées */
    {
        float wa = k_invm[a->var], wb = k_invm[b->var], ws = wa + wb;
        if (ws <= 0.0f)
            return;
        wa /= ws;
        wb /= ws;
        a->pos[0] -= bn[0] * best * wa;
        a->pos[2] -= bn[1] * best * wa;
        b->pos[0] += bn[0] * best * wb;
        b->pos[2] += bn[1] * best * wb;
    }
    {
        float va = a->v[0] * bn[0] + a->v[1] * bn[1], vb = b->v[0] * bn[0] + b->v[1] * bn[1];
        if (va > vb) {
            float m = (va + vb) * 0.5f;
            a->v[0] += (m - va) * bn[0];
            a->v[1] += (m - va) * bn[1];
            b->v[0] += (m - vb) * bn[0];
            b->v[1] += (m - vb) * bn[1];
        }
    }
    a->still = b->still = 0.0f;
}

int bales_awake(void)
{
    int i, n = 0;
    for (i = 0; i < BALES_MAX; ++i)
        n += g_b[i].used && !g_b[i].held && g_b[i].still <= 1.0f;
    return n;
}

void bales_update(float dt, BalesGroundFn ground)
{
    int i, j;
    if (dt <= 0.0f)
        return;
    if (dt > 0.05f)
        dt = 0.05f;
    for (i = 0; i < BALES_MAX; ++i) {
        Bale *b = &g_b[i];
        float sp, sup, n[3] = { 0.0f, 1.0f, 0.0f };
        int on;
        if (!b->used || b->held || i == g_ignore)
            continue; /* v1.8.182 : la chaise où il est assis ne bouge pas */
        if (b->still > 1.0f) {
            b->still += dt; /* v1.8.89 : posée et immobile : rien à recalculer (beaucoup de bottes) */
            continue;
        }
        /* frottement au sol (lourde : elle s'arrête vite) */
        sp = sqrtf(b->v[0] * b->v[0] + b->v[1] * b->v[1]);
        if (sp > 0.0f) {
            float fr = k_fric[b->var] * dt, k = sp > fr ? (sp - fr) / sp : 0.0f;
            b->v[0] *= k;
            b->v[1] *= k;
        }
        if (fabsf(b->w) > 0.0f)
            b->w = fabsf(b->w) > 3.0f * dt ? b->w - (b->w > 0.0f ? 3.0f : -3.0f) * dt : 0.0f;
        b->pos[0] += b->v[0] * dt;
        b->pos[2] += b->v[1] * dt;
        b->yaw += b->w * dt;
        /* sol ou botte dessous */
        sup = support(b, b->pos[0], b->pos[2], b->pos[1] + 0.35f, ground, &on, n);
        if (sup < -1e8f)
            sup = b->pos[1];
        if (b->pos[1] > sup + 0.01f) {
            b->vy -= 9.81f * dt;
            b->pos[1] += b->vy * dt;
            if (b->pos[1] <= sup) {
                b->pos[1] = sup;
                b->vy = 0.0f;
            }
        } else {
            b->pos[1] = sup;
            b->vy = 0.0f;
        }
        if (on)
            memcpy(b->n, n, sizeof(n));
        else
            ground_normal(b, ground);
        if (sp < 0.02f && fabsf(b->w) < 0.01f && b->vy == 0.0f)
            b->still += dt;
        else
            b->still = 0.0f;
        /* v1.8.102 : coincée entre deux bottes (poussée d'avant en arrière d'un millimètre) : elle
           ne s'endormait jamais. Moins de 3 cm parcourus en 1,5 s sans vitesse réelle : endormie. */
        {
            float dx = b->pos[0] - b->ref[0], dy = b->pos[1] - b->ref[1], dz = b->pos[2] - b->ref[2];
            if (sp > 0.1f || fabsf(b->vy) > 0.3f || fabsf(b->w) > 0.05f || dx * dx + dy * dy + dz * dz > 0.03f * 0.03f) {
                b->idle = 0.0f;
                memcpy(b->ref, b->pos, sizeof(b->ref));
            } else if ((b->idle += dt) > 1.5f && b->still <= 1.0f) {
                b->still = 1.5f;
                b->v[0] = b->v[1] = b->vy = b->w = 0.0f;
            }
        }
    }
    {
        /* v1.8.100 : seulement les paires dont une botte bouge (avant : 20 000 paires à chaque appel) */
        int act[BALES_MAX], all[BALES_MAX], na = 0, nl = 0, a, k;
        for (i = 0; i < BALES_MAX; ++i) {
            if (!g_b[i].used || g_b[i].held)
                continue;
            all[nl++] = i;
            if (g_b[i].still < 0.5f)
                act[na++] = i;
        }
        for (a = 0; a < na; ++a) {
            i = act[a];
            for (k = 0; k < nl; ++k) {
                j = all[k];
                if (j == i || (j < i && g_b[j].still < 0.5f))
                    continue; /* paire déjà vue (les deux bougent) */
                if (j < i)
                    separate(&g_b[j], &g_b[i]);
                else
                    separate(&g_b[i], &g_b[j]);
            }
        }
    }
}

int bales_bike_pass(const float pos[3], const float vel[3], float r)
{
    int i, hit = 0;
    float sp = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
    if (sp < 1.0f)
        return 0;
    for (i = 0; i < BALES_MAX; ++i) {
        Bale *b = &g_b[i];
        float lx, ly, along, want, dir[2];
        if (!b->used || b->held || k_take[b->var] <= 0.0f)
            continue;
        if (pos[1] < b->pos[1] - 0.6f || pos[1] > b->pos[1] + dim_h(b->var) + 0.9f)
            continue; /* passe au-dessus (saut) */
        if (!near_xz(b, pos[0], pos[2], r))
            continue;
        to_local(b, pos[0], pos[2], &lx, &ly);
        if (!inside(b, lx, ly, r))
            continue;
        dir[0] = vel[0] / sp;
        dir[1] = vel[2] / sp;
        along = b->v[0] * dir[0] + b->v[1] * dir[1];
        want = sp * k_take[b->var];
        if (along < want) {
            b->v[0] += (want - along) * dir[0];
            b->v[1] += (want - along) * dir[1];
            /* choc décentré : elle tourne un peu */
            {
                float ox = pos[0] - b->pos[0], oz = pos[2] - b->pos[2];
                float cr = ox * dir[1] - oz * dir[0], len = dim_hy(b->var) + dim_hx(b->var);
                b->w += -cr / (len * len) * (want - along) * 1.0f;
                if (b->w > 2.0f)
                    b->w = 2.0f;
                if (b->w < -2.0f)
                    b->w = -2.0f;
            }
            b->still = b->idle = 0.0f;
            if (along < 0.3f) {
                if (g_log)
                    g_log("bottes : botte %d percutee a %.1f m/s, glisse a %.1f m/s", i, sp, want);
                ++hit; /* v1.8.89 : seulement les nouveaux chocs (la moto est freinée une fois) */
            }
        }
    }
    return hit;
}

int bales_height(float x, float z, float ymax, float *h)
{
    int k, ok = 0;
    for (k = 0; k < all_count(); ++k) {
        const Bale *b = all_get(k);
        float lx, ly, top;
        if (!b || PUSHABLE(b))
            continue; /* v1.8.96 : on ne monte pas sur la petite (sinon elle bloque la marche : marche trop haute) */
        if (!near_xz(b, x, z, 0.0f))
            continue;
        to_local(b, x, z, &lx, &ly);
        if (!inside(b, lx, ly, 0.0f))
            continue;
        top = b->pos[1] + dim_h(b->var);
        if (top <= ymax && (!ok || top > *h)) {
            *h = top;
            ok = 1;
        }
    }
    return ok;
}

int bales_blocked(float x, float z, float feet, float r)
{
    int k;
    for (k = 0; k < all_count(); ++k) {
        const Bale *b = all_get(k);
        float lx, ly, top;
        if (!b)
            continue;
        if (PUSHABLE(b))
            continue; /* v1.8.84 : la petite : il la pousse (bales_push_walker) ; v1.8.96 : aussi celles des autres */
        top = b->pos[1] + dim_h(b->var);
        if (feet >= top - 0.06f || b->pos[1] > feet + 1.7f)
            continue; /* dessus, ou bien plus haut que lui */
        if (!near_xz(b, x, z, r))
            continue;
        to_local(b, x, z, &lx, &ly);
        if (inside(b, lx, ly, r))
            return 1;
    }
    return 0;
}

/* sortie la plus courte d'un point (lx, ly) hors de l'empreinte agrandie de r : déplacement local */
static float push_out(const Bale *b, float lx, float ly, float r, float *ox, float *oy)
{
    float hx = dim_hx(b->var) + r, hy = dim_hy(b->var) + r;
    if (dim_round(b->var)) {
        float d = sqrtf(lx * lx + ly * ly), o = hx - d;
        if (d < 1e-4f) {
            *ox = o;
            *oy = 0.0f;
        } else {
            *ox = lx / d * o;
            *oy = ly / d * o;
        }
        return o;
    }
    {
        float px = hx - fabsf(lx), py = hy - fabsf(ly);
        if (px < py) {
            *ox = lx < 0.0f ? -px : px;
            *oy = 0.0f;
            return px;
        }
        *ox = 0.0f;
        *oy = ly < 0.0f ? -py : py;
        return py;
    }
}

/* v1.8.96 : poussées à demander aux autres joueurs (leurs petites bottes) */
static struct {
    uint32_t id;
    int slot;
    float dx, dz;
} g_push[8];
static int g_npush = 0;

int bales_net_push_take(uint32_t *id, int *slot, float *dx, float *dz)
{
    if (!g_npush)
        return 0;
    --g_npush;
    *id = g_push[g_npush].id;
    *slot = g_push[g_npush].slot;
    *dx = g_push[g_npush].dx;
    *dz = g_push[g_npush].dz;
    return 1;
}

void bales_net_push_apply(int slot, float dx, float dz)
{
    Bale *b;
    if (slot < 0 || slot >= BALES_MAX || !g_b[slot].used || g_b[slot].held || !PUSHABLE(&g_b[slot]))
        return;
    if (!(fabsf(dx) < 0.5f && fabsf(dz) < 0.5f))
        return;
    b = &g_b[slot];
    b->pos[0] += dx;
    b->pos[2] += dz;
    b->still = b->idle = 0.0f;
}

int bales_push_walker(float x, float z, float feet, float r, float out[2])
{
    int k, hit = 0;
    out[0] = out[1] = 0.0f;
    for (k = 0; k < all_count(); ++k) {
        const Bale *b = all_get(k);
        float lx, ly, ox, oy, e1[2], e2[2], top;
        if (!b || (g_ignore >= 0 && b == &g_b[g_ignore]))
            continue; /* v1.8.181 : la chaise où il est assis */
        top = b->pos[1] + dim_h(b->var);
        if (feet >= top - 0.06f || b->pos[1] > feet + 1.7f)
            continue;
        if (!near_xz(b, x + out[0], z + out[1], r))
            continue;
        to_local(b, x + out[0], z + out[1], &lx, &ly);
        if (!inside(b, lx, ly, r))
            continue;
        push_out(b, lx, ly, r, &ox, &oy);
        axes(b, e1, e2);
        if (PUSHABLE(b)) {
            /* v1.8.84 : la petite est lourde : elle n'avance qu'à 30 % de son pas, lui est retenu */
            float wx = e1[0] * ox + e2[0] * oy, wz = e1[1] * ox + e2[1] * oy;
            float mv = IS_FURN(b->var) ? 0.08f : 0.3f; /* v1.8.183 : le mobilier bouge à peine quand il le frôle */
            if (k < BALES_MAX) {
                Bale *m = &g_b[k];
                m->pos[0] -= wx * mv;
                m->pos[2] -= wz * mv;
                m->still = m->idle = 0.0f;
            } else {
                /* v1.8.96 : botte d'un autre joueur : on lui demande de la pousser (et on la voit bouger) */
                int q = k - BALES_MAX;
                Bale *m = &g_rp[q / BALES_MAX].b[q % BALES_MAX];
                m->pos[0] -= wx * 0.3f;
                m->pos[2] -= wz * 0.3f;
                if (g_npush < 8) {
                    g_push[g_npush].id = g_rp[q / BALES_MAX].id;
                    g_push[g_npush].slot = q % BALES_MAX;
                    g_push[g_npush].dx = -wx * 0.3f;
                    g_push[g_npush].dz = -wz * 0.3f;
                    ++g_npush;
                }
            }
            out[0] += wx * (1.0f - mv);
            out[1] += wz * (1.0f - mv);
        } else {
            out[0] += e1[0] * ox + e2[0] * oy;
            out[1] += e1[1] * ox + e2[1] * oy;
        }
        hit = 1;
    }
    return hit;
}

int bales_push(const float p[3], float r, float push[3])
{
    int k, hit = 0;
    push[0] = push[1] = push[2] = 0.0f;
    for (k = 0; k < all_count(); ++k) {
        const Bale *b = all_get(k);
        float lx, ly, lh, ox, oy, side, up, e1[2], e2[2], H;
        if (!b)
            continue;
        H = dim_h(b->var);
        lh = p[1] - b->pos[1];
        if (lh < -r || lh > H + r)
            continue;
        if (!near_xz(b, p[0], p[2], r))
            continue;
        to_local(b, p[0], p[2], &lx, &ly);
        if (!inside(b, lx, ly, r))
            continue;
        side = push_out(b, lx, ly, r, &ox, &oy);
        up = H + r - lh;
        if (up < side) {
            push[1] += up; /* sorti par le dessus : il est posé sur la botte */
        } else {
            axes(b, e1, e2);
            push[0] += e1[0] * ox + e2[0] * oy;
            push[2] += e1[1] * ox + e2[1] * oy;
        }
        hit = 1;
    }
    return hit;
}

/* ---------- dessin ---------- */

static unsigned long lit_gray(const float nrm[3])
{
    static const float L[3] = { 0.36f, 0.86f, 0.36f };
    float d = nrm[0] * L[0] + nrm[1] * L[1] + nrm[2] * L[2], k;
    unsigned long g;
    k = 0.50f + 0.55f * (d > 0.0f ? d : 0.0f);
    if (k > 1.0f)
        k = 1.0f;
    g = (unsigned long)(k * 255.0f);
    return g | (g << 8) | (g << 16);
}

/* v1.8.92 : qualité : 0 = haute (modèles complets), 1 = optimisée (botte ronde simplifiée) */
static int g_low = 0;
void bales_set_quality(int optimized)
{
    g_low = optimized != 0;
}

/* v1.8.91 : caméra (tri des bottes à dessiner) */
static int g_view_ok = 0;
static float g_view_eye[3], g_view_fwd[3], g_view_max = 300.0f;
void bales_set_view(const float eye[3], const float fwd[3], float maxd)
{
    g_view_ok = eye && fwd;
    if (g_view_ok) {
        memcpy(g_view_eye, eye, sizeof(g_view_eye));
        memcpy(g_view_fwd, fwd, sizeof(g_view_fwd));
        g_view_max = maxd;
    }
}
static int out_of_view(const Bale *b)
{
    float d[3] = { b->pos[0] - g_view_eye[0], b->pos[1] - g_view_eye[1], b->pos[2] - g_view_eye[2] };
    if (!g_view_ok)
        return 0;
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > g_view_max * g_view_max)
        return 1;
    return d[0] * g_view_fwd[0] + d[1] * g_view_fwd[1] + d[2] * g_view_fwd[2] < -2.5f;
}

static int mesh_one(const Bale *b, float (*pos)[3], float (*uv)[2], unsigned long *col, unsigned char *tex,
                    int max_verts, int tex0)
{
    const float(*m)[9];
    float a[3], e1[3], e2[3], s = sinf(b->yaw), c = cosf(b->yaw), k;
    int nv, v, j;
    if (b->var < 0 || b->var >= NVAR || out_of_view(b))
        return 0;
    nv = (g_low ? vtris_low(b->var) : vtris(b->var)) * 3;
    if (nv > max_verts)
        return 0;
    m = g_low ? vmesh_low(b->var) : vmesh(b->var);
    memcpy(a, b->n, sizeof(a));
    /* e1 (largeur) dans le plan de la pente, e2 = a x e1 */
    e1[0] = s;
    e1[1] = 0.0f;
    e1[2] = c;
    k = e1[0] * a[0] + e1[1] * a[1] + e1[2] * a[2];
    for (j = 0; j < 3; ++j)
        e1[j] -= a[j] * k;
    k = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    if (k < 1e-5f)
        return 0;
    for (j = 0; j < 3; ++j)
        e1[j] /= k;
    e2[0] = a[1] * e1[2] - a[2] * e1[1];
    e2[1] = a[2] * e1[0] - a[0] * e1[2];
    e2[2] = a[0] * e1[1] - a[1] * e1[0];
    for (v = 0; v < nv; ++v) {
        const float *q = m[v];
        float nr[3];
        for (j = 0; j < 3; ++j) {
            pos[v][j] = b->pos[j] + e1[j] * q[0] + e2[j] * q[1] + a[j] * q[2];
            nr[j] = e1[j] * q[3] + e2[j] * q[4] + a[j] * q[5];
        }
        uv[v][0] = q[6];
        uv[v][1] = q[7];
        col[v] = lit_gray(nr);
        tex[v] = (unsigned char)(tex0 + (IS_FURN(b->var) ? BALE_TEXS : 0) + (int)q[8]);
    }
    return nv;
}

int bales_mesh(float (*pos)[3], float (*uv)[2], unsigned long *col, unsigned char *tex, int max_verts,
               const float eye[3], int tex0)
{
    int i, n = 0;
    for (i = 0; i < BALES_MAX; ++i) {
        const Bale *b = &g_src[i];
        if (!b->used)
            continue;
        if (eye) {
            float dx = b->pos[0] - eye[0], dz = b->pos[2] - eye[2];
            if (dx * dx + dz * dz > 250.0f * 250.0f)
                continue;
        }
        n += mesh_one(b, pos + n, uv + n, col + n, tex + n, max_verts - n, tex0);
    }
    return n;
}

int bales_textures(void)
{
    return NTEX;
}

const unsigned char *bales_texture_z(int k, int *len, int *w, int *h)
{
    if (k < 0 || k >= NTEX)
        k = 0;
    if (k >= BALE_TEXS) {
        k -= BALE_TEXS;
        *len = k_furn_tex_zlen[k];
        *w = *h = k_furn_tex_size[k];
        return k_furn_tex_z[k];
    }
    *len = k_bale_tex_zlen[k];
    *w = *h = k_bale_tex_size[k];
    return k_bale_tex_z[k];
}

/* ---------- rediffusion ---------- */

typedef struct {
    float pos[3], yaw, n[3];
    unsigned char slot, var, pad[2];
} BaleRec;

#define HIST_MAX 20000
#define POOL_MAX 120000
static double *g_ht = NULL;
static int *g_hoff = NULL, *g_hcnt = NULL;
static BaleRec *g_pool = NULL;
static int g_hn = 0, g_pn = 0;
static BaleRec g_cur[BALES_MAX];

void bales_hist_clear(void)
{
    g_hn = 0;
    g_pn = 0;
}

static int snap_now(BaleRec *r)
{
    int i, n = 0;
    for (i = 0; i < BALES_MAX; ++i) {
        const Bale *b = &g_b[i];
        if (!b->used)
            continue;
        memset(&r[n], 0, sizeof(r[n]));
        memcpy(r[n].pos, b->pos, sizeof(r[n].pos));
        r[n].yaw = b->yaw;
        memcpy(r[n].n, b->n, sizeof(r[n].n));
        r[n].slot = (unsigned char)i;
        r[n].var = (unsigned char)b->var;
        ++n;
    }
    return n;
}

void bales_hist_record(double t)
{
    int n;
    if (t < 0.0)
        return;
    if (!g_ht) {
        g_ht = (double *)malloc(sizeof(double) * HIST_MAX);
        g_hoff = (int *)malloc(sizeof(int) * HIST_MAX);
        g_hcnt = (int *)malloc(sizeof(int) * HIST_MAX);
        g_pool = (BaleRec *)malloc(sizeof(BaleRec) * POOL_MAX);
    }
    if (!g_ht || !g_hoff || !g_hcnt || !g_pool)
        return;
    if (g_hn && t < g_ht[g_hn - 1] - 1.0)
        bales_hist_clear(); /* l'horloge repart */
    n = snap_now(g_cur);
    if (!n && !g_hn)
        return;
    if (g_hn) {
        if (g_hcnt[g_hn - 1] == n && memcmp(g_pool + g_hoff[g_hn - 1], g_cur, sizeof(BaleRec) * n) == 0)
            return;
        if (t - g_ht[g_hn - 1] < 0.04 && t >= g_ht[g_hn - 1])
            return;
    }
    if (g_hn >= HIST_MAX || g_pn + n > POOL_MAX) {
        int keep = g_hn / 2, off = g_hoff[keep], k;
        memmove(g_pool, g_pool + off, sizeof(BaleRec) * (g_pn - off));
        g_pn -= off;
        memmove(g_ht, g_ht + keep, sizeof(double) * (g_hn - keep));
        memmove(g_hcnt, g_hcnt + keep, sizeof(int) * (g_hn - keep));
        memmove(g_hoff, g_hoff + keep, sizeof(int) * (g_hn - keep));
        g_hn -= keep;
        for (k = 0; k < g_hn; ++k)
            g_hoff[k] -= off;
    }
    g_ht[g_hn] = t;
    g_hoff[g_hn] = g_pn;
    g_hcnt[g_hn] = n;
    memcpy(g_pool + g_pn, g_cur, sizeof(BaleRec) * n);
    g_pn += n;
    g_hn++;
}

int bales_hist_count(void)
{
    return g_hn;
}

void bales_show_live(void)
{
    g_src = g_b;
}

int bales_show_replay(double t)
{
    int lo = 0, hi = g_hn - 1, k, n = 0;
    if (!g_hn) {
        g_src = g_b;
        return 0;
    }
    memset(g_disp, 0, sizeof(g_disp));
    g_src = g_disp;
    if (t < g_ht[0])
        return 0;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (g_ht[mid] <= t)
            lo = mid;
        else
            hi = mid - 1;
    }
    for (k = 0; k < g_hcnt[lo]; ++k) {
        const BaleRec *r = &g_pool[g_hoff[lo] + k];
        Bale *b = &g_disp[r->slot % BALES_MAX];
        b->used = 1;
        memcpy(b->pos, r->pos, sizeof(b->pos));
        b->yaw = r->yaw;
        memcpy(b->n, r->n, sizeof(b->n));
        b->var = r->var < NVAR ? r->var : 0;
        ++n;
    }
    return n;
}

int bales_shown_count(void)
{
    int i, n = 0;
    for (i = 0; i < BALES_MAX; ++i)
        n += g_src[i].used;
    return n;
}

/* ---------- en ligne ---------- */

typedef struct {
    float pos[3], yaw, n[3];
    unsigned char slot, var, pad[2];
} NetBale;

int bales_net_moving(void)
{
    int i;
    for (i = 0; i < BALES_MAX; ++i)
        if (g_b[i].used && g_b[i].still < 0.5f)
            return 1;
    return 0;
}

#define NET_PER_PART 30
int bales_net_pack(unsigned char *buf, int max, int part, int *nparts)
{
    int i, n = 0, k = 0, total = bales_count();
    *nparts = total ? (total + NET_PER_PART - 1) / NET_PER_PART : 1;
    if (max < 4 + NET_PER_PART * (int)sizeof(NetBale))
        return 0;
    buf[0] = 2; /* version (v1.8.89 : en parties) */
    buf[2] = (unsigned char)part;
    buf[3] = (unsigned char)*nparts;
    for (i = 0; i < BALES_MAX; ++i) {
        NetBale nb;
        if (!g_b[i].used)
            continue;
        if (k++ / NET_PER_PART != part)
            continue;
        memcpy(nb.pos, g_b[i].pos, sizeof(nb.pos));
        nb.yaw = g_b[i].yaw;
        memcpy(nb.n, g_b[i].n, sizeof(nb.n));
        nb.slot = (unsigned char)i;
        nb.var = (unsigned char)g_b[i].var;
        nb.pad[0] = nb.pad[1] = 0;
        memcpy(buf + 4 + n * sizeof(NetBale), &nb, sizeof(nb));
        ++n;
    }
    buf[1] = (unsigned char)n;
    return 4 + n * (int)sizeof(NetBale);
}

void bales_net_recv(uint32_t id, const unsigned char *buf, int len, double now)
{
    int i, k = -1, old = 0, n;
    RPeer *p;
    if (len < 4 || buf[0] != 2)
        return;
    n = buf[1];
    if (n > NET_PER_PART || len < 4 + n * (int)sizeof(NetBale))
        return;
    for (i = 0; i < RPEERS; ++i) {
        if (g_rp[i].id == id) {
            k = i;
            break;
        }
        if (g_rp[i].seen < g_rp[old].seen)
            old = i;
    }
    if (k < 0) {
        k = old;
        memset(&g_rp[k], 0, sizeof(g_rp[k]));
        g_rp[k].id = id;
        if (g_log)
            g_log("bottes : en ligne : bottes d'un autre joueur recues");
    }
    p = &g_rp[k];
    g_rbusy = 1;
    if (buf[2] == 0) /* première partie d'un nouvel état : les autres suivent */
        for (i = 0; i < BALES_MAX; ++i)
            p->b[i].used = 0;
    for (i = 0; i < n; ++i) {
        NetBale nb;
        Bale *b;
        memcpy(&nb, buf + 4 + i * sizeof(NetBale), sizeof(nb));
        if (nb.slot >= BALES_MAX || nb.var >= NVAR || !isfinite(nb.pos[0]) || !isfinite(nb.pos[1]) ||
            !isfinite(nb.pos[2]) || !isfinite(nb.yaw) || !isfinite(nb.n[1]))
            continue;
        b = &p->b[nb.slot];
        memset(b, 0, sizeof(*b));
        b->used = 1;
        b->var = nb.var;
        memcpy(b->pos, nb.pos, sizeof(b->pos));
        b->yaw = nb.yaw;
        memcpy(b->n, nb.n, sizeof(b->n));
    }
    p->seen = now;
    g_rbusy = 0;
}

void bales_net_clock(double now)
{
    g_rnow = now;
}

int bales_net_remote_count(void)
{
    int i, j, n = 0;
    for (i = 0; i < RPEERS; ++i)
        if (g_rp[i].id && g_rnow - g_rp[i].seen < 3.0)
            for (j = 0; j < BALES_MAX; ++j)
                n += g_rp[i].b[j].used;
    return n;
}

int bales_mesh_remote(float (*pos)[3], float (*uv)[2], unsigned long *col, unsigned char *tex, int max_verts,
                      int tex0)
{
    int i, j, n = 0;
    if (g_rbusy)
        return 0;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 3.0)
            continue;
        for (j = 0; j < BALES_MAX; ++j)
            if (g_rp[i].b[j].used)
                n += mesh_one(&g_rp[i].b[j], pos + n, uv + n, col + n, tex + n, max_verts - n, tex0);
    }
    return n;
}

/* ---------- v1.8.87 : boîtes pour la physique du jeu (la moto bute dedans) ---------- */
static int boxes_of(const Bale *b, BaleBox *out, int max)
{
    float a[3], e1[3], e2[3], s = sinf(b->yaw), c = cosf(b->yaw), k, H = dim_h(b->var);
    int j, n = 0, q;
    memcpy(a, b->n, sizeof(a));
    e1[0] = s;
    e1[1] = 0.0f;
    e1[2] = c;
    k = e1[0] * a[0] + e1[1] * a[1] + e1[2] * a[2];
    for (j = 0; j < 3; ++j)
        e1[j] -= a[j] * k;
    k = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    if (k < 1e-5f)
        return 0;
    for (j = 0; j < 3; ++j)
        e1[j] /= k;
    e2[0] = a[1] * e1[2] - a[2] * e1[1];
    e2[1] = a[2] * e1[0] - a[0] * e1[2];
    e2[2] = a[0] * e1[1] - a[1] * e1[0];
    for (q = 0; q < (dim_round(b->var) ? 2 : 1) && n < max; ++q) {
        BaleBox *o = &out[n++];
        float u[3], w[3];
        if (q == 0) {
            memcpy(u, e1, sizeof(u));
            memcpy(w, e2, sizeof(w));
        } else {
            /* ronde : deuxième boîte tournée de 45° (les deux font un octogone) */
            for (j = 0; j < 3; ++j) {
                u[j] = (e1[j] + e2[j]) * 0.70710678f;
                w[j] = (e2[j] - e1[j]) * 0.70710678f;
            }
        }
        for (j = 0; j < 3; ++j) {
            o->c[j] = b->pos[j] + a[j] * H * 0.5f;
            /* lignes de R : colonnes = axes x (u), y (haut), z (w) de la boîte */
            o->R[j * 3 + 0] = u[j];
            o->R[j * 3 + 1] = a[j];
            o->R[j * 3 + 2] = w[j];
        }
        if (dim_round(b->var)) {
            o->h[0] = o->h[2] = dim_hx(b->var) * 0.95f;
        } else {
            o->h[0] = dim_hx(b->var);
            o->h[2] = dim_hy(b->var);
        }
        o->h[1] = H * 0.5f;
    }
    return n;
}

/* v1.8.89 : la petite n'est pas un mur pour la physique du jeu quand une moto arrive vite dessus, ou
   pendant qu'elle glisse : le plugin la pousse et freine un peu la moto */
static const float *g_bk_pos = NULL;
static float g_bk_sp = 0.0f;
static int soft(const Bale *b)
{
    /* v1.8.96 : la petite n'est jamais un mur pour le jeu (à l'arrêt, la moto la poussait pas) */
    return PUSHABLE(b);
    if (!PUSHABLE(b))
        return 0;
    if (b >= g_b && b < g_b + BALES_MAX && b->still < 0.3f)
        return 1; /* (une des nôtres, qui glisse) */
    if (g_bk_pos && g_bk_sp > 3.0f) {
        float dx = b->pos[0] - g_bk_pos[0], dz = b->pos[2] - g_bk_pos[2];
        if (dx * dx + dz * dz < 3.5f * 3.5f)
            return 1;
    }
    return 0;
}

int bales_boxes(BaleBox *out, int *slot, int max)
{
    int i, j, n = 0, rs = BALES_MAX * 2;
    for (i = 0; i < BALES_MAX; ++i) {
        int k, m;
        if (!g_b[i].used || g_b[i].held || soft(&g_b[i]))
            continue;
        m = boxes_of(&g_b[i], out + n, max - n);
        for (k = 0; k < m; ++k)
            slot[n + k] = i * 2 + k;
        n += m;
    }
    if (g_rbusy)
        return n;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 3.0)
            continue;
        for (j = 0; j < BALES_MAX; ++j) {
            int k, m;
            if (!g_rp[i].b[j].used || soft(&g_rp[i].b[j]))
                continue;
            m = boxes_of(&g_rp[i].b[j], out + n, max - n);
            for (k = 0; k < m; ++k)
                slot[n + k] = rs++;
            n += m;
        }
    }
    return n;
}

void bales_set_bike(const float pos[3], float speed)
{
    g_bk_pos = pos;
    g_bk_sp = speed;
}

/* v1.8.94 : notre moto (pos, rayon r) dans une petite botte d'un autre joueur ? clé (joueur, numéro) */
int bales_net_bike_touch(const float pos[3], float r, uint32_t *key)
{
    int i, j;
    if (g_rbusy)
        return 0;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 3.0)
            continue;
        for (j = 0; j < BALES_MAX; ++j) {
            const Bale *b = &g_rp[i].b[j];
            float lx, ly;
            if (!b->used || !PUSHABLE(b) || pos[1] < b->pos[1] - 0.6f || pos[1] > b->pos[1] + dim_h(b->var) + 0.9f ||
                !near_xz(b, pos[0], pos[2], r))
                continue;
            to_local(b, pos[0], pos[2], &lx, &ly);
            if (inside(b, lx, ly, r)) {
                *key = g_rp[i].id * 256u + (uint32_t)j;
                return 1;
            }
        }
    }
    return 0;
}

/* v1.8.96 : la ronde est un mur pour notre moto : point p (moto), rayon r : déplacement (x, z) pour
   l'en sortir et normale (vers l'extérieur) ; rend la profondeur (0 : pas dedans) */
float bales_round_block(const float p[3], float r, float out[2], float n[2])
{
    int k;
    float best = 0.0f;
    out[0] = out[1] = n[0] = n[1] = 0.0f;
    for (k = 0; k < all_count(); ++k) {
        const Bale *b = all_get(k);
        float dx, dz, d, R, o;
        if (!b || !dim_round(b->var))
            continue;
        if (p[1] < b->pos[1] - 0.3f || p[1] > b->pos[1] + dim_h(b->var) + 0.2f)
            continue; /* au-dessus (saut) */
        dx = p[0] - b->pos[0];
        dz = p[2] - b->pos[2];
        R = dim_hx(b->var) + r;
        d = sqrtf(dx * dx + dz * dz);
        if (d >= R)
            continue;
        o = R - d;
        if (o > best) {
            best = o;
            if (d < 1e-4f) {
                n[0] = 1.0f;
                n[1] = 0.0f;
            } else {
                n[0] = dx / d;
                n[1] = dz / d;
            }
            out[0] = n[0] * o;
            out[1] = n[1] * o;
        }
    }
    return best;
}

/* ---------- v1.8.97 : dessin par modèle ---------- */
const float *bales_model_data(int model, int *nverts)
{
    switch (model) {
    case OBJM_BALE_ROUND:
        *nverts = k_bale_tris[0] * 3;
        return &k_bale_m[0][0][0];
    case OBJM_BALE_ROUND_LOW:
        *nverts = k_bale_tris_low[0] * 3;
        return &k_bale_low[0][0][0];
    case OBJM_BALE_SMALL:
        *nverts = k_bale_tris[1] * 3;
        return &k_bale_m[1][0][0];
    }
    if (model >= OBJM_FURN0 && model < OBJM_FURN0 + FURN_VARIANTS) {
        *nverts = k_furn_tris[model - OBJM_FURN0] * 3;
        return &k_furn_m[model - OBJM_FURN0][0][0];
    }
    *nverts = 0;
    return NULL;
}

/* botte -> objet ; près de la caméra (40 m) la ronde complète, plus loin la simplifiée */
static int bale_inst(ObjInst *o, const Bale *b, int tex0)
{
    float a[3], e1[3], e2[3], s = sinf(b->yaw), c = cosf(b->yaw), k;
    int j, low = g_low;
    if (b->var < 0 || b->var >= NVAR || out_of_view(b))
        return 0;
    if (!low && g_view_ok) {
        float d[3] = { b->pos[0] - g_view_eye[0], b->pos[1] - g_view_eye[1], b->pos[2] - g_view_eye[2] };
        low = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 40.0f * 40.0f;
    }
    memcpy(a, b->n, sizeof(a));
    e1[0] = s;
    e1[1] = 0.0f;
    e1[2] = c;
    k = e1[0] * a[0] + e1[1] * a[1] + e1[2] * a[2];
    for (j = 0; j < 3; ++j)
        e1[j] -= a[j] * k;
    k = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    if (k < 1e-5f)
        return 0;
    for (j = 0; j < 3; ++j)
        e1[j] /= k;
    e2[0] = a[1] * e1[2] - a[2] * e1[1];
    e2[1] = a[2] * e1[0] - a[0] * e1[2];
    e2[2] = a[0] * e1[1] - a[1] * e1[0];
    if (dim_round(b->var))
        o->model = low ? OBJM_BALE_ROUND_LOW : OBJM_BALE_ROUND;
    else if (IS_FURN(b->var))
        o->model = OBJM_FURN0 + b->var - BALE_VARIANTS; /* v1.8.178 */
    else
        o->model = OBJM_BALE_SMALL;
    /* une seule texture par variante (la ronde : enrubannage, la petite : paille, le mobilier : la sienne) */
    o->tex = tex0 + vtex(b->var);
    objinst_matrix(o->m, e1, e2, a, b->pos);
    return 1;
}

int bales_instances(ObjInst *out, int max, int tex0)
{
    int i, n = 0;
    for (i = 0; i < BALES_MAX && n < max; ++i)
        if (g_src[i].used)
            n += bale_inst(&out[n], &g_src[i], tex0);
    return n;
}

int bales_instances_remote(ObjInst *out, int max, int tex0)
{
    int i, j, n = 0;
    if (g_rbusy)
        return 0;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 3.0)
            continue;
        for (j = 0; j < BALES_MAX && n < max; ++j)
            if (g_rp[i].b[j].used)
                n += bale_inst(&out[n], &g_rp[i].b[j], tex0);
    }
    return n;
}

/*
 * ramps.c - v1.8.119 : rampes FMX (voir ramps.h)
 */
#include "ramps.h"
#include "terrain.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ramp_model.inc"

typedef struct {
    int used;
    int shape, color;
    float pos[3]; /* bas de la rampe (milieu de la largeur), au sol */
    float yaw;    /* le saut vers (sin yaw, 0, cos yaw) */
    float sx, sy; /* v1.8.124 : pente du sol sous la rampe (m par m) : en largeur, en longueur */
} Ramp;

static Ramp g_r[RAMPS_MAX];
static int g_shape = 0, g_color = 1;
static RampsLogFn g_log = NULL;

/* rampes des autres joueurs (reçues par le fil du réseau) */
#define RPEERS 8
static struct {
    uint32_t id;
    double seen;
    Ramp r[RAMPS_MAX];
} g_rp[RPEERS];
static const Ramp *nth_ramp(int k);
static volatile int g_rbusy = 0;
static double g_rnow = 0.0;

void ramps_init(RampsLogFn log)
{
    g_log = log;
}

int ramps_count(void)
{
    int i, n = 0;
    for (i = 0; i < RAMPS_MAX; ++i)
        n += g_r[i].used;
    return n;
}

int ramps_shapes(void) { return RAMP_SHAPES; }
/* v1.8.150 : piste d'élan retirée du menu (la dernière forme : gardée pour dessiner celles des autres) */
int ramps_shapes_menu(void) { return RAMP_SHAPES - 1; }
int ramps_colors(void) { return RAMP_COLORS; }
const char *ramps_shape_name(int s) { return s >= 0 && s < RAMP_SHAPES ? k_ramp_name[s] : ""; }
const char *ramps_color_name(int c) { return c >= 0 && c < RAMP_COLORS ? k_ramp_color[c] : ""; }
unsigned long ramps_color_rgb(int c) { return c >= 0 && c < RAMP_COLORS ? k_ramp_rgb[c] : 0x808080UL; }
void ramps_set_shape(int s) { if (s >= 0 && s < RAMP_SHAPES - 1) g_shape = s; }
void ramps_set_color(int c) { if (c >= 0 && c < RAMP_COLORS) g_color = c; }
int ramps_shape(void) { return g_shape; }
int ramps_color(void) { return g_color; }
float ramps_length(int s) { return s >= 0 && s < RAMP_SHAPES ? k_ramp_dim[s][1] : 0.0f; }
float ramps_shape_height(int s) { return s >= 0 && s < RAMP_SHAPES ? k_ramp_dim[s][2] : 0.0f; }
int ramps_shape_fixed(int s) { return s >= 0 && s < RAMP_SHAPES && k_ramp_ftex[s] >= 0; }

float ramps_icon_prof(int s, float u)
{
    static float top = 0.0f;
    float t;
    int j, k;
    if (top <= 0.0f)
        for (k = 0; k < RAMP_SHAPES; ++k)
            for (j = 0; j <= RAMP_PROF; ++j)
                top = k_ramp_prof[k][j] > top ? k_ramp_prof[k][j] : top;
    if (s < 0 || s >= RAMP_SHAPES || top <= 0.0f)
        return 0.0f;
    u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
    t = u * RAMP_PROF;
    j = (int)t;
    if (j >= RAMP_PROF)
        j = RAMP_PROF - 1;
    t -= (float)j;
    return (k_ramp_prof[s][j] + (k_ramp_prof[s][j + 1] - k_ramp_prof[s][j]) * t) / top;
}

/* repère : x (largeur) = (-cos, 0, sin), y (longueur, vers le saut) = (sin, 0, cos) */
static void axes(const Ramp *r, float ex[3], float ey[3])
{
    float s = sinf(r->yaw), c = cosf(r->yaw);
    ex[0] = -c; ex[1] = 0.0f; ex[2] = s;
    ey[0] = s;  ey[1] = 0.0f; ey[2] = c;
}

/* dessus du tablier à la distance ly du bas */
static float prof(int shape, float ly)
{
    float L = k_ramp_dim[shape][1], t;
    int k;
    if (ly <= 0.0f)
        return k_ramp_prof[shape][0];
    if (ly >= L)
        return k_ramp_prof[shape][RAMP_PROF];
    t = ly / L * RAMP_PROF;
    k = (int)t;
    if (k >= RAMP_PROF)
        k = RAMP_PROF - 1;
    t -= (float)k;
    return k_ramp_prof[shape][k] + (k_ramp_prof[shape][k + 1] - k_ramp_prof[shape][k]) * t;
}

/* v1.8.124 : plan du sol sous l'emprise (moindres carrés), puis baissé pour ne flotter nulle part */
static void fit_ground(Ramp *r, int (*ground)(float, float, float *))
{
    float ex[3], ey[3], W = k_ramp_dim[r->shape][0], L = k_ramp_dim[r->shape][1];
    float sl[64][3], m = 0.0f, mx = 0.0f, my = 0.0f, cxx = 0.0f, cyy = 0.0f, cxh = 0.0f, cyh = 0.0f, a, d = 0.0f;
    int n = 0, i, j, k;
    r->sx = r->sy = 0.0f;
    if (!ground)
        return;
    axes(r, ex, ey);
    for (j = 0; j <= 10; ++j)
        for (i = -2; i <= 2; ++i) {
            float lx = W * 0.5f * (float)i, ly = L * (float)j / 10.0f, h;
            float px = r->pos[0] + ex[0] * lx + ey[0] * ly, pz = r->pos[2] + ex[2] * lx + ey[2] * ly;
            if (n < 64 && ground(px, pz, &h) && isfinite(h)) {
                sl[n][0] = lx; sl[n][1] = ly; sl[n][2] = h;
                ++n;
            }
        }
    if (n < 6)
        return;
    for (k = 0; k < n; ++k) {
        mx += sl[k][0]; my += sl[k][1]; m += sl[k][2];
    }
    mx /= n; my /= n; m /= n;
    for (k = 0; k < n; ++k) {
        float dx = sl[k][0] - mx, dy = sl[k][1] - my, dh = sl[k][2] - m;
        cxx += dx * dx; cyy += dy * dy; cxh += dx * dh; cyh += dy * dh;
    }
    r->sx = cxx > 1e-4f ? cxh / cxx : 0.0f;
    r->sy = cyy > 1e-4f ? cyh / cyy : 0.0f;
    /* v1.8.125 : pente limitée (8 %) : sur une colline, une rampe trop penchée envoyait la moto presque à la verticale */
    r->sx = r->sx < -0.08f ? -0.08f : r->sx > 0.08f ? 0.08f : r->sx;
    r->sy = r->sy < -0.08f ? -0.08f : r->sy > 0.08f ? 0.08f : r->sy;
    a = m - r->sx * mx - r->sy * my;
    for (k = 0; k < n; ++k) {
        float e = a + r->sx * sl[k][0] + r->sy * sl[k][1] - sl[k][2];
        d = e > d ? e : d;
    }
    r->pos[1] = a - (d > 1.2f ? 1.2f : d);
}

int ramps_add(float x, float y, float z, float yaw, int (*ground)(float x, float z, float *h))
{
    int i;
    for (i = 0; i < RAMPS_MAX; ++i)
        if (!g_r[i].used) {
            Ramp *r = &g_r[i];
            r->used = 1;
            r->shape = g_shape;
            r->color = g_color;
            r->pos[0] = x;
            r->pos[1] = y;
            r->pos[2] = z;
            r->yaw = yaw;
            fit_ground(r, ground);
            if (g_log)
                g_log("rampes : rampe %d posee (%s, %s) en (%.1f %.1f %.1f), cap %.0f deg, pente sol %.1f%% / %.1f%% (sol au bas %.2f)",
                      i, k_ramp_name[r->shape], k_ramp_color[r->color], x, r->pos[1], z, yaw * 57.29578f, r->sy * 100.0f,
                      r->sx * 100.0f, y);
            return i;
        }
    return -1;
}

/* distance horizontale de (x, z) à l'emprise de la rampe (0 dedans) */
static float dist_to(const Ramp *r, float x, float z)
{
    float ex[3], ey[3], dx = x - r->pos[0], dz = z - r->pos[2], lx, ly, ox, oy;
    float W = k_ramp_dim[r->shape][0], L = k_ramp_dim[r->shape][1];
    axes(r, ex, ey);
    lx = dx * ex[0] + dz * ex[2];
    ly = dx * ey[0] + dz * ey[2];
    ox = fabsf(lx) > W ? fabsf(lx) - W : 0.0f;
    oy = ly < 0.0f ? -ly : ly > L ? ly - L : 0.0f;
    return sqrtf(ox * ox + oy * oy);
}

int ramps_remove_nearest(float x, float z, float maxd)
{
    int i, best = -1;
    float bd = maxd;
    for (i = 0; i < RAMPS_MAX; ++i) {
        float d;
        if (!g_r[i].used)
            continue;
        d = dist_to(&g_r[i], x, z);
        if (d <= bd) {
            bd = d;
            best = i;
        }
    }
    if (best < 0)
        return 0;
    g_r[best].used = 0;
    if (g_log)
        g_log("rampes : rampe %d retiree", best);
    return 1;
}

void ramps_clear(void)
{
    memset(g_r, 0, sizeof(g_r));
}

static int height_of(const Ramp *r, float x, float z, float ymax, float *h)
{
    float ex[3], ey[3], dx = x - r->pos[0], dz = z - r->pos[2], lx, ly, top;
    if (!r->used || r->shape < 0 || r->shape >= RAMP_SHAPES)
        return 0;
    axes(r, ex, ey);
    lx = dx * ex[0] + dz * ex[2];
    ly = dx * ey[0] + dz * ey[2];
    if (fabsf(lx) > k_ramp_dim[r->shape][0] || ly < 0.0f || ly > k_ramp_dim[r->shape][1])
        return 0;
    top = r->pos[1] + prof(r->shape, ly) + r->sx * lx + r->sy * ly;
    if (top > ymax)
        return 0;
    *h = top;
    return 1;
}

float ramps_local(float x, float z, float *lx, float *ly, float *W, float *L)
{
    int i, best = -1;
    float bd = 1e9f;
    for (i = 0; i < RAMPS_MAX; ++i) {
        float d;
        if (!g_r[i].used)
            continue;
        d = dist_to(&g_r[i], x, z);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (best < 0)
        return -1.0f;
    {
        const Ramp *r = &g_r[best];
        float ex[3], ey[3], dx = x - r->pos[0], dz = z - r->pos[2];
        axes(r, ex, ey);
        *lx = dx * ex[0] + dz * ex[2];
        *ly = dx * ey[0] + dz * ey[2];
        *W = k_ramp_dim[r->shape][0];
        *L = k_ramp_dim[r->shape][1];
    }
    return bd;
}

float ramps_top(int i)
{
    float m = 0.0f;
    int j;
    if (i < 0 || i >= RAMPS_MAX || !g_r[i].used)
        return 0.0f;
    for (j = 0; j <= RAMP_PROF; ++j) {
        float ly = k_ramp_dim[g_r[i].shape][1] * (float)j / RAMP_PROF;
        float h = k_ramp_prof[g_r[i].shape][j] + g_r[i].sy * ly;
        m = h > m ? h : m;
    }
    return g_r[i].pos[1] + m + fabsf(g_r[i].sx) * k_ramp_dim[g_r[i].shape][0];
}

int ramps_blocked(float x, float z, float feet, float r, float step)
{
    static const float k_dir[5][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    int k;
    float h;
    for (k = 0; k < 5; ++k)
        if (ramps_height(x + k_dir[k][0] * r, z + k_dir[k][1] * r, 1e9f, &h) && h > feet + step)
            return 1;
    return 0;
}

int ramps_height(float x, float z, float ymax, float *h)
{
    int i, j, ok = 0;
    float t;
    for (i = 0; i < RAMPS_MAX; ++i)
        if (height_of(&g_r[i], x, z, ymax, &t) && (!ok || t > *h)) {
            *h = t;
            ok = 1;
        }
    if (g_rbusy)
        return ok;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 5.0)
            continue;
        for (j = 0; j < RAMPS_MAX; ++j)
            if (height_of(&g_rp[i].r[j], x, z, ymax, &t) && (!ok || t > *h)) {
                *h = t;
                ok = 1;
            }
    }
    return ok;
}

/* boîtes d'une rampe : tablier en 12 dalles inclinées (sous la surface), dos vertical, haut plat */
static int boxes_of(const Ramp *r, BaleBox *out, int max)
{
    const float th = 0.5f; /* épaisseur des dalles */
    float ex[3], ey[3], up[3] = { 0.0f, 1.0f, 0.0f }, W = k_ramp_dim[r->shape][0], L = k_ramp_dim[r->shape][1];
    int k, j, n = 0;
    axes(r, ex, ey);
    /* v1.8.143 : 16 dalles (le profil a 96 points), pente du sol comprise */
    for (k = 0; k + 6 <= RAMP_PROF && n < max - 1; k += 6) {
        float ya = L * k / RAMP_PROF, yb = L * (k + 6) / RAMP_PROF;
        float ha = k_ramp_prof[r->shape][k] + r->sy * ya, hb = k_ramp_prof[r->shape][k + 6] + r->sy * yb;
        float dy = yb - ya, dh = hb - ha, l = sqrtf(dy * dy + dh * dh), my = (ya + yb) * 0.5f, mh = (ha + hb) * 0.5f;
        BaleBox *o = &out[n++];
        float a[3], w[3];
        if (l < 1e-4f) {
            --n;
            continue;
        }
        dy /= l;
        dh /= l;
        for (j = 0; j < 3; ++j) {
            w[j] = ey[j] * dy + up[j] * dh; /* le long de la pente */
            a[j] = up[j] * dy - ey[j] * dh; /* perpendiculaire à la pente, vers le haut */
            o->c[j] = r->pos[j] + ey[j] * my + up[j] * mh - a[j] * th * 0.5f;
            o->R[j * 3 + 0] = ex[j];
            o->R[j * 3 + 1] = a[j];
            o->R[j * 3 + 2] = w[j];
        }
        o->h[0] = W;
        o->h[1] = th * 0.5f;
        o->h[2] = l * 0.5f + 0.04f; /* (les dalles se recouvrent un peu : pas de marche entre elles) */
    }
    if (n < max) {
        /* dos : on ne traverse pas la rampe par derrière (son haut sous le bord du tablier : la moto
           ne doit pas le toucher en sortant de la rampe) */
        float top = prof(r->shape, L - 0.3f) + r->sy * (L - 0.3f) - 0.2f;
        BaleBox *o = &out[n++];
        for (j = 0; j < 3; ++j) {
            o->c[j] = r->pos[j] + ey[j] * (L - 0.15f) + up[j] * (top * 0.5f);
            o->R[j * 3 + 0] = ex[j];
            o->R[j * 3 + 1] = up[j];
            o->R[j * 3 + 2] = ey[j];
        }
        o->h[0] = W;
        o->h[1] = top * 0.5f;
        o->h[2] = 0.15f;
    }
    return n;
}

int ramps_boxes(BaleBox *out, int *slot, int max, int base)
{
    int i, j, n = 0, rs = base + RAMPS_MAX * RAMP_BOXES;
    for (i = 0; i < RAMPS_MAX; ++i) {
        int k, m;
        if (!g_r[i].used)
            continue;
        m = boxes_of(&g_r[i], out + n, max - n);
        for (k = 0; k < m; ++k)
            slot[n + k] = base + i * RAMP_BOXES + k;
        n += m;
    }
    if (g_rbusy)
        return n;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 5.0)
            continue;
        for (j = 0; j < RAMPS_MAX; ++j) {
            int k, m;
            if (!g_rp[i].r[j].used)
                continue;
            m = boxes_of(&g_rp[i].r[j], out + n, max - n);
            for (k = 0; k < m; ++k)
                slot[n + k] = rs++;
            n += m;
        }
    }
    return n;
}

static void terrain_of(const Ramp *r, float yoff)
{
    float ex[3], ey[3], c[3] = { r->pos[0], r->pos[1] - yoff, r->pos[2] };
    if (!r->used || r->shape < 0 || r->shape >= RAMP_SHAPES)
        return;
    axes(r, ex, ey);
    terrain_raise(c, ex, ey, k_ramp_dim[r->shape][0], k_ramp_dim[r->shape][1], k_ramp_prof[r->shape], RAMP_PROF, r->sx, r->sy);
}

static uint32_t sig_add(uint32_t h, const Ramp *r)
{
    const unsigned char *p = (const unsigned char *)r;
    size_t k;
    if (!r->used)
        return h;
    for (k = 0; k < sizeof(*r); ++k)
        h = (h ^ p[k]) * 16777619u;
    return h;
}

uint32_t ramps_signature(void)
{
    uint32_t h = 2166136261u;
    int i, j;
    for (i = 0; i < RAMPS_MAX; ++i)
        h = sig_add(h, &g_r[i]);
    if (g_rbusy)
        return 0; /* (réessayé à l'appel suivant) */
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 5.0)
            continue;
        for (j = 0; j < RAMPS_MAX; ++j)
            h = sig_add(h, &g_rp[i].r[j]);
    }
    return h;
}

void ramps_terrain(float yoff)
{
    int i, j;
    for (i = 0; i < RAMPS_MAX; ++i)
        terrain_of(&g_r[i], yoff);
    if (g_rbusy)
        return;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 5.0)
            continue;
        for (j = 0; j < RAMPS_MAX; ++j)
            terrain_of(&g_rp[i].r[j], yoff);
    }
}

const float *ramps_model_data(int shape, int *nverts)
{
    if (shape < 0 || shape >= RAMP_SHAPES) {
        *nverts = 0;
        return NULL;
    }
    *nverts = (int)k_ramp_dim[shape][3] * 3;
    return &k_ramp_m[shape][0][0];
}

int ramps_textures(void)
{
    return RAMP_TEXS; /* v1.8.121 : 4 couleurs + réception */
}

const unsigned char *ramps_texture_z(int k, int *len, int *w, int *h)
{
    if (k < 0 || k >= RAMP_TEXS)
        return NULL;
    *len = k_ramp_tex_zlen[k];
    *w = *h = RAMP_TEX_SIZE;
    return k_ramp_tex_z[k];
}

static int inst_of(ObjInst *o, const Ramp *r, int tex0)
{
    float ex[3], ey[3], up[3] = { 0.0f, 1.0f, 0.0f };
    if (!r->used || r->shape < 0 || r->shape >= RAMP_SHAPES)
        return 0;
    axes(r, ex, ey);
    /* v1.8.124 : cisaillement : la hauteur reste verticale, la rampe suit la pente du sol */
    ex[1] = r->sx;
    ey[1] = r->sy;
    o->model = OBJM_RAMP0 + r->shape;
    o->tex = tex0 + (k_ramp_ftex[r->shape] >= 0 ? k_ramp_ftex[r->shape] : r->color); /* (réception : sa texture) */
    objinst_matrix(o->m, ex, ey, up, r->pos);
    return 1;
}

/* ---- v1.8.159 : rediffusion : les rampes (les nôtres et celles des autres) telles qu'elles étaient ---- */
#define RH_MAX 2000
#define RH_POOL 60000
static double *g_rht = NULL;
static int *g_rhoff = NULL, *g_rhcnt = NULL;
static Ramp *g_rhpool = NULL;
static int g_rhn = 0, g_rhpn = 0;
static uint32_t g_rhsig = 0;
static Ramp g_rshow[RAMPS_MAX * (1 + RPEERS)];
static int g_rshow_n = -1; /* -1 : en direct */

void ramps_hist_clear(void)
{
    g_rhn = g_rhpn = 0;
    g_rhsig = 0;
}

void ramps_hist_record(double t)
{
    static Ramp cur[RAMPS_MAX * (1 + RPEERS)];
    uint32_t sig;
    int n = 0, k;
    const Ramp *r;
    if (t < 0.0)
        return;
    if (!g_rht) {
        g_rht = (double *)malloc(sizeof(double) * RH_MAX);
        g_rhoff = (int *)malloc(sizeof(int) * RH_MAX);
        g_rhcnt = (int *)malloc(sizeof(int) * RH_MAX);
        g_rhpool = (Ramp *)malloc(sizeof(Ramp) * RH_POOL);
    }
    if (!g_rht || !g_rhoff || !g_rhcnt || !g_rhpool)
        return;
    if (g_rhn && t < g_rht[g_rhn - 1] - 1.0)
        ramps_hist_clear(); /* l'horloge repart */
    sig = ramps_signature();
    if (!sig || (g_rhn && sig == g_rhsig))
        return;
    for (k = 0; (r = nth_ramp(k)) != NULL && n < RAMPS_MAX * (1 + RPEERS); ++k)
        cur[n++] = *r;
    if (!n && !g_rhn)
        return;
    if (g_rhn >= RH_MAX || g_rhpn + n > RH_POOL)
        ramps_hist_clear();
    g_rhsig = sig;
    g_rht[g_rhn] = t;
    g_rhoff[g_rhn] = g_rhpn;
    g_rhcnt[g_rhn] = n;
    memcpy(g_rhpool + g_rhpn, cur, sizeof(Ramp) * (size_t)n);
    g_rhpn += n;
    ++g_rhn;
}

int ramps_hist_count(void)
{
    return g_rhn;
}

void ramps_show_live(void)
{
    g_rshow_n = -1;
}

int ramps_show_replay(double t)
{
    int lo = 0, hi = g_rhn - 1;
    if (!g_rhn) {
        g_rshow_n = -1;
        return 0;
    }
    if (t < g_rht[0]) {
        g_rshow_n = 0;
        return 0;
    }
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (g_rht[mid] <= t)
            lo = mid;
        else
            hi = mid - 1;
    }
    memcpy(g_rshow, g_rhpool + g_rhoff[lo], sizeof(Ramp) * (size_t)g_rhcnt[lo]);
    g_rshow_n = g_rhcnt[lo];
    return g_rshow_n;
}

int ramps_instances(ObjInst *out, int max, int tex0)
{
    int i, j, n = 0;
    if (g_rshow_n >= 0) {
        for (i = 0; i < g_rshow_n && n < max; ++i)
            n += inst_of(&out[n], &g_rshow[i], tex0);
        return n;
    }
    for (i = 0; i < RAMPS_MAX && n < max; ++i)
        n += inst_of(&out[n], &g_r[i], tex0);
    if (g_rbusy)
        return n;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 5.0)
            continue;
        for (j = 0; j < RAMPS_MAX && n < max; ++j)
            n += inst_of(&out[n], &g_rp[i].r[j], tex0);
    }
    return n;
}

/* ---- en ligne : un paquet avec toutes nos rampes (version 2 : + pente, 26 octets par rampe) ---- */
int ramps_net_pack(unsigned char *buf, int max)
{
    int i, n = 2;
    if (max < 2 + RAMPS_MAX * 26)
        return 0;
    buf[0] = 2;
    buf[1] = 0;
    for (i = 0; i < RAMPS_MAX; ++i) {
        const Ramp *r = &g_r[i];
        if (!r->used)
            continue;
        buf[n] = (unsigned char)i;
        buf[n + 1] = (unsigned char)(r->shape * 16 + r->color);
        memcpy(buf + n + 2, r->pos, 12);
        memcpy(buf + n + 14, &r->yaw, 4);
        memcpy(buf + n + 18, &r->sx, 4);
        memcpy(buf + n + 22, &r->sy, 4);
        n += 26;
        ++buf[1];
    }
    return n;
}

void ramps_net_recv(uint32_t id, const unsigned char *buf, int len, double now)
{
    int i, k, slot = -1, cnt, rs;
    if (len < 2 || (buf[0] != 1 && buf[0] != 2))
        return;
    rs = buf[0] == 2 ? 26 : 18; /* (version 1 : sans pente) */
    cnt = buf[1];
    if (len < 2 + cnt * rs)
        return;
    for (i = 0; i < RPEERS; ++i)
        if (g_rp[i].id == id)
            slot = i;
    if (slot < 0)
        for (i = 0; i < RPEERS; ++i)
            if (!g_rp[i].id || now - g_rp[i].seen > 30.0) {
                slot = i;
                break;
            }
    if (slot < 0)
        return;
    g_rbusy = 1;
    g_rp[slot].id = id;
    g_rp[slot].seen = now;
    memset(g_rp[slot].r, 0, sizeof(g_rp[slot].r));
    for (k = 0; k < cnt; ++k) {
        const unsigned char *p = buf + 2 + k * rs;
        Ramp *r;
        if (p[0] >= RAMPS_MAX)
            continue;
        r = &g_rp[slot].r[p[0]];
        r->used = 1;
        r->shape = p[1] / 16;
        r->color = p[1] % 16;
        if (r->shape >= RAMP_SHAPES || r->color >= RAMP_COLORS) {
            r->used = 0;
            continue;
        }
        memcpy(r->pos, p + 2, 12);
        memcpy(&r->yaw, p + 14, 4);
        if (rs == 26) {
            memcpy(&r->sx, p + 18, 4);
            memcpy(&r->sy, p + 22, 4);
        }
        if (!isfinite(r->sx) || !isfinite(r->sy) || fabsf(r->sx) > 0.5f || fabsf(r->sy) > 0.5f)
            r->sx = r->sy = 0.0f;
        if (!isfinite(r->pos[0]) || !isfinite(r->pos[1]) || !isfinite(r->pos[2]) || !isfinite(r->yaw))
            r->used = 0;
    }
    g_rbusy = 0;
}

void ramps_net_clock(double now)
{
    g_rnow = now;
}

int ramps_net_remote_count(void)
{
    int i, j, n = 0;
    for (i = 0; i < RPEERS; ++i)
        if (g_rp[i].id && g_rnow - g_rp[i].seen <= 5.0)
            for (j = 0; j < RAMPS_MAX; ++j)
                n += g_rp[i].r[j].used;
    return n;
}

/* v1.8.146 : rampes en maillage de triangles */
static const Ramp *nth_ramp(int k)
{
    int i, j;
    for (i = 0; i < RAMPS_MAX; ++i)
        if (g_r[i].used && k-- == 0)
            return &g_r[i];
    if (g_rbusy)
        return NULL;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 5.0)
            continue;
        for (j = 0; j < RAMPS_MAX; ++j)
            if (g_rp[i].r[j].used && k-- == 0)
                return &g_rp[i].r[j];
    }
    return NULL;
}

int ramps_mesh_count(void)
{
    int n = 0;
    while (nth_ramp(n))
        ++n;
    return n;
}

static void tri_out(float *v, int *ix, int *nt, int a, int b, int c, const float out[3])
{
    float u[3], w[3], n[3];
    int k;
    if (*nt >= RAMP_MESH_T)
        return;
    for (k = 0; k < 3; ++k) {
        u[k] = v[b * 3 + k] - v[a * 3 + k];
        w[k] = v[c * 3 + k] - v[a * 3 + k];
    }
    n[0] = u[1] * w[2] - u[2] * w[1];
    n[1] = u[2] * w[0] - u[0] * w[2];
    n[2] = u[0] * w[1] - u[1] * w[0];
    if (n[0] * n[0] + n[1] * n[1] + n[2] * n[2] < 1e-10f)
        return; /* (triangle plat : rien) */
    if (n[0] * out[0] + n[1] * out[1] + n[2] * out[2] < 0.0f) {
        k = b;
        b = c;
        c = k;
    }
    ix[*nt * 3 + 0] = a;
    ix[*nt * 3 + 1] = b;
    ix[*nt * 3 + 2] = c;
    ++*nt;
}

static void quad_out(float *v, int *ix, int *nt, int a, int b, int c, int d, const float out[3])
{
    tri_out(v, ix, nt, a, b, c, out);
    tri_out(v, ix, nt, a, c, d, out);
}

static int g_mesh_top = 0;
int ramps_mesh_top(int k, float yoff, float *v, int *ix, int *nt)
{
    int n;
    g_mesh_top = 1;
    n = ramps_mesh(k, yoff, v, ix, nt);
    g_mesh_top = 0;
    return n;
}

int ramps_mesh_walls(int k, float yoff, float *v, int *ix, int *nt)
{
    int n;
    g_mesh_top = 2;
    n = ramps_mesh(k, yoff, v, ix, nt);
    g_mesh_top = 0;
    return n;
}

int ramps_mesh(int k, float yoff, float *v, int *ix, int *nt)
{
    const Ramp *r = nth_ramp(k);
    float ex[3], ey[3], W, L, up[3] = { 0.0f, 1.0f, 0.0f }, mex[3], mey[3], dn[3] = { 0.0f, -1.0f, 0.0f };
    int rows = 0, j, q, nv = 0;
    *nt = 0;
    if (!r || r->shape < 0 || r->shape >= RAMP_SHAPES)
        return 0;
    axes(r, ex, ey);
    W = k_ramp_dim[r->shape][0];
    L = k_ramp_dim[r->shape][1];
    for (q = 0; q < 3; ++q) {
        mex[q] = -ex[q];
        mey[q] = -ey[q];
    }
    /* rangées le long de la rampe (une sur 3 points du profil) : 4 sommets (dessus gauche, droite, dessous gauche, droite) */
    for (j = 0; j <= RAMP_PROF; j += 3) {
        float ly = L * (float)j / RAMP_PROF, h = k_ramp_prof[r->shape][j];
        int s;
        if (nv + 4 > RAMP_MESH_V)
            break;
        for (s = 0; s < 4; ++s) {
            float lx = (s & 1) ? W : -W;
            float g = r->pos[1] + r->sx * lx + r->sy * ly - yoff;
            float *p = &v[nv * 3];
            p[0] = r->pos[0] + ex[0] * lx + ey[0] * ly;
            p[1] = s < 2 ? g + h : g - 0.6f;
            p[2] = r->pos[2] + ex[2] * lx + ey[2] * ly;
            ++nv;
        }
        ++rows;
        if (j < RAMP_PROF && j + 3 > RAMP_PROF)
            j = RAMP_PROF - 3; /* (dernière rangée : le bout du profil) */
    }
    for (j = 0; j + 1 < rows; ++j) {
        int a = j * 4, b = (j + 1) * 4;
        if (g_mesh_top != 2)
            quad_out(v, ix, nt, a + 0, a + 1, b + 1, b + 0, up);     /* dessus */
        if (g_mesh_top == 1)
            continue;
        quad_out(v, ix, nt, a + 0, a + 2, b + 2, b + 0, mex); /* côté x- */
        quad_out(v, ix, nt, a + 1, a + 3, b + 3, b + 1, ex);     /* côté x+ */
        if (g_mesh_top != 2)
            quad_out(v, ix, nt, a + 2, a + 3, b + 3, b + 2, dn);     /* dessous */
    }
    if (rows >= 2 && g_mesh_top != 1) {
        int b = (rows - 1) * 4;
        if (g_mesh_top != 2)
            quad_out(v, ix, nt, 0, 1, 3, 2, mey);     /* avant (pas en mur : on y entre) */
        quad_out(v, ix, nt, b + 0, b + 1, b + 3, b + 2, ey); /* arrière */
    }
    return *nt ? nv : 0;
}

/* v1.8.152 : sphère (p, r) dans une rampe (la nôtre ou celle d'un autre) : déplacement pour l'en sortir
   (par le dessus, un côté ou le dos, le plus court) ; 1 si touchée */
int ramps_push(const float p[3], float r, float push[3])
{
    int k, hit = 0;
    const Ramp *rp;
    push[0] = push[1] = push[2] = 0.0f;
    for (k = 0; (rp = nth_ramp(k)) != NULL; ++k) {
        float ex[3], ey[3], W, L, dx, dz, lx, ly, top, base, up, side, back;
        if (rp->shape < 0 || rp->shape >= RAMP_SHAPES)
            continue;
        axes(rp, ex, ey);
        W = k_ramp_dim[rp->shape][0];
        L = k_ramp_dim[rp->shape][1];
        dx = p[0] - rp->pos[0];
        dz = p[2] - rp->pos[2];
        lx = dx * ex[0] + dz * ex[2];
        ly = dx * ey[0] + dz * ey[2];
        if (fabsf(lx) > W + r || ly < 0.0f || ly > L + r)
            continue;
        {
            float cl = ly > L ? L : ly, clx = lx > W ? W : lx < -W ? -W : lx;
            top = rp->pos[1] + prof(rp->shape, cl) + rp->sx * clx + rp->sy * cl;
            base = rp->pos[1] + rp->sx * clx + rp->sy * cl - 0.6f;
        }
        if (p[1] - r > top || p[1] < base || top - (rp->pos[1] + rp->sx * lx + rp->sy * ly) < 0.15f)
            continue; /* (au-dessus, dessous, ou rampe presque à plat ici : rien) */
        up = top - p[1] + r;
        side = W + r - fabsf(lx);
        back = L + r - ly;
        if (up <= side && up <= back) {
            push[1] += up;
        } else if (side <= back) {
            float sg = lx >= 0.0f ? 1.0f : -1.0f;
            push[0] += ex[0] * side * sg;
            push[2] += ex[2] * side * sg;
        } else {
            push[0] += ey[0] * back;
            push[2] += ey[2] * back;
        }
        hit = 1;
    }
    return hit;
}

/* v1.8.155 : notre moto contre les côtés et le dos des rampes (contrôlé par le plugin, comme la botte ronde) :
   point p (rayon r) sous le tablier d'une rampe : entrée (m) et sortie la plus courte (out, normale n, à plat) */
float ramps_wall_block(const float p[3], float r, float out[2], float n[2])
{
    int k;
    float best = 0.0f;
    const Ramp *rp;
    out[0] = out[1] = n[0] = n[1] = 0.0f;
    for (k = 0; (rp = nth_ramp(k)) != NULL; ++k) {
        float ex[3], ey[3], W, L, dx, dz, lx, ly, top, side, back, cl, clx;
        if (rp->shape < 0 || rp->shape >= RAMP_SHAPES)
            continue;
        axes(rp, ex, ey);
        W = k_ramp_dim[rp->shape][0];
        L = k_ramp_dim[rp->shape][1];
        dx = p[0] - rp->pos[0];
        dz = p[2] - rp->pos[2];
        lx = dx * ex[0] + dz * ex[2];
        ly = dx * ey[0] + dz * ey[2];
        if (fabsf(lx) >= W + r || ly <= 0.0f || ly >= L + r)
            continue;
        cl = ly > L ? L : ly;
        clx = lx > W ? W : lx < -W ? -W : lx;
        top = rp->pos[1] + prof(rp->shape, cl) + rp->sx * clx + rp->sy * cl;
        /* v1.8.156 : seulement si le tablier passe nettement au-dessus du point (moto dessous) : en montant
           la rampe, le centre de la moto est au-dessus du tablier (1.8.155 : 0.25 m, bloquait la montée) */
        if (p[1] + 0.8f > top || p[1] < rp->pos[1] - 1.0f)
            continue;
        side = W + r - fabsf(lx);
        back = L + r - ly;
        if (side <= back) {
            float sg = lx >= 0.0f ? 1.0f : -1.0f;
            if (side > best) {
                best = side;
                n[0] = ex[0] * sg;
                n[1] = ex[2] * sg;
            }
        } else if (back > best) {
            best = back;
            n[0] = ey[0];
            n[1] = ey[2];
        }
    }
    out[0] = n[0] * best;
    out[1] = n[1] * best;
    return best;
}

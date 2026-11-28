/*
 * cones.c - v1.8.45 : cônes à poser sur la piste (voir cones.h)
 */
#include "cones.h"
#include "objinst.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* dimensions d'un cône de chantier de 70 cm */
/* v1.8.49 : mesures du modèle 3D donné par le joueur (cone_model.inc) */
#include "cone_model.inc"
/* v1.8.50 : un peu plus petit (le joueur le trouvait trop gros) : 56 cm de haut */
#define CONE_S      0.80f
#define CONE_H      (0.70f * CONE_S)             /* hauteur totale */
#define CONE_PLATE  (CONE_MODEL_PLATE * CONE_S)  /* épaisseur du socle */
#define CONE_HALF   (CONE_MODEL_HALF * CONE_S)   /* demi-côté du socle carré */
#define CONE_RB     (0.196f * CONE_S)            /* rayon en bas du corps */
#define CONE_RT     (0.015f * CONE_S)            /* rayon en haut */
/* v1.8.51 : au-dessus du sol physique : l'herbe 3D du jeu cachait tout le socle (vu sur clip) ;
   réglable : cones_hauteur dans MXRP_Ragdoll.ini */
static float g_lift = 0.045f;
#define CONE_LIFT g_lift
void cones_set_lift(float m)
{
    g_lift = m < -0.2f ? -0.2f : (m > 0.5f ? 0.5f : m);
}

/* v1.8.57 : chaque cône est un petit corps rigide : 4 coins du socle et la pointe (points reliés
   par des longueurs fixes), avec la gravité, le sol, le frottement : il bascule, roule, glisse,
   et se laisse encore pousser une fois tombé */
/* v1.8.61 : 8 points en rond autour du socle (au lieu des 4 coins) : couché, il roule sur ce rond
   au lieu de glisser (le joueur : « il a pas roulé il a glissé ») ; le dernier point est la pointe */
#define NR 8
#define TIP NR
#define NP (NR + 1)
typedef struct {
    int used;
    float pos[3];      /* centre du dessous du socle (calculé) */
    int down;          /* renversé (calculé) */
    float p[NP][3];    /* points, monde */
    float pp[NP][3];   /* points à l'image d'avant (vitesse) */
    float gh[NP];      /* dernière hauteur du sol connue sous chaque point */
    float spawn[4];    /* x, y, z, cap : là où il a été posé */
    float still;       /* temps passé immobile (endormi au-delà) */
    int asleep;
    float gq[NP][2];    /* v1.8.104 : où le sol a été lu pour chaque point (gh[] valable là) */
    float idle, ref[3]; /* v1.8.103 : temps sans vrai déplacement, position de référence */
    float cool;        /* attente avant une autre poussée de la jambe */
    int held;          /* v1.8.62 : tenu par le pilote (pas de physique) */
    int var;           /* v1.8.52 : variante (texture) */
} Cone;

static void v_sub(float o[3], const float a[3], const float b[3]);
static float v_dot(const float a[3], const float b[3]);
static void v_cross(float o[3], const float a[3], const float b[3]);
static int v_norm(float a[3]);

static Cone g_c[CONES_MAX];
/* v1.8.52 : ce qui est dessiné : les cônes du moment, ou ceux de la rediffusion */
static Cone g_disp[CONES_MAX];
static const Cone *g_src = g_c;
static int g_var = 0;

int cones_variants(void)
{
    return CONE_VARIANTS;
}

void cones_set_variant(int v)
{
    if (v >= 0 && v < CONE_VARIANTS)
        g_var = v;
}

int cones_variant(void)
{
    return g_var;
}

unsigned long cones_variant_rgb(int v)
{
    return v >= 0 && v < CONE_VARIANTS ? k_cone_var_rgb[v] : 0x2A5CF0UL;
}
static ConesLogFn g_log = 0;

/* masses : socle lourd (caoutchouc), pointe légère */
static const float k_invm[NP] = { 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 1.2f };

static void local_pt(int j, float out[3]) /* x (e1), y (e2), h (axe) */
{
    if (j < NR) {
        /* rond un peu plus large que le socle carré (ses coins sont coupés) */
        float ang = 6.2831853f * (float)j / NR;
        out[0] = cosf(ang) * CONE_HALF * 1.08f;
        out[1] = sinf(ang) * CONE_HALF * 1.08f;
        out[2] = 0.0f;
    } else {
        out[0] = out[1] = 0.0f;
        out[2] = CONE_H;
    }
}

static float rest_len(int i, int j)
{
    float a[3], b[3], d[3];
    local_pt(i, a);
    local_pt(j, b);
    v_sub(d, a, b);
    return sqrtf(v_dot(d, d));
}

/* repère du cône à partir de ses points : centre du socle, axe, e1, e2 */
static void frame_of(const Cone *c, float base[3], float a[3], float e1[3], float e2[3])
{
    float d1[3], k;
    int i;
    for (i = 0; i < 3; ++i) {
        int j;
        base[i] = 0.0f;
        for (j = 0; j < NR; ++j)
            base[i] += c->p[j][i];
        base[i] /= NR;
        a[i] = c->p[TIP][i] - base[i];
        d1[i] = c->p[0][i] - c->p[NR / 2][i]; /* point 0 : le long de e1 */
    }
    if (!v_norm(a)) {
        a[0] = a[2] = 0.0f;
        a[1] = 1.0f;
    }
    k = v_dot(d1, a);
    for (i = 0; i < 3; ++i)
        e1[i] = d1[i] - a[i] * k;
    if (!v_norm(e1)) {
        e1[0] = 1.0f;
        e1[1] = e1[2] = 0.0f;
    }
    v_cross(e2, a, e1);
}

static void refresh(Cone *c)
{
    float base[3], a[3], e1[3], e2[3];
    frame_of(c, base, a, e1, e2);
    memcpy(c->pos, base, sizeof(base));
    c->down = a[1] < 0.6f;
}

static ConesGroundFn g_ground = 0; /* (dernière fonction du sol donnée à cones_update) */

/* debout, centre du socle en (x, y, z), cap yaw. v1.8.58 : posé sur le sol réel sous ses coins
   (sinon un coin dans le sol le relançait : il faisait un salto et retombait sur le côté) */
static void place_upright_ex(Cone *c, float x, float y, float z, float yaw, int snap)
{
    float e1[3] = { cosf(yaw), 0.0f, -sinf(yaw) }, up[3] = { 0.0f, 1.0f, 0.0f }, e2[3], l[3];
    int j, k;
    v_cross(e2, up, e1);
    if (g_ground && snap) {
        float h, top = -1e9f;
        for (j = 0; j < NR; ++j) {
            local_pt(j, l);
            if (g_ground(x + e1[0] * l[0] + e2[0] * l[1], z + e1[2] * l[0] + e2[2] * l[1], &h) && isfinite(h) &&
                h > top)
                top = h;
        }
        if (g_ground(x, z, &h) && isfinite(h) && h > top)
            top = h;
        /* snap 1 : posé sur le sol ; 2 (tenu) : jamais sous le sol (v1.8.64 : en pente, un côté du
           socle était dans la pente et le cône était relancé en le lâchant) */
        if (top > -1e8f && (snap == 1 || y < top + CONE_LIFT + 0.005f))
            y = top + CONE_LIFT + 0.005f;
    }
    for (j = 0; j < NP; ++j) {
        local_pt(j, l);
        for (k = 0; k < 3; ++k)
            c->p[j][k] = (k == 1 ? y : (k == 0 ? x : z)) + e1[k] * l[0] + e2[k] * l[1] + up[k] * l[2];
        memcpy(c->pp[j], c->p[j], sizeof(c->p[j]));
        c->gh[j] = y;
        c->gq[j][0] = c->gq[j][1] = 1e9f; /* sol à relire */
    }
    c->asleep = 0;
    c->idle = 0.0f;
    c->still = 0.0f;
    refresh(c);
}

static float cone_yaw(const Cone *c);

static void place_upright(Cone *c, float x, float y, float z, float yaw)
{
    place_upright_ex(c, x, y, z, yaw, 1);
}

/* v1.8.62 : tenu par le pilote (animation de pose) : centre du socle en (x, y, z), sans physique ;
   cones_hold_end : posé, il reprend la physique */
void cones_hold(int i, float x, float y, float z, float yaw)
{
    Cone *c;
    if (i < 0 || i >= CONES_MAX || !g_c[i].used)
        return;
    c = &g_c[i];
    c->held = 1;
    place_upright_ex(c, x, y, z, yaw, 2);
}

void cones_set_spawn(int i, float x, float y, float z, float yaw)
{
    if (i < 0 || i >= CONES_MAX || !g_c[i].used)
        return;
    g_c[i].spawn[0] = x;
    g_c[i].spawn[1] = y;
    g_c[i].spawn[2] = z;
    g_c[i].spawn[3] = yaw;
}

void cones_remove(int i)
{
    if (i >= 0 && i < CONES_MAX)
        g_c[i].used = 0;
}

void cones_hold_end(int i)
{
    if (i < 0 || i >= CONES_MAX || !g_c[i].used)
        return;
    if (g_c[i].held) {
        /* lâché : posé sur le sol réel sous son socle (en pente, il se couche sur la pente) */
        Cone *c = &g_c[i];
        float base[3], a[3], e1[3], e2[3];
        frame_of(c, base, a, e1, e2);
        place_upright_ex(c, base[0], base[1], base[2], cone_yaw(c), 2);
    }
    g_c[i].held = 0;
    g_c[i].asleep = 0;
    g_c[i].idle = 0.0f;
    g_c[i].still = 0.0f;
}

static float cone_yaw(const Cone *c)
{
    float base[3], a[3], e1[3], e2[3];
    frame_of(c, base, a, e1, e2);
    return atan2f(-e1[2], e1[0]);
}

void cones_init(ConesLogFn log)
{
    g_log = log;
    cones_clear();
}

void cones_clear(void)
{
    memset(g_c, 0, sizeof(g_c));
}

int cones_count(void)
{
    int i, n = 0;
    for (i = 0; i < CONES_MAX; ++i)
        n += g_c[i].used;
    return n;
}

int cones_down_count(void)
{
    int i, n = 0;
    for (i = 0; i < CONES_MAX; ++i)
        n += g_c[i].used && g_c[i].down;
    return n;
}

int cones_moved_count(void)
{
    int i, n = 0;
    for (i = 0; i < CONES_MAX; ++i) {
        const Cone *c = &g_c[i];
        float dx, dz;
        if (!c->used)
            continue;
        dx = c->pos[0] - c->spawn[0];
        dz = c->pos[2] - c->spawn[2];
        n += c->down || dx * dx + dz * dz > 0.05f * 0.05f;
    }
    return n;
}

int cones_add(float x, float y, float z)
{
    int i;
    for (i = 0; i < CONES_MAX; ++i) {
        if (!g_c[i].used) {
            Cone *c = &g_c[i];
            memset(c, 0, sizeof(*c));
            c->used = 1;
            c->var = g_var;
            c->spawn[0] = x;
            c->spawn[1] = y;
            c->spawn[2] = z;
            c->spawn[3] = 0.7f * (float)i;
            place_upright(c, x, y + CONE_LIFT, z, c->spawn[3]);
            if (g_log)
                g_log("cones : pose n %d en (%.2f, %.2f, %.2f), variante %d, %d cones", i, x, y, z, g_var + 1,
                      cones_count());
            return i;
        }
    }
    return -1;
}

int cones_remove_nearest(float x, float z, float maxd)
{
    int i, best = -1;
    float bd = maxd * maxd;
    for (i = 0; i < CONES_MAX; ++i) {
        float dx, dz, d;
        if (!g_c[i].used)
            continue;
        dx = g_c[i].pos[0] - x;
        dz = g_c[i].pos[2] - z;
        d = dx * dx + dz * dz;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (best < 0)
        return 0;
    g_c[best].used = 0;
    return 1;
}

/* relevés là où ils sont (même cap) */
void cones_reset_upright(void)
{
    int i;
    for (i = 0; i < CONES_MAX; ++i) {
        Cone *c = &g_c[i];
        float y;
        if (!c->used)
            continue;
        y = c->pos[1];
        if (c->down) {
            /* couché : le socle est de travers ; le sol est sous le point le plus bas */
            int j;
            y = 1e9f;
            for (j = 0; j < NP; ++j)
                if (c->gh[j] < y)
                    y = c->gh[j];
            y += CONE_LIFT;
        }
        place_upright(c, c->pos[0], y, c->pos[2], cone_yaw(c));
    }
}

/* v1.8.57 : remis là où ils ont été posés */
void cones_reset_spawn(void)
{
    int i;
    for (i = 0; i < CONES_MAX; ++i) {
        Cone *c = &g_c[i];
        if (c->used)
            place_upright(c, c->spawn[0], c->spawn[1] + CONE_LIFT, c->spawn[2], c->spawn[3]);
    }
}

/* change la vitesse des points : socle (w_base) et pointe (w_tip) ; pointe plus vite : il bascule
   vers l'avant, plus lente : vers l'arrière */
static void cone_push_vel(Cone *c, float w_base, float w_tip, const float dv[3])
{
    int j, k;
    for (j = 0; j < NP; ++j) {
        float w = j == TIP ? w_tip : w_base;
        /* Verlet : la vitesse est (p - pp) / dt ; on la change par pp (dt d'un pas : 1/120 s) */
        for (k = 0; k < 3; ++k)
            c->pp[j][k] -= dv[k] * w * (1.0f / 120.0f);
    }
    c->asleep = 0;
    c->idle = 0.0f;
    c->still = 0.0f;
}

/* v1.8.60 : bascule : rotation autour du bord du socle le plus loin dans le sens (nx, nz),
   omega rad/s, plus une vitesse vlin dans ce sens */
static void cone_tip_over(Cone *c, float nx, float nz, float omega, float vlin)
{
    float base[3], a[3], e1[3], e2[3], pivot[3], ax[3], up[3] = { 0.0f, 1.0f, 0.0f }, n[3] = { nx, 0.0f, nz };
    int j, k;
    frame_of(c, base, a, e1, e2);
    for (k = 0; k < 3; ++k)
        pivot[k] = base[k] + n[k] * CONE_HALF;
    v_cross(ax, up, n);
    for (j = 0; j < NP; ++j) {
        float r[3], v[3];
        v_sub(r, c->p[j], pivot);
        v_cross(v, ax, r);
        for (k = 0; k < 3; ++k)
            c->pp[j][k] = c->p[j][k] - (v[k] * omega + n[k] * vlin) / 120.0f;
    }
    c->asleep = 0;
    c->idle = 0.0f;
    c->still = 0.0f;
}

static int knock(int i, const float vel[3]);
void cones_knock(int i, const float vel[3])
{
    knock(i, vel);
}

/* rend 1 si le cône a été lancé (nouveau choc) */
static int knock(int i, const float vel[3])
{
    Cone *c;
    float sp, m, dv[3];
    if (i < 0 || i >= CONES_MAX || !g_c[i].used || g_c[i].held)
        return 0;
    c = &g_c[i];
    sp = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
    if (sp < 0.1f)
        return 0;
    /* v1.8.59 : plus lourd (le joueur le trouvait trop léger, il s'envolait) : il part à 70 % de la
       vitesse de ce qui le frappe (9 m/s au plus) et décolle à peine */
    /* v1.8.89 : encore plus lourd (« bien trop léger ») : 35 % de la vitesse, 4,5 m/s au plus */
    /* v1.8.90 : encore plus lourd face à la moto : 20 % de sa vitesse, 3 m/s au plus */
    m = sp * 0.2f > 3.0f ? 3.0f / sp : 0.2f;
    {
        /* déjà lancé dans ce sens assez vite : rien de plus (appelé à chaque image du contact) */
        float cv[3] = { 0.0f, 0.0f, 0.0f };
        int j, k;
        for (k = 0; k < 3; ++k)
            for (j = 0; j < NP; ++j)
                cv[k] += (c->p[j][k] - c->pp[j][k]) * 120.0f / NP;
        if ((cv[0] * vel[0] + cv[2] * vel[2]) / sp > sp * m * 0.8f)
            return 0;
        /* projeté devant ce qui le frappe, un peu vers le haut ; pointe plus vite : il bascule */
        dv[0] = vel[0] * m - cv[0];
        dv[1] = 0.10f + 0.02f * sp * m;
        dv[2] = vel[2] * m - cv[2];
    }
    cone_push_vel(c, 0.6f, 1.8f, dv);
    return 1;
}

int cones_get(int i, float pos[3], int *down)
{
    if (i < 0 || i >= CONES_MAX || !g_c[i].used)
        return 0;
    memcpy(pos, g_c[i].pos, sizeof(float) * 3);
    if (down)
        *down = g_c[i].down;
    return 1;
}

/* un pas de simulation */
static void cone_step(Cone *c, float dt, ConesGroundFn ground)
{
    static const float G = 9.81f, MU = 0.7f;
    float v[NP][3], vmax = 0.0f;
    int j, k, it, contact[NP];
    for (j = 0; j < NP; ++j) {
        for (k = 0; k < 3; ++k) {
            v[j][k] = (c->p[j][k] - c->pp[j][k]) * 0.999f;
            c->pp[j][k] = c->p[j][k];
            c->p[j][k] += v[j][k];
        }
        c->p[j][1] -= G * dt * dt;
        contact[j] = 0;
    }
    for (it = 0; it < 6; ++it) {
        int a, b;
        /* longueurs fixes (corps rigide) */
        for (a = 0; a < NP; ++a)
            for (b = a + 1; b < NP; ++b) {
                float d[3], l, r = rest_len(a, b), wa = k_invm[a], wb = k_invm[b], f;
                v_sub(d, c->p[b], c->p[a]);
                l = sqrtf(v_dot(d, d));
                if (l < 1e-6f)
                    continue;
                f = (l - r) / (l * (wa + wb));
                for (k = 0; k < 3; ++k) {
                    c->p[a][k] += d[k] * f * wa;
                    c->p[b][k] -= d[k] * f * wb;
                }
            }
        /* sol */
        for (j = 0; j < NP; ++j) {
            float h;
            /* v1.8.104 : le sol n'est relu que si le point a bougé de plus de 2 cm (c'était
               l'essentiel du temps d'un cône qui roule : relief, déformation, objets, bottes) */
            if (ground && (fabsf(c->p[j][0] - c->gq[j][0]) > 0.02f || fabsf(c->p[j][2] - c->gq[j][1]) > 0.02f) &&
                ground(c->p[j][0], c->p[j][2], &h) && isfinite(h)) {
                c->gh[j] = h;
                c->gq[j][0] = c->p[j][0];
                c->gq[j][1] = c->p[j][2];
            }
            h = c->gh[j] + CONE_LIFT;
            if (c->p[j][1] < h) {
                c->p[j][1] = h;
                contact[j] = 1;
            }
        }
    }
    /* au sol : rebond faible, frottement (Coulomb) ; v1.8.60 : vitesse limitée (pas d'envol) */
    for (j = 0; j < NP; ++j) {
        float vel[3], vh, dvh, vs;
        for (k = 0; k < 3; ++k)
            vel[k] = (c->p[j][k] - c->pp[j][k]) / dt;
        if (vel[1] > 3.0f)
            vel[1] = 3.0f;
        vs = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
        if (vs > 12.0f)
            for (k = 0; k < 3; ++k)
                vel[k] *= 12.0f / vs;
        for (k = 0; k < 3; ++k)
            c->pp[j][k] = c->p[j][k] - vel[k] * dt;
        if (contact[j]) {
            if (vel[1] < 0.0f)
                vel[1] = -vel[1] * 0.12f;
            else if (vel[1] > 0.4f)
                vel[1] = 0.4f; /* sorti du sol : pas lancé en l'air */
            vh = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
            dvh = MU * G * dt * 1.8f;
            if (vh <= dvh) {
                vel[0] = vel[2] = 0.0f;
            } else {
                vel[0] *= (vh - dvh) / vh;
                vel[2] *= (vh - dvh) / vh;
            }
            for (k = 0; k < 3; ++k)
                c->pp[j][k] = c->p[j][k] - vel[k] * dt;
        }
        {
            float sp = sqrtf(v_dot(vel, vel));
            if (sp > vmax)
                vmax = sp;
        }
    }
    /* immobile un moment : endormi (stable, et rien à calculer) */
    if (vmax < 0.06f) {
        c->still += dt;
        if (c->still > 0.6f) {
            c->asleep = 1;
            for (j = 0; j < NP; ++j)
                memcpy(c->pp[j], c->p[j], sizeof(c->p[j]));
        }
    } else {
        c->still = 0.0f;
    }
}

void cones_update(float dt, ConesGroundFn ground)
{
    int i, n, s;
    if (dt <= 0.0f)
        return;
    if (dt > 0.1f)
        dt = 0.1f;
    g_ground = ground;
    n = (int)ceilf(dt * 120.0f);
    if (n < 1)
        n = 1;
    for (i = 0; i < CONES_MAX; ++i) {
        Cone *c = &g_c[i];
        if (!c->used)
            continue;
        if (c->cool > 0.0f)
            c->cool -= dt;
        if (!c->asleep && !c->held) {
            for (s = 0; s < n; ++s)
                cone_step(c, dt / (float)n, ground);
            refresh(c);
            /* v1.8.103 : cônes posés les uns dans les autres : ils tremblaient sans fin et ne
               s'endormaient jamais (2 ms par appel à pied). Moins de 2 cm en 1,5 s : endormi. */
            {
                float dx = c->pos[0] - c->ref[0], dy = c->pos[1] - c->ref[1], dz = c->pos[2] - c->ref[2];
                if (dx * dx + dy * dy + dz * dz > 0.02f * 0.02f) {
                    c->idle = 0.0f;
                    memcpy(c->ref, c->pos, sizeof(c->ref));
                } else if ((c->idle += dt) > 1.5f) {
                    int j;
                    c->asleep = 1;
                    c->still = 1.0f;
                    for (j = 0; j < NP; ++j)
                        memcpy(c->pp[j], c->p[j], sizeof(c->p[j]));
                }
            }
        } else {
            refresh(c);
        }
    }
}

int cones_bike_pass(const float pos[3], const float vel[3], float r)
{
    int i, n = 0;
    float sp = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
    if (sp < 1.0f)
        return 0;
    for (i = 0; i < CONES_MAX; ++i) {
        Cone *c = &g_c[i];
        float dx, dz;
        if (!c->used || c->held || fabsf(c->pos[1] - pos[1]) > 1.3f)
            continue;
        dx = c->pos[0] - pos[0];
        dz = c->pos[2] - pos[2];
        if (dx * dx + dz * dz > r * r)
            continue;
        {
            /* projeté devant la moto, un peu sur le côté où il était */
            float d = sqrtf(dx * dx + dz * dz), ux = vel[0] / sp, uz = vel[2] / sp, side, v[3];
            side = d > 0.01f ? (dx * uz - dz * ux) / d : 0.0f;
            v[0] = vel[0] + uz * side * 0.2f * sp;
            v[1] = 0.0f;
            v[2] = vel[2] - ux * side * 0.2f * sp;
            n += knock(i, v); /* v1.8.90 : nouveaux chocs seulement (la moto est freinée) */
        }
    }
    return n;
}

/* pilote debout (cylindre de rayon r, des pieds à la tête) : il pousse les points des cônes,
   debout ou tombés, à sa vitesse ; en courant, il les envoie plus loin */
int cones_push_walker(float x, float z, float feet, float r, float vx, float vz, float out[2])
{
    int i, j, hit = 0;
    out[0] = out[1] = 0.0f;
    for (i = 0; i < CONES_MAX; ++i) {
        Cone *c = &g_c[i];
        float nx, nz, l, shift = 0.0f, vn;
        if (!c->used || c->held)
            continue;
        if (c->pos[1] > feet + 1.5f || c->pos[1] < feet - 1.0f)
            continue;
        /* v1.8.60 : le cône entier est poussé dans un seul sens (du pilote vers le cône). Avant,
           chaque point était sorti de son côté : le cône se coinçait entre les jambes, était porté,
           puis s'envolait (vu sur clip) */
        nx = c->pos[0] - x;
        nz = c->pos[2] - z;
        l = sqrtf(nx * nx + nz * nz);
        if (l > 1.5f)
            continue;
        if (l > 1e-4f) {
            nx /= l;
            nz /= l;
        } else if (vx * vx + vz * vz > 1e-4f) {
            l = sqrtf(vx * vx + vz * vz);
            nx = vx / l;
            nz = vz / l;
        } else {
            nx = 0.0f;
            nz = 1.0f;
        }
        for (j = 0; j < NP; ++j) {
            /* debout : la jambe touche le corps du cône (la pointe, avec le rayon du corps à mi-hauteur) ;
               le socle n'est poussé que si on marche vraiment dessus */
            float R = c->down ? r + 0.03f : (j == TIP ? r + CONE_RB * 0.6f : r * 0.55f);
            float dx = c->p[j][0] - x, dz = c->p[j][2] - z, along = dx * nx + dz * nz, side = -dx * nz + dz * nx;
            if (c->p[j][1] > feet + 1.7f || c->p[j][1] < feet - 0.15f || fabsf(side) >= R)
                continue;
            {
                float need = sqrtf(R * R - side * side) - along; /* jusqu'au bord du cylindre */
                if (need > shift)
                    shift = need;
            }
        }
        if (shift <= 0.0f)
            continue;
        if (shift > 0.3f)
            shift = 0.3f;
        vn = vx * nx + vz * nz;
        if (vn < 0.0f)
            vn = 0.0f;
        for (j = 0; j < NP; ++j) {
            /* déplacé avec sa vitesse d'avant (pp aussi), puis à la vitesse du pilote vers l'avant */
            float cur = ((c->p[j][0] - c->pp[j][0]) * nx + (c->p[j][2] - c->pp[j][2]) * nz) * 120.0f, add;
            c->p[j][0] += nx * shift;
            c->p[j][2] += nz * shift;
            c->pp[j][0] += nx * shift;
            c->pp[j][2] += nz * shift;
            add = vn - cur;
            if (add > 0.0f) {
                c->pp[j][0] -= nx * add / 120.0f;
                c->pp[j][2] -= nz * add / 120.0f;
            }
        }
        if (vn > 1.0f && c->cool <= 0.0f) {
            if (!c->down) {
                /* debout : il bascule autour du bord du socle (rotation franche), plus vite en courant */
                cone_tip_over(c, nx, nz, 7.5f + 1.2f * vn, vn > 3.0f ? vn * 0.55f : 0.0f);
            } else if (vn > 3.0f) {
                /* tombé, en courant : coup de pied */
                float dv[3] = { nx * vn * 0.6f, 0.5f, nz * vn * 0.6f };
                cone_push_vel(c, 1.0f, 1.0f, dv);
            }
            c->cool = 0.6f;
        }
        c->asleep = 0;
        c->idle = 0.0f;
        c->still = 0.0f;
        hit = 1;
    }
    return hit;
}

int cones_body_touch(const float p[3], float r)
{
    int i, j, n = 0;
    for (i = 0; i < CONES_MAX; ++i) {
        Cone *c = &g_c[i];
        int touched = 0;
        if (!c->used || c->held)
            continue;
        if (fabsf(c->pos[1] - p[1]) > 1.2f || fabsf(c->pos[0] - p[0]) > 1.2f || fabsf(c->pos[2] - p[2]) > 1.2f)
            continue;
        for (j = 0; j < NP; ++j) {
            float d[3], l, R = r + 0.04f;
            int k;
            v_sub(d, c->p[j], p);
            l = sqrtf(v_dot(d, d));
            if (l >= R)
                continue;
            if (l < 1e-4f) {
                d[0] = d[2] = 0.0f;
                d[1] = 1.0f;
                l = 1.0f;
            }
            for (k = 0; k < 3; ++k)
                c->p[j][k] = p[k] + d[k] / l * R;
            touched = 1;
        }
        if (touched) {
            c->asleep = 0;
            c->idle = 0.0f;
            c->still = 0.0f;
            ++n;
        }
    }
    return n;
}

/* ---------- dessin ---------- */

static void v_sub(float o[3], const float a[3], const float b[3])
{
    o[0] = a[0] - b[0];
    o[1] = a[1] - b[1];
    o[2] = a[2] - b[2];
}

static float v_dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void v_cross(float o[3], const float a[3], const float b[3])
{
    float t[3];
    t[0] = a[1] * b[2] - a[2] * b[1];
    t[1] = a[2] * b[0] - a[0] * b[2];
    t[2] = a[0] * b[1] - a[1] * b[0];
    memcpy(o, t, sizeof(t));
}

static int v_norm(float a[3])
{
    float l = sqrtf(v_dot(a, a));
    if (l < 1e-6f)
        return 0;
    a[0] /= l;
    a[1] /= l;
    a[2] /= l;
    return 1;
}

/* couleur 0xAABBGGRR éclaircie / assombrie */
static unsigned long shade(unsigned long c, float k)
{
    unsigned long r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    float fr = (float)r * k, fg = (float)g * k, fb = (float)b * k;
    r = fr > 255.0f ? 255 : (unsigned long)fr;
    g = fg > 255.0f ? 255 : (unsigned long)fg;
    b = fb > 255.0f ? 255 : (unsigned long)fb;
    return (c & 0xFF000000UL) | (b << 16) | (g << 8) | r;
}

/* axe du cône (incliné s'il est renversé) et base du socle */
static void cone_frame(const Cone *c, float a[3], float base[3], float e1[3], float e2[3])
{
    frame_of(c, base, a, e1, e2);
}

typedef struct {
    int put;
    int max;
    ConesProjectFn proj;
    ConesQuadFn fn;
} Out;

/* projette 4 points ; écrit le quadrilatère s'ils sont tous devant la caméra */
static void out_quad(Out *o, const float p[4][3], unsigned long col)
{
    float x[4], y[4];
    int k;
    if (o->put >= o->max)
        return;
    for (k = 0; k < 4; ++k)
        if (!o->proj(p[k], &x[k], &y[k]))
            return;
    o->fn(o->put++, x, y, col);
}

#define COL_ORANGE 0xFF1060FFUL
#define COL_WHITE  0xFFEEEEEEUL
#define COL_PLATE  0xFF151515UL

static int draw_one(Out *o, const Cone *c, const float eye[3], int segs)
{
    static const float k_band[6] = { CONE_PLATE, 0.30f, 0.41f, 0.50f, 0.58f, CONE_H };
    static const unsigned long k_col[5] = { COL_ORANGE, COL_WHITE, COL_ORANGE, COL_WHITE, COL_ORANGE };
    const float light[3] = { 0.40f, 0.82f, 0.41f };
    float a[3], base[3], e1[3], e2[3], mid[3], w[3], f[3], s[3];
    float ring[6][5][3]; /* hauteur, point (gauche .. droite) */
    int b, k, n = segs >= 4 ? 5 : 3, start = o->put, plate_last;
    cone_frame(c, a, base, e1, e2);
    for (k = 0; k < 3; ++k)
        mid[k] = base[k] + a[k] * 0.35f;
    v_sub(w, eye, mid);
    plate_last = v_dot(w, a) < -0.05f; /* caméra du côté du dessous : socle devant */
    /* f : vers la caméra, perpendiculaire à l'axe ; s : sur le côté */
    {
        float kk = v_dot(w, a);
        f[0] = w[0] - a[0] * kk;
        f[1] = w[1] - a[1] * kk;
        f[2] = w[2] - a[2] * kk;
        if (!v_norm(f))
            memcpy(f, e1, sizeof(f));
    }
    v_cross(s, a, f);
    for (b = 0; b < 6; ++b) {
        float h = k_band[b], r = CONE_RB + (CONE_RT - CONE_RB) * (h - CONE_PLATE) / (CONE_H - CONE_PLATE);
        for (k = 0; k < n; ++k) {
            /* angle de -90° (gauche) à +90° (droite), 0 = face à la caméra */
            float ang = -1.5708f + 3.14159f * (float)k / (float)(n - 1), cs = cosf(ang), sn = sinf(ang);
            int j;
            for (j = 0; j < 3; ++j)
                ring[b][k][j] = base[j] + a[j] * h + r * (f[j] * cs + s[j] * sn);
        }
    }
    if (!plate_last) {
        float q[4][3];
        int sg[4][2] = { { 1, 1 }, { 1, -1 }, { -1, -1 }, { -1, 1 } };
        for (k = 0; k < 4; ++k) {
            int j;
            for (j = 0; j < 3; ++j)
                q[k][j] = base[j] + a[j] * CONE_PLATE + e1[j] * CONE_HALF * sg[k][0] + e2[j] * CONE_HALF * sg[k][1];
        }
        out_quad(o, q, COL_PLATE);
    }
    for (b = 0; b < 5; ++b) {
        for (k = 0; k + 1 < n; ++k) {
            float q[4][3], nrm[3], lk;
            int j;
            memcpy(q[0], ring[b][k], sizeof(q[0]));
            memcpy(q[1], ring[b][k + 1], sizeof(q[1]));
            memcpy(q[2], ring[b + 1][k + 1], sizeof(q[2]));
            memcpy(q[3], ring[b + 1][k], sizeof(q[3]));
            /* éclairage : normale au milieu de la face */
            for (j = 0; j < 3; ++j)
                nrm[j] = (ring[b][k][j] + ring[b][k + 1][j]) * 0.5f - (base[j] + a[j] * k_band[b]);
            v_norm(nrm);
            lk = v_dot(nrm, light);
            lk = 0.62f + 0.45f * (lk > 0.0f ? lk : 0.0f);
            out_quad(o, q, shade(k_col[b], lk));
        }
    }
    if (plate_last) {
        float q[4][3];
        int sg[4][2] = { { 1, 1 }, { 1, -1 }, { -1, -1 }, { -1, 1 } };
        for (k = 0; k < 4; ++k) {
            int j;
            for (j = 0; j < 3; ++j)
                q[k][j] = base[j] + e1[j] * CONE_HALF * sg[k][0] + e2[j] * CONE_HALF * sg[k][1];
        }
        out_quad(o, q, COL_PLATE);
    }
    return o->put - start;
}

/* caché derrière le relief : des points entre la caméra et le cône sont sous le sol */
static int hidden_by_ground(const float eye[3], const float p[3], ConesGroundFn ground)
{
    int k;
    for (k = 1; k <= 7; ++k) {
        float t = (float)k / 8.0f, x = eye[0] + (p[0] - eye[0]) * t, y = eye[1] + (p[1] - eye[1]) * t,
              z = eye[2] + (p[2] - eye[2]) * t, h;
        if (ground(x, z, &h) && h > y + 0.08f)
            return 1;
    }
    return 0;
}

int cones_draw(int max, const float eye[3], ConesProjectFn proj, ConesQuadFn put, ConesGroundFn ground)
{
    int idx[CONES_MAX], n = 0, i, j;
    float dist[CONES_MAX];
    Out o;
    o.put = 0;
    o.max = max;
    o.proj = proj;
    o.fn = put;
    for (i = 0; i < CONES_MAX; ++i) {
        const Cone *c = &g_c[i];
        float d[3], top[3];
        if (!c->used)
            continue;
        v_sub(d, c->pos, eye);
        dist[i] = sqrtf(v_dot(d, d));
        if (dist[i] > 120.0f)
            continue;
        top[0] = c->pos[0];
        top[1] = c->pos[1] + (c->down ? 0.15f : 0.45f);
        top[2] = c->pos[2];
        if (ground && dist[i] > 2.0f && hidden_by_ground(eye, top, ground))
            continue;
        idx[n++] = i;
    }
    /* du plus loin au plus proche (les proches dessinés par-dessus) */
    for (i = 1; i < n; ++i) {
        int v = idx[i];
        for (j = i; j > 0 && dist[idx[j - 1]] < dist[v]; --j)
            idx[j] = idx[j - 1];
        idx[j] = v;
    }
    /* pas assez de place : on garde les plus proches */
    {
        int need = 0, first = 0;
        for (i = n - 1; i >= 0; --i) {
            int cost = dist[idx[i]] < 25.0f ? 21 : 11;
            if (need + cost > max)
                break;
            need += cost;
            first = i;
        }
        if (n && need == 0)
            first = n;
        for (i = first; i < n; ++i)
            draw_one(&o, &g_c[idx[i]], eye, dist[idx[i]] < 25.0f ? 4 : 2);
    }
    return o.put;
}

/* ---------- v1.8.49 : modèle 3D (dessiné dans la scène, avec la profondeur) ---------- */

static unsigned long lit_gray(const float nrm[3])
{
    /* soleil haut, un peu de côté ; la texture donne la couleur */
    static const float L[3] = { 0.36f, 0.86f, 0.36f };
    float d = nrm[0] * L[0] + nrm[1] * L[1] + nrm[2] * L[2], k;
    unsigned long g;
    k = 0.50f + 0.55f * (d > 0.0f ? d : 0.0f);
    if (k > 1.0f)
        k = 1.0f;
    g = (unsigned long)(k * 255.0f);
    return g | (g << 8) | (g << 16);
}

/* v1.8.91 : caméra : les cônes derrière elle ou à plus de maxd m ne sont pas calculés */
static int g_view_ok = 0;
static float g_view_eye[3], g_view_fwd[3], g_view_max = 150.0f;
void cones_set_view(const float eye[3], const float fwd[3], float maxd)
{
    g_view_ok = eye && fwd;
    if (g_view_ok) {
        memcpy(g_view_eye, eye, sizeof(g_view_eye));
        memcpy(g_view_fwd, fwd, sizeof(g_view_fwd));
        g_view_max = maxd;
    }
}
static int out_of_view(const float p[3])
{
    float d[3] = { p[0] - g_view_eye[0], p[1] - g_view_eye[1], p[2] - g_view_eye[2] };
    if (!g_view_ok)
        return 0;
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > g_view_max * g_view_max)
        return 1;
    return d[0] * g_view_fwd[0] + d[1] * g_view_fwd[1] + d[2] * g_view_fwd[2] < -1.5f;
}

int cones_mesh(float (*pos)[3], float (*uv)[2], unsigned long *col, unsigned char *tex, int max_verts,
               const float eye[3])
{
    int i, n = 0;
    for (i = 0; i < CONES_MAX; ++i) {
        const Cone *c = &g_src[i];
        float a[3], base[3], e1[3], e2[3];
        int v, k;
        if (!c->used || out_of_view(c->pos))
            continue;
        if (eye) {
            float dx = c->pos[0] - eye[0], dz = c->pos[2] - eye[2];
            if (dx * dx + dz * dz > 250.0f * 250.0f)
                continue;
        }
        if (n + CONE_MODEL_TRIS * 3 > max_verts)
            break;
        cone_frame(c, a, base, e1, e2);
        for (v = 0; v < CONE_MODEL_TRIS * 3; ++v) {
            const float *m = k_cone_model[v];
            float nr[3];
            for (k = 0; k < 3; ++k) {
                pos[n][k] = base[k] + (e1[k] * m[0] + e2[k] * m[1] + a[k] * m[2]) * CONE_S;
                nr[k] = e1[k] * m[3] + e2[k] * m[4] + a[k] * m[5];
            }
            uv[n][0] = m[6];
            uv[n][1] = m[7];
            col[n] = lit_gray(nr);
            tex[n] = (unsigned char)(c->var >= 0 && c->var < CONE_VARIANTS ? c->var : 0);
            ++n;
        }
    }
    return n;
}

/* ---------- v1.8.52 : rediffusion : l'état des cônes est retenu au temps de la rediffusion ---------- */

typedef struct {
    float p[NP][3];
    unsigned char slot, var, pad[2];
} ConeRec;

#define HIST_MAX 20000
#define POOL_MAX 150000
static double *g_ht = NULL;       /* temps de chaque état */
static int *g_hoff = NULL, *g_hcnt = NULL;
static ConeRec *g_pool = NULL;
static int g_hn = 0, g_pn = 0;
static ConeRec g_cur[CONES_MAX];

void cones_hist_clear(void)
{
    g_hn = 0;
    g_pn = 0;
}

static int snap_now(ConeRec *r)
{
    int i, n = 0;
    for (i = 0; i < CONES_MAX; ++i) {
        const Cone *c = &g_c[i];
        if (!c->used)
            continue;
        memset(&r[n], 0, sizeof(r[n]));
        memcpy(r[n].p, c->p, sizeof(r[n].p));
        r[n].slot = (unsigned char)i;
        r[n].var = (unsigned char)c->var;
        ++n;
    }
    return n;
}

void cones_hist_record(double t)
{
    int n;
    if (t < 0.0)
        return;
    if (!g_ht) {
        g_ht = (double *)malloc(sizeof(double) * HIST_MAX);
        g_hoff = (int *)malloc(sizeof(int) * HIST_MAX);
        g_hcnt = (int *)malloc(sizeof(int) * HIST_MAX);
        g_pool = (ConeRec *)malloc(sizeof(ConeRec) * POOL_MAX);
        if (!g_ht || !g_hoff || !g_hcnt || !g_pool)
            return;
    }
    if (!g_pool)
        return;
    if (g_hn && t < g_ht[g_hn - 1] - 1.0) {
        if (g_log)
            g_log("cones : rediffusion : l'horloge repart (%.1f s -> %.1f s), etats effaces", g_ht[g_hn - 1], t);
        cones_hist_clear();
    }
    n = snap_now(g_cur);
    if (g_hn) {
        /* rien n'a changé : pas de nouvel état */
        if (g_hcnt[g_hn - 1] == n && memcmp(g_pool + g_hoff[g_hn - 1], g_cur, sizeof(ConeRec) * n) == 0)
            return;
        if (t - g_ht[g_hn - 1] < 0.04 && t >= g_ht[g_hn - 1])
            return;
    }
    if (g_hn >= HIST_MAX || g_pn + n > POOL_MAX) {
        /* plein : on oublie la première moitié */
        int keep = g_hn / 2, off = g_hoff[keep], k;
        memmove(g_pool, g_pool + off, sizeof(ConeRec) * (g_pn - off));
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
    memcpy(g_pool + g_pn, g_cur, sizeof(ConeRec) * n);
    g_pn += n;
    g_hn++;
}

int cones_hist_count(void)
{
    return g_hn;
}

void cones_show_live(void)
{
    g_src = g_c;
}

int cones_show_replay(double t)
{
    int lo = 0, hi = g_hn - 1, k, i;
    if (!g_hn) {
        g_src = g_c; /* rien de retenu : ceux du moment */
        return 0;
    }
    memset(g_disp, 0, sizeof(g_disp));
    g_src = g_disp;
    if (t < g_ht[0])
        return 0; /* pas encore posés à ce moment */
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (g_ht[mid] <= t)
            lo = mid;
        else
            hi = mid - 1;
    }
    for (k = 0; k < g_hcnt[lo]; ++k) {
        const ConeRec *r = &g_pool[g_hoff[lo] + k];
        Cone *c = &g_disp[r->slot % CONES_MAX];
        c->used = 1;
        memcpy(c->p, r->p, sizeof(c->p));
        c->var = r->var;
        refresh(c);
    }
    for (i = 0, k = 0; i < CONES_MAX; ++i)
        k += g_disp[i].used;
    return k;
}

int cones_shown_count(void)
{
    int i, n = 0;
    for (i = 0; i < CONES_MAX; ++i)
        n += g_src[i].used;
    return n;
}

const unsigned char *cones_texture_z(int v, int *len, int *w, int *h)
{
    if (v < 0 || v >= CONE_VARIANTS)
        v = 0;
    *len = k_cone_tex_zlen[v];
    *w = CONE_TEX_W;
    *h = CONE_TEX_H;
    return k_cone_tex_z[v];
}

/* ---------- v1.8.65 : en ligne : nos cônes envoyés aux autres plugins, les leurs affichés ---------- */

/* sur le réseau, par cône : centre du socle, axe, e1 (9 float), numéro, variante */
typedef struct {
    float b[3], a[3], e1[3];
    unsigned char slot, var, pad[2];
} NetCone;

#define NET_PER_PART 24
#define RPEERS 8
typedef struct {
    uint32_t id;
    double seen;
    int n;
    NetCone c[CONES_MAX];
    unsigned char have[CONES_MAX];
    double hide[CONES_MAX]; /* v1.8.67 : donné / retiré : ignoré jusqu'à ce moment (paquets en retard) */
    double adopted[CONES_MAX];
} RPeer;
static RPeer g_rp[RPEERS];
static volatile long g_rbusy = 0;
static double g_rnow = 0.0;

int cones_net_moving(void)
{
    int i;
    for (i = 0; i < CONES_MAX; ++i)
        if (g_c[i].used && (!g_c[i].asleep || g_c[i].held))
            return 1;
    return 0;
}

int cones_net_pack(unsigned char *buf, int max, int part, int *nparts)
{
    int i, n = 0, k = 0, total = cones_count(), np;
    np = total ? (total + NET_PER_PART - 1) / NET_PER_PART : 1;
    *nparts = np;
    if (max < 4 + NET_PER_PART * (int)sizeof(NetCone))
        return 0;
    buf[0] = 1; /* version */
    buf[1] = (unsigned char)part;
    buf[2] = (unsigned char)np;
    for (i = 0; i < CONES_MAX; ++i) {
        NetCone nc;
        float e2[3];
        if (!g_c[i].used)
            continue;
        if (k++ / NET_PER_PART != part)
            continue;
        frame_of(&g_c[i], nc.b, nc.a, nc.e1, e2);
        nc.slot = (unsigned char)i;
        nc.var = (unsigned char)g_c[i].var;
        nc.pad[0] = nc.pad[1] = 0;
        memcpy(buf + 4 + n * sizeof(NetCone), &nc, sizeof(nc));
        ++n;
    }
    buf[3] = (unsigned char)n;
    return 4 + n * (int)sizeof(NetCone);
}

void cones_net_recv(uint32_t id, const unsigned char *buf, int len, double now)
{
    int i, k = -1, old = 0, part, np, n;
    RPeer *p;
    if (len < 4 || buf[0] != 1)
        return;
    part = buf[1];
    np = buf[2];
    n = buf[3];
    if (len < 4 + n * (int)sizeof(NetCone) || n > NET_PER_PART)
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
            g_log("cones : en ligne : cones d'un autre joueur recus");
    }
    p = &g_rp[k];
    g_rbusy = 1;
    if (part == 0) {
        /* nouvel état : la première partie efface ceux qui ne sont plus envoyés (dans cette partie) */
        memset(p->have, 0, sizeof(p->have));
    }
    for (i = 0; i < n; ++i) {
        NetCone nc;
        memcpy(&nc, buf + 4 + i * sizeof(NetCone), sizeof(nc));
        if (nc.slot >= CONES_MAX || !isfinite(nc.b[0]) || !isfinite(nc.a[0]) || !isfinite(nc.e1[0]))
            continue;
        if (now < p->hide[nc.slot])
            continue;
        p->c[nc.slot] = nc;
        p->have[nc.slot] = 1;
    }
    (void)np;
    p->seen = now;
    g_rbusy = 0;
}

void cones_net_clock(double now)
{
    g_rnow = now;
}

int cones_net_remote_count(void)
{
    int i, j, n = 0;
    for (i = 0; i < RPEERS; ++i)
        if (g_rp[i].id && g_rnow - g_rp[i].seen < 3.0)
            for (j = 0; j < CONES_MAX; ++j)
                n += g_rp[i].have[j];
    return n;
}

/* sommets des cônes des autres joueurs (mêmes conventions que cones_mesh) */
int cones_mesh_remote(float (*pos)[3], float (*uv)[2], unsigned long *col, unsigned char *tex, int max_verts)
{
    int i, j, v, k, n = 0;
    if (g_rbusy)
        return 0;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 3.0)
            continue;
        for (j = 0; j < CONES_MAX; ++j) {
            const NetCone *c = &g_rp[i].c[j];
            float e2[3];
            if (!g_rp[i].have[j] || out_of_view(c->b))
                continue;
            if (n + CONE_MODEL_TRIS * 3 > max_verts)
                return n;
            v_cross(e2, c->a, c->e1);
            for (v = 0; v < CONE_MODEL_TRIS * 3; ++v) {
                const float *m = k_cone_model[v];
                float nr[3];
                for (k = 0; k < 3; ++k) {
                    pos[n][k] = c->b[k] + (c->e1[k] * m[0] + e2[k] * m[1] + c->a[k] * m[2]) * CONE_S;
                    nr[k] = c->e1[k] * m[3] + e2[k] * m[4] + c->a[k] * m[5];
                }
                uv[n][0] = m[6];
                uv[n][1] = m[7];
                col[n] = lit_gray(nr);
                tex[n] = (unsigned char)(c->var < CONE_VARIANTS ? c->var : 0);
                ++n;
            }
        }
    }
    return n;
}

/* ---------- v1.8.66 : cônes des autres joueurs : on peut tout faire avec ----------
   Un cône est simulé par un seul joueur (son « propriétaire »). Pour le toucher, le pousser, le
   ramasser, on le demande : le propriétaire nous le donne (son état complet), il devient le nôtre. */

typedef struct {
    float p[NP][3], pp[NP][3], spawn[4];
    unsigned char var, pad[3];
} GiveCone;

int cones_net_give_size(void)
{
    return (int)sizeof(GiveCone);
}

/* le propriétaire : son cône slot quitte sa liste, son état est écrit dans out ; 0 si impossible */
int cones_net_give(int slot, unsigned char *out)
{
    GiveCone g;
    Cone *c;
    if (slot < 0 || slot >= CONES_MAX || !g_c[slot].used || g_c[slot].held)
        return 0;
    c = &g_c[slot];
    memcpy(g.p, c->p, sizeof(g.p));
    memcpy(g.pp, c->pp, sizeof(g.pp));
    memcpy(g.spawn, c->spawn, sizeof(g.spawn));
    g.var = (unsigned char)c->var;
    memset(g.pad, 0, sizeof(g.pad));
    memcpy(out, &g, sizeof(g));
    c->used = 0;
    return 1;
}

/* on reçoit un cône donné : il devient le nôtre ; rend son numéro (-1 : plein / invalide) */
int cones_net_adopt(uint32_t from, int from_slot, const unsigned char *data, int len)
{
    GiveCone g;
    int i, j;
    if (len < (int)sizeof(GiveCone))
        return -1;
    memcpy(&g, data, sizeof(g));
    for (j = 0; j < NP; ++j)
        if (!isfinite(g.p[j][0]) || !isfinite(g.p[j][1]) || !isfinite(g.p[j][2]))
            return -1;
    /* plus affiché comme celui de l'autre joueur ; déjà reçu à l'instant : pas une seconde fois */
    for (i = 0; i < RPEERS; ++i)
        if (g_rp[i].id == from && from_slot >= 0 && from_slot < CONES_MAX) {
            if (g_rnow - g_rp[i].adopted[from_slot] < 1.0)
                return -1;
            g_rp[i].adopted[from_slot] = g_rnow;
            g_rp[i].have[from_slot] = 0;
            g_rp[i].hide[from_slot] = g_rnow + 1.5;
        }
    for (i = 0; i < CONES_MAX; ++i) {
        if (!g_c[i].used) {
            Cone *c = &g_c[i];
            memset(c, 0, sizeof(*c));
            c->used = 1;
            memcpy(c->p, g.p, sizeof(c->p));
            memcpy(c->pp, g.pp, sizeof(c->pp));
            memcpy(c->spawn, g.spawn, sizeof(c->spawn));
            c->var = g.var < CONE_VARIANTS ? g.var : 0;
            for (j = 0; j < NP; ++j) {
                c->gh[j] = g.p[j][1];
                c->gq[j][0] = c->gq[j][1] = 1e9f;
            }
            refresh(c);
            return i;
        }
    }
    return -1;
}

/* cône d'un autre joueur le plus proche de (x, z) : propriétaire, numéro chez lui, distance */
int cones_net_nearest(float x, float z, float maxd, uint32_t *id, int *slot, float pos[3])
{
    int i, j, found = 0;
    float bd = maxd * maxd;
    if (g_rbusy)
        return 0;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 3.0)
            continue;
        for (j = 0; j < CONES_MAX; ++j) {
            const NetCone *c = &g_rp[i].c[j];
            float dx, dz;
            if (!g_rp[i].have[j])
                continue;
            dx = c->b[0] - x;
            dz = c->b[2] - z;
            if (dx * dx + dz * dz < bd) {
                bd = dx * dx + dz * dz;
                *id = g_rp[i].id;
                *slot = j;
                if (pos)
                    memcpy(pos, c->b, sizeof(float) * 3);
                found = 1;
            }
        }
    }
    return found;
}

/* cône d'un autre joueur qui touche un point (sphère p, r) : propriétaire et numéro */
int cones_net_touching(const float p[3], float r, uint32_t *id, int *slot)
{
    int i, j;
    if (g_rbusy)
        return 0;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 3.0)
            continue;
        for (j = 0; j < CONES_MAX; ++j) {
            const NetCone *c = &g_rp[i].c[j];
            float d[3], t, q[3], k;
            int m;
            if (!g_rp[i].have[j])
                continue;
            /* distance au segment socle-pointe */
            for (m = 0; m < 3; ++m)
                d[m] = p[m] - c->b[m];
            t = v_dot(d, c->a);
            t = t < 0.0f ? 0.0f : (t > CONE_H ? CONE_H : t);
            for (m = 0; m < 3; ++m)
                q[m] = d[m] - c->a[m] * t;
            k = CONE_HALF + (CONE_RT - CONE_HALF) * t / CONE_H;
            if (v_dot(q, q) < (r + k) * (r + k)) {
                *id = g_rp[i].id;
                *slot = j;
                return 1;
            }
        }
    }
    return 0;
}

/* retirer tous les cônes des autres joueurs de l'affichage (ils les retirent chez eux) */
void cones_net_forget(uint32_t id, int slot)
{
    int i;
    for (i = 0; i < RPEERS; ++i)
        if (g_rp[i].id == id && slot >= 0 && slot < CONES_MAX) {
            g_rp[i].have[slot] = 0;
            g_rp[i].hide[slot] = g_rnow + 1.5;
        }
}

/* ---------- v1.8.97 : dessin par modèle ---------- */
const float *cones_model_data(int *nverts)
{
    *nverts = CONE_MODEL_TRIS * 3;
    return &k_cone_model[0][0];
}

static void cone_inst(ObjInst *o, const float base[3], const float a[3], const float e1[3], const float e2[3], int var,
                      int tex0)
{
    float x[3], y[3], z[3];
    int k;
    for (k = 0; k < 3; ++k) {
        x[k] = e1[k] * CONE_S;
        y[k] = e2[k] * CONE_S;
        z[k] = a[k] * CONE_S;
    }
    o->model = OBJM_CONE;
    o->tex = tex0 + (var >= 0 && var < CONE_VARIANTS ? var : 0);
    objinst_matrix(o->m, x, y, z, base);
}

int cones_instances(ObjInst *out, int max, int tex0)
{
    int i, n = 0;
    for (i = 0; i < CONES_MAX && n < max; ++i) {
        const Cone *c = &g_src[i];
        float a[3], base[3], e1[3], e2[3];
        if (!c->used || out_of_view(c->pos))
            continue;
        cone_frame(c, a, base, e1, e2);
        cone_inst(&out[n++], base, a, e1, e2, c->var, tex0);
    }
    return n;
}

int cones_instances_remote(ObjInst *out, int max, int tex0)
{
    int i, j, n = 0;
    if (g_rbusy)
        return 0;
    for (i = 0; i < RPEERS; ++i) {
        if (!g_rp[i].id || g_rnow - g_rp[i].seen > 3.0)
            continue;
        for (j = 0; j < CONES_MAX && n < max; ++j) {
            const NetCone *c = &g_rp[i].c[j];
            float e2[3];
            if (!g_rp[i].have[j] || out_of_view(c->b))
                continue;
            v_cross(e2, c->a, c->e1);
            cone_inst(&out[n++], c->b, c->a, c->e1, e2, c->var, tex0);
        }
    }
    return n;
}

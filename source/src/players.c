/*
 * players.c - v1.8 : collisions avec les autres joueurs (voir players.h)
 */
#include "players.h"

#include <math.h>
#include <string.h>

#define MAX_BIKES 64
#define MAX_WALKERS 8
#define BIKE_R 0.38f      /* demi-largeur de la moto (guidon compris), m */
#define BIKE_FRONT 0.70f  /* du centre à la roue avant, m */
#define BIKE_BACK 0.65f   /* du centre à la roue arrière, m */
#define BIKE_ROUND 0.62f  /* sens inconnu : la moto devient un disque */
#define BIKE_LOW 1.50f    /* v1.8.2 : la hauteur donnée par le jeu n'est pas forcément le centre */
#define BIKE_HIGH 1.80f   /* v1.8.2 : (vu : pas de choc, bassin plus haut que le centre + 0,85) */
#define WALKER_R 0.24f
#define STALE 1.0         /* s sans nouvelle position : la moto n'est plus là */

typedef struct {
    int num, used;
    float pos[3], vel[3];
    double t;
    float yaw;            /* degrés, comme le jeu le donne */
    float heading;        /* radians, 0 = +z (même repère que la marche) */
    int head_ok;          /* sens connu (roule, ou correspondance yaw -> sens apprise) */
    int crashed;          /* v1.8.4 : pilote tombé : la position donnée n'est plus celle de la moto */
} Bike;

typedef struct {
    float x, z, ymin, ymax;
} Walker2;

/* v1.8.4 : moto vue par son modèle 3D (vraie place et vraie orientation, couchée comprise) :
   segment roue arrière -> roue avant, à mi-hauteur de la moto, et un rayon */
#define MAX_MODELS 16
typedef struct {
    float a[3], b[3];
    int own;
} BikeModel;
static BikeModel g_m[MAX_MODELS];
static int g_nm = 0;
static int g_own_on = 0;

#define MODEL_R 0.34f
/* point le plus proche du segment [a, b] */
static float seg_closest(const float a[3], const float b[3], const float p[3], float q[3])
{
    float ab[3], t = 0.0f, l2 = 0.0f, d2 = 0.0f;
    int k;
    for (k = 0; k < 3; ++k) {
        ab[k] = b[k] - a[k];
        l2 += ab[k] * ab[k];
        t += (p[k] - a[k]) * ab[k];
    }
    t = l2 > 1e-6f ? t / l2 : 0.0f;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    for (k = 0; k < 3; ++k) {
        q[k] = a[k] + ab[k] * t;
        d2 += (p[k] - q[k]) * (p[k] - q[k]);
    }
    return sqrtf(d2);
}

/* une moto vue par son modèle près de (x, z) ? (pour ne pas compter deux fois la même moto) */
static int model_near(float x, float z, float r)
{
    int k;
    for (k = 0; k < g_nm; ++k) {
        float cx = 0.5f * (g_m[k].a[0] + g_m[k].b[0]) - x, cz = 0.5f * (g_m[k].a[2] + g_m[k].b[2]) - z;
        if (cx * cx + cz * cz < r * r)
            return 1;
    }
    return 0;
}

static Bike g_b[MAX_BIKES];
static Walker2 g_w[MAX_WALKERS];
static int g_nw = 0;
static int g_self = -1000;
static float g_self_pos[3];
static int g_self_pos_ok = 0;
static double g_now = 0.0;
static float g_near_dy = 0.0f;
static PlayersLogFn g_log = 0;
/* correspondance yaw du jeu -> sens : 4 possibilités (signe de sin, signe de cos), apprise sur
   les motos qui roulent */
static float g_vote[4];
static int g_votes = 0, g_map = -1;

void players_init(PlayersLogFn log)
{
    g_log = log;
    players_reset();
}

void players_reset(void)
{
    memset(g_b, 0, sizeof(g_b));
    g_nw = 0;
}

void players_set_self(int num) { g_self = num; }
void players_set_self_pos(const float p[3])
{
    memcpy(g_self_pos, p, sizeof(g_self_pos));
    g_self_pos_ok = 1;
}
void players_walkers_clear(void) { g_nw = 0; }

void players_walker_add(const float pts[RD_NUM_POINTS][3])
{
    Walker2 *w;
    int i;
    if (g_nw >= MAX_WALKERS)
        return;
    w = &g_w[g_nw++];
    w->x = 0.5f * (pts[RD_PELVIS][0] + pts[RD_CHEST][0]);
    w->z = 0.5f * (pts[RD_PELVIS][2] + pts[RD_CHEST][2]);
    w->ymin = w->ymax = pts[0][1];
    for (i = 1; i < RD_NUM_POINTS; ++i) {
        if (pts[i][1] < w->ymin)
            w->ymin = pts[i][1];
        if (pts[i][1] > w->ymax)
            w->ymax = pts[i][1];
    }
}

static void map_dir(int m, float yaw_deg, float *dx, float *dz)
{
    float a = yaw_deg * 3.14159265f / 180.0f;
    *dx = (m & 1 ? -1.0f : 1.0f) * sinf(a);
    *dz = (m & 2 ? -1.0f : 1.0f) * cosf(a);
}

void players_track(int n, const void *arr, int elem, double now)
{
    int i, k;
    g_now = now;
    if (!arr || elem < 20 || n <= 0)
        return;
    for (i = 0; i < n && i < 256; ++i) {
        const unsigned char *e = (const unsigned char *)arr + (size_t)i * (size_t)elem;
        int num;
        float p[4];
        Bike *b = NULL;
        memcpy(&num, e, 4);
        memcpy(p, e + 4, 16); /* x, y, z, yaw */
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
            continue;
        for (k = 0; k < MAX_BIKES; ++k)
            if (g_b[k].used && g_b[k].num == num) {
                b = &g_b[k];
                break;
            }
        if (!b) {
            for (k = 0; k < MAX_BIKES; ++k)
                if (!g_b[k].used || now - g_b[k].t > 10.0) {
                    b = &g_b[k];
                    memset(b, 0, sizeof(*b));
                    b->used = 1;
                    b->num = num;
                    memcpy(b->pos, p, sizeof(b->pos));
                    b->t = now;
                    break;
                }
            if (!b)
                continue;
        } else {
            double dt = now - b->t;
            if (dt > 0.005 && dt < 1.0) {
                float a = dt > 0.2 ? 1.0f : 0.5f;
                for (k = 0; k < 3; ++k)
                    b->vel[k] += a * ((p[k] - b->pos[k]) / (float)dt - b->vel[k]);
            } else if (dt >= 1.0) {
                b->vel[0] = b->vel[1] = b->vel[2] = 0.0f;
            }
            memcpy(b->pos, p, sizeof(b->pos));
            b->t = now;
        }
        b->yaw = p[3];
        if (elem >= 28)
            memcpy(&b->crashed, e + 24, 4);
        {
            float hs = sqrtf(b->vel[0] * b->vel[0] + b->vel[2] * b->vel[2]);
            if (hs > 3.0f) {
                /* il roule : le sens est celui du mouvement ; on apprend aussi le yaw du jeu */
                b->heading = atan2f(b->vel[0], b->vel[2]);
                b->head_ok = 1;
                if (hs > 6.0f && isfinite(p[3]) && g_votes < 400) {
                    int m;
                    for (m = 0; m < 4; ++m) {
                        float dx, dz;
                        map_dir(m, p[3], &dx, &dz);
                        g_vote[m] += (dx * b->vel[0] + dz * b->vel[2]) / hs;
                    }
                    if (++g_votes == 60) {
                        int best = 0;
                        for (m = 1; m < 4; ++m)
                            if (g_vote[m] > g_vote[best])
                                best = m;
                        g_map = g_vote[best] > 0.8f * (float)g_votes ? best : -1;
                        if (g_log)
                            g_log("collisions : sens des motos d'apres le jeu %s (accord %.0f %%)",
                                  g_map >= 0 ? "appris" : "pas fiable, disque autour des motos arretees",
                                  100.0f * g_vote[best] / (float)g_votes);
                    }
                }
            } else if (g_map >= 0 && isfinite(p[3])) {
                float dx, dz;
                map_dir(g_map, p[3], &dx, &dz);
                b->heading = atan2f(dx, dz);
                b->head_ok = 1;
            } else if (hs < 1.0f) {
                b->head_ok = b->head_ok && g_map >= 0;
            }
        }
    }
}

/* v1.8.2 : position prévue maintenant (le jeu ne donne les positions que de temps en temps :
   à 35 km/h la moto avance d'1 m entre deux, elle passait « à travers » le pilote) */
static float g_sweep = 0.0f; /* allongement de la moto vers l'arrière (chemin parcouru), m */
static void bike_now(const Bike *b, float out[3])
{
    double dt = g_now - b->t;
    int k;
    if (dt < 0.0)
        dt = 0.0;
    if (dt > 0.3)
        dt = 0.3;
    for (k = 0; k < 3; ++k)
        out[k] = b->pos[k] + b->vel[k] * (float)dt;
}

/* distance horizontale de (x, z) à la moto (segment roue arrière -> roue avant), rayon de la forme */
static float bike_dist(const Bike *b, float x, float z, float *rad, float *ox, float *oz)
{
    float bp[3], px, pz, cx = 0.0f, cz = 0.0f;
    bike_now(b, bp);
    px = x - bp[0];
    pz = z - bp[2];
    if (b->head_ok) {
        float fx = sinf(b->heading), fz = cosf(b->heading), s = px * fx + pz * fz;
        if (s > BIKE_FRONT)
            s = BIKE_FRONT;
        if (s < -BIKE_BACK - g_sweep)
            s = -BIKE_BACK - g_sweep;
        cx = s * fx;
        cz = s * fz;
        *rad = BIKE_R;
    } else {
        *rad = BIKE_ROUND;
    }
    *ox = px - cx;
    *oz = pz - cz;
    return sqrtf(*ox * *ox + *oz * *oz);
}

static int bike_live(const Bike *b)
{
    if (!b->used || b->num == g_self || g_now - b->t >= STALE)
        return 0;
    /* notre moto (au cas où notre numéro ne serait pas encore connu) : à notre position */
    if (g_self_pos_ok) {
        float dx = b->pos[0] - g_self_pos[0], dy = b->pos[1] - g_self_pos[1], dz = b->pos[2] - g_self_pos[2];
        if (dx * dx + dy * dy + dz * dz < 0.8f * 0.8f)
            return 0;
    }
    return 1;
}

int players_blocked(float x, float z, float feet, float r)
{
    int k;
    for (k = 0; k < g_nm; ++k) {
        /* le pilote debout = points à 0,25 / 0,6 / 1,0 m au-dessus des pieds */
        static const float hs[3] = { 0.25f, 0.6f, 1.0f };
        int j;
        if (g_m[k].own && !g_own_on)
            continue;
        for (j = 0; j < 3; ++j) {
            float p[3] = { x, feet + hs[j], z }, q[3];
            if (seg_closest(g_m[k].a, g_m[k].b, p, q) < MODEL_R + r)
                return 1;
        }
    }
    for (k = 0; k < MAX_BIKES; ++k) {
        const Bike *b = &g_b[k];
        float rad, ox, oz;
        if (!bike_live(b) || b->crashed || model_near(b->pos[0], b->pos[2], 1.6f) ||
            feet < b->pos[1] - BIKE_LOW - 0.3f || feet > b->pos[1] + 0.3f)
            continue;
        if (bike_dist(b, x, z, &rad, &ox, &oz) < rad + r)
            return 1;
    }
    for (k = 0; k < g_nw; ++k) {
        const Walker2 *w = &g_w[k];
        float dx = x - w->x, dz = z - w->z;
        if (feet > w->ymax || feet < w->ymin - 0.6f)
            continue;
        if (dx * dx + dz * dz < (WALKER_R + r) * (WALKER_R + r))
            return 1;
    }
    return 0;
}

int players_push(const float p[3], float r, float push[3])
{
    int k, hit = 0;
    push[0] = push[1] = push[2] = 0.0f;
    for (k = 0; k < g_nm; ++k) {
        float q[3], d;
        if (g_m[k].own && !g_own_on)
            continue;
        d = seg_closest(g_m[k].a, g_m[k].b, p, q);
        if (d < MODEL_R + r) {
            float m = MODEL_R + r - d;
            int j;
            if (d > 1e-4f)
                for (j = 0; j < 3; ++j)
                    push[j] += (p[j] - q[j]) / d * m;
            else
                push[1] += m;
            hit = 1;
        }
    }
    for (k = 0; k < MAX_BIKES; ++k) {
        const Bike *b = &g_b[k];
        float rad, ox, oz, d;
        if (!bike_live(b) || b->crashed || model_near(b->pos[0], b->pos[2], 1.6f) ||
            p[1] < b->pos[1] - BIKE_LOW || p[1] > b->pos[1] + BIKE_HIGH)
            continue;
        d = bike_dist(b, p[0], p[2], &rad, &ox, &oz);
        if (d < rad + r) {
            float m = rad + r - d;
            if (d > 1e-4f) {
                push[0] += ox / d * m;
                push[2] += oz / d * m;
            } else {
                push[0] += m;
            }
            hit = 1;
        }
    }
    for (k = 0; k < g_nw; ++k) {
        const Walker2 *w = &g_w[k];
        float dx = p[0] - w->x, dz = p[2] - w->z, d;
        if (p[1] < w->ymin - r || p[1] > w->ymax + r)
            continue;
        d = sqrtf(dx * dx + dz * dz);
        if (d < WALKER_R + r) {
            float m = WALKER_R + r - d;
            if (d > 1e-4f) {
                push[0] += dx / d * m;
                push[2] += dz / d * m;
            } else {
                push[2] += m;
            }
            hit = 1;
        }
    }
    return hit;
}

int players_hit(const float p[3], float r, float min_speed, float vel[3], int *num)
{
    int k;
    for (k = 0; k < MAX_BIKES; ++k) {
        const Bike *b = &g_b[k];
        float rad, ox, oz, hs, d;
        if (!bike_live(b) || b->crashed || p[1] < b->pos[1] - BIKE_LOW || p[1] > b->pos[1] + BIKE_HIGH)
            continue;
        hs = sqrtf(b->vel[0] * b->vel[0] + b->vel[2] * b->vel[2]);
        if (hs < min_speed)
            continue;
        /* chemin parcouru pendant 0,12 s compris (images ou positions espacées) */
        g_sweep = hs * 0.12f;
        d = bike_dist(b, p[0], p[2], &rad, &ox, &oz);
        g_sweep = 0.0f;
        if (d < rad + r) {
            memcpy(vel, b->vel, sizeof(b->vel));
            if (num)
                *num = b->num;
            return 1;
        }
    }
    return 0;
}

int players_nearest(const float p[3], float *dist, float *speed, int *num)
{
    int k, best = 0;
    float bd = 1e9f;
    for (k = 0; k < MAX_BIKES; ++k) {
        const Bike *b = &g_b[k];
        float rad, ox, oz, d;
        if (!bike_live(b))
            continue;
        d = bike_dist(b, p[0], p[2], &rad, &ox, &oz) - rad;
        if (d < bd) {
            bd = d;
            g_near_dy = p[1] - b->pos[1];
            *speed = sqrtf(b->vel[0] * b->vel[0] + b->vel[2] * b->vel[2]);
            *num = b->num;
            best = 1;
        }
    }
    *dist = bd;
    return best;
}

int players_walker_count(void) { return g_nw; }

void players_models_clear(int own_collide)
{
    g_nm = 0;
    g_own_on = own_collide;
}

void players_model_add(const float M[12], int own)
{
    /* repère du modèle : x à droite, y en haut, z en avant ; origine près du sol, entre les roues */
    static const float la[3] = { 0.0f, 0.55f, -0.62f }, lb[3] = { 0.0f, 0.62f, 0.66f };
    BikeModel *m;
    int k;
    if (g_nm >= MAX_MODELS)
        return;
    m = &g_m[g_nm++];
    for (k = 0; k < 3; ++k) {
        m->a[k] = M[k * 4 + 0] * la[0] + M[k * 4 + 1] * la[1] + M[k * 4 + 2] * la[2] + M[k * 4 + 3];
        m->b[k] = M[k * 4 + 0] * lb[0] + M[k * 4 + 1] * lb[1] + M[k * 4 + 2] * lb[2] + M[k * 4 + 3];
    }
    m->own = own;
}

int players_model_count(void) { return g_nm; }


float players_near_dy(void) { return g_near_dy; }

void players_set_now(double now) { g_now = now; }

/* v1.8.5 : pilote debout dans un obstacle : de combien le sortir (à l'horizontale), en regardant
   à plusieurs hauteurs (pieds, genoux, bassin, torse) ; la plus grosse poussée l'emporte */
int players_push_walker(float x, float z, float feet, float r, float out[2])
{
    static const float hs[4] = { 0.25f, 0.6f, 1.0f, 1.4f };
    int j, hit = 0;
    float best = 0.0f;
    out[0] = out[1] = 0.0f;
    for (j = 0; j < 4; ++j) {
        float p[3] = { x, feet + hs[j], z }, pp[3], m;
        if (!players_push(p, r, pp))
            continue;
        m = pp[0] * pp[0] + pp[2] * pp[2];
        if (m > best) {
            best = m;
            out[0] = pp[0];
            out[1] = pp[2];
            hit = 1;
        }
    }
    return hit;
}

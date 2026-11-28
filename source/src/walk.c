/*
 * walk.c - se relever et marcher après une chute
 */
#include "walk.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265f

/* longueur de jambe utilisée pour la marche capturée (mise à jour par walk_begin) */
static float g_leg = 0.9f;

/* ---------- vecteurs ---------- */

static void v_set(float *o, float x, float y, float z)
{
    o[0] = x;
    o[1] = y;
    o[2] = z;
}

static float v_dist(const float *a, const float *b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void v_norm(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-8f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

/* o = a + b * s */
static void v_madd(float *o, const float *a, const float *b, float s)
{
    o[0] = a[0] + b[0] * s;
    o[1] = a[1] + b[1] * s;
    o[2] = a[2] + b[2] * s;
}

static float smoothstep(float x)
{
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1.0f)
        return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

static WalkGroundFn g_ground_fn = NULL;

void walk_set_ground_fn(WalkGroundFn fn)
{
    g_ground_fn = fn;
}

static WalkObjectFn g_obj_fn = NULL;
/* sol sous le pilote à l'image d'avant : une marche de 45 cm au plus au-dessus (v0.40.43 : avec
   le bassin comme repère, il montait sur les barrières du ponton) */
static float g_ref_y = 0.0f;
static int g_ref_ok = 0;
#define STEP_UP 0.45f
/* au départ (pas encore de sol connu) : repère donné par le plugin (la moto dont il descend) */
static float g_start_ref = -1e9f;
void walk_set_start_ref(float y) { g_start_ref = y; }
float walk_support_y(const Walker *w) { return w->support_ok ? w->support : -1e9f; }
static WalkBlockFn g_block_fn = NULL;

void walk_set_block_fn(WalkBlockFn fn)
{
    g_block_fn = fn;
}

void walk_set_object_fn(WalkObjectFn fn)
{
    g_obj_fn = fn;
}

/* en l'air : le pilote est posé sur un plan invisible qui suit son saut (la pose debout
   habituelle, jambes pliées par dip) */
static int g_air_on = 0;
static float g_air_y = 0.0f;

float walk_ground_height(const Ragdoll *rd, float x, float z)
{
    const float *n = rd->ground_n;
    float h, ho;
    if (g_air_on)
        return g_air_y;
    if (g_ground_fn && g_ground_fn(x, z, &h)) {
        /* objets de la piste (tremplins, bottes de paille...) : la surface la plus haute qui
           reste sous le bassin (marche de 60 cm au plus) */
        if (g_obj_fn && g_obj_fn(x, z, g_ref_ok ? g_ref_y + STEP_UP : fmaxf(rd->pos[RD_PELVIS][1] + 0.3f, g_start_ref), &ho) &&
            ho > h + 0.02f)
            h = ho;
        return h;
    }
    if (n[1] < 0.1f)
        return rd->ground_y;
    return (rd->ground_y - n[0] * x - n[2] * z) / n[1];
}

#define JUMP_SPEED 3.3f  /* m/s : environ 55 cm de haut */
#define FALL_START 0.35f
#define PREJUMP_TIME 0.25f
#define PUSH_TIME 0.09f /* élan avant de quitter le sol */ /* marche plus haute : il tombe au lieu de descendre d'un coup */

/* ressort critique : x suit cible sans à-coup (omega : vitesse, 1/s) */
static void smooth_to(float *x, float *v, float target, float omega, float dt)
{
    float a = -2.0f * omega * *v - omega * omega * (*x - target);
    *v += a * dt;
    *x += *v * dt;
    if (*x < 0.0f) {
        *x = 0.0f;
        if (*v < 0.0f)
            *v = 0.0f;
    }
}

/* sol réel sous (x, z) : surface la plus haute qui ne dépasse pas ymax (hors saut) */
static float ground_below(const Ragdoll *rd, float x, float z, float ymax)
{
    int ok = g_ref_ok, air = g_air_on;
    float ry = g_ref_y, h;
    g_ref_ok = 1;
    g_ref_y = ymax - STEP_UP;
    g_air_on = 0;
    h = walk_ground_height(rd, x, z);
    g_ref_ok = ok;
    g_ref_y = ry;
    g_air_on = air;
    return h;
}

/*
 * IK à deux os : articulation entre root et end, longueurs a et b, pliée du
 * côté de hint. Si end est trop loin, le membre est tendu vers lui.
 */
static void two_bone(const float *root, float *end, float a, float b, const float *hint,
                     float *joint)
{
    float e[3], d, x, h, p[3], dot;
    int k;
    for (k = 0; k < 3; ++k)
        e[k] = end[k] - root[k];
    d = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    if (d < 1e-5f) {
        v_madd(joint, root, hint, a);
        return;
    }
    for (k = 0; k < 3; ++k)
        e[k] /= d;
    {
        /* v0.40.51 : IK « douce » : près de la jambe tendue, l'angle du genou change très vite
           (le genou sautait d'une image à l'autre dans les bosses). La jambe s'approche de
           tendue en douceur, le pied reste un poil plus court que la cible */
        float L = a + b, s0 = 0.97f * L;
        if (d > s0) {
            float span = L - 0.002f - s0, dd = s0 + span * (1.0f - expf(-(d - s0) / span));
            d = dd;
            v_madd(end, root, e, d);
        }
    }
    if (d < fabsf(a - b) + 0.002f)
        d = fabsf(a - b) + 0.002f;
    x = (a * a - b * b + d * d) / (2.0f * d);
    h = a * a - x * x;
    h = h > 0.0f ? sqrtf(h) : 0.0f;
    dot = hint[0] * e[0] + hint[1] * e[1] + hint[2] * e[2];
    for (k = 0; k < 3; ++k)
        p[k] = hint[k] - e[k] * dot;
    v_norm(p);
    for (k = 0; k < 3; ++k)
        joint[k] = root[k] + e[k] * x + p[k] * h;
}

/* ---------- pose ---------- */

/* inclinaison des pieds de la dernière pose (pour orienter les chevilles) */
static float w_foot_pitch[2];

void walk_foot_pitch(float out[2])
{
    out[0] = w_foot_pitch[0];
    out[1] = w_foot_pitch[1];
}

void walk_calibrate(Walker *w, const Ragdoll *rd)
{
    const float(*P)[3] = (const float(*)[3])rd->pos;
    float U[3], R[3], F[3], K[3];
    int k;
    for (k = 0; k < 3; ++k) {
        U[k] = P[RD_CHEST][k] - P[RD_PELVIS][k];
        R[k] = P[RD_HIP_R][k] - P[RD_HIP_L][k];
        K[k] = 0.5f * (P[RD_KNEE_L][k] + P[RD_KNEE_R][k]) - 0.5f * (P[RD_HIP_L][k] + P[RD_HIP_R][k]);
    }
    /* F = U x R */
    F[0] = U[1] * R[2] - U[2] * R[1];
    F[1] = U[2] * R[0] - U[0] * R[2];
    F[2] = U[0] * R[1] - U[1] * R[0];
    /* Notre repère : droite r, avant f, haut u avec u x r = -f. Si le corps dit
     * U x R = avant (genoux devant), sa droite est notre gauche. */
    w->mirror = (F[0] * K[0] + F[1] * K[1] + F[2] * K[2]) > 0.0f;
    w->calibrated = 1;
}

/* Longueur d'un os : celle de la contrainte du ragdoll (prise sur le vrai squelette au départ),
   pas la distance actuelle : après une grosse chute, le corps est étiré et le pilote se
   relevait avec un cou trop long (casque qui flottait au-dessus des épaules, v0.40.52) */
static float rest_dist(const Ragdoll *rd, const float (*P)[3], int a, int b)
{
    int i;
    for (i = 0; i < rd->num_constraints; ++i) {
        const RdConstraint *c = &rd->constraints[i];
        if (c->type == RD_CONSTRAINT_FIXED && ((c->a == a && c->b == b) || (c->a == b && c->b == a)) && c->length > 0.01f)
            return c->length;
    }
    return v_dist(P[a], P[b]);
}

void walk_begin(Walker *w, const Ragdoll *rd)
{
    const float(*P)[3] = (const float(*)[3])rd->pos;
    float dx, dz, sh, d;
    int mirror = w->mirror, calibrated = w->calibrated;
    float ankle = w->ankle_height > 0.0f ? w->ankle_height : ANKLE_HEIGHT;
    memset(w, 0, sizeof(*w));
    g_ref_ok = 0; /* nouveau départ : sol pas encore connu */
    g_air_on = 0;
    w->mirror = mirror;
    w->calibrated = calibrated;
    w->ankle_height = ankle;

    w->thigh = 0.5f * (rest_dist(rd, P, RD_HIP_L, RD_KNEE_L) + rest_dist(rd, P, RD_HIP_R, RD_KNEE_R));
    w->shin = 0.5f * (rest_dist(rd, P, RD_KNEE_L, RD_FOOT_L) + rest_dist(rd, P, RD_KNEE_R, RD_FOOT_R));
    w->upper_arm =
        0.5f * (rest_dist(rd, P, RD_SHOULDER_L, RD_ELBOW_L) + rest_dist(rd, P, RD_SHOULDER_R, RD_ELBOW_R));
    w->forearm = 0.5f * (rest_dist(rd, P, RD_ELBOW_L, RD_HAND_L) + rest_dist(rd, P, RD_ELBOW_R, RD_HAND_R));
    w->spine = rest_dist(rd, P, RD_PELVIS, RD_CHEST);
    w->neck = rest_dist(rd, P, RD_CHEST, RD_NECK);
    w->head = rest_dist(rd, P, RD_NECK, RD_HEAD);
    w->hip_half = 0.5f * rest_dist(rd, P, RD_HIP_L, RD_HIP_R);
    w->shoulder_half = 0.5f * rest_dist(rd, P, RD_SHOULDER_L, RD_SHOULDER_R);
    sh = 0.5f * (rest_dist(rd, P, RD_NECK, RD_SHOULDER_L) + rest_dist(rd, P, RD_NECK, RD_SHOULDER_R));
    d = sh * sh - w->shoulder_half * w->shoulder_half;
    w->shoulder_drop = d > 0.0f ? sqrtf(d) : 0.0f;
    w->hip_height = 0.97f * (w->thigh + w->shin);
    g_leg = w->thigh + w->shin;

    w->pos[0] = P[RD_PELVIS][0];
    w->pos[1] = P[RD_PELVIS][2];
    /* on se relève tourné vers où pointait la tête */
    dx = P[RD_HEAD][0] - P[RD_PELVIS][0];
    dz = P[RD_HEAD][2] - P[RD_PELVIS][2];
    w->heading = (dx * dx + dz * dz > 1e-4f) ? atan2f(dx, dz) : 0.0f;

    memcpy(w->from, rd->pos, sizeof(w->from));
    w->state = WALK_GETTING_UP;
    w->t = 0.0f;
    {
        /* v1.8.15 : sur le dos (ventre vers le ciel) : il se retourne d'abord sur le ventre */
        float up[3], sd[3], fw[3], ax[3];
        int k2;
        for (k2 = 0; k2 < 3; ++k2) {
            up[k2] = P[RD_NECK][k2] - P[RD_PELVIS][k2];
            sd[k2] = P[RD_SHOULDER_R][k2] - P[RD_SHOULDER_L][k2];
            ax[k2] = P[RD_HEAD][k2] - P[RD_PELVIS][k2];
        }
        fw[0] = sd[1] * up[2] - sd[2] * up[1];
        fw[1] = sd[2] * up[0] - sd[0] * up[2];
        fw[2] = sd[0] * up[1] - sd[1] * up[0];
        if (w->mirror)
            fw[1] = -fw[1];
        v_norm(fw);
        ax[1] = 0.0f;
        v_norm(ax);
        w->getup_roll = fw[1] > 0.3f;
        memcpy(w->roll_axis, ax, sizeof(ax));
        for (k2 = 0; k2 < 3; ++k2) {
            int i2;
            w->roll_c[k2] = 0.0f;
            for (i2 = 0; i2 < RD_NUM_POINTS; ++i2)
                w->roll_c[k2] += P[i2][k2] / (float)RD_NUM_POINTS;
        }
        w->getup_T = w->getup_roll ? 2.7f : 2.1f;
    }
}

/* v1.8.15 : à quatre pattes : mains au sol sous les épaules, genoux au sol sous les hanches */
static void quad_pose(const Walker *w, const Ragdoll *rd, float out[RD_NUM_POINTS][3])
{
    float f[3], r[3], up[3], base, d[3];
    int side;
    v_set(f, sinf(w->heading), 0.0f, cosf(w->heading));
    v_set(r, f[2], 0.0f, -f[0]);
    if (w->mirror) {
        r[0] = -r[0];
        r[2] = -r[2];
    }
    v_set(up, 0.0f, 1.0f, 0.0f);
    base = walk_ground_height(rd, w->pos[0], w->pos[1]);
    v_set(out[RD_PELVIS], w->pos[0], base + w->thigh + 0.08f, w->pos[1]);
    v_madd(out[RD_PELVIS], out[RD_PELVIS], f, -0.20f);
    /* dos presque à plat, épaules à hauteur des bras tendus */
    v_set(d, f[0], (base + w->upper_arm + w->forearm + 0.10f - out[RD_PELVIS][1]) / (w->spine + 1e-3f), f[2]);
    v_norm(d);
    v_madd(out[RD_CHEST], out[RD_PELVIS], d, w->spine);
    v_madd(out[RD_NECK], out[RD_CHEST], f, w->neck);
    v_madd(d, f, up, 0.6f);
    v_norm(d);
    v_madd(out[RD_HEAD], out[RD_NECK], d, w->head);
    for (side = 0; side < 2; ++side) {
        float sign = side == 0 ? -1.0f : 1.0f, hint[3], g;
        int hip = side == 0 ? RD_HIP_L : RD_HIP_R, knee = side == 0 ? RD_KNEE_L : RD_KNEE_R;
        int foot = side == 0 ? RD_FOOT_L : RD_FOOT_R, shoulder = side == 0 ? RD_SHOULDER_L : RD_SHOULDER_R;
        int elbow = side == 0 ? RD_ELBOW_L : RD_ELBOW_R, hand = side == 0 ? RD_HAND_L : RD_HAND_R;
        v_madd(out[hip], out[RD_PELVIS], r, sign * w->hip_half);
        /* genou au sol sous la hanche, pied derrière à plat */
        memcpy(out[knee], out[hip], sizeof(out[knee]));
        g = walk_ground_height(rd, out[knee][0], out[knee][2]);
        out[knee][1] = g + 0.07f;
        v_set(d, out[knee][0] - out[hip][0], out[knee][1] - out[hip][1], out[knee][2] - out[hip][2]);
        v_norm(d);
        v_madd(out[knee], out[hip], d, w->thigh);
        v_madd(out[foot], out[knee], f, -w->shin);
        g = walk_ground_height(rd, out[foot][0], out[foot][2]);
        out[foot][1] = g + 0.08f;
        v_set(d, out[foot][0] - out[knee][0], out[foot][1] - out[knee][1], out[foot][2] - out[knee][2]);
        v_norm(d);
        v_madd(out[foot], out[knee], d, w->shin);
        /* main au sol sous l'épaule */
        v_madd(out[shoulder], out[RD_NECK], r, sign * w->shoulder_half);
        out[shoulder][1] -= w->shoulder_drop;
        memcpy(out[hand], out[shoulder], sizeof(out[hand]));
        v_madd(out[hand], out[hand], f, 0.05f);
        g = walk_ground_height(rd, out[hand][0], out[hand][2]);
        out[hand][1] = g + 0.06f;
        v_set(hint, 0.0f, 0.0f, 0.0f);
        v_madd(hint, hint, r, sign * 0.6f);
        v_madd(hint, hint, f, -0.4f);
        v_norm(hint);
        two_bone(out[shoulder], out[hand], w->upper_arm, w->forearm, hint, out[elbow]);
    }
}

/* rotation de p autour de l'axe (c, a unitaire) d'un angle t */
static void rot_axis(const float p[3], const float c[3], const float a[3], float t, float out[3])
{
    float v[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] }, cs = cosf(t), sn = sinf(t), dt;
    dt = v[0] * a[0] + v[1] * a[1] + v[2] * a[2];
    out[0] = c[0] + v[0] * cs + (a[1] * v[2] - a[2] * v[1]) * sn + a[0] * dt * (1.0f - cs);
    out[1] = c[1] + v[1] * cs + (a[2] * v[0] - a[0] * v[2]) * sn + a[1] * dt * (1.0f - cs);
    out[2] = c[2] + v[2] * cs + (a[0] * v[1] - a[1] * v[0]) * sn + a[2] * dt * (1.0f - cs);
}

/* Pose debout / en marche, d'après la position, la direction, la vitesse et la phase */
/* Position d'un pied dans le cycle : appui (pied fixe au sol, il recule par rapport au
 * bassin), puis phase aérienne (il revient devant en se levant). Retourne le décalage
 * avant/arrière ; *up = hauteur levée, *pitch = pointe du pied (+ = vers le bas). */
static float gait_foot(float ph, float half, float lift, float stance, float *up, float *pitch, float kick)
{
    float u = ph / (2.0f * PI), s;
    u -= floorf(u);
    if (u < stance) {
        s = u / stance;
        *up = 0.0f;
        /* talon qui se pose au début, pointe qui pousse à la fin */
        *pitch = s < 0.15f ? -0.25f * (1.0f - s / 0.15f) : (s > 0.8f ? 0.45f * (s - 0.8f) / 0.2f : 0.0f);
        return half * (1.0f - 2.0f * s);
    }
    s = (u - stance) / (1.0f - stance);
    /* en course, le talon remonte vers les fesses au début de la phase aérienne, puis le
       genou passe devant */
    *up = lift * sinf(PI * s) + kick * sinf(PI * (s < 0.6f ? s / 0.6f : 1.0f)) * (1.0f - s);
    *pitch = 0.45f * (1.0f - s) + 0.6f * kick * (1.0f - s) - 0.25f * s * s;
    return half * (-1.0f + 2.0f * smoothstep(s * s * (1.0f + kick) / (1.0f + kick * s)));
}

#include "mocap_walk.inc"


/* longueur d'un cycle de marche (deux pas), continue entre marche et course. En marche, c'est
   celle du mouvement capturé (à notre longueur de jambe) : les pieds restent plantés. */
static float gait_cycle(float speed)
{
    float run = speed > WALK_SPEED ? (speed - WALK_SPEED) / (RUN_SPEED - WALK_SPEED) : 0.0f;
    float walk_len = mocap_walk_cycle_len * g_leg;
    if (run > 1.0f)
        run = 1.0f;
    return walk_len + (2.4f - walk_len) * run;
}

/*
 * Marche capturée (base CMU, voir mocap_walk.inc) : pose à la phase donnée, adaptée à nos
 * longueurs d'os (on garde les directions des os capturés), tournée vers le cap, posée sur le
 * relief. Ordre des points : celui de ragdoll.h.
 */
static float g_pose_here = 0.0f, g_pose_slope = 0.0f; /* sol sous le pilote et pente (stand_pose) */
static void mocap_walk_pose(const Walker *w, const Ragdoll *rd, float base, float out[RD_NUM_POINTS][3])
{
    static float min_ankle = -1.0f;
    float u = w->phase / (2.0f * PI), fs, a, f[3], r[3], up[3], loc[RD_NUM_POINTS][3], lift, hip_drop;
    int i0, i1, i, k;
    if (min_ankle < 0.0f) {
        min_ankle = 10.0f;
        for (i = 0; i < MOCAP_WALK_SAMPLES; ++i)
            for (k = 0; k < 2; ++k) {
                float y = mocap_walk[i][k == 0 ? RD_FOOT_L : RD_FOOT_R][1];
                if (y < min_ankle)
                    min_ankle = y;
            }
    }
    u -= floorf(u);
    fs = u * MOCAP_WALK_SAMPLES;
    i0 = (int)fs % MOCAP_WALK_SAMPLES;
    i1 = (i0 + 1) % MOCAP_WALK_SAMPLES;
    a = fs - floorf(fs);
    for (i = 0; i < RD_NUM_POINTS; ++i)
        for (k = 0; k < 3; ++k)
            loc[i][k] = (mocap_walk[i0][i][k] * (1.0f - a) + mocap_walk[i1][i][k] * a) * g_leg;
    /* nos bottes : la cheville la plus basse de la capture est remontée à notre hauteur */
    lift = w->ankle_height - min_ankle * g_leg;
    v_set(f, sinf(w->heading), 0.0f, cosf(w->heading));
    v_set(r, f[2], 0.0f, -f[0]);
    if (w->mirror) {
        r[0] = -r[0];
        r[2] = -r[2];
    }
    v_set(up, 0.0f, 1.0f, 0.0f);
#define TO_WORLD(dst, l)                                                                  \
    do {                                                                                  \
        int kk;                                                                           \
        for (kk = 0; kk < 3; ++kk)                                                        \
            (dst)[kk] = r[kk] * (l)[0] + up[kk] * (l)[1] + f[kk] * (l)[2];                \
    } while (0)
    /* bassin : dans notre squelette, les hanches sont à sa hauteur ; dans la capture, elles
       sont un peu plus bas. On place donc le bassin à la hauteur des hanches capturées. */
    hip_drop = loc[RD_PELVIS][1] - 0.5f * (loc[RD_HIP_L][1] + loc[RD_HIP_R][1]);
    {
        float p[3];
        TO_WORLD(p, loc[RD_PELVIS]);
        v_set(out[RD_PELVIS], w->pos[0] + p[0], base + lift + p[1] - hip_drop, w->pos[1] + p[2]);
    }
    /* chaînes : on garde la direction de chaque os capturé, avec notre longueur */
    {
        static const int chain[][3] = {
            { RD_PELVIS, RD_CHEST, 0 }, { RD_CHEST, RD_NECK, 1 }, { RD_NECK, RD_HEAD, 2 },
            { RD_NECK, RD_SHOULDER_L, 3 }, { RD_SHOULDER_L, RD_ELBOW_L, 4 }, { RD_ELBOW_L, RD_HAND_L, 5 },
            { RD_NECK, RD_SHOULDER_R, 3 }, { RD_SHOULDER_R, RD_ELBOW_R, 4 }, { RD_ELBOW_R, RD_HAND_R, 5 },
            { RD_PELVIS, RD_HIP_L, 6 }, { RD_PELVIS, RD_HIP_R, 6 },
        };
        float len[9];
        len[0] = w->spine;
        len[1] = w->neck;
        len[2] = w->head;
        len[3] = sqrtf(w->shoulder_half * w->shoulder_half + w->shoulder_drop * w->shoulder_drop);
        len[4] = w->upper_arm;
        len[5] = w->forearm;
        len[6] = w->hip_half;
        len[7] = w->thigh;
        len[8] = w->shin;
        for (i = 0; i < (int)(sizeof(chain) / sizeof(chain[0])); ++i) {
            float d[3], dw[3];
            int from = chain[i][0], to = chain[i][1];
            for (k = 0; k < 3; ++k)
                d[k] = loc[to][k] - loc[from][k];
            if (chain[i][2] == 6)
                d[1] -= hip_drop; /* nos hanches sont à la hauteur du point du bassin */
            v_norm(d);
            TO_WORLD(dw, d);
            v_madd(out[to], out[from], dw, len[chain[i][2]]);
        }
    }
    /* tête droite, regard devant (dans la capture, l'acteur penche un peu la tête) */
    {
        float hd[3];
        int kk;
        for (kk = 0; kk < 3; ++kk)
            hd[kk] = up[kk] + 0.12f * f[kk];
        v_norm(hd);
        v_madd(out[RD_HEAD], out[RD_NECK], hd, w->head);
    }
    /* pieds d'abord (trajectoire capturée exacte : ils restent plantés pendant l'appui),
       posés sur le relief ; si une jambe ne peut pas les atteindre, tout le haut du corps
       descend un peu (sinon le pied serait tiré et glisserait) */
    {
        float drop = 0.0f, reach = 0.995f * (w->thigh + w->shin);
        for (k = 0; k < 2; ++k) {
            int hip = k == 0 ? RD_HIP_L : RD_HIP_R, foot = k == 0 ? RD_FOOT_L : RD_FOOT_R;
            float fp[3], g, hx, hz, h2, v, need;
            TO_WORLD(fp, loc[foot]);
            out[foot][0] = w->pos[0] + fp[0];
            out[foot][2] = w->pos[1] + fp[2];
            g = walk_ground_height(rd, out[foot][0], out[foot][2]);
            {
                /* pied au-dessus d'un vide (bien plus bas que la pente) : le bassin ne plonge pas */
                float ahead = (out[foot][0] - w->pos[0]) * f[0] + (out[foot][2] - w->pos[1]) * f[2];
                float expect = g_pose_here + g_pose_slope * ahead;
                if (g < expect - 0.25f)
                    g = expect;
            }
            out[foot][1] = g + lift + loc[foot][1];
            if (out[foot][1] < g + w->ankle_height)
                out[foot][1] = g + w->ankle_height;
            hx = out[foot][0] - out[hip][0];
            hz = out[foot][2] - out[hip][2];
            h2 = hx * hx + hz * hz;
            v = out[hip][1] - out[foot][1];
            if (h2 < reach * reach) {
                need = v - sqrtf(reach * reach - h2);
                if (need > drop)
                    drop = need;
            }
        }
        if (drop > 0.0f)
            for (i = 0; i < RD_NUM_POINTS; ++i)
                if (i != RD_FOOT_L && i != RD_FOOT_R && i != RD_KNEE_L && i != RD_KNEE_R)
                    out[i][1] -= drop;
    }
    /* genoux : direction capturée du pliage */
    for (k = 0; k < 2; ++k) {
        int hip = k == 0 ? RD_HIP_L : RD_HIP_R, knee = k == 0 ? RD_KNEE_L : RD_KNEE_R;
        int foot = k == 0 ? RD_FOOT_L : RD_FOOT_R;
        float kn[3], hint[3];
        /* direction du genou capturé, par rapport au milieu hanche-pied */
        for (i = 0; i < 3; ++i)
            hint[i] = loc[knee][i] - 0.5f * (loc[hip][i] + loc[foot][i]);
        TO_WORLD(kn, hint);
        v_norm(kn);
        two_bone(out[hip], out[foot], w->thigh, w->shin, kn, out[knee]);
    }
#undef TO_WORLD
}

/* Saut, chute et réception (v0.40.47) : pose mélangée à la pose debout.
   En l'air : jambes repliées (une un peu devant), buste droit, bras levés devant en montant puis
   écartés pour l'équilibre en descendant. Réception (et élan avant le saut) : accroupi, genoux
   devant, buste penché, bras vers l'avant. */
static void air_land_pose(const Walker *w, float out[RD_NUM_POINTS][3], const float f[3], const float r[3])
{
    float wa = w->airw, wl = w->land, wsum = wa + wl, wmax = wa > wl ? wa : wl;
    float P[RD_NUM_POINTS][3], up[3] = { 0.0f, 1.0f, 0.0f }, tilt[3], lean, ua;
    int side, k;
    if (wsum < 0.001f)
        return;
    if (wmax > 1.0f)
        wmax = 1.0f;
    ua = wa / wsum; /* part de la pose en l'air */
    memcpy(P, out, sizeof(P));
    lean = ua * 0.12f + (1.0f - ua) * 0.30f;
    v_madd(tilt, up, f, lean);
    v_norm(tilt);
    v_madd(P[RD_CHEST], P[RD_PELVIS], tilt, w->spine);
    v_madd(P[RD_NECK], P[RD_CHEST], tilt, w->neck);
    {
        float hd[3];
        v_madd(hd, up, tilt, 0.4f);
        v_norm(hd);
        v_madd(P[RD_HEAD], P[RD_NECK], hd, w->head);
    }
    for (side = 0; side < 2; ++side) {
        float sign = side == 0 ? -1.0f : 1.0f;
        int hip = side == 0 ? RD_HIP_L : RD_HIP_R, knee = side == 0 ? RD_KNEE_L : RD_KNEE_R;
        int foot = side == 0 ? RD_FOOT_L : RD_FOOT_R, shoulder = side == 0 ? RD_SHOULDER_L : RD_SHOULDER_R;
        int elbow = side == 0 ? RD_ELBOW_L : RD_ELBOW_R, hand = side == 0 ? RD_HAND_L : RD_HAND_R;
        float th[3], sh[3], ka[3], fa[3], kh[3], ft[3], ua_[3], fo[3], el[3], hd[3];
        /* v0.40.48 : membres posés directement (pas d'IK) : en montant, cuisses levées devant et
           tibias repliés dessous ; en descendant, jambes qui se déplient pour se poser */
        float rise = w->risew, lead = side == 0 ? 1.0f : 0.75f;
        v_set(th, 0.0f, -(0.55f + 0.30f * (1.0f - rise)), 0.0f);
        v_madd(th, th, f, (0.80f - 0.35f * (1.0f - rise)) * lead);
        v_madd(th, th, r, sign * 0.08f);
        v_norm(th);
        v_set(sh, 0.0f, -1.0f, 0.0f);
        v_madd(sh, sh, f, -(0.55f - 0.35f * (1.0f - rise)));
        v_norm(sh);
        v_madd(ka, P[hip], th, w->thigh);
        v_madd(fa, ka, sh, w->shin);
        /* réception : pied au sol (pose debout, bassin abaissé), genou devant */
        /* pieds côte à côte sous les hanches (pas de pas en avant pendant l'élan ou la réception) */
        ft[0] = P[hip][0] + f[0] * (side == 0 ? 0.08f : -0.02f) + r[0] * sign * 0.05f;
        ft[2] = P[hip][2] + f[2] * (side == 0 ? 0.08f : -0.02f) + r[2] * sign * 0.05f;
        ft[1] = out[foot][1] < out[foot == RD_FOOT_L ? RD_FOOT_R : RD_FOOT_L][1] ? out[foot][1]
                                                                               : out[foot == RD_FOOT_L ? RD_FOOT_R : RD_FOOT_L][1];
        v_madd(kh, f, r, sign * 0.20f);
        v_norm(kh);
        {
            float kl[3];
            two_bone(P[hip], ft, w->thigh, w->shin, kh, kl);
            for (k = 0; k < 3; ++k) {
                P[knee][k] = ka[k] * ua + kl[k] * (1.0f - ua);
                P[foot][k] = fa[k] * ua + ft[k] * (1.0f - ua);
            }
        }
        /* bras : bras qui descend un peu devant et sur le côté, coude plié, avant-bras devant
           (en montant) ; écartés pour l'équilibre (en descendant) ; devant vers le bas (réception) */
        v_madd(P[shoulder], P[RD_NECK], r, sign * w->shoulder_half);
        P[shoulder][1] -= w->shoulder_drop;
        {
            float u1[3], f1[3], u2[3], f2[3], u3[3], f3[3];
            v_set(u1, 0.0f, -0.55f, 0.0f); v_madd(u1, u1, f, 0.55f); v_madd(u1, u1, r, sign * 0.30f);
            v_set(f1, 0.0f, 0.35f, 0.0f); v_madd(f1, f1, f, 0.85f); v_madd(f1, f1, r, sign * 0.05f);
            v_set(u2, 0.0f, -0.85f, 0.0f); v_madd(u2, u2, r, sign * 0.45f); v_madd(u2, u2, f, 0.25f);
            v_set(f2, 0.0f, -0.30f, 0.0f); v_madd(f2, f2, r, sign * 0.25f); v_madd(f2, f2, f, 0.80f);
            v_set(u3, 0.0f, -0.80f, 0.0f); v_madd(u3, u3, f, 0.50f); v_madd(u3, u3, r, sign * 0.25f);
            v_set(f3, 0.0f, -0.10f, 0.0f); v_madd(f3, f3, f, 0.95f); v_madd(f3, f3, r, -sign * 0.10f);
            for (k = 0; k < 3; ++k) {
                float air_u = u1[k] * rise + u2[k] * (1.0f - rise), air_f = f1[k] * rise + f2[k] * (1.0f - rise);
                ua_[k] = air_u * ua + u3[k] * (1.0f - ua);
                fo[k] = air_f * ua + f3[k] * (1.0f - ua);
            }
            v_norm(ua_);
            v_norm(fo);
        }
        v_madd(el, P[shoulder], ua_, w->upper_arm);
        v_madd(hd, el, fo, w->forearm);
        memcpy(P[elbow], el, sizeof(el));
        memcpy(P[hand], hd, sizeof(hd));
    }
    {
        int i;
        for (i = 0; i < RD_NUM_POINTS; ++i)
            for (k = 0; k < 3; ++k)
                out[i][k] += (P[i][k] - out[i][k]) * wmax;
    }
}

static void stand_pose(const Walker *w, const Ragdoll *rd, float out[RD_NUM_POINTS][3])
{
    float f[3], r[3], up[3], tilt[3];
    float s = fabsf(w->speed) / WALK_SPEED; /* 0 = immobile, 1 = marche, >1 = course */
    float amp = s > 1.0f ? 1.0f : s;
    float run = w->speed > WALK_SPEED ? (w->speed - WALK_SPEED) / (RUN_SPEED - WALK_SPEED) : 0.0f;
    float base, base_here, slope = 0.0f, pelvis_y, bob;
    float cycle, stance, half, lift, offs[2], ups[2], pitches[2];
    float dir = w->speed >= 0.0f ? 1.0f : -1.0f;
    int side;

    if (run > 1.0f)
        run = 1.0f;
    /* longueur d'un cycle (deux pas) et part d'appui : le pied posé ne glisse pas */
    cycle = gait_cycle(fabsf(w->speed));
    stance = 0.62f - 0.27f * run;          /* en course : phase sans appui */
    v_set(f, sinf(w->heading), 0.0f, cosf(w->heading));
    v_set(r, f[2], 0.0f, -f[0]); /* droite */
    /* v0.40.51 : pente sous le pilote dans le sens de la marche (bosses) */
    base_here = walk_ground_height(rd, w->pos[0], w->pos[1]);
    {
        float ga = walk_ground_height(rd, w->pos[0] + f[0] * 0.25f, w->pos[1] + f[2] * 0.25f);
        float gb = walk_ground_height(rd, w->pos[0] - f[0] * 0.25f, w->pos[1] - f[2] * 0.25f);
        /* un côté au-dessus d'un vide : pas une pente */
        if (fabsf(ga - base_here) > 0.30f)
            ga = base_here;
        if (fabsf(gb - base_here) > 0.30f)
            gb = base_here;
        slope = (ga - gb) / 0.5f;
    }
    /* pas plus courts dans une pente raide : le pied de derrière ne reste pas en l'air */
    half = 0.5f * stance * cycle * amp / (1.0f + 0.9f * fabsf(slope));
    g_pose_here = base_here;
    g_pose_slope = slope;
    lift = (0.09f + 0.14f * run) * amp;
    if (w->mirror) {
        r[0] = -r[0];
        r[2] = -r[2];
    }
    v_set(up, 0.0f, 1.0f, 0.0f);
    for (side = 0; side < 2; ++side)
        offs[side] = dir * gait_foot(w->phase + (side == 0 ? 0.0f : PI), half, lift, stance, &ups[side],
                                     &pitches[side], 0.30f * run * amp);

    base = base_here;
    {
        /* sur une pente, le bassin descend au niveau du pied le plus bas */
        int sd;
        for (sd = 0; sd < 2; ++sd) {
            float sgn = sd == 0 ? -1.0f : 1.0f;
            float g = walk_ground_height(rd, w->pos[0] + f[0] * offs[sd] + r[0] * sgn * w->hip_half,
                                         w->pos[1] + f[2] * offs[sd] + r[2] * sgn * w->hip_half);
            /* v0.40.51 : un vide = bien plus bas que ce que donne la pente (bord d'une marche) */
            if (g < base && g > base_here + slope * offs[sd] - 0.25f)
                base = g;
        }
        /* bassin entre le sol sous lui et le pied le plus bas (avant : tout en bas, la jambe
           de derrière se repliait très haut dans les bosses) */
        base = base + 0.35f * (base_here - base);
        /* v0.40.50 : un pied au-dessus d'un vide (bord d'une marche, du ponton) ne fait pas
           tomber le bassin d'un coup : 25 cm au plus sous le sol du pilote (la chute, si le
           pilote avance dans le vide, est gérée par le saut / la chute) */
    }
    /* le bassin est au plus bas quand les deux pieds sont au sol (pas écartés) */
    bob = 0.03f * amp * (cosf(2.0f * w->phase) - 1.0f) * 0.5f + 0.035f * run * cosf(2.0f * w->phase);
    pelvis_y = base + w->ankle_height + w->hip_height - 0.035f * amp - 0.06f * run + bob;
    pelvis_y -= w->crouch * w->grab;
    pelvis_y += w->rise;
    pelvis_y -= w->dip; /* jambes pliées : en l'air et à la réception d'un saut ou d'une chute */
    pelvis_y -= 0.42f * w->hip_height * w->squat; /* v1.8.170 : accroupi */
    if (w->sit > 0.0f)
        pelvis_y += (w->sit_y - pelvis_y) * (w->sit > 1.0f ? 1.0f : w->sit);

    v_set(out[RD_PELVIS], w->pos[0], pelvis_y, w->pos[1]);
    /* le bassin se balance un peu vers la jambe d'appui */
    v_madd(out[RD_PELVIS], out[RD_PELVIS], r, 0.025f * amp * sinf(w->phase));
    {
        /* à l'arrêt : il ne reste pas figé, le poids passe lentement d'une jambe à l'autre
           et il respire */
        float idle = (1.0f - amp) * (1.0f - w->grab);
        v_madd(out[RD_PELVIS], out[RD_PELVIS], r, 0.025f * idle * sinf(0.9f * w->t));
        out[RD_PELVIS][1] -= 0.012f * idle * (1.0f + sinf(0.9f * w->t + 1.2f)) * 0.5f;
    }

    /* buste : penché vers l'avant quand on avance, plus en courant */
    v_madd(tilt, up, f, w->stand_lean /* debout : buste un peu en arrière (réglable) */ + dir * (0.05f * amp + 0.30f * run) +
                            0.55f * w->grab - 0.25f * w->crouch * w->grab / 0.45f + w->lean +
                            0.40f * w->squat /* v1.8.170 : buste penché, accroupi */);
    v_norm(tilt);
    v_madd(out[RD_CHEST], out[RD_PELVIS], tilt, w->spine);
    out[RD_CHEST][1] += 0.006f * (1.0f - amp) * sinf(2.4f * w->t); /* respiration */
    v_madd(out[RD_NECK], out[RD_CHEST], tilt, w->neck);
    /* la tête reste droite : elle regarde devant */
    v_madd(out[RD_HEAD], out[RD_NECK], up, w->head * 0.7f);
    v_madd(out[RD_HEAD], out[RD_HEAD], tilt, w->head * 0.3f);
    {
        float hd[3];
        int k;
        for (k = 0; k < 3; ++k)
            hd[k] = out[RD_HEAD][k] - out[RD_NECK][k];
        v_norm(hd);
        v_madd(out[RD_HEAD], out[RD_NECK], hd, w->head);
    }

    for (side = 0; side < 2; ++side) {
        float sign = side == 0 ? -1.0f : 1.0f; /* gauche = -r */
        int hip = side == 0 ? RD_HIP_L : RD_HIP_R;
        int knee = side == 0 ? RD_KNEE_L : RD_KNEE_R;
        int foot = side == 0 ? RD_FOOT_L : RD_FOOT_R;
        int shoulder = side == 0 ? RD_SHOULDER_L : RD_SHOULDER_R;
        int elbow = side == 0 ? RD_ELBOW_L : RD_ELBOW_R;
        int hand = side == 0 ? RD_HAND_L : RD_HAND_R;
        float fwd = offs[side];
        float fp[3], gy, arm[3], fore[3], knee_hint[3];
        /* bras opposé à la jambe : il avance quand la jambe de l'autre côté avance (courbe
           douce, en phase avec le pas) */
        float other = cosf(w->phase + (side == 0 ? PI : 0.0f));
        float swing = (0.42f * amp + 0.38f * run) * other * dir;

        /* jambe : la hanche avance un peu avec la jambe (rotation du bassin) */
        v_madd(out[hip], out[RD_PELVIS], r, sign * w->hip_half);
        v_madd(out[hip], out[hip], f, 0.12f * fwd);
        v_madd(fp, out[RD_PELVIS], r, sign * (w->hip_half * 0.85f));
        v_madd(fp, fp, f, fwd);
        if (w->crouch > 0.0f && w->grab > 0.0f) {
            /* accroupi pour relever : pieds écartés et décalés, dos plus droit */
            float c = w->crouch * w->grab / 0.45f;
            v_madd(fp, fp, r, sign * 0.12f * c);
            v_madd(fp, fp, f, (side == 0 ? 0.14f : -0.10f) * c);
        }
        if (w->squat > 0.0f)
            v_madd(fp, fp, r, sign * 0.10f * w->squat); /* v1.8.170 : accroupi : pieds écartés */
        if (w->sit > 0.0f && !(w->leg_lift > 0.0f && side == w->leg_side)) {
            /* assis sur la moto : le pied d'appui s'écarte pour se poser à côté de la moto */
            v_madd(fp, fp, r, sign * 0.30f * (w->sit > 1.0f ? 1.0f : w->sit));
            v_madd(fp, fp, f, 0.10f * (w->sit > 1.0f ? 1.0f : w->sit));
        }
        gy = walk_ground_height(rd, fp[0], fp[2]);
        {
            /* pied au-dessus d'un vide (bien plus bas que la pente) : au niveau du sol du pilote */
            float ahead = (fp[0] - w->pos[0]) * f[0] + (fp[2] - w->pos[1]) * f[2], expect = base_here + slope * ahead;
            if (gy < expect - 0.25f)
                gy = expect;
        }
        fp[1] = gy + w->ankle_height + ups[side];
        /* v0.40.51 : le pied ne monte jamais à hauteur de hanche (en descente, en courant, le
           pied de derrière passait au-dessus et le genou se retournait sur le côté) */
        if (fp[1] > out[hip][1] - 0.35f + 0.15f * w->squat)
            fp[1] = out[hip][1] - 0.35f + 0.15f * w->squat;
        memcpy(out[foot], fp, sizeof(fp));
        /* genou vers l'avant, un peu vers l'extérieur */
        memcpy(knee_hint, f, sizeof(knee_hint));
        v_madd(knee_hint, knee_hint, r, sign * (0.15f + 0.30f * w->squat)); /* v1.8.170 : accroupi : genoux ouverts */
        v_norm(knee_hint);
        two_bone(out[hip], out[foot], w->thigh, w->shin, knee_hint, out[knee]);
        gy = walk_ground_height(rd, out[foot][0], out[foot][2]);
        {
            float ahead = (out[foot][0] - w->pos[0]) * f[0] + (out[foot][2] - w->pos[1]) * f[2], expect = base_here + slope * ahead;
            if (gy < expect - 0.25f)
                gy = expect;
        }
        gy += w->ankle_height;
        if (out[foot][1] < gy) {
            out[foot][1] = gy;
            two_bone(out[hip], out[foot], w->thigh, w->shin, knee_hint, out[knee]);
        }
        /* v0.40.51 : pied parallèle à la pente (pointe relevée en montée) */
        w_foot_pitch[side] = pitches[side] - atanf(slope);

        /* bras : épaules qui tournent à l'opposé du bassin, bras qui se balance depuis
           l'épaule, coude plus plié quand le bras passe devant, et plié à 90° en courant */
        v_madd(out[shoulder], out[RD_NECK], r, sign * w->shoulder_half);
        out[shoulder][1] -= w->shoulder_drop;
        v_madd(out[shoulder], out[shoulder], f, 0.05f * swing);
        v_set(arm, 0.0f, -cosf(swing), 0.0f);
        v_madd(arm, arm, f, sinf(swing));
        v_madd(arm, arm, r, sign * (0.12f + 0.06f * run));
        v_norm(arm);
        v_madd(out[elbow], out[shoulder], arm, w->upper_arm);
        {
            /* pliage du coude : angle entre le bras et l'avant-bras, vers l'avant */
            float bend = 0.30f + 0.25f * amp + 0.35f * amp * (swing > 0.0f ? swing : 0.0f) + 1.05f * run;
            float perp[3], d = arm[0] * f[0] + arm[1] * f[1] + arm[2] * f[2];
            int k;
            for (k = 0; k < 3; ++k)
                perp[k] = f[k] - arm[k] * d;
            v_norm(perp);
            for (k = 0; k < 3; ++k)
                fore[k] = arm[k] * cosf(bend) + perp[k] * sinf(bend);
            /* en courant, les mains reviennent un peu vers le milieu du corps */
            v_madd(fore, fore, r, -sign * (0.06f + 0.18f * run));
            v_norm(fore);
        }
        v_madd(out[hand], out[elbow], fore, w->forearm);

        /* mains au guidon : on mélange avec la pose libre, coude vers l'extérieur et le bas */
        if (w->grab > 0.0f || w->carry > 0.0f) {
            float hand_t[3], hint[3], el[3];
            int k;
            float ahead, gw = w->grab > w->carry ? w->grab : w->carry;
            for (k = 0; k < 3; ++k)
                hand_t[k] = out[hand][k] + (w->grab_hand[side][k] - out[hand][k]) * gw;
            /* les mains restent devant le buste : un bras ne traverse jamais le corps */
            ahead = (hand_t[0] - out[RD_CHEST][0]) * f[0] + (hand_t[2] - out[RD_CHEST][2]) * f[2];
            if (ahead < 0.30f)
                v_madd(hand_t, hand_t, f, 0.30f - ahead);
            /* coude naturel : vers le bas, un peu vers l'extérieur et vers l'arrière */
            /* v0.40.73 : coudes vers l'extérieur, un peu levés (position de pilote) ; avant
               vers le bas : coudes trop bas en montant sur la moto */
            v_set(hint, 0.0f, -0.30f, 0.0f);
            v_madd(hint, hint, r, sign * 0.90f);
            v_madd(hint, hint, f, -0.30f);
            v_norm(hint);
            two_bone(out[shoulder], hand_t, w->upper_arm, w->forearm, hint, el);
            memcpy(out[elbow], el, sizeof(el));
            memcpy(out[hand], hand_t, sizeof(hand_t));
        }
        /* enjamber : le pied passe au-dessus de la selle */
        if (w->leg_lift > 0.0f && side == w->leg_side) {
            /* comme un vrai pilote : le talon remonte vers l'arrière (genou plié), le pied passe
               bien au-dessus de l'arrière de la selle, puis redescend de l'autre côté
               (courbe de Bézier cubique : départ, talon en haut derrière, au-dessus de la selle,
               arrivée) */
            float u = w->leg_lift > 1.0f ? 1.0f : w->leg_lift, p1[3], p2[3], ft[3], kh[3];
            float b0 = (1 - u) * (1 - u) * (1 - u), b1 = 3 * u * (1 - u) * (1 - u), b2 = 3 * u * u * (1 - u), b3 = u * u * u;
            float clear = w->leg_clear > 0.0f ? w->leg_clear : base + 1.0f;
            int k;
            {
                /* direction de la moto (du pied de départ vers l'arrivée, à plat) */
                float side_d[3] = { w->leg_target[0] - out[foot][0], 0.0f, w->leg_target[2] - out[foot][2] };
                v_norm(side_d);
                /* la jambe monte en arrière ET vers la moto (cuisse écartée, genou plié), passe
                   au-dessus de l'arrière de la selle, puis redescend de l'autre côté */
                memcpy(p1, out[foot], sizeof(p1));
                /* jambe TENDUE : le pied tourne autour de la hanche (rayon ~ longueur de la jambe),
                   d'abord vers l'arrière et le haut, puis au-dessus de l'arrière de la selle et
                   redescend de l'autre côté */
                float d0[3], d1[3], d2[3], d3[3], dir[3], r0, r3, rr, L = 0.97f * (w->thigh + w->shin);
                for (k = 0; k < 3; ++k) {
                    d0[k] = out[foot][k] - out[hip][k];
                    d3[k] = w->leg_target[k] - out[hip][k];
                }
                r0 = sqrtf(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]);
                r3 = sqrtf(d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2]);
                v_norm(d0);
                v_norm(d3);
                /* v0.40.57 : la jambe passe en arc bas autour de l'arrière de la moto (juste
                   au-dessus du garde-boue), pas en grand coup de pied vers le haut */
                v_set(d1, 0.0f, 0.30f, 0.0f);
                v_madd(d1, d1, f, -0.85f);
                v_madd(d1, d1, side_d, 0.05f);
                v_norm(d1);
                v_set(d2, 0.0f, 0.22f, 0.0f);
                v_madd(d2, d2, f, -0.55f);
                v_madd(d2, d2, side_d, 0.80f);
                v_norm(d2);
                {
                    /* la jambe reste longtemps en haut : poids du milieu en racine de sin */
                    /* le pic est avant le milieu : une fois au-dessus de la selle, le pied redescend
                       directement vers le sol de l'autre côté (pas de jambe tendue sur le côté) */
                    float um = u / 0.8f > 1.0f ? 1.0f : u / 0.8f, sm = sinf(PI * um), wm = sm > 0.0f ? powf(sm, 0.75f) : 0.0f;
                    for (k = 0; k < 3; ++k)
                        dir[k] = (1.0f - wm) * ((1.0f - u) * d0[k] + u * d3[k]) + wm * ((1.0f - u) * d1[k] + u * d2[k]);
                }
                v_norm(dir);
                /* rayon : jambe presque tendue au milieu, distances réelles au départ et à l'arrivée */
                rr = (1.0f - u) * r0 + u * r3;
                rr += (L - rr) * (b1 + b2);
                if (rr > L)
                    rr = L;
                for (k = 0; k < 3; ++k)
                    ft[k] = out[hip][k] + dir[k] * rr;
                {
                    /* le pied doit passer au-dessus de la selle */
                    /* v0.40.57 : tant que le pied est au-dessus de la moto (entre le pied de
                       départ et l'arrivée, près du milieu), il reste plus haut que la selle */
                    /* milieu de la moto : le pied d'arrivée est posé à 0,40 m sur le côté */
                    float need = clear + 0.10f, mid[2], lat_d, wq;
                    mid[0] = w->leg_target[0] - side_d[0] * 0.40f;
                    mid[1] = w->leg_target[2] - side_d[2] * 0.40f;
                    lat_d = fabsf((ft[0] - mid[0]) * side_d[0] + (ft[2] - mid[1]) * side_d[2]);
                    wq = 1.0f - (lat_d - 0.14f) / 0.12f;
                    wq = wq < 0.0f ? 0.0f : wq > 1.0f ? 1.0f : wq;
                    if (ft[1] < need)
                        ft[1] += (need - ft[1]) * wq * sinf(PI * u);
                }
                (void)p1;
                (void)p2;
                memcpy(out[foot], ft, sizeof(ft));
                /* genou : vers le haut, l'arrière et la moto pendant le passage (il passe au-dessus
                   de la selle avant le pied), vers l'avant en se posant */
                /* genou vers le bas pendant le passage : la jambe reste presque tendue */
                v_set(kh, 0.0f, -0.6f * (b1 + b2), 0.0f);
                v_madd(kh, kh, f, 1.0f * b3 + 0.4f * b0 + 0.2f * (b1 + b2));
                v_madd(kh, kh, side_d, 0.2f * (b1 + b2));
                v_norm(kh);
            }
            two_bone(out[hip], out[foot], w->thigh, w->shin, kh, out[knee]);
        }
        /* assis : les pieds montent sur les repose-pieds (genoux devant, un peu écartés) */
        if (w->pegs > 0.0f) {
            float u = w->pegs > 1.0f ? 1.0f : w->pegs, ft[3], kh[3];
            int k;
            for (k = 0; k < 3; ++k)
                ft[k] = out[foot][k] + (w->peg[side][k] - out[foot][k]) * u;
            ft[1] += 0.10f * sinf(PI * u); /* le pied se lève avant de se poser */
            memcpy(out[foot], ft, sizeof(ft));
            v_set(kh, 0.0f, 0.3f, 0.0f);
            v_madd(kh, kh, f, 1.0f);
            v_madd(kh, kh, r, sign * 0.35f);
            v_norm(kh);
            two_bone(out[hip], out[foot], w->thigh, w->shin, kh, out[knee]);
        }
    }
    /* marche capturée : on la mélange à la pose calculée selon la vitesse (pleine en marche,
       remplacée par la course calculée en accélérant, et jamais pendant un geste avec la moto) */
    {
        float wgt = amp * (1.0f - run) * (1.0f - w->grab) * (1.0f - w->squat) * (w->leg_lift > 0.0f ? 0.0f : 1.0f) *
                    (1.0f - (w->airw > w->land ? w->airw : w->land));
        if (wgt > 0.001f && w->speed > 0.0f) {
            float mo[RD_NUM_POINTS][3];
            int i, k;
            mocap_walk_pose(w, rd, base, mo);
            for (i = 0; i < RD_NUM_POINTS; ++i)
                for (k = 0; k < 3; ++k)
                    out[i][k] += (mo[i][k] - out[i][k]) * wgt;
        }
    }
    air_land_pose(w, out, f, r);
}

/* Pose à genou (étape du relevé) : genou gauche au sol, pied droit devant, main droite
 * sur le genou droit, main gauche en appui sur la cuisse */
static void kneel_pose(const Walker *w, const Ragdoll *rd, float out[RD_NUM_POINTS][3])
{
    float f[3], r[3], up[3], tilt[3], base;
    int side;
    v_set(f, sinf(w->heading), 0.0f, cosf(w->heading));
    v_set(r, f[2], 0.0f, -f[0]);
    if (w->mirror) {
        r[0] = -r[0];
        r[2] = -r[2];
    }
    v_set(up, 0.0f, 1.0f, 0.0f);
    base = walk_ground_height(rd, w->pos[0], w->pos[1]);
    v_set(out[RD_PELVIS], w->pos[0], base + 0.62f * (w->thigh + w->shin), w->pos[1]);
    v_madd(out[RD_PELVIS], out[RD_PELVIS], f, -0.05f);
    v_madd(tilt, up, f, 0.35f);
    v_norm(tilt);
    v_madd(out[RD_CHEST], out[RD_PELVIS], tilt, w->spine);
    v_madd(out[RD_NECK], out[RD_CHEST], tilt, w->neck);
    v_madd(out[RD_HEAD], out[RD_NECK], up, w->head);
    for (side = 0; side < 2; ++side) {
        float sign = side == 0 ? -1.0f : 1.0f;
        int hip = side == 0 ? RD_HIP_L : RD_HIP_R;
        int knee = side == 0 ? RD_KNEE_L : RD_KNEE_R;
        int foot = side == 0 ? RD_FOOT_L : RD_FOOT_R;
        int shoulder = side == 0 ? RD_SHOULDER_L : RD_SHOULDER_R;
        int elbow = side == 0 ? RD_ELBOW_L : RD_ELBOW_R;
        int hand = side == 0 ? RD_HAND_L : RD_HAND_R;
        float hint[3], g;
        v_madd(out[hip], out[RD_PELVIS], r, sign * w->hip_half);
        if (side == 0) {
            /* genou gauche au sol sous la hanche, pied derrière, pointe au sol */
            float kd[3];
            v_madd(out[knee], out[hip], f, 0.12f);
            g = walk_ground_height(rd, out[knee][0], out[knee][2]);
            out[knee][1] = g + 0.07f;
            v_set(kd, out[knee][0] - out[hip][0], out[knee][1] - out[hip][1], out[knee][2] - out[hip][2]);
            v_norm(kd);
            v_madd(out[knee], out[hip], kd, w->thigh);
            v_madd(out[foot], out[knee], f, -w->shin * 0.97f);
            g = walk_ground_height(rd, out[foot][0], out[foot][2]);
            out[foot][1] = g + 0.10f;
            {
                float fd[3] = { out[foot][0] - out[knee][0], out[foot][1] - out[knee][1], out[foot][2] - out[knee][2] };
                v_norm(fd);
                v_madd(out[foot], out[knee], fd, w->shin);
            }
        } else {
            /* pied droit à plat devant, genou plié vers le haut */
            v_madd(out[foot], out[hip], f, 0.42f);
            g = walk_ground_height(rd, out[foot][0], out[foot][2]);
            out[foot][1] = g + w->ankle_height;
            v_madd(hint, up, f, 1.0f);
            v_norm(hint);
            two_bone(out[hip], out[foot], w->thigh, w->shin, hint, out[knee]);
        }
        v_madd(out[shoulder], out[RD_NECK], r, sign * w->shoulder_half);
        out[shoulder][1] -= w->shoulder_drop;
        /* les mains en appui : sur le genou droit, et sur la cuisse gauche */
        {
            float tgt[3];
            if (side == 1)
                v_madd(tgt, out[RD_KNEE_R], up, 0.06f);
            else {
                int k;
                for (k = 0; k < 3; ++k)
                    tgt[k] = 0.5f * (out[RD_HIP_L][k] + out[RD_KNEE_L][k]);
                v_madd(tgt, tgt, up, 0.08f);
            }
            v_set(hint, 0.0f, -0.4f, 0.0f);
            v_madd(hint, hint, r, sign * 0.7f);
            v_madd(hint, hint, f, -0.3f);
            v_norm(hint);
            memcpy(out[hand], tgt, sizeof(tgt));
            two_bone(out[shoulder], out[hand], w->upper_arm, w->forearm, hint, out[elbow]);
        }
    }
}

void walk_update(Walker *w, Ragdoll *rd, const WalkInput *in, float dt)
{
    float target[RD_NUM_POINTS][3];
    int i, k;

    if (w->state == WALK_OFF || dt <= 0.0f) {
        if (w->state == WALK_OFF)
            g_air_on = 0;
        return;
    }
    if (dt > 0.05f)
        dt = 0.05f;
    w->t += dt;
    g_ref_ok = w->support_ok;
    g_ref_y = w->support;
    g_air_on = 0;

    if (w->state == WALK_ON) {
        float want = 0.0f, accel;
        if (in->steer) {
            /* le pilote se tourne vers la direction demandée, puis avance */
            float d = in->steer_heading + (in->steer_back ? PI : 0.0f) - w->heading, step = 7.0f * dt;
            while (d > PI)
                d -= 2.0f * PI;
            while (d < -PI)
                d += 2.0f * PI;
            w->heading += d > step ? step : (d < -step ? -step : d);
            want = in->steer_back ? -(in->run ? BACK_SPEED * 1.7f : BACK_SPEED) : (in->run ? RUN_SPEED : WALK_SPEED);
            if (fabsf(d) > 1.6f)
                want *= 0.3f; /* demi-tour : il ralentit en tournant */
            if (fabsf(d) > 0.2f && fabsf(w->speed) < 0.3f)
                w->phase += 3.0f * dt;
        } else if (in->forward)
            want = in->run ? RUN_SPEED : WALK_SPEED;
        else if (in->back)
            want = -BACK_SPEED;
        if (in->left && !in->steer)
            w->heading -= TURN_RATE * dt;
        if (in->right && !in->steer)
            w->heading += TURN_RATE * dt;
        {
            /* v1.8.170 : accroupi : on se redresse en l'air, assis ou en tenant la moto ; sinon pas lents */
            float tgt = (in->crouch && !w->air && w->sit <= 0.0f && w->grab < 0.01f && w->leg_lift <= 0.0f) ? 1.0f : 0.0f;
            smooth_to(&w->squat, &w->squat_v, tgt, 9.0f, dt);
            if (w->squat < 0.0f)
                w->squat = 0.0f;
            if (w->squat > 1.0f)
                w->squat = 1.0f;
            if (w->squat > 0.05f) {
                float lim = CROUCH_SPEED + (WALK_SPEED - CROUCH_SPEED) * (1.0f - w->squat);
                if (want > lim)
                    want = lim;
                if (want < -lim)
                    want = -lim;
            }
        }
        accel = 4.0f * dt;
        if (w->speed < want)
            w->speed = w->speed + accel > want ? want : w->speed + accel;
        else
            w->speed = w->speed - accel < want ? want : w->speed - accel;

        {
            /* objets de la piste (barrières, murs, bords trop hauts) : on bute, on glisse le long */
            float nx = w->pos[0] + sinf(w->heading) * w->speed * dt;
            float nz = w->pos[1] + cosf(w->heading) * w->speed * dt;
            if (g_block_fn) {
                float feet = w->support_ok ? w->support : walk_ground_height(rd, w->pos[0], w->pos[1]);
                /* déjà dans un objet (descendu de la moto contre une barrière...) : on le laisse
                   sortir, sinon il resterait coincé */
                if (!g_block_fn(w->pos[0], w->pos[1], feet, 0.28f) && g_block_fn(nx, nz, feet, 0.28f)) {
                    if (!g_block_fn(nx, w->pos[1], feet, 0.28f))
                        nz = w->pos[1];
                    else if (!g_block_fn(w->pos[0], nz, feet, 0.28f))
                        nx = w->pos[0];
                    else {
                        nx = w->pos[0];
                        nz = w->pos[1];
                        w->speed *= 0.5f;
                    }
                }
            }
            w->pos[0] = nx;
            w->pos[1] = nz;
        }

        /* saut et chute d'une hauteur (v0.40.46, animation v0.40.47) */
        {
            int jump = in->jump && !w->jump_prev && w->squat < 0.5f;
            float g;
            w->jump_prev = in->jump;
            if (!w->air && w->prejump <= 0.0f && w->support_ok) {
                g = ground_below(rd, w->pos[0], w->pos[1], w->support + STEP_UP);
                if (jump && w->grab < 0.01f && w->sit <= 0.0f && w->leg_lift <= 0.0f) {
                    w->prejump = PREJUMP_TIME; /* élan : il s'accroupit un instant */
                } else if (g < w->support - FALL_START) {
                    w->air = 1; /* le sol se dérobe (bord du ponton...) : il tombe */
                    w->air_y = w->support;
                    w->vy = 0.0f;
                    w->air_t = 0.0f;
                    w->air_jump = 0;
                }
            }
            if (w->prejump > 0.0f) {
                w->prejump -= dt;
                w->land_tgt = 1.0f;
                if (w->prejump > PUSH_TIME) {
                    w->dip_tgt = 0.17f; /* il s'accroupit */
                    w->push_dip = 0.0f;
                } else {
                    /* poussée : les jambes se tendent, le bassin monte de plus en plus vite
                       (vitesse continue avec celle du saut : pas d'à-coup au décollage) */
                    float u, vpush;
                    if (w->push_dip <= 0.0f)
                        w->push_dip = w->dip > 0.05f ? w->dip : 0.05f; /* début de la poussée */
                    u = 1.0f - w->prejump / PUSH_TIME;
                    if (u > 1.0f)
                        u = 1.0f;
                    vpush = 2.0f * w->push_dip / PUSH_TIME * u; /* le bassin remonte de push_dip */
                    w->dip -= vpush * dt;
                    if (w->dip < 0.0f)
                        w->dip = 0.0f;
                    w->dip_v = -vpush;
                    w->dip_tgt = 0.0f;
                    w->land_tgt = 0.3f;
                }
                if (w->prejump <= 0.0f) {
                    w->prejump = 0.0f;
                    w->air = 1;
                    w->air_y = w->support;
                    /* vitesse de décollage = celle de la poussée (continue) */
                    w->vy = 2.0f * w->push_dip / PUSH_TIME;
                    if (w->vy > JUMP_SPEED)
                        w->vy = JUMP_SPEED;
                    if (w->vy < 2.4f)
                        w->vy = 2.4f;
                    w->push_dip = 0.0f;
                    w->air_t = 0.0f;
                    w->air_jump = 1;
                }
            }
            if (w->air) {
                w->vy -= 9.81f * dt;
                if (w->vy < -25.0f)
                    w->vy = -25.0f;
                w->air_y += w->vy * dt;
                w->air_t += dt;
                /* chute d'une marche (escalier) : pas de pose en l'air, seulement si elle dure */
                w->air_tgt = w->air_jump ? 1.0f : smoothstep((w->air_t - 0.18f) / 0.25f);
                w->land_tgt = 0.0f;
                w->dip_tgt = 0.0f;
                g = ground_below(rd, w->pos[0], w->pos[1], w->air_y + 0.05f);
                if (w->vy <= 0.0f && w->air_y <= g) {
                    /* réception : les jambes amortissent, plus fort si la chute est haute */
                    float impact = -w->vy, d = 0.02f + 0.035f * impact; /* saut normal : ~13 cm */
                    w->air = 0;
                    w->air_y = g;
                    w->support = g;
                    w->dip_tgt = d > 0.50f ? 0.50f : d;
                    w->land_tgt = 0.09f * impact + (w->air_jump ? 0.10f : 0.0f);
                    if (w->land_tgt > 1.0f)
                        w->land_tgt = 1.0f;
                    w->air_tgt = 0.0f;
                    w->land_hold = impact > 7.0f ? 0.6f : 0.15f;
                    if (impact > 7.0f)
                        w->speed *= 0.3f; /* grosse chute : il s'arrête presque */
                }
            } else if (w->prejump <= 0.0f) {
                /* il se redresse après la réception (plus lentement après une grosse chute) */
                w->air_tgt = 0.0f;
                if (w->land_hold > 0.0f)
                    w->land_hold -= dt;
                else {
                    w->land_tgt -= 1.5f * dt;
                    if (w->land_tgt < 0.0f)
                        w->land_tgt = 0.0f;
                    w->dip_tgt -= w->dip_tgt * (3.0f * dt > 1.0f ? 1.0f : 3.0f * dt);
                }
            }
            /* v0.40.49 : tout passe en douceur (ressort amorti), plus de sauts de pose d'une
               image à l'autre */
            smooth_to(&w->airw, &w->airw_v, w->air_tgt, 9.0f, dt);
            smooth_to(&w->land, &w->land_v, w->land_tgt, 11.0f, dt);
            if (!(w->prejump > 0.0f && w->prejump <= PUSH_TIME))
                smooth_to(&w->dip, &w->dip_v, w->dip_tgt, w->prejump > 0.0f ? 22.0f : 12.0f, dt);
            smooth_to(&w->risew, &w->rise_v, w->air && w->vy > -1.0f ? smoothstep((w->vy + 1.0f) / 2.5f) : 0.0f, 7.0f, dt);
            if (w->risew > 1.0f)
                w->risew = 1.0f;
            g_air_on = w->air;
            g_air_y = w->air_y;
        }

        /* cycle de marche : un cycle complet (deux pas) tous les ~1,3 m (figé en l'air) */
        float phase_air = w->phase;
        if (fabsf(w->speed) > 0.02f) {
            w->phase += 2.0f * PI * fabsf(w->speed) / gait_cycle(fabsf(w->speed)) * dt;
        } else {
            /* à l'arrêt, on ramène les pieds côte à côte */
            float m = fmodf(w->phase, PI);
            if (m > 0.05f && m < PI - 0.05f)
                w->phase += 2.0f * dt;
        }
        if (w->phase > 2.0f * PI)
            w->phase -= 2.0f * PI;
        /* tourner sur place fait aussi bouger les pieds */
        if ((in->left || in->right) && !in->steer && fabsf(w->speed) < 0.3f)
            w->phase += 3.0f * dt;
        if (w->air)
            w->phase = phase_air;
    }

    stand_pose(w, rd, target);

    if (w->state == WALK_GETTING_UP) {
        /* v1.8.15 : comme un vrai pilote : (sur le dos : il se retourne sur le ventre), il pousse
         * sur les bras à quatre pattes, passe un genou devant (à genou), puis se lève sur la jambe
         * avant */
        float kneel[RD_NUM_POINTS][3], quad[RD_NUM_POINTS][3], start[RD_NUM_POINTS][3];
        float T = w->getup_T > 0.5f ? w->getup_T : GETUP_TIME, x = w->t / T;
        float x0 = 0.0f;
        kneel_pose(w, rd, kneel);
        quad_pose(w, rd, quad);
        if (w->getup_roll) {
            /* il roule sur le côté jusqu'au ventre (un demi-tour autour de son grand axe) */
            float a = smoothstep(x / 0.22f) * 3.14159265f;
            for (i = 0; i < RD_NUM_POINTS; ++i)
                rot_axis(w->from[i], w->roll_c, w->roll_axis, a, start[i]);
            x0 = 0.22f;
        } else {
            memcpy(start, w->from, sizeof(start));
        }
        if (x < x0) {
            memcpy(target, start, sizeof(start));
        } else if (x < x0 + (1.0f - x0) * 0.40f) {
            /* au sol -> à quatre pattes : le buste se soulève d'abord, puis les genoux viennent */
            float y = (x - x0) / ((1.0f - x0) * 0.40f);
            float a = smoothstep(y / 0.8f), legs = smoothstep((y - 0.2f) / 0.8f);
            for (i = 0; i < RD_NUM_POINTS; ++i) {
                int is_leg = i == RD_HIP_L || i == RD_HIP_R || i == RD_KNEE_L || i == RD_KNEE_R ||
                             i == RD_FOOT_L || i == RD_FOOT_R || i == RD_PELVIS;
                float u = is_leg ? legs : a;
                for (k = 0; k < 3; ++k)
                    target[i][k] = start[i][k] + (quad[i][k] - start[i][k]) * u;
            }
        } else if (x < x0 + (1.0f - x0) * 0.68f) {
            /* quatre pattes -> à genou (le buste se redresse) */
            float u = smoothstep((x - x0 - (1.0f - x0) * 0.40f) / ((1.0f - x0) * 0.28f));
            for (i = 0; i < RD_NUM_POINTS; ++i)
                for (k = 0; k < 3; ++k)
                    target[i][k] = quad[i][k] + (kneel[i][k] - quad[i][k]) * u;
        } else {
            float u = smoothstep((x - x0 - (1.0f - x0) * 0.68f) / ((1.0f - x0) * 0.32f));
            for (i = 0; i < RD_NUM_POINTS; ++i)
                for (k = 0; k < 3; ++k)
                    target[i][k] = kneel[i][k] + (target[i][k] - kneel[i][k]) * u;
        }
        /* rien sous le sol pendant la transition */
        for (i = 0; i < RD_NUM_POINTS; ++i) {
            float g = walk_ground_height(rd, target[i][0], target[i][2]) + 0.05f;
            if (target[i][1] < g)
                target[i][1] = g;
        }
        if (w->t >= T) {
            w->state = WALK_ON;
            w->t = 0.0f;
        }
    }

    for (i = 0; i < RD_NUM_POINTS; ++i)
        for (k = 0; k < 3; ++k) {
            rd->pos[i][k] = target[i][k];
            rd->prev[i][k] = target[i][k];
        }
    w->support = w->air ? w->air_y : walk_ground_height(rd, w->pos[0], w->pos[1]);
    w->support_ok = 1;
    /* les appels hors de walk_update (montée sur la moto...) gardent ce sol comme repère */
    g_ref_ok = 1;
    g_ref_y = w->support;
    g_start_ref = -1e9f; /* le sol est connu maintenant */
}

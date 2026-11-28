/*
 * ragdoll.c - moteur physique du ragdoll
 */
#include "ragdoll.h"

#include <math.h>
#include <string.h>

/* Position assise sur la moto, repère local :
 * X = droite, Y = haut, Z = avant. Hauteurs mesurées depuis le sol. */
static float pt_radius(const Ragdoll *rd, int i);
static const float k_seated_pose[RD_NUM_POINTS][3] = {
    /* HEAD       */ { 0.00f, 1.62f,  0.18f },
    /* NECK       */ { 0.00f, 1.45f,  0.10f },
    /* CHEST      */ { 0.00f, 1.25f,  0.00f },
    /* PELVIS     */ { 0.00f, 0.98f, -0.22f },
    /* SHOULDER_L */ {-0.20f, 1.42f,  0.06f },
    /* ELBOW_L    */ {-0.34f, 1.22f,  0.28f },
    /* HAND_L     */ {-0.38f, 1.10f,  0.52f },
    /* SHOULDER_R */ { 0.20f, 1.42f,  0.06f },
    /* ELBOW_R    */ { 0.34f, 1.22f,  0.28f },
    /* HAND_R     */ { 0.38f, 1.10f,  0.52f },
    /* HIP_L      */ {-0.12f, 0.95f, -0.20f },
    /* KNEE_L     */ {-0.24f, 0.78f,  0.20f },
    /* FOOT_L     */ {-0.20f, 0.36f,  0.00f },
    /* HIP_R      */ { 0.12f, 0.95f, -0.20f },
    /* KNEE_R     */ { 0.24f, 0.78f,  0.20f },
    /* FOOT_R     */ { 0.20f, 0.36f,  0.00f },
};

/* Masses en kg (total ~73 kg) */
static const float k_mass[RD_NUM_POINTS] = {
    5.0f, 2.0f, 18.0f, 14.0f,
    3.0f, 2.0f, 1.0f,
    3.0f, 2.0f, 1.0f,
    5.0f, 4.0f, 2.0f,
    5.0f, 4.0f, 2.0f,
};

static const char *k_names[RD_NUM_POINTS] = {
    "tete", "cou", "torse", "bassin",
    "epaule_g", "coude_g", "main_g",
    "epaule_d", "coude_d", "main_d",
    "hanche_g", "genou_g", "pied_g",
    "hanche_d", "genou_d", "pied_d",
};

/* ---------- petites fonctions vectorielles ---------- */

static float dist3(const float a[3], const float b[3])
{
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* Pose de référence utilisée pour mesurer les longueurs (assise par défaut) */
static const float (*g_pose)[3] = k_seated_pose;

static float pose_dist(int a, int b)
{
    return dist3(g_pose[a], g_pose[b]);
}

/* ---------- construction du squelette ---------- */

static void add_constraint(Ragdoll *rd, int a, int b, float length,
                           RdConstraintType type, int visible)
{
    RdConstraint *c;
    if (rd->num_constraints >= RD_MAX_CONSTRAINTS)
        return;
    c = &rd->constraints[rd->num_constraints++];
    c->a = a;
    c->b = b;
    c->length = length;
    c->type = type;
    c->visible = visible;
}

static void bone(Ragdoll *rd, int a, int b)
{
    add_constraint(rd, a, b, pose_dist(a, b), RD_CONSTRAINT_FIXED, 1);
}

static void brace(Ragdoll *rd, int a, int b)
{
    add_constraint(rd, a, b, pose_dist(a, b), RD_CONSTRAINT_FIXED, 0);
}

/* Le membre a-b-c ne peut pas se replier plus que ratio x sa longueur dépliée */
static void limit(Ragdoll *rd, int a, int b, int c, float ratio)
{
    float full = pose_dist(a, b) + pose_dist(b, c);
    add_constraint(rd, a, c, full * ratio, RD_CONSTRAINT_MIN, 0);
}

static void build_skeleton(Ragdoll *rd)
{
    rd->num_constraints = 0;

    /* Os visibles */
    bone(rd, RD_HEAD, RD_NECK);
    bone(rd, RD_NECK, RD_CHEST);
    bone(rd, RD_CHEST, RD_PELVIS);
    bone(rd, RD_NECK, RD_SHOULDER_L);
    bone(rd, RD_SHOULDER_L, RD_ELBOW_L);
    bone(rd, RD_ELBOW_L, RD_HAND_L);
    bone(rd, RD_NECK, RD_SHOULDER_R);
    bone(rd, RD_SHOULDER_R, RD_ELBOW_R);
    bone(rd, RD_ELBOW_R, RD_HAND_R);
    bone(rd, RD_PELVIS, RD_HIP_L);
    bone(rd, RD_HIP_L, RD_KNEE_L);
    bone(rd, RD_KNEE_L, RD_FOOT_L);
    bone(rd, RD_PELVIS, RD_HIP_R);
    bone(rd, RD_HIP_R, RD_KNEE_R);
    bone(rd, RD_KNEE_R, RD_FOOT_R);

    /* Rigidité du tronc (invisible) */
    brace(rd, RD_SHOULDER_L, RD_SHOULDER_R);
    brace(rd, RD_HIP_L, RD_HIP_R);
    brace(rd, RD_SHOULDER_L, RD_CHEST);
    brace(rd, RD_SHOULDER_R, RD_CHEST);
    brace(rd, RD_SHOULDER_L, RD_PELVIS);
    brace(rd, RD_SHOULDER_R, RD_PELVIS);
    brace(rd, RD_HIP_L, RD_CHEST);
    brace(rd, RD_HIP_R, RD_CHEST);
    brace(rd, RD_NECK, RD_PELVIS);
    /* bassin solidaire du torse : sans ça, les hanches pouvaient tourner autour de la colonne
       et le bassin se retournait par rapport au buste */
    brace(rd, RD_HIP_L, RD_SHOULDER_L);
    brace(rd, RD_HIP_R, RD_SHOULDER_R);
    brace(rd, RD_HIP_L, RD_SHOULDER_R);
    brace(rd, RD_HIP_R, RD_SHOULDER_L);

    /* Tête : le cou ne plie pas trop */
    brace(rd, RD_HEAD, RD_SHOULDER_L);
    brace(rd, RD_HEAD, RD_SHOULDER_R);

    /* Limites d'articulations : coudes et genoux ne se replient pas entièrement */
    limit(rd, RD_SHOULDER_L, RD_ELBOW_L, RD_HAND_L, 0.45f);
    limit(rd, RD_SHOULDER_R, RD_ELBOW_R, RD_HAND_R, 0.45f);
    limit(rd, RD_HIP_L, RD_KNEE_L, RD_FOOT_L, 0.40f);
    limit(rd, RD_HIP_R, RD_KNEE_R, RD_FOOT_R, 0.40f);

    /* Les cuisses ne traversent pas le torse */
    add_constraint(rd, RD_CHEST, RD_KNEE_L, 0.30f, RD_CONSTRAINT_MIN, 0);
    add_constraint(rd, RD_CHEST, RD_KNEE_R, 0.30f, RD_CONSTRAINT_MIN, 0);
    /* Les mains ne traversent pas le torse */
    add_constraint(rd, RD_PELVIS, RD_HAND_L, 0.15f, RD_CONSTRAINT_MIN, 0);
    add_constraint(rd, RD_PELVIS, RD_HAND_R, 0.15f, RD_CONSTRAINT_MIN, 0);
}

static void detect_hinge(Ragdoll *rd);

/* ---------- API ---------- */

void rd_config_default(RdConfig *cfg)
{
    cfg->gravity = 9.81f;
    cfg->damping = 0.05f;
    cfg->ground_friction = 1.2f; /* v0.41.19 : 0,7 -> 1,2 (le pilote glissait trop loin) */
    cfg->ground_bounce = 0.15f;
    cfg->point_radius = 0.08f;
    cfg->ground_offset = -0.55f;
    cfg->substep = 1.0f / 240.0f;
    cfg->iterations = 8;
    cfg->rest_speed = 0.15f;
    cfg->rest_time = 1.0f;
    cfg->rot_transpose = 0;
}

void rd_init(Ragdoll *rd, const RdConfig *cfg)
{
    int i;
    memset(rd, 0, sizeof(*rd));
    rd->cfg = *cfg;
    if (rd->cfg.substep <= 0.0f)
        rd->cfg.substep = 1.0f / 240.0f;
    if (rd->cfg.iterations < 1)
        rd->cfg.iterations = 1;
    rd->ground_n[1] = 1.0f;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        rd->inv_mass[i] = 1.0f / k_mass[i];
        memcpy(rd->pos[i], k_seated_pose[i], sizeof(rd->pos[i]));
        memcpy(rd->prev[i], k_seated_pose[i], sizeof(rd->prev[i]));
    }
    build_skeleton(rd);
}

void rd_spawn(Ragdoll *rd, const float bike_pos[3], const float bike_rot[3][3],
              const float bike_vel[3])
{
    int i, r, c;
    const float h = rd->cfg.substep;

    build_skeleton(rd); /* longueurs de la pose assise */
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float local[3];
        float world[3];
        local[0] = k_seated_pose[i][0];
        local[1] = k_seated_pose[i][1] + rd->cfg.ground_offset;
        local[2] = k_seated_pose[i][2];
        for (r = 0; r < 3; ++r) {
            world[r] = bike_pos[r];
            for (c = 0; c < 3; ++c) {
                float m = rd->cfg.rot_transpose ? bike_rot[c][r] : bike_rot[r][c];
                world[r] += m * local[c];
            }
        }
        for (r = 0; r < 3; ++r) {
            rd->pos[i][r] = world[r];
            rd->prev[i][r] = world[r] - bike_vel[r] * h;
        }
    }

    /* Le sol est supposé plat, à la hauteur des roues au moment de la chute */
    rd->ground_y = bike_pos[1] + rd->cfg.ground_offset;
    rd->ground_n[0] = 0.0f;
    rd->ground_n[1] = 1.0f;
    rd->ground_n[2] = 0.0f;
    rd->accumulator = 0.0f;
    rd->time = 0.0f;
    rd->still_time = 0.0f;
    rd->max_impact = 0.0f;
    rd->head_impact = 0.0f;
    rd->resting = 0;
    detect_hinge(rd);
    rd->guard_speed = 0.0f;
    rd->h0 = -1.0f;
    rd->active = 1;
}

void rd_spawn_points(Ragdoll *rd, const float points[RD_NUM_POINTS][3], const float vel[3],
                     float ground_y)
{
    int i, r;
    const float h = rd->cfg.substep;

    /* longueurs des os = celles du vrai squelette au moment de la chute */
    g_pose = points;
    build_skeleton(rd);
    g_pose = k_seated_pose;

    for (i = 0; i < RD_NUM_POINTS; ++i)
        for (r = 0; r < 3; ++r) {
            rd->pos[i][r] = points[i][r];
            rd->prev[i][r] = points[i][r] - vel[r] * h;
        }
    rd->ground_y = ground_y;
    rd->ground_n[0] = 0.0f;
    rd->ground_n[1] = 1.0f;
    rd->ground_n[2] = 0.0f;
    rd->accumulator = 0.0f;
    rd->time = 0.0f;
    rd->still_time = 0.0f;
    rd->max_impact = 0.0f;
    rd->head_impact = 0.0f;
    rd->resting = 0;
    detect_hinge(rd);
    rd->guard_speed = 0.0f;
    rd->h0 = -1.0f;
    rd->active = 1;
}

void rd_stop(Ragdoll *rd)
{
    rd->active = 0;
    rd->resting = 0;
}

static void solve_constraints(Ragdoll *rd)
{
    int k;
    for (k = 0; k < rd->num_constraints; ++k) {
        const RdConstraint *c = &rd->constraints[k];
        float *pa = rd->pos[c->a];
        float *pb = rd->pos[c->b];
        float d[3], len, diff, wa, wb, w;
        d[0] = pb[0] - pa[0];
        d[1] = pb[1] - pa[1];
        d[2] = pb[2] - pa[2];
        len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-6f)
            continue;
        if (c->type == RD_CONSTRAINT_MIN && len >= c->length)
            continue;
        diff = (len - c->length) / len;
        wa = rd->inv_mass[c->a];
        wb = rd->inv_mass[c->b];
        w = wa + wb;
        if (w <= 0.0f)
            continue;
        pa[0] += d[0] * diff * wa / w;
        pa[1] += d[1] * diff * wa / w;
        pa[2] += d[2] * diff * wa / w;
        pb[0] -= d[0] * diff * wb / w;
        pb[1] -= d[1] * diff * wb / w;
        pb[2] -= d[2] * diff * wb / w;
    }
}

/* ---------- limites des hanches et des genoux ----------
 * Les distances seules laissent le genou plier à l'envers ou sur le côté et la cuisse
 * partir n'importe où : les genoux « partent dans tous les sens » en l'air. On ajoute
 * ici de vraies limites angulaires, dans le repère du bassin. */

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float r[3])
{
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

static int norm3(float v[3])
{
    float l = sqrtf(dot3(v, v));
    if (l < 1e-5f)
        return 0;
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
    return 1;
}

/* déplace le genou de delta (et un peu la hanche en sens inverse, selon les masses) */
static void move_knee(Ragdoll *rd, int hip, int knee, float delta[3])
{
    float wh = rd->inv_mass[hip], wk = rd->inv_mass[knee], w = wh + wk;
    int k;
    float l = sqrtf(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    const float max_step = 0.005f; /* par itération : la correction ne lance jamais le pilote */
    if (w <= 0.0f || l < 1e-7f)
        return;
    if (l > max_step)
        for (k = 0; k < 3; ++k)
            delta[k] *= max_step / l;
    {
        /* un point posé au sol n'est jamais poussé dans le sol (sinon le sol le repousse et
           l'articulation et le sol se renvoient le pilote) */
        const int pt[2] = { knee, hip };
        const float sgn[2] = { wk / w, -wh / w };
        int j;
        for (j = 0; j < 2; ++j) {
            float d[3], gd, dn;
            float *p = rd->pos[pt[j]];
            for (k = 0; k < 3; ++k)
                d[k] = delta[k] * sgn[j];
            const float *n = rd->pt_n[pt[j]];
            gd = n[0] * p[0] + n[1] * p[1] + n[2] * p[2] - rd->pt_d[pt[j]] - pt_radius(rd, pt[j]);
            dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
            if (gd < 0.03f && dn < 0.0f)
                for (k = 0; k < 3; ++k)
                    d[k] -= dn * n[k];
            for (k = 0; k < 3; ++k)
                p[k] += d[k];
        }
    }
}

static void leg_limits(Ragdoll *rd, int hip, int knee, int foot, float side, const float a[3], const float f[3],
                       const float u[3])
{
    const float sg = rd->hinge_sign < 0.0f ? -1.0f : 1.0f;
    float t[3], L[3], o[3], b[3], delta[3], tl, ll, s, ob, oa, ta, tf, tu, out;
    int k;
    /* 1. hanche = rotule dans un cône : la cuisse peut monter loin vers l'avant, peu vers
       l'arrière, moyennement vers l'extérieur, très peu vers l'intérieur (repère du bassin) */
    for (k = 0; k < 3; ++k)
        t[k] = rd->pos[knee][k] - rd->pos[hip][k];
    tl = sqrtf(dot3(t, t));
    if (tl > 1e-4f) {
        float x, y, z, r, th, cx, cz, lx, lz, tmax;
        for (k = 0; k < 3; ++k)
            t[k] /= tl;
        x = dot3(t, a) * side; /* vers l'extérieur */
        z = dot3(t, f);        /* vers l'avant */
        y = -dot3(t, u);       /* vers le bas (jambe tendue sous le bassin) */
        r = sqrtf(x * x + z * z);
        th = atan2f(r, y);     /* écart à la jambe pendante */
        if (r > 1e-4f) {
            cx = x / r;
            cz = z / r;
            lx = x > 0.0f ? 0.85f : 0.35f;  /* extérieur ~49°, intérieur ~20° */
            lz = z > 0.0f ? 2.20f : 0.50f;  /* avant ~126°, arrière ~29° */
            tmax = 1.0f / sqrtf((cx / lx) * (cx / lx) + (cz / lz) * (cz / lz));
            if (th > tmax) {
                float n[3], ct = cosf(tmax), st = sinf(tmax);
                for (k = 0; k < 3; ++k)
                    n[k] = -u[k] * ct + (a[k] * side * cx + f[k] * cz) * st;
                for (k = 0; k < 3; ++k)
                    delta[k] = (rd->pos[hip][k] + n[k] * tl - rd->pos[knee][k]) * 0.5f;
                move_knee(rd, hip, knee, delta);
            }
        }
    }
    (void)ta; (void)tf; (void)tu; (void)out;
    /* 2. genou = charnière : le tibia part vers l'arrière de la cuisse (autour de l'axe des
       hanches, qui tourne avec la cuisse), presque pas de côté */
    {
        float th[3], sh[3], e2[3], e3[3], sl, s1, s2, s3, n2, n3, ad;
        for (k = 0; k < 3; ++k) {
            th[k] = rd->pos[knee][k] - rd->pos[hip][k];
            sh[k] = rd->pos[foot][k] - rd->pos[knee][k];
        }
        if (!norm3(th))
            return;
        ad = dot3(a, th);
        for (k = 0; k < 3; ++k)
            e3[k] = a[k] - th[k] * ad;
        if (!norm3(e3))
            return;
        cross3(e3, th, e2); /* vers l'arrière de la cuisse */
        for (k = 0; k < 3; ++k)
            e2[k] *= sg;
        sl = sqrtf(dot3(sh, sh));
        if (sl < 1e-4f)
            return;
        s1 = dot3(sh, th);
        s2 = dot3(sh, e2);
        s3 = dot3(sh, e3);
        n2 = s2 < -0.03f * sl ? -0.03f * sl : s2; /* un rien d'hyperextension, jamais à l'envers */
        /* tonus : une jambe relâchée reste un peu pliée (jamais raide comme un bâton) */
        if (n2 < 0.30f * sl) {
            float want = 0.30f * sl, step = 0.0004f;
            n2 = n2 + step < want ? n2 + step : want;
        }
        n3 = s3;
        {
            float m3 = 0.30f * (n2 > 0.0f ? n2 : 0.0f) + 0.04f * sl;
            if (n3 > m3) n3 = m3;
            if (n3 < -m3) n3 = -m3;
        }
        if (n2 == s2 && n3 == s3)
            return;
        {
            float ns[3], nl;
            for (k = 0; k < 3; ++k)
                ns[k] = th[k] * s1 + e2[k] * n2 + e3[k] * n3;
            nl = sqrtf(dot3(ns, ns));
            if (nl < 1e-5f)
                return;
            for (k = 0; k < 3; ++k)
                delta[k] = (rd->pos[knee][k] + ns[k] / nl * sl - rd->pos[foot][k]) * 0.5f;
            move_knee(rd, knee, foot, delta); /* déplace le pied (et le genou à l'opposé) */
        }
    }
    (void)L; (void)o; (void)b; (void)ll; (void)s; (void)ob; (void)oa;
}

/* Au départ, le pilote est assis : genoux bien pliés. Le sens trouvé fixe le repère
   (le squelette du jeu peut être en miroir de notre pose de référence). */
static void detect_hinge(Ragdoll *rd)
{
    const int legs[2][3] = { { RD_HIP_L, RD_KNEE_L, RD_FOOT_L }, { RD_HIP_R, RD_KNEE_R, RD_FOOT_R } };
    float a[3], sum = 0.0f;
    int l, k;
    for (k = 0; k < 3; ++k)
        a[k] = rd->pos[RD_HIP_R][k] - rd->pos[RD_HIP_L][k];
    for (l = 0; l < 2; ++l) {
        float th[3], sh[3], n[3];
        for (k = 0; k < 3; ++k) {
            th[k] = rd->pos[legs[l][1]][k] - rd->pos[legs[l][0]][k];
            sh[k] = rd->pos[legs[l][2]][k] - rd->pos[legs[l][1]][k];
        }
        cross3(th, sh, n);
        sum += dot3(n, a); /* > 0 pour notre pose de référence */
    }
    rd->hinge_sign = sum < 0.0f ? -1.0f : 1.0f;
    {
        /* assis : les cuisses partent vers l'avant du bassin */
        float u[3], f[3], th[3];
        for (k = 0; k < 3; ++k) {
            u[k] = rd->pos[RD_CHEST][k] - rd->pos[RD_PELVIS][k];
            th[k] = rd->pos[RD_KNEE_L][k] + rd->pos[RD_KNEE_R][k] - rd->pos[RD_HIP_L][k] - rd->pos[RD_HIP_R][k];
        }
        cross3(a, u, f);
        rd->hip_sign = dot3(f, th) < 0.0f ? -1.0f : 1.0f;
    }
    {
        /* assis : les mains sont devant les épaules (sur le guidon) */
        float as[3], u[3], f[3], d[3];
        for (k = 0; k < 3; ++k) {
            as[k] = rd->pos[RD_SHOULDER_R][k] - rd->pos[RD_SHOULDER_L][k];
            u[k] = rd->pos[RD_NECK][k] - rd->pos[RD_PELVIS][k];
            d[k] = rd->pos[RD_HAND_L][k] + rd->pos[RD_HAND_R][k] - rd->pos[RD_SHOULDER_L][k] - rd->pos[RD_SHOULDER_R][k];
        }
        cross3(as, u, f);
        rd->arm_sign = dot3(f, d) < 0.0f ? -1.0f : 1.0f;
    }
}

/* Bras : épaule = rotule très mobile (cône large), coude = il plie vers l'avant / l'intérieur,
   jamais vers l'arrière (repère du torse). */
static void arm_limits(Ragdoll *rd, int sh, int el, int ha, float side, const float a[3], const float f[3],
                       const float u[3])
{
    float t[3], delta[3], tl;
    int k;
    for (k = 0; k < 3; ++k)
        t[k] = rd->pos[el][k] - rd->pos[sh][k];
    tl = sqrtf(dot3(t, t));
    if (tl < 1e-4f)
        return;
    for (k = 0; k < 3; ++k)
        t[k] /= tl;
    {
        float x = dot3(t, a) * side, z = dot3(t, f), y = -dot3(t, u), r = sqrtf(x * x + z * z);
        float th = atan2f(r, y);
        if (r > 1e-4f) {
            float cx = x / r, cz = z / r;
            float lx = x > 0.0f ? 2.6f : 0.75f;  /* écarté ~150°, en travers du torse ~43° */
            float lz = z > 0.0f ? 2.9f : 0.95f;  /* devant / en haut ~166°, derrière ~54° */
            float tmax = 1.0f / sqrtf((cx / lx) * (cx / lx) + (cz / lz) * (cz / lz));
            if (th > tmax) {
                float n[3], ct = cosf(tmax), st = sinf(tmax);
                for (k = 0; k < 3; ++k)
                    n[k] = -u[k] * ct + (a[k] * side * cx + f[k] * cz) * st;
                for (k = 0; k < 3; ++k)
                    delta[k] = (rd->pos[sh][k] + n[k] * tl - rd->pos[el][k]) * 0.5f;
                move_knee(rd, sh, el, delta);
            }
        }
    }
    (void)ha;
    /* coude : pas de règle de sens ici. Sans la rotation du bras (que 3 points ne donnent
       pas), le sens du pli dépend de la pose ; le coude ne dépasse jamais « tendu » (longueurs
       des os) et ne se replie pas entièrement (limite de distance). */
}

static void joint_limits(Ragdoll *rd)
{
    float a[3], u[3], f[3];
    int k;
    for (k = 0; k < 3; ++k) {
        a[k] = rd->pos[RD_HIP_R][k] - rd->pos[RD_HIP_L][k];
        u[k] = rd->pos[RD_CHEST][k] - rd->pos[RD_PELVIS][k];
    }
    if (!norm3(a))
        return;
    {
        float d = dot3(u, a);
        for (k = 0; k < 3; ++k)
            u[k] -= a[k] * d;
    }
    if (!norm3(u))
        return;
    cross3(a, u, f); /* avant du bassin */
    if (rd->hip_sign < 0.0f) {
        f[0] = -f[0];
        f[1] = -f[1];
        f[2] = -f[2];
    }
    leg_limits(rd, RD_HIP_L, RD_KNEE_L, RD_FOOT_L, -1.0f, a, f, u);
    leg_limits(rd, RD_HIP_R, RD_KNEE_R, RD_FOOT_R, 1.0f, a, f, u);
    {
        float as[3], ut[3], ft[3], d;
        for (k = 0; k < 3; ++k) {
            as[k] = rd->pos[RD_SHOULDER_R][k] - rd->pos[RD_SHOULDER_L][k];
            ut[k] = rd->pos[RD_NECK][k] - rd->pos[RD_PELVIS][k];
        }
        if (!norm3(as))
            return;
        d = dot3(ut, as);
        for (k = 0; k < 3; ++k)
            ut[k] -= as[k] * d;
        if (!norm3(ut))
            return;
        cross3(as, ut, ft);
        if (rd->arm_sign < 0.0f)
            for (k = 0; k < 3; ++k)
                ft[k] = -ft[k];
        arm_limits(rd, RD_SHOULDER_L, RD_ELBOW_L, RD_HAND_L, -1.0f, as, ft, ut);
        arm_limits(rd, RD_SHOULDER_R, RD_ELBOW_R, RD_HAND_R, 1.0f, as, ft, ut);
    }
}

/* v0.41.20 : épaisseur de chaque partie (avant : 8 cm partout, le pilote couché rentrait dans
   le sol : un torse avec pare-pierres fait ~25 cm, un casque ~30 cm). Valeurs pour
   point_radius = 0,08 (réglage du .ini), proportionnelles sinon. */
static float pt_radius(const Ragdoll *rd, int i)
{
    /* v0.41.21 : vu en jeu (capture du joueur) : torse et bras encore dans le sol avec les
       valeurs de la 0.41.20, jambes bien posées : le haut du corps est remonté */
    static const float r[RD_NUM_POINTS] = {
        0.17f,               /* tête + casque */
        0.12f,               /* cou */
        0.18f,               /* poitrine (pare-pierres, dos) */
        0.16f,               /* bassin */
        /* v1.8.13 : bras et jambes plus épais (maillot, gants, bottes : ils passaient dans le sol) */
        0.13f, 0.10f, 0.08f, /* épaule, coude, main gauches */
        0.13f, 0.10f, 0.08f, /* droites */
        0.12f, 0.07f, 0.06f, /* hanche, genou (genouillère), botte gauches */
        0.12f, 0.07f, 0.06f, /* droites */
    };
    return r[i] * (rd->cfg.point_radius / 0.08f);
}

/*
 * Sol = plan incliné : dot(ground_n, p) = ground_y. Avec la normale (0, 1, 0),
 * ground_y est simplement la hauteur du sol.
 */
static float ground_dist(const Ragdoll *rd, int i)
{
    const float *n = rd->pt_n[i], *p = rd->pos[i];
    /* v1.8.13 : contre le sol, bras et jambes un peu plus épais (maillot, gants, bottes : ils
       passaient dans le sol) ; les chocs entre membres gardent les rayons d'avant */
    static const float extra[RD_NUM_POINTS] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.02f, 0.03f, 0.03f, 0.02f, 0.03f, 0.03f,
                                                0.01f, 0.03f, 0.03f, 0.01f, 0.03f, 0.03f };
    return n[0] * p[0] + n[1] * p[1] + n[2] * p[2] - rd->pt_d[i] - pt_radius(rd, i) - extra[i] - rd->gextra[i];
}

float rd_ground_radius(const Ragdoll *rd, int i)
{
    static const float extra[RD_NUM_POINTS] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.02f, 0.03f, 0.03f, 0.02f, 0.03f, 0.03f,
                                                0.01f, 0.03f, 0.03f, 0.01f, 0.03f, 0.03f };
    return pt_radius(rd, i) + extra[i];
}

/* v1.8.23 : membres (segments entre deux points) contre le sol, à leur milieu */
static const int k_seg[10][2] = {
    { RD_SHOULDER_L, RD_ELBOW_L }, { RD_ELBOW_L, RD_HAND_L }, { RD_SHOULDER_R, RD_ELBOW_R }, { RD_ELBOW_R, RD_HAND_R },
    { RD_HIP_L, RD_KNEE_L },       { RD_KNEE_L, RD_FOOT_L },  { RD_HIP_R, RD_KNEE_R },       { RD_KNEE_R, RD_FOOT_R },
    { RD_NECK, RD_CHEST },         { RD_CHEST, RD_PELVIS },
};
static const float k_seg_thick[10] = { 0.09f, 0.07f, 0.09f, 0.07f, 0.10f, 0.08f, 0.10f, 0.08f, 0.13f, 0.14f };

/* Sol sous chaque point, une fois par pas : le relief réel s'il est connu, sinon le plan */
float ragdoll_query_y = 1e9f;

static void update_ground(Ragdoll *rd)
{
    int i, k;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float h, n[3];
        ragdoll_query_y = rd->pos[i][1];
        if (rd->ground_fn && rd->ground_fn(rd->pos[i][0], rd->pos[i][2], &h, n) && n[1] > 0.2f) {
            for (k = 0; k < 3; ++k)
                rd->pt_n[i][k] = n[k];
            rd->pt_d[i] = n[0] * rd->pos[i][0] + n[1] * h + n[2] * rd->pos[i][2];
        } else {
            for (k = 0; k < 3; ++k)
                rd->pt_n[i][k] = rd->ground_n[k];
            rd->pt_d[i] = rd->ground_y;
        }
    }
    ragdoll_query_y = 1e9f;
    rd->seg_on = 0;
    if (rd->ground_fn) {
        int j;
        rd->seg_on = 1;
        for (j = 0; j < 10; ++j) {
            const float *a = rd->pos[k_seg[j][0]], *b = rd->pos[k_seg[j][1]];
            float m[3], h, n[3];
            for (k = 0; k < 3; ++k)
                m[k] = 0.5f * (a[k] + b[k]);
            ragdoll_query_y = m[1];
            if (rd->ground_fn(m[0], m[2], &h, n) && n[1] > 0.2f) {
                for (k = 0; k < 3; ++k)
                    rd->seg_n[j][k] = n[k];
                rd->seg_d[j] = n[0] * m[0] + n[1] * h + n[2] * m[2];
            } else {
                /* pas de relief ici : le plus haut des deux bouts */
                int e = rd->pt_d[k_seg[j][0]] > rd->pt_d[k_seg[j][1]] ? k_seg[j][0] : k_seg[j][1];
                for (k = 0; k < 3; ++k)
                    rd->seg_n[j][k] = rd->pt_n[e][k];
                rd->seg_d[j] = rd->pt_d[e];
            }
        }
        ragdoll_query_y = 1e9f;
    }
}

/* Empêche les points de passer sous le sol (correction de position seule) */
static void clamp_ground(Ragdoll *rd)
{
    int i, k;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float d = ground_dist(rd, i);
        if (d < 0.0f)
            for (k = 0; k < 3; ++k)
                rd->pos[i][k] -= d * rd->pt_n[i][k];
    }
    if (rd->seg_on) {
        /* v1.8.23 : milieu des membres : un avant-bras ou un tibia posé sur une bosse ne passe
           plus dedans ; les deux bouts remontent ensemble */
        int j;
        for (j = 0; j < 10; ++j) {
            float *a = rd->pos[k_seg[j][0]], *b = rd->pos[k_seg[j][1]], m[3], d;
            const float *n = rd->seg_n[j];
            for (k = 0; k < 3; ++k)
                m[k] = 0.5f * (a[k] + b[k]);
            d = n[0] * m[0] + n[1] * m[1] + n[2] * m[2] - rd->seg_d[j] - k_seg_thick[j];
            if (d < 0.0f)
                for (k = 0; k < 3; ++k) {
                    a[k] -= d * n[k];
                    b[k] -= d * n[k];
                }
        }
    }
}


/* v0.41.22 : les bras ne traversent plus le corps. Avant, rien n'empêchait le coude et la
   main de passer à travers le torse : couché, le bras finissait sous le dos (invisible sur
   la capture du joueur, « il y a pas de bras »). Le torse est une capsule cou -> bassin ;
   un bras dessous (côté sol) est sorti sur le côté, de son côté d'épaule. */
static void body_collide(Ragdoll *rd)
{
    static const int arm[4][2] = {
        { RD_ELBOW_L, RD_SHOULDER_L }, { RD_HAND_L, RD_SHOULDER_L },
        { RD_ELBOW_R, RD_SHOULDER_R }, { RD_HAND_R, RD_SHOULDER_R },
    };
    const float *a = rd->pos[RD_NECK], *b = rd->pos[RD_PELVIS];
    float ab[3], ll, tr = 0.13f * (rd->cfg.point_radius / 0.08f);
    int j, k;
    for (k = 0; k < 3; ++k)
        ab[k] = b[k] - a[k];
    ll = dot3(ab, ab);
    if (ll < 1e-6f)
        return;
    for (j = 0; j < 4; ++j) {
        int i = arm[j][0], sh = arm[j][1];
        float *p = rd->pos[i], t, c[3], d[3], dl, minr, push;
        for (k = 0; k < 3; ++k)
            d[k] = p[k] - a[k];
        t = dot3(d, ab) / ll;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        for (k = 0; k < 3; ++k) {
            c[k] = a[k] + ab[k] * t;
            d[k] = p[k] - c[k];
        }
        dl = sqrtf(dot3(d, d));
        minr = tr + pt_radius(rd, i);
        if (dl >= minr)
            continue;
        {
            /* bras sous le torse (côté sol) : on le sort par le côté de son épaule, assez
               loin pour qu'il soit à côté du torse et non dessous */
            const float *n = rd->pt_n[RD_CHEST];
            float side[3], sn, dn = dot3(d, n);
            for (k = 0; k < 3; ++k)
                side[k] = rd->pos[sh][k] - c[k];
            sn = dot3(side, n);
            for (k = 0; k < 3; ++k)
                side[k] -= n[k] * sn;
            if (dn < 0.0f && norm3(side)) {
                float lat = dot3(d, side), need = minr * minr - dn * dn;
                need = need > 0.0f ? sqrtf(need) : 0.0f;
                if (lat >= need)
                    continue;
                push = need - lat;
                for (k = 0; k < 3; ++k)
                    d[k] = side[k];
            } else {
                if (dl < 1e-4f)
                    continue;
                push = minr - dl;
                for (k = 0; k < 3; ++k)
                    d[k] /= dl;
            }
        }
        /* doux : 2 cm au plus par passe, sans donner de vitesse (l'ancienne position bouge
           aussi) : le bras glisse sur le côté sans faire sauter le corps */
        if (push > 0.02f)
            push = 0.02f;
        for (k = 0; k < 3; ++k) {
            p[k] += d[k] * push;
            rd->prev[i][k] += d[k] * push;
        }
    }
}

/* v0.41.27 : objets de la piste (murs, boîtes) : le point est sorti de l'objet. vel = 1 : une
   fois par pas, la vitesse vers l'objet est aussi retirée (pas de rebond) et frottée */
static void obstacle_contact(Ragdoll *rd, int vel)
{
    int i, k;
    if (!rd->obstacle_fn)
        return;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float push[3], pl;
        if (!rd->obstacle_fn(rd->pos[i], pt_radius(rd, i), push))
            continue;
        pl = sqrtf(dot3(push, push));
        if (pl < 1e-6f || pl > 1.0f)
            continue; /* plus d'1 m : mauvais côté d'un gros objet, on ne téléporte pas */
        for (k = 0; k < 3; ++k)
            rd->pos[i][k] += push[k];
        if (vel) {
            float n[3], v[3], vn;
            for (k = 0; k < 3; ++k) {
                n[k] = push[k] / pl;
                v[k] = rd->pos[i][k] - rd->prev[i][k];
            }
            vn = dot3(v, n);
            for (k = 0; k < 3; ++k) {
                float vt = v[k] - vn * n[k];
                /* plus de vitesse vers l'objet ; le long de l'objet, frottement */
                rd->prev[i][k] = rd->pos[i][k] - (vt * 0.8f + (vn > 0.0f ? vn * n[k] : 0.0f));
            }
        }
    }
}

/* Contact avec le sol : rebond et frottement de Coulomb (décélération mu * g) */
static void ground_contact(Ragdoll *rd)
{
    int i, k;
    const float h = rd->cfg.substep;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        const float *n = rd->pt_n[i];
        float *p = rd->pos[i];
        float *q = rd->prev[i];
        float v[3], vt[3], vn, speed, decel, scale;
        float d = ground_dist(rd, i);

        if (d > 0.0f)
            continue;

        for (k = 0; k < 3; ++k)
            v[k] = (p[k] - q[k]) / h;
        vn = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];

        /* vitesse d'arrivée contre le sol */
        if (-vn > rd->max_impact)
            rd->max_impact = -vn;
        if (i == RD_HEAD && -vn > rd->head_impact)
            rd->head_impact = -vn;

        for (k = 0; k < 3; ++k) {
            p[k] -= d * n[k];
            vt[k] = v[k] - vn * n[k];
        }
        vn = vn < 0.0f ? -vn * rd->cfg.ground_bounce : vn;

        /* frottement le long du sol */
        speed = sqrtf(vt[0] * vt[0] + vt[1] * vt[1] + vt[2] * vt[2]);
        decel = rd->cfg.ground_friction * rd->cfg.gravity * h;
        scale = speed > decel ? (speed - decel) / speed : 0.0f;
        for (k = 0; k < 3; ++k)
            q[k] = p[k] - (vt[k] * scale + vn * n[k]) * h;
    }
}

/* v1.8.8 : le corps qui glisse vite au sol roule (tonneaux / roulades) au lieu de glisser à plat.
   Les points prennent peu à peu la vitesse d'un corps qui roule sans glisser autour d'un axe
   couché, perpendiculaire au mouvement (le point en bas s'arrête, le haut passe par-dessus),
   avec une perte de vitesse (chocs au sol) pour qu'il ne roule pas plus loin qu'il ne glissait. */
static void roll_assist(Ragdoll *rd)
{
    const float h = rd->cfg.substep, R = 0.28f, beta = 0.16f, loss = 5.0f; /* v1.8.9 : roule plus (0,06 -> 0,16) */
    const float *n = rd->ground_fn ? rd->pt_n[RD_PELVIS] : rd->ground_n; /* relief sous le bassin */
    float c[3] = { 0, 0, 0 }, vc[3] = { 0, 0, 0 }, vn, sp, a[3], w, keep;
    int i, k, contacts = 0;
    /* seulement avec le vrai relief (sol estimé d'après le corps du jeu : il est trop flou) */
    if (rd->time < 0.12f || !rd->ground_fn)
        return;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        if (ground_dist(rd, i) <= 0.02f)
            ++contacts;
        for (k = 0; k < 3; ++k) {
            c[k] += rd->pos[i][k];
            vc[k] += (rd->pos[i][k] - rd->prev[i][k]) / h;
        }
    }
    if (contacts < 2)
        return;
    for (k = 0; k < 3; ++k) {
        c[k] /= (float)RD_NUM_POINTS;
        vc[k] /= (float)RD_NUM_POINTS;
    }
    vn = vc[0] * n[0] + vc[1] * n[1] + vc[2] * n[2];
    for (k = 0; k < 3; ++k)
        vc[k] -= vn * n[k]; /* vitesse le long du sol */
    sp = sqrtf(vc[0] * vc[0] + vc[1] * vc[1] + vc[2] * vc[2]);
    if (sp < 1.5f)
        return;
    /* axe a = n x v (couché, perpendiculaire au mouvement), vitesse de rotation sp / R */
    a[0] = n[1] * vc[2] - n[2] * vc[1];
    a[1] = n[2] * vc[0] - n[0] * vc[2];
    a[2] = n[0] * vc[1] - n[1] * vc[0];
    for (k = 0; k < 3; ++k)
        a[k] /= sp;
    keep = (sp - loss * h) / sp;
    w = sp * keep / R;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float r[3], v[3], t[3];
        /* le tronc seulement : bras et jambes suivent par les liaisons (les genoux pliaient à l'envers) */
        if (rd->inv_mass[i] <= 0.0f || !(i == RD_PELVIS || i == RD_CHEST || i == RD_NECK || i == RD_HEAD ||
                                         i == RD_HIP_L || i == RD_HIP_R || i == RD_SHOULDER_L || i == RD_SHOULDER_R))
            continue;
        for (k = 0; k < 3; ++k) {
            r[k] = rd->pos[i][k] - c[k];
            v[k] = (rd->pos[i][k] - rd->prev[i][k]) / h;
        }
        /* vitesse d'un corps qui roule : vc + w (a x r), plus la vitesse verticale actuelle */
        t[0] = vc[0] * keep + w * (a[1] * r[2] - a[2] * r[1]);
        t[1] = vc[1] * keep + w * (a[2] * r[0] - a[0] * r[2]);
        t[2] = vc[2] * keep + w * (a[0] * r[1] - a[1] * r[0]);
        for (k = 0; k < 3; ++k) {
            float nv = v[k] + (t[k] + vn * n[k] - v[k]) * beta;
            rd->prev[i][k] = rd->pos[i][k] - nv * h;
        }
    }
}

/* Frottement des articulations : la vitesse relative de deux points liés diminue un peu à
   chaque pas (comme des muscles), les membres suivent le corps au lieu de fouetter. La
   quantité de mouvement est conservée. */
static void joint_damping(Ragdoll *rd)
{
    static const int pairs[][2] = {
        { RD_HIP_L, RD_KNEE_L }, { RD_KNEE_L, RD_FOOT_L }, { RD_HIP_R, RD_KNEE_R }, { RD_KNEE_R, RD_FOOT_R },
        { RD_SHOULDER_L, RD_ELBOW_L }, { RD_ELBOW_L, RD_HAND_L }, { RD_SHOULDER_R, RD_ELBOW_R },
        { RD_ELBOW_R, RD_HAND_R }, { RD_NECK, RD_HEAD },
    };
    const float k = 7.0f * rd->cfg.substep; /* ~7 /s */
    int i, j;
    for (i = 0; i < (int)(sizeof(pairs) / sizeof(pairs[0])); ++i) {
        int a = pairs[i][0], b = pairs[i][1];
        float wa = rd->inv_mass[a], wb = rd->inv_mass[b], w = wa + wb;
        if (w <= 0.0f)
            continue;
        for (j = 0; j < 3; ++j) {
            /* vitesses x h = pos - prev */
            float va = rd->pos[a][j] - rd->prev[a][j], vb = rd->pos[b][j] - rd->prev[b][j];
            float dv = (vb - va) * k;
            rd->prev[b][j] += dv * wb / w; /* vb diminue */
            rd->prev[a][j] -= dv * wa / w; /* va augmente */
        }
    }
}

/* Garde-fou : les limites d'articulations corrigent des positions, ce qui peut créer de la
   vitesse quand elles se battent avec le sol. Le centre du corps ne peut pas accélérer plus
   que la gravité, et un membre ne fouette pas à plus de 14 m/s par rapport au corps. */
static void energy_guard(Ragdoll *rd)
{
    const float h = rd->cfg.substep;
    float vc[3] = { 0, 0, 0 }, m = 0.0f, s, lim;
    int i, k;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float mi = rd->inv_mass[i] > 0.0f ? 1.0f / rd->inv_mass[i] : 0.0f;
        for (k = 0; k < 3; ++k)
            vc[k] += (rd->pos[i][k] - rd->prev[i][k]) / h * mi;
        m += mi;
    }
    if (m <= 0.0f)
        return;
    for (k = 0; k < 3; ++k)
        vc[k] /= m;
    s = sqrtf(vc[0] * vc[0] + vc[1] * vc[1] + vc[2] * vc[2]);
    lim = rd->guard_speed + rd->cfg.gravity * h * 1.2f + 0.02f;
    if (rd->guard_speed > 0.0f && s > lim) {
        float cut = (s - lim) / s;
        for (i = 0; i < RD_NUM_POINTS; ++i)
            for (k = 0; k < 3; ++k)
                rd->prev[i][k] += vc[k] * cut * h; /* retire l'excès à tout le corps */
        for (k = 0; k < 3; ++k)
            vc[k] *= 1.0f - cut;
        s = lim;
    }
    {
        /* au sol, le corps ne doit pas aller plus vite à l'horizontale qu'à la chute : sur une
           pente, glisser ajoutait de la vitesse à chaque pas (pilote qui s'envolait). Limite :
           vitesse de départ qui s'éteint en ~1 s, + 3 m/s (rouler, dévaler un peu) */
        float hs = sqrtf(vc[0] * vc[0] + vc[2] * vc[2]), cap;
        if (rd->h0 < 0.0f)
            rd->h0 = hs;
        /* v0.41.19 : s'éteint en 0,6 s (1 s avant) et + 1,5 m/s (3 avant) : 9 m de glissade à
           34 km/h, 20 m à 77 km/h, trop pour un corps qui frotte la terre */
        cap = rd->h0 * expf(-rd->time / 0.6f) + 1.5f;
        if (hs > cap) {
            float cut = (hs - cap) / hs;
            /* on freine le long de la pente : la descente verticale aussi (sinon le corps
               s'enfonçait dans la pente) */
            float cy = vc[1] < 0.0f ? vc[1] * cut : 0.0f;
            for (i = 0; i < RD_NUM_POINTS; ++i) {
                rd->prev[i][0] += vc[0] * cut * h;
                rd->prev[i][1] += cy * h;
                rd->prev[i][2] += vc[2] * cut * h;
            }
            vc[0] *= 1.0f - cut;
            vc[1] -= cy;
            vc[2] *= 1.0f - cut;
            s = sqrtf(vc[0] * vc[0] + vc[1] * vc[1] + vc[2] * vc[2]);
        }
    }
    rd->guard_speed = s > 0.001f ? s : 0.001f;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float vr[3], r;
        for (k = 0; k < 3; ++k)
            vr[k] = (rd->pos[i][k] - rd->prev[i][k]) / h - vc[k];
        r = sqrtf(vr[0] * vr[0] + vr[1] * vr[1] + vr[2] * vr[2]);
        if (r > 14.0f)
            for (k = 0; k < 3; ++k)
                rd->prev[i][k] += vr[k] * (1.0f - 14.0f / r) * h;
    }
}

static void substep(Ragdoll *rd)
{
    int i, it;
    const float h = rd->cfg.substep;
    const float keep = 1.0f - rd->cfg.damping * h;
    const float g = -rd->cfg.gravity * h * h;

    /* Verlet */
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float *p = rd->pos[i];
        float *q = rd->prev[i];
        float nx = p[0] + (p[0] - q[0]) * keep;
        float ny = p[1] + (p[1] - q[1]) * keep + g;
        float nz = p[2] + (p[2] - q[2]) * keep;
        q[0] = p[0];
        q[1] = p[1];
        q[2] = p[2];
        p[0] = nx;
        p[1] = ny;
        p[2] = nz;
    }

    joint_damping(rd);
    update_ground(rd);
    roll_assist(rd); /* v1.8.8 (avant le contact : il ne pousse pas les points dans le sol) */
    ground_contact(rd);
    obstacle_contact(rd, 1);

    for (it = 0; it < rd->cfg.iterations; ++it) {
        solve_constraints(rd);
        joint_limits(rd);
        body_collide(rd);
        if (it == rd->cfg.iterations - 1)
            obstacle_contact(rd, 0);
        clamp_ground(rd);
    }
    energy_guard(rd);

    rd->time += h;
    if (rd_max_speed(rd) < rd->cfg.rest_speed) {
        rd->still_time += h;
        if (rd->still_time >= rd->cfg.rest_time)
            rd->resting = 1;
    } else {
        rd->still_time = 0.0f;
        rd->resting = 0;
    }
}

void rd_step(Ragdoll *rd, float dt)
{
    if (!rd->active || dt <= 0.0f)
        return;
    if (dt > 0.1f)
        dt = 0.1f; /* évite l'emballement après une pause */
    rd->accumulator += dt;
    while (rd->accumulator >= rd->cfg.substep) {
        substep(rd);
        rd->accumulator -= rd->cfg.substep;
    }
}

float rd_max_speed(const Ragdoll *rd)
{
    int i;
    float best = 0.0f;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float v = dist3(rd->pos[i], rd->prev[i]) / rd->cfg.substep;
        if (v > best)
            best = v;
    }
    return best;
}

float rd_max_bone_error(const Ragdoll *rd)
{
    int k;
    float worst = 0.0f;
    for (k = 0; k < rd->num_constraints; ++k) {
        const RdConstraint *c = &rd->constraints[k];
        float len, err;
        if (c->type != RD_CONSTRAINT_FIXED)
            continue;
        len = dist3(rd->pos[c->a], rd->pos[c->b]);
        err = fabsf(len - c->length) / c->length;
        if (err > worst)
            worst = err;
    }
    return worst;
}

void rd_center(const Ragdoll *rd, float out[3])
{
    int i;
    float total = 0.0f;
    out[0] = out[1] = out[2] = 0.0f;
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float m = k_mass[i];
        out[0] += rd->pos[i][0] * m;
        out[1] += rd->pos[i][1] * m;
        out[2] += rd->pos[i][2] * m;
        total += m;
    }
    out[0] /= total;
    out[1] /= total;
    out[2] /= total;
}

const char *rd_point_name(int index)
{
    if (index < 0 || index >= RD_NUM_POINTS)
        return "?";
    return k_names[index];
}

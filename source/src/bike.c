/*
 * bike.c - corps physiques ODE de la moto : liste et redressement
 *
 * Disposition d'un dxBody (ODE, simple précision, 64 bits), vérifiée en jeu
 * par la position de la rotation de la télémétrie :
 *   +0x00 monde (dxWorld*)   +0x08 suivant (dxBody*)   +0x10 tome   +0x18 tag
 *   +0x20 userdata           +0x28 premier joint       +0x30 drapeaux
 *   +0x38 geom               +0x40 masse (kg)          +0x44 centre, +0x54 inertie
 *   +0x84 inverse inertie    +0xB4 1/masse
 *   +0xB8 position (4)       +0xC8 rotation dMatrix3 (12, lignes de 4)
 *   +0xF8 quaternion (w x y z)  +0x108 vitesse  +0x118 vitesse angulaire
 */
#include "bike.h"
#include "hook.h"
#include "terrain.h"

#include <math.h>
#include <string.h>

#define OFF_WORLD 0x00
#define OFF_NEXT  0x08
#define OFF_FLAGS 0x30
#define OFF_MASS  0x40
#define OFF_POS   0xB8
#define OFF_R     0xC8
#define OFF_Q     0xF8
#define OFF_LVEL  0x108
#define OFF_AVEL  0x118
#define OFF_FACC  0x128 /* forces et couples accumulés (dVector3 facc, tacc) */
#define BODY_DISABLED 4 /* dxBodyDisabled dans ODE */
#define BODY_NOGRAVITY 8 /* dxBodyNoGravity : la gravité du monde ne s'applique plus à ce corps */

typedef struct {
    float pos[4];
    float R[12];
    float q[4];
    float lvel[4];
    float avel[4];
} BodyState;

static int read_state(uintptr_t b, BodyState *s)
{
    return safe_read((const void *)(b + OFF_POS), s, sizeof(*s));
}

static int body_ok(uintptr_t b)
{
    BodyState s;
    float qn;
    if (!b || (b & 7) || !read_state(b, &s))
        return 0;
    qn = s.q[0] * s.q[0] + s.q[1] * s.q[1] + s.q[2] * s.q[2] + s.q[3] * s.q[3];
    return fabsf(qn - 1.0f) < 0.02f && s.R[3] == 0.0f && s.R[7] == 0.0f && s.R[11] == 0.0f &&
           isfinite(s.pos[0]) && fabsf(s.pos[0]) < 1e5f;
}

/* Tous les corps du monde ODE du châssis, en suivant la liste du monde */
static int list_bodies(uintptr_t chassis, uintptr_t *out, int max, uintptr_t *first_out)
{
    uintptr_t world = 0, first = 0, b;
    int off, n = 0;
    if (!safe_read((const void *)(chassis + OFF_WORLD), &world, 8) || !world)
        return 0;
    /* dxWorld : on cherche dans ses premiers champs un pointeur de corps dont la liste contient le châssis */
    for (off = 0; off < 0x100 && !first; off += 8) {
        uintptr_t p = 0;
        int k;
        if (!safe_read((const void *)(world + off), &p, 8) || !body_ok(p))
            continue;
        for (b = p, k = 0; b && k < 512; ++k) {
            if (b == chassis) {
                first = p;
                break;
            }
            if (!safe_read((const void *)(b + OFF_NEXT), &b, 8))
                break;
        }
    }
    if (!first) {
        /* liste du monde introuvable : corps trouvés en mémoire avec le même monde */
        int m = mem_world_bodies(out, max);
        if (first_out)
            *first_out = 0;
        if (m > 0)
            return m;
        first = chassis;
    }
    if (first_out)
        *first_out = first;
    for (b = first; b && n < max; ) {
        if (!body_ok(b))
            break;
        out[n++] = b;
        if (!safe_read((const void *)(b + OFF_NEXT), &b, 8))
            break;
    }
    return n;
}

void bike_log_bodies(uintptr_t chassis, BikeLogFn log)
{
    uintptr_t list[64], first = 0, world = 0;
    int i, n;
    BodyState c;
    if (!log || !chassis || !read_state(chassis, &c))
        return;
    safe_read((const void *)(chassis + OFF_WORLD), &world, 8);
    n = list_bodies(chassis, list, 64, &first);
    log("moto : monde ODE %p, premier corps %p, %d corps", (void *)world, (void *)first, n);
    for (i = 0; i < n; ++i) {
        BodyState s;
        float m = 0.0f, dx, dy, dz;
        unsigned flags = 0;
        read_state(list[i], &s);
        safe_read((const void *)(list[i] + OFF_MASS), &m, 4);
        safe_read((const void *)(list[i] + OFF_FLAGS), &flags, 4);
        dx = s.pos[0] - c.pos[0];
        dy = s.pos[1] - c.pos[1];
        dz = s.pos[2] - c.pos[2];
        log("  corps %d %p%s : %.1f kg, a %.2f m du chassis, pos (%.2f %.2f %.2f), drapeaux %x, "
            "R [%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f]",
            i, (void *)list[i], list[i] == chassis ? " (chassis)" : "", m, sqrtf(dx * dx + dy * dy + dz * dz),
            s.pos[0], s.pos[1], s.pos[2], flags, s.R[0], s.R[1], s.R[2], s.R[4], s.R[5], s.R[6], s.R[8], s.R[9],
            s.R[10]);
    }
}

/* quaternion (w x y z) d'une matrice de rotation 3x3 (lignes) */
static void quat_from_R(const float m[9], float q[4])
{
    float tr = m[0] + m[4] + m[8], s;
    if (tr > 0.0f) {
        s = sqrtf(tr + 1.0f) * 2.0f;
        q[0] = 0.25f * s;
        q[1] = (m[7] - m[5]) / s;
        q[2] = (m[2] - m[6]) / s;
        q[3] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[0] = (m[7] - m[5]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[1] + m[3]) / s;
        q[3] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[0] = (m[2] - m[6]) / s;
        q[1] = (m[1] + m[3]) / s;
        q[2] = 0.25f * s;
        q[3] = (m[5] + m[7]) / s;
    } else {
        s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[0] = (m[3] - m[1]) / s;
        q[1] = (m[2] + m[6]) / s;
        q[2] = (m[5] + m[7]) / s;
        q[3] = 0.25f * s;
    }
}

int bike_upright(uintptr_t chassis, float cg_height, const float exclude[3], BikeLogFn log)
{
    uintptr_t list[64];
    BodyState c;
    float Rc[9], Rn[9], D[9], fx, fz, h, l, lift;
    int i, j, k, n, moved = 0;
    if (!chassis || !read_state(chassis, &c))
        return 0;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            Rc[i * 3 + j] = c.R[i * 4 + j];
    /* avant de la moto = 3e colonne (axe z du châssis), gardé à l'horizontale */
    fx = Rc[2];
    fz = Rc[8];
    l = sqrtf(fx * fx + fz * fz);
    if (l < 0.2f) {
        if (log)
            log("moto : redressement impossible, la moto est verticale (avant %.2f %.2f %.2f)", Rc[2], Rc[5], Rc[8]);
        return 0;
    }
    h = atan2f(fx / l, fz / l);
    /* debout : droite (cos h, 0, -sin h), haut (0 1 0), avant (sin h, 0, cos h) en colonnes */
    Rn[0] = cosf(h); Rn[1] = 0.0f; Rn[2] = sinf(h);
    Rn[3] = 0.0f;    Rn[4] = 1.0f; Rn[5] = 0.0f;
    Rn[6] = -sinf(h); Rn[7] = 0.0f; Rn[8] = cosf(h);
    /* D = Rn x Rc^T : rotation à appliquer à toute la moto autour du châssis */
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) {
            float s = 0.0f;
            for (k = 0; k < 3; ++k)
                s += Rn[i * 3 + k] * Rc[j * 3 + k];
            D[i * 3 + j] = s;
        }
    lift = cg_height - c.pos[1];
    n = list_bodies(chassis, list, 64, NULL);
    for (i = 0; i < n; ++i) {
        BodyState s;
        float rel[3], R3[9], R2[9], dx, dy, dz;
        unsigned flags = 0;
        if (!read_state(list[i], &s))
            continue;
        dx = s.pos[0] - c.pos[0];
        dy = s.pos[1] - c.pos[1];
        dz = s.pos[2] - c.pos[2];
        if (dx * dx + dy * dy + dz * dz > 2.5f * 2.5f)
            continue; /* pas une pièce de la moto */
        if (exclude) {
            float ex = s.pos[0] - exclude[0], ey = s.pos[1] - exclude[1], ez = s.pos[2] - exclude[2];
            if (ex * ex + ey * ey + ez * ez < 0.6f * 0.6f)
                continue; /* corps de chute du pilote */
        }
        rel[0] = dx; rel[1] = dy; rel[2] = dz;
        for (j = 0; j < 3; ++j)
            s.pos[j] = c.pos[j] + D[j * 3 + 0] * rel[0] + D[j * 3 + 1] * rel[1] + D[j * 3 + 2] * rel[2];
        s.pos[1] += lift;
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                R3[j * 3 + k] = s.R[j * 4 + k];
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                R2[j * 3 + k] = D[j * 3 + 0] * R3[0 * 3 + k] + D[j * 3 + 1] * R3[1 * 3 + k] + D[j * 3 + 2] * R3[2 * 3 + k];
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                s.R[j * 4 + k] = R2[j * 3 + k];
        quat_from_R(R2, s.q);
        memset(s.lvel, 0, sizeof(s.lvel));
        memset(s.avel, 0, sizeof(s.avel));
        if (safe_write((void *)(list[i] + OFF_POS), &s, sizeof(s))) {
            ++moved;
            /* réveille le corps s'il était endormi */
            if (safe_read((const void *)(list[i] + OFF_FLAGS), &flags, 4) && (flags & BODY_DISABLED)) {
                flags &= ~(unsigned)BODY_DISABLED;
                safe_write((void *)(list[i] + OFF_FLAGS), &flags, 4);
            }
        }
    }
    if (log)
        log("moto : redressee (cap %.0f deg, monte de %.2f m), %d corps deplaces", h * 57.2958f, lift, moved);
    return moved;
}

/* ---------- relever la moto progressivement ---------- */

static struct {
    int active, n;
    uintptr_t body[64];
    BodyState start[64];
    float pivot[3];
    float axis[3], angle, lift;
    /* direction : axe de direction (monde, moto couchée), tête de fourche, angle de chaque pièce */
    float steer_axis[3], head[3];
    float steer[64];
    uintptr_t chassis;
    int wheel[2];     /* v0.41.01 : indices des deux roues (pièces les plus basses à l'arrivée) */
    int ichassis;
    float Rc0[9];     /* rotation du châssis au départ */
    /* v0.41.12 : masse inverse (dxBody +0xB4) et inertie inverse (+0x84) gardées pendant
       qu'on tient la moto : mises à zéro, les liaisons (suspensions) ne bougent plus rien */
    int inv_saved[64];
    float inv_mass[64];
    float inv_I[64][12];
} g_lift;

/* Moto couchée, moteur en prise : ses roues tournent toutes seules et creusent la piste
   (déformation du terrain). On leur enlève leur rotation propre (même rotation que le châssis). */
int bike_calm_wheels(uintptr_t chassis)
{
    uintptr_t list[64];
    BodyState c;
    int i, n, done = 0;
    if (!chassis || !read_state(chassis, &c))
        return 0;
    /* moto qui bouge vraiment (respawn, explosion) : on n'y touche pas */
    if (fabsf(c.lvel[0]) + fabsf(c.lvel[1]) + fabsf(c.lvel[2]) > 3.0f ||
        fabsf(c.avel[0]) + fabsf(c.avel[1]) + fabsf(c.avel[2]) > 6.0f)
        return 0;
    n = list_bodies(chassis, list, 64, NULL);
    for (i = 0; i < n; ++i) {
        BodyState s;
        float m = 0.0f, dx, dy, dz, w;
        if (list[i] == chassis || !read_state(list[i], &s))
            continue;
        safe_read((const void *)(list[i] + OFF_MASS), &m, 4);
        if (m > 50.0f)
            continue; /* le pilote */
        dx = s.pos[0] - c.pos[0];
        dy = s.pos[1] - c.pos[1];
        dz = s.pos[2] - c.pos[2];
        if (dx * dx + dy * dy + dz * dz > 2.5f * 2.5f)
            continue;
        w = fabsf(s.avel[0] - c.avel[0]) + fabsf(s.avel[1] - c.avel[1]) + fabsf(s.avel[2] - c.avel[2]);
        if (w < 0.5f)
            continue;
        if (safe_write((void *)(list[i] + OFF_AVEL), c.avel, sizeof(float) * 3))
            ++done;
    }
    return done;
}

uintptr_t bike_lift_chassis(void)
{
    return g_lift.active ? g_lift.chassis : 0;
}
/* 1 : pièces gelées (ODE ne les fait plus bouger) pendant le respawn */
static int g_freeze = 0;

static void unfreeze_all(void);
static void clear_forces(uintptr_t b);
static BikeGroundFn g_ground = NULL;
void bike_set_ground_fn(BikeGroundFn fn) { g_ground = fn; }
/* hauteur du centre des roues au-dessus du sol, mesurée sur la moto arrêtée droite sur ses
   roues (même relief que pour la vérification : un décalage du relief s'annule) */
static float g_wheel_h = 0.0f;
/* v1.7.1 : rayon d'une roue de cross (21" et 19" avec pneu : 0,34-0,35 m) ; avant 0,40 + 8 cm de
   marge : la moto tenue flottait (roue arrière bien au-dessus du sol, vu en jeu) */
#define WHEEL_R 0.345f
/* v1.7.16 : relief encore provisoire (piste nouvelle, pas encore recalé à l'arrêt) : on ne
   descend pas la moto vers un sol estimé (Scottsdale : moto enfoncée de ~10 cm, vu en jeu) */
static int g_ground_trusted = 1;
void bike_set_ground_trusted(int on) { g_ground_trusted = on; }

/* les deux pièces les plus basses (les roues) parmi pos[n] ; retourne leur nombre */
static int lowest_two(const float (*pos)[3], int n, int skip, int idx[2])
{
    int i, k = 0;
    idx[0] = idx[1] = -1;
    for (i = 0; i < n; ++i) {
        if (i == skip)
            continue;
        if (idx[0] < 0 || pos[i][1] < pos[idx[0]][1]) {
            idx[1] = idx[0];
            idx[0] = i;
        } else if (idx[1] < 0 || pos[i][1] < pos[idx[1]][1]) {
            idx[1] = i;
        }
    }
    for (i = 0; i < 2; ++i)
        if (idx[i] >= 0)
            ++k;
    return k;
}

float bike_measure_wheels(uintptr_t chassis)
{
    uintptr_t list[64];
    float pos[64][3];
    int i, n, m = 0, idx[2], skip = -1;
    float h = 1e9f;
    if (!g_ground || !chassis)
        return 0.0f;
    n = list_bodies(chassis, list, 64, NULL);
    {
        BodyState c;
        if (!read_state(chassis, &c))
            return 0.0f;
        for (i = 0; i < n; ++i) {
            BodyState st;
            float mass = 0.0f, dx, dy, dz;
            if (!read_state(list[i], &st))
                continue;
            safe_read((const void *)(list[i] + OFF_MASS), &mass, 4);
            dx = st.pos[0] - c.pos[0];
            dy = st.pos[1] - c.pos[1];
            dz = st.pos[2] - c.pos[2];
            if (mass > 50.0f && list[i] != chassis)
                continue;
            if (dx * dx + dy * dy + dz * dz > 2.5f * 2.5f)
                continue;
            if (list[i] == chassis)
                skip = m;
            pos[m][0] = st.pos[0];
            pos[m][1] = st.pos[1];
            pos[m][2] = st.pos[2];
            ++m;
        }
    }
    if (lowest_two((const float (*)[3])pos, m, skip, idx) < 2)
        return 0.0f;
    for (i = 0; i < 2; ++i) {
        float gy;
        if (g_ground(pos[idx[i]][0], pos[idx[i]][2], &gy) && pos[idx[i]][1] - gy < h)
            h = pos[idx[i]][1] - gy;
    }
    if (h > 0.15f && h < 0.7f)
        g_wheel_h = h;
    if (g_wheel_h > 0.42f || g_wheel_h < 0.25f)
        g_wheel_h = 0.0f; /* v1.7.1 : roue de cross 0,34-0,35 m : mesure faussée par le relief */
    return h < 1e8f ? h : 0.0f;
}

/* Moto tenue : une roue à moins de 2 cm de sa hauteur posée = elle touche ; on remonte la moto */
static int g_lift_raised_logged = 0;
static float g_lift_extra = 0.0f, g_lift_need_max = 0.0f;
static int g_lift_need_logged = 0;
static int g_keep_up = 1;          /* surveillance des roues (pas pour la moto tenue après une descente) */
static float g_keep_total = 0.0f;   /* déjà remontée par la surveillance */
void bike_lift_keep_wheels(int on) { g_keep_up = on; }
static BikeLogFn g_lift_log = NULL;
static void unlock_bodies(void);
static void lock_bodies(BikeLogFn log);
static void keep_wheels_up(const float (*pos)[3], int n, BikeLogFn log)
{
    int idx[2], i;
    float need = 0.0f;
    float wh = g_wheel_h > 0.0f ? g_wheel_h : WHEEL_R; /* pas encore mesurée */
    if (!g_ground || lowest_two(pos, n, -1, idx) < 2)
        return;
    for (i = 0; i < 2; ++i) {
        float gy, d;
        if (!g_ground(pos[idx[i]][0], pos[idx[i]][2], &gy))
            continue;
        d = gy + wh + 0.02f - pos[idx[i]][1];
        if (d > need)
            need = d;
    }
    /* v0.40.18 : jamais plus de 10 cm au total. Sur une moto à 8 pièces (pilote de 64 kg encore
       accroché), 39 cm d'un coup ont étiré les liaisons : explosion à 1992 km/h */
    if (need > 0.10f - g_keep_total)
        need = 0.10f - g_keep_total;
    if (!g_keep_up)
        need = 0.0f;
    if (need > 0.001f) {
        g_keep_total += need;
        g_lift.lift += need;
        if (log && !g_lift_raised_logged) {
            g_lift_raised_logged = 1;
            log("moto : une roue touchait le sol, moto remontee de %.2f m", need);
        }
    }
}
void bike_lift_freeze(int on)
{
    if (!on && g_freeze)
        unfreeze_all();
    g_freeze = on;
}

/* rotation d'angle a autour de l'axe unitaire u (lignes) */
static void axis_angle_R(const float u[3], float a, float R[9])
{
    float c = cosf(a), s = sinf(a), t = 1.0f - c;
    R[0] = t * u[0] * u[0] + c;        R[1] = t * u[0] * u[1] - s * u[2]; R[2] = t * u[0] * u[2] + s * u[1];
    R[3] = t * u[0] * u[1] + s * u[2]; R[4] = t * u[1] * u[1] + c;        R[5] = t * u[1] * u[2] - s * u[0];
    R[6] = t * u[0] * u[2] - s * u[1]; R[7] = t * u[1] * u[2] + s * u[0]; R[8] = t * u[2] * u[2] + c;
}

int bike_lift_begin(uintptr_t chassis, float cg_height, float pitch, const float exclude[3], BikeLogFn log)
{
    uintptr_t list[64];
    BodyState c;
    float Rc[9], Rn[9], D[9], fx, fz, h, l, tr, ca;
    int i, j, k, n;
    g_lift.active = 0;
    if (!chassis || !read_state(chassis, &c))
        return 0;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            Rc[i * 3 + j] = c.R[i * 4 + j];
    fx = Rc[2];
    fz = Rc[8];
    l = sqrtf(fx * fx + fz * fz);
    if (l < 0.2f)
        return 0;
    h = atan2f(fx / l, fz / l);
    /* debout, mais incliné comme la pente sous les roues (pitch > 0 : l'avant monte).
       Colonnes : droite (horizontale), haut = avant x droite, avant */
    {
        float sp = sinf(pitch), cp = cosf(pitch), sh = sinf(h), ch = cosf(h);
        Rn[0] = ch;   Rn[1] = -sp * sh; Rn[2] = sh * cp;
        Rn[3] = 0.0f; Rn[4] = cp;       Rn[5] = sp;
        Rn[6] = -sh;  Rn[7] = -sp * ch; Rn[8] = ch * cp;
    }
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) {
            float sum = 0.0f;
            for (k = 0; k < 3; ++k)
                sum += Rn[i * 3 + k] * Rc[j * 3 + k];
            D[i * 3 + j] = sum;
        }
    /* D en axe + angle */
    tr = D[0] + D[4] + D[8];
    ca = 0.5f * (tr - 1.0f);
    if (ca > 1.0f) ca = 1.0f;
    if (ca < -1.0f) ca = -1.0f;
    g_lift.angle = acosf(ca);
    g_lift.axis[0] = D[7] - D[5];
    g_lift.axis[1] = D[2] - D[6];
    g_lift.axis[2] = D[3] - D[1];
    l = sqrtf(g_lift.axis[0] * g_lift.axis[0] + g_lift.axis[1] * g_lift.axis[1] + g_lift.axis[2] * g_lift.axis[2]);
    if (l < 1e-4f) {
        g_lift.axis[0] = 1.0f; g_lift.axis[1] = 0.0f; g_lift.axis[2] = 0.0f;
        if (g_lift.angle > 1.0f) {
            /* demi-tour : axe = colonne de D + I la plus longue */
            float best = -1.0f;
            for (k = 0; k < 3; ++k) {
                float v[3] = { D[0 * 3 + k] + (k == 0), D[1 * 3 + k] + (k == 1), D[2 * 3 + k] + (k == 2) };
                float m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (m > best && m > 1e-4f) {
                    best = m;
                    g_lift.axis[0] = v[0] / m; g_lift.axis[1] = v[1] / m; g_lift.axis[2] = v[2] / m;
                }
            }
        }
    } else {
        for (k = 0; k < 3; ++k)
            g_lift.axis[k] /= l;
    }
    g_lift.chassis = chassis;
    g_lift.lift = cg_height - c.pos[1];
    memcpy(g_lift.Rc0, Rc, sizeof(g_lift.Rc0));
    g_lift.wheel[0] = g_lift.wheel[1] = -1;
    g_lift.ichassis = -1;
    g_lift_need_max = 0.0f;
    g_lift_need_logged = 0;
    g_lift_raised_logged = 0;
    g_keep_total = 0.0f;
    g_lift_log = log;
    memcpy(g_lift.pivot, c.pos, sizeof(g_lift.pivot));
    n = list_bodies(chassis, list, 64, NULL);
    unlock_bodies(); /* jamais laisser des pièces bloquées d'une prise précédente */
    memset(g_lift.inv_saved, 0, sizeof(g_lift.inv_saved));
    g_lift.n = 0;
    for (i = 0; i < n; ++i) {
        BodyState s;
        float m = 0.0f, dx, dy, dz;
        if (!read_state(list[i], &s))
            continue;
        safe_read((const void *)(list[i] + OFF_MASS), &m, 4);
        dx = s.pos[0] - c.pos[0];
        dy = s.pos[1] - c.pos[1];
        dz = s.pos[2] - c.pos[2];
        if (dx * dx + dy * dy + dz * dz > 2.5f * 2.5f)
            continue;
        if (list[i] != chassis && m > 50.0f)
            continue; /* le corps du pilote (80 kg) n'est pas une pièce de la moto */
        (void)exclude; /* le pilote est déjà écarté par sa masse ; la distance écartait des pièces de la moto */
        if (list[i] == chassis)
            g_lift.ichassis = g_lift.n;
        g_lift.body[g_lift.n] = list[i];
        g_lift.start[g_lift.n] = s;
        g_lift.steer[g_lift.n] = 0.0f;
        ++g_lift.n;
    }
    /* Direction : à la chute, le guidon reste braqué. Axe de direction en repère moto
       (x droite, y haut, z avant) : incliné de 25° vers l'arrière ; tête de fourche à
       0,35 m au-dessus et 0,40 m devant le centre de gravité du châssis. Pour chaque pièce,
       l'angle de braquage = rotation de son axe x (droite) autour de l'axe de direction ;
       les roues et le bras oscillant tournent autour de x et gardent un angle nul. */
    {
        const float sl[3] = { 0.0f, 0.906f, -0.423f }, hl[3] = { 0.0f, 0.35f, 0.40f };
        for (j = 0; j < 3; ++j) {
            g_lift.steer_axis[j] = Rc[j * 3 + 0] * sl[0] + Rc[j * 3 + 1] * sl[1] + Rc[j * 3 + 2] * sl[2];
            g_lift.head[j] = c.pos[j] + Rc[j * 3 + 0] * hl[0] + Rc[j * 3 + 1] * hl[1] + Rc[j * 3 + 2] * hl[2];
        }
        for (i = 0; i < g_lift.n; ++i) {
            const BodyState *s = &g_lift.start[i];
            float xb[3], xl[3], a, b2;
            /* axe x de la pièce, exprimé dans le repère du châssis */
            for (j = 0; j < 3; ++j)
                xb[j] = s->R[j * 4 + 0];
            for (j = 0; j < 3; ++j)
                xl[j] = Rc[0 * 3 + j] * xb[0] + Rc[1 * 3 + j] * xb[1] + Rc[2 * 3 + j] * xb[2];
            /* projeté sur le plan perpendiculaire à l'axe de direction : angle depuis x */
            {
                float d = xl[0] * sl[0] + xl[1] * sl[1] + xl[2] * sl[2];
                float p0 = xl[0] - d * sl[0], p1 = xl[1] - d * sl[1], p2 = xl[2] - d * sl[2];
                /* base du plan : e1 = x, e2 = s x e1 */
                float e2[3] = { sl[1] * 0.0f - sl[2] * 0.0f, sl[2] * 1.0f - sl[0] * 0.0f, sl[0] * 0.0f - sl[1] * 1.0f };
                a = p0;
                b2 = p0 * 0.0f + p1 * e2[1] + p2 * e2[2] + p0 * e2[0];
                g_lift.steer[i] = atan2f(b2, a);
            }
            if (fabsf(g_lift.steer[i]) < 0.05f || fabsf(g_lift.steer[i]) > 1.4f)
                g_lift.steer[i] = 0.0f; /* pas une pièce qui braque (ou mesure douteuse) */
        }
    }
    /* Roues hors du sol une fois debout. Couchée, la suspension est détendue : les roues pendent
       plus bas que sur une moto posée, elles restaient dans la terre et creusaient (v0.40.12).
       Les deux pièces les plus basses à l'arrivée sont les roues (rayon ~0,36 m) */
    if (g_ground && g_lift.n > 1) {
        float Dfull[9], low_y[2] = { 1e9f, 1e9f }, low_x[2] = { 0 }, low_z[2] = { 0 }, extra = -1e9f;
        int w;
        axis_angle_R(g_lift.axis, g_lift.angle, Dfull);
        for (i = 0; i < g_lift.n; ++i) {
            const BodyState *s = &g_lift.start[i];
            float rel[3], p[3];
            if (g_lift.body[i] == chassis)
                continue;
            for (j = 0; j < 3; ++j)
                rel[j] = s->pos[j] - g_lift.pivot[j];
            for (j = 0; j < 3; ++j)
                p[j] = g_lift.pivot[j] + Dfull[j * 3 + 0] * rel[0] + Dfull[j * 3 + 1] * rel[1] + Dfull[j * 3 + 2] * rel[2];
            p[1] += g_lift.lift;
            if (p[1] < low_y[0]) {
                low_y[1] = low_y[0]; low_x[1] = low_x[0]; low_z[1] = low_z[0];
                g_lift.wheel[1] = g_lift.wheel[0];
                low_y[0] = p[1]; low_x[0] = p[0]; low_z[0] = p[2];
                g_lift.wheel[0] = i;
            } else if (p[1] < low_y[1]) {
                low_y[1] = p[1]; low_x[1] = p[0]; low_z[1] = p[2];
                g_lift.wheel[1] = i;
            }
        }
        for (w = 0; w < 2; ++w) {
            float gy;
            if (low_y[w] < 1e8f && g_ground(low_x[w], low_z[w], &gy)) {
                /* v0.40.14 : rayon 0,40 m et 8 cm de marge (0,36 + 2 cm : la roue arrière touchait encore,
                   terre projetée dans le clip) */
                float need = (g_wheel_h > 0.0f ? gy + g_wheel_h + 0.03f : gy + WHEEL_R + 0.03f) - low_y[w];
                if (log)
                    log("moto : roue %d a l'arrivee %.2f m au-dessus du sol actuel (centre)", w + 1, low_y[w] - gy);
                if (need > extra)
                    extra = need;
            }
        }
        if (extra > 0.6f)
            extra = 0.6f;
        if (extra > 0.0f) {
            g_lift.lift += extra;
            if (log)
                log("moto : montee de %.2f m de plus pour que les roues ne touchent pas le sol", extra);
        } else if (extra < -0.02f && extra > -1e8f && g_ground_trusted) {
            /* v1.7.2 : les deux roues arrivaient au-dessus du sol (0,42 / 0,48 m pour un rayon de
               0,345 m) : la moto tenue flottait. On la descend jusqu'à la roue la plus basse. */
            if (extra < -0.30f)
                extra = -0.30f;
            g_lift.lift += extra;
            if (log)
                log("moto : descendue de %.2f m pour que les roues touchent le sol", -extra);
        }
    }
    g_lift.active = g_lift.n > 0;
    if (log)
        log("moto : on la releve (cap %.0f deg, pente %.0f deg, rotation %.0f deg, monte de %.2f m, %d pieces)",
            h * 57.2958f, pitch * 57.2958f, g_lift.angle * 57.2958f, g_lift.lift, g_lift.n);
    return g_lift.active;
}

static float g_lift_last_t = 0.0f;
/* v1.8.20 : remet la moto à la dernière position du geste (juste avant chaque image) */
void bike_lift_reapply(void)
{
    bike_lift_apply(g_lift_last_t);
}

void bike_lift_apply(float t)
{
    float D[9];
    int i, j, k;
    if (!g_lift.active)
        return;
    g_lift_last_t = t;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    t = t * t * (3.0f - 2.0f * t); /* départ et arrivée en douceur */
    /* v0.40.57 : une moto couchée est lourde à décoller, puis vient plus facilement */
    t = powf(t, 1.4f);
    if (t >= 1.0f) {
        /* debout : où sont vraiment les roues (la physique a pu les faire descendre) ? */
        float pos[64][3];
        int m = 0;
        for (i = 0; i < g_lift.n && m < 64; ++i) {
            BodyState cur;
            if (g_lift.body[i] == g_lift.chassis || !read_state(g_lift.body[i], &cur))
                continue;
            pos[m][0] = cur.pos[0];
            pos[m][1] = cur.pos[1];
            pos[m][2] = cur.pos[2];
            ++m;
        }
        keep_wheels_up((const float (*)[3])pos, m, g_lift_log);
    }
    axis_angle_R(g_lift.axis, g_lift.angle * t, D);
    /* v0.41.01 : pendant la rotation, le bord du pneu descend (roue couchée : on touche par le
       flanc ; roue à moitié relevée : par la bande de roulement, plus bas). La bosse fixe de
       15 cm ne suffisait pas sur certaines pentes : la roue creusait des trous. On calcule, à
       chaque image, le point le plus bas de chaque roue et on monte la moto de ce qu'il faut. */
    {
        float bump = 0.15f * sinf(3.14159265f * t), need = 0.0f;
        float r = g_wheel_h > 0.0f ? g_wheel_h : WHEEL_R, ay = 0.0f;
        int w;
        for (j = 0; j < 3; ++j) /* axe droite (colonne 0) du châssis à cet instant : D x Rc0 */
            ay += D[1 * 3 + j] * g_lift.Rc0[j * 3 + 0];
        if (ay < 0.0f)
            ay = -ay;
        if (ay > 1.0f)
            ay = 1.0f;
        for (w = 0; w < 2 && g_ground; ++w) {
            int b = g_lift.wheel[w];
            float rel[3], p[3], gy, low;
            if (b < 0)
                continue;
            for (j = 0; j < 3; ++j)
                rel[j] = g_lift.start[b].pos[j] - g_lift.pivot[j];
            for (j = 0; j < 3; ++j)
                p[j] = g_lift.pivot[j] + D[j * 3 + 0] * rel[0] + D[j * 3 + 1] * rel[1] + D[j * 3 + 2] * rel[2];
            p[1] += g_lift.lift * t;
            if (!g_ground(p[0], p[2], &gy))
                continue;
            /* disque de rayon r, axe incliné de ay sur la verticale, pneu de 12 cm de large */
            low = p[1] - r * sqrtf(1.0f - ay * ay) - 0.06f * ay;
            if (gy + 0.03f - low > need)
                need = gy + 0.03f - low;
        }
        /* au tout début la roue est déjà posée : on ne la décolle que progressivement */
        if (t < 0.15f)
            need *= t / 0.15f;
        g_lift_extra = need > bump ? need : bump;
        if (g_lift_extra > 0.8f)
            g_lift_extra = 0.8f;
        if (need > g_lift_need_max)
            g_lift_need_max = need;
        if (t >= 1.0f && !g_lift_need_logged && g_lift_log) {
            g_lift_need_logged = 1;
            g_lift_log("moto : relevee, roues gardees hors du sol pendant le geste (jusqu'a %.2f m de plus, bosse fixe 0.15 m)",
                       g_lift_need_max);
        }
    }
    for (i = 0; i < g_lift.n; ++i) {
        BodyState s = g_lift.start[i];
        float rel[3], R3[9], R2[9];
        unsigned flags = 0;
        if (g_lift.steer[i] != 0.0f) {
            /* on remet le guidon droit pendant qu'on relève la moto */
            float S[9], h[3], Rs[9];
            axis_angle_R(g_lift.steer_axis, -g_lift.steer[i] * t, S);
            for (j = 0; j < 3; ++j)
                h[j] = s.pos[j] - g_lift.head[j];
            for (j = 0; j < 3; ++j)
                s.pos[j] = g_lift.head[j] + S[j * 3 + 0] * h[0] + S[j * 3 + 1] * h[1] + S[j * 3 + 2] * h[2];
            for (j = 0; j < 3; ++j)
                for (k = 0; k < 3; ++k)
                    Rs[j * 3 + k] = S[j * 3 + 0] * s.R[0 * 4 + k] + S[j * 3 + 1] * s.R[1 * 4 + k] + S[j * 3 + 2] * s.R[2 * 4 + k];
            for (j = 0; j < 3; ++j)
                for (k = 0; k < 3; ++k)
                    s.R[j * 4 + k] = Rs[j * 3 + k];
        }
        for (j = 0; j < 3; ++j)
            rel[j] = s.pos[j] - g_lift.pivot[j];
        for (j = 0; j < 3; ++j)
            s.pos[j] = g_lift.pivot[j] + D[j * 3 + 0] * rel[0] + D[j * 3 + 1] * rel[1] + D[j * 3 + 2] * rel[2];
        /* + bosse au milieu du geste : en se redressant, les roues décrivent un arc autour du
           châssis et passaient sous le sol (trous dans la piste) ; on la soulève un peu plus
           pendant la rotation (0 au départ et à l'arrivée) */
        s.pos[1] += g_lift.lift * t + g_lift_extra;
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                R3[j * 3 + k] = s.R[j * 4 + k];
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                R2[j * 3 + k] = D[j * 3 + 0] * R3[0 * 3 + k] + D[j * 3 + 1] * R3[1 * 3 + k] + D[j * 3 + 2] * R3[2 * 3 + k];
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                s.R[j * 4 + k] = R2[j * 3 + k];
        quat_from_R(R2, s.q);
        memset(s.lvel, 0, sizeof(s.lvel));
        memset(s.avel, 0, sizeof(s.avel));
        clear_forces(g_lift.body[i]);
        if (safe_write((void *)(g_lift.body[i] + OFF_POS), &s, sizeof(s)) &&
            safe_read((const void *)(g_lift.body[i] + OFF_FLAGS), &flags, 4)) {
            /* gelée : vitesse nulle d'un pas à l'autre, le jeu voit une moto immobile */
            /* v0.40.13 : plus jamais « désactivé » (physique qui explose à la reprise) ; à la place,
               sans gravité tant qu'on la tient : les roues ne retombent plus (suspension
               qui se détend) entre deux corrections et ne touchent plus le sol */
            unsigned nf = (flags & ~(unsigned)BODY_DISABLED);
            nf = g_freeze ? (nf | BODY_NOGRAVITY) : (nf & ~(unsigned)BODY_NOGRAVITY);
            if (nf != flags)
                safe_write((void *)(g_lift.body[i] + OFF_FLAGS), &nf, 4);
        }
    }
    if (g_freeze)
        lock_bodies(g_lift_log);
}

/* Forces accumulées : ODE ne les remet à zéro que pour les corps actifs. Pièces en pause, le
   jeu continue d'en ajouter (moteur, suspensions) ; à la reprise, des minutes de forces
   partaient d'un coup et la physique explosait (jeu planté, v0.40.9). */
static void clear_forces(uintptr_t b)
{
    static const float zero[8] = { 0 };
    safe_write((void *)(b + OFF_FACC), zero, sizeof(zero));
}

#define OFF_INVI    0x84
#define OFF_INVMASS 0xB4
static int g_lock_logged = 0;
/* pièces « immobiles » pour la physique : masse et inertie inverses à zéro */
static void lock_bodies(BikeLogFn log)
{
    int i, done = 0;
    for (i = 0; i < g_lift.n; ++i) {
        float m = 0.0f, inv = 0.0f;
        static const float zero[12] = { 0 };
        if (g_lift.inv_saved[i])
            continue;
        if (!safe_read((const void *)(g_lift.body[i] + OFF_MASS), &m, 4) ||
            !safe_read((const void *)(g_lift.body[i] + OFF_INVMASS), &inv, 4) ||
            !safe_read((const void *)(g_lift.body[i] + OFF_INVI), g_lift.inv_I[i], sizeof(g_lift.inv_I[i])))
            continue;
        /* disposition vérifiée : masse x masse inverse = 1 */
        if (!(m > 0.1f && m < 1000.0f && fabsf(m * inv - 1.0f) < 0.01f)) {
            g_lift.inv_saved[i] = -1;
            if (log && g_lock_logged < 3) {
                ++g_lock_logged;
                log("moto : piece %d, masse %.2f / masse inverse %.4f : disposition inattendue, pas bloquee", i, m, inv);
            }
            continue;
        }
        g_lift.inv_mass[i] = inv;
        if (safe_write((void *)(g_lift.body[i] + OFF_INVMASS), zero, 4) &&
            safe_write((void *)(g_lift.body[i] + OFF_INVI), zero, sizeof(zero))) {
            g_lift.inv_saved[i] = 1;
            ++done;
        }
    }
    if (done && log && g_lock_logged < 6) {
        ++g_lock_logged;
        log("moto : %d piece(s) bloquees (suspensions figees tant qu'on la tient)", done);
    }
}

/* v0.41.26 : on n'écrit dans une pièce que si c'est encore elle (corps ODE valide, masse qui va
   avec la masse inverse gardée). Après un retour aux stands ou un changement de piste, les
   pièces sont libérées : écrire dedans abîmait la mémoire du jeu (crash plus tard dans le
   dessin ou le relief, dumps du joueur du 10/11 à 23:58 et 00:02) */
static int body_still_ours(int i)
{
    float m = 0.0f;
    if (!body_ok(g_lift.body[i]))
        return 0;
    if (g_lift.inv_saved[i] == 1)
        return safe_read((const void *)(g_lift.body[i] + OFF_MASS), &m, 4) && m > 0.1f && m < 1000.0f &&
               fabsf(m * g_lift.inv_mass[i] - 1.0f) < 0.01f;
    return 1;
}

static void unlock_bodies(void)
{
    int i;
    for (i = 0; i < g_lift.n; ++i)
        if (g_lift.inv_saved[i] == 1 && !body_still_ours(i)) {
            g_lift.inv_saved[i] = 0;
        } else if (g_lift.inv_saved[i] == 1) {
            safe_write((void *)(g_lift.body[i] + OFF_INVI), g_lift.inv_I[i], sizeof(g_lift.inv_I[i]));
            safe_write((void *)(g_lift.body[i] + OFF_INVMASS), &g_lift.inv_mass[i], 4);
            g_lift.inv_saved[i] = 0;
        } else if (g_lift.inv_saved[i] < 0)
            g_lift.inv_saved[i] = 0;
}

static void unfreeze_all(void)
{
    int i;
    unlock_bodies();
    for (i = 0; i < g_lift.n; ++i) {
        unsigned flags = 0;
        static const float zero[8] = { 0 };
        if (!body_still_ours(i))
            continue;
        clear_forces(g_lift.body[i]);
        safe_write((void *)(g_lift.body[i] + OFF_LVEL), zero, sizeof(zero)); /* vitesses nulles */
        if (safe_read((const void *)(g_lift.body[i] + OFF_FLAGS), &flags, 4) && (flags & (BODY_DISABLED | BODY_NOGRAVITY))) {
            flags &= ~(unsigned)(BODY_DISABLED | BODY_NOGRAVITY);
            safe_write((void *)(g_lift.body[i] + OFF_FLAGS), &flags, 4);
        }
    }
}

void bike_lift_end(void)
{
    unlock_bodies(); /* toujours : masses remises avant de lâcher */
    if (g_freeze)
        unfreeze_all(); /* on dégèle les pièces avant de lâcher */
    g_freeze = 0;
    g_lift.active = 0;
}

/* oublie la moto tenue sans rien écrire (ses pièces n'existent peut-être plus) */
void bike_lift_forget(void)
{
    memset(&g_lift, 0, sizeof(g_lift));
    g_freeze = 0;
}

int bike_lift_frozen(void) { return g_lift.active && g_freeze; }
int bike_lift_bodies(uintptr_t *out, int max)
{
    int i;
    for (i = 0; i < g_lift.n && i < max; ++i)
        out[i] = g_lift.body[i];
    return i;
}

int bike_lift_active(void)
{
    return g_lift.active;
}

/* ---------- remonter : replacer la moto (et le pilote) après le respawn ---------- */

static struct {
    int ok;
    float pos[3];
    float R[9];
} g_target;

int bike_capture_target(uintptr_t chassis)
{
    BodyState c;
    int i, j;
    g_target.ok = 0;
    if (!chassis || !read_state(chassis, &c))
        return 0;
    memcpy(g_target.pos, c.pos, sizeof(g_target.pos));
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            g_target.R[i * 3 + j] = c.R[i * 4 + j];
    g_target.ok = 1;
    return 1;
}

int bike_move_to_target(uintptr_t chassis, BikeLogFn log)
{
    /* Après le respawn, le jeu remet la moto droite sur ses roues, presque au même endroit.
       On ne corrige que sa direction (rotation autour de la verticale) et sa place à plat ;
       jamais l'inclinaison ni la hauteur : une moto penchée ou trop basse a les roues dans le
       sol, la physique la rejette et creuse d'énormes trous dans la piste (v0.40.3). */
    uintptr_t list[64];
    BodyState c;
    float D[9], dist2, a, ca, sa, hc, ht, tx, tz;
    int i, j, k, n, moved = 0;
    if (!g_target.ok || !chassis || !read_state(chassis, &c))
        return 0;
    dist2 = (c.pos[0] - g_target.pos[0]) * (c.pos[0] - g_target.pos[0]) +
            (c.pos[2] - g_target.pos[2]) * (c.pos[2] - g_target.pos[2]);
    if (dist2 > 1.5f * 1.5f) {
        /* le jeu l'a remise ailleurs : on la laisse où il l'a mise */
        if (log)
            log("moto : respawn a %.1f m de l'endroit releve, on laisse le jeu la placer", sqrtf(dist2));
        g_target.ok = 0;
        return 0;
    }
    /* cap (axe avant) de la moto du jeu et de celle relevée */
    hc = atan2f(c.R[0 * 4 + 2], c.R[2 * 4 + 2]);
    ht = atan2f(g_target.R[0 * 3 + 2], g_target.R[2 * 3 + 2]);
    a = ht - hc;
    while (a > 3.14159265f)
        a -= 6.2831853f;
    while (a < -3.14159265f)
        a += 6.2831853f;
    if (dist2 < 0.6f * 0.6f && fabsf(a) < 0.25f) {
        /* déjà au bon endroit et dans le bon sens : on n'y touche pas */
        if (log)
            log("moto : respawn au bon endroit (%.1f m, %.0f deg), laissee au jeu", sqrtf(dist2), a * 57.3f);
        g_target.ok = 0;
        return 1;
    }
    ca = cosf(a);
    sa = sinf(a);
    D[0] = ca;  D[1] = 0.0f; D[2] = sa;
    D[3] = 0.0f; D[4] = 1.0f; D[5] = 0.0f;
    D[6] = -sa; D[7] = 0.0f; D[8] = ca;
    tx = g_target.pos[0];
    tz = g_target.pos[2];
    n = list_bodies(chassis, list, 64, NULL);
    for (i = 0; i < n; ++i) {
        BodyState s;
        float rel[3], R3[9], R2[9];
        if (!read_state(list[i], &s))
            continue;
        for (j = 0; j < 3; ++j)
            rel[j] = s.pos[j] - c.pos[j];
        if (rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] > 4.0f * 4.0f)
            continue; /* pas sur la moto */
        s.pos[0] = tx + D[0] * rel[0] + D[2] * rel[2];
        s.pos[1] = c.pos[1] + rel[1]; /* hauteur du jeu */
        s.pos[2] = tz + D[6] * rel[0] + D[8] * rel[2];
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                R3[j * 3 + k] = s.R[j * 4 + k];
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                R2[j * 3 + k] = D[j * 3 + 0] * R3[0 * 3 + k] + D[j * 3 + 1] * R3[1 * 3 + k] + D[j * 3 + 2] * R3[2 * 3 + k];
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                s.R[j * 4 + k] = R2[j * 3 + k];
        quat_from_R(R2, s.q);
        memset(s.lvel, 0, sizeof(s.lvel));
        memset(s.avel, 0, sizeof(s.avel));
        if (safe_write((void *)(list[i] + OFF_POS), &s, sizeof(s)))
            ++moved;
    }
    if (log)
        log("moto : tournee de %.0f deg et replacee a plat (%.1f m depuis le respawn), %d corps", a * 57.3f,
            sqrtf(dist2), moved);
    /* une seule fois : on la laisse ensuite au jeu */
    g_target.ok = 0;
    return moved;
}

void bike_clear_target(void)
{
    g_target.ok = 0;
}

int bike_has_target(void)
{
    return g_target.ok;
}

/* Fait basculer la moto sur le côté (rotation autour de axis, rad/s, pivot au sol) : sert à
   descendre « en tombant doucement » quand on s'arrête (le jeu passe alors en chute) */
int bike_tip(uintptr_t chassis, const float axis[3], float rate, float pivot_drop)
{
    uintptr_t list[64];
    BodyState c;
    float w[3], piv[3];
    int i, k, n, done = 0;
    if (!chassis || !read_state(chassis, &c))
        return 0;
    for (k = 0; k < 3; ++k) {
        w[k] = axis[k] * rate;
        piv[k] = c.pos[k];
    }
    piv[1] -= pivot_drop;
    n = list_bodies(chassis, list, 64, NULL);
    for (i = 0; i < n; ++i) {
        BodyState s;
        float r[3];
        unsigned flags = 0;
        if (!read_state(list[i], &s))
            continue;
        for (k = 0; k < 3; ++k)
            r[k] = s.pos[k] - piv[k];
        if (r[0] * r[0] + r[1] * r[1] + r[2] * r[2] > 3.0f * 3.0f)
            continue;
        s.lvel[0] = w[1] * r[2] - w[2] * r[1];
        s.lvel[1] = w[2] * r[0] - w[0] * r[2];
        s.lvel[2] = w[0] * r[1] - w[1] * r[0];
        s.avel[0] = w[0];
        s.avel[1] = w[1];
        s.avel[2] = w[2];
        if (safe_write((void *)(list[i] + OFF_LVEL), s.lvel, sizeof(s.lvel)) &&
            safe_write((void *)(list[i] + OFF_AVEL), s.avel, sizeof(s.avel)))
            ++done;
        if (safe_read((const void *)(list[i] + OFF_FLAGS), &flags, 4) && (flags & BODY_DISABLED)) {
            flags &= ~(unsigned)BODY_DISABLED;
            safe_write((void *)(list[i] + OFF_FLAGS), &flags, 4);
        }
    }
    return done;
}

/* v1.8.89 : moto (et pilote) freinés : vitesse horizontale de toutes les pièces à moins de 2,5 m du
   châssis multipliée par k (choc dans un objet lourd). Rend le nombre de pièces */
int bike_slow(uintptr_t chassis, float k)
{
    uintptr_t list[64];
    BodyState c, s;
    int i, n, done = 0;
    if (!chassis || !read_state(chassis, &c))
        return 0;
    n = list_bodies(chassis, list, 64, NULL);
    for (i = 0; i < n; ++i) {
        float dx, dy, dz, v[2];
        if (!read_state(list[i], &s))
            continue;
        dx = s.pos[0] - c.pos[0];
        dy = s.pos[1] - c.pos[1];
        dz = s.pos[2] - c.pos[2];
        if (dx * dx + dy * dy + dz * dz > 2.5f * 2.5f)
            continue;
        v[0] = s.lvel[0] * k;
        v[1] = s.lvel[2] * k;
        if (safe_write((void *)(list[i] + OFF_LVEL), &v[0], 4) && safe_write((void *)(list[i] + OFF_LVEL + 8), &v[1], 4))
            ++done;
    }
    return done;
}

/* v1.8.96 : moto (et pilote) sortis d'un obstacle : pièces à moins de 2,5 m du châssis déplacées de
   (dx, dz), et leur vitesse vers l'obstacle (contre la normale (nx, nz)) annulée. Nombre de pièces */
int bike_block(uintptr_t chassis, float dx, float dz, float nx, float nz)
{
    uintptr_t list[64];
    BodyState c, s;
    int i, n, done = 0;
    if (!chassis || !read_state(chassis, &c))
        return 0;
    n = list_bodies(chassis, list, 64, NULL);
    for (i = 0; i < n; ++i) {
        float ddx, ddy, ddz, vn, p[2], v[2];
        if (!read_state(list[i], &s))
            continue;
        ddx = s.pos[0] - c.pos[0];
        ddy = s.pos[1] - c.pos[1];
        ddz = s.pos[2] - c.pos[2];
        if (ddx * ddx + ddy * ddy + ddz * ddz > 2.5f * 2.5f)
            continue;
        p[0] = s.pos[0] + dx;
        p[1] = s.pos[2] + dz;
        vn = s.lvel[0] * nx + s.lvel[2] * nz;
        v[0] = s.lvel[0];
        v[1] = s.lvel[2];
        if (vn < 0.0f) {
            v[0] -= nx * vn * 1.15f; /* un petit rebond */
            v[1] -= nz * vn * 1.15f;
        }
        if (safe_write((void *)(list[i] + OFF_POS), &p[0], 4) && safe_write((void *)(list[i] + OFF_POS + 8), &p[1], 4) &&
            safe_write((void *)(list[i] + OFF_LVEL), &v[0], 4) && safe_write((void *)(list[i] + OFF_LVEL + 8), &v[1], 4))
            ++done;
    }
    return done;
}

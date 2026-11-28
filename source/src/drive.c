/*
 * drive.c - pilote le squelette 3D du pilote (rider.hrc) avec le ragdoll
 */
#include "drive.h"
#include "hook.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ---------- disposition mémoire (beta21e, voir docs/notes-mxbikes-beta21e.md) ---------- */

#define OFF_MODEL_WORLD 0x11C   /* matrice monde 3x4 par lignes du modèle */
#define OFF_LODS        0x1F0   /* 4 pointeurs de LOD */
#define LOD_COUNT       0x10
#define LOD_ARRAY       0x18
#define NODE_SIZE       0x3F8
#define NODE_LOCAL      0x68    /* matrice locale 4x4 par lignes */
#define NODE_WORLD      0x100   /* matrice monde 3x4 par lignes + (0 0 0 1) */

#define MAX_LODS  4
#define MAX_BONES 384 /* mods de pilote : Rider+ 102 os, Rider+ Mobius 224 */

/* ---------- petites maths : matrices 4x4 par lignes [R | t] ---------- */

typedef struct { float m[16]; } M4;
typedef struct { float v[3]; } V3;

static void m4_identity(M4 *a)
{
    memset(a, 0, sizeof(*a));
    a->m[0] = a->m[5] = a->m[10] = a->m[15] = 1.0f;
}

static void m4_mul(const M4 *a, const M4 *b, M4 *out)
{
    M4 t;
    int r, c, k;
    for (r = 0; r < 4; ++r)
        for (c = 0; c < 4; ++c) {
            float v = 0.0f;
            for (k = 0; k < 4; ++k)
                v += a->m[r * 4 + k] * b->m[k * 4 + c];
            t.m[r * 4 + c] = v;
        }
    *out = t;
}

/* inverse d'une transformation rigide */
static void m4_inv_rigid(const M4 *a, M4 *out)
{
    M4 t;
    int r, c;
    m4_identity(&t);
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 3; ++c)
            t.m[r * 4 + c] = a->m[c * 4 + r];
    for (r = 0; r < 3; ++r)
        t.m[r * 4 + 3] = -(t.m[r * 4 + 0] * a->m[3] + t.m[r * 4 + 1] * a->m[7] +
                           t.m[r * 4 + 2] * a->m[11]);
    *out = t;
}

static V3 m4_pos(const M4 *a)
{
    V3 p = { { a->m[3], a->m[7], a->m[11] } };
    return p;
}

static V3 m4_axis_x(const M4 *a)
{
    V3 p = { { a->m[0], a->m[4], a->m[8] } };
    return p;
}

static V3 v3(const float *p)
{
    V3 r = { { p[0], p[1], p[2] } };
    return r;
}

static V3 v_sub(V3 a, V3 b)
{
    V3 r = { { a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2] } };
    return r;
}

static V3 v_add_scaled(V3 a, V3 b, float s)
{
    V3 r = { { a.v[0] + b.v[0] * s, a.v[1] + b.v[1] * s, a.v[2] + b.v[2] * s } };
    return r;
}

static float v_dot(V3 a, V3 b)
{
    return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2];
}

static V3 v_cross(V3 a, V3 b)
{
    V3 r = { { a.v[1] * b.v[2] - a.v[2] * b.v[1], a.v[2] * b.v[0] - a.v[0] * b.v[2],
               a.v[0] * b.v[1] - a.v[1] * b.v[0] } };
    return r;
}

static V3 v_norm(V3 a)
{
    float l = sqrtf(v_dot(a, a));
    if (l < 1e-8f) {
        V3 x = { { 1.0f, 0.0f, 0.0f } };
        return x;
    }
    a.v[0] /= l;
    a.v[1] /= l;
    a.v[2] /= l;
    return a;
}

/* rotation pure (3x3 dans une M4, translation nulle) */
static V3 rot_apply(const M4 *r, V3 a)
{
    V3 o;
    int i;
    for (i = 0; i < 3; ++i)
        o.v[i] = r->m[i * 4] * a.v[0] + r->m[i * 4 + 1] * a.v[1] + r->m[i * 4 + 2] * a.v[2];
    return o;
}

static void rot_only(const M4 *a, M4 *out)
{
    *out = *a;
    out->m[3] = out->m[7] = out->m[11] = 0.0f;
}

/* Repère orthonormé : x selon a, z perpendiculaire à (a, b). Colonnes x, y, z. */
static void frame(V3 a, V3 b, M4 *out)
{
    V3 x = v_norm(a);
    V3 z = v_norm(v_cross(x, b));
    V3 y = v_cross(z, x);
    m4_identity(out);
    out->m[0] = x.v[0]; out->m[1] = y.v[0]; out->m[2] = z.v[0];
    out->m[4] = x.v[1]; out->m[5] = y.v[1]; out->m[6] = z.v[1];
    out->m[8] = x.v[2]; out->m[9] = y.v[2]; out->m[10] = z.v[2];
}

/* Plus petite rotation qui amène la direction u sur v (vecteurs unitaires) */
static __attribute__((unused)) void rot_between(V3 u, V3 v, M4 *out)
{
    V3 k = v_cross(u, v);
    float c = v_dot(u, v);
    m4_identity(out);
    if (c < -0.9999f) {
        /* demi-tour autour d'un axe perpendiculaire */
        V3 p = fabsf(u.v[0]) < 0.9f ? (V3){ { 1, 0, 0 } } : (V3){ { 0, 1, 0 } };
        V3 a = v_norm(v_cross(u, p));
        int r, cc;
        for (r = 0; r < 3; ++r)
            for (cc = 0; cc < 3; ++cc)
                out->m[r * 4 + cc] = 2.0f * a.v[r] * a.v[cc] - (r == cc ? 1.0f : 0.0f);
        return;
    }
    {
        /* Rodrigues : R = I + [k] + [k]^2 / (1 + c) */
        float f = 1.0f / (1.0f + c);
        float K[9] = { 0, -k.v[2], k.v[1], k.v[2], 0, -k.v[0], -k.v[1], k.v[0], 0 };
        int r, cc, i;
        for (r = 0; r < 3; ++r)
            for (cc = 0; cc < 3; ++cc) {
                float k2 = 0.0f;
                for (i = 0; i < 3; ++i)
                    k2 += K[r * 3 + i] * K[i * 3 + cc];
                out->m[r * 4 + cc] = (r == cc ? 1.0f : 0.0f) + K[r * 3 + cc] + k2 * f;
            }
    }
}

/* out = a x transpose(b), rotations seulement */
static void rot_mul_t(const M4 *a, const M4 *b, M4 *out)
{
    M4 bt;
    int r, c;
    m4_identity(&bt);
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 3; ++c)
            bt.m[r * 4 + c] = b->m[c * 4 + r];
    m4_mul(a, &bt, out);
}

/* ---------- hiérarchie connue (relevée en jeu, beta21e) ---------- */

typedef struct {
    int index;
    const char *name;
    int parent;
    float t[3];
} KnownBone;

static const KnownBone k_known[] = {
#include "rider_skeleton_beta21e.inc"
};
#define NUM_KNOWN ((int)(sizeof(k_known) / sizeof(k_known[0])))

/* Nom du parent d'un os d'après la hiérarchie connue, NULL si racine ou inconnu */
static const char *known_parent_name(const char *name)
{
    int i;
    for (i = 0; i < NUM_KNOWN; ++i)
        if (!strcmp(k_known[i].name, name))
            return k_known[i].parent >= 0 ? k_known[k_known[i].parent].name : NULL;
    return NULL;
}

static int known_bone(const char *name)
{
    int i;
    for (i = 0; i < NUM_KNOWN; ++i)
        if (!strcmp(k_known[i].name, name))
            return 1;
    return 0;
}

/* Autres modèles de pilote : même squelette, mais un nom d'os peut différer par la casse ou le
   préfixe (« RiderRig_Head », « rider_rig:Head »...). On le ramène au nom connu, par la fin du
   nom (après le dernier « _ » ou « : ») comparée sans tenir compte de la casse. */
static int same_nocase(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* rôle d'un os par la fin de son nom (après le dernier « _ », « : » ou « | »), sans la casse */
static int role_by_tail(const char *name, const char *const *names, int n)
{
    const char *tail = name, *p;
    int b;
    for (p = name; *p; ++p)
        if (*p == '_' || *p == ':' || *p == '|')
            tail = p + 1;
    for (b = 0; b < n; ++b) {
        const char *bt = strncmp(names[b], "riderRIG_", 9) ? names[b] : names[b] + 9;
        if (same_nocase(name, names[b]) || same_nocase(tail, bt))
            return b;
    }
    return -1;
}

/* ---------- os pilotés ---------- */

enum {
    B_ROOT, B_SPINE1, B_SPINE2, B_SPINE3, B_SPINE4, B_NECK1, B_NECK2, B_HEAD,
    B_SHOULDER_L, B_ELBOW_L, B_WRIST_L, B_SHOULDER_R, B_ELBOW_R, B_WRIST_R,
    B_HIP_L, B_KNEE_L, B_ANKLE_L, B_HIP_R, B_KNEE_R, B_ANKLE_R, B_PELVIS,
    B_COUNT
};

static const char *k_bone_names[B_COUNT] = {
    "riderRIG_Root", "riderRIG_Spine1", "riderRIG_Spine2", "riderRIG_Spine3", "riderRIG_Spine4",
    "riderRIG_Neck1", "riderRIG_Neck2", "riderRIG_Head",
    "riderRIG_LeftShoulder", "riderRIG_LeftElbow", "riderRIG_LeftWrist",
    "riderRIG_RightShoulder", "riderRIG_RightElbow", "riderRIG_RightWrist",
    "riderRIG_LeftHip", "riderRIG_LeftKnee", "riderRIG_LeftAnkle",
    "riderRIG_RightHip", "riderRIG_RightKnee", "riderRIG_RightAnkle", "riderRIG_Pelvis",
};

/* Os dont on impose la rotation monde (les autres suivent leur parent) */
static const int k_driven[] = {
    B_ROOT, B_SPINE1, B_SPINE2, B_SPINE3, B_SPINE4, B_NECK1, B_NECK2, B_HEAD,
    B_SHOULDER_L, B_ELBOW_L, B_SHOULDER_R, B_ELBOW_R, B_HIP_L, B_KNEE_L, B_HIP_R, B_KNEE_R,
};
#define NUM_DRIVEN ((int)(sizeof(k_driven) / sizeof(k_driven[0])))

#define HEAD_CENTER 0.12f /* centre de la tête, le long de l'axe X de l'os Head */

typedef struct {
    uint8_t *arr;
    int count;
    char name[MAX_BONES][40];
    int parent[MAX_BONES];
    int role[MAX_BONES];      /* B_* ou -1 */
    M4 base_local[MAX_BONES]; /* matrices locales au départ, remises à la fin */
    float fist_axis[MAX_BONES][3]; /* doigts : axe de flexion dans le repère de l'os */
    float fist_angle[MAX_BONES];   /* doigts : flexion à ajouter pour fermer le poing, rad */
} Lod;

/* État du pilotage d'UN squelette. Plusieurs places : 0 = le joueur, les autres = pilotes
   des autres joueurs (réseau). Chaque fil choisit la sienne (drive_select), 0 par défaut. */
#define HIST 12
typedef struct {
    volatile float g_fist;
    /* dernière matrice locale écrite par os (LOD 0), pour voir si le jeu la réécrit pendant sa mise à jour */
    M4 g_written[MAX_BONES];
    int g_written_ok[MAX_BONES];
    M4 g_expect_w[MAX_BONES]; /* matrices monde attendues (LOD 0) d'après nos locales */
    int g_expect_n; /* 0 = main de la pose du jeu, 1 = poing fermé */
    
    void *g_skeleton;
    Lod g_lods[MAX_LODS];
    int g_num_lods;
    M4 g_r0[B_COUNT];      /* rotation monde de chaque os piloté au départ */
    int g_have[B_COUNT];
    M4 g_f0_pelvis, g_f0_torso, g_f0_head;
    V3 g_u0[B_COUNT];      /* direction des membres au départ */
    /* membres : repère (direction de l'os, axe de pliage) au départ, pour éviter qu'ils vrillent */
    M4 g_fl0[B_COUNT];
    V3 g_hinge0[4];        /* axe de pliage au départ : jambe G, jambe D, bras G, bras D */
    /* pieds : repère (talon -> pointe, axe de la cheville) au départ, et pilotage */
    M4 g_ffoot0[2];
    int g_foot_ok;
    volatile int g_feet_on;
    float g_feet_dir[2][3];
    /* mains : repère (poignet -> majeur, poignet -> pouce) au départ, et pilotage en marchant */
    M4 g_fhand0[2];
    int g_hand_ok;
    /* repère de la marche (droite, avant) : tête de niveau et mains orientées */
    volatile int g_walk_frame_on;
    volatile int g_head_on;
    float g_head_dir[3], g_head_right[3];
    float g_walk_right[3], g_walk_fwd[3];
    volatile int g_grip_on;    /* mains au guidon : orientées pour serrer la poignée */
    float g_grip_bar[3];       /* axe du guidon, de la main gauche vers la main droite */
    float g_root_err;
    int g_is_mod;        /* modèle de pilote d'un mod (rider.edf chargé directement) */
    float g_root_up;   /* hauteur ajoutée au pilote (menu P, par modèle) */
    int g_root_from_node; /* 1 = racine lue dans le nœud (si modèle x locale ne colle pas) */
    int g_restore_pending;
    float g_model_y;
    V3 g_body_prev;          /* position du corps de chute du jeu à l'image précédente */
    float g_body_still; /* depuis combien de temps il ne bouge plus, s */
    V3 g_body_vel;             /* vitesse lissée du corps de chute du jeu, m/s */
    int g_slope_seen;      /* une vraie glissade sur une pente a été vue */
    
    /* positions du corps de chute du jeu, une tous les 0,1 s, sur 1,2 s */
    float g_hist_t[HIST];
    V3 g_hist_p[HIST];
    int g_hist_n;
    int g_fist_disagree;
    M4 g_wrist_keep[MAX_LODS][2];
    int g_wrist_keep_ok[MAX_LODS][2];
} DriveState;
static DriveState g_slots[DRIVE_SLOTS];
static __thread DriveState *g_cur_slot = NULL;
#define DS (g_cur_slot ? g_cur_slot : &g_slots[0])
#define g_fist (DS->g_fist)
#define g_written (DS->g_written)
#define g_written_ok (DS->g_written_ok)
#define g_expect_w (DS->g_expect_w)
#define g_expect_n (DS->g_expect_n)
#define g_skeleton (DS->g_skeleton)
#define g_lods (DS->g_lods)
#define g_num_lods (DS->g_num_lods)
#define g_r0 (DS->g_r0)
#define g_have (DS->g_have)
#define g_f0_pelvis (DS->g_f0_pelvis)
#define g_f0_torso (DS->g_f0_torso)
#define g_f0_head (DS->g_f0_head)
#define g_u0 (DS->g_u0)
#define g_fl0 (DS->g_fl0)
#define g_hinge0 (DS->g_hinge0)
#define g_ffoot0 (DS->g_ffoot0)
#define g_foot_ok (DS->g_foot_ok)
#define g_feet_on (DS->g_feet_on)
#define g_feet_dir (DS->g_feet_dir)
#define g_fhand0 (DS->g_fhand0)
#define g_hand_ok (DS->g_hand_ok)
#define g_walk_frame_on (DS->g_walk_frame_on)
#define g_head_on (DS->g_head_on)
#define g_head_dir (DS->g_head_dir)
#define g_head_right (DS->g_head_right)
#define g_walk_right (DS->g_walk_right)
#define g_walk_fwd (DS->g_walk_fwd)
#define g_grip_on (DS->g_grip_on)
#define g_grip_bar (DS->g_grip_bar)
#define g_root_err (DS->g_root_err)
#define g_is_mod (DS->g_is_mod)
#define g_root_up (DS->g_root_up)
#define g_root_from_node (DS->g_root_from_node)
#define g_restore_pending (DS->g_restore_pending)
#define g_model_y (DS->g_model_y)
#define g_body_prev (DS->g_body_prev)
#define g_body_still (DS->g_body_still)
#define g_body_vel (DS->g_body_vel)
#define g_slope_seen (DS->g_slope_seen)
#define g_hist_t (DS->g_hist_t)
#define g_hist_p (DS->g_hist_p)
#define g_hist_n (DS->g_hist_n)
#define g_fist_disagree (DS->g_fist_disagree)
#define g_wrist_keep (DS->g_wrist_keep)
#define g_wrist_keep_ok (DS->g_wrist_keep_ok)
static DriveLogFn g_log = NULL;
void drive_set_model(int is_mod, float up) { g_is_mod = is_mod; g_root_up = up; }
void drive_select(int slot) { g_cur_slot = (slot > 0 && slot < DRIVE_SLOTS) ? &g_slots[slot] : NULL; }

static void body_history_push(float t, V3 p)
{
    int i;
    if (g_hist_n > 0 && t - g_hist_t[g_hist_n - 1] < 0.1f)
        return;
    if (g_hist_n == HIST) {
        for (i = 1; i < HIST; ++i) {
            g_hist_t[i - 1] = g_hist_t[i];
            g_hist_p[i - 1] = g_hist_p[i];
        }
        --g_hist_n;
    }
    g_hist_t[g_hist_n] = t;
    g_hist_p[g_hist_n] = p;
    ++g_hist_n;
}

/* position du corps à l'instant t (l'échantillon le plus proche, à 0,06 s près) */
static int body_history_at(float t, V3 *out)
{
    int i, best = -1;
    float bd = 0.06f;
    for (i = 0; i < g_hist_n; ++i)
        if (fabsf(g_hist_t[i] - t) < bd) {
            bd = fabsf(g_hist_t[i] - t);
            best = i;
        }
    if (best < 0)
        return 0;
    *out = g_hist_p[best];
    return 1;
}

float drive_model_y(void)
{
    return g_model_y;
}

void drive_init(DriveLogFn log)
{
    g_log = log;
    g_skeleton = NULL;
    g_num_lods = 0;
}

void *drive_skeleton(void)
{
    return g_skeleton;
}

float drive_root_error(void)
{
    return g_root_err;
}

static int looks_like_pointer(uint64_t v)
{
    return v > 0x10000ull && v < 0x00007FFFFFFFFFFFull && (v & 7) == 0;
}

static int read_model_world(const void *skeleton, M4 *out)
{
    float w[12];
    if (!safe_read((const uint8_t *)skeleton + OFF_MODEL_WORLD, w, sizeof(w)))
        return 0;
    m4_identity(out);
    memcpy(out->m, w, sizeof(w));
    return isfinite(w[3]) && isfinite(w[7]) && isfinite(w[11]);
}

static int read_node(uint8_t *node, M4 *local, M4 *world, char *name, size_t name_size)
{
    float w[12];
    size_t i;
    if (local && !safe_read(node + NODE_LOCAL, local->m, sizeof(local->m)))
        return 0;
    if (world) {
        if (!safe_read(node + NODE_WORLD, w, sizeof(w)))
            return 0;
        m4_identity(world);
        memcpy(world->m, w, sizeof(w));
    }
    if (name) {
        uint8_t raw[40];
        if (!safe_read(node, raw, sizeof(raw)))
            return 0;
        for (i = 0; i + 1 < name_size && i < sizeof(raw) && raw[i] >= 32 && raw[i] < 127; ++i)
            name[i] = (char)raw[i];
        name[i] = '\0';
    }
    return 1;
}

/* Prépare un LOD : os, rôles, parents (retrouvés par monde = monde(parent) x locale) */
/* ---------- poings ----------
 * Pour chaque doigt (os 1, 2, 3), l'axe de flexion vient de la pose du jeu (les doigts tiennent
 * déjà le guidon, donc sont un peu pliés) : axe = (segment 1) x (segment 2). On ajoute à chaque
 * phalange ce qui manque pour un poing fermé. */

static int find_bone(const Lod *lod, const char *name)
{
    int k;
    for (k = 0; k < lod->count; ++k)
        if (!strcmp(lod->name[k], name))
            return k;
    return -1;
}

static int finger_of(const char *name, int *seg)
{
    static const char *k_f[] = { "Index", "Middle", "Ring", "Pink", "Thumb" };
    const char *s = strstr(name, "riderRIG_");
    int f;
    if (!s)
        return -1;
    s += 9;
    if (!strncmp(s, "Left", 4))
        s += 4;
    else if (!strncmp(s, "Right", 5))
        s += 5;
    else
        return -1;
    for (f = 0; f < 5; ++f) {
        size_t l = strlen(k_f[f]);
        if (!strncmp(s, k_f[f], l) && s[l] >= '1' && s[l] <= '3' && s[l + 1] == 0) {
            *seg = s[l] - '0';
            return f;
        }
    }
    return -1;
}

static int child_of(const Lod *lod, int k)
{
    int c;
    for (c = 0; c < lod->count; ++c)
        if (lod->parent[c] == k)
            return c;
    return -1;
}

static float angle_between(V3 a, V3 b)
{
    float c = v_dot(v_norm(a), v_norm(b));
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c);
}

static void setup_fist(Lod *lod, const M4 *worlds)
{
    /* flexion totale visée par phalange (poing fermé), degrés */
    static const float k_target[5][3] = {
        { 85, 100, 65 }, { 85, 100, 65 }, { 85, 100, 65 }, { 85, 100, 65 }, { 0, 35, 45 } };
    int k;
    for (k = 0; k < lod->count; ++k) {
        int seg = 0, f = finger_of(lod->name[k], &seg), s1 = k, s2, s3;
        V3 p1, p2, p3, axis, ax_local, din, dout;
        M4 r;
        float cur, add;
        lod->fist_angle[k] = 0.0f;
        if (f < 0 || f == 4)
            continue; /* le pouce garde la pose du jeu (il est déjà replié sur la poignée) */
        /* remonte au 1er os du doigt */
        while (s1 >= 0) {
            int sg = 0;
            if (finger_of(lod->name[s1], &sg) != f || sg == 1)
                break;
            s1 = lod->parent[s1];
        }
        if (s1 < 0 || lod->parent[s1] < 0)
            continue;
        s2 = child_of(lod, s1);
        s3 = s2 >= 0 ? child_of(lod, s2) : -1;
        if (s2 < 0 || s3 < 0)
            continue;
        p1 = m4_pos(&worlds[s1]);
        p2 = m4_pos(&worlds[s2]);
        p3 = m4_pos(&worlds[s3]);
        {
            /* sens de flexion : vers la paume. La paume est du côté du pouce (repère de la main :
               doigts, travers index -> auriculaire, normale vers le pouce). Pour le pouce : vers
               l'auriculaire. Si le doigt est déjà plié dans la pose du jeu, on garde son vrai axe. */
            char side[8], nm[40];
            int wrist = lod->parent[s1], i1, p1b, t1b;
            V3 d = v_norm(v_sub(p2, p1)), across, palm, curl;
            const char *rest = lod->name[k] + 9;
            strcpy(side, !strncmp(rest, "Left", 4) ? "Left" : "Right");
            snprintf(nm, sizeof(nm), "riderRIG_%sIndex1", side);
            i1 = find_bone(lod, nm);
            snprintf(nm, sizeof(nm), "riderRIG_%sPink1", side);
            p1b = find_bone(lod, nm);
            snprintf(nm, sizeof(nm), "riderRIG_%sThumb1", side);
            t1b = find_bone(lod, nm);
            if (i1 < 0 || p1b < 0 || t1b < 0 || wrist < 0)
                continue;
            across = v_norm(v_sub(m4_pos(&worlds[p1b]), m4_pos(&worlds[i1])));
            if (f == 4) {
                palm = v_sub(m4_pos(&worlds[p1b]), p1); /* le pouce se replie vers l'auriculaire */
            } else {
                palm = v_sub(m4_pos(&worlds[t1b]), m4_pos(&worlds[wrist]));
                palm = v_add_scaled(palm, across, -v_dot(palm, across));
            }
            palm = v_add_scaled(palm, d, -v_dot(palm, d));
            if (sqrtf(v_dot(palm, palm)) < 1e-6f)
                continue;
            axis = v_norm(v_cross(d, v_norm(palm)));
            curl = v_cross(v_sub(p2, p1), v_sub(p3, p2));
            if (sqrtf(v_dot(curl, curl)) > 0.15f * sqrtf(v_dot(v_sub(p2, p1), v_sub(p2, p1))) *
                                               sqrtf(v_dot(v_sub(p3, p2), v_sub(p3, p2)))) {
                /* doigt déjà plié dans la pose du jeu (autour de la poignée) : c'est forcément vers
                   la paume, on suit son vrai axe */
                curl = v_norm(curl);
                if (v_dot(curl, axis) < 0.0f)
                    ++g_fist_disagree;
                axis = curl;
            }
        }
        /* flexion actuelle à cette articulation */
        din = v_sub(m4_pos(&worlds[k]), m4_pos(&worlds[lod->parent[k]]));
        {
            int c = child_of(lod, k);
            if (c >= 0) {
                dout = v_sub(m4_pos(&worlds[c]), m4_pos(&worlds[k]));
                cur = angle_between(din, dout);
            } else {
                cur = angle_between(v_sub(p2, p1), v_sub(p3, p2)) * 0.6f; /* dernière phalange */
            }
        }
        add = k_target[f][seg - 1] * 0.0174533f - cur;
        if (add <= 0.0f)
            continue;
        /* axe dans le repère de l'os : R^T x axe */
        rot_only(&worlds[k], &r);
        ax_local.v[0] = r.m[0] * axis.v[0] + r.m[4] * axis.v[1] + r.m[8] * axis.v[2];
        ax_local.v[1] = r.m[1] * axis.v[0] + r.m[5] * axis.v[1] + r.m[9] * axis.v[2];
        ax_local.v[2] = r.m[2] * axis.v[0] + r.m[6] * axis.v[1] + r.m[10] * axis.v[2];
        ax_local = v_norm(ax_local);
        lod->fist_axis[k][0] = ax_local.v[0];
        lod->fist_axis[k][1] = ax_local.v[1];
        lod->fist_axis[k][2] = ax_local.v[2];
        lod->fist_angle[k] = add;
    }
}

/* rotation d'angle a autour de l'axe unitaire u (M4, lignes) */
static void axis_rot(const float u[3], float a, M4 *out)
{
    float c = cosf(a), s = sinf(a), t = 1.0f - c;
    m4_identity(out);
    out->m[0] = t * u[0] * u[0] + c;
    out->m[1] = t * u[0] * u[1] - s * u[2];
    out->m[2] = t * u[0] * u[2] + s * u[1];
    out->m[4] = t * u[0] * u[1] + s * u[2];
    out->m[5] = t * u[1] * u[1] + c;
    out->m[6] = t * u[1] * u[2] - s * u[0];
    out->m[8] = t * u[0] * u[2] - s * u[1];
    out->m[9] = t * u[1] * u[2] + s * u[0];
    out->m[10] = t * u[2] * u[2] + c;
}

void drive_set_fist(float amount)
{
    g_fist = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
}

static int g_dump_bones = 0; /* v1.1.2 : hiérarchie corrigée : liste des os dans le journal */

static int setup_lod(const void *skeleton, int lod_i, Lod *lod, M4 *worlds)
{
    uint64_t lp = 0, arr = 0;
    int32_t count = 0;
    static M4 locals[MAX_BONES];
    int k, p, b;

    if (!safe_read((const uint8_t *)skeleton + OFF_LODS + 8 * lod_i, &lp, sizeof(lp)) ||
        !looks_like_pointer(lp))
        return 0;
    if (!safe_read((const uint8_t *)(uintptr_t)lp + LOD_COUNT, &count, sizeof(count)) ||
        !safe_read((const uint8_t *)(uintptr_t)lp + LOD_ARRAY, &arr, sizeof(arr)))
        return 0;
    if (count < 3 || count > MAX_BONES || !looks_like_pointer(arr)) {
        if (g_log && lod_i == 0)
            g_log("pilotage : LOD 0 refuse (%d noeuds, max %d)", count, MAX_BONES);
        return 0;
    }

    lod->arr = (uint8_t *)(uintptr_t)arr;
    lod->count = count;
    for (k = 0; k < count; ++k) {
        char name[40];
        if (!read_node(lod->arr + (size_t)k * NODE_SIZE, &locals[k], &worlds[k], name,
                       sizeof(name))) {
            if (g_log && lod_i == 0)
                g_log("pilotage : LOD 0, noeud %d sur %d illisible", k, count);
            return 0;
        }
        lod->base_local[k] = locals[k];
        memcpy(lod->name[k], name, sizeof(lod->name[k]));
        lod->role[k] = -1;
        for (b = 0; b < B_COUNT; ++b)
            if (!strcmp(name, k_bone_names[b]))
                lod->role[k] = b;
    }
    /* parents : d'après la hiérarchie connue (par nom). Pour un os inconnu (autre modèle de
     * pilote), l'os précédent qui explique le mieux la matrice monde. */
    for (k = 0; k < count; ++k) {
        float best = 1e9f;
        lod->parent[k] = -1;
        if (known_bone(lod->name[k])) {
            const char *pn = known_parent_name(lod->name[k]);
            float ek = 1e9f, eb = 1e9f;
            int bp = -1;
            if (pn) {
                /* le plus proche avant lui (certains mods ont deux squelettes à la suite :
                   chacun garde ses parents) ; « point01 » chez certains mods */
                for (p = k - 1; p >= 0 && lod->parent[k] < 0; --p)
                    if (same_nocase(lod->name[p], pn))
                        lod->parent[k] = p;
                for (p = k + 1; p < count && lod->parent[k] < 0; ++p)
                    if (same_nocase(lod->name[p], pn))
                        lod->parent[k] = p;
            }
            /* autre modèle de pilote : un os intermédiaire peut s'intercaler (le parent connu
               n'est plus le vrai) ; on garde le parent connu s'il explique la matrice monde,
               sinon l'os qui l'explique bien mieux. Pilote de base : rien ne change. */
            if (lod->parent[k] >= 0 || pn) {
                for (p = 0; p < count; ++p) {
                    M4 t;
                    float e = 0.0f;
                    int i;
                    if (p == k)
                        continue;
                    m4_mul(&worlds[p], &locals[k], &t);
                    for (i = 0; i < 12; ++i)
                        e += fabsf(t.m[i] - worlds[k].m[i]);
                    if (p == lod->parent[k])
                        ek = e;
                    if (e < eb) {
                        eb = e;
                        bp = p;
                    }
                }
                /* v0.41.25 : jamais un ancêtre du parent connu (même chaîne d'os). Pose où la
                   colonne est droite au départ : riderRIG_armour rattaché à Spine1 au lieu de
                   Spine4 ; à pied, la protection et le haut du sweat restaient en bas et le
                   casque flottait au-dessus (capture du joueur, une partie sur deux) */
                if (bp >= 0 && pn) {
                    const char *an = pn;
                    int guard = 0;
                    while (an && guard++ < 64) {
                        if (same_nocase(lod->name[bp], an)) {
                            if (g_log && lod_i == 0 && bp != lod->parent[k] && ek > 0.05f && eb < 0.02f && eb < ek * 0.3f)
                                g_log("pilotage : parent de %s : %s garde (%s est un ancetre, ignore)", lod->name[k],
                                      lod->parent[k] >= 0 ? lod->name[lod->parent[k]] : pn, lod->name[bp]);
                            bp = -1;
                            break;
                        }
                        an = known_parent_name(an);
                    }
                }
                if (bp >= 0 && bp != lod->parent[k] && ek > 0.05f && eb < 0.02f && eb < ek * 0.3f) {
                    if (g_log && lod_i == 0)
                        g_log("pilotage : parent de %s : %s (au lieu de %s)", lod->name[k], lod->name[bp],
                              lod->parent[k] >= 0 ? lod->name[lod->parent[k]] : "aucun");
                    lod->parent[k] = bp;
                }
            }
            continue;
        }
        for (p = 0; p < k; ++p) {
            M4 t;
            float e = 0.0f;
            int i;
            m4_mul(&worlds[p], &locals[k], &t);
            for (i = 0; i < 12; ++i)
                e += fabsf(t.m[i] - worlds[k].m[i]);
            if (e < best) {
                best = e;
                lod->parent[k] = p;
            }
        }
        if (best > 0.05f)
            lod->parent[k] = -1; /* racine (Point01) ou relation inconnue */
    }
    /* v1.1.2 : pas de boucle dans la hiérarchie. Rider+RolledUp : 0 racine dans le journal, donc
       au moins une boucle (riderRIG_Root et Point01 parents l'un de l'autre ?) : les matrices
       calculées n'avaient plus de sens, pilote invisible au sol. Dans une boucle, on coupe le
       lien vers un parent placé plus loin dans la liste (les os sont calculés dans l'ordre). */
    for (k = 0; k < count; ++k) {
        int j = lod->parent[k], steps = 0;
        while (j >= 0 && j != k && steps++ <= count)
            j = lod->parent[j];
        if (j == k) {
            int c = k, cut = k;
            steps = 0;
            do {
                if (lod->parent[c] > c) {
                    cut = c;
                    break;
                }
                c = lod->parent[c];
            } while (c != k && steps++ <= count);
            if (g_log && lod_i == 0)
                g_log("pilotage : boucle dans les parents, %s n'a plus de parent (etait %s)", lod->name[cut],
                      lod->name[lod->parent[cut]]);
            lod->parent[cut] = -1;
            g_dump_bones = 1;
        }
    }
    setup_fist(lod, worlds);
    return 1;
}

/* v1.8.79 : os de la tête au départ (pose du jeu) et maintenant (nos matrices) : le casque suit */
static int g_head_spawn_n = 0;
int drive_head_frames(float spawn[12], float now[12])
{
    int k;
    if (!g_head_spawn_n || !g_have[B_HEAD] || g_num_lods < 1)
        return 0;
    for (k = 0; k < g_expect_n && k < MAX_BONES; ++k)
        if (g_lods[0].role[k] == B_HEAD) {
            memcpy(spawn, g_r0[B_HEAD].m, 12 * sizeof(float));
            memcpy(now, g_expect_w[k].m, 12 * sizeof(float));
            return g_head_spawn_n;
        }
    return 0;
}

int drive_spawn(void *skeleton, Ragdoll *rd, const float vel[3], float ground_y)
{
    static M4 worlds[MAX_BONES];
    float pts[RD_NUM_POINTS][3];
    V3 P[RD_NUM_POINTS];
    M4 world_b[B_COUNT], model;
    int k, b, i, missing = 0;

    g_skeleton = NULL;
    g_num_lods = 0;
    memset(g_have, 0, sizeof(g_have));

    for (i = 0; i < MAX_LODS; ++i) {
        static M4 lod_worlds[MAX_BONES];
        Lod *lod = &g_lods[g_num_lods];
        if (!setup_lod(skeleton, i, lod, i == 0 ? worlds : lod_worlds))
            continue;
        ++g_num_lods;
    }
    if (g_num_lods == 0) {
        if (g_log)
            g_log("pilotage : aucun LOD lisible");
        return 0;
    }
    /* Les LOD moins détaillés ne sont pas mis à jour quand on est près du pilote : leurs
     * matrices monde sont anciennes. On reprend donc leurs parents du LOD 0, par nom. */
    for (i = 1; i < g_num_lods; ++i) {
        Lod *lod = &g_lods[i];
        for (k = 0; k < lod->count; ++k) {
            int k0, p0, p;
            lod->parent[k] = -1;
            for (k0 = 0; k0 < g_lods[0].count; ++k0)
                if (!strcmp(g_lods[0].name[k0], lod->name[k]))
                    break;
            if (k0 == g_lods[0].count || (p0 = g_lods[0].parent[k0]) < 0)
                continue;
            for (p = 0; p < k; ++p)
                if (!strcmp(lod->name[p], g_lods[0].name[p0]))
                    lod->parent[k] = p;
        }
    }

    /* LOD 0 : rotations et positions de départ des os pilotés */
    for (k = 0; k < g_lods[0].count; ++k) {
        b = g_lods[0].role[k];
        if (b < 0)
            continue;
        world_b[b] = worlds[k];
        rot_only(&worlds[k], &g_r0[b]);
        g_have[b] = 1;
    }
    /* autre modèle de pilote : os manquants cherchés par la fin du nom, sans la casse (on ne
       touche jamais à un modèle où tous les os ont leur nom exact) */
    for (b = 0; b < B_COUNT; ++b)
        if (!g_have[b])
            break;
    if (b < B_COUNT) {
        for (k = 0; k < g_lods[0].count; ++k) {
            int r;
            if (g_lods[0].role[k] >= 0)
                continue;
            r = role_by_tail(g_lods[0].name[k], k_bone_names, B_COUNT);
            if (r < 0 || g_have[r])
                continue;
            g_lods[0].role[k] = r;
            world_b[r] = worlds[k];
            rot_only(&worlds[k], &g_r0[r]);
            g_have[r] = 1;
            if (g_log)
                g_log("pilotage : os %s pris pour %s", g_lods[0].name[k], k_bone_names[r]);
        }
    }
    for (b = 0; b < B_COUNT; ++b)
        if (!g_have[b]) {
            ++missing;
            if (g_log)
                g_log("pilotage : os %s introuvable", k_bone_names[b]);
        }
    if (missing)
        return 0;

    /* contrôle : monde(Point01) = matrice du modèle x locale(Point01) ? */
    g_root_err = 0.0f;
    if (read_model_world(skeleton, &model)) {
        M4 pred;
        m4_mul(&model, &g_lods[0].base_local[0], &pred);
        for (i = 0; i < 12; ++i)
            g_root_err += fabsf(pred.m[i] - worlds[0].m[i]);
    }
    /* l'écart vient surtout du décalage d'une image entre la matrice du modèle (déjà mise à
     * jour) et les os (image précédente) : on ne lit le nœud que si l'écart est énorme */
    /* pilote de base : modèle x locale colle (écart ~0,001). Certains mods (rider.edf chargé
       directement) ont une matrice de modèle qui ne porte pas la racine (écart 0,07 à 0,18 :
       pilote enfoncé dans le sol) : on lit alors la racine dans son nœud */
    g_root_from_node = g_is_mod && g_root_err > 0.05f; /* (pilote de base : l'écart vient du décalage d'une image) */

    /* points du ragdoll = positions réelles des os */
    P[RD_PELVIS] = m4_pos(&world_b[B_PELVIS]);
    P[RD_CHEST] = m4_pos(&world_b[B_SPINE4]);
    P[RD_NECK] = m4_pos(&world_b[B_NECK1]);
    P[RD_HEAD] = v_add_scaled(m4_pos(&world_b[B_HEAD]), m4_axis_x(&world_b[B_HEAD]), HEAD_CENTER);
    P[RD_SHOULDER_L] = m4_pos(&world_b[B_SHOULDER_L]);
    P[RD_ELBOW_L] = m4_pos(&world_b[B_ELBOW_L]);
    P[RD_HAND_L] = m4_pos(&world_b[B_WRIST_L]);
    P[RD_SHOULDER_R] = m4_pos(&world_b[B_SHOULDER_R]);
    P[RD_ELBOW_R] = m4_pos(&world_b[B_ELBOW_R]);
    P[RD_HAND_R] = m4_pos(&world_b[B_WRIST_R]);
    P[RD_HIP_L] = m4_pos(&world_b[B_HIP_L]);
    P[RD_KNEE_L] = m4_pos(&world_b[B_KNEE_L]);
    P[RD_FOOT_L] = m4_pos(&world_b[B_ANKLE_L]);
    P[RD_HIP_R] = m4_pos(&world_b[B_HIP_R]);
    P[RD_KNEE_R] = m4_pos(&world_b[B_KNEE_R]);
    P[RD_FOOT_R] = m4_pos(&world_b[B_ANKLE_R]);
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        if (!isfinite(P[i].v[0]) || !isfinite(P[i].v[1]) || !isfinite(P[i].v[2])) {
            if (g_log)
                g_log("pilotage : point %d du squelette invalide", i);
            return 0;
        }
        memcpy(pts[i], P[i].v, sizeof(pts[i]));
    }

    /* repères de départ */
    frame(v_sub(P[RD_CHEST], P[RD_PELVIS]), v_sub(P[RD_HIP_R], P[RD_HIP_L]), &g_f0_pelvis);
    frame(v_sub(P[RD_NECK], P[RD_PELVIS]), v_sub(P[RD_SHOULDER_R], P[RD_SHOULDER_L]), &g_f0_torso);
    {
        /* repère de départ de la tête : son axe de côté à elle (et non la ligne des épaules) ;
           sinon une tête tournée au moment du départ (chute, pose du jeu) restait tournée
           pendant toute la marche */
        V3 sh = v_sub(P[RD_SHOULDER_R], P[RD_SHOULDER_L]), side = sh;
        const M4 *hm = &world_b[B_HEAD];
        V3 ay = { { hm->m[1], hm->m[5], hm->m[9] } }, az = { { hm->m[2], hm->m[6], hm->m[10] } };
        float dy = v_dot(ay, sh), dz = v_dot(az, sh), ls = sqrtf(v_dot(sh, sh)) + 1e-6f;
        /* v1.8.68 : tête tournée de côté en roulant (le joueur regarde à gauche / à droite) : l'axe
           de côté de la tête n'est plus le long des épaules, l'ancien choix prenait l'axe avant et
           la tête restait tournée après F ou une chute. L'axe de côté de la tête (et son sens) est
           appris quand la tête est droite (axe Z, vers la droite, sur le pilote rider.hrc) */
        /* v1.8.81 : axe de côté de la tête = +Y, toujours (mesuré en jeu : sur la moto, regard
           devant, l'axe Y de l'os de la tête va vers la droite du pilote). Avec Z (v1.8.75 à 1.8.80),
           la tête à pied était tournée de 90° vers la gauche, et le casque avec elle. La 1.8.68
           choisissait Z quand la tête était tournée au départ : elle restait tournée */
        static const int s_axis = 1, s_sign = 1; /* 1 = y, 2 = z */
        (void)dy;
        (void)dz;
        (void)ls;
        ++g_head_spawn_n;
        {
            V3 ax = s_axis == 2 ? az : ay;
            side = s_sign > 0 ? ax : v_add_scaled(ax, ax, -2.0f);
        }
        frame(v_sub(P[RD_HEAD], P[RD_NECK]), side, &g_f0_head);
    }
    g_u0[B_SHOULDER_L] = v_norm(v_sub(P[RD_ELBOW_L], P[RD_SHOULDER_L]));
    g_u0[B_ELBOW_L] = v_norm(v_sub(P[RD_HAND_L], P[RD_ELBOW_L]));
    g_u0[B_SHOULDER_R] = v_norm(v_sub(P[RD_ELBOW_R], P[RD_SHOULDER_R]));
    g_u0[B_ELBOW_R] = v_norm(v_sub(P[RD_HAND_R], P[RD_ELBOW_R]));
    g_u0[B_HIP_L] = v_norm(v_sub(P[RD_KNEE_L], P[RD_HIP_L]));
    g_u0[B_KNEE_L] = v_norm(v_sub(P[RD_FOOT_L], P[RD_KNEE_L]));
    g_u0[B_HIP_R] = v_norm(v_sub(P[RD_KNEE_R], P[RD_HIP_R]));
    g_u0[B_KNEE_R] = v_norm(v_sub(P[RD_FOOT_R], P[RD_KNEE_R]));
    {
        /* axes de pliage (genou, coude) ; membre tendu : axe gauche-droite du bassin / des épaules */
        static const int seg[4][4] = {
            { RD_HIP_L, RD_KNEE_L, RD_FOOT_L, B_HIP_L }, { RD_HIP_R, RD_KNEE_R, RD_FOOT_R, B_HIP_R },
            { RD_SHOULDER_L, RD_ELBOW_L, RD_HAND_L, B_SHOULDER_L },
            { RD_SHOULDER_R, RD_ELBOW_R, RD_HAND_R, B_SHOULDER_R },
        };
        V3 lat_hips = v_norm(v_sub(P[RD_HIP_R], P[RD_HIP_L]));
        V3 lat_sh = v_norm(v_sub(P[RD_SHOULDER_R], P[RD_SHOULDER_L]));
        int s;
        for (s = 0; s < 4; ++s) {
            V3 a = v_sub(P[seg[s][1]], P[seg[s][0]]), b = v_sub(P[seg[s][2]], P[seg[s][1]]);
            V3 h = v_cross(a, b);
            V3 lat = s < 2 ? lat_hips : lat_sh;
            if (sqrtf(v_dot(h, h)) < 0.15f * sqrtf(v_dot(a, a)) * sqrtf(v_dot(b, b)))
                h = lat;
            else if (v_dot(h, lat) < 0.0f)
                h = v_add_scaled((V3){ { 0, 0, 0 } }, h, -1.0f);
            g_hinge0[s] = v_norm(h);
            frame(a, g_hinge0[s], &g_fl0[seg[s][3]]);
            frame(b, g_hinge0[s], &g_fl0[seg[s][3] + 1]);
        }
    }
    /* pieds : direction cheville -> pointe d'après l'os Toe */
    g_foot_ok = 0;
    {
        int kl = -1, kr = -1;
        for (k = 0; k < g_lods[0].count; ++k) {
            if (!strcmp(g_lods[0].name[k], "riderRIG_LeftToe"))
                kl = k;
            else if (!strcmp(g_lods[0].name[k], "riderRIG_RightToe"))
                kr = k;
        }
        if (kl >= 0 && kr >= 0) {
            frame(v_sub(m4_pos(&worlds[kl]), P[RD_FOOT_L]), g_hinge0[0], &g_ffoot0[0]);
            frame(v_sub(m4_pos(&worlds[kr]), P[RD_FOOT_R]), g_hinge0[1], &g_ffoot0[1]);
            g_foot_ok = 1;
        }
    }
    /* mains : direction des doigts (majeur) et du pouce, d'après les os de la main */
    g_hand_ok = 0;
    {
        int ml = -1, mr = -1, tl = -1, tr = -1;
        for (k = 0; k < g_lods[0].count; ++k) {
            const char *nm = g_lods[0].name[k];
            if (!strcmp(nm, "riderRIG_LeftMiddle1"))
                ml = k;
            else if (!strcmp(nm, "riderRIG_RightMiddle1"))
                mr = k;
            else if (!strcmp(nm, "riderRIG_LeftThumb1"))
                tl = k;
            else if (!strcmp(nm, "riderRIG_RightThumb1"))
                tr = k;
        }
        if (ml >= 0 && mr >= 0 && tl >= 0 && tr >= 0) {
            frame(v_sub(m4_pos(&worlds[ml]), P[RD_HAND_L]), v_sub(m4_pos(&worlds[tl]), P[RD_HAND_L]), &g_fhand0[0]);
            frame(v_sub(m4_pos(&worlds[mr]), P[RD_HAND_R]), v_sub(m4_pos(&worlds[tr]), P[RD_HAND_R]), &g_fhand0[1]);
            g_hand_ok = 1;
        }
    }

    /* le sol ne peut pas être au-dessus du pilote : au plus sous son point le plus bas */
    {
        float lowest = 1e9f;
        for (k = 0; k < g_lods[0].count; ++k)
            if (worlds[k].m[7] < lowest)
                lowest = worlds[k].m[7];
        lowest -= rd->cfg.point_radius;
        if (lowest < ground_y) {
            if (g_log)
                g_log("pilotage : sol estime %.2f au-dessus du pilote, ramene a %.2f", ground_y,
                      lowest);
            ground_y = lowest;
        }
    }
    rd_spawn_points(rd, (const float(*)[3])pts, vel, ground_y);
    g_skeleton = skeleton;
    if (read_model_world(skeleton, &model))
        g_body_prev = m4_pos(&model);
    g_body_still = 0.0f;
    memset(&g_body_vel, 0, sizeof(g_body_vel));
    g_hist_n = 0;
    g_slope_seen = 0;
    g_restore_pending = 0;

    if (g_log) {
        int l;
        g_log("pilotage : depart, %d LOD, erreur racine %.4f%s, bassin (%.2f %.2f %.2f), sol %.2f",
              g_num_lods, g_root_err, g_root_from_node ? " (racine lue dans le noeud)" : "",
              P[RD_PELVIS].v[0], P[RD_PELVIS].v[1], P[RD_PELVIS].v[2], ground_y);
        for (l = 0; l < g_num_lods; ++l) {
            int roles = 0, roots = 0;
            for (k = 0; k < g_lods[l].count; ++k) {
                roles += g_lods[l].role[k] >= 0;
                roots += g_lods[l].parent[k] < 0;
            }
            {
                int fingers = 0, j;
                for (j = 0; j < g_lods[l].count; ++j)
                    fingers += g_lods[l].fist_angle[j] > 0.0f;
                g_log("  LOD %d : %d os, %d reconnus, %d racine(s), %d phalanges pour les poings (%d axes pris sur la pose)",
                      l, g_lods[l].count, roles, roots, fingers, g_fist_disagree);
            static int dumped = 0; /* v1.8.116 : une fois par partie (plus de 100 lignes à chaque chute : à-coup) */
            if (l == g_num_lods - 1 && g_log && (g_dump_bones || roots == 0) && !dumped) {
                g_dump_bones = 0;
                dumped = 1;
                drive_log_nodes(g_log);
            }
            }
        }
    }
    return 1;
}

/* Rotations monde voulues pour chaque os piloté, à partir des points du ragdoll */
static void desired_rotations(const Ragdoll *rd, M4 *des)
{
    V3 P[RD_NUM_POINTS];
    M4 f, d_pelvis, d_torso, d_head, q, dp;
    int i;
    for (i = 0; i < RD_NUM_POINTS; ++i)
        P[i] = v3(rd->pos[i]);

    frame(v_sub(P[RD_CHEST], P[RD_PELVIS]), v_sub(P[RD_HIP_R], P[RD_HIP_L]), &f);
    rot_mul_t(&f, &g_f0_pelvis, &d_pelvis);
    frame(v_sub(P[RD_NECK], P[RD_PELVIS]), v_sub(P[RD_SHOULDER_R], P[RD_SHOULDER_L]), &f);
    rot_mul_t(&f, &g_f0_torso, &d_torso);
    /* en marchant, la tête reste de niveau (axe gauche-droite = celui de la marche), au lieu
       de suivre la ligne des épaules qui bascule à chaque pas */
    if (g_head_on) {
        /* marche / course : même repère que la pose (cou -> tête), mais lissé par plugin.c.
           Cou et tête tournent ensemble, comme avant la v0.25.1 (le mélange cou / buste de la
           v0.25.1 faisait tourner le casque) */
        frame(v3(g_head_dir), v3(g_head_right), &f);
        rot_mul_t(&f, &g_f0_head, &d_head);
    } else {
        frame(v_sub(P[RD_HEAD], P[RD_NECK]),
              g_walk_frame_on ? v3(g_walk_right) : v_sub(P[RD_SHOULDER_R], P[RD_SHOULDER_L]), &f);
        rot_mul_t(&f, &g_f0_head, &d_head);
    }
    for (i = B_NECK1; i <= B_HEAD; ++i)
        m4_mul(&d_head, &g_r0[i], &des[i]);

    m4_mul(&d_pelvis, &g_r0[B_ROOT], &des[B_ROOT]);
    for (i = B_SPINE1; i <= B_SPINE4; ++i)
        m4_mul(&d_torso, &g_r0[i], &des[i]);

    /* membres : repère (os, axe de pliage). L'axe de pliage actuel est celui du genou / du
     * coude ; membre presque tendu : on garde celui du départ, tourné avec le bassin / le buste
     * (sinon le membre vrillerait sur lui-même) */
    {
        static const int seg[4][4] = {
            { RD_HIP_L, RD_KNEE_L, RD_FOOT_L, B_HIP_L }, { RD_HIP_R, RD_KNEE_R, RD_FOOT_R, B_HIP_R },
            { RD_SHOULDER_L, RD_ELBOW_L, RD_HAND_L, B_SHOULDER_L },
            { RD_SHOULDER_R, RD_ELBOW_R, RD_HAND_R, B_SHOULDER_R },
        };
        int s;
        V3 arm_h[2] = { { { 1, 0, 0 } }, { { 1, 0, 0 } } }; /* axe du coude de chaque bras */
        for (s = 0; s < 4; ++s) {
            V3 a = v_sub(P[seg[s][1]], P[seg[s][0]]), b = v_sub(P[seg[s][2]], P[seg[s][1]]);
            V3 ref = rot_apply(s < 2 ? &d_pelvis : &d_torso, g_hinge0[s]);
            V3 h = v_cross(a, b);
            float hl = sqrtf(v_dot(h, h)), lim = 0.15f * sqrtf(v_dot(a, a)) * sqrtf(v_dot(b, b));
            M4 F;
            int bone = seg[s][3];
            if (hl < lim) {
                h = ref;
            } else {
                if (v_dot(h, ref) < 0.0f)
                    h = v_add_scaled((V3){ { 0, 0, 0 } }, h, -1.0f);
                h = v_norm(h);
                if (hl < 2.0f * lim) {
                    /* transition douce vers l'axe de référence */
                    float t = (hl - lim) / lim;
                    h = v_norm(v_add_scaled(v_add_scaled((V3){ { 0, 0, 0 } }, ref, 1.0f - t), h, t));
                }
            }
            if (s >= 2)
                arm_h[s - 2] = h;
            frame(a, h, &F);
            rot_mul_t(&F, &g_fl0[bone], &q);
            m4_mul(&q, &g_r0[bone], &des[bone]);
            frame(b, h, &F);
            rot_mul_t(&F, &g_fl0[bone + 1], &q);
            m4_mul(&q, &g_r0[bone + 1], &des[bone + 1]);
            /* pied : à plat dans la direction voulue, même axe de cheville que le genou */
            if (s < 2 && g_feet_on && g_foot_ok) {
                V3 fd = v3(g_feet_dir[s]);
                frame(fd, h, &F);
                rot_mul_t(&F, &g_ffoot0[s], &q);
                m4_mul(&q, &g_r0[s == 0 ? B_ANKLE_L : B_ANKLE_R], &des[s == 0 ? B_ANKLE_L : B_ANKLE_R]);
            }
        }
        /* mains en marchant : dans le prolongement de l'avant-bras, pouce vers l'avant
           (paume tournée vers la cuisse), comme une main détendue */
        if (g_grip_on && g_hand_ok) {
            /* v0.40.67 : mains au guidon : le guidon passe dans la paume (doigts dans le
               prolongement de l'avant-bras, perpendiculaires au guidon), pouce le long du
               guidon vers le milieu de la moto */
            for (s = 0; s < 2; ++s) {
                int el = s == 0 ? RD_ELBOW_L : RD_ELBOW_R, ha = s == 0 ? RD_HAND_L : RD_HAND_R;
                int wb = s == 0 ? B_WRIST_L : B_WRIST_R;
                V3 bar = v_norm(v3(g_grip_bar));
                V3 dir = v_norm(v_sub(P[ha], P[el]));
                V3 up = { { 0, 1, 0 } };
                /* v0.40.69 : doigts vers l'avant et un peu vers le bas (par-dessus la poignée),
                   perpendiculaires au guidon ; avant : dans l'axe de l'avant-bras, ils
                   pointaient vers le ciel quand le coude était plus bas que la main */
                V3 fwd = v_cross(up, bar);
                V3 fing, thumb = v_add_scaled((V3){ { 0, 0, 0 } }, bar, s == 0 ? 1.0f : -1.0f);
                M4 F;
                if (v_dot(fwd, dir) < 0.0f)
                    fwd = v_add_scaled((V3){ { 0, 0, 0 } }, fwd, -1.0f);
                fwd = v_norm(fwd);
                fing = v_add_scaled(v_add_scaled((V3){ { 0, 0, 0 } }, fwd, 0.85f), up, -0.45f);
                fing = v_add_scaled(fing, bar, -v_dot(fing, bar));
                frame(v_norm(fing), thumb, &F);
                rot_mul_t(&F, &g_fhand0[s], &q);
                m4_mul(&q, &g_r0[wb], &des[wb]);
            }
        } else if (g_walk_frame_on && g_hand_ok) {
            for (s = 0; s < 2; ++s) {
                int el = s == 0 ? RD_ELBOW_L : RD_ELBOW_R, ha = s == 0 ? RD_HAND_L : RD_HAND_R;
                int wb = s == 0 ? B_WRIST_L : B_WRIST_R;
                V3 dir = v_sub(P[ha], P[el]);
                V3 thumb = v_add_scaled(v3(g_walk_fwd), v3(g_walk_right), s == 0 ? 0.25f : -0.25f);
                M4 F;
                {
                    /* en courant, l'avant-bras monte presque à l'horizontale, vers l'avant : le
                       repère « pouce vers l'avant » devenait instable et la main se retournait.
                       Pouce perpendiculaire à l'axe du coude : stable dans toutes les positions
                       (vers l'avant bras le long du corps, vers le haut avant-bras levé) */
                    V3 hh = arm_h[s], down = { { 0, -1, 0 } };
                    V3 t = v_norm(v_cross(hh, v_norm(dir)));
                    V3 t_down = v_cross(hh, down);
                    if (v_dot(t_down, thumb) < 0.0f)
                        t = v_add_scaled((V3){ { 0, 0, 0 } }, t, -1.0f);
                    if (sqrtf(v_dot(t, t)) > 0.5f)
                        thumb = v_add_scaled(t, v3(g_walk_right), s == 0 ? 0.15f : -0.15f);
                }
                frame(dir, thumb, &F);
                rot_mul_t(&F, &g_fhand0[s], &q);
                m4_mul(&q, &g_r0[wb], &des[wb]);
            }
        }
        (void)dp;
    }
}

static int is_driven(int role)
{
    int i;
    if (role == B_ANKLE_L || role == B_ANKLE_R)
        return g_feet_on && g_foot_ok;
    if (role == B_WRIST_L || role == B_WRIST_R)
        return (g_walk_frame_on || g_grip_on) && g_hand_ok;
    for (i = 0; i < NUM_DRIVEN; ++i)
        if (k_driven[i] == role)
            return 1;
    return 0;
}

/*
 * Accroche le ragdoll au corps de chute du jeu, qui, lui, touche le vrai relief :
 *  - le bassin est tiré horizontalement vers ce corps (ressort doux) ;
 *  - le sol est placé sous ce corps (body_ground_offset plus bas).
 * Pendant les premiers instants, le corps du jeu peut sauter : on attend un peu.
 */
void drive_anchor(void *skeleton, Ragdoll *rd, float dt, float strength, float body_ground_offset)
{
    M4 model;
    V3 body, d, vel, target_n, n;
    float a, moved, nl;
    int i;
    if (!g_skeleton || skeleton != g_skeleton || !rd->active || dt <= 0.0f)
        return;
    if (!read_model_world(skeleton, &model))
        return;
    body = m4_pos(&model);
    g_model_y = body.v[1];

    moved = sqrtf(v_dot(v_sub(body, g_body_prev), v_sub(body, g_body_prev)));
    vel = v_sub(body, g_body_prev);
    g_body_prev = body;
    if (moved > 2.0f) /* saut du corps du jeu : on ignore cette image */
        return;
    /* vitesse du corps du jeu, lissée sur ~0,3 s */
    a = 1.0f - expf(-dt / 0.3f);
    for (i = 0; i < 3; ++i)
        g_body_vel.v[i] += (vel.v[i] / dt - g_body_vel.v[i]) * a;
    g_body_still = moved / dt < 0.05f ? g_body_still + dt : 0.0f;

    /* Au début de la chute, la matrice du modèle est encore au niveau des roues :
     * on garde le sol de départ. */
    if (rd->time < 0.6f)
        return;

    /* Pente : si le corps du jeu GLISSE en descendant (vitesse régulière, pas une chute qui
     * accélère), le sol penche dans ce sens. On compare deux tranches de 0,3 s. */
    target_n.v[0] = 0.0f;
    target_n.v[1] = 1.0f;
    target_n.v[2] = 0.0f;
    body_history_push(rd->time, body);
    {
        V3 p0, p1, p2;
        if (rd->time > 1.5f && body_history_at(rd->time - 0.6f, &p0) &&
            body_history_at(rd->time - 0.3f, &p1)) {
            V3 d1 = v_sub(p1, p0), d2 = v_sub(body, p1);
            float vy1 = d1.v[1] / 0.3f, vy2 = d2.v[1] / 0.3f;
            float h2 = sqrtf(d2.v[0] * d2.v[0] + d2.v[2] * d2.v[2]);
            V3 dt6 = v_sub(body, p0);
            float h6 = sqrtf(dt6.v[0] * dt6.v[0] + dt6.v[2] * dt6.v[2]);
            p2 = body;
            (void)p2;
            if (h6 > 0.06f && vy1 < -0.01f && vy2 < -0.01f && fabsf(vy1 - vy2) < 0.25f &&
                vy2 > -1.0f && -d2.v[1] < 1.2f * h2 && h6 < 0.6f * 3.0f) {
                float grade = -dt6.v[1] / h6;
                if (grade > 1.2f)
                    grade = 1.2f; /* 50 degrés au plus */
                target_n.v[0] = grade * dt6.v[0] / h6;
                target_n.v[2] = grade * dt6.v[2] / h6;
                target_n = v_norm(target_n);
                g_slope_seen = 1;
            } else if (g_body_still > 0.5f && g_slope_seen) {
                target_n = v3(rd->ground_n); /* posé sur la pente : on la garde */
            }
        }
    }
    a = 1.0f - expf(-dt / 0.5f);
    n = v3(rd->ground_n);
    for (i = 0; i < 3; ++i)
        n.v[i] += (target_n.v[i] - n.v[i]) * a;
    nl = sqrtf(v_dot(n, n));
    if (nl > 1e-6f)
        for (i = 0; i < 3; ++i)
            rd->ground_n[i] = n.v[i] / nl;

    /* le sol passe body_ground_offset sous le corps du jeu */
    rd->ground_y = v_dot(v3(rd->ground_n), body) - body_ground_offset;

    if (strength > 0.0f) {
        V3 pelvis = v3(rd->pos[RD_PELVIS]);
        a = 1.0f - expf(-strength * dt);
        d = v_sub(body, pelvis);
        /* verticalement, on ne ramène que si le pilote est monté bien plus haut que le corps */
        d.v[1] = pelvis.v[1] > body.v[1] + 1.0f ? body.v[1] + 1.0f - pelvis.v[1] : 0.0f;
        {
            /* à l'horizontale, le pilote est libre : on ne le rattrape que s'il s'est éloigné de
               plus de 15 m du corps du jeu (sinon il glissait au sol, tiré vers le corps du jeu,
               comme une marionnette : même à 2,5 m il remontait les pentes tout seul) */
            float h = sqrtf(d.v[0] * d.v[0] + d.v[2] * d.v[2]);
            float keep = 0.0f; /* plus jamais à l'horizontale : le pilote est libre */
            (void)h;
            d.v[0] *= keep;
            d.v[2] *= keep;
        }
        for (i = 0; i < RD_NUM_POINTS; ++i) {
            int k;
            for (k = 0; k < 3; ++k) {
                rd->pos[i][k] += d.v[k] * a;
                rd->prev[i][k] += d.v[k] * a;
            }
        }
    }
}

int drive_apply(void *skeleton, const Ragdoll *rd)
{
    static M4 W[MAX_BONES];
    M4 des[B_COUNT], model;
    V3 pelvis;
    int l, k, written = 0;

    if (!g_skeleton || skeleton != g_skeleton || !rd->active)
        return 0;
    if (!read_model_world(skeleton, &model))
        return 0;

    g_model_y = model.m[7];

    desired_rotations(rd, des);
    pelvis = v3(rd->pos[RD_PELVIS]);

    for (l = 0; l < g_num_lods; ++l) {
        Lod *lod = &g_lods[l];
        for (k = 0; k < lod->count; ++k) {
            uint8_t *node = lod->arr + (size_t)k * NODE_SIZE;
            int p = lod->parent[k], role = lod->role[k];
            M4 local;
            if (!read_node(node, &local, NULL, NULL, 0))
                return written;
            if (p < 0 && role != B_ROOT) {
                /* racine : rattachée à la matrice du modèle (ou lue dans le nœud si la
                 * relation n'a pas été vérifiée au départ) */
                if (!g_root_from_node || !read_node(node, NULL, &W[k], NULL, 0))
                    m4_mul(&model, &local, &W[k]);
                continue;
            }
            if (role >= 0 && is_driven(role)) {
                M4 inv;
                V3 t;
                const M4 *parent_w = p >= 0 ? &W[p] : &model; /* riderRIG_Root sans parent (mods) : le modèle */
                W[k] = des[role];
                if (role == B_ROOT) {
                    t = pelvis;
                    t.v[1] += g_root_up;
                } else {
                    /* on garde la longueur de l'os : position = parent x translation locale */
                    M4 tmp;
                    m4_mul(parent_w, &lod->base_local[k], &tmp);
                    t = m4_pos(&tmp);
                }
                W[k].m[3] = t.v[0];
                W[k].m[7] = t.v[1];
                W[k].m[11] = t.v[2];
                m4_inv_rigid(parent_w, &inv);
                m4_mul(&inv, &W[k], &local);
                local.m[12] = local.m[13] = local.m[14] = 0.0f;
                local.m[15] = 1.0f;
                if (safe_write(node + NODE_LOCAL, local.m, sizeof(local.m))) {
                    ++written;
                    if (l == 0 && k < MAX_BONES) {
                        g_written[k] = local;
                        g_written_ok[k] = 1;
                    }
                }
            } else if (lod->fist_angle[k] > 0.0f) {
                /* doigt : pose du jeu + flexion vers le poing */
                M4 r;
                axis_rot(lod->fist_axis[k], lod->fist_angle[k] * g_fist, &r);
                m4_mul(&lod->base_local[k], &r, &local);
                safe_write(node + NODE_LOCAL, local.m, sizeof(local.m));
                m4_mul(&W[p], &local, &W[k]);
            } else {
                m4_mul(&W[p], &local, &W[k]);
            }
        }
        if (l == 0) {
            memcpy(g_expect_w, W, sizeof(M4) * (size_t)(lod->count < MAX_BONES ? lod->count : MAX_BONES));
            g_expect_n = lod->count;
        }
    }
    g_restore_pending = 1;
    return written;
}

void drive_restore(void)
{
    int l, k;
    if (!g_restore_pending) {
        g_skeleton = NULL;
        return;
    }
    for (l = 0; l < g_num_lods; ++l)
        for (k = 0; k < g_lods[l].count; ++k)
            safe_write(g_lods[l].arr + (size_t)k * NODE_SIZE + NODE_LOCAL, g_lods[l].base_local[k].m,
                       sizeof(g_lods[l].base_local[k].m));
    g_restore_pending = 0;
    g_skeleton = NULL;
    if (g_log)
        g_log("pilotage : matrices locales d'origine remises");
}

void drive_set_feet(const float left[3], const float right[3])
{
    if (!left || !right) {
        g_feet_on = 0;
        return;
    }
    memcpy(g_feet_dir[0], left, sizeof(g_feet_dir[0]));
    memcpy(g_feet_dir[1], right, sizeof(g_feet_dir[1]));
    g_feet_on = 1;
}

void drive_set_head(const float dir[3], const float right[3])
{
    if (!dir || !right) {
        g_head_on = 0;
        return;
    }
    memcpy(g_head_dir, dir, sizeof(g_head_dir));
    memcpy(g_head_right, right, sizeof(g_head_right));
    g_head_on = 1;
}

void drive_get_snap(DriveSnap *s)
{
    memset(s, 0, sizeof(*s));
    s->head_on = g_head_on;
    s->feet_on = g_feet_on;
    s->frame_on = g_walk_frame_on;
    memcpy(s->head_dir, g_head_dir, sizeof(s->head_dir));
    memcpy(s->head_right, g_head_right, sizeof(s->head_right));
    memcpy(s->feet, g_feet_dir, sizeof(s->feet));
    memcpy(s->frame_right, g_walk_right, sizeof(s->frame_right));
    memcpy(s->frame_fwd, g_walk_fwd, sizeof(s->frame_fwd));
    s->fist = g_fist;
    s->root_up = g_root_up;
}

void drive_set_snap(const DriveSnap *s)
{
    g_head_on = 0;
    g_feet_on = 0;
    g_walk_frame_on = 0;
    memcpy(g_head_dir, s->head_dir, sizeof(g_head_dir));
    memcpy(g_head_right, s->head_right, sizeof(g_head_right));
    memcpy(g_feet_dir, s->feet, sizeof(g_feet_dir));
    memcpy(g_walk_right, s->frame_right, sizeof(g_walk_right));
    memcpy(g_walk_fwd, s->frame_fwd, sizeof(g_walk_fwd));
    g_fist = s->fist;
    g_root_up = s->root_up;
    g_head_on = s->head_on;
    g_feet_on = s->feet_on;
    g_walk_frame_on = s->frame_on;
}

void drive_set_grip(const float bar[3])
{
    if (!bar) {
        g_grip_on = 0;
        return;
    }
    memcpy(g_grip_bar, bar, sizeof(g_grip_bar));
    g_grip_on = 1;
}

void drive_set_walk_frame(const float right[3], const float fwd[3])
{
    if (!right || !fwd) {
        g_walk_frame_on = 0;
        return;
    }
    memcpy(g_walk_right, right, sizeof(g_walk_right));
    memcpy(g_walk_fwd, fwd, sizeof(g_walk_fwd));
    g_walk_frame_on = 1;
}

/* Après la mise à jour du jeu : quels os pilotés le jeu a-t-il réécrits ? (journal) */
/* Poignets : le jeu réécrit leur rotation à chaque image. En marchant elle est bonne ; en
   courant (grands mouvements de bras) elle part de travers. On garde donc celle vue en
   marchant et, en courant, on la remet après la mise à jour du jeu (poignet et doigts). */

void drive_wrists_post(void *skeleton, int hold)
{
    int l, k;
    static unsigned char sub[MAX_BONES];
    static M4 W[MAX_BONES];
    if (!g_skeleton || skeleton != g_skeleton)
        return;
    for (l = 0; l < g_num_lods; ++l) {
        Lod *lod = &g_lods[l];
        int any = 0;
        for (k = 0; k < lod->count; ++k) {
            int side = lod->role[k] == B_WRIST_L ? 0 : lod->role[k] == B_WRIST_R ? 1 : -1;
            uint8_t *node = lod->arr + (size_t)k * NODE_SIZE;
            sub[k] = 0;
            if (side < 0)
                continue;
            if (!hold) {
                M4 loc;
                if (read_node(node, &loc, NULL, NULL, 0)) {
                    g_wrist_keep[l][side] = loc;
                    g_wrist_keep_ok[l][side] = 1;
                }
            } else if (g_wrist_keep_ok[l][side]) {
                sub[k] = 1;
                any = 1;
            }
        }
        if (!any)
            continue;
        for (k = 0; k < lod->count; ++k) {
            uint8_t *node = lod->arr + (size_t)k * NODE_SIZE;
            int p = lod->parent[k];
            M4 loc;
            if (!sub[k] && (p < 0 || p >= k || !sub[p]))
                continue;
            if (p < 0 || p >= k)
                continue;
            if (!sub[k])
                sub[k] = 1; /* doigt sous le poignet */
            if (!read_node(lod->arr + (size_t)p * NODE_SIZE, NULL, &W[p], NULL, 0) && !(p < k && sub[p]))
                continue;
            if (lod->role[k] == B_WRIST_L || lod->role[k] == B_WRIST_R) {
                loc = g_wrist_keep[l][lod->role[k] == B_WRIST_L ? 0 : 1];
                safe_write(node + NODE_LOCAL, loc.m, sizeof(loc.m));
            } else if (!read_node(node, &loc, NULL, NULL, 0)) {
                continue;
            }
            m4_mul(&W[p], &loc, &W[k]);
            safe_write(node + NODE_WORLD, W[k].m, 12 * sizeof(float));
        }
    }
}

void drive_log_nodes(DriveLogFn log)
{
    const Lod *lod = &g_lods[0];
    int k;
    if (!log || g_num_lods < 1)
        return;
    for (k = 0; k < lod->count; ++k) {
        M4 l;
        read_node(lod->arr + (size_t)k * NODE_SIZE, &l, NULL, NULL, 0);
        log("  os %d %s : parent %d %s, role %d, local t (%.3f %.3f %.3f)", k, lod->name[k], lod->parent[k],
            lod->parent[k] >= 0 ? lod->name[lod->parent[k]] : "-", lod->role[k], lod->base_local[k].m[3],
            lod->base_local[k].m[7], lod->base_local[k].m[11]);
    }
}

int drive_check_overwritten(void *skeleton, DriveLogFn log)
{
    char list[600];
    size_t n = 0;
    int k, count = 0;
    Lod *lod;
    if (!g_skeleton || skeleton != g_skeleton || g_num_lods < 1 || !log)
        return 0;
    lod = &g_lods[0];
    list[0] = 0;
    for (k = 0; k < lod->count && k < MAX_BONES; ++k) {
        M4 now;
        int i;
        float e = 0.0f;
        if (!g_written_ok[k] || !read_node(lod->arr + (size_t)k * NODE_SIZE, &now, NULL, NULL, 0))
            continue;
        for (i = 0; i < 12; ++i)
            e += fabsf(now.m[i] - g_written[k].m[i]);
        if (e > 1e-3f) {
            ++count;
            if (n + 30 < sizeof(list))
                n += (size_t)snprintf(list + n, sizeof(list) - n, " %s", lod->name[k] + (strncmp(lod->name[k], "riderRIG_", 9) ? 0 : 9));
        }
    }
    log("pilotage : %d os reecrits par le jeu pendant sa mise a jour :%s", count, count ? list : " aucun");
    {
        /* et les positions monde : le jeu les a-t-il déplacées après les locales (cinématique
           inverse des bras vers le guidon) ? */
        int moved = 0;
        n = 0;
        list[0] = 0;
        for (k = 0; k < g_expect_n && k < lod->count && k < MAX_BONES; ++k) {
            M4 wnow;
            float dx, dy, dz, d;
            if (!read_node(lod->arr + (size_t)k * NODE_SIZE, NULL, &wnow, NULL, 0))
                continue;
            dx = wnow.m[3] - g_expect_w[k].m[3];
            dy = wnow.m[7] - g_expect_w[k].m[7];
            dz = wnow.m[11] - g_expect_w[k].m[11];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > 0.02f) {
                ++moved;
                if (n + 40 < sizeof(list))
                    n += (size_t)snprintf(list + n, sizeof(list) - n, " %s(%.2f)",
                                          lod->name[k] + (strncmp(lod->name[k], "riderRIG_", 9) ? 0 : 9), d);
            }
        }
        log("pilotage : %d os places ailleurs par le jeu (m) :%s", moved, moved ? list : " aucun");
    }
    return count;
}

/* Monde attendu (LOD 0) d'un os par son nom, d'après nos dernières matrices (lignes 3x4) */
int drive_expected_world(const char *name, float out[12])
{
    int k;
    if (g_num_lods < 1)
        return 0;
    for (k = 0; k < g_expect_n && k < MAX_BONES; ++k)
        if (!strcmp(g_lods[0].name[k], name)) {
            memcpy(out, g_expect_w[k].m, 12 * sizeof(float));
            return 1;
        }
    return 0;
}

/* Descendu sans chute : le jeu dessine certains os (les bras, mains sur le guidon) d'après une
   troisième matrice du nœud (+0x1C0), qu'il ne recalcule pas depuis nos matrices locales.
   Après sa mise à jour, on y recopie la matrice monde (+0x100). log : une fois, l'écart avant copie. */
int drive_sync_render(void *skeleton, DriveLogFn log)
{
    int l, k, n = 0;
    if (!g_skeleton || skeleton != g_skeleton)
        return 0;
    for (l = 0; l < g_num_lods; ++l) {
        Lod *lod = &g_lods[l];
        for (k = 0; k < lod->count; ++k) {
            uint8_t *node = lod->arr + (size_t)k * NODE_SIZE;
            float w[12], r[12];
            if (!safe_read(node + NODE_WORLD, w, sizeof(w)) || !safe_read(node + 0x1C0, r, sizeof(r)))
                continue;
            if (log && l == 0) {
                float d = fabsf(w[3] - r[3]) + fabsf(w[7] - r[7]) + fabsf(w[11] - r[11]);
                if (d > 0.05f)
                    log("  rendu : %s ecart %.2f m (monde %.2f %.2f %.2f, rendu %.2f %.2f %.2f)", lod->name[k], d, w[3], w[7],
                        w[11], r[3], r[7], r[11]);
            }
            if (!isfinite(w[3]) || !isfinite(w[7]) || !isfinite(w[11]))
                continue;
            if (safe_write(node + 0x1C0, w, sizeof(w)))
                ++n;
        }
    }
    return n;
}

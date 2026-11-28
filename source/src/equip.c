/*
 * equip.c - réglage de la position du casque et des bottes (voir equip.h)
 */
#include "equip.h"
#include "glcam.h"
#include "lang.h"
#include "hook.h"
#include "config.h"
#include "padinject.h"
#include "padstyle.h"
#include "padicon.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define OFF_MODEL_WORLD 0x11C
#define OFF_LODS        0x1F0
#define LOD_COUNT       0x10
#define LOD_ARRAY       0x18
#define NODE_SIZE       0x3F8
#define NODE_WORLD      0x100
#define NODE_SECOND     0x140

enum { K_OTHER = 0, K_HELMET, K_BOOT_L, K_BOOT_R, K_BIKE, K_COCKPIT };
/* K_COCKPIT : casque de la vue embarquée (c_helmet.edf) et caméra de casque : le jeu les colle à
   la caméra ; avec la caméra du plugin, on les voyait par-dessus l'image (v0.40.20) */
enum { G_HELMET = 0, G_BOOTS = 1, G_RIDER = 2 };

typedef struct {
    int group;
    char name[160]; /* v1.1.4 : « pilote @ piste » ou « pilote @ casque » */
    float up, fwd; /* décalages, m */
    float pitch;   /* v1.1 : casque penché vers le haut (+) ou le bas (-), degrés */
} Setting;

#define MAX_SETTINGS 128
static Setting g_set[MAX_SETTINGS];
static int g_nset = 0;
static int g_dirty = 0;
static char g_file[512];
static EquipLogFn g_log = NULL;

typedef struct {
    uintptr_t p;
    int kind;
    int set;          /* index dans g_set, -1 si aucun */
    int mapped;       /* axes « haut » et « avant » trouvés */
    int ax_up, ax_fwd;
    float sg_up, sg_fwd;
    int second;       /* -1 inconnu, 0 non, 1 : la seconde matrice suit aussi le monde */
    uint64_t sig;     /* début du chemin du modèle : adresse réutilisée par un autre modèle ? */
    char eq[64];      /* nom du casque / des bottes */
    int gen;          /* réglage choisi pour ce pilote (g_gen) */
} Model;

/* v0.40.23 : 64 ne suffisaient pas en ligne (86 modèles vus) : le casque du joueur, vu après les
   objets de la piste, n'était jamais suivi ni caché */
#define MAX_MODELS 1024
static Model g_models[MAX_MODELS];
static int g_nmodels = 0;

static float g_fwd[3] = { 0, 0, 1 }, g_pos[3];
static float g_pos2[3];           /* modèle du pilote (corps de chute du jeu) : le casque y reste accroché */
static volatile int g_pos2_ok = 0;
static volatile int g_pos_ok = 0;
static volatile int g_frame_ok = 0;
static volatile int g_cur[3] = { -1, -1, -1 }; /* réglage du casque / des bottes affiché dans le menu */
static int g_logged_kind[4];

/* ---------- fichier ---------- */

static int find_setting(int group, const char *name, int create)
{
    int i;
    for (i = 0; i < g_nset; ++i)
        if (g_set[i].group == group && !strcmp(g_set[i].name, name))
            return i;
    if (!create || g_nset >= MAX_SETTINGS)
        return -1;
    g_set[g_nset].group = group;
    snprintf(g_set[g_nset].name, sizeof(g_set[g_nset].name), "%s", name);
    g_set[g_nset].up = g_set[g_nset].fwd = g_set[g_nset].pitch = 0.0f;
    return g_nset++;
}

/* v1.1.4 : réglages enregistrés avec le pilote : hauteur du pilote par pilote ET par piste,
   casque et bottes par pilote. Un nouveau couple reprend le réglage précédent (sans pilote ou
   sans piste) pour ne rien perdre. */
static char g_rider[64] = "", g_track[96] = "";
static int g_gen = 1;

#define RIDER_UP_DEFAULT -0.10f
static int find_or_copy(int group, const char *key, const char *fallback)
{
    int i = find_setting(group, key, 0), f;
    if (i >= 0)
        return i;
    i = find_setting(group, key, 1);
    f = (fallback && group != G_RIDER) ? find_setting(group, fallback, 0) : -1; /* v1.7.18 : pilote sur une nouvelle piste : -0,10 m, pas la valeur d'avant du pilote */
    if (i >= 0 && f >= 0) {
        g_set[i].up = g_set[f].up;
        g_set[i].fwd = g_set[f].fwd;
        g_set[i].pitch = g_set[f].pitch;
    } else if (i >= 0 && group == G_RIDER) {
        g_set[i].up = RIDER_UP_DEFAULT; /* v1.7.18 : hauteur du pilote de base (demandé) */
    }
    return i;
}

static void update_rider_setting(void)
{
    char key[200];
    if (!g_rider[0])
        return;
    if (g_track[0]) {
        snprintf(key, sizeof(key), "%s @ %s", g_rider, g_track);
        g_cur[G_RIDER] = find_or_copy(G_RIDER, key, g_rider);
    } else {
        g_cur[G_RIDER] = find_or_copy(G_RIDER, g_rider, NULL);
    }
}

void equip_set_track(const char *track)
{
    snprintf(g_track, sizeof(g_track), "%s", track ? track : "");
    {
        size_t i;
        for (i = 0; g_track[i]; ++i)
            if (g_track[i] == '|')
                g_track[i] = '/';
    }
    update_rider_setting();
    ++g_gen;
}

static void save_file(void)
{
    FILE *f;
    int i;
    if (!g_file[0] || !g_dirty)
        return;
    f = fopen(g_file, "w");
    if (!f)
        return;
    fprintf(f, "; MXRP_Ragdoll : position du casque et des bottes, par modele (menu : touche P)\n");
    fprintf(f, "; type|nom du dossier|hauteur (m)|avant (m)|angle (degres, casque)\n");
    for (i = 0; i < g_nset; ++i)
        if (g_set[i].group == G_RIDER || g_set[i].up != 0.0f || g_set[i].fwd != 0.0f || g_set[i].pitch != 0.0f)
            fprintf(f, "%s|%s|%.4f|%.4f|%.2f\n", g_set[i].group == G_HELMET ? "casque" : g_set[i].group == G_RIDER ? "pilote" : "bottes", g_set[i].name,
                    g_set[i].up, g_set[i].fwd, g_set[i].pitch);
    fclose(f);
    g_dirty = 0;
    if (g_log)
        g_log("equipement : reglages enregistres dans %s", g_file);
}

void equip_init(const char *save_path, EquipLogFn log)
{
    FILE *f;
    char line[400];
    g_log = log;
    g_nset = 0;
    if (!save_path)
        return;
    {
        size_t l = strlen(save_path);
        snprintf(g_file, sizeof(g_file), "%s%sMXRP_Ragdoll_equipement.ini", save_path,
                 (l && save_path[l - 1] != '\\' && save_path[l - 1] != '/') ? "\\" : "");
    }
    f = fopen(g_file, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char type[16], name[160];
        float up, fwd, pitch = 0.0f;
        if (line[0] == ';')
            continue;
        if (sscanf(line, "%15[^|]|%159[^|]|%f|%f|%f", type, name, &up, &fwd, &pitch) >= 4) {
            int i = find_setting(!strcmp(type, "casque") ? G_HELMET : !strcmp(type, "pilote") ? G_RIDER : G_BOOTS, name, 1);
            if (i >= 0) {
                g_set[i].up = up;
                g_set[i].fwd = fwd;
                g_set[i].pitch = pitch;
            }
        }
    }
    fclose(f);
    if (g_log)
        g_log("equipement : %d reglage(s) charge(s)", g_nset);
}

void equip_reset(void)
{
    save_file();
    g_nmodels = 0;
    g_cur[0] = g_cur[1] = -1; /* (le pilote reste : son modèle est donné par le plugin) */
    g_frame_ok = 0;
}

void equip_set_player_pos(const float pos[3])
{
    memcpy(g_pos, pos, sizeof(g_pos));
    g_pos_ok = 1;
}

void equip_set_player_model_pos(const float pos[3])
{
    if (pos)
        memcpy(g_pos2, pos, sizeof(g_pos2));
    g_pos2_ok = pos != NULL;
}

/* équipement du joueur : à moins de 3 m de lui, ou de son modèle (v0.40.20 : en marchant à plus
   de 3 m de l'endroit de la chute, le casque n'était plus reconnu et restait visible en 1re
   personne) */
static int near_player(const float w[12])
{
    float dx = w[3] - g_pos[0], dy = w[7] - g_pos[1], dz = w[11] - g_pos[2];
    if (g_pos_ok && dx * dx + dy * dy + dz * dz < 9.0f)
        return 1;
    dx = w[3] - g_pos2[0];
    dy = w[7] - g_pos2[1];
    dz = w[11] - g_pos2[2];
    return g_pos2_ok && dx * dx + dy * dy + dz * dz < 1.0f;
}

void equip_set_rider_frame(const float up[3], const float fwd[3])
{
    float l = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    (void)up;
    if (l < 1e-3f)
        return;
    g_fwd[0] = fwd[0] / l;
    g_fwd[1] = 0.0f;
    g_fwd[2] = fwd[2] / l;
    g_frame_ok = 1;
}

/* ---------- modèles ---------- */

/* nom du dossier après "\helmets\" ou "\boots\" dans le chemin du fichier */
static int classify(const char *path, char *name, size_t nsz)
{
    char low[300];
    const char *hit;
    int kind = K_OTHER, i;
    size_t n = strlen(path);
    if (n >= sizeof(low))
        n = sizeof(low) - 1;
    for (i = 0; i < (int)n; ++i) {
        char c = path[i];
        low[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : (c == '/' ? '\\' : c));
    }
    low[n] = 0;
    if ((hit = strstr(low, "\\helmets\\")) && strstr(low, "helmet.hrc"))
        kind = K_HELMET;
    else if ((hit = strstr(low, "\\boots\\")) && strstr(low, "left_boot.hrc"))
        kind = K_BOOT_L;
    else if ((hit = strstr(low, "\\boots\\")) && strstr(low, "right_boot.hrc"))
        kind = K_BOOT_R;
    if (kind == K_OTHER) {
        if ((strstr(low, "\\helmets\\") && strstr(low, "c_helmet")) || strstr(low, "\\helmetcams\\"))
            return K_COCKPIT;
        return strstr(low, "\\bikes\\") ? K_BIKE : K_OTHER;
    }
    {
        const char *s = path + (hit - low) + (kind == K_HELMET ? 9 : 7);
        size_t k = 0;
        while (s[k] && s[k] != '\\' && s[k] != '/' && k + 1 < nsz) {
            name[k] = s[k];
            ++k;
        }
        name[k] = 0;
    }
    return name[0] ? kind : K_OTHER;
}

static Model *lookup(void *model)
{
    int i;
    char path[260], name[64] = "";
    Model *m;
    uint64_t sig = 0;
    if (!safe_read(model, &sig, sizeof(sig)))
        return NULL;
    for (i = 0; i < g_nmodels; ++i)
        if (g_models[i].p == (uintptr_t)model) {
            if (g_models[i].sig == sig)
                return &g_models[i];
            break; /* adresse reprise par un autre modèle (joueurs qui arrivent ou partent) */
        }
    if (i == g_nmodels && g_nmodels >= MAX_MODELS)
        g_nmodels = 0; /* plein : on repart de zéro (les modèles encore là se réinscrivent) */
    memset(path, 0, sizeof(path));
    if (!safe_read(model, path, sizeof(path) - 1))
        return NULL;
    path[sizeof(path) - 1] = 0;
    m = i < g_nmodels ? &g_models[i] : &g_models[g_nmodels++];
    memset(m, 0, sizeof(*m));
    m->p = (uintptr_t)model;
    m->sig = sig;
    m->set = -1;
    m->second = -1;
    m->kind = classify(path, name, sizeof(name));
    if (m->kind != K_OTHER && m->kind != K_BIKE && m->kind != K_COCKPIT) {
        m->set = find_setting(m->kind == K_HELMET ? G_HELMET : G_BOOTS, name, 1);
        snprintf(m->eq, sizeof(m->eq), "%s", name);
        if (g_log && !g_logged_kind[m->kind]) {
            g_logged_kind[m->kind] = 1;
            g_log("equipement : %s \"%s\" (%s)", m->kind == K_HELMET ? "casque" : "botte", name, path);
        }
    }
    return m;
}

static int looks_ptr(uint64_t v)
{
    return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
}

/* moto cachée pendant la bascule : p' = to + M (p - from) sur ses nœuds */
/* pilote : hauteur réglée par modèle (mods de pilote parfois affichés trop bas) */
void equip_set_rider_model(const char *name)
{
    char nm[64];
    const char *p = name, *q;
    size_t k = 0;
    /* nom du dossier après « riders\ » */
    for (q = name; *q; ++q)
        if ((q[0] == 'r' || q[0] == 'R') && !strncmp(q + 1, "iders", 5) && (q[6] == '\\' || q[6] == '/'))
            p = q + 7;
    while (p[k] && p[k] != '\\' && p[k] != '/' && k + 1 < sizeof(nm)) {
        nm[k] = p[k];
        ++k;
    }
    nm[k] = 0;
    if (nm[0] && strcmp(nm, g_rider)) {
        snprintf(g_rider, sizeof(g_rider), "%s", nm);
        ++g_gen;
    }
    update_rider_setting();
}

float equip_rider_up(void)
{
    int i = g_cur[G_RIDER];
    return i >= 0 && i < g_nset ? g_set[i].up : RIDER_UP_DEFAULT;
}

static volatile int g_hide_helmet = 0;
static float g_head[3];
static volatile int g_head_ok = 0;
void equip_hide_helmet(int on) { g_hide_helmet = on; }
/* v1.1 : réglages du menu P (casque, bottes) seulement à pied : sur la moto et pendant la chute,
   l'équipement reste où le jeu le met */
static volatile int g_on_foot = 0;
void equip_set_on_foot(int on)
{
    g_on_foot = on;
}

void equip_set_player_head(const float head[3])
{
    if (head)
        memcpy(g_head, head, sizeof(g_head));
    g_head_ok = head != NULL;
}

/* où le casque est vraiment dessiné : 1er nœud du 1er niveau de détail */
static int first_node_pos(void *model, float out[3])
{
    uint64_t lp = 0, arr = 0;
    int32_t count = 0;
    float a[12];
    if (!safe_read((const uint8_t *)model + OFF_LODS, &lp, 8) || lp <= 0x10000 || lp >= 0x7FFFFFFFFFFFull)
        return 0;
    if (!safe_read((const uint8_t *)(uintptr_t)lp + LOD_COUNT, &count, 4) ||
        !safe_read((const uint8_t *)(uintptr_t)lp + LOD_ARRAY, &arr, 8) || count < 1 || arr <= 0x10000 ||
        arr >= 0x7FFFFFFFFFFFull)
        return 0;
    if (!safe_read((const uint8_t *)(uintptr_t)arr + NODE_WORLD, a, sizeof(a)))
        return 0;
    out[0] = a[3];
    out[1] = a[7];
    out[2] = a[11];
    return 1;
}
static int first_node_world(void *model, float out[12])
{
    uint64_t lp = 0, arr = 0;
    int32_t count = 0;
    if (!safe_read((const uint8_t *)model + OFF_LODS, &lp, 8) || lp <= 0x10000 || lp >= 0x7FFFFFFFFFFFull)
        return 0;
    if (!safe_read((const uint8_t *)(uintptr_t)lp + LOD_COUNT, &count, 4) ||
        !safe_read((const uint8_t *)(uintptr_t)lp + LOD_ARRAY, &arr, 8) || count < 1 || arr <= 0x10000 ||
        arr >= 0x7FFFFFFFFFFFull)
        return 0;
    return safe_read((const uint8_t *)(uintptr_t)arr + NODE_WORLD, out, sizeof(float) * 12);
}
static volatile int g_hide_cockpit = 0;
void equip_hide_cockpit(int on) { g_hide_cockpit = on; }
static volatile int g_fix_on = 0;
static float g_fix_M[9], g_fix_from[3], g_fix_to[3];

void equip_set_bike_fix(int on, const float M[9], const float from[3], const float to[3])
{
    if (on) {
        memcpy(g_fix_M, M, sizeof(g_fix_M));
        memcpy(g_fix_from, from, sizeof(g_fix_from));
        memcpy(g_fix_to, to, sizeof(g_fix_to));
    }
    g_fix_on = on;
}

static void bike_fix(void *model)
{
    float w[12];
    int l, k, i, j;
    if (!safe_read((const uint8_t *)model + OFF_MODEL_WORLD, w, sizeof(w)))
        return;
    {
        float dx = w[3] - g_fix_from[0], dy = w[7] - g_fix_from[1], dz = w[11] - g_fix_from[2];
        if (dx * dx + dy * dy + dz * dz > 9.0f)
            return; /* pas la moto du joueur */
    }
    for (l = 0; l < 4; ++l) {
        uint64_t lp = 0, arr = 0;
        int32_t count = 0;
        if (!safe_read((const uint8_t *)model + OFF_LODS + 8 * l, &lp, 8) || lp <= 0x10000 || lp >= 0x7FFFFFFFFFFFull)
            continue;
        if (!safe_read((const uint8_t *)(uintptr_t)lp + LOD_COUNT, &count, 4) ||
            !safe_read((const uint8_t *)(uintptr_t)lp + LOD_ARRAY, &arr, 8) || count < 1 || count > 256 ||
            arr <= 0x10000 || arr >= 0x7FFFFFFFFFFFull)
            continue;
        for (k = 0; k < count; ++k) {
            uint8_t *node = (uint8_t *)(uintptr_t)arr + (size_t)k * NODE_SIZE;
            float a[12], b[12], d[3];
            if (!safe_read(node + NODE_WORLD, a, sizeof(a)))
                break;
            for (i = 0; i < 3; ++i)
                d[i] = a[i * 4 + 3] - g_fix_from[i];
            for (i = 0; i < 3; ++i) {
                for (j = 0; j < 3; ++j)
                    b[i * 4 + j] = g_fix_M[i * 3 + 0] * a[0 * 4 + j] + g_fix_M[i * 3 + 1] * a[1 * 4 + j] + g_fix_M[i * 3 + 2] * a[2 * 4 + j];
                b[i * 4 + 3] = g_fix_to[i] + g_fix_M[i * 3 + 0] * d[0] + g_fix_M[i * 3 + 1] * d[1] + g_fix_M[i * 3 + 2] * d[2];
            }
            safe_write(node + NODE_WORLD, b, sizeof(b));
        }
    }
}

/* v1.1 : casque penché vers le haut / le bas autour de son origine (la tête), axe gauche-droite
   du pilote. Tous les noeuds de tous les LOD tournent ensemble (et la seconde matrice si elle
   est une matrice monde). */
/* rotation R (3x3, monde) de tout le casque autour de son origine (1er nœud) */
static void helmet_apply(void *model, const float R[9]);
static void helmet_pitch(void *model, float deg)
{
    float L[3], R[9], a = deg * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
    /* axe latéral = haut du monde x avant du pilote */
    L[0] = g_fwd[2];
    L[1] = 0.0f;
    L[2] = -g_fwd[0];
    {
        float n = sqrtf(L[0] * L[0] + L[2] * L[2]);
        if (n < 1e-4f)
            return;
        L[0] /= n;
        L[2] /= n;
    }
    /* rotation d'angle a autour de L (Rodrigues) ; signe : + = visière vers le haut */
    {
        float x = L[0], y = L[1], z = L[2], t = 1.0f - c;
        R[0] = c + x * x * t;     R[1] = x * y * t - z * s; R[2] = x * z * t + y * s;
        R[3] = y * x * t + z * s; R[4] = c + y * y * t;     R[5] = y * z * t - x * s;
        R[6] = z * x * t - y * s; R[7] = z * y * t + x * s; R[8] = c + z * z * t;
    }
    helmet_apply(model, R);
}

/* v1.8.80 : le jeu tourne le casque avec la tête quand on regarde à gauche / à droite en roulant,
   et après F ou une chute il le laisse tourné (notre tête, elle, regarde devant). En roulant, on
   apprend la place du casque par rapport à l'os de la tête du pilote (ils tournent ensemble) ;
   ensuite (F, chute, marche) le casque suit l'os de la tête que nous dirigeons. */
static volatile int g_hd_n = 0;
static float g_hd_now[12];
static volatile int g_rh_ok = 0;
static float g_rh[12];
static int g_rel_n = 0;
static float g_rel[12]; /* casque dans le repère de la tête (3x4 du jeu, translation inutilisée) */

void equip_set_head_frames(const float spawn[12], const float now[12], int n)
{
    (void)spawn;
    if (n > 0 && now)
        memcpy(g_hd_now, now, sizeof(g_hd_now));
    g_hd_n = n;
}

void equip_set_ride_head(const float w[12])
{
    if (w) {
        memcpy(g_rh, w, sizeof(g_rh));
        g_rh_ok = 1;
    } else
        g_rh_ok = 0;
}

/* rotation pure (colonnes = axes, orthonormées) d'une matrice 3x4 du jeu */
static int rot3(const float w[12], float r[9])
{
    float x[3] = { w[0], w[4], w[8] }, y[3] = { w[1], w[5], w[9] }, z[3], n, d;
    int i;
    n = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (n < 1e-5f)
        return 0;
    for (i = 0; i < 3; ++i)
        x[i] /= n;
    d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    for (i = 0; i < 3; ++i)
        y[i] -= d * x[i];
    n = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (n < 1e-5f)
        return 0;
    for (i = 0; i < 3; ++i)
        y[i] /= n;
    z[0] = x[1] * y[2] - x[2] * y[1];
    z[1] = x[2] * y[0] - x[0] * y[2];
    z[2] = x[0] * y[1] - x[1] * y[0];
    /* même sens que l'axe z d'origine (repère direct ou non) */
    if (z[0] * w[2] + z[1] * w[6] + z[2] * w[10] < 0.0f)
        for (i = 0; i < 3; ++i)
            z[i] = -z[i];
    for (i = 0; i < 3; ++i) {
        r[i * 3 + 0] = x[i];
        r[i * 3 + 1] = y[i];
        r[i * 3 + 2] = z[i];
    }
    return 1;
}
static void mul3(const float a[9], const float b[9], float o[9])
{
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            o[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j];
}
static void tr3(const float a[9], float o[9])
{
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            o[i * 3 + j] = a[j * 3 + i];
}

static void diag_axes(const char *what, const float w[12])
{
    if (g_log)
        g_log("  diag %s : x(%.2f %.2f %.2f) y(%.2f %.2f %.2f) z(%.2f %.2f %.2f) pos(%.2f %.2f %.2f)", what, w[0], w[4], w[8],
              w[1], w[5], w[9], w[2], w[6], w[10], w[3], w[7], w[11]);
}

static void helmet_rel_learn(const float w0[12])
{
    float H[9], W[9], t[9], r[9], k;
    int i, j;
    if (!g_rh_ok)
        return;
    {
        float dx = w0[3] - g_rh[3], dy = w0[7] - g_rh[7], dz = w0[11] - g_rh[11];
        if (dx * dx + dy * dy + dz * dz > 0.4f * 0.4f)
            return; /* pas ce casque sur cette tête */
    }
    if (!rot3(g_rh, H) || !rot3(w0, W))
        return;
    tr3(H, t);
    mul3(t, W, r);
    k = g_rel_n < 30 ? 1.0f / (float)(g_rel_n + 1) : 0.02f;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            g_rel[i * 4 + j] += (r[i * 3 + j] - g_rel[i * 4 + j]) * k;
    if (g_rel_n < 1000000)
        ++g_rel_n;
    if (g_rel_n == 30 && g_log)
        g_log("equipement : place du casque sur la tete apprise en roulant");
    {
        static unsigned last = 0;
        unsigned now = (unsigned)GetTickCount();
        if (g_log && now - last > 30000) {
            last = now;
            g_log("diag casque en roulant (n %d, avant %.2f %.2f %.2f) :", g_rel_n, g_fwd[0], g_fwd[1], g_fwd[2]);
            diag_axes("tete", g_rh);
            diag_axes("casque", w0);
            diag_axes("rel", g_rel);
        }
    }
}

static void helmet_follow_head(void *model, const float w0[12])
{
    float Hn[9], Wc[9], R[9], t[9], T[9], C[9], ang;
    if (g_hd_n <= 0 || g_rel_n < 30 || !rot3(w0, Wc) || !rot3(g_hd_now, Hn) || !rot3(g_rel, R)) {
        static unsigned last = 0;
        unsigned now = (unsigned)GetTickCount();
        if (g_log && now - last > 15000) {
            last = now;
            g_log("diag casque : pas suivi (depart %d, appris %d)", g_hd_n, g_rel_n);
        }
        return;
    }
    mul3(Hn, R, T);
    tr3(Wc, t);
    mul3(T, t, C);
    ang = acosf(fmaxf(-1.0f, fminf(1.0f, (C[0] + C[4] + C[8] - 1.0f) * 0.5f)));
    {
        static unsigned last = 0;
        unsigned now = (unsigned)GetTickCount();
        if (g_log && now - last > 10000) {
            float tw[12];
            int i, j;
            last = now;
            for (i = 0; i < 3; ++i) {
                for (j = 0; j < 3; ++j)
                    tw[i * 4 + j] = T[i * 3 + j];
                tw[i * 4 + 3] = 0.0f;
            }
            g_log("diag casque a pied (pied %d, avant %.2f %.2f %.2f, correction %.0f deg) :", g_on_foot, g_fwd[0], g_fwd[1],
                  g_fwd[2], ang * 57.2958f);
            diag_axes("tete", g_hd_now);
            diag_axes("casque", w0);
            diag_axes("vise", tw);
        }
    }
    if (ang < 0.01f)
        return;
    {
        static unsigned last_log = 0;
        unsigned now = (unsigned)GetTickCount();
        if (g_log && ang > 0.3f && now - last_log > 5000) {
            last_log = now;
            g_log("equipement : casque tourne de %.0f degres par rapport a la tete, remis sur la tete", ang * 57.2958f);
        }
    }
    helmet_apply(model, C);
}

static void helmet_apply(void *model, const float R[9])
{
    Model *m = lookup(model);
    float piv[3];
    int l, k, have_piv = 0;
    for (l = 0; l < 4; ++l) {
        uint64_t lp = 0, arr = 0;
        int32_t count = 0;
        if (!safe_read((const uint8_t *)model + OFF_LODS + 8 * l, &lp, 8) || !looks_ptr(lp))
            continue;
        if (!safe_read((const uint8_t *)(uintptr_t)lp + LOD_COUNT, &count, 4) ||
            !safe_read((const uint8_t *)(uintptr_t)lp + LOD_ARRAY, &arr, 8) || count < 1 || count > 128 ||
            !looks_ptr(arr))
            continue;
        for (k = 0; k < count; ++k) {
            uint8_t *node = (uint8_t *)(uintptr_t)arr + (size_t)k * NODE_SIZE;
            float w[12], b[12];
            int pass, i, j;
            if (!safe_read(node + NODE_WORLD, w, sizeof(w)))
                break;
            if (!have_piv) {
                piv[0] = w[3];
                piv[1] = w[7];
                piv[2] = w[11];
                have_piv = 1;
            }
            for (pass = 0; pass < 2; ++pass) {
                float *src = pass ? b : w;
                float out[12];
                if (pass && (!m || m->second != 1 || !safe_read(node + NODE_SECOND, b, sizeof(b))))
                    break;
                for (i = 0; i < 3; ++i) {
                    for (j = 0; j < 3; ++j)
                        out[i * 4 + j] = R[i * 3 + 0] * src[0 * 4 + j] + R[i * 3 + 1] * src[1 * 4 + j] + R[i * 3 + 2] * src[2 * 4 + j];
                    out[i * 4 + 3] = piv[i] + R[i * 3 + 0] * (src[3] - piv[0]) + R[i * 3 + 1] * (src[7] - piv[1]) +
                                     R[i * 3 + 2] * (src[11] - piv[2]);
                }
                safe_write(node + (pass ? NODE_SECOND : NODE_WORLD), out, sizeof(out));
            }
        }
    }
}

void equip_after_update(void *model, int allowed)
{
    Model *m = lookup(model);
    float w[12], o[3], up, fwd, extra[3] = { 0.0f, 0.0f, 0.0f };
    int l, k;
    Setting *s;
    if (m && m->kind == K_BIKE) {
        if (g_fix_on && allowed)
            bike_fix(model);
        return;
    }
    if (m && m->kind == K_COCKPIT) {
        /* casque de la vue embarquée : caché tant que la caméra du plugin est active à pied */
        if (!g_hide_cockpit || !allowed)
            return;
        up = -1000.0f;
        fwd = 0.0f;
        o[0] = 0.0f;
        o[1] = up;
        o[2] = 0.0f;
        goto shift;
    }
    if (!m || m->kind == K_OTHER || m->set < 0)
        return;
    if (!safe_read((const uint8_t *)model + OFF_MODEL_WORLD, w, sizeof(w)))
        return;
    {
        /* le joueur : l'équipement à moins de 3 m de lui (les autres pilotes partagent le réglage) */
        int mine = near_player(w);
        if (mine)
            g_cur[m->kind == K_HELMET ? G_HELMET : G_BOOTS] = m->set;
    }
    if (g_rider[0] && m->gen != g_gen && m->eq[0]) {
        /* réglage du casque / des bottes pour ce pilote */
        char key[200];
        snprintf(key, sizeof(key), "%s @ %s", g_rider, m->eq);
        {
            int si = find_or_copy(m->kind == K_HELMET ? G_HELMET : G_BOOTS, key, m->eq);
            if (si >= 0)
                m->set = si;
        }
        m->gen = g_gen;
        if (near_player(w))
            g_cur[m->kind == K_HELMET ? G_HELMET : G_BOOTS] = m->set;
    }
    s = &g_set[m->set];
    up = g_on_foot ? s->up : 0.0f;
    fwd = g_on_foot ? s->fwd : 0.0f;
    if (m->kind == K_HELMET && g_hide_helmet && allowed) {
        /* vue 1re personne : le casque du joueur est envoyé loin sous le sol (invisible) */
        /* v0.40.22 : le casque est à la tête du pilote (là où il est dessiné). Avec un pilote mod,
           la position du modèle restait à l'endroit de la chute : le casque n'était plus reconnu */
        float np[3];
        int on_head = 0, by_head = 0;
        if (g_head_ok && first_node_pos(model, np)) {
            by_head = 1;
            float dx = np[0] - g_head[0], dy = np[1] - g_head[1], dz = np[2] - g_head[2];
            on_head = dx * dx + dy * dy + dz * dz < 0.6f * 0.6f;
            if (g_log) {
                static int logged = 0;
                if (logged < 4) {
                    ++logged;
                    g_log("equipement : casque %p a %.2f m de la tete (%s)", model, sqrtf(dx * dx + dy * dy + dz * dz),
                          on_head ? "cache" : "pas le mien");
                }
            }
        }
        /* v0.40.38 : seulement le casque sur MA tête. « près du joueur » cachait aussi le casque
           d'un autre pilote qui marchait à côté (en ligne) */
        if (by_head ? on_head : near_player(w)) {
            up = -1000.0f;
            fwd = 0.0f;
        }
    }
    {
        /* v0.40.53 : à pied (après une chute), le casque doit rester sur la tête du pilote. S'il
           s'en écarte (le jeu le place parfois d'après son propre corps de chute), on le remet
           à sa place habituelle par rapport à la tête (mesurée quand il était bien posé) */
        float np[3];
        extra[0] = extra[1] = extra[2] = 0.0f;
        if (m->kind == K_HELMET && g_head_ok && !g_hide_helmet && allowed && first_node_pos(model, np)) {
            static float base[3];
            static int base_ok = 0, n_fix = 0;
            static unsigned last_log = 0;
            float d[3] = { np[0] - g_head[0], np[1] - g_head[1], np[2] - g_head[2] };
            float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dist < 0.6f) { /* mon casque */
                if (dist < 0.05f) {
                    memcpy(base, d, sizeof(base));
                    base_ok = 1;
                } else {
                    /* v0.40.56 : plus de correction. L'écart casque / point de la tête change
                       normalement avec l'inclinaison de la tête (le casque tourne avec elle) :
                       la correction en coordonnées du monde le décollait (pilote qui tient la
                       moto, tête baissée : casque 15 cm au-dessus). Diagnostic seulement. */
                    (void)base_ok;
                    ++n_fix;
                }
                if (g_log) {
                    unsigned now = (unsigned)GetTickCount();
                    if (now - last_log > 15000) {
                        last_log = now;
                        g_log("equipement : casque a %.3f m de la tete (%+.2f %+.2f %+.2f)", dist, d[0], d[1], d[2]);
                    }
                }
            }
        }
    }
    if (m->kind == K_HELMET && allowed && !g_head_ok && !g_on_foot && g_rh_ok) {
        float w0[12];
        if (first_node_world(model, w0))
            helmet_rel_learn(w0); /* en roulant */
    }
    if (m->kind == K_HELMET && allowed && g_frame_ok && g_head_ok && up > -100.0f && !g_hide_helmet) {
        float w0[12];
        if (first_node_world(model, w0)) {
            float d0 = w0[3] - g_head[0], d1 = w0[7] - g_head[1], d2 = w0[11] - g_head[2];
            if (d0 * d0 + d1 * d1 + d2 * d2 < 0.6f * 0.6f) /* mon casque */
                helmet_follow_head(model, w0);
        }
    }
    if (m->kind == K_HELMET && g_on_foot && s->pitch != 0.0f && up > -100.0f && allowed && g_frame_ok)
        helmet_pitch(model, s->pitch);
    if (!allowed || !g_frame_ok || (up == 0.0f && fwd == 0.0f && extra[0] == 0.0f && extra[1] == 0.0f && extra[2] == 0.0f))
        return;
    /* hauteur = verticale du monde, avant = direction où regarde le pilote (à l'horizontale) :
       les axes propres de chaque modèle de casque varient d'un mod à l'autre */
    o[0] = g_fwd[0] * fwd + extra[0];
    o[1] = up + extra[1];
    o[2] = g_fwd[2] * fwd + extra[2];
shift:
    for (l = 0; l < 4; ++l) {
        uint64_t lp = 0, arr = 0;
        int32_t count = 0;
        if (!safe_read((const uint8_t *)model + OFF_LODS + 8 * l, &lp, 8) || !looks_ptr(lp))
            continue;
        if (!safe_read((const uint8_t *)(uintptr_t)lp + LOD_COUNT, &count, 4) ||
            !safe_read((const uint8_t *)(uintptr_t)lp + LOD_ARRAY, &arr, 8) || count < 1 || count > 128 ||
            !looks_ptr(arr))
            continue;
        for (k = 0; k < count; ++k) {
            uint8_t *node = (uint8_t *)(uintptr_t)arr + (size_t)k * NODE_SIZE;
            float a[12], b[12];
            if (!safe_read(node + NODE_WORLD, a, sizeof(a)))
                break;
            if (m->second < 0 && safe_read(node + NODE_SECOND, b, sizeof(b))) {
                /* la seconde matrice est-elle aussi une matrice monde ? (même translation) */
                m->second = fabsf(a[3] - b[3]) + fabsf(a[7] - b[7]) + fabsf(a[11] - b[11]) < 0.01f;
                if (g_log)
                    g_log("equipement : seconde matrice des noeuds %s", m->second ? "monde aussi (decalee)" : "differente (non touchee)");
            }
            a[3] += o[0];
            a[7] += o[1];
            a[11] += o[2];
            safe_write(node + NODE_WORLD, a, sizeof(a));
            if (m->second == 1 && safe_read(node + NODE_SECOND, b, sizeof(b))) {
                b[3] += o[0];
                b[7] += o[1];
                b[11] += o[2];
                safe_write(node + NODE_SECOND, b, sizeof(b));
            }
        }
    }
}

/* ---------- menu ---------- */

static volatile int g_open = 0;
static volatile int g_tuto_now = 0; /* v1.8.111 : tutoriel demandé dans le menu (ouvert tout de suite) */
int equip_tutorial_requested(void)
{
    int r = g_tuto_now;
    g_tuto_now = 0;
    return r;
}

static int key(int vk)
{
#ifdef _WIN32
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    (void)vk;
    return 0;
#endif
}

int equip_menu_open(void)
{
    return g_open;
}

/* ---------- page « Touches » : réglage des touches en jeu ---------- */

static PluginConfig *g_cfg_p = NULL;
static char g_keys_file[512];
static int g_ksel = 0;
static int g_capture = 0;   /* 1 : on attend une touche ou un bouton pour l'action choisie */

typedef struct {
    const char *label;
    size_t key_off;  /* int[2] dans PluginConfig */
    long pad_off;    /* int dans PluginConfig, -1 si pas de bouton de manette */
} Action;

static const Action k_actions[] = {
    { "Avancer", offsetof(PluginConfig, key_forward), -1 },
    { "Reculer", offsetof(PluginConfig, key_back), -1 },
    { "Gauche", offsetof(PluginConfig, key_left), -1 },
    { "Droite", offsetof(PluginConfig, key_right), -1 },
    { "Courir", offsetof(PluginConfig, key_run), (long)offsetof(PluginConfig, pad_run) },
    { "Se relever", offsetof(PluginConfig, key_getup), (long)offsetof(PluginConfig, pad_getup) },
    { "Relever / monter", offsetof(PluginConfig, key_bike), (long)offsetof(PluginConfig, pad_bike) },
    { "Descendre", offsetof(PluginConfig, key_dismount), (long)offsetof(PluginConfig, pad_dismount) },
    { "Vue 1re / 3e personne", offsetof(PluginConfig, key_view), (long)offsetof(PluginConfig, pad_view) },
    { "Sauter", offsetof(PluginConfig, key_jump), (long)offsetof(PluginConfig, pad_jump) },
    { "S'accroupir", offsetof(PluginConfig, key_crouch), (long)offsetof(PluginConfig, pad_crouch) },
    { "Clavier prio (bascule)", offsetof(PluginConfig, key_prio_kbd), -1 },
    { "Manette prio (bascule)", offsetof(PluginConfig, key_prio_pad), (long)offsetof(PluginConfig, pad_prio_pad) },
    { "Ce menu", offsetof(PluginConfig, key_menu), -1 },
};
#define NUM_ACTIONS ((int)(sizeof(k_actions) / sizeof(k_actions[0])))

void equip_set_config(void *cfg, const char *save_path)
{
    g_cfg_p = (PluginConfig *)cfg;
    g_keys_file[0] = 0;
    if (save_path) {
        size_t l = strlen(save_path);
        snprintf(g_keys_file, sizeof(g_keys_file), "%s%sMXRP_Ragdoll_touches.ini", save_path,
                 (l && save_path[l - 1] != '\\' && save_path[l - 1] != '/') ? "\\" : "");
        if (g_cfg_p && cfg_load(g_cfg_p, g_keys_file, CFG_MENU) && g_log)
            g_log("touches : reglages du menu P lus dans %s", g_keys_file);
    }
}

static void save_opts(FILE *f);
static void save_keys(void);
/* v1.8.110 : tutoriel fermé : options enregistrées tout de suite */
void equip_save_options(void)
{
    save_keys();
}
static void save_keys(void)
{
    FILE *f;
    int i;
    if (!g_cfg_p || !g_keys_file[0] || !(f = fopen(g_keys_file, "w")))
        return;
    fprintf(f, "; MXRP_Ragdoll : touches et options reglees dans le menu P (lu apres MXRP_Ragdoll.ini)\n");
    for (i = 0; i < NUM_ACTIONS; ++i) {
        static const char *k_keys[] = { "key_forward", "key_back", "key_left", "key_right", "key_run",
                                        "key_getup", "key_bike", "key_dismount", "key_view", "key_jump", "key_crouch", "key_prio_kbd",
                                        "key_prio_pad", "key_menu" };
        static const char *k_pads[] = { NULL, NULL, NULL, NULL, "pad_run", "pad_getup", "pad_bike", "pad_dismount", "pad_view",
                                        "pad_jump", "pad_crouch", NULL, "pad_prio_pad", NULL };
        const int *k = (const int *)((const char *)g_cfg_p + k_actions[i].key_off);
        if (k[0] > 0 && k[1] > 0)
            fprintf(f, "%s = %s %s\n", k_keys[i], cfg_key_name(k[0]), cfg_key_name(k[1]));
        else
            fprintf(f, "%s = %s\n", k_keys[i], k[0] > 0 ? cfg_key_name(k[0]) : "NONE");
        if (k_pads[i])
            fprintf(f, "%s = %s\n", k_pads[i],
                    *(const int *)((const char *)g_cfg_p + k_actions[i].pad_off) ? cfg_pad_name(*(const int *)((const char *)g_cfg_p + k_actions[i].pad_off)) : "NONE");
    }
    save_opts(f);
    fclose(f);
    if (g_log)
        g_log("touches : enregistrees dans %s", g_keys_file);
}

/* attend une touche (clavier) ou un bouton (manette) pour l'action choisie */
static void capture_step(void)
{
#ifdef _WIN32
    static int armed = 0, pad_prev = 0;
    int vk;
    PadState ps;
    int pad_now = 0;
    if (padinject_read(&ps))
        pad_now = ps.buttons | (ps.rt > 0.5f ? PAD_RT : 0) | (ps.lt > 0.5f ? PAD_LT : 0);
    if (!armed) {
        /* on attend que tout soit relâché (Entrée qui a lancé la capture) */
        int any = 0;
        for (vk = 0x08; vk <= 0xFE; ++vk)
            if (vk != VK_LBUTTON && vk != VK_RBUTTON && key(vk))
                any = 1;
        if (!any && !pad_now) {
            armed = 1;
            pad_prev = 0;
        }
        return;
    }
    if (key(VK_ESCAPE)) {
        g_capture = 0;
        armed = 0;
        return;
    }
    for (vk = 0x08; vk <= 0xFE; ++vk) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_ESCAPE || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU)
            continue; /* les touches gauche / droite ont aussi un code commun (SHIFT, CTRL, ALT) */
        if (key(vk)) {
            int *k = (int *)((char *)g_cfg_p + k_actions[g_ksel].key_off);
            k[0] = vk;
            k[1] = 0;
            g_capture = 0;
            armed = 0;
            save_keys();
            return;
        }
    }
    if (k_actions[g_ksel].pad_off >= 0 && pad_now && !pad_prev) {
        *(int *)((char *)g_cfg_p + k_actions[g_ksel].pad_off) = pad_now;
        g_capture = 0;
        armed = 0;
        save_keys();
        return;
    }
    pad_prev = pad_now;
#endif
}

/* lignes d'équipement : 0 casque hauteur, 1 casque avant, 2 bottes hauteur, 3 bottes avant,
   4 pilote hauteur, 5 casque angle */
static int eq_group(int i) { return i == 5 || i < 2 ? G_HELMET : i < 4 ? G_BOOTS : G_RIDER; }
static float *eq_val(int i, int si)
{
    if (i == 5) return &g_set[si].pitch;
    return (i % 2 == 0 || i == 4) ? &g_set[si].up : &g_set[si].fwd;
}
static float eq_lim(int i) { return i == 5 ? 45.0f : i == 4 ? 2.00f : 0.20f; }
static float eq_step(int i, int fine)
{
    if (i == 5) return fine ? 0.2f : 1.0f;
    return fine ? 0.001f : (i == 4 ? 0.02f : 0.005f);
}

/* ---------- menu P : une seule liste (équipement, options, touches), sans Tab ---------- */
enum { R_HEAD, R_EQUIP, R_OPT, R_KEY };
typedef struct { int type, idx; const char *head; } Row;

/* options réglables du menu (enregistrées avec les touches) */
typedef struct {
    const char *label, *ini;
    size_t off;
    int is_int;
    float step, lo, hi;
    const char *unit;
} Opt;
static const Opt k_opts[] = {
    { "Langue", "language", offsetof(PluginConfig, language), 2, 1.0f, 0.0f, (float)LANG_AUTO, "" }, /* v1.8.118 : + Auto */
    { "Fleche sur la moto au sol", "bike_marker", offsetof(PluginConfig, bike_marker), 1, 1.0f, 0.0f, 1.0f, "" },
    { "Sensibilite souris", "mouse_sensitivity", offsetof(PluginConfig, mouse_sensitivity), 0, 0.0005f, 0.0005f, 0.02f, "" },
    /* v1.8.162 : champ de vision en 1re personne (0 = celui du jeu) */
    { "Champ de vision a pied (1re personne)", "fov_1re_personne", offsetof(PluginConfig, fov_fp), 0, 1.0f, 0.0f, 140.0f, " deg" },
    /* v1.8.107 : noms des boutons affichés (3 = liste Auto / Xbox / PlayStation) */
    { "Boutons de la manette", "boutons_manette", offsetof(PluginConfig, pad_style), 3, 1.0f, 0.0f, 2.0f, "" },
    /* v1.8.110 : OUI = la fenêtre du tutoriel s'ouvre en fermant le menu P */
    { "Afficher le tutoriel", "tutoriel", offsetof(PluginConfig, tutorial), 1, 1.0f, 0.0f, 1.0f, "" },
    /* v1.8.111 : messages « garde appuyé » et leur barre de progression */
    { "Messages et barres de progression", "barres_progression", offsetof(PluginConfig, hold_msgs), 1, 1.0f, 0.0f, 1.0f, "" },
    /* v1.8.172 : au lancement, la dernière release est installée si elle est plus récente */
    { "Mises a jour automatiques", "mises_a_jour_auto", offsetof(PluginConfig, auto_update), 1, 1.0f, 0.0f, 1.0f, "" },
    /* v1.8.175 : OUI = une fenêtre demande avant d'installer ; NON = installée sans demander */
    { "Demander avant d'installer une mise a jour", "maj_demander", offsetof(PluginConfig, update_ask), 1, 1.0f, 0.0f, 1.0f, "" },
    /* v1.8.200 : NON de base : l'hôte d'un serveur RAGDOLL choisit d'expulser les joueurs sans plugin */
    /* v1.8.204 : multi coupé tant que le joueur ne l'a pas activé (question au premier lancement) */
    { "Multi : partager avec les autres joueurs du plugin", "multi", offsetof(PluginConfig, online_ok), 1, 1.0f, 0.0f, 1.0f, "" },
    { "Expulser les joueurs sans plugin (mon serveur)", "expulsion_auto", offsetof(PluginConfig, auto_kick), 1, 1.0f, 0.0f, 1.0f, "" },
};
#define NUM_OPTS ((int)(sizeof(k_opts) / sizeof(k_opts[0])))

static Row g_rows[80];
static int g_nrows = 0, g_row = 1, g_scroll = 0;

static void build_rows(void)
{
    int i, n = 0;
    /* menu P : seulement la souris et les touches */
    if (g_cfg_p) {
        /* v1.1 : réglages du pilote et de l'équipement de retour (par modèle) */
        static const int k_eq_order[] = { 4, 0, 1, 5, 2, 3 };
        g_rows[n].type = R_HEAD; g_rows[n++].head = "PILOTE ET EQUIPEMENT";
        for (i = 0; i < 6; ++i) { g_rows[n].type = R_EQUIP; g_rows[n++].idx = k_eq_order[i]; }
        g_rows[n].type = R_HEAD; g_rows[n++].head = "LANGUE";
        g_rows[n].type = R_OPT; g_rows[n++].idx = 0;
        g_rows[n].type = R_HEAD; g_rows[n++].head = "AFFICHAGE";
        g_rows[n].type = R_OPT; g_rows[n++].idx = 1;
        g_rows[n].type = R_OPT; g_rows[n++].idx = 5; /* v1.8.111 : avec la flèche sur la moto */
        /* v1.8.108 : option « objets optimisés » retirée (le dessin rapide la rend inutile) */
        g_rows[n].type = R_HEAD; g_rows[n++].head = "SOURIS";
        g_rows[n].type = R_OPT; g_rows[n++].idx = 2;
        g_rows[n].type = R_HEAD; g_rows[n++].head = "MANETTE"; /* v1.8.107 */
        g_rows[n].type = R_OPT; g_rows[n++].idx = 3;
        g_rows[n].type = R_HEAD; g_rows[n++].head = "AIDE"; /* v1.8.110 */
        g_rows[n].type = R_OPT; g_rows[n++].idx = 4;
        g_rows[n].type = R_HEAD; g_rows[n++].head = "TOUCHES";
        for (i = 0; i < NUM_ACTIONS; ++i) { g_rows[n].type = R_KEY; g_rows[n++].idx = i; }
    }
    g_nrows = n;
    if (g_row >= n || g_rows[g_row].type == R_HEAD)
        g_row = 1;
}

static void save_opts(FILE *f)
{
    int i;
    for (i = 0; i < NUM_OPTS; ++i) {
        const void *p = (const char *)g_cfg_p + k_opts[i].off;
        if (k_opts[i].is_int)
            fprintf(f, "%s = %d\n", k_opts[i].ini, *(const int *)p);
        else
            fprintf(f, "%s = %.4f\n", k_opts[i].ini, *(const float *)p);
    }
    fprintf(f, "multi_demande = %d\n", g_cfg_p->online_asked); /* v1.8.204 */
    fprintf(f, "expulsion_info = %d\n", g_cfg_p->auto_kick_info); /* v1.8.200 : messages déjà montrés */
}

void equip_menu_keys(int focused, int vk1, int vk2)
{
#ifdef _WIN32
    static int prev_p = 0, prev_ud = 0, prev_del = 0, prev_enter = 1, hold = 0, opt_dirty = 0;
    int p = focused && ((vk1 > 0 && key(vk1)) || (vk2 > 0 && key(vk2)));
    Row *r;
    if (p && !prev_p) {
        g_open = !g_open;
        if (!g_open) {
            save_file();
            if (opt_dirty) {
                save_keys(); /* (les options sont enregistrées avec les touches) */
                opt_dirty = 0;
            }
        }
    }
    prev_p = p;
    if (!g_open || !focused) {
        hold = 0;
        g_capture = 0;
        return;
    }
    if (g_capture) {
        capture_step();
        return;
    }
    build_rows();
    {
        /* Haut / Bas : ligne suivante (les titres sont sautés), en continu si on garde la touche */
        static int ud_hold = 0;
        int ud = key(VK_UP) ? -1 : key(VK_DOWN) ? 1 : 0;
        if (ud && (ud != prev_ud || (ud_hold > 30 && ud_hold % 5 == 0))) {
            int k = g_row;
            do
                k = (k + ud + g_nrows) % g_nrows;
            while (g_rows[k].type == R_HEAD);
            g_row = k;
        }
        ud_hold = ud ? ud_hold + 1 : 0;
        prev_ud = ud;
        if (key(VK_PRIOR) || key(VK_HOME))
            g_row = 1;
    }
    r = &g_rows[g_row];
    if (r->type == R_KEY) {
        int en = key(VK_RETURN);
        if (en && !prev_enter) {
            g_ksel = r->idx;
            g_capture = 1;
        }
        prev_enter = en;
        hold = 0;
        return;
    }
    prev_enter = key(VK_RETURN);
    {
        int lr = key(VK_RIGHT) ? 1 : key(VK_LEFT) ? -1 : 0;
        int del = key(VK_DELETE) || key(VK_BACK);
        int step_now = lr && (hold == 0 || (hold > 35 && hold % 4 == 0));
        hold = lr ? hold + 1 : 0;
        if (r->type == R_EQUIP) {
            int i = r->idx, group = eq_group(i), si = g_cur[group];
            float lim = eq_lim(i);
            if (si >= 0 && si < g_nset) {
                float *v = eq_val(i, si);
                if (step_now) {
                    float step = eq_step(i, key(VK_SHIFT));
                    *v += lr * step;
                    if (*v > lim) *v = lim;
                    if (*v < -lim) *v = -lim;
                    *v = roundf(*v * 1000.0f) / 1000.0f;
                    g_dirty = 1;
                }
                if (del && !prev_del) {
                    *v = 0.0f;
                    g_dirty = 1;
                }
            }
        } else if (r->type == R_OPT && g_cfg_p) {
            const Opt *o = &k_opts[r->idx];
            void *pv = (char *)g_cfg_p + o->off;
            int en = key(VK_RETURN);
            if (o->is_int >= 2) {
                /* choix dans une liste (langue ; v1.8.107 : boutons de la manette) */
                int *iv = (int *)pv, nmax = (int)o->hi + 1;
                if (step_now && lr) {
                    *iv = ((*iv + lr) % nmax + nmax) % nmax;
                    if (o->off == offsetof(PluginConfig, language))
                        lang_set(*iv);
                    if (o->off == offsetof(PluginConfig, pad_style))
                        padstyle_set_mode(*iv);
                    opt_dirty = 1;
                }
            } else if (o->is_int) {
                int *iv = (int *)pv;
                if ((step_now && lr) || (en && !prev_enter)) {
                    *iv = !*iv;
                    opt_dirty = 1;
                    /* v1.8.111 : « Afficher le tutoriel » mis sur OUI : le menu se ferme et le tutoriel
                       s'ouvre tout de suite (avant : il fallait fermer le menu, et rien ne se voyait) */
                    if (o->off == offsetof(PluginConfig, tutorial) && *iv) {
                        g_open = 0;
                        save_file();
                        save_keys();
                        opt_dirty = 0;
                        g_tuto_now = 1;
                    }
                }
            } else if (step_now) {
                float *fv = (float *)pv, st = key(VK_SHIFT) ? o->step / 5.0f : o->step;
                /* v1.8.162 : champ de vision : part de celui du jeu ; en dessous de 30 : celui du jeu (0) */
                if (o->off == offsetof(PluginConfig, fov_fp) && *fv < 1.0f && lr) {
                    float g = glcam_ride_fov() > 20.0f ? glcam_ride_fov() : glcam_game_hfov(); /* (v1.8.165 : celui de la moto) */
                    *fv = (g > 20.0f ? floorf(g + 0.5f) : 90.0f) - lr * st;
                }
                *fv += lr * st;
                if (o->off == offsetof(PluginConfig, fov_fp) && *fv < 30.0f)
                    *fv = 0.0f;
                if (*fv < o->lo) *fv = o->lo;
                if (*fv > o->hi) *fv = o->hi;
                opt_dirty = 1;
            }
        }
        prev_del = del;
    }
#else
    (void)focused;
    (void)vk1;
    (void)vk2;
#endif
}

static void quad(SPluginQuad_t *q, float x0, float y0, float x1, float y1, unsigned long c)
{
    q->m_aafPos[0][0] = x0; q->m_aafPos[0][1] = y0;
    q->m_aafPos[1][0] = x0; q->m_aafPos[1][1] = y1;
    q->m_aafPos[2][0] = x1; q->m_aafPos[2][1] = y1;
    q->m_aafPos[3][0] = x1; q->m_aafPos[3][1] = y0;
    q->m_iSprite = 0;
    q->m_ulColor = c;
}

static void text(SPluginString_t *s, const char *t, float x, float y, float size, unsigned long c)
{
    snprintf(s->m_szString, sizeof(s->m_szString), "%s", t);
    s->m_afPos[0] = x;
    s->m_afPos[1] = y;
    s->m_iFont = 1;
    s->m_fSize = size;
    s->m_iJustify = 0;
    s->m_ulColor = c;
}

int equip_menu_draw(SPluginQuad_t *q, int max_q, SPluginString_t *s, int max_s, int *ns, int font, float aspect)
{
    const float x0 = 0.03f, y0 = 0.10f, w = 0.46f, row = 0.032f;
    const int vis = 16; /* lignes visibles */
    static const char *k_eq[6] = { "Casque : hauteur", "Casque : avant / arriere", "Bottes : hauteur",
                                   "Bottes : avant / arriere", "Pilote : hauteur", "Casque : angle haut / bas" };
    int nq = 0, n = 0, i, k;
    char t[100];
    const char *help = "";
    *ns = 0;
    if (!g_open || max_q < 8)
        return 0;
    build_rows();
    if (g_row < g_scroll + 1)
        g_scroll = g_row - 1;
    if (g_row >= g_scroll + vis)
        g_scroll = g_row - vis + 1;
    if (g_scroll < 0)
        g_scroll = 0;
    /* fond, bandeau, titre */
    quad(&q[nq++], x0, y0, x0 + w, y0 + 0.07f + vis * row + 0.07f, 0xE0101418);
    quad(&q[nq++], x0, y0, x0 + w, y0 + 0.055f, 0xFF0078D0);
    if (font && n < max_s)
        text(&s[n++], tr("MXRP Ragdoll  -  reglages"), x0 + 0.014f, y0 + 0.014f, 0.026f, 0xFFFFFFFF);
    for (k = 0; k < vis && g_scroll + k < g_nrows; ++k) {
        const Row *r = &g_rows[g_scroll + k];
        float y = y0 + 0.07f + k * row;
        int sel = g_scroll + k == g_row;
        if (r->type == R_HEAD) {
            if (nq < max_q)
                quad(&q[nq++], x0 + 0.008f, y + row - 0.008f, x0 + w - 0.008f, y + row - 0.006f, 0xFF0078D0);
            if (font && n < max_s)
                text(&s[n++], tr(r->head), x0 + 0.012f, y + 0.004f, 0.020f, 0xFF00B4FF);
            continue;
        }
        if (sel && nq < max_q)
            quad(&q[nq++], x0 + 0.005f, y - 0.002f, x0 + w - 0.005f, y + row - 0.004f, g_capture ? 0xB000A0FF : 0x700078D0);
        if (r->type == R_EQUIP) {
            int i2 = r->idx, group = eq_group(i2), si = g_cur[group];
            float v = si >= 0 ? *eq_val(i2, si) : 0.0f;
            float lim = eq_lim(i2), cx = x0 + w * 0.80f, half = w * 0.12f, bar = v / lim * half;
            if (nq + 2 <= max_q) {
                quad(&q[nq++], cx - half, y + 0.013f, cx + half, y + 0.017f, 0x80FFFFFF);
                quad(&q[nq++], bar < 0 ? cx + bar : cx, y + 0.008f, bar < 0 ? cx : cx + bar + 0.002f, y + 0.022f, 0xFF00A0FF);
            }
            if (font && n < max_s) {
                if (i2 == 5)
                    snprintf(t, sizeof(t), "%-26s %+.1f deg", tr(k_eq[i2]), v);
                else
                    snprintf(t, sizeof(t), "%-26s %+.3f m", tr(k_eq[i2]), v);
                text(&s[n++], t, x0 + 0.016f, y + 0.004f, 0.018f, si >= 0 ? 0xFFFFFFFF : 0xFF808080);
            }
            if (sel) {
                snprintf(t, sizeof(t), "%s : %s", tr(group == G_HELMET ? "Casque" : group == G_BOOTS ? "Bottes" : "Pilote"),
                         si >= 0 ? g_set[si].name : tr("(pas encore vu en piste)"));
                help = "Gauche/Droite : regler (Maj = fin)   Suppr : 0";
                if (font && n < max_s)
                    text(&s[n++], t, x0 + 0.014f, y0 + 0.07f + vis * row + 0.010f, 0.017f, 0xFF00B4FF);
            }
        } else if (r->type == R_OPT && g_cfg_p) {
            const Opt *o = &k_opts[r->idx];
            const void *pv = (const char *)g_cfg_p + o->off;
            if (font && n < max_s) {
                if (o->is_int == 3)
                    snprintf(t, sizeof(t), "%-30s < %s >", tr(o->label), padstyle_mode_name(*(const int *)pv));
                else if (o->is_int == 2)
                    snprintf(t, sizeof(t), "%-30s < %s >", tr(o->label), lang_name(*(const int *)pv));
                else if (o->is_int)
                    snprintf(t, sizeof(t), "%-30s %s", tr(o->label), *(const int *)pv ? tr("OUI") : tr("NON"));
                else if (o->off == offsetof(PluginConfig, fov_fp) && *(const float *)pv < 1.0f)
                    snprintf(t, sizeof(t), "%-30s %s", tr(o->label), tr("comme sur la moto"));
                else if (o->off == offsetof(PluginConfig, fov_fp))
                    snprintf(t, sizeof(t), "%-30s %.0f%s", tr(o->label), *(const float *)pv, o->unit);
                else if (o->step < 0.01f)
                    snprintf(t, sizeof(t), "%-30s %.1f%s", tr(o->label), *(const float *)pv * 1000.0f, o->unit);
                else
                    snprintf(t, sizeof(t), "%-30s %.2f%s", tr(o->label), *(const float *)pv, o->unit);
                text(&s[n++], t, x0 + 0.016f, y + 0.004f, 0.018f, 0xFFFFFFFF);
            }
            if (sel)
                help = tr(o->is_int == 3 ? "Gauche/Droite : Auto / Xbox / PlayStation"
                          : o->is_int == 2 ? "Gauche/Droite : changer de langue"
                          : o->is_int    ? "Gauche/Droite ou Entree : oui / non"
                                         : "Gauche/Droite : regler (Maj = fin)");
        } else if (r->type == R_KEY && g_cfg_p) {
            const Action *a = &k_actions[r->idx];
            const int *kk = (const int *)((const char *)g_cfg_p + a->key_off);
            char ks[40];
            if (kk[1] > 0)
                snprintf(ks, sizeof(ks), "%s %s", cfg_key_name(kk[0]), cfg_key_name(kk[1]));
            else
                snprintf(ks, sizeof(ks), "%s", cfg_key_name(kk[0]));
            if (font && n < max_s) {
                if (a->pad_off >= 0) {
                    /* v1.8.109 : le bouton de la manette après le texte (symbole PlayStation dessiné) */
                    snprintf(t, sizeof(t), "%-24s %-8s %s ", tr(a->label), ks, tr("manette"));
                    text(&s[n++], t, x0 + 0.016f, y + 0.004f, 0.018f, 0xFFFFFFFF);
                    padicon_text(q, max_q, &nq, s, max_s, &n, cfg_pad_name(*(const int *)((const char *)g_cfg_p + a->pad_off)),
                                 x0 + 0.016f + padicon_text_width(t, 0.018f, aspect), y + 0.004f, 0.018f, 0xFFFFFFFF, 0, aspect);
                } else {
                    snprintf(t, sizeof(t), "%-24s %s", tr(a->label), ks);
                    text(&s[n++], t, x0 + 0.016f, y + 0.004f, 0.018f, 0xFFFFFFFF);
                }
            }
            if (sel)
                help = tr(g_capture ? "Appuie sur une touche ou un bouton...  (Echap : annuler)"
                                    : "Entree : changer la touche / le bouton");
        }
    }
    /* barre de défilement */
    if (g_nrows > vis && nq + 2 <= max_q) {
        float top = y0 + 0.07f, hgt = vis * row, a = (float)g_scroll / g_nrows, b = (float)(g_scroll + vis) / g_nrows;
        quad(&q[nq++], x0 + w - 0.006f, top, x0 + w - 0.003f, top + hgt, 0x40FFFFFF);
        quad(&q[nq++], x0 + w - 0.006f, top + a * hgt, x0 + w - 0.003f, top + b * hgt, 0xFF0078D0);
    }
    if (font && n < max_s)
        text(&s[n++], help, x0 + 0.014f, y0 + 0.07f + vis * row + 0.034f, 0.016f, g_capture ? 0xFF00C0FF : 0xFFC0C0C0);
    if (font && n < max_s)
        text(&s[n++], tr("Haut/Bas : choisir      P : fermer et enregistrer"), x0 + 0.014f, y0 + 0.07f + vis * row + 0.054f, 0.016f,
             0xFFC0C0C0);
    (void)i;
    *ns = n;
    return nq;
}

/* v1.8.21 : casque et bottes connus (chemin du modèle, en minuscules avec « \\ ») */
int equip_each_gear(EquipGearFn fn, void *user)
{
    int i, n = 0;
    for (i = 0; i < g_nmodels; ++i) {
        Model *m = &g_models[i];
        char path[300];
        uint64_t sig = 0;
        int k;
        if (m->kind != K_HELMET && m->kind != K_BOOT_L && m->kind != K_BOOT_R)
            continue;
        if (!safe_read((const void *)m->p, &sig, sizeof(sig)) || sig != m->sig)
            continue;
        memset(path, 0, sizeof(path));
        if (!safe_read((const void *)m->p, path, sizeof(path) - 1))
            continue;
        for (k = 0; path[k]; ++k)
            path[k] = (char)(path[k] >= 'A' && path[k] <= 'Z' ? path[k] + 32 : (path[k] == '/' ? '\\' : path[k]));
        fn(path, (void *)m->p, user);
        ++n;
    }
    return n;
}

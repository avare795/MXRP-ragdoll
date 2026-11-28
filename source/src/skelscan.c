/*
 * skelscan.c - recherche des os du pilote dans la mémoire du jeu
 *
 * Constat de la v0.2.0 (beta21e) : la fonction interceptée met à jour TOUS
 * les modèles 3D (objets de piste compris), pas seulement le pilote.
 * Chaque structure commence par le chemin de son fichier .edf, ce qui permet
 * de reconnaître les modèles du pilote.
 */
#include "skelscan.h"
#include "hook.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_SKELETONS     1024
#define NAME_BYTES        128
#define ROOT_BYTES        4096   /* taille lue dans chaque structure */
#define CHILD_BYTES       2048   /* taille lue derrière chaque pointeur trouvé */
#define MAX_CHILDREN      64     /* pointeurs suivis par structure */
#define MAX_LINES_PER_RUN 400    /* évite un journal énorme */
#define NODE_STRIDE       0x50   /* écart observé entre deux positions d'os */
#define DUMP_NODES        24

/* Disposition observée en v0.2.1 (beta21e), structure d'un modèle :
 *   +0x000 chemin du fichier
 *   +0x11C matrice monde 3x4 (lignes : rotation | translation)
 *   +0x220 pointeur vers la hiérarchie des nœuds, +0x228 nombre de nœuds (75 pour rider.hrc) */
#define OFF_WORLD      0x11C
#define OFF_NODES      0x220
#define OFF_NODE_COUNT 0x228
#define MAX_NODES      128
#define NODE_DUMP      0x100

typedef struct {
    void *ptr;
    uint64_t updates;
    char name[NAME_BYTES];
    int is_rider;
    int kind;            /* SKEL_KIND_* */
} SkelEntry;

static SkelEntry g_skels[MAX_SKELETONS];
/* table de hachage pointeur -> index (+1), pour rester rapide : la fonction
 * interceptée est appelée des milliers de fois par seconde */
#define HASH_SIZE 4096
static int16_t g_hash[HASH_SIZE];
static int g_num_skels = 0;
static int g_overflow = 0;
static int g_listed_all = 0;
/* (la liste complète des modèles n'est écrite qu'une fois) */
static ScanLogFn g_log = NULL;
static int g_budget = 0;

static uint8_t g_root_buf[ROOT_BYTES];
static uint8_t g_child_buf[CHILD_BYTES];

/* Mots qui désignent un modèle du pilote dans le chemin du fichier */
static const char *k_rider_words[] = {
    "rider", "riders", "helmet", "boots", "gloves", "protection", "neckbrace", "armour", "goggle",
};

void skelscan_init(ScanLogFn log)
{
    g_log = log;
    skelscan_reset();
}

void skelscan_reset(void)
{
    memset(g_skels, 0, sizeof(g_skels));
    memset(g_hash, 0, sizeof(g_hash));
    g_num_skels = 0;
    g_overflow = 0;
}

/* Copie le texte du début de la structure s'il ressemble à un chemin de fichier */
static void read_name(const void *ptr, char *out, size_t size)
{
    uint8_t buf[NAME_BYTES];
    size_t i;
    out[0] = '\0';
    if (!safe_read(ptr, buf, sizeof(buf)))
        return;
    for (i = 0; i < sizeof(buf) && i + 1 < size; ++i) {
        if (buf[i] == 0)
            break;
        if (!isprint(buf[i])) {
            out[0] = '\0';
            return;
        }
        out[i] = (char)buf[i];
    }
    if (i == sizeof(buf))
        i = size - 1; /* pas de fin de chaîne : on tronque */
    out[i] = '\0';
    if (i < 4)
        out[0] = '\0';
}

static int name_is_rider(const char *name)
{
    char low[NAME_BYTES];
    size_t i, k;
    for (i = 0; name[i] && i + 1 < sizeof(low); ++i)
        low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = '\0';
    if (strstr(low, "tracks"))
        return 0;
    for (k = 0; k < sizeof(k_rider_words) / sizeof(k_rider_words[0]); ++k)
        if (strstr(low, k_rider_words[k]))
            return 1;
    return 0;
}

static int ends_with(const char *s, const char *end)
{
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !strcmp(s + a - b, end);
}

static int classify(const char *name)
{
    /* tous les modèles de pilote (mods compris) : ...\riders\<nom>\rider.hrc, casse et
       séparateurs quelconques */
    char low[260];
    size_t i, n = strlen(name);
    if (n >= sizeof(low))
        n = sizeof(low) - 1;
    for (i = 0; i < n; ++i) {
        char c = name[i];
        low[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : (c == '/' ? '\\' : c));
    }
    low[n] = 0;
    if (!strstr(low, "riders"))
        return SKEL_KIND_OTHER;
    if (ends_with(low, "\\rider.hrc"))
        return SKEL_KIND_RIDER_HRC;
    if (ends_with(low, "\\rider.edf"))
        return SKEL_KIND_RIDER_EDF;
    if (ends_with(low, "\\c_rider.edf"))
        return SKEL_KIND_RIDER_CEDF;
    if (ends_with(low, "rider.edf"))
        return SKEL_KIND_RIDER_MESH;
    return SKEL_KIND_OTHER;
}

static unsigned hash_ptr(const void *p)
{
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    return (unsigned)(v & (HASH_SIZE - 1));
}

int skelscan_record(void *skeleton)
{
    unsigned h;
    SkelEntry *e;
    if (!skeleton)
        return -1;
    for (h = hash_ptr(skeleton);; h = (h + 1) & (HASH_SIZE - 1)) {
        int idx = g_hash[h];
        if (idx == 0)
            break;
        if (g_skels[idx - 1].ptr == skeleton) {
            ++g_skels[idx - 1].updates;
            return idx - 1;
        }
    }
    if (g_num_skels >= MAX_SKELETONS) {
        g_overflow = 1;
        return -1;
    }
    e = &g_skels[g_num_skels++];
    e->ptr = skeleton;
    e->updates = 1;
    read_name(skeleton, e->name, sizeof(e->name));
    e->is_rider = name_is_rider(e->name);
    e->kind = classify(e->name);
    g_hash[h] = (int16_t)g_num_skels;
    return g_num_skels - 1;
}

void skelscan_log_table(void)
{
    int i, riders = 0, unnamed = 0;
    if (!g_log)
        return;
    for (i = 0; i < g_num_skels; ++i) {
        if (g_skels[i].is_rider)
            ++riders;
        if (!g_skels[i].name[0])
            ++unnamed;
    }
    g_log("modeles vus : %d%s, dont %d du pilote, %d sans nom (mises a jour totales %llu)",
          g_num_skels, g_overflow ? " (liste pleine)" : "", riders, unnamed,
          (unsigned long long)hook_call_count());
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        if (g_listed_all && !e->is_rider && e->name[0])
            continue; /* après la première fois, les objets de piste ne sont plus listés */
        g_log("  [%d] %p %s : %llu mises a jour, \"%s\"", i, e->ptr,
              e->is_rider ? "PILOTE" : (e->name[0] ? "objet" : "sans nom"),
              (unsigned long long)e->updates, e->name);
    }
    g_listed_all = 1;
}

/* Lit le plus possible : essaie la taille entière puis des morceaux plus petits */
static size_t read_block(const uint8_t *addr, uint8_t *out, size_t size)
{
    size_t got = 0;
    if (safe_read(addr, out, size))
        return size;
    while (got + 256 <= size && safe_read(addr + got, out + got, 256))
        got += 256;
    return got;
}

static int near_center(const float *v, const float c[3], float r, float *dist)
{
    float dx, dy, dz;
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]))
        return 0;
    dx = v[0] - c[0];
    dy = v[1] - c[1];
    dz = v[2] - c[2];
    if (fabsf(dx) > r || fabsf(dy) > r || fabsf(dz) > r)
        return 0;
    *dist = sqrtf(dx * dx + dy * dy + dz * dz);
    return *dist <= r;
}

static int scan_floats(const char *prefix, const uint8_t *buf, size_t size, const float c[3],
                       float r)
{
    size_t off;
    int hits = 0;
    for (off = 0; off + 12 <= size; off += 4) {
        float v[3], d;
        memcpy(v, buf + off, sizeof(v));
        if (!near_center(v, c, r, &d))
            continue;
        ++hits;
        if (g_budget > 0) {
            --g_budget;
            g_log("    %s+0x%03X : (%.3f %.3f %.3f) a %.2f m", prefix, (unsigned)off, v[0], v[1],
                  v[2], d);
        }
        off += 8; /* ne pas redécouper le même triplet */
    }
    return hits;
}

static int looks_like_pointer(uint64_t v)
{
    return v > 0x10000ull && v < 0x00007FFFFFFFFFFFull && (v & 7) == 0;
}

/* Écrit des blocs de NODE_STRIDE octets, 20 flottants par ligne */
static void dump_nodes(const uint8_t *buf, size_t size)
{
    size_t off;
    int n = 0;
    for (off = 0; off + NODE_STRIDE <= size && n < DUMP_NODES && g_budget > 0; off += NODE_STRIDE) {
        float f[NODE_STRIDE / 4];
        char line[400];
        int k, len = 0;
        memcpy(f, buf + off, sizeof(f));
        for (k = 0; k < NODE_STRIDE / 4 && len < (int)sizeof(line) - 16; ++k)
            len += snprintf(line + len, sizeof(line) - len, " %.3f", f[k]);
        g_log("      noeud %2d (+0x%03X) :%s", n, (unsigned)off, line);
        --g_budget;
        ++n;
    }
}

static void dump_header(const uint8_t *buf, size_t size)
{
    size_t off;
    for (off = 0; off + 16 <= size && g_budget > 0; off += 16) {
        float f[4];
        uint64_t q[2];
        memcpy(f, buf + off, sizeof(f));
        memcpy(q, buf + off, sizeof(q));
        if (!q[0] && !q[1])
            continue;
        g_log("    +0x%03X : %12.4g %12.4g %12.4g %12.4g | %016llX %016llX", (unsigned)off, f[0],
              f[1], f[2], f[3], (unsigned long long)q[0], (unsigned long long)q[1]);
        --g_budget;
    }
}

static void scan_one(int i, const float center[3], float radius, int dump)
{
    const SkelEntry *e = &g_skels[i];
    const uint8_t *root = (const uint8_t *)e->ptr;
    size_t got = read_block(root, g_root_buf, ROOT_BYTES);
    size_t off;
    int hits, children = 0, child_hits = 0;
    char prefix[64];

    if (got == 0) {
        g_log("  [%d] %p illisible", i, (const void *)root);
        return;
    }
    g_log("  [%d] %p \"%s\"", i, (const void *)root, e->name);
    if (dump)
        dump_header(g_root_buf, got < 0x400 ? got : 0x400);

    snprintf(prefix, sizeof(prefix), "[%d]", i);
    hits = scan_floats(prefix, g_root_buf, got, center, radius);

    /* un niveau plus loin : les structures pointées */
    for (off = 0; off + 8 <= got && children < MAX_CHILDREN; off += 8) {
        uint64_t v;
        size_t cgot;
        int h;
        memcpy(&v, g_root_buf + off, sizeof(v));
        if (!looks_like_pointer(v))
            continue;
        cgot = read_block((const uint8_t *)(uintptr_t)v, g_child_buf, CHILD_BYTES);
        if (cgot == 0)
            continue;
        ++children;
        snprintf(prefix, sizeof(prefix), "[%d]+0x%03X->", i, (unsigned)off);
        h = scan_floats(prefix, g_child_buf, cgot, center, radius);
        child_hits += h;
        if (dump && h >= 3) {
            g_log("    tableau pointe par +0x%03X (%016llX), %d position(s) :", (unsigned)off,
                  (unsigned long long)v, h);
            dump_nodes(g_child_buf, cgot);
        }
    }
    g_log("  [%d] : %d position(s) directe(s), %d via %d pointeur(s)", i, hits, child_hits,
          children);
}

static void dump_text_or_floats(const char *title, const uint8_t *buf, size_t size)
{
    size_t off;
    char line[400];
    for (off = 0; off + 16 <= size && g_budget > 0; off += 16) {
        float f[4];
        uint64_t q[2];
        char txt[17];
        int k;
        memcpy(f, buf + off, sizeof(f));
        memcpy(q, buf + off, sizeof(q));
        for (k = 0; k < 16; ++k)
            txt[k] = isprint(buf[off + k]) ? (char)buf[off + k] : '.';
        txt[16] = '\0';
        snprintf(line, sizeof(line), "%s+0x%03X : %10.4g %10.4g %10.4g %10.4g | %016llX %016llX | %s",
                 title, (unsigned)off, f[0], f[1], f[2], f[3], (unsigned long long)q[0],
                 (unsigned long long)q[1], txt);
        g_log("%s", line);
        --g_budget;
    }
}

/*
 * Disposition (beta21e), d'après l'analyse du jeu :
 *   modèle +0x1F0 : jusqu'à 4 niveaux de détail (LOD), pointeurs
 *   LOD    +0x10  : nombre de nœuds (int), +0x18 : tableau de nœuds
 *   nœud   : 0x3F8 octets, commence par son nom ; +0x100 : matrice monde
 *            3x4 par lignes [r r r tx][r r r ty][r r r tz] puis (0 0 0 1)
 */
#define OFF_LODS        0x1F0
#define LOD_COUNT       0x10
#define LOD_ARRAY       0x18
#define NODE_SIZE       0x3F8
#define NODE_WORLD      0x100

static void dump_hierarchy(int i, int raw)
{
    const uint8_t *root = (const uint8_t *)g_skels[i].ptr;
    uint64_t lod = 0, arr = 0;
    int32_t count = 0;
    float w[12];
    static uint8_t node[NODE_SIZE];
    int k;

    if (safe_read(root + OFF_WORLD, w, sizeof(w)))
        g_log("  HIERARCHIE [%d] \"%s\" : monde pos (%.3f %.3f %.3f)", i, g_skels[i].name, w[3],
              w[7], w[11]);
    if (!safe_read(root + OFF_LODS, &lod, sizeof(lod)) || !looks_like_pointer(lod) ||
        !safe_read((const uint8_t *)(uintptr_t)lod + LOD_COUNT, &count, sizeof(count)) ||
        !safe_read((const uint8_t *)(uintptr_t)lod + LOD_ARRAY, &arr, sizeof(arr))) {
        g_log("    LOD 0 illisible");
        return;
    }
    g_log("    LOD 0 @ %016llX : %d noeud(s) a %016llX", (unsigned long long)lod, count,
          (unsigned long long)arr);
    if (count <= 0 || count > 512 || !looks_like_pointer(arr))
        return;

    for (k = 0; k < count && g_budget > 0; ++k) {
        const uint8_t *addr = (const uint8_t *)(uintptr_t)arr + (size_t)k * NODE_SIZE;
        char name[48];
        float m[16];
        size_t n;
        if (!safe_read(addr, node, NODE_SIZE)) {
            g_log("    os %2d illisible", k);
            continue;
        }
        for (n = 0; n + 1 < sizeof(name) && node[n] && isprint(node[n]); ++n)
            name[n] = (char)node[n];
        name[n] = '\0';
        memcpy(m, node + NODE_WORLD, sizeof(m));
        g_log("    os %2d %-28s monde (%8.3f %8.3f %8.3f) | lignes (%.2f %.2f %.2f) (%.2f %.2f %.2f)",
              k, name, m[3], m[7], m[11], m[0], m[1], m[2], m[4], m[5], m[6]);
        --g_budget;
        if (raw && k < 2) {
            char title[24];
            snprintf(title, sizeof(title), "      os%d", k);
            dump_text_or_floats(title, node, NODE_SIZE);
        }
    }
}

void skelscan_check_locals(void *skeleton);

void skelscan_dump_rider(void)
{
    int i;
    if (!g_log)
        return;
    g_budget = 2500;
    for (i = 0; i < g_num_skels; ++i)
        if (g_skels[i].kind == SKEL_KIND_RIDER_HRC) {
            dump_hierarchy(i, 0);
            skelscan_check_locals(g_skels[i].ptr);
        }
}

void skelscan_scan(const float center[3], float radius, int dump, const char *label)
{
    int i, riders = 0;
    if (!g_log)
        return;
    g_budget = MAX_LINES_PER_RUN;
    for (i = 0; i < g_num_skels; ++i)
        riders += g_skels[i].is_rider;
    g_log("SCAN %s : moto (%.2f %.2f %.2f), rayon %.1f m, %d modele(s) du pilote", label,
          center[0], center[1], center[2], radius, riders);

    for (i = 0; i < g_num_skels; ++i)
        if (g_skels[i].is_rider)
            scan_one(i, center, radius, dump);

    if (riders == 0)
        g_log("  aucun modele du pilote reconnu : voir la liste des modeles sans nom");
    if (g_budget <= 0)
        g_log("  (journal limite a %d lignes pour ce scan)", MAX_LINES_PER_RUN);
}

/* ---------- test d'écriture (v0.3.0) ---------- */

const char *skelscan_name(int index)
{
    if (index < 0 || index >= g_num_skels)
        return "";
    return g_skels[index].name;
}

int skelscan_kind(int index)
{
    if (index < 0 || index >= g_num_skels)
        return SKEL_KIND_OTHER;
    return g_skels[index].kind;
}

/*
 * Décale de dy mètres (vers le haut) la translation monde de tous les nœuds
 * de tous les niveaux de détail du modèle. Retourne le nombre de nœuds modifiés.
 * N'écrit que dans des nœuds qu'on a pu relire et dont la matrice est saine.
 */
int skelscan_lift_ex(void *skeleton, float dy, int with_second)
{
    const uint8_t *root = (const uint8_t *)skeleton;
    int lod_i, total = 0;
    for (lod_i = 0; lod_i < 4; ++lod_i) {
        uint64_t lod = 0, arr = 0;
        int32_t count = 0;
        int k;
        if (!safe_read(root + OFF_LODS + 8 * lod_i, &lod, sizeof(lod)) || !looks_like_pointer(lod))
            continue;
        if (!safe_read((const uint8_t *)(uintptr_t)lod + LOD_COUNT, &count, sizeof(count)) ||
            !safe_read((const uint8_t *)(uintptr_t)lod + LOD_ARRAY, &arr, sizeof(arr)))
            continue;
        if (count <= 0 || count > 512 || !looks_like_pointer(arr))
            continue;
        for (k = 0; k < count; ++k) {
            uint8_t *m = (uint8_t *)(uintptr_t)arr + (size_t)k * NODE_SIZE + NODE_WORLD;
            float row1[4], last[4];
            if (!safe_read(m + 16, row1, sizeof(row1)) || !safe_read(m + 48, last, sizeof(last)))
                continue;
            if (last[3] != 1.0f || !isfinite(row1[3]))
                continue;
            row1[3] += dy;
            if (safe_write(m + 16 + 12, &row1[3], sizeof(float)))
                ++total;
            if (with_second) {
                /* seconde matrice du nœud (+0x140), même disposition supposée */
                float ty;
                if (safe_read(m + 0x40 + 16 + 12, &ty, sizeof(ty)) && isfinite(ty)) {
                    ty += dy;
                    safe_write(m + 0x40 + 16 + 12, &ty, sizeof(ty));
                }
            }
        }
    }
    return total;
}

int skelscan_lift(void *skeleton, float dy)
{
    return skelscan_lift_ex(skeleton, dy, 0);
}

/* Translation verticale de la matrice monde du modèle (+0x11C, ty à +0x138) */
#define OFF_MODEL_TY 0x138

int skelscan_model_ty(void *skeleton, float *ty)
{
    float w[12];
    const uint8_t *root = (const uint8_t *)skeleton;
    int r;
    if (!safe_read(root + OFF_WORLD, w, sizeof(w)))
        return 0;
    /* lignes de rotation de norme ~1 : c'est bien une matrice */
    for (r = 0; r < 3; ++r) {
        float n = w[r * 4] * w[r * 4] + w[r * 4 + 1] * w[r * 4 + 1] + w[r * 4 + 2] * w[r * 4 + 2];
        if (!(n > 0.9f && n < 1.1f))
            return 0;
    }
    *ty = w[7];
    return isfinite(*ty);
}

int skelscan_set_model_ty(void *skeleton, float ty)
{
    return safe_write((uint8_t *)skeleton + OFF_MODEL_TY, &ty, sizeof(ty));
}

/* ---------- matrices locales (v0.3.2) ---------- */

#define NODE_LOCAL 0x68   /* matrice locale 4x4 par lignes, lue par le jeu pendant la mise à jour */

typedef struct { float m[16]; } Mat4;

static void mat_mul(const Mat4 *a, const Mat4 *b, Mat4 *out)
{
    int r, c, k;
    for (r = 0; r < 4; ++r)
        for (c = 0; c < 4; ++c) {
            float v = 0.0f;
            for (k = 0; k < 4; ++k)
                v += a->m[r * 4 + k] * b->m[k * 4 + c];
            out->m[r * 4 + c] = v;
        }
}

static float mat_diff(const Mat4 *a, const Mat4 *b)
{
    float d = 0.0f;
    int i;
    for (i = 0; i < 12; ++i)
        d += fabsf(a->m[i] - b->m[i]);
    return d;
}

/* Retrouve le tableau d'os du LOD lod_i. Retourne le nombre d'os (0 si illisible). */
static int lod_nodes(const void *skeleton, int lod_i, uint8_t **arr_out)
{
    const uint8_t *root = (const uint8_t *)skeleton;
    uint64_t lod = 0, arr = 0;
    int32_t count = 0;
    if (!safe_read(root + OFF_LODS + 8 * lod_i, &lod, sizeof(lod)) || !looks_like_pointer(lod))
        return 0;
    if (!safe_read((const uint8_t *)(uintptr_t)lod + LOD_COUNT, &count, sizeof(count)) ||
        !safe_read((const uint8_t *)(uintptr_t)lod + LOD_ARRAY, &arr, sizeof(arr)))
        return 0;
    if (count <= 0 || count > 512 || !looks_like_pointer(arr))
        return 0;
    *arr_out = (uint8_t *)(uintptr_t)arr;
    return count;
}

/*
 * Vérifie la convention monde = monde(parent) x locale en cherchant, pour
 * chaque os, le parent qui explique le mieux sa matrice monde.
 */
void skelscan_check_locals(void *skeleton)
{
    static Mat4 world[256], local[256];
    static char names[256][32];
    uint8_t *arr;
    int count = lod_nodes(skeleton, 0, &arr), k, p, ok = 0;
    if (!count || !g_log)
        return;
    for (k = 0; k < count; ++k) {
        uint8_t *n = arr + (size_t)k * NODE_SIZE;
        size_t c;
        uint8_t nm[32];
        safe_read(n + NODE_WORLD, world[k].m, sizeof(world[k].m));
        safe_read(n + NODE_LOCAL, local[k].m, sizeof(local[k].m));
        safe_read(n, nm, sizeof(nm));
        for (c = 0; c + 1 < sizeof(nm) && nm[c] && isprint(nm[c]); ++c)
            names[k][c] = (char)nm[c];
        names[k][c] = '\0';
    }
    g_log("  MATRICES LOCALES (+0x%X) de %d os : parent trouve par monde(parent) x locale", NODE_LOCAL,
          count);
    for (k = 0; k < count; ++k) {
        int best = -1;
        float best_err = 1e9f;
        Mat4 t;
        for (p = 0; p < count; ++p) {
            float e;
            if (p == k)
                continue;
            mat_mul(&world[p], &local[k], &t);
            e = mat_diff(&t, &world[k]);
            if (e < best_err) {
                best_err = e;
                best = p;
            }
        }
        if (best_err < 0.01f)
            ++ok;
        g_log("    os %2d %-28s parent %2d %-28s erreur %.4f | locale t(%.3f %.3f %.3f)", k, names[k],
              best, best >= 0 ? names[best] : "-", best_err, local[k].m[3], local[k].m[7],
              local[k].m[11]);
    }
    g_log("  %d/%d os expliques par un parent (erreur < 0.01)", ok, count);
}

/*
 * Si l'animation du jeu ne réécrit pas la matrice locale à chaque image, il ne
 * faut pas tourner encore et encore la même matrice : on garde la matrice
 * d'origine (base) et celle qu'on a écrite.
 */
#define ROT_CACHE 64
typedef struct { uint8_t *node; Mat4 base, written; } RotCache;
static RotCache g_rot_cache[ROT_CACHE];
static int g_rot_cache_n = 0;

static RotCache *rot_cache_get(uint8_t *node)
{
    int i;
    for (i = 0; i < g_rot_cache_n; ++i)
        if (g_rot_cache[i].node == node)
            return &g_rot_cache[i];
    if (g_rot_cache_n >= ROT_CACHE)
        return NULL;
    memset(&g_rot_cache[g_rot_cache_n], 0, sizeof(RotCache));
    g_rot_cache[g_rot_cache_n].node = node;
    return &g_rot_cache[g_rot_cache_n++];
}

void skelscan_rotate_reset(void)
{
    g_rot_cache_n = 0;
}

static int g_rot_reused = 0;   /* nombre de fois où la matrice n'avait pas été réécrite par le jeu */
int skelscan_rotate_reused(void)
{
    return g_rot_reused;
}

/* Tourne la matrice locale de l'os d'index bone de angle radians autour de son axe Z local */
int skelscan_rotate_local(void *skeleton, int bone, float angle)
{
    int lod_i, done = 0;
    float c = cosf(angle), s = sinf(angle);
    for (lod_i = 0; lod_i < 4; ++lod_i) {
        uint8_t *arr;
        int count = lod_nodes(skeleton, lod_i, &arr);
        Mat4 l, rz, out;
        uint8_t *n;
        if (bone < 0 || bone >= count)
            continue;
        n = arr + (size_t)bone * NODE_SIZE;
        if (!safe_read(n + NODE_LOCAL, l.m, sizeof(l.m)) || l.m[15] != 1.0f)
            continue;
        {
            RotCache *rc = rot_cache_get(n);
            if (rc) {
                if (rc->written.m[15] == 1.0f && !memcmp(rc->written.m, l.m, sizeof(l.m))) {
                    l = rc->base;   /* le jeu n'a pas réécrit : on repart de la base */
                    ++g_rot_reused;
                } else {
                    rc->base = l;
                }
            }
        }
        memset(&rz, 0, sizeof(rz));
        rz.m[0] = c; rz.m[1] = -s;
        rz.m[4] = s; rz.m[5] = c;
        rz.m[10] = 1.0f; rz.m[15] = 1.0f;
        mat_mul(&l, &rz, &out);
        if (safe_write(n + NODE_LOCAL, out.m, sizeof(out.m))) {
            RotCache *rc = rot_cache_get(n);
            if (rc)
                rc->written = out;
            ++done;
        }
    }
    return done;
}

int skelscan_model_pos(void *skeleton, float out[3])
{
    float w[12];
    if (!safe_read((const uint8_t *)skeleton + OFF_WORLD, w, sizeof(w)))
        return 0;
    out[0] = w[3];
    out[1] = w[7];
    out[2] = w[11];
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

int skelscan_node_pos(void *skeleton, const char *node, float out[3])
{
    uint8_t *arr = NULL;
    int n = lod_nodes(skeleton, 0, &arr), k;
    char name[64];
    for (k = 0; k < n; ++k) {
        float w[12];
        read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
        if (strcmp(name, node))
            continue;
        if (!safe_read(arr + (size_t)k * NODE_SIZE + NODE_WORLD, w, sizeof(w)))
            return 0;
        out[0] = w[3];
        out[1] = w[7];
        out[2] = w[11];
        return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
    }
    return 0;
}

int skelscan_node_world(void *skeleton, const char *node, float out[12])
{
    uint8_t *arr = NULL;
    int n = lod_nodes(skeleton, 0, &arr), k, i;
    char name[64];
    for (k = 0; k < n; ++k) {
        read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
        if (strcmp(name, node))
            continue;
        if (!safe_read(arr + (size_t)k * NODE_SIZE + NODE_WORLD, out, 12 * sizeof(float)))
            return 0;
        for (i = 0; i < 12; ++i)
            if (!isfinite(out[i]))
                return 0;
        return 1;
    }
    return 0;
}

int skelscan_model_world(void *skeleton, float out[12])
{
    int i;
    if (!safe_read((const uint8_t *)skeleton + OFF_WORLD, out, 12 * sizeof(float)))
        return 0;
    for (i = 0; i < 12; ++i)
        if (!isfinite(out[i]))
            return 0;
    return 1;
}

/* ---------- modèles de la moto (diagnostic, puis guidon) ---------- */

static int model_world_pos(const void *skeleton, float out[3])
{
    float m[12];
    if (!safe_read((const uint8_t *)skeleton + OFF_WORLD, m, sizeof(m)))
        return 0;
    out[0] = m[3];
    out[1] = m[7];
    out[2] = m[11];
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

int skelscan_kind_near(int kind, const float center[3], float radius)
{
    int i;
    for (i = 0; i < g_num_skels; ++i) {
        float p[3];
        if (g_skels[i].kind != kind || !model_world_pos(g_skels[i].ptr, p))
            continue;
        if ((p[0] - center[0]) * (p[0] - center[0]) + (p[2] - center[2]) * (p[2] - center[2]) < radius * radius)
            return 1;
    }
    return 0;
}

void skelscan_log_riders(const float center[3], ScanLogFn log)
{
    int i, n = 0;
    if (!log)
        return;
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3], d = -1.0f;
        if (!e->is_rider || !e->name[0])
            continue;
        if (model_world_pos(e->ptr, p))
            d = sqrtf((p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
                      (p[2] - center[2]) * (p[2] - center[2]));
        if (n++ < 12)
            {
            uint8_t *arr = NULL;
            log("  modele du pilote \"%s\" (type %d, %d noeuds) a %.1f m", e->name, e->kind, lod_nodes(e->ptr, 0, &arr), d);
        }
    }
    if (!n)
        log("  aucun modele du pilote vu");
}

void skelscan_log_near(const float center[3], float radius)
{
    int i;
    if (!g_log)
        return;
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3], d;
        uint8_t *arr;
        int n, k;
        if (e->is_rider || !e->name[0] || strstr(e->name, "tracks") || !model_world_pos(e->ptr, p))
            continue;
        d = sqrtf((p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
                  (p[2] - center[2]) * (p[2] - center[2]));
        if (d > radius)
            continue;
        n = lod_nodes(e->ptr, 0, &arr);
        g_log("moto : modele \"%s\" a %.2f m, %d noeud(s)", e->name, d, n);
        for (k = 0; k < n && k < 96; ++k) {
            char name[48];
            float w[12];
            read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
            if (!safe_read(arr + (size_t)k * NODE_SIZE + NODE_WORLD, w, sizeof(w)))
                continue;
            g_log("    %d %s : (%.3f %.3f %.3f)", k, name[0] ? name : "?", w[3], w[7], w[11]);
        }
    }
}

int skelscan_find_node(const float center[3], float radius, const char *word, float out[3])
{
    int i;
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3];
        uint8_t *arr;
        int n, k;
        if (e->is_rider || !e->name[0] || strstr(e->name, "tracks") || !model_world_pos(e->ptr, p))
            continue;
        if ((p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
                (p[2] - center[2]) * (p[2] - center[2]) > radius * radius)
            continue;
        n = lod_nodes(e->ptr, 0, &arr);
        for (k = 0; k < n; ++k) {
            char name[48], low[48];
            size_t c;
            float w[12];
            read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
            for (c = 0; name[c] && c + 1 < sizeof(low); ++c)
                low[c] = (char)tolower((unsigned char)name[c]);
            low[c] = '\0';
            if (!strstr(low, word) || !safe_read(arr + (size_t)k * NODE_SIZE + NODE_WORLD, w, sizeof(w)))
                continue;
            out[0] = w[3];
            out[1] = w[7];
            out[2] = w[11];
            return 1;
        }
    }
    return 0;
}

/* Toutes les positions des nœuds dont le nom contient word (modèles proches) */
int skelscan_find_nodes(const float center[3], float radius, const char *word, float out[][3], int max)
{
    int i, found = 0;
    for (i = 0; i < g_num_skels && found < max; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3];
        uint8_t *arr;
        int n, k;
        if (e->is_rider || !e->name[0] || strstr(e->name, "tracks") || !model_world_pos(e->ptr, p))
            continue;
        if ((p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
                (p[2] - center[2]) * (p[2] - center[2]) > radius * radius)
            continue;
        n = lod_nodes(e->ptr, 0, &arr);
        for (k = 0; k < n && found < max; ++k) {
            char name[48], low[48];
            size_t c;
            float w[12];
            read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
            for (c = 0; name[c] && c + 1 < sizeof(low); ++c)
                low[c] = (char)tolower((unsigned char)name[c]);
            low[c] = '\0';
            if (!strstr(low, word) || !safe_read(arr + (size_t)k * NODE_SIZE + NODE_WORLD, w, sizeof(w)))
                continue;
            out[found][0] = w[3];
            out[found][1] = w[7];
            out[found][2] = w[11];
            ++found;
        }
    }
    return found;
}

/* Journal : tous les nœuds du modèle qui contient un nœud nommé word (ex. le guidon) */
void skelscan_log_model_nodes(const float center[3], float radius, const char *word, ScanLogFn log)
{
    int i;
    if (!log)
        return;
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3];
        uint8_t *arr;
        int n, k, has = 0;
        if (e->is_rider || !e->name[0] || strstr(e->name, "tracks") || !model_world_pos(e->ptr, p))
            continue;
        if ((p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
                (p[2] - center[2]) * (p[2] - center[2]) > radius * radius)
            continue;
        n = lod_nodes(e->ptr, 0, &arr);
        for (k = 0; k < n && !has; ++k) {
            char name[48];
            read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
            has = strstr(name, word) != NULL;
        }
        if (!has)
            continue;
        log("modele %s : %d noeuds", e->name, n);
        for (k = 0; k < n; ++k) {
            char name[48];
            float w[12];
            read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
            if (safe_read(arr + (size_t)k * NODE_SIZE + NODE_WORLD, w, sizeof(w)))
                log("  noeud %-24s (%.3f %.3f %.3f)", name, w[3], w[7], w[11]);
        }
    }
}

/* Journal : tous les modèles proches qui ont un os de poignet (autre squelette du pilote ?),
   avec la position de leur poignet gauche, pour chaque LOD */
void skelscan_log_wrists(const float center[3], float radius, ScanLogFn log)
{
    int i;
    if (!log)
        return;
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3];
        int lod;
        if (!e->name[0] || !model_world_pos(e->ptr, p))
            continue;
        if ((p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
                (p[2] - center[2]) * (p[2] - center[2]) > radius * radius)
            continue;
        for (lod = 0; lod < 4; ++lod) {
            uint8_t *arr;
            int n = lod_nodes(e->ptr, lod, &arr), k;
            for (k = 0; k < n; ++k) {
                char name[48];
                float w[12], s2[12];
                read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
                if (!strstr(name, "LeftWrist") || !safe_read(arr + (size_t)k * NODE_SIZE + NODE_WORLD, w, sizeof(w)))
                    continue;
                if (!safe_read(arr + (size_t)k * NODE_SIZE + 0x140, s2, sizeof(s2)))
                    memset(s2, 0, sizeof(s2));
                log("poignet : %s (maj %llu) LOD %d os %d : monde (%.2f %.2f %.2f), 2e matrice t (%.2f %.2f %.2f)", e->name,
                    (unsigned long long)e->updates, lod, k, w[3], w[7], w[11], s2[3], s2[7], s2[11]);
            }
        }
    }
}

/* Adresse du nœud node_word (LOD 0) du modèle dont le nom contient model_word, près de center */
uint8_t *skelscan_node_addr(const float center[3], float radius, const char *model_word, const char *node_word)
{
    int i;
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3];
        uint8_t *arr;
        int n, k;
        if (!e->name[0] || !strstr(e->name, model_word) || !model_world_pos(e->ptr, p))
            continue;
        if ((p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
                (p[2] - center[2]) * (p[2] - center[2]) > radius * radius)
            continue;
        n = lod_nodes(e->ptr, 0, &arr);
        for (k = 0; k < n; ++k) {
            char name[48];
            read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
            if (!strcmp(name, node_word))
                return arr + (size_t)k * NODE_SIZE;
        }
    }
    return NULL;
}

void skelscan_log_model_all(const float center[3], float radius, const char *model_word, ScanLogFn log)
{
    int i;
    for (i = 0; log && i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3];
        uint8_t *arr;
        int n, k;
        if (!e->name[0] || !strstr(e->name, model_word) || !model_world_pos(e->ptr, p))
            continue;
        if ((p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
                (p[2] - center[2]) * (p[2] - center[2]) > radius * radius)
            continue;
        n = lod_nodes(e->ptr, 0, &arr);
        log("modele %s : %d noeuds, modele a (%.2f %.2f %.2f)", e->name, n, p[0], p[1], p[2]);
        for (k = 0; k < n; ++k) {
            char name[48];
            float w[12];
            read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
            if (safe_read(arr + (size_t)k * NODE_SIZE + NODE_WORLD, w, sizeof(w)))
                log("  %2d %-28s (%.2f %.2f %.2f)", k, name, w[3], w[7], w[11]);
        }
    }
}

/* Cherche, dans tous les nœuds (tous LOD) et la structure des modèles proches, des triplets de
   floats proches d'un des points donnés (là où le jeu dessine les mains) */
void skelscan_find_points(const float center[3], float radius, const float pts[][3], int npts, float tol, ScanLogFn log)
{
    int i, hits = 0;
    if (!log)
        return;
    for (i = 0; i < g_num_skels && hits < 60; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3];
        int lod;
        if (!e->name[0] || !strstr(e->name, "rider") || !model_world_pos(e->ptr, p))
            continue; /* seulement le pilote et son équipement (pas la moto, qui a les leviers) */
        if ((p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
                (p[2] - center[2]) * (p[2] - center[2]) > radius * radius)
            continue;
        for (lod = -1; lod < 4 && hits < 60; ++lod) {
            uint8_t *arr = NULL;
            int n, k;
            if (lod < 0) {
                n = 1;
                arr = (uint8_t *)e->ptr;
            } else {
                n = lod_nodes(e->ptr, lod, &arr);
            }
            for (k = 0; k < n && hits < 60; ++k) {
                static float buf[0x400 / 4];
                size_t sz = lod < 0 ? 0x400 : NODE_SIZE, o;
                char name[48] = "(modele)";
                if (!safe_read(arr + (size_t)k * (lod < 0 ? 0 : NODE_SIZE), buf, sz))
                    continue;
                if (lod >= 0)
                    read_name(arr + (size_t)k * NODE_SIZE, name, sizeof(name));
                for (o = 0; o + 3 <= sz / 4 && hits < 60; ++o) {
                    int q;
                    for (q = 0; q < npts; ++q) {
                        /* x, y, z contigus, ou colonne de matrice 3x4 par lignes (pas de 4) */
                        int contig = fabsf(buf[o] - pts[q][0]) < tol && o + 2 < sz / 4 &&
                                     fabsf(buf[o + 1] - pts[q][1]) < tol && fabsf(buf[o + 2] - pts[q][2]) < tol;
                        int col = o + 8 < sz / 4 && fabsf(buf[o] - pts[q][0]) < tol &&
                                  fabsf(buf[o + 4] - pts[q][1]) < tol && fabsf(buf[o + 8] - pts[q][2]) < tol;
                        if (contig || col) {
                            ++hits;
                            log("main : %s LOD %d os %d %s +0x%03X (%s) : %.3f %.3f %.3f ~ point %d", e->name, lod, k,
                                name, (unsigned)(o * 4), col ? "colonne" : "suite", buf[o], col ? buf[o + 4] : buf[o + 1],
                                col ? buf[o + 8] : buf[o + 2], q);
                        }
                    }
                }
            }
        }
    }
    log("main : %d resultat(s) pres des poignees", hits);
}

/* v0.40.57 : modèle (hors pilote et piste) dont le chemin finit par suffix, le plus proche de
   center à moins de radius : sa matrice monde (3x4 par lignes) et son chemin de fichier */
/* v1.8.4 : modèles de motos (chemin avec « bikes ») à moins de radius de center */
int skelscan_each_bike_model(const float center[3], float radius, SkelBikeFn fn, void *user)
{
    int i, n = 0;
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3], d, m[12];
        char low[NAME_BYTES];
        size_t c;
        if (e->is_rider || !e->name[0])
            continue;
        for (c = 0; e->name[c] && c + 1 < sizeof(low); ++c)
            low[c] = e->name[c] == '/' ? '\\' : (char)tolower((unsigned char)e->name[c]);
        low[c] = '\0';
        if (!strstr(low, "bikes\\") || !model_world_pos(e->ptr, p))
            continue;
        d = (p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
            (p[2] - center[2]) * (p[2] - center[2]);
        if (d > radius * radius || !skelscan_model_world(e->ptr, m))
            continue;
        fn(low, m, user);
        ++n;
    }
    return n;
}

/* v1.8.19 : chaque modèle (pas un pilote) dont le chemin contient sub, à moins de radius */
int skelscan_each_model_sub(const float center[3], float radius, const char *sub, SkelBikeFn fn, void *user)
{
    int i, n = 0;
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3], d, m[12];
        char low[NAME_BYTES];
        size_t c;
        if (e->is_rider || !e->name[0])
            continue;
        for (c = 0; e->name[c] && c + 1 < sizeof(low); ++c)
            low[c] = e->name[c] == '/' ? '\\' : (char)tolower((unsigned char)e->name[c]);
        low[c] = '\0';
        if (!strstr(low, sub) || !model_world_pos(e->ptr, p))
            continue;
        d = (p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
            (p[2] - center[2]) * (p[2] - center[2]);
        if (d > radius * radius || !skelscan_model_world(e->ptr, m))
            continue;
        fn(low, m, user);
        ++n;
    }
    return n;
}

/* v1.7.33 : modèle (pas un pilote) dont le chemin contient sub (minuscules) ; world peut être NULL */
int skelscan_find_model_sub(const float center[3], float radius, const char *sub, float world[12], char *path,
                            size_t path_size)
{
    int i, best = -1;
    float bd = radius * radius;
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3], d;
        char low[NAME_BYTES];
        size_t c;
        if (e->is_rider || !e->name[0] || !model_world_pos(e->ptr, p))
            continue;
        for (c = 0; e->name[c] && c + 1 < sizeof(low); ++c)
            low[c] = (char)tolower((unsigned char)e->name[c]);
        low[c] = '\0';
        if (!strstr(low, sub))
            continue;
        d = (p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
            (p[2] - center[2]) * (p[2] - center[2]);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (best < 0 || (world && !skelscan_model_world(g_skels[best].ptr, world)))
        return 0;
    if (path && path_size) {
        strncpy(path, g_skels[best].name, path_size - 1);
        path[path_size - 1] = '\0';
    }
    return 1;
}

int skelscan_find_model(const float center[3], float radius, const char *suffix, float world[12], char *path,
                        size_t path_size)
{
    int i, best = -1;
    float bd = radius * radius;
    for (i = 0; i < g_num_skels; ++i) {
        const SkelEntry *e = &g_skels[i];
        float p[3], d;
        char low[NAME_BYTES];
        size_t c;
        if (e->is_rider || !e->name[0] || strstr(e->name, "tracks") || !model_world_pos(e->ptr, p))
            continue;
        for (c = 0; e->name[c] && c + 1 < sizeof(low); ++c)
            low[c] = (char)tolower((unsigned char)e->name[c]);
        low[c] = '\0';
        if (!ends_with(low, suffix))
            continue;
        d = (p[0] - center[0]) * (p[0] - center[0]) + (p[1] - center[1]) * (p[1] - center[1]) +
            (p[2] - center[2]) * (p[2] - center[2]);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (best < 0 || !skelscan_model_world(g_skels[best].ptr, world))
        return 0;
    if (path && path_size) {
        strncpy(path, g_skels[best].name, path_size - 1);
        path[path_size - 1] = '\0';
    }
    return 1;
}

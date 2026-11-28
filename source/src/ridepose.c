/*
 * ridepose.c - v1.7.2 : place du pilote sur sa moto, apprise quand le jeu le fait rouler
 * (voir ridepose.h). Fichier : MXRP_Ragdoll_pose_moto.ini (une ligne par moto).
 */
#include "ridepose.h"
#include "skelscan.h"
#include "uipatch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define NPT 6 /* 0 main gauche, 1 main droite, 2 bassin, 3 cheville gauche, 4 cheville droite, 5 milieu des épaules (v1.7.8) */
#define BUF 240
#define MIN_SAMPLES 40
#define MAX_BIKES 32

typedef struct {
    char dir[200];
    float p[NPT][3]; /* mains : repère du guidon ; bassin et chevilles : repère du cadre (v1.7.2) */
    int n;           /* échantillons qui ont servi */
    int sh_ok;       /* v1.7.8.1 : épaules apprises (pas dans les fichiers « 2| ») */
    float refh;      /* v1.7.3 : point de référence de la moto (télémétrie) au-dessus du sol, 0 = inconnu */
    int v;           /* v1.7.28 : 4 = cadre cherché seulement dans le dossier de la moto */
} BikePose;

static BikePose g_bikes[MAX_BIKES];
static int g_nbikes = 0, g_dirty = 0;
static char g_file[700] = "";
static RidePoseLogFn g_log = NULL;

/* échantillons de la moto en cours */
static char g_cur[200] = "";
static float g_buf[BUF][NPT][3];
static int g_nbuf = 0, g_since = 0;
static double g_last = 0.0;
/* diagnostic : appels, loin de la moto, pas un pilote, sans guidon, gardés */
static unsigned g_cnt[5];

static double now_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    return 0.0;
#endif
}

static BikePose *find_bike(const char *dir, int create)
{
    int i;
    for (i = 0; i < g_nbikes; ++i)
        if (!strcmp(g_bikes[i].dir, dir))
            return &g_bikes[i];
    if (!create)
        return NULL;
    if (g_nbikes < MAX_BIKES)
        i = g_nbikes++;
    else
        i = MAX_BIKES - 1;
    memset(&g_bikes[i], 0, sizeof(g_bikes[i]));
    snprintf(g_bikes[i].dir, sizeof(g_bikes[i].dir), "%s", dir);
    return &g_bikes[i];
}

void ridepose_init(const char *save_dir, RidePoseLogFn log)
{
    FILE *f;
    char line[1024];
    size_t l;
    g_log = log;
    if (!save_dir)
        return;
    l = strlen(save_dir);
    snprintf(g_file, sizeof(g_file), "%s%sMXRP_Ragdoll_pose_moto.ini", save_dir,
             (l && save_dir[l - 1] != '\\' && save_dir[l - 1] != '/') ? "\\" : "");
    f = fopen(g_file, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *bar = strchr(line, '|');
        BikePose bp;
        int k, got;
        /* v1.7.8 : « 3| » (épaules en plus : on réapprend) */
        char *q;
        int ver = line[0] - '0', npts;
        if ((ver != 2 && ver != 3 && ver != 4) || line[1] != '|')
            continue;
        npts = ver >= 3 ? NPT : 5; /* « 2| » : pose de la v1.7.2-1.7.7 gardée, sans les épaules */
        bar = strchr(line + 2, '|');
        if (!bar)
            continue;
        *bar = 0;
        memset(&bp, 0, sizeof(bp));
        snprintf(bp.dir, sizeof(bp.dir), "%s", line + 2);
        q = bar + 1;
        bp.n = (int)strtol(q, &q, 10);
        got = 1;
        for (k = 0; k < npts * 3 && got; ++k) {
            char *e;
            float v = strtof(q, &e);
            if (e == q)
                got = 0;
            bp.p[k / 3][k % 3] = v;
            q = e;
        }
        if (!got || bp.n < 0)
            continue;
        bp.sh_ok = ver >= 3;
        bp.v = ver;
        for (k = 0; k < npts; ++k)
            if (!isfinite(bp.p[k][0]) || !isfinite(bp.p[k][1]) || !isfinite(bp.p[k][2]))
                got = 0;
        if (got)
            *find_bike(bp.dir, 1) = bp;
    }
    fclose(f);
    if (g_log && g_nbikes)
        g_log("pose sur la moto : %d moto(s) deja apprise(s) (%s)", g_nbikes, g_file);
}

void ridepose_save(void)
{
    FILE *f;
    int i;
    if (!g_dirty || !g_file[0])
        return;
    f = fopen(g_file, "w");
    if (!f)
        return;
    fprintf(f, "# MXRP Ragdoll : place du pilote sur chaque moto, apprise quand il roule (fichier refait tout seul)\n");
    fprintf(f, "# 4|moto|echantillons main_g(xyz) main_d (repere du guidon) bassin cheville_g cheville_d epaules (repere du cadre)\n");
    for (i = 0; i < g_nbikes; ++i) {
        const BikePose *b = &g_bikes[i];
        int k;
        if (b->n <= 0)
            continue;
        fprintf(f, "%d|%s|%d", b->sh_ok ? (b->v >= 4 ? 4 : 3) : 2, b->dir, b->n);
        for (k = 0; k < (b->sh_ok ? NPT : 5) * 3; ++k)
            fprintf(f, " %.4f", b->p[k / 3][k % 3]);
        fprintf(f, "\n");
    }
    fclose(f);
    g_dirty = 0;
}

/* M : 3x4 en lignes (monde = R * local + t) */
static int to_local(const float M[12], const float w[3], float out[3])
{
    float a = M[0], b = M[1], c = M[2], d = M[4], e = M[5], f = M[6], g = M[8], h = M[9], i = M[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    float x = w[0] - M[3], y = w[1] - M[7], z = w[2] - M[11];
    if (fabsf(det) < 1e-6f)
        return 0;
    out[0] = ((e * i - f * h) * x - (b * i - c * h) * y + (b * f - c * e) * z) / det;
    out[1] = (-(d * i - f * g) * x + (a * i - c * g) * y - (a * f - c * d) * z) / det;
    out[2] = ((d * h - e * g) * x - (a * h - b * g) * y + (a * e - b * d) * z) / det;
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

static void to_world(const float M[12], const float l[3], float out[3])
{
    int r;
    for (r = 0; r < 3; ++r)
        out[r] = M[r * 4 + 0] * l[0] + M[r * 4 + 1] * l[1] + M[r * 4 + 2] * l[2] + M[r * 4 + 3];
}

static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float median(float *v, int n)
{
    qsort(v, (size_t)n, sizeof(float), cmpf);
    return n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/* modèle du cadre de la moto : nom lu dans le gfx.cfg (chassis { model { file = ... } }) */
static char g_cdir[200] = "", g_cfile[64] = "", g_sfile[64] = "steer.hrc"; /* v1.7.33 : guidon aussi */
static const char *chassis_file(const char *dir)
{
    char path[260], ln[256], blk[3][32] = { "", "", "" };
    FILE *f;
    int depth = 0;
    if (!strcmp(dir, g_cdir))
        return g_cfile;
    snprintf(g_cdir, sizeof(g_cdir), "%s", dir);
    snprintf(g_cfile, sizeof(g_cfile), "chassis.hrc");
    snprintf(g_sfile, sizeof(g_sfile), "steer.hrc");
    snprintf(path, sizeof(path), "%s\\gfx.cfg", dir);
    f = fopen(path, "r");
    if (!f) {
        /* v1.7.31 : moto dans un .pkz : son gfx.cfg est dans l'archive */
        char tp[600];
        if (uipatch_pkz_to_temp(dir, "gfx.cfg", tp, sizeof(tp)))
            f = fopen(tp, "r");
    }
    if (!f)
        return g_cfile;
    while (fgets(ln, sizeof(ln), f)) {
        char w[64] = "", v[64] = "";
        if (strchr(ln, '{')) {
            ++depth;
            continue;
        }
        if (strchr(ln, '}')) {
            if (depth > 0)
                --depth;
            continue;
        }
        if (sscanf(ln, " %63s = %63s", w, v) == 2) {
            if (depth == 2 && (!strcmp(blk[0], "chassis") || !strcmp(blk[0], "steer")) && !strcmp(blk[1], "model") &&
                !strcmp(w, "file")) {
                char *c;
                for (c = v; *c; ++c)
                    if (*c >= 'A' && *c <= 'Z')
                        *c = (char)(*c - 'A' + 'a');
                if (!strcmp(blk[0], "chassis"))
                    snprintf(g_cfile, sizeof(g_cfile), "%s", v);
                else
                    snprintf(g_sfile, sizeof(g_sfile), "%s", v);
            }
        } else if (sscanf(ln, " %31s", w) == 1 && depth < 3) {
            snprintf(blk[depth], sizeof(blk[depth]), "%s", w); /* nom du bloc qui suit */
        }
    }
    fclose(f);
    return g_cfile;
}

/* v1.7.28 : modèle du cadre de CETTE moto. Certaines motos (mods) ont un cadre « model.edf » :
   chercher juste « model.edf » trouvait aussi les pneus (tyres\\...\\model.edf) ou une autre moto,
   et le pilote se mettait à côté. On cherche « <dossier de la moto>\\<fichier> ». */
static int chassis_frame_of(const float center[3], const char *dir, float Mc[12])
{
    char suf[300], *c;
    const char *file = chassis_file(dir), *name = strrchr(dir, '\\'), *n2 = strrchr(dir, '/');
    if (n2 > name)
        name = n2;
    name = name ? name + 1 : dir;
    snprintf(suf, sizeof(suf), "%s\\%s", name, file);
    for (c = suf; *c; ++c)
        *c = (char)tolower((unsigned char)*c);
    if (skelscan_find_model(center, 2.5f, suf, Mc, NULL, 0))
        return 1;
    snprintf(suf, sizeof(suf), "%s/%s", name, file);
    for (c = suf; *c; ++c)
        *c = (char)tolower((unsigned char)*c);
    if (skelscan_find_model(center, 2.5f, suf, Mc, NULL, 0))
        return 1;
    /* nom propre au cadre (chassis.hrc…) : pas de risque de confusion avec les pneus */
    return strstr(file, "chassis") && skelscan_find_model(center, 2.5f, file, Mc, NULL, 0);
}

/* v1.7.32 : en ligne, le guidon le plus proche était parfois celui d'un autre joueur (vu : une
   Suzuki à côté de la Fantic du joueur). On cherche d'abord le guidon de SA moto (dossier = ID). */
static char g_own_bike[128] = "";
void ridepose_set_bike(const char *id)
{
    char *c;
    snprintf(g_own_bike, sizeof(g_own_bike), "%s", id ? id : "");
    for (c = g_own_bike; *c; ++c)
        *c = (char)tolower((unsigned char)*c);
}
int ridepose_find_steer(const float center[3], float M[12], char *path, size_t pl)
{
    const char *files[3] = { "steer.hrc", "steer.edf", NULL };
    int i, k;
    if (g_own_bike[0]) {
        /* v1.7.33 : nom du modèle du guidon lu dans le gfx.cfg de SA moto (dossier, sinon .pkz) */
        char any[700], sub[140];
        snprintf(sub, sizeof(sub), "%s", g_own_bike);
        if (skelscan_find_model_sub(center, 2.5f, sub, NULL, any, sizeof(any))) {
            char *e = strstr(any, sub);
            if (!e) {
                char low[700], *c;
                snprintf(low, sizeof(low), "%s", any);
                for (c = low; *c; ++c)
                    *c = (char)tolower((unsigned char)*c);
                e = strstr(low, sub);
                if (e)
                    e = any + (e - low);
            }
            if (e) {
                e[strlen(sub)] = 0;
                files[2] = chassis_file(any) ? g_sfile : NULL; /* lit le gfx.cfg (cadre et guidon) */
            }
        }
        for (i = 2; i >= 0; --i) {
            if (!files[i])
                continue;
            for (k = 0; k < 2; ++k) {
                char suf[200];
                snprintf(suf, sizeof(suf), "%s%c%s", g_own_bike, k ? '/' : '\\', files[i]);
                if (skelscan_find_model(center, 2.5f, suf, M, path, pl))
                    return 1;
            }
        }
    }
    return skelscan_find_model(center, 2.5f, "steer.hrc", M, path, pl) ||
           skelscan_find_model(center, 2.5f, "steer.edf", M, path, pl);
}

static int steer_frame(const float center[3], float M[12], char *dir, size_t dl)
{
    char path[700], *s1, *s2;
    if (!ridepose_find_steer(center, M, path, sizeof(path)))
        return 0;
    s1 = strrchr(path, '\\');
    s2 = strrchr(path, '/');
    if (s2 > s1)
        s1 = s2;
    if (s1)
        *s1 = 0;
    snprintf(dir, dl, "%s", path);
    return 1;
}

/* estimation à partir des échantillons : mains = médiane ; bassin et chevilles = médiane des
   moments où il est assis (le tiers le plus bas du bassin : debout sur les repose-pieds en roulant,
   le bassin est plus haut) */
static void learn(void)
{
    static float v[BUF];
    float ys[BUF], cut;
    int k, c, i, n, sit[BUF], ns = 0;
    BikePose *b;
    for (i = 0; i < g_nbuf; ++i)
        ys[i] = g_buf[i][2][1];
    memcpy(v, ys, sizeof(float) * (size_t)g_nbuf);
    qsort(v, (size_t)g_nbuf, sizeof(float), cmpf);
    cut = v[g_nbuf / 3];
    for (i = 0; i < g_nbuf; ++i)
        if (ys[i] <= cut + 0.01f)
            sit[ns++] = i;
    b = find_bike(g_cur, 1);
    for (k = 0; k < NPT; ++k)
        for (c = 0; c < 3; ++c) {
            n = 0;
            if (k < 2)
                for (i = 0; i < g_nbuf; ++i)
                    v[n++] = g_buf[i][k][c];
            else
                for (i = 0; i < ns; ++i)
                    v[n++] = g_buf[sit[i]][k][c];
            b->p[k][c] = median(v, n);
        }
    b->n = g_nbuf;
    b->sh_ok = 1;
    b->v = 4; /* v1.7.28 */
    g_dirty = 1;
    if (g_log)
        g_log("pose sur la moto apprise (%s, %d moments) : mains %.2f %.2f %.2f / %.2f %.2f %.2f, bassin %.2f %.2f %.2f, "
              "chevilles %.2f %.2f %.2f / %.2f %.2f %.2f",
              g_cur, g_nbuf, b->p[0][0], b->p[0][1], b->p[0][2], b->p[1][0], b->p[1][1], b->p[1][2], b->p[2][0],
              b->p[2][1], b->p[2][2], b->p[3][0], b->p[3][1], b->p[3][2], b->p[4][0], b->p[4][1], b->p[4][2]);
}

void ridepose_sample(void *skeleton, const float bike_pos[3], void *own_skel)
{
    static const char *const nodes[] = { "riderRIG_LeftWrist", "riderRIG_RightWrist", "riderRIG_Pelvis",
                                         "riderRIG_LeftAnkle", "riderRIG_RightAnkle" };
    float mp[3], M[12], Mc[12], w[NPT][3], mid[2][3], loc[NPT][3];
    char dir[200];
    double t = now_s();
    int k;
    if (t - g_last < 0.1 || !skeleton || (own_skel && skeleton != own_skel))
        return;
    ++g_cnt[0];
    if (!skelscan_model_pos(skeleton, mp))
        return;
    if ((mp[0] - bike_pos[0]) * (mp[0] - bike_pos[0]) + (mp[1] - bike_pos[1]) * (mp[1] - bike_pos[1]) +
            (mp[2] - bike_pos[2]) * (mp[2] - bike_pos[2]) > 1.6f * 1.6f) {
        ++g_cnt[1];
        return;
    }
    for (k = 0; k < 5; ++k)
        if (!skelscan_node_pos(skeleton, nodes[k], w[k])) {
            ++g_cnt[2];
            return; /* pas un pilote */
        }
    {
        float sl[3], sr[3];
        if (!skelscan_node_pos(skeleton, "riderRIG_LeftShoulder", sl) ||
            !skelscan_node_pos(skeleton, "riderRIG_RightShoulder", sr)) {
            ++g_cnt[2];
            return;
        }
        for (k = 0; k < 3; ++k)
            w[5][k] = 0.5f * (sl[k] + sr[k]);
    }
    if (!skelscan_node_pos(skeleton, "riderRIG_LeftMiddle", mid[0]) ||
        !skelscan_node_pos(skeleton, "riderRIG_RightMiddle", mid[1])) {
        /* pilote sans os des doigts : on garde le poignet */
        int c;
        for (k = 0; k < 2; ++k)
            for (c = 0; c < 3; ++c)
                mid[k][c] = w[k][c];
    }
    if (!steer_frame(bike_pos, M, dir, sizeof(dir))) {
        ++g_cnt[3];
        return;
    }
    if (!chassis_frame_of(bike_pos, dir, Mc)) {
        ++g_cnt[3];
        return;
    }
    {
        /* v1.7.2 : modèles à jour (ils suivent le pilote) : un modèle pas redessiné (vue
           casque ?) resterait derrière la moto qui roule */
        static float pd[2] = { -1.0f, -1.0f };
        float d[2];
        int stale = 0;
        d[0] = sqrtf((M[3] - mp[0]) * (M[3] - mp[0]) + (M[7] - mp[1]) * (M[7] - mp[1]) + (M[11] - mp[2]) * (M[11] - mp[2]));
        d[1] = sqrtf((Mc[3] - mp[0]) * (Mc[3] - mp[0]) + (Mc[7] - mp[1]) * (Mc[7] - mp[1]) + (Mc[11] - mp[2]) * (Mc[11] - mp[2]));
        for (k = 0; k < 2; ++k) {
            if (pd[k] >= 0.0f && fabsf(d[k] - pd[k]) > 0.06f)
                stale = 1;
            pd[k] = d[k];
        }
        if (stale || d[0] > 2.0f || d[1] > 2.0f) {
            g_last = t;
            ++g_cnt[3];
            return;
        }
    }
    g_last = t;
    /* paume : entre le poignet et la base du majeur */
    for (k = 0; k < 2; ++k) {
        int c;
        for (c = 0; c < 3; ++c)
            w[k][c] += 0.5f * (mid[k][c] - w[k][c]);
    }
    for (k = 0; k < NPT; ++k)
        if (!to_local(k < 2 ? M : Mc, w[k], loc[k]))
            return;
    {
        /* garde-fou : tout le pilote à moins de 1,6 m du guidon / du cadre */
        for (k = 0; k < NPT; ++k) {
            float d = sqrtf(loc[k][0] * loc[k][0] + loc[k][1] * loc[k][1] + loc[k][2] * loc[k][2]);
            if (d > 2.2f)
                return;
        }
    }
    if (strcmp(dir, g_cur)) {
        snprintf(g_cur, sizeof(g_cur), "%s", dir);
        g_nbuf = g_since = 0;
    }
    if (g_nbuf < BUF) {
        memcpy(g_buf[g_nbuf++], loc, sizeof(loc));
    } else {
        memmove(g_buf[0], g_buf[1], sizeof(g_buf[0]) * (BUF - 1));
        memcpy(g_buf[BUF - 1], loc, sizeof(loc));
    }
    ++g_since;
    if (++g_cnt[4] == 1 && g_log)
        g_log("pose sur la moto : on regarde le pilote rouler (%s)", g_cur);
    /* première estimation à 40 moments (4 s à rouler droit), puis mise à jour toutes les 30 s */
    if ((g_nbuf == MIN_SAMPLES && g_since == MIN_SAMPLES) || (g_nbuf >= MIN_SAMPLES && g_since >= 300)) {
        g_since = 0;
        learn();
    }
}

void ridepose_report(void)
{
    if (g_log && g_cnt[0])
        g_log("pose sur la moto : %u modeles regardes, %u loin de la moto, %u pas un pilote, %u guidon ou cadre introuvable / pas a jour, %u moments gardes",
              g_cnt[0], g_cnt[1], g_cnt[2], g_cnt[3], g_cnt[4]);
    memset(g_cnt, 0, sizeof(g_cnt));
}

int ridepose_world(const float center[3], float grip[2][3], float pelvis[3], float ankle[2][3])
{
    return ridepose_world2(center, grip, pelvis, ankle, NULL);
}

/* v1.7.26 : la même moto sur un autre PC (autre dossier) : on compare le nom du dossier de la moto */
static const char *last_part(const char *p)
{
    const char *a = strrchr(p, '\\'), *b = strrchr(p, '/');
    if (b > a)
        a = b;
    return a ? a + 1 : p;
}
static const BikePose *find_by_name(const char *dir)
{
    int i;
    const char *n = last_part(dir);
    for (i = 0; i < g_nbikes; ++i)
        if (g_bikes[i].n >= MIN_SAMPLES && !_stricmp(last_part(g_bikes[i].dir), n))
            return &g_bikes[i];
    return NULL;
}
/* v1.7.26 : moto jamais vue rouler sur ce PC : place moyenne mesurée sur des motos OEM 2023
   (KTM 250 SX-F, Yamaha YZ450F). Remplacée dès que le pilote a roulé un peu. */
static const BikePose g_default_pose = {
    "", { { -0.371f, 0.244f, -0.070f }, { 0.371f, 0.244f, -0.070f }, { 0.000f, 1.055f, -0.266f },
          { -0.157f, 0.527f, -0.179f }, { 0.157f, 0.527f, -0.179f }, { 0.000f, 1.411f, -0.118f } },
    MIN_SAMPLES, 1, 0.0f, 4
};

int ridepose_world2(const float center[3], float grip[2][3], float pelvis[3], float ankle[2][3], float shoulder[3])
{
    float M[12], Mc[12];
    char dir[200];
    const BikePose *b;
    static char logged[200] = "";
    if (!steer_frame(center, M, dir, sizeof(dir)))
        return 0;
    b = find_bike(dir, 0);
    if (b && b->v < 4 && !strstr(chassis_file(dir), "chassis") && (pelvis || ankle || shoulder))
        b = NULL; /* v1.7.28 : appris avant avec peut-être le mauvais cadre (pneu…) : à refaire */
    if (!b || b->n < MIN_SAMPLES) {
        b = find_by_name(dir);
        if (!b) {
            if (grip)
                return 0; /* mains : les poignées lues dans le gfx.cfg de la moto valent mieux qu'une moyenne */
            b = &g_default_pose;
        }
        if (g_log && strcmp(logged, dir)) {
            snprintf(logged, sizeof(logged), "%s", dir);
            g_log("pose sur la moto : %s pour %s (pas encore apprise ici)", b == &g_default_pose ? "place moyenne" : "place apprise ailleurs",
                  last_part(dir));
        }
    }
    if ((pelvis || ankle || shoulder) && !chassis_frame_of(center, dir, Mc))
        return 0;
    if (grip) {
        to_world(M, b->p[0], grip[0]);
        to_world(M, b->p[1], grip[1]);
    }
    if (pelvis)
        to_world(Mc, b->p[2], pelvis);
    if (ankle) {
        to_world(Mc, b->p[3], ankle[0]);
        to_world(Mc, b->p[4], ankle[1]);
    }
    if (shoulder) {
        if (!b->sh_ok)
            return 0;
        to_world(Mc, b->p[5], shoulder);
    }
    return 1;
}

/* v1.7.3 : hauteur du point de référence de la moto au-dessus du sol, mesurée sur ses roues */
int ridepose_refh(const float center[3], float *h)
{
    float M[12];
    char dir[200];
    const BikePose *b;
    if (!steer_frame(center, M, dir, sizeof(dir)) || (b = find_bike(dir, 0)) == NULL || b->refh <= 0.0f)
        return 0;
    *h = b->refh;
    return 1;
}

void ridepose_set_refh(const float center[3], float h)
{
    float M[12];
    char dir[200];
    BikePose *b;
    if (h <= 0.0f || h > 1.5f || !steer_frame(center, M, dir, sizeof(dir)))
        return;
    b = find_bike(dir, 1);
    if (fabsf(b->refh - h) > 0.005f) {
        b->refh = h;
        g_dirty = 1;
        ridepose_save();
        if (g_log)
            g_log("moto : point de reference a %.2f m du sol (mesure sur les roues, garde pour cette moto)", h);
    }
}

/* v1.7.27 : relire le gfx.cfg de la moto (nouvelle session, moto peut-être modifiée) */
void ridepose_reload_gfx(void)
{
    g_cdir[0] = 0;
}

/* v1.8.4 : nom du modèle du cadre d'une moto (dossier en minuscules), plusieurs motos gardées */
const char *ridepose_chassis_of(const char *dir)
{
    static struct { char dir[200]; char file[64]; } c[16];
    static int next = 0;
    int i;
    for (i = 0; i < 16; ++i)
        if (c[i].dir[0] && !strcmp(c[i].dir, dir))
            return c[i].file;
    i = next;
    next = (next + 1) % 16;
    snprintf(c[i].dir, sizeof(c[i].dir), "%s", dir);
    snprintf(c[i].file, sizeof(c[i].file), "%s", chassis_file(dir));
    return c[i].file;
}

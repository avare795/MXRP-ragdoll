/*
 * plugin.c - plugin MX Bikes "Ragdoll" (phase 1 : API officielle uniquement)
 *
 * Ce que fait cette version :
 *  - détecte la chute du joueur grâce à m_iCrashed (données de télémétrie)
 *  - lance une simulation de ragdoll physique à partir de la position,
 *    de l'orientation et de la vitesse de la moto au moment de la chute
 *  - affiche le ragdoll simulé dans une fenêtre de debug (vue de côté)
 *  - écrit un journal de chaque chute (MXRP_Ragdoll.log)
 *
 * Phase 2 (diagnostic, désactivée par défaut : skeleton_hook = 1 dans le ini) :
 *  - intercepte la mise à jour du squelette du pilote dans mxbikes.exe (beta21e)
 *  - pendant une chute, cherche les os du pilote en mémoire et note les
 *    résultats dans le journal, pour pouvoir ensuite y écrire notre ragdoll
 *
 * Sans skeleton_hook, le plugin n'utilise que l'API officielle et ne modifie
 * rien dans le jeu.
 */
#include "mxb_api.h"
#include "config.h"
#include "drive.h"
#include "hook.h"
#include "ragdoll.h"
#include "skelscan.h"
#include "walk.h"
#include "terrain.h"
#include "glcam.h"
#include "camhook.h"
#include "replay.h"
#include "net.h"
#include "kbhook.h"
#include "objects.h"
#include "players.h"
#include "cones.h"
#include "profsave.h"
#include "padstyle.h"
#include "padicon.h"
#include "ramps.h"
#include "gamever.h"
#include "groundhook.h"
#include "update.h"
/* v1.8.145 (BETA 3) : outils de diagnostic des rampes (sonde au journal, essai piloté par fichier) : 0 = coupés */
#ifndef MXRP_DIAG
#define MXRP_DIAG 0
#endif
#include "bales.h"
#include "inflate.h"
static void players_refresh(void); /* v1.8.1 */
#include "lang.h"
#include "uiswitch.h"
#include "uipatch.h"
#include "ridepose.h"
#include "rclock.h"
#include "bike.h"
static void bike_nocollide_update(int force_on);
static void bike_release_all(const char *why);
static int dismount_crash_on(void);
static void release_inputs(const char *why);
#include "padinject.h"
#include "equip.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <time.h>
#endif

#define PLUGIN_VERSION "1.8.205"
#define SEAT_FWD 0.12f /* v1.7.10 : fesses plus en avant sur la selle que la pose apprise */
#define HAND_FWD 0.06f
#define SEAT_DOWN 0.07f /* v1.7.11 : assis plus bas que la pose apprise (pas assez assis, vu en jeu) */
#define SIT_LEAN 0.60f /* v1.7.11 : buste moins penché assis */ /* v1.7.10 : mains plus en avant */

/* Moments des scans mémoire après le début d'une chute, en secondes */
static const float k_scan_times[] = { 0.15f, 0.75f, 2.0f };
#define NUM_SCANS ((int)(sizeof(k_scan_times) / sizeof(k_scan_times[0])))

#define INI_PATH  "plugins\\MXRP_Ragdoll.ini"
#define FONT_FILE "MXRP_Ragdoll.fnt"
#define FONT_PATH "plugins\\" FONT_FILE
/* v0.41.30 : ancien nom (mxb_ragdoll) : lu si le nouveau fichier n'est pas là */
#define OLD_INI_PATH  "plugins\\mxb_ragdoll.ini"
#define OLD_FONT_FILE "mxb_ragdoll.fnt"
#define OLD_FONT_PATH "plugins\\" OLD_FONT_FILE

#define MAX_QUADS   448
#define MAX_STRINGS 40

/* ---------- état global ---------- */

static PluginConfig g_cfg;
static Ragdoll g_rd;
static FILE *g_log = NULL;

static int   g_was_crashed = 0;
static int   g_has_time = 0;
static float g_last_time = 0.0f;
static float g_hide_timer = 0.0f;   /* > 0 : fenêtre encore visible après le respawn */
static int   g_rest_logged = 0;
static float g_view_dir[2] = { 0.0f, 1.0f }; /* direction horizontale (x, z) de la vue de côté */
static float g_view_center = 0.0f;

static int   g_next_scan = NUM_SCANS; /* prochain scan à faire pendant la chute */
static float g_last_bike_pos[3];
/* v1.7 : pour apprendre la place du pilote sur la moto quand il roule droit */
static volatile float g_ride_speed = 0.0f, g_ride_steer = 99.0f, g_ride_roll = 99.0f;
static volatile int g_ride_crashed = 1;
static volatile int g_ride_ok = 0; /* v1.7.1 : roule droit maintenant (télémétrie) */
static unsigned g_ride_n[4];      /* diagnostic : télémétrie, en piste, assez vite, droit */
static void *g_own_skel = NULL; /* v1.3.4 : notre pilote (squelette), pour ne pas le prendre pour un autre */
static int   g_hier_dumps = 0;        /* vidages complets de la hiérarchie (limités pour le journal) */

/*
 * Test d'écriture (write_test = 1). Constats : écrire APRÈS la mise à jour
 * ne bouge que l'équipement (v0.3.0) ; écrire AVANT bouge tout le pilote
 * (v0.3.1). On écrit donc les matrices LOCALES des os avant la mise à jour :
 *   phase 1 (0,3 s -> 3 s) : tête et épaules tournées de 90 degrés
 *   phase 2 (3 s -> 6 s)   : bas du dos (Spine1) plié de 90 degrés
 */
#define WRITE_PHASE1_START 0.3f
#define WRITE_PHASE2_START 3.0f
#define WRITE_PHASE2_END   6.0f
static volatile int g_write_phase = 0;   /* 0 = rien, 1 ou 2 */
static int g_write_counts[3];            /* os modifiés à la dernière écriture, par phase */
static int g_write_logged[3];

/*
 * Ragdoll 3D (ragdoll_3d = 1). La télémétrie détecte la chute ; la simulation
 * et l'écriture des os se font dans le rappel AVANT la mise à jour de
 * rider.hrc (fil de rendu du jeu).
 */
enum { DRIVE_IDLE = 0, DRIVE_REQUESTED, DRIVE_ACTIVE, DRIVE_END_REQUESTED };
static int g_mode3d = 0;
static volatile int g_drive_state = DRIVE_IDLE;
static float g_drive_vel[3], g_drive_bike[3], g_drive_ground;
static volatile int g_drive_req_id = 0;
static volatile int g_drive_is_mod = 0; /* le pilote piloté est un mod (rider.edf) */
static float g_drive_wait = 0.0f;       /* temps d'attente du squelette après la chute */
static double g_drive_last = 0.0;
static volatile int g_drive_writes = 0;
static float g_drive_log_timer = 0.0f;

/* Se relever et marcher (walking = 1), uniquement pendant une chute */
static Walker g_walker;
/* v1.8.3 : renversé par une moto : au sol, notre caméra continue de le suivre (en 3e personne) */
static volatile int g_knocked = 0;
static volatile int g_cam_free_look = 0; /* v1.8.12 : 1re personne au sol, vue libre au-dessus de la tête */
static volatile int g_walk_logged = 0;

static void log_line(const char *fmt, ...);
static double now_seconds(void);

/*
 * Relief de la piste (heightfield ODE lu en mémoire). Sa hauteur est recalée
 * sur le corps de chute du jeu au repos (g_terrain_c), et il n'est utilisé
 * qu'une fois ce recalage fait.
 */
static volatile int g_terrain_ok = 0;
static float g_terrain_c = 0.0f;
/* v1.8.148 : rampes données aux roues (surfaces sous les roues du jeu) ; 1 : permis (physique, rampes posées) */
static volatile int g_ramps_phys = 0;
static int ramp_ground_fn(float x, float z, float *h, float n[3]);
static int g_terrain_calib_logged = 0;
/* Cache du relief sur le disque, un fichier par piste (chargement instantané) */
static char g_save_dir[400] = "";
static char g_user_dir[400] = "";
/* v1.8.106 : tous nos fichiers dans Documents\PiBoSo\MX Bikes\MXRP_Ragdoll\ (avec le \ final) */
static char g_rdir[520] = "";
static volatile int g_draw_state = 0; /* 0 = en piste, 1 = spectateur, 2 = rediffusion */
static int g_live_suspended = 0;   /* pilotage en attente pendant une rediffusion */ /* Documents\\PiBoSo\\MX Bikes */
static char g_cache_path[600] = "";
static int g_terrain_from_cache = 0; /* chargé du cache, pas encore vérifié sur une chute */
/* v0.41.17 : relief actif tout de suite sur une piste nouvelle : 1 = hauteur provisoire
   (décalage habituel), 2 = mesurée sur la moto en roulant ; la moto arrêtée affine ensuite */
static int g_terrain_prov = 0;
static int g_event_count = 0;
static float g_cache_save_c = 0.0f;

#ifdef _WIN32
static DWORD WINAPI cache_save_thread(LPVOID arg)
{
    (void)arg;
    if (terrain_save(g_cache_path, g_cache_save_c))
        log_line("relief : sauvegarde pour les prochaines fois (%s)", g_cache_path);
    else
        log_line("relief : sauvegarde impossible (%s)", g_cache_path);
    return 0;
}

static void cache_save_async(float c)
{
    HANDLE t;
    if (!g_cache_path[0])
        return;
    g_cache_save_c = c;
    t = CreateThread(NULL, 0, cache_save_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}
#else
static void cache_save_async(float c) { (void)c; }
#endif

/* Objets de la piste : hauteurs ODE exactes, décalées comme le relief (g_terrain_c) pour que le
   pilote soit posé dessus comme sur le sable (v0.40.44 : il était ~20 cm trop haut sur le ponton) */
static int ground_fn(float x, float z, float *h);
/* v1.8.82 : objets de la piste seuls (sans les bottes de foin) : le sol des bottes */
static int objects_height_raw(float x, float z, float ymax, float *h)
{
    if (!objects_height(x, z, ymax - g_terrain_c, h))
        return 0;
    *h += g_terrain_c;
    return 1;
}

static int objects_height_fn(float x, float z, float ymax, float *h)
{
    static int logged = 0;
    float t, hb;
    int ok = objects_height_raw(x, z, ymax, h);
    /* v1.8.82 : les bottes de foin sont des objets sur lesquels on marche */
    if (bales_height(x, z, ymax, &hb) && (!ok || hb > *h)) {
        *h = hb;
        ok = 1;
    }
    /* v1.8.119 : rampes FMX : on marche sur le tablier */
    if (ramps_height(x, z, ymax, &hb) && (!ok || hb > *h)) {
        *h = hb;
        ok = 1;
    }
    if (!ok)
        return 0;
    if (logged < 3 && ground_fn(x, z, &t) && *h > t + 0.05f) {
        ++logged;
        log_line("objets : pied sur un objet en (%.1f %.1f), %.2f m au-dessus du sol", x, z, *h - t);
    }
    return 1;
}

static int objects_blocked_fn(float x, float z, float feet, float r)
{
    /* v1.8 : motos et pilotes à pied des autres joueurs aussi */
    return objects_blocked(x, z, feet - g_terrain_c, r) || players_blocked(x, z, feet, r) ||
           bales_blocked(x, z, feet, r) /* v1.8.82 */ || ramps_blocked(x, z, feet, r, 0.45f); /* v1.8.125 */
}

static int ground_fn(float x, float z, float *h)
{
    if (!g_terrain_ok || !terrain_height(x, z, h, NULL))
        return 0;
    *h += g_terrain_c;
    return 1;
}

/* Sol du pilote à pied : hauteur ACTUELLE (ornières et bosses creusées par la déformation),
   sinon le relief d'origine (v0.40.51 : les pieds s'enfonçaient ou flottaient dans les ornières) */
static int ground_walk_fn(float x, float z, float *h)
{
    /* v1.8.125 : sans les rampes écrites dans la grille (pour la moto) : à pied, la rampe est un objet
       (on marche sur le tablier, on bute sur les côtés) ; avant, il grimpait sur une pente invisible */
    if (g_terrain_ok && terrain_height_live_base(x, z, h)) {
        *h += g_terrain_c;
        return 1;
    }
    return ground_fn(x, z, h);
}

/* hauteur de référence de la moto (son châssis) : les objets sous elle sont son sol */
static float g_bike_obj_ref = -1e9f;

/* Sol ACTUEL (ornières et bosses de la déformation du terrain comprises) : pour la moto tenue.
   Objets de la piste compris (ponton) sous le châssis de la moto */
static int ground_now_fn(float x, float z, float *h)
{
    float ho;
    int ok;
    if (g_terrain_ok && terrain_height_live(x, z, h)) {
        *h += g_terrain_c;
        ok = 1;
    } else
        ok = ground_fn(x, z, h);
    if (g_bike_obj_ref > -1e8f && objects_height_fn(x, z, g_bike_obj_ref, &ho) && (!ok || ho > *h)) {
        *h = ho;
        ok = 1;
    }
    return ok;
}

/* Sol du ragdoll = plan tangent au relief sous le bassin */
/* Relief sous un point du ragdoll (recalé sur le corps du jeu) ; 0 si inconnu */
static int ragdoll_ground(float x, float z, float *h, float n[3])
{
    float ho;
    float hl;
    if (!g_terrain_ok || !terrain_height(x, z, h, n))
        return 0;
    /* v1.8.10 : hauteur ACTUELLE (bosses et ornières de la déformation) comme pour la marche :
       sur le relief d'origine, des parties du corps passaient sous la terre déplacée */
    if (terrain_height_live(x, z, &hl) && hl > *h)
        *h = hl;
    *h += g_terrain_c;
    /* objets de la piste sous le point (il tombe sur le ponton, pas à travers) */
    if (objects_height_fn(x, z, ragdoll_query_y + 0.3f, &ho) && ho > *h) {
        *h = ho;
        n[0] = 0.0f;
        n[1] = 1.0f;
        n[2] = 0.0f;
    }
    return 1;
}

/* objets de la piste contre le pilote au sol (mêmes hauteurs décalées que le relief) */
static int ragdoll_obstacle(const float p[3], float r, float push[3])
{
    float q[3] = { p[0], p[1] - g_terrain_c, p[2] }, pp[3];
    int hit = g_terrain_ok && objects_push(q, r, push);
    if (!hit)
        push[0] = push[1] = push[2] = 0.0f;
    cones_body_touch(p, r); /* v1.8.45 : le corps renverse les cônes */
    /* v1.8.82 : les bottes de foin repoussent le corps (lourdes : elles ne bougent pas) */
    if (bales_push(p, r, pp)) {
        push[0] += pp[0];
        push[1] += pp[1];
        push[2] += pp[2];
        hit = 1;
    }
    /* v1.8.152 : les côtés et le dos des rampes repoussent le corps (il ne les traverse plus) */
    if (ramps_push(p, r, pp)) {
        push[0] += pp[0];
        push[1] += pp[1];
        push[2] += pp[2];
        hit = 1;
    }
    /* v1.8 : le corps au sol est repoussé par les motos et les pilotes des autres joueurs */
    if (players_push(p, r, pp)) {
        push[0] += pp[0];
        push[1] += pp[1];
        push[2] += pp[2];
        hit = 1;
    }
    return hit;
}

/* v1.8.20 : éléments du pilote contre le sol. Le casque et les bottes sont des modèles 3D à part ;
   leur taille est lue dans l'en-tête de leur .edf (« EDF\0 » puis min x,y,z, max x,y,z, dans le
   repère du modèle) et placée avec la position du modèle dans le jeu. Les points de cette boîte
   qui passent sous le sol remontent le point du ragdoll qui porte l'élément (tête, genou, pied).
   Les membres (segments entre deux points) sont aussi vérifiés en leur milieu. */
typedef struct {
    char path[300];
    int ok;
    float bb[6];
} EdfBox;
/* v1.8.126 : la lecture (fichier, ou recherche dans les .pkz : plus d'une seconde) se fait sur un fil à
   part pendant qu'on roule ; à la chute, seulement le cache (avant : 1,3 s d'image figée à la 1re chute) */
static EdfBox g_edf[12];
static int g_edf_next = 0;
static CRITICAL_SECTION g_edf_cs;
static volatile LONG g_edf_init = 0, g_edf_warm = 0;

static void edf_lock(void)
{
    if (InterlockedCompareExchange(&g_edf_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_edf_cs);
        g_edf_init = 2;
    }
    while (g_edf_init != 2)
        Sleep(0);
    EnterCriticalSection(&g_edf_cs);
}

static int edf_cached(const char *model_path, float bb[6], int *ok)
{
    int i, hit = 0;
    edf_lock();
    for (i = 0; i < 12; ++i)
        if (g_edf[i].path[0] && !strcmp(g_edf[i].path, model_path)) {
            memcpy(bb, g_edf[i].bb, sizeof(g_edf[i].bb));
            *ok = g_edf[i].ok;
            hit = 1;
            break;
        }
    LeaveCriticalSection(&g_edf_cs);
    return hit;
}

static int edf_read(const char *model_path, float bb[6]);

/* à la chute : le cache seulement ; pas encore lu : lecture lancée à côté, boîte par défaut en attendant */
static void edf_warm_start(void);
static int edf_box(const char *model_path, float bb[6])
{
    int ok = 0;
    if (edf_cached(model_path, bb, &ok))
        return ok;
    edf_warm_start();
    return 0;
}

static int edf_read(const char *model_path, float bb[6])
{
    EdfBox e;
    char path[300], dir[300], *dot, *sl;
    unsigned char h[28];
    FILE *f;
    int ok = 0;
    EdfBox *c = &e;
    const int i = 0;
    memset(&e, 0, sizeof(e));
    snprintf(e.path, sizeof(e.path), "%s", model_path);
    snprintf(path, sizeof(path), "%s", model_path);
    dot = strrchr(path, '.');
    if (dot)
        snprintf(dot, sizeof(path) - (size_t)(dot - path), ".edf");
    f = fopen(path, "rb");
    if (f) {
        ok = fread(h, 1, sizeof(h), f) == sizeof(h);
        fclose(f);
    }
    if (!ok) {
        /* élément dans un .pkz */
        snprintf(dir, sizeof(dir), "%s", path);
        sl = strrchr(dir, '\\');
        if (strrchr(dir, '/') > sl)
            sl = strrchr(dir, '/');
        if (sl) {
            *sl = 0;
            ok = uipatch_bike_file_head(dir, sl + 1, h, sizeof(h));
        }
    }
    if (ok && !memcmp(h, "EDF", 4)) {
        float v[6];
        memcpy(v, h + 4, sizeof(v));
        if (v[3] > v[0] && v[4] > v[1] && v[5] > v[2] && v[3] - v[0] < 1.0f && v[4] - v[1] < 1.0f && v[5] - v[2] < 1.0f) {
            memcpy(c[i].bb, v, sizeof(v));
            c[i].ok = 1;
        }
    }
    /* (pas de journal ici : fil à part ; écrit par edf_warm_start) */
    (void)path;
    memcpy(bb, c[i].bb, sizeof(c[i].bb));
    edf_lock();
    g_edf[g_edf_next] = e;
    g_edf_next = (g_edf_next + 1) % 12;
    LeaveCriticalSection(&g_edf_cs);
    return c[i].ok;
}

/* lecture à l'avance : casque et bottes du pilote, sur un fil à part */
static char g_edf_todo[6][300];
static int g_edf_ntodo = 0;

static void edf_collect_cb(const char *path, void *model, void *user)
{
    float bb[6];
    int ok, k;
    (void)model;
    (void)user;
    if (g_edf_ntodo >= 6 || edf_cached(path, bb, &ok))
        return;
    for (k = 0; k < g_edf_ntodo; ++k)
        if (!strcmp(g_edf_todo[k], path))
            return;
    snprintf(g_edf_todo[g_edf_ntodo++], sizeof(g_edf_todo[0]), "%s", path);
}

static DWORD WINAPI edf_warm_thread(LPVOID arg)
{
    int k;
    float bb[6];
    (void)arg;
    for (k = 0; k < g_edf_ntodo; ++k)
        edf_read(g_edf_todo[k], bb);
    InterlockedExchange(&g_edf_warm, 0);
    return 0;
}

static void edf_warm_start(void)
{
    HANDLE th;
    static int logged = 0;
    if (InterlockedCompareExchange(&g_edf_warm, 1, 0) != 0)
        return;
    {
        /* lectures finies : au journal (sur le fil principal) */
        int k;
        edf_lock();
        for (k = 0; k < 12; ++k)
            if (g_edf[k].path[0] && !(logged & (1 << k))) {
                logged |= 1 << k;
                log_line("pilote au sol : %s %s (lu a l'avance)", g_edf[k].path,
                         g_edf[k].ok ? "lu (taille de l'element pour le sol)" : "pas lisible, boite par defaut");
            }
        LeaveCriticalSection(&g_edf_cs);
    }
    g_edf_ntodo = 0;
    equip_each_gear(edf_collect_cb, NULL);
    if (!g_edf_ntodo) {
        InterlockedExchange(&g_edf_warm, 0);
        return;
    }
    th = CreateThread(NULL, 0, edf_warm_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&g_edf_warm, 0);
}

typedef struct {
    Ragdoll *rd;
    float need[RD_NUM_POINTS]; /* v1.8.23 : jusqu'où l'élément descend sous le point (m) */
    int n;
} EquipGround;
static void equip_ground_cb(const char *low, const float M[12], void *user)
{
    EquipGround *eg = (EquipGround *)user;
    Ragdoll *rd = eg->rd;
    int tg[2], nt = 0, a, b, c2, k, j;
    float bb[6], cen[3], hw[3], wc[3], d;
    if (strstr(low, "\\helmets\\") && strstr(low, "helmet.hrc") && !strstr(low, "c_helmet"))
        tg[nt++] = RD_HEAD;
    else if (strstr(low, "\\boots\\") && strstr(low, "left_boot"))
        tg[nt++] = RD_KNEE_L, tg[nt++] = RD_FOOT_L;
    else if (strstr(low, "\\boots\\") && strstr(low, "right_boot"))
        tg[nt++] = RD_KNEE_R, tg[nt++] = RD_FOOT_R;
    else
        return;
    if (!edf_box(low, bb)) {
        /* v1.8.23 : bottes dans un .pkz chiffré : boîte d'une botte ordinaire (même repère que
           les left_boot / right_boot lisibles : du genou, x, vers le bout du pied) */
        static const float bl[6] = { 0.13f, -0.07f, -0.08f, 0.50f, 0.37f, 0.21f };
        static const float br[6] = { 0.13f, -0.36f, -0.08f, 0.51f, 0.07f, 0.21f };
        if (nt != 2)
            return;
        memcpy(bb, tg[0] == RD_KNEE_L ? bl : br, sizeof(bb));
    }
    for (k = 0; k < 3; ++k) {
        cen[k] = 0.5f * (bb[k] + bb[k + 3]);
        hw[k] = 0.5f * (bb[k + 3] - bb[k]);
    }
    /* l'élément est bien sur notre pilote (sinon : un autre joueur, ou repère inattendu) */
    for (k = 0; k < 3; ++k)
        wc[k] = M[k * 4 + 0] * cen[0] + M[k * 4 + 1] * cen[1] + M[k * 4 + 2] * cen[2] + M[k * 4 + 3];
    d = 1e9f;
    for (j = 0; j < nt; ++j) {
        float dd = 0.0f;
        for (k = 0; k < 3; ++k)
            dd += (wc[k] - rd->pos[tg[j]][k]) * (wc[k] - rd->pos[tg[j]][k]);
        if (dd < d)
            d = dd;
    }
    if (d > 0.45f * 0.45f) {
        static int rej = 0;
        if (rej < 4) {
            ++rej;
            log_line("pilote au sol : %s a %.2f m du point du corps : pas sur notre pilote (ou autre repere), ignore",
                     low, sqrtf(d));
        }
        return;
    }
    ++eg->n;
    /* centres des faces, et coins rapprochés du centre (casque et bottes sont arrondis) */
    for (a = -1; a <= 1; ++a)
        for (b = -1; b <= 1; ++b)
            for (c2 = -1; c2 <= 1; ++c2) {
                int nz = (a != 0) + (b != 0) + (c2 != 0);
                float l[3], w[3], gy, gn[3], pen, s = nz == 3 ? 0.72f : (nz == 2 ? 0.85f : 1.0f);
                int best = tg[0];
                if (nz == 0)
                    continue;
                l[0] = cen[0] + a * hw[0] * s;
                l[1] = cen[1] + b * hw[1] * s;
                l[2] = cen[2] + c2 * hw[2] * s;
                for (k = 0; k < 3; ++k)
                    w[k] = M[k * 4 + 0] * l[0] + M[k * 4 + 1] * l[1] + M[k * 4 + 2] * l[2] + M[k * 4 + 3];
                (void)gy;
                (void)gn;
                if (nt == 2) {
                    /* le bout de la botte le plus proche : genou ou pied */
                    float d0 = 0.0f, d1 = 0.0f;
                    for (k = 0; k < 3; ++k) {
                        d0 += (w[k] - rd->pos[tg[0]][k]) * (w[k] - rd->pos[tg[0]][k]);
                        d1 += (w[k] - rd->pos[tg[1]][k]) * (w[k] - rd->pos[tg[1]][k]);
                    }
                    best = d0 < d1 ? tg[0] : tg[1];
                }
                pen = rd->pos[best][1] - w[1]; /* l'élément descend de pen sous le point */
                if (pen > eg->need[best])
                    eg->need[best] = pen;
            }
}

static void equip_gear_cb(const char *path, void *model, void *user)
{
    float M[12];
    if (skelscan_model_world(model, M))
        equip_ground_cb(path, M, user);
}

static void equip_ground_fix(Ragdoll *rd, float dt)
{
    /* v1.8.23 : plus de poussée après coup (le casque tremblait : poussé, retombé, poussé...).
       La taille du casque et des bottes devient une épaisseur en plus contre le sol pour la
       tête, les genoux et les pieds, prise en compte par le ragdoll lui-même, et changée en
       douceur. Les membres sont vérifiés en leur milieu dans le ragdoll. */
    EquipGround eg;
    int i;
    static int logged = 0;
    float k = 1.0f - expf(-2.5f * dt);
    memset(&eg, 0, sizeof(eg));
    eg.rd = rd;
    if (g_terrain_ok && rd->ground_fn)
        equip_each_gear(equip_gear_cb, &eg);
    if (!logged && eg.n) {
        logged = 1;
        log_line("pilote au sol : %d element(s) (casque, bottes) pris pour l'epaisseur contre le sol", eg.n);
    }
    for (i = 0; i < RD_NUM_POINTS; ++i) {
        float want = 0.0f;
        if (eg.need[i] > 0.0f) {
            want = eg.need[i] - rd_ground_radius(rd, i);
            if (want < 0.0f)
                want = 0.0f;
            if (want > 0.15f)
                want = 0.15f;
        }
        rd->gextra[i] += (want - rd->gextra[i]) * k;
    }
}

static void terrain_plane(Ragdoll *rd)
{
    float h, n[3], x = rd->pos[RD_PELVIS][0], z = rd->pos[RD_PELVIS][2];
    if (!g_terrain_ok || !terrain_height(x, z, &h, n))
        return;
    h += g_terrain_c;
    rd->ground_n[0] = n[0];
    rd->ground_n[1] = n[1];
    rd->ground_n[2] = n[2];
    rd->ground_y = n[0] * x + n[1] * h + n[2] * z;
}

/* Recale le relief sur le corps de chute du jeu quand il est posé
   (ou vérifie le recalage venu du cache) */
static int terrain_bike_ref(float ref[3]);
static int g_terrain_bike_cal = 0; /* relief recalé sur la moto arrêtée (référence la plus sûre) */

/* v1.7.4 : hauteur du point de référence de la moto (télémétrie) au-dessus du sol, mesurée sur
   ses roues, gardée pour chaque moto (MXRP_Ragdoll_hauteur_moto.ini : « id moto|hauteur »).
   Sans elle, le relief était recalé avec 8 cm supposés puis sur les roues : 17 cm de saut dans
   la même partie sur la KTM (hauteur du pilote à refaire à chaque fois) */
static char g_refh_bike[100] = "";
static float g_refh = 0.0f;
static int g_refh_ok = 0; /* le point peut être sous le sol (KTM : -8 cm) */
static void refh_path(char *out, size_t n)
{
    size_t l = strlen(g_user_dir);
    (void)l;
    snprintf(out, n, "%sMXRP_Ragdoll_hauteur_moto.ini", g_rdir);
}
static void refh_select(const char *bike)
{
    char path[600], ln[300];
    FILE *f;
    snprintf(g_refh_bike, sizeof(g_refh_bike), "%s", bike ? bike : "");
    g_refh = 0.0f;
    g_refh_ok = 0;
    if (!g_user_dir[0] || !g_refh_bike[0])
        return;
    refh_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(ln, sizeof(ln), f)) {
        char *bar = strrchr(ln, '|');
        if (!bar || ln[0] == ';')
            continue;
        *bar = 0;
        if (!strcmp(ln, g_refh_bike)) {
            float h = (float)atof(bar + 1);
            if (h > -1.0f && h < 1.5f) {
                g_refh = h;
                g_refh_ok = 1;
            }
        }
    }
    fclose(f);
    if (g_refh_ok)
        log_line("moto : point de reference a %+.2f m du sol (mesure gardee pour %s)", g_refh, g_refh_bike);
}
static void refh_store(float h)
{
    char path[600], ln[300], out[8192] = "; MXRP_Ragdoll : hauteur de chaque moto au-dessus du sol (mesuree sur ses roues)\n";
    FILE *f;
    size_t ol;
    if (!g_user_dir[0] || !g_refh_bike[0] || h <= -1.0f || h > 1.5f) {
        log_line("moto : hauteur au-dessus du sol pas gardee (%+.2f m, moto \"%s\")", h, g_refh_bike);
        return;
    }
    if (g_refh_ok && fabsf(h - g_refh) < 0.01f)
        return;
    g_refh = h;
    g_refh_ok = 1;
    refh_path(path, sizeof(path));
    f = fopen(path, "r");
    if (f) {
        while (fgets(ln, sizeof(ln), f)) {
            char key[300], *bar;
            if (ln[0] == ';')
                continue;
            snprintf(key, sizeof(key), "%s", ln);
            bar = strrchr(key, '|');
            if (!bar)
                continue;
            *bar = 0;
            if (!strcmp(key, g_refh_bike))
                continue;
            ol = strlen(out);
            if (ol + strlen(ln) + 2 < sizeof(out))
                snprintf(out + ol, sizeof(out) - ol, "%s%s", ln, strchr(ln, '\n') ? "" : "\n");
        }
        fclose(f);
    }
    ol = strlen(out);
    snprintf(out + ol, sizeof(out) - ol, "%s|%.3f\n", g_refh_bike, h);
    f = fopen(path, "w");
    if (f) {
        fputs(out, f);
        fclose(f);
    }
    log_line("moto : point de reference a %+.2f m du sol (mesure sur les roues, gardee pour %s)", h, g_refh_bike);
}

/* Moto arrêtée, droite, roues au sol : on recale le relief dessus (une fois par piste). Plus
   sûr que le corps couché du jeu (Suède : +0,23 m sur le corps, pilote enfoncé et moto relevée
   qui flottait) */
/* v1.7.34 : juste avant de soulever la moto (F) : elle est posée sur ses roues, c'est le vrai sol.
   Vu sur Lakewood en ligne : relief du cache 10 cm trop bas (-0,16 au lieu de -0,06), la moto était
   « descendue de 0,08 m » dans la terre : trous dans la piste, puis la moto envoyée en l'air. */
static int terrain_recal_wheels(uintptr_t cb, const char *why)
{
    float h;
    if (!cb || !g_terrain_ok)
        return 0;
    h = bike_measure_wheels(cb);
    if (h > 0.25f && h < 0.75f && fabsf(h - 0.345f) > 0.03f) {
        float c2 = g_terrain_c + (h - 0.345f);
        log_line("relief : recale sur les roues %s : %+.2f m (avant %+.2f m)", why, c2, g_terrain_c);
        g_terrain_c = c2;
        cache_save_async(c2);
        bike_measure_wheels(cb); /* hauteur des roues avec le nouveau relief */
        return 1;
    }
    return 0;
}

static void terrain_calibrate_stopped(const float pos[3])
{
    float h, c;
    if (g_terrain_bike_cal || !terrain_ready())
        return;
    if (!terrain_height(pos[0], pos[2], &h, NULL))
        return;
    {
        /* v1.8.22 : moto arrêtée SUR un objet (grille de départ, ponton) : ce n'est pas le relief.
           Flanders : moto sur la grille, relief monté de 0,51 m, tout le pilote flottait au sol */
        float ho, hc = h + g_terrain_c;
        if (objects_height_fn(pos[0], pos[2], pos[1] + 0.3f, &ho) && ho > hc + 0.10f) {
            static int said = 0;
            if (!said) {
                said = 1;
                log_line("relief : moto arretee sur un objet (%.2f m au-dessus du relief), pas de recalage ici", ho - hc);
            }
            return;
        }
    }
    g_terrain_bike_cal = 1;
    {
        /* v1.7.3 : hauteur du point de la moto au-dessus du sol mesurée sur ses roues (gardée
           pour chaque moto) ; avant : 8 cm supposés pour toutes, 17 cm faux sur la KTM, et le
           relief changeait à chaque partie (hauteur du pilote à refaire à chaque fois) */
        float refh = g_refh_ok ? g_refh : 0.08f;
        c = (pos[1] - refh) - h;
    }
    g_terrain_calib_logged = 1;
    g_terrain_from_cache = 0;
    {
        int was_prov = g_terrain_prov;
        g_terrain_prov = 0;
        if (was_prov && fabsf(c) <= 1.5f) {
            /* hauteur provisoire : on prend la vraie et on enregistre la piste */
            log_line("relief : mesure sur la moto arretee : %+.2f m (provisoire %+.2f m)", c, g_terrain_c);
            g_terrain_c = c;
            g_terrain_ok = 1;
            cache_save_async(c);
            return;
        }
    }
    if (fabsf(c) > 1.5f) {
        log_line("relief : ecart de %+.2f m avec la moto arretee, relief ignore", c);
        g_terrain_ok = 0;
        return;
    }
    if (g_terrain_ok && fabsf(c - g_terrain_c) < 0.04f) {
        log_line("relief : verifie sur la moto arretee (ecart %+.2f m)", c - g_terrain_c);
        return;
    }
    if (g_terrain_ok && fabsf(c - g_terrain_c) > 0.25f) {
        /* v1.8.22 : grand écart d'un coup : la moto n'est sans doute pas sur le relief (grille de
           départ, objet pas vu) ; on garde le relief connu, les roues le recaleront si besoin */
        log_line("relief : ecart de %+.2f m avec la moto arretee, trop grand : relief garde (%+.2f m)", c - g_terrain_c, g_terrain_c);
        return;
    }
    if (g_terrain_ok && !(g_refh_ok && terrain_live_ready())) {
        /* v1.7.6 : relief déjà connu (cache, recalé sur les roues) : on ne le change pas sur ce
           point de la moto (au départ la moto n'est pas encore posée : +0,15 m vu en jeu) ; la
           mesure sur les roues le corrige si besoin */
        log_line("relief : ecart de %+.2f m avec la moto arretee, garde (grille du jeu ou hauteur de la moto pas encore connue)", c - g_terrain_c);
        return;
    }
    log_line("relief : recale sur la moto arretee : %+.2f m (avant %+.2f m%s)", c, g_terrain_c, g_terrain_ok ? "" : ", inactif");
    g_terrain_c = c;
    g_terrain_ok = 1;
    cache_save_async(c);
}

static void terrain_calibrate(void *skeleton)
{
    float body[3], h, c, ref[3];
    int from_bike = terrain_bike_ref(ref);
    if (!terrain_ready() || g_terrain_calib_logged || (!from_bike && !skelscan_model_pos(skeleton, body)))
        return;
    if (!from_bike && g_terrain_prov == 2) {
        g_terrain_calib_logged = 1; /* mesure sur la moto en roulant : plus sûre que le corps couché */
        return;
    }
    g_terrain_calib_logged = 1;
    if (from_bike) {
        /* descente volontaire : le corps du jeu n'est pas couché au sol (il est sur la moto),
           on recale sur la moto arrêtée, roues au sol */
        memcpy(body, ref, sizeof(body));
        body[1] += g_cfg.body_ground;
    }
    if (!terrain_height(body[0], body[2], &h, NULL)) {
        log_line("relief : le corps du jeu (%.1f, %.1f) est hors du relief connu", body[0], body[2]);
        return;
    }
    c = (body[1] - g_cfg.body_ground) - h;
    if (from_bike)
        log_line("relief : recalage sur la moto (descente), ecart %+.2f m", c - g_terrain_c);
    if (g_terrain_from_cache) {
        g_terrain_from_cache = 0;
        if (fabsf(c - g_terrain_c) < 0.3f) {
            log_line("relief : cache verifie sur la chute (ecart %+.2f m)", c - g_terrain_c);
            return;
        }
        log_line("relief : le cache ne correspond plus (ecart %+.2f m), nouvelle recherche", c - g_terrain_c);
        g_terrain_ok = 0;
        g_terrain_bike_cal = 0;
        remove(g_cache_path);
        terrain_clear();
        terrain_scan_async(log_line);
        return;
    }
    if (fabsf(c) < (from_bike ? 1.5f : 0.6f)) { /* la moto arrêtée est une référence sûre */
        g_terrain_c = c;
        g_terrain_ok = 1;
        g_terrain_prov = 0;
        log_line("relief : recale de %+.2f m sous le corps du jeu (%.2f, %.2f, %.2f) -> ACTIF", c,
                 body[0], body[1], body[2]);
        cache_save_async(c);
    } else {
        log_line("relief : ecart de %+.2f m avec le corps du jeu, relief ignore", c);
    }
}

/*
 * Caméra qui suit le pilote quand il marche (fil de rendu, appelée par glcam au
 * début de chaque passe en perspective). Repère du jeu : y en haut, cap 0 = +z,
 * vue = lignes (droite, haut, avant), l'avant de la caméra est +z de la vue.
 */
static float g_cam_pos[3], g_cam_look[3];
static int g_cam_on = 0;
#define HEAD_YAW_MAX 1.35f /* le cou tourne de 77° au plus */
static volatile float g_walk_style = 1.0f; /* 1 = réglages de la marche appliqués, 0 = pendant un geste */
static volatile int g_first_person = 0; /* à pied : caméra dans la tête du pilote (touche V / clic du stick droit) */
/* v1.8.6 : la vue du jeu en roulant est-elle une vue de pilote (casque, guidon) ? reprise à pied */
static volatile int g_game_fp = -1;
static volatile int g_game_fp_prev = -1; /* v1.8.27 : vue d'avant le dernier changement */
static volatile double g_game_fp_t = 0.0;
static volatile double g_view_key_t = -100.0; /* v1.8.117 : dernier appui du joueur sur la touche / le bouton de vue du jeu */
static volatile int g_game_fp_player = 0;     /* v1.8.117 : le dernier changement de vue vient du joueur */
/* v1.8.15 : distance et hauteur de la caméra extérieure du jeu en roulant (reprises à pied / au sol) */
static float g_gcam_d = 0.0f, g_gcam_h = 0.0f;
static int g_gcam_ok = 0;
/* v1.8.162 : zoom à pied (molette) : distance en 3e personne, champ de vision en 1re personne */
static float g_zoom_dist = 1.0f;
static volatile int g_fp_zoom = 0; /* v1.8.167 : zoom à pied en 1re personne : 0 (aucun) à 3 */
static const float k_fp_zoom[5] = {1.0f, 1.5f, 2.5f, 4.0f, 6.0f}; /* v1.8.169 : 4e niveau */
static volatile float g_fp_zoom_cur = 1.0f; /* v1.8.171 : zoom 1re personne affiché (animé) */
static volatile LONG g_mclick_acc = 0; /* v1.8.168 : clics sur la molette à pied */
/* v1.8.168 : un cran de zoom de plus ; après le 3e, retour sans zoom */
static void fp_zoom_next(void) { g_fp_zoom = (g_fp_zoom + 1) % 5; }
static volatile LONG g_wheel_acc = 0;
static volatile double g_foot_cam_t = -100.0;
static volatile int g_pad_zoom = 0;      /* v1.8.163 : croix Haut (-1 : rapproche) / Bas tenue (+1 : éloigne) à pied */
static volatile float g_ride_fp_fov = 0.0f; /* v1.8.163 : champ de vision du jeu en roulant en 1re personne */
static volatile float g_foot_fov_want = 0.0f; /* v1.8.166 : champ de vision horizontal voulu à pied en 1re personne */
static float cam_dist(void) { return (g_gcam_ok ? g_gcam_d : g_cfg.camera_distance) * g_zoom_dist; }
static float cam_height(void) { return g_gcam_ok ? g_gcam_h : g_cfg.camera_height; }
static double g_cam_last = 0.0;
/* caméra libre : cap et hauteur d'orbite autour du pilote (souris, stick droit) */
static float g_cam_yaw = 0.0f, g_cam_pitch = 0.3f, g_cam_age = 0.0f;
static volatile int g_cam_yaw_ok = 0;
static volatile int g_running = 0; /* en piste, pas en pause : la souris peut être prise */
static volatile int g_kbd_prio = 1, g_pad_prio = 1;
#ifdef _WIN32
/* curseur de la fenêtre du jeu : enlevé pendant que la caméra prend la souris (le jeu le remettait
   à chaque mouvement, flèche au milieu de l'écran), remis dès qu'elle la lâche */
static HWND g_cur_hw = NULL;
static LONG_PTR g_cur_saved = 0;
static int g_cur_shows = 0; /* ShowCursor(FALSE) faits par nous */
/* v0.40.52 : le jeu remet sa flèche à chaque WM_SETCURSOR, dans le fil de sa fenêtre (nos
   appels viennent d'un autre fil) : on intercepte ce message dans la fenêtre elle-même */
static WNDPROC g_cur_orig_proc = NULL;
static HWND g_cur_sub_hw = NULL;
static volatile LONG g_cur_hidden = 0;
static int g_cur_wnd_hidden = 0; /* nombre de ShowCursor(FALSE) faits dans le fil de la fenêtre */
static LRESULT CALLBACK cursor_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    /* v0.40.53 : ShowCursor ne vaut que pour le fil qui l'appelle : celui de la fenêtre */
    /* v0.41.16 : on compte exactement nos appels et on rend exactement autant : le jeu garde
       le curseur Windows caché (il dessine le sien dans les menus) ; avant, on forçait le
       compteur à « visible » en rendant la souris : flèche Windows partout après une chute */
    if (g_cur_hidden && !g_cur_wnd_hidden) {
        int n = 0;
        while (n < 30) {
            ++n;
            if (ShowCursor(FALSE) < 0)
                break;
        }
        g_cur_wnd_hidden = n;
    } else if (!g_cur_hidden && g_cur_wnd_hidden) {
        for (; g_cur_wnd_hidden > 0; --g_cur_wnd_hidden)
            ShowCursor(TRUE);
    }
    /* v1.8.162 : molette à pied : zoom (pris par le plugin, pas donné au jeu) */
    if (m == WM_MOUSEWHEEL && now_seconds() - g_foot_cam_t < 0.3) {
        InterlockedExchangeAdd(&g_wheel_acc, (LONG)GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }
    /* v1.8.168 : clic molette à pied : cran de zoom suivant (1re personne) */
    if (m == WM_MBUTTONDOWN && now_seconds() - g_foot_cam_t < 0.3)
        InterlockedIncrement(&g_mclick_acc);
    if (m == WM_SETCURSOR && g_cur_hidden && LOWORD(lp) == HTCLIENT) {
        SetCursor(NULL);
        return TRUE;
    }
    return CallWindowProcA(g_cur_orig_proc, h, m, wp, lp);
}
static void cursor_subclass(HWND hw)
{
    if (g_cur_sub_hw || !hw)
        return;
    g_cur_orig_proc = (WNDPROC)SetWindowLongPtrA(hw, GWLP_WNDPROC, (LONG_PTR)cursor_wndproc);
    if (g_cur_orig_proc)
        g_cur_sub_hw = hw;
    log_line("souris : fenetre du jeu %s pour cacher le curseur", g_cur_orig_proc ? "interceptee" : "pas interceptee");
}
static void cursor_unsubclass(void)
{
    if (g_cur_sub_hw && g_cur_orig_proc &&
        (WNDPROC)GetWindowLongPtrA(g_cur_sub_hw, GWLP_WNDPROC) == cursor_wndproc)
        SetWindowLongPtrA(g_cur_sub_hw, GWLP_WNDPROC, (LONG_PTR)g_cur_orig_proc);
    g_cur_sub_hw = NULL;
}
static void cursor_hide(HWND hw)
{
    if (hw) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hw, &pid);
        if (pid == GetCurrentProcessId())
            cursor_subclass(hw);
    }
    InterlockedExchange(&g_cur_hidden, 1);
    if (g_cur_hw) {
        /* v0.40.50 : le jeu remet parfois sa flèche : on la recache à chaque image */
        if (GetCursor())
            SetCursor(NULL);
        return;
    }
    if (!hw)
        return;
    while (ShowCursor(FALSE) >= 0 && g_cur_shows < 20)
        ++g_cur_shows;
    ++g_cur_shows;
    g_cur_saved = GetClassLongPtrA(hw, GCLP_HCURSOR);
    SetClassLongPtrA(hw, GCLP_HCURSOR, 0);
    SetCursor(NULL);
    g_cur_hw = hw;
}
static void cursor_restore(void)
{
    InterlockedExchange(&g_cur_hidden, 0);
    if (!g_cur_hw)
        return;
    SetClassLongPtrA(g_cur_hw, GCLP_HCURSOR, g_cur_saved);
    SetCursor((HCURSOR)g_cur_saved);
    for (; g_cur_shows > 0; --g_cur_shows)
        ShowCursor(TRUE);
    g_cur_hw = NULL;
}
#else
static void cursor_restore(void) {}
#endif
static int g_was_foot = 0;      /* v0.41.16 : diagnostic du curseur au retour sur la moto */
static DWORD g_t_back = 0;
static volatile int g_mouse_free = 0; /* Échap : souris rendue (menu du jeu), reprise en bougeant le pilote */
/* v1.8.46 : roue des cônes (ouverte, case visée, curseur virtuel de la souris en pixels) */
static volatile int g_cone_menu = 0, g_wheel_sel = -1;
static volatile float g_wheel_px = 0.0f, g_wheel_py = 0.0f;
static char g_msg[192] = ""; /* v1.8.141 : 192 (avertissement des rampes) */
/* v1.2 */
static int g_srv_ragdoll = 0;
static int g_srv_tagged = 0;
static int g_session_online = 0; /* v1.8.204 : session sur un serveur (en ligne) */ /* v1.8.204 : nom du serveur avec RAGDOLL (même multi coupé) */
static int g_is_host = 0;    /* v1.8.204 : on héberge (liste des joueurs remplie) */
static int g_learn_off = 0; /* v1.7.30 : en ligne : la pose sur la moto n'est pas apprise */     /* en ligne sur un serveur RAGDOLL */
static char g_my_name[100] = "";
static int g_entries_clear = 1; /* nouvel événement : liste des joueurs à refaire */
          /* message court à l'écran */
/* v0.40.80 : appui long en cours : barre de progression (0..1) et secondes restantes */
static float g_msg_prog = -1.0f, g_msg_left = 0.0f;
static double g_msg_prog_t = 0.0;
/* v1.8.112 : touches utilisables affichées avec le message (clavier, puis bouton de la manette) */
static char g_msg_keys[48] = "";
static volatile int g_prompt = 0; /* v1.8.113 : à côté de la moto : 1 la relever, 2 monter dessus */
static volatile int g_prompt_up = 0; /* v1.8.114 : au sol après une chute : 1 pas encore possible, 2 possible */
static int g_msg_pad = 0;
static char g_reset_keys[24] = ""; /* respawn du jeu (profil) : touche du clavier */
static int g_reset_pad = 0;        /* respawn du jeu (profil) : bouton ou croix de la manette (masque XInput) */
/* nom d'un bouton de manette à l'écran (croix directionnelle en mots) */
static const char *pad_shown(int mask)
{
    static char b[2][64];
    static int k = 0;
    const char *n = cfg_pad_name(mask);
    char *o = b[k++ & 1];
    size_t ol = 0;
    o[0] = 0;
    while (*n && ol + 1 < sizeof(b[0])) {
        static const struct { const char *code, *fr; } d[] = {
            { "DUP", "fleche haut" }, { "DDOWN", "fleche bas" }, { "DLEFT", "fleche gauche" }, { "DRIGHT", "fleche droite" } };
        size_t i, done = 0;
        for (i = 0; i < 4 && !done; ++i) {
            size_t l = strlen(d[i].code);
            if (!strncmp(n, d[i].code, l) && (n[l] == 0 || n[l] == '+')) {
                snprintf(o + ol, sizeof(b[0]) - ol, "%s", tr(d[i].fr));
                ol = strlen(o);
                n += l;
                done = 1;
            }
        }
        if (!done)
            o[ol++] = *n++, o[ol] = 0;
    }
    return o;
}

static void hold_progress(const char *fr, float held, float need, const int *keys, int pad, const char *keytxt)
{
    if (!g_cfg.hold_msgs)
        return; /* v1.8.111 : option du menu P (messages et barres de progression cachés) */
    g_msg_keys[0] = 0;
    if (keytxt)
        snprintf(g_msg_keys, sizeof(g_msg_keys), "%s", keytxt);
    else if (keys && keys[0] > 0 && keys[1] > 0)
        snprintf(g_msg_keys, sizeof(g_msg_keys), "%s %s", cfg_key_name(keys[0]), cfg_key_name(keys[1]));
    else if (keys && keys[0] > 0)
        snprintf(g_msg_keys, sizeof(g_msg_keys), "%s", cfg_key_name(keys[0]));
    g_msg_pad = pad;
    snprintf(g_msg, sizeof(g_msg), "%s", tr(fr));
    g_msg_prog = need > 0.0f ? held / need : 1.0f;
    g_msg_left = need - held > 0.0f ? need - held : 0.0f;
    g_msg_prog_t = now_seconds();
}
static double g_msg_t = -100.0;
static int game_focused(void);
static void bike_find(const float rot[9], const float pos[3], const float vel[3]);
static int input_ok(void);
static volatile int g_mlt_open = 0; /* v1.8.204 : question « activer le multi ? » ouverte */
static volatile int g_upd_open = 0; /* v1.8.175 : fenêtre « mise à jour disponible » ouverte */
static volatile int g_tuto_open = 0; /* v1.8.110 : fenêtre du tutoriel ouverte (actions du plugin bloquées) */
static int key_down(int vk);
static int bind_key(const int k[2]);

/* ---------- gestes du pilote avec la moto ---------- */

#define PI_F 3.14159265f

static volatile int g_bike_lock;

static struct {
    int on;
    float side;              /* +1 : le pilote est à droite de la moto, -1 : à gauche */
    float right[3], fwd[3];  /* repère de la moto debout */
    float pivot[3];          /* centre de gravité du châssis au départ */
    float from[2], from_heading;
    float t;                 /* temps dans le geste en cours */
    int mount;               /* 1 : il enjambe la moto */
    int far_start;           /* la moto est couchée de son côté : plus de recul au départ */
    float grip[2][3];        /* poignées gauche et droite de la moto (monde) */
    int grip_ok;
    float tgt[2];            /* où il se tient (pour savoir s'il s'éloigne) */
    float adj[2];            /* recalage pour que les deux bras soient à bonne allonge */
    int hold_tank;           /* moto tenue debout : la main de l'autre côté va sur le réservoir */
} g_gesture;

/* descendre de la moto sans chute (touche réglable) : 1 = descente, 2 = à pied, 3 = remonte */
static volatile int g_dismounted = 0;
static int g_vol_phase = 0;
static float g_vol_t = 0.0f;
static int g_vol_e_prev = 1;
static float g_vol_free = 0.0f;
static int g_lift_stand = 0;  /* moto tenue droite sans le pilote (béquille) après une descente */
static volatile int g_lift_hold_render = 0; /* moto relevée et tenue : maintenue aussi avant chaque image */
static int g_soft_crash = 0;
static int g_vol_early = 0;   /* pilote en mod : le jeu passe en chute avant de prendre le squelette */
static int g_vol_tip = 0;     /* descente : 1 = attend la moto, 2 = bascule, 3 = jeu en chute (mains libres) */
static volatile int g_fade_want = 0; /* descente : écran noir pendant la bascule de la moto */
static volatile float g_fade = 0.0f;
static float g_vol_wait = 0.0f;  /* la chute qui commence est la descente : pas de ragdoll */
static float g_tip_t = 0.0f; /* > 0 : on couche la moto pour une descente par chute */ /* temps passé à pied sans tenir le bouton de remontée */ /* bouton de remontée : il faut le relâcher avant (Y gardé pour descendre) */
static volatile int g_dismount_pending = 0;
/* repère de la moto d'après la télémétrie (la moto n'est pas tombée : c'est le bon) */
static float g_tele_pos[3], g_tele_R[9];
static volatile int g_tele_ok = 0;

/* Sol sous la moto pendant une descente volontaire (moto arrêtée et droite) */
static int terrain_bike_ref(float ref[3])
{
    if (!(g_dismounted || g_lift_stand) || !g_tele_ok || g_tele_R[4] < 0.9f)
        return 0;
    ref[0] = g_tele_pos[0];
    /* le point de la télémétrie est à ~8 cm au-dessus du sol (Lacapelle : avec ground_offset
       = -0,55 le relief était recalé 0,47 m trop bas, pilote enfoncé ; assis, le bassin est à
       1,02-1,08 m au-dessus de ce point en Italie comme à Lacapelle) */
    ref[1] = g_tele_pos[1] - 0.08f;
    ref[2] = g_tele_pos[2];
    return 1;
}
static void dismount_reset(const char *why);

static float lerp_angle(float a, float b, float t)
{
    float d = b - a;
    while (d > PI_F)
        d -= 2.0f * PI_F;
    while (d < -PI_F)
        d += 2.0f * PI_F;
    return a + d * t;
}

static float smooth01(float x)
{
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1.0f)
        return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

/* repère actuel du châssis : centre de gravité et rotation (lignes) */
static int chassis_frame(uintptr_t body, float pos[3], float R[9])
{
    float s[16];
    int i, j;
    if (!body || !safe_read((const void *)(body + 0xB8), s, sizeof(s))) {
        if (g_dismounted && g_tele_ok) {
            /* descendu sans chute : la télémétrie donne la moto */
            memcpy(pos, g_tele_pos, sizeof(g_tele_pos));
            memcpy(R, g_tele_R, sizeof(g_tele_R));
            return 1;
        }
        return 0;
    }
    memcpy(pos, s, 3 * sizeof(float));
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            R[i * 3 + j] = s[4 + i * 4 + j];
    return 1;
}

/* point du châssis (repère moto : x droite, y haut, z avant) en monde */
static void chassis_point(const float pos[3], const float R[9], float x, float y, float z, float out[3])
{
    int i;
    for (i = 0; i < 3; ++i)
        out[i] = pos[i] + R[i * 3 + 0] * x + R[i * 3 + 1] * y + R[i * 3 + 2] * z;
}

static int g_grips_logged = 0;
/* poignées visées avant le glissement le long du guidon (pour placer le pilote) */
static float g_grab_raw[2][3];

/* v0.40.57 : poignées données par le gfx.cfg de la moto (bloc steer -> leftgrip / rightgrip,
   « pos » dans le repère du modèle du guidon). C'est là que le jeu pose les mains du pilote. */
static char g_gfx_dir[700] = "";
static int g_gfx_ok = 0;
static float g_gfx_grip[2][3];
static float g_gfx_up[3] = { 0.0f, 1.0f, 0.0f }; /* axe haut du modèle du guidon */

static int gfx_read_grips(const char *dir)
{
    char path[760], tok[64], stack[8][24], key[24] = "";
    FILE *f;
    int depth = 0, c, n, got = 0, want_val = 0;
    snprintf(path, sizeof(path), "%s\\gfx.cfg", dir);
    /* v1.7.31 : d'abord le dossier de la moto, sinon le gfx.cfg dans son .pkz */
    if ((f = fopen(path, "r")) == NULL) {
        if (!uipatch_pkz_to_temp(dir, "gfx.cfg", path, sizeof(path)) || (f = fopen(path, "r")) == NULL)
            return 0;
        log_line("moto : gfx.cfg lu dans %s.pkz", dir);
    }
    for (;;) {
        c = fgetc(f);
        if (c == EOF)
            break;
        if (c == '#' || (c == '/' && 0)) {
            while (c != EOF && c != '\n')
                c = fgetc(f);
            continue;
        }
        if (c == '{') {
            if (depth < 8) {
                strncpy(stack[depth], key, sizeof(stack[0]) - 1);
                stack[depth][sizeof(stack[0]) - 1] = '\0';
            }
            ++depth;
            key[0] = '\0';
            continue;
        }
        if (c == '}') {
            if (depth > 0)
                --depth;
            key[0] = '\0';
            continue;
        }
        if (c == '=') {
            want_val = 1;
            continue;
        }
        if (isspace(c))
            continue;
        n = 0;
        while (c != EOF && !isspace(c) && c != '{' && c != '}' && c != '=') {
            if (n + 1 < (int)sizeof(tok))
                tok[n++] = (char)c;
            c = fgetc(f);
        }
        tok[n] = '\0';
        if (c != EOF)
            ungetc(c, f);
        if (want_val) {
            want_val = 0;
            if (depth == 3 && !strcmp(stack[0], "steer") && !strcmp(stack[2], "pos") &&
                (!strcmp(stack[1], "leftgrip") || !strcmp(stack[1], "rightgrip")) && key[0] >= 'x' && key[0] <= 'z' &&
                !key[1]) {
                int sd = stack[1][0] == 'r' ? 1 : 0;
                g_gfx_grip[sd][key[0] - 'x'] = (float)atof(tok);
                got |= 1 << (sd * 3 + key[0] - 'x');
            }
            key[0] = '\0';
        } else {
            strncpy(key, tok, sizeof(key) - 1);
            key[sizeof(key) - 1] = '\0';
        }
    }
    fclose(f);
    return got == 63;
}

/* garde-fou : chaque poignée du gfx.cfg est près du levier de son côté (sinon repère faux) */
static int gfx_grips_plausible(const float center[3], float g[2][3])
{
    static const char *lev[2] = { "clutch_lever", "brake_lever" };
    static int warned = 0;
    int sd;
    for (sd = 0; sd < 2; ++sd) {
        float lv[3], d0, d1;
        if (!skelscan_find_node(center, 2.5f, lev[sd], lv))
            continue;
        d0 = sqrtf((lv[0] - g[sd][0]) * (lv[0] - g[sd][0]) + (lv[1] - g[sd][1]) * (lv[1] - g[sd][1]) + (lv[2] - g[sd][2]) * (lv[2] - g[sd][2]));
        d1 = sqrtf((lv[0] - g[1 - sd][0]) * (lv[0] - g[1 - sd][0]) + (lv[1] - g[1 - sd][1]) * (lv[1] - g[1 - sd][1]) +
                   (lv[2] - g[1 - sd][2]) * (lv[2] - g[1 - sd][2]));
        if (d0 > 0.25f && d1 > 0.25f) {
            if (!warned) {
                warned = 1;
                log_line("moto : poignees du gfx.cfg loin des leviers (%.2f m), on garde les leviers", d0 < d1 ? d0 : d1);
            }
            return 0;
        }
    }
    return 1;
}

/* poignées du gfx.cfg en monde (0 gauche, 1 droite) ; 0 si pas de gfx.cfg lisible */
static int gfx_grips_world(const float center[3], float out[2][3])
{
    float M[12];
    char mp[700], dir[700];
    char *sl;
    int k, i;
    if (!ridepose_find_steer(center, M, mp, sizeof(mp))) /* v1.7.32 : le guidon de SA moto */
        return 0;
    strncpy(dir, mp, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    sl = strrchr(dir, '\\');
    if (!sl || (strrchr(dir, '/') && strrchr(dir, '/') > sl))
        sl = strrchr(dir, '/');
    if (!sl)
        return 0;
    *sl = '\0';
    if (strcmp(dir, g_gfx_dir)) {
        strncpy(g_gfx_dir, dir, sizeof(g_gfx_dir) - 1);
        g_gfx_ok = gfx_read_grips(dir);
        if (g_gfx_ok)
            log_line("moto : poignees du gfx.cfg (%s) : gauche %.3f %.3f %.3f, droite %.3f %.3f %.3f", dir,
                     g_gfx_grip[0][0], g_gfx_grip[0][1], g_gfx_grip[0][2], g_gfx_grip[1][0], g_gfx_grip[1][1],
                     g_gfx_grip[1][2]);
        else
            log_line("moto : pas de poignees lisibles dans %s\\gfx.cfg (archive .pkz ?), poignees estimees", dir);
    }
    if (!g_gfx_ok)
        return 0;
    {
        float ul = sqrtf(M[1] * M[1] + M[5] * M[5] + M[9] * M[9]);
        if (ul > 0.1f) {
            g_gfx_up[0] = M[1] / ul;
            g_gfx_up[1] = M[5] / ul;
            g_gfx_up[2] = M[9] / ul;
        }
    }
    for (k = 0; k < 2; ++k)
        for (i = 0; i < 3; ++i)
            out[k][i] = M[i * 4 + 0] * g_gfx_grip[k][0] + M[i * 4 + 1] * g_gfx_grip[k][1] + M[i * 4 + 2] * g_gfx_grip[k][2] +
                        M[i * 4 + 3];
    return 1;
}

/* v1.7.29 : garde-fou des points appris : ils doivent être sur la moto que le pilote tient
   (corps physique). Vu en ligne sur une Fantic : le modèle 3D trouvé n'était pas celui de la
   moto tenue, mains à 1,6 m des épaules et bassin 0,7 m trop bas, pilote à côté de la moto. */
static int learned_grips_ok(const float pos[3], float g[2][3])
{
    static int warned = 0;
    int k;
    for (k = 0; k < 2; ++k) {
        float dx = g[k][0] - pos[0], dy = g[k][1] - pos[1], dz = g[k][2] - pos[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 1.3f || dy < -0.3f) {
            if (!warned) {
                warned = 1;
                log_line("moto : mains apprises pas sur la moto tenue (%.2f m du cadre, hauteur %+.2f), poignees du gfx.cfg a la place", d, dy);
            }
            return 0;
        }
    }
    return 1;
}
static int learned_seat_ok(const float c3[3], const float lp[3], float ground)
{
    static int warned = 0;
    float dx = lp[0] - c3[0], dz = lp[2] - c3[2], h = sqrtf(dx * dx + dz * dz), up = lp[1] - ground;
    if (h > 0.9f || up < 0.6f || up > 1.6f) {
        if (!warned) {
            warned = 1;
            log_line("moto : selle apprise pas sur la moto tenue (%.2f m du cadre, %.2f m au-dessus du sol), selle estimee a la place", h, up);
        }
        return 0;
    }
    return 1;
}

/* Mains sur les poignées du guidon (estimées : 0,40 m de chaque côté, 0,45 m au-dessus
   et 0,55 m devant le centre de gravité du châssis) */
static void gesture_hands(uintptr_t body, float weight)
{
    float pos[3], R[9], ga[3], gb[3], r[3], da, db, g_gfx_w[2][3];
    int from_gfx = 0;
    if (!chassis_frame(body, pos, R))
        return;
    if (ridepose_world(pos, g_gfx_w, NULL, NULL) && learned_grips_ok(pos, g_gfx_w)) {
        /* v1.7 : là où le jeu met les mains quand on roule avec cette moto */
        int k;
        memcpy(ga, g_gfx_w[0], sizeof(ga));
        memcpy(gb, g_gfx_w[1], sizeof(gb));
        /* v1.7.10 : mains un peu plus en avant sur les poignées (demandé en jeu) */
        for (k = 0; k < 3; k += 2) {
            ga[k] += g_gesture.fwd[k] * HAND_FWD;
            gb[k] += g_gesture.fwd[k] * HAND_FWD;
        }
        from_gfx = 1;
        if (!g_grips_logged) {
            g_grips_logged = 1;
            log_line("moto : mains placees comme quand il roule (pose apprise)");
        }
    } else
    /* vraies poignées : les nœuds des leviers dans le modèle 3D du guidon (steer.hrc),
       embrayage à gauche, frein à droite, un peu vers l'extérieur le long du guidon */
    if (gfx_grips_world(pos, g_gfx_w) && gfx_grips_plausible(pos, g_gfx_w) &&
        (g_gfx_w[0][0] - g_gfx_w[1][0]) * (g_gfx_w[0][0] - g_gfx_w[1][0]) + (g_gfx_w[0][1] - g_gfx_w[1][1]) * (g_gfx_w[0][1] - g_gfx_w[1][1]) +
                (g_gfx_w[0][2] - g_gfx_w[1][2]) * (g_gfx_w[0][2] - g_gfx_w[1][2]) > 0.3f * 0.3f) {
        /* v0.40.57 : les poignées du gfx.cfg (là où le jeu met les mains en roulant) */
        memcpy(ga, g_gfx_w[0], sizeof(ga));
        memcpy(gb, g_gfx_w[1], sizeof(gb));
        {
            /* v0.40.72 : 2 cm plus bas que le point du gfx.cfg le long du guidon (0 : trop
               haut, 10 cm : un peu trop bas) */
            int k;
            for (k = 0; k < 3; ++k) {
                ga[k] -= g_gfx_up[k] * 0.02f;
                gb[k] -= g_gfx_up[k] * 0.02f;
            }
        }
        from_gfx = 1;
        if (!g_grips_logged) {
            float lv[3];
            g_grips_logged = 1;
            if (skelscan_find_node(pos, 2.5f, "clutch_lever", lv))
                log_line("moto : poignee gauche (gfx) a %.2f m du levier d'embrayage", sqrtf((lv[0] - ga[0]) * (lv[0] - ga[0]) +
                         (lv[1] - ga[1]) * (lv[1] - ga[1]) + (lv[2] - ga[2]) * (lv[2] - ga[2])));
            if (skelscan_find_node(pos, 2.5f, "brake_lever", lv))
                log_line("moto : poignee droite (gfx) a %.2f m du levier de frein", sqrtf((lv[0] - gb[0]) * (lv[0] - gb[0]) +
                         (lv[1] - gb[1]) * (lv[1] - gb[1]) + (lv[2] - gb[2]) * (lv[2] - gb[2])));
        }
    } else if (skelscan_find_node(pos, 2.5f, "clutch_lever", ga) && skelscan_find_node(pos, 2.5f, "brake_lever", gb)) {
        float ax[3] = { ga[0] - gb[0], ga[1] - gb[1], ga[2] - gb[2] }, l;
        int k;
        l = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        if (l > 0.2f && l < 1.2f)
            for (k = 0; k < 3; ++k) {
                ga[k] += ax[k] / l * 0.07f;
                gb[k] -= ax[k] / l * 0.07f;
            }
        ga[1] -= 0.02f; /* le nœud du levier est un peu au-dessus du guidon */
        gb[1] -= 0.02f;
        {
            /* si le modèle a des nœuds de poignée (grip, handle, throttle), on prend le plus proche
               de chaque levier : c'est l'endroit prévu pour la main */
            /* (pas « grip » : sur ce guidon, « ODI Grips+bar end » est le centre du maillage des
               poignées, pas une main ; il tirait la main droite vers le milieu du guidon) */
            static const char *k_words[] = { "throttle", "lhand", "rhand", "hand_" };
            float cand[16][3], *best[2] = { NULL, NULL }, bd[2] = { 0.30f * 0.30f, 0.30f * 0.30f };
            int nc = 0, w2, c, side;
            float *g2[2] = { ga, gb };
            for (w2 = 0; w2 < 4 && nc < 16; ++w2)
                nc += skelscan_find_nodes(pos, 2.5f, k_words[w2], cand + nc, 16 - nc);
            for (side = 0; side < 2; ++side)
                for (c = 0; c < nc; ++c) {
                    float dx = cand[c][0] - g2[side][0], dy = cand[c][1] - g2[side][1], dz = cand[c][2] - g2[side][2];
                    float d = dx * dx + dy * dy + dz * dz;
                    if (d < bd[side]) {
                        bd[side] = d;
                        best[side] = cand[c];
                    }
                }
            for (side = 0; side < 2; ++side)
                if (best[side])
                    memcpy(g2[side], best[side], 3 * sizeof(float));
            if (!g_grips_logged) {
                g_grips_logged = 1;
                log_line("moto : poignees prises sur le modele 3D (guidon de %.2f m, noeuds de poignee : %s / %s)", l,
                         best[0] ? "oui" : "non", best[1] ? "oui" : "non");
                {
                    static int listed = 0;
                    if (!listed) {
                        listed = 1;
                        skelscan_log_model_nodes(pos, 2.5f, "lever", log_line);
                    }
                }
            }
        }
    } else {
        chassis_point(pos, R, -0.40f, 0.45f, 0.55f, ga);
        chassis_point(pos, R, 0.40f, 0.45f, 0.55f, gb);
        if (!g_grips_logged) {
            g_grips_logged = 1;
            log_line("moto : leviers introuvables dans le modele 3D, poignees estimees");
        }
    }
    memcpy(g_gesture.grip[0], ga, sizeof(ga));
    memcpy(g_gesture.grip[1], gb, sizeof(gb));
    g_gesture.grip_ok = 1;
    r[0] = cosf(g_walker.heading);
    r[1] = 0.0f;
    r[2] = -sinf(g_walker.heading);
    if (g_walker.mirror) {
        r[0] = -r[0];
        r[2] = -r[2];
    }
    da = (ga[0] - g_walker.pos[0]) * r[0] + (ga[2] - g_walker.pos[1]) * r[2];
    db = (gb[0] - g_walker.pos[0]) * r[0] + (gb[2] - g_walker.pos[1]) * r[2];
    /* main droite (index 1, côté +r) sur la poignée la plus à droite du pilote */
    memcpy(g_walker.grab_hand[1], da > db ? ga : gb, sizeof(ga));
    memcpy(g_walker.grab_hand[0], da > db ? gb : ga, sizeof(ga));
    {
        /* journal : les mains atteignent-elles les poignées ? (toutes les 5 s) */
        static float next_t = 0.0f;
        static float clock_t = 0.0f;
        clock_t += 1.0f / 60.0f;
        if (weight > 0.9f && clock_t >= next_t) {
            int sd, k;
            float d[2];
            next_t = clock_t + 5.0f;
            for (sd = 0; sd < 2; ++sd) {
                const float *sh = g_rd.pos[sd == 0 ? RD_SHOULDER_L : RD_SHOULDER_R];
                const float *gp = (sd == 1) == (da > db) ? ga : gb;
                float v = 0.0f;
                for (k = 0; k < 3; ++k)
                    v += (sh[k] - gp[k]) * (sh[k] - gp[k]);
                d[sd] = sqrtf(v);
            }
            log_line("moto tenue : epaule-poignee %.2f / %.2f m (bras %.2f m, poignet compris)", d[0], d[1],
                     g_walker.upper_arm + g_walker.forearm + (from_gfx ? 0.05f : 0.0f));
        }
    }
    if (g_gesture.hold_tank && !g_gesture.mount) {
        /* v0.40.75 : moto tenue debout, il est à côté du réservoir : une main sur la poignée de
           son côté, l'autre posée sur le haut du réservoir (la poignée d'en face est trop loin,
           vu en jeu : main en l'air devant la poitrine) */
        float mid[3], d0, d1;
        int k, far_i;
        for (k = 0; k < 3; ++k)
            mid[k] = 0.5f * (g_walker.grab_hand[0][k] + g_walker.grab_hand[1][k]);
        d0 = (g_walker.grab_hand[0][0] - g_walker.pos[0]) * (g_walker.grab_hand[0][0] - g_walker.pos[0]) +
             (g_walker.grab_hand[0][2] - g_walker.pos[1]) * (g_walker.grab_hand[0][2] - g_walker.pos[1]);
        d1 = (g_walker.grab_hand[1][0] - g_walker.pos[0]) * (g_walker.grab_hand[1][0] - g_walker.pos[0]) +
             (g_walker.grab_hand[1][2] - g_walker.pos[1]) * (g_walker.grab_hand[1][2] - g_walker.pos[1]);
        far_i = d0 > d1 ? 0 : 1;
        {
            /* v1.7.1 : la main se pose sur la selle, là où le pilote s'assoit quand il roule
               (avant : 40 cm derrière le guidon, sur l'avant de la selle) */
            /* v1.7.15 : sur le haut du réservoir, juste derrière la colonne de direction (selle :
               main posée bas contre la hanche ; guidon : les deux mains collées, vu en jeu) */
            if (1) {
                g_walker.grab_hand[far_i][0] = mid[0] - g_gesture.fwd[0] * 0.28f;
                g_walker.grab_hand[far_i][1] = mid[1] - 0.18f;
                g_walker.grab_hand[far_i][2] = mid[2] - g_gesture.fwd[2] * 0.28f;
            } else {
                g_walker.grab_hand[far_i][0] = mid[0] - g_gesture.fwd[0] * 0.55f;
                g_walker.grab_hand[far_i][1] = mid[1] - 0.25f;
                g_walker.grab_hand[far_i][2] = mid[2] - g_gesture.fwd[2] * 0.55f;
            }
        }
    }
    {
        /* v0.40.68 : poignée hors de portée (l'autre poignée quand il est à côté de la moto) :
           la main glisse le long du guidon vers le milieu au lieu d'un bras tendu à fond */
        float reach = 1.0f * (g_walker.upper_arm + g_walker.forearm);
        int sd, k, st;
        float other[2][3];
        memcpy(g_grab_raw, g_walker.grab_hand, sizeof(g_grab_raw));
        memcpy(other[0], g_walker.grab_hand[1], sizeof(other[0]));
        memcpy(other[1], g_walker.grab_hand[0], sizeof(other[1]));
        for (sd = 0; sd < 2; ++sd) {
            const float *sh = g_rd.pos[sd == 0 ? RD_SHOULDER_L : RD_SHOULDER_R];
            float *hp = g_walker.grab_hand[sd], base[3];
            memcpy(base, hp, sizeof(base));
            for (st = 0; st <= 10; ++st) { /* au plus 45 % du guidon (la main de l'autre côté, il est à côté du réservoir) */
                float u = 0.045f * st, d2 = 0.0f, q[3];
                for (k = 0; k < 3; ++k) {
                    q[k] = base[k] + (other[sd][k] - base[k]) * u;
                    d2 += (q[k] - sh[k]) * (q[k] - sh[k]);
                }
                memcpy(hp, q, sizeof(q));
                if (d2 <= reach * reach)
                    break;
            }
        }
    }
    if (from_gfx) {
        /* le point du gfx.cfg est le milieu de la paume sur la poignée ; notre main est le
           poignet : 7 cm vers l'épaule (la main serre la poignée) */
        int sd, k;
        for (sd = 0; sd < 2; ++sd) {
            const float *sh = g_rd.pos[sd == 0 ? RD_SHOULDER_L : RD_SHOULDER_R];
            float v[3], l;
            for (k = 0; k < 3; ++k)
                v[k] = sh[k] - g_walker.grab_hand[sd][k];
            l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l > 0.1f)
                for (k = 0; k < 3; ++k)
                    g_walker.grab_hand[sd][k] += v[k] / l * 0.05f;
        }
    }
    {
        /* v1.8.27 : mains lissées. La main qui glisse le long du guidon change de place par
           crans selon la distance à l'épaule ; l'épaule suit la main : ça faisait un aller-retour
           à chaque image (tête du pilote : 1 à 4 cm d'à-coups par image dans le journal) */
        static float sm[2][3];
        static double sm_t = 0.0;
        double tn = now_seconds();
        float sdt = (float)(tn - sm_t), d2 = 0.0f, kk;
        int sd, k;
        for (sd = 0; sd < 2; ++sd)
            for (k = 0; k < 3; ++k)
                d2 += (g_walker.grab_hand[sd][k] - sm[sd][k]) * (g_walker.grab_hand[sd][k] - sm[sd][k]);
        if (sm_t == 0.0 || sdt > 0.3f || sdt < 0.0f || d2 > 0.5f * 0.5f)
            memcpy(sm, g_walker.grab_hand, sizeof(sm));
        else {
            kk = 1.0f - expf(-12.0f * sdt);
            for (sd = 0; sd < 2; ++sd)
                for (k = 0; k < 3; ++k)
                    sm[sd][k] += (g_walker.grab_hand[sd][k] - sm[sd][k]) * kk;
        }
        sm_t = tn;
        memcpy(g_walker.grab_hand, sm, sizeof(sm));
    }
    g_walker.grab = weight;
}

static void gesture_clear(void)
{
    g_grips_logged = 0;
    memset(&g_gesture, 0, sizeof(g_gesture));
    g_walker.grab = 0.0f;
    g_walker.crouch = 0.0f;
    g_walker.leg_lift = 0.0f;
    g_walker.sit = 0.0f;
    g_walker.pegs = 0.0f;
    g_walker.rise = 0.0f;
    g_walker.lean = 0.0f;
    g_walker.leg_clear = 0.0f;
}

/* Au début du relevage : le pilote va se placer du côté où est la selle, face à la moto */
static void gesture_lift_begin(uintptr_t body)
{
    float pos[3], R[9], h, l, upd;
    if (!chassis_frame(body, pos, R))
        return;
    l = sqrtf(R[2] * R[2] + R[8] * R[8]);
    if (l < 0.2f)
        return;
    h = atan2f(R[2] / l, R[8] / l);
    g_gesture.fwd[0] = sinf(h); g_gesture.fwd[1] = 0.0f; g_gesture.fwd[2] = cosf(h);
    g_gesture.right[0] = cosf(h); g_gesture.right[1] = 0.0f; g_gesture.right[2] = -sinf(h);
    /* du côté où arrive le pilote (gauche ou droite). Si le haut de la moto couchée est
       tourné vers lui, le guidon est de son côté : il s'écarte davantage au début. */
    upd = R[1] * g_gesture.right[0] + R[7] * g_gesture.right[2];
    g_gesture.side = ((g_walker.pos[0] - pos[0]) * g_gesture.right[0] + (g_walker.pos[1] - pos[2]) * g_gesture.right[2]) >= 0.0f
                         ? 1.0f : -1.0f;
    g_gesture.far_start = upd * g_gesture.side > 0.0f;
    memcpy(g_gesture.pivot, pos, sizeof(pos));
    g_gesture.from[0] = g_walker.pos[0];
    g_gesture.from[1] = g_walker.pos[1];
    g_gesture.from_heading = g_walker.heading;
    g_gesture.t = 0.0f;
    g_gesture.mount = 0;
    g_gesture.hold_tank = 0;
    g_gesture.on = 1;
    g_gesture.adj[0] = g_gesture.adj[1] = 0.0f;
    g_walker.leg_lift = 0.0f;
}

/* À chaque pas pendant que le pilote tient la moto */
static void gesture_update(uintptr_t body, float dt, float lift_progress)
{
    float px, pz, hd, a;
    if (!g_gesture.on)
        return;
    g_gesture.t += dt;
    if (!g_gesture.mount) {
        /* sur le côté de la moto, juste derrière la poignée de son côté, tourné vers l'avant
           et un peu vers la moto : une main sur la poignée proche, l'autre passe devant lui
           jusqu'à la poignée opposée, comme pour pousser une moto */
        float ca = cosf(0.55f), sa = sinf(0.55f); /* 30° vers la moto */
        float dx_ = ca * g_gesture.fwd[0] - sa * g_gesture.side * g_gesture.right[0];
        float dz_ = ca * g_gesture.fwd[2] - sa * g_gesture.side * g_gesture.right[2];
        float lat = 0.50f + (g_gesture.far_start ? 0.45f : 0.15f) * (1.0f - smooth01(lift_progress));
        float mx = g_gesture.pivot[0] + g_gesture.fwd[0] * 0.55f; /* centre du guidon, moto debout */
        float mz = g_gesture.pivot[2] + g_gesture.fwd[2] * 0.55f;
        px = mx + g_gesture.right[0] * g_gesture.side * lat - g_gesture.fwd[0] * 0.25f;
        pz = mz + g_gesture.right[2] * g_gesture.side * lat - g_gesture.fwd[2] * 0.25f;
        hd = atan2f(dx_, dz_);
        a = smooth01(g_gesture.t / 0.4f);
        if (lift_progress < 1.0f) {
            g_gesture.tgt[0] = px;
            g_gesture.tgt[1] = pz;
            g_walker.pos[0] = g_gesture.from[0] + (px - g_gesture.from[0]) * a;
            g_walker.pos[1] = g_gesture.from[1] + (pz - g_gesture.from[1]) * a;
            g_walker.heading = lerp_angle(g_gesture.from_heading, hd, a);
        } else if (g_gesture.grip_ok && (g_bike_lock || fabsf(g_walker.speed) < 0.05f)) {
            /* v0.40.68 : aussi après le relevé tant que le joueur ne marche pas (vu en jeu :
               il restait collé au guidon, tête au-dessus, un bras replié et l'autre tendu) */
            /* moto debout : on se recale sur les vraies poignées. Il se met 0,40 m derrière
               la poignée de son côté et 0,20 m plus à l'extérieur, tourné vers le centre du
               guidon : les deux poignées sont devant lui */
            const float *ga = g_gesture.grip[0], *gb = g_gesture.grip[1];
            float sa_ = (ga[0] - g_gesture.pivot[0]) * g_gesture.right[0] + (ga[2] - g_gesture.pivot[2]) * g_gesture.right[2];
            float sb_ = (gb[0] - g_gesture.pivot[0]) * g_gesture.right[0] + (gb[2] - g_gesture.pivot[2]) * g_gesture.right[2];
            const float *near_g = (sa_ * g_gesture.side > sb_ * g_gesture.side) ? ga : gb;
            float cx = 0.5f * (ga[0] + gb[0]), cz = 0.5f * (ga[2] + gb[2]);
            float k = 1.0f - expf(-8.0f * dt);
            /* assez près pour que les bras restent pliés (coudes souples) */
            /* v0.40.59 : plus près de la moto et du guidon (avant : 15 cm à l'extérieur et 30 cm
               derrière la poignée, le bras qui va à l'autre poignée était tendu à fond) */
            /* v0.40.74 : plus en arrière, à côté du réservoir (demandé en jeu : il était collé à
               l'avant de la moto, la tête au-dessus du guidon). Place fixe : le recalage des
               bras le ramenait vers l'avant */
            /* v1.7.13 : 0,40 m derrière la poignée et 0,20 m à l'extérieur (avant 0,60 / 0,12 : il
               se tenait au niveau de la selle, vu en jeu sous plusieurs angles) */
            px = near_g[0] + g_gesture.right[0] * g_gesture.side * 0.15f - g_gesture.fwd[0] * 0.50f;
            pz = near_g[2] + g_gesture.right[2] * g_gesture.side * 0.15f - g_gesture.fwd[2] * 0.50f;
            g_gesture.hold_tank = 1; /* v0.40.75 */
            if (0) {
                /* v0.40.61 : on cherche où il doit se tenir pour que chaque épaule soit à ~80 %
                   de l'allonge de son bras de sa poignée (bras un peu pliés, ni tendus ni
                   repliés). Les épaules sont prises sur la pose actuelle (dans le repère du
                   pilote), puis on fait tourner le pilote vers le milieu du guidon à chaque essai. */
                float want = 0.80f * (g_walker.upper_arm + g_walker.forearm + 0.05f);
                float msg = g_walker.mirror ? -1.0f : 1.0f, hc = g_walker.heading;
                float fl[2], rl[2], sy[2], qx = px, qz = pz;
                int sd, it;
                for (sd = 0; sd < 2; ++sd) {
                    const float *sh = g_rd.pos[sd == 0 ? RD_SHOULDER_L : RD_SHOULDER_R];
                    float sx_ = sh[0] - g_walker.pos[0], sz_ = sh[2] - g_walker.pos[1];
                    fl[sd] = sx_ * sinf(hc) + sz_ * cosf(hc);
                    rl[sd] = (sx_ * cosf(hc) - sz_ * sinf(hc)) * msg;
                    sy[sd] = sh[1];
                }
                for (it = 0; it < 40; ++it) {
                    float h2 = atan2f(cx - qx, cz - qz), gx = 0.0f, gz = 0.0f;
                    for (sd = 0; sd < 2; ++sd) {
                        const float *gp = g_grab_raw[sd]; /* v0.40.69 : vraies poignées (la main glissée suivait le pilote : il finissait au-dessus du guidon) */
                        float shx = qx + sinf(h2) * fl[sd] + cosf(h2) * msg * rl[sd];
                        float shz = qz + cosf(h2) * fl[sd] - sinf(h2) * msg * rl[sd];
                        float ux = gp[0] - shx, uz = gp[2] - shz, uy = gp[1] - sy[sd];
                        float d = sqrtf(ux * ux + uy * uy + uz * uz), hh = sqrtf(ux * ux + uz * uz);
                        if (hh > 0.02f && d > 0.02f) {
                            float st = (d - want) * (d < want ? 1.0f : 0.5f) * (d / hh > 3.0f ? 3.0f : d / hh); /* v0.40.73 : un bras replié compte double (il se collait au guidon) */
                            gx += ux / hh * st;
                            gz += uz / hh * st;
                        }
                    }
                    qx += gx;
                    qz += gz;
                }
                /* pas trop loin de la place prévue (et jamais sur la moto) */
                {
                    float ox = qx - px, oz = qz - pz, ol = sqrtf(ox * ox + oz * oz);
                    float lat_in = -(ox * g_gesture.right[0] + oz * g_gesture.right[2]) * g_gesture.side;
                    if (ol > 0.40f) {
                        ox *= 0.40f / ol;
                        oz *= 0.40f / ol;
                    }
                    if (lat_in > 0.12f) {
                        ox += g_gesture.right[0] * g_gesture.side * (lat_in - 0.12f);
                        oz += g_gesture.right[2] * g_gesture.side * (lat_in - 0.12f);
                    }
                    px += ox;
                    pz += oz;
                }
            }
            g_gesture.tgt[0] = px;
            g_gesture.tgt[1] = pz;
            g_walker.pos[0] += (px - g_walker.pos[0]) * k;
            g_walker.pos[1] += (pz - g_walker.pos[1]) * k;
            g_walker.heading = lerp_angle(g_walker.heading, atan2f(cx - px, cz - pz), k);
        }
        /* penché et accroupi au départ, il se redresse avec la moto */
        /* il s'accroupit d'abord (dos droit), puis pousse sur les jambes avec la moto */
        /* v0.40.57 : même courbe que la moto (lourde au départ, plus facile ensuite) ; les
           jambes poussent d'abord (accroupi -> debout aux trois quarts), le buste reste penché
           en tirant et ne se redresse qu'à la fin */
        if (lift_progress <= 0.0f) {
            /* v0.40.72 : genoux pliés ET buste penché dos plat (charnière de hanche), comme pour
               soulever une charge ; avant : accroupi tout droit, un genou presque au sol */
            g_walker.crouch = 0.35f * smooth01(g_gesture.t / 0.45f);
            g_walker.lean = 0.85f * smooth01(g_gesture.t / 0.45f);
        } else {
            float e = powf(smooth01(lift_progress), 1.4f), e2 = e / 0.8f > 1.0f ? 1.0f : e / 0.8f;
            g_walker.crouch = 0.35f * (1.0f - smooth01(e / 0.7f)); /* les jambes poussent d'abord */
            g_walker.lean = 0.85f * (1.0f - smooth01(e / 0.9f)) - 0.20f * smooth01(e); /* le buste se redresse avec la moto, debout il se tient droit */
            (void)e2;
        }
        {
            float dx = g_walker.pos[0] - g_gesture.tgt[0], dz = g_walker.pos[1] - g_gesture.tgt[1];
            float close_w = dx * dx + dz * dz < 0.6f * 0.6f ? 1.0f : 0.0f; /* il s'éloigne : il lâche */
            /* v0.40.67 : il arrive et s'accroupit d'abord, PUIS tend les mains vers le guidon
               (avant : les mains partaient au guidon dès l'appui, bras étirés depuis loin) */
            gesture_hands(body, smooth01((g_gesture.t - 0.30f) / 0.30f) * close_w);
        }
    } else {
        /* monter sur la moto, comme un vrai pilote :
           0,0-0,4 s  il se place à côté de la selle, face à l'avant, mains au guidon
           0,3-0,6 s  il plie un peu les genoux et se penche sur le réservoir
           0,5-1,15 s il monte sur la pointe du pied d'appui, le talon de l'autre jambe remonte
                      derrière et la jambe passe au-dessus de l'arrière de la selle
           0,85-1,4 s le bassin glisse au milieu de la moto et s'assoit, un pied de chaque côté
           ensuite    assis, penché vers l'avant, prêt à partir (respawn) */
        float t = g_gesture.t, lat, r[3], gy, ftx, ftz, stand_y;
        float fw[3] = { g_gesture.fwd[0], 0.0f, g_gesture.fwd[2] }, rt[3] = { g_gesture.right[0], 0.0f, g_gesture.right[2] };
        float sc[2], sx, sz, lp[3], la[2][3], ls[3];
        int learned = 0, lean_ok = 0;
        {
            /* repère de la moto DEBOUT (lu maintenant), et la selle d'après les vraies poignées :
               le pilote assis a le bassin ~0,62 m derrière le milieu du guidon */
            float cp[3], R9[9], l;
            if (chassis_frame(body, cp, R9)) {
                l = sqrtf(R9[2] * R9[2] + R9[8] * R9[8]);
                if (l > 0.3f) {
                    fw[0] = R9[2] / l;
                    fw[2] = R9[8] / l;
                    rt[0] = fw[2];
                    rt[2] = -fw[0];
                    if (rt[0] * g_gesture.right[0] + rt[2] * g_gesture.right[2] < 0.0f) {
                        rt[0] = -rt[0];
                        rt[2] = -rt[2];
                    }
                }
                sc[0] = cp[0] - fw[0] * 0.10f;
                sc[1] = cp[2] - fw[2] * 0.10f;
            } else {
                sc[0] = g_gesture.pivot[0];
                sc[1] = g_gesture.pivot[2];
            }
            if (g_gesture.grip_ok) {
                /* v0.40.70 : 0,52 m (0,62 : assis, les bras n'atteignaient pas les poignées, vu en
                   1re personne : mains contre le réservoir) */
                sc[0] = 0.5f * (g_gesture.grip[0][0] + g_gesture.grip[1][0]) - fw[0] * 0.52f;
                sc[1] = 0.5f * (g_gesture.grip[0][2] + g_gesture.grip[1][2]) - fw[2] * 0.52f;
            }
            {
                /* v1.7 : la selle et les repose-pieds de CETTE moto, là où le jeu met le pilote quand
                   il roule (bassin, chevilles) ; le bassin finit 5 cm devant sc */
                float c3[3];
                if (chassis_frame(body, c3, R9) && ridepose_world(c3, NULL, lp, la) &&
                    learned_seat_ok(c3, lp, walk_ground_height(&g_rd, lp[0], lp[2]))) {
                    learned = 1;
                    lean_ok = ridepose_world2(c3, NULL, NULL, NULL, ls); /* épaules : pas encore apprises = buste d'avant */
                    /* v1.7.10 : fesses 12 cm plus en avant sur la selle (demandé en jeu) */
                    sc[0] = lp[0] + fw[0] * (SEAT_FWD - 0.05f);
                    sc[1] = lp[2] + fw[2] * (SEAT_FWD - 0.05f);
                }
            }
        }
        sx = sc[0] + rt[0] * g_gesture.side * 0.42f + fw[0] * 0.05f;
        sz = sc[1] + rt[2] * g_gesture.side * 0.42f + fw[2] * 0.05f;
        a = smooth01(t / 0.40f);
        g_walker.pos[0] = g_gesture.from[0] + (sx - g_gesture.from[0]) * a;
        g_walker.pos[1] = g_gesture.from[1] + (sz - g_gesture.from[1]) * a;
        g_walker.heading = lerp_angle(g_gesture.from_heading, atan2f(fw[0], fw[2]), a);
        gesture_hands(body, 1.0f);
        r[0] = cosf(g_walker.heading);
        r[2] = -sinf(g_walker.heading);
        if (g_walker.mirror) {
            r[0] = -r[0];
            r[2] = -r[2];
        }
        /* la moto est du côté -side x droite de la moto : la jambe de ce côté passe */
        g_walker.leg_side = (-g_gesture.side * (rt[0] * r[0] + rt[2] * r[2])) > 0.0f ? 1 : 0;
        /* petite flexion avant l'élan (il faut grab > 0 pour l'accroupi : c'est le cas) */
        g_walker.crouch = 0.07f * smooth01((t - 0.30f) / 0.15f) * (1.0f - smooth01((t - 0.48f) / 0.15f));
        g_walker.rise = 0.08f * smooth01((t - 0.50f) / 0.25f) * (1.0f - smooth01((t - 1.0f) / 0.3f));
        g_walker.leg_lift = smooth01((t - 0.50f) / 0.65f);
        /* les mains au guidon penchent déjà le buste (pose « grab ») : on le redresse un peu,
           sauf pendant le passage de la jambe (contrepoids), puis assis le dos est droit */
        /* il se penche bien en avant sur le réservoir pendant que la jambe tendue passe derrière
           (contrepoids), puis se redresse assis */
        g_walker.lean = -0.20f + 0.45f * sinf(PI_F * g_walker.leg_lift) + 0.25f * g_walker.sit; /* assis : penché vers le guidon */
        if (lean_ok) {
            /* v1.7.8 : assis, le buste penché comme quand il roule (épaules apprises) : avant, il
               était trop droit, les mains n'atteignaient pas les poignées */
            float dx = ls[0] - lp[0], dy = ls[1] - lp[1], dz = ls[2] - lp[2];
            float fc = dx * fw[0] + dz * fw[2];
            if (dy > 0.2f) {
                /* v1.7.11 : 60 % de la pente apprise (en roulant il est en position d'attaque ;
                   assis à l'arrêt, il était trop recroquevillé sur le guidon) */
                float want = SIT_LEAN * fc / dy - g_walker.stand_lean - 0.55f;
                if (want < -0.3f)
                    want = -0.3f;
                if (want > 1.2f)
                    want = 1.2f;
                g_walker.lean = -0.20f + 0.45f * sinf(PI_F * g_walker.leg_lift) + g_walker.sit * (want + 0.20f);
            }
        }
        /* hauteurs prises sur le sol où marche le pilote (le centre de gravité noté au départ
           est celui de la moto couchée : trop bas d'environ 0,4 m) */
        {
            float g_here = walk_ground_height(&g_rd, sx, sz);
            g_walker.leg_clear = g_here + 1.05f; /* le pied passe au-dessus de l'arrière de la selle */
        }
        ftx = sc[0] - rt[0] * g_gesture.side * 0.40f + fw[0] * 0.10f;
        ftz = sc[1] - rt[2] * g_gesture.side * 0.40f + fw[2] * 0.10f;
        gy = walk_ground_height(&g_rd, ftx, ftz);
        g_walker.leg_target[0] = ftx;
        g_walker.leg_target[1] = gy + g_walker.ankle_height;
        g_walker.leg_target[2] = ftz;
        /* le bassin glisse au milieu de la moto et s'assoit sur la selle */
        /* v0.40.57 : le bassin passe sur la selle PENDANT que la jambe passe (avant : après) */
        lat = 0.42f * smooth01((t - 0.62f) / 0.50f); /* le bassin passe au-dessus de la selle pendant que la jambe redescend */
        g_walker.pos[0] -= rt[0] * g_gesture.side * lat;
        g_walker.pos[1] -= rt[2] * g_gesture.side * lat;
        g_walker.sit = smooth01((t - 0.85f) / 0.45f);
        gy = walk_ground_height(&g_rd, g_walker.pos[0], g_walker.pos[1]);
        stand_y = gy + g_walker.ankle_height + g_walker.hip_height;
        /* selle de cross haute : assis, le bassin est à peine plus bas que debout (pointe des pieds) */
        /* puis les pieds montent sur les repose-pieds et il s'assoit vraiment sur la selle,
           comme quand il roule (le respawn enchaîne sur la pose de pilotage) */
        g_walker.pegs = smooth01((t - 1.35f) / 0.40f);
        {
            int k2;
            for (k2 = 0; k2 < 2; ++k2) {
                /* k2 = 0 gauche du pilote : -r */
                float sg = k2 == 0 ? -1.0f : 1.0f, px2, pz2;
                px2 = sc[0] + fw[0] * 0.12f + r[0] * sg * 0.22f;
                pz2 = sc[1] + fw[2] * 0.12f + r[2] * sg * 0.22f;
                g_walker.peg[k2][0] = px2;
                g_walker.peg[k2][1] = walk_ground_height(&g_rd, px2, pz2) + 0.40f;
                g_walker.peg[k2][2] = pz2;
                if (learned) {
                    /* v1.7 : cheville de ce côté du pilote, comme quand il roule */
                    float d0 = (la[0][0] - sc[0]) * r[0] + (la[0][2] - sc[1]) * r[2];
                    memcpy(g_walker.peg[k2], la[(sg * d0 > 0.0f) ? 0 : 1], 3 * sizeof(float));
                }
            }
        }
        g_walker.sit_y = (stand_y - 0.04f) + ((learned ? lp[1] - SEAT_DOWN : gy + 1.0f) - (stand_y - 0.04f)) * g_walker.pegs;
        {
            /* journal de la montée, 4 fois par seconde */
            static float next_log = 0.0f;
            if (t < 0.02f)
                next_log = 0.0f;
            if (t >= next_log) {
                next_log = t + 0.25f;
                log_line("montee %.2f s : jambe %.2f, assis %.2f, repose-pieds %.2f, bassin y %+.2f (sol), "
                         "pieds y %+.2f / %+.2f, bassin a %.2f m du milieu de la selle, buste %+.2f%s",
                         t, g_walker.leg_lift, g_walker.sit, g_walker.pegs, g_rd.pos[RD_PELVIS][1] - gy,
                         g_rd.pos[RD_FOOT_L][1] - gy, g_rd.pos[RD_FOOT_R][1] - gy,
                         sqrtf((g_rd.pos[RD_PELVIS][0] - sc[0]) * (g_rd.pos[RD_PELVIS][0] - sc[0]) +
                               (g_rd.pos[RD_PELVIS][2] - sc[1]) * (g_rd.pos[RD_PELVIS][2] - sc[1])),
                         g_walker.lean, lean_ok ? " (appris)" : "");
            }
        }
    }
}

/* la moto a été cherchée pendant cette chute (sinon l'adresse est d'une autre chute) */
static int g_body_searched = 0;

/* épreuve en cours : 1 si on a le droit de modifier le jeu (mode Essai hors ligne) */
static int g_event_allowed = 0;  /* mode Essai : le plugin peut toucher à la physique de la moto */
static int g_visual_allowed = 0; /* ragdoll, marche, caméra : seulement l'affichage, aussi en ligne */

/* remonter sur la moto : temps depuis l'appui, nombre de replacements faits */
static float g_mount_t = 0.0f;
static volatile int g_getup_release = 0; /* relevé avec E / Y : la touche doit être relâchée avant de servir à la moto */
static int g_mount_moves = 0;
static int g_respawn_key_down = 0;

/* Touche de respawn du joueur : lue dans son profil (CTRL_RESET de controls.txt, lecture seule).
   Croix ou bouton de la manette : ajoutés à la lecture XInput ; touche du clavier : envoyée. */
static char g_controls_path[700] = "";
static long long g_controls_mtime = -1;
/* changement de vue du jeu (CTRL_CHANGEVIEW du profil) : sert aussi à pied pour la vue 1re / 3e personne */
static unsigned short g_view_pad_game = 0;
static int g_view_vk_game = 0;
static int g_view_kind = -1, g_view_code = 0; /* v1.8.24 : bouton de vue du jeu (pour le presser) */
static int g_view_want = -1;                  /* v1.8.24 : vue choisie à pied, à garder en roulant */
static volatile int g_essai_on = 0;
static unsigned short g_shiftup_mask = 0;     /* v1.8.132 : bouton « vitesse + » du profil (essai de rampe) */
static volatile int g_riding_ok = 0;
static volatile unsigned g_btn_recent = 0; /* v1.8.28 : boutons de la manette appuyés dans la dernière seconde */          /* v1.8.24 : il roule vraiment (pas tombé, pas arrêté) */

static long long file_mtime(const char *path)
{
    struct stat st;
    return path[0] && stat(path, &st) == 0 ? (long long)st.st_mtime : -1;
}

static void load_reset_binding(void)
{
    char path[700], prof[128] = "", line[512];
    FILE *f;
    int found = 0;
    if (!g_user_dir[0])
        return;
    snprintf(path, sizeof(path), "%s\\global.ini", g_user_dir);
    if ((f = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "lastprofile=", 12)) {
                size_t n;
                snprintf(prof, sizeof(prof), "%s", line + 12);
                n = strlen(prof);
                while (n && (prof[n - 1] == '\n' || prof[n - 1] == '\r' || prof[n - 1] == ' '))
                    prof[--n] = 0;
            }
        fclose(f);
    }
    if (!prof[0])
        return;
    snprintf(path, sizeof(path), "%s\\profiles\\%s\\controls.txt", g_user_dir, prof);
    snprintf(g_controls_path, sizeof(g_controls_path), "%s", path);
    g_controls_mtime = file_mtime(path);
    if (!(f = fopen(path, "r")))
        return;
    g_view_pad_game = 0;
    g_view_vk_game = 0;
    while (fgets(line, sizeof(line), f)) {
        char kind[32], guid[80];
        int code = 0;
        if (!strncmp(line, "CTRL_CHANGEVIEW ", 16) && sscanf(line + 16, "%31s", kind) == 1) {
            if (!strcmp(kind, "C_BUTTON") && sscanf(line + 16, "%*s %79s %d", guid, &code) == 2) {
                g_view_pad_game = padinject_game_mask(PAD_RESET_BUTTON, code);
                g_view_kind = PAD_RESET_BUTTON, g_view_code = code;
            } else if (!strcmp(kind, "C_POV") && sscanf(line + 16, "%*s %79s %d", guid, &code) == 2) {
                g_view_pad_game = padinject_game_mask(PAD_RESET_POV, code);
                g_view_kind = PAD_RESET_POV, g_view_code = code;
            } else if (!strcmp(kind, "KEY") && sscanf(line + 16, "%*s %d", &code) == 1 && code > 0) {
                g_view_kind = PAD_RESET_KEY, g_view_code = code;
#ifdef _WIN32
                g_view_vk_game = (int)MapVirtualKeyA((UINT)(code & 0x7F), 1 /* MAPVK_VSC_TO_VK */);
#endif
            }
            log_line("vue : changement de vue du jeu = %s %d (aussi a pied, prioritaire)", kind, code);
            continue;
        }
        if (!strncmp(line, "CTRL_SHIFTUP ", 13) && sscanf(line + 13, "%31s %79s %d", kind, guid, &code) == 3 &&
            !strcmp(kind, "C_BUTTON"))
            g_shiftup_mask = padinject_game_mask(PAD_RESET_BUTTON, code);
        if (found || strncmp(line, "CTRL_RESET ", 11))
            continue;
        if (sscanf(line + 11, "%31s", kind) != 1)
            break;
        found = 1;
        g_reset_keys[0] = 0;
        g_reset_pad = 0;
        if (!strcmp(kind, "KEY") && sscanf(line + 11, "%*s %d", &code) == 1) {
            padinject_set_reset(PAD_RESET_KEY, code);
#ifdef _WIN32
            snprintf(g_reset_keys, sizeof(g_reset_keys), "%s", cfg_key_name((int)MapVirtualKeyA((UINT)(code & 0x7F), 1)));
#endif
            log_line("respawn : touche du clavier %d (profil %s)", code, prof);
        } else if (!strcmp(kind, "C_POV") && sscanf(line + 11, "%*s %79s %d", guid, &code) == 2) {
            padinject_set_reset(PAD_RESET_POV, code);
            g_reset_pad = padinject_game_mask(PAD_RESET_POV, code);
            log_line("respawn : croix de la manette, direction %d (profil %s)", code, prof);
        } else if (!strcmp(kind, "C_BUTTON") && sscanf(line + 11, "%*s %79s %d", guid, &code) == 2) {
            padinject_set_reset(PAD_RESET_BUTTON, code);
            g_reset_pad = padinject_game_mask(PAD_RESET_BUTTON, code);
            {
                static const char *k_names[10] = { "Start", "Back", "clic gauche", "clic droit", "LB", "RB", "A", "B", "X", "Y" };
                log_line("respawn : bouton %d de la manette = %s si XInput (profil %s, manette %s)", code,
                         code >= 0 && code < 10 ? k_names[code] : "?", prof, guid);
            }
        } else if ((!strcmp(kind, "BUTTON") || !strcmp(kind, "POV")) &&
                   ((sscanf(line + 11, "%*s %79s %d", guid, &code) == 2 && strchr(guid, '-')) ||
                    (snprintf(guid, sizeof(guid), "?"), sscanf(line + 11, "%*s %d", &code) == 1))) {
            /* v1.8.203 : manette DirectInput (pas XInput) : bloquée et rejouée par la lecture DirectInput */
            padinject_set_reset(!strcmp(kind, "BUTTON") ? PAD_RESET_DIBUTTON : PAD_RESET_DIPOV, code);
            log_line("respawn : %s %d d'une manette DirectInput (profil %s, manette %s)",
                     !strcmp(kind, "BUTTON") ? "bouton" : "croix, direction", code, prof, guid);
        } else {
            padinject_set_reset(PAD_RESET_POV, 0);
            log_line("respawn : touche \"%s\" du profil non geree, croix vers le haut", kind);
        }
    }
    fclose(f);
    if (!found) {
        padinject_set_reset(PAD_RESET_POV, 0);
        log_line("respawn : pas de touche de respawn dans le profil %s, croix vers le haut", prof);
    }
}

/* le jeu enregistre les touches en sortant des paramètres : on relit le fichier s'il a changé */
static void reset_binding_poll(void)
{
    static double last = 0.0;
    double now = now_seconds();
    if (now - last < 1.0)
        return;
    last = now;
    if (!g_controls_path[0] || file_mtime(g_controls_path) != g_controls_mtime) {
        if (g_controls_path[0])
            log_line("respawn : touches du profil modifiees, relues");
        load_reset_binding();
    }
}

static void press_respawn_key(int down)
{
    if (!down && g_respawn_key_down) {
        long di, x, fake;
        padinject_inject_counts(&di, &x, &fake);
        log_line("respawn : appui vu par le jeu : DirectInput %ld, XInput %ld, manette simulee %ld lecture(s)", di, x, fake);
    } else if (down) {
        long a, b, c;
        padinject_inject_counts(&a, &b, &c);
    }
    padinject_pov_up(down);
    g_respawn_key_down = down;
}

/* relever la moto : temps depuis le début du geste, pilote immobile pendant le geste */
static float g_lift_t = 0.0f;
static volatile double g_lift_wall = 0.0; /* v1.8.23 : heure du dernier pas du geste (100 Hz) */
static volatile float g_lift_lp = 0.0f;   /* avancement du geste à ce pas */
static volatile int g_bike_lock = 0;

/* 0 rien, 1 surveillance en cours, 2 fini */
static volatile int g_watch_phase = 0;

/* notre caméra en matrice caméra -> monde, lignes 3x4 (format du jeu) */
static float g_cam_rows[12];
static volatile int g_cam_rows_ok = 0;
/* statistiques : le jeu a-t-il repris notre caméra ? */
static volatile int g_cam_stat_used = 0, g_cam_stat_frames = 0, g_cam_stat_writes = 0, g_cam_stat_bad = 0;

/* Écrit notre caméra dans celle du jeu (pour qu'il dessine et trie les objets depuis elle) */
static void game_camera_write(int where)
{
    uintptr_t a = mem_camera_candidate();
    float m[17];
    (void)where;
    if (g_watch_phase != 3)
        return; /* v0.9.6 : remplacé par l'interception du calcul (camhook) */
    if (!a || !g_cam_rows_ok || !g_cfg.walk_camera || g_drive_state != DRIVE_ACTIVE ||
        g_walker.state == WALK_OFF)
        return;
    if (!safe_read((const void *)a, m, sizeof(m)) || !camera_matrix_ok(m)) {
        ++g_cam_stat_bad;
        return;
    }
    if (safe_write((void *)a, g_cam_rows, sizeof(g_cam_rows)))
        ++g_cam_stat_writes;
}

/* Juste après le calcul de la caméra par le jeu : on la remplace par la nôtre */
static volatile int g_camhook_logged = 0;
static volatile long g_camhook_writes = 0;

/* descente : tant que le jeu n'est pas en chute, il ne reprend pas notre caméra (vue lointaine) :
   on garde la sienne, puis notre caméra 3e personne glisse depuis elle dès la chute */
/* v1.8.7 : notre caméra suit le corps : à pied, renversé, ou en chute si on roulait en 1re personne */
static int cam_body(void)
{
    /* v1.8.11 : aussi en chute en 3e personne (pilote au centre, la moto au fond) */
    return 1;
}

static int cam_wait_crash(void)
{
    return g_dismounted && dismount_crash_on() && g_vol_tip != 3;
}

static void cam_matmul_cb(const float *a, const float *b, float *out)
{
    int active = g_cfg.walk_camera && g_drive_state == DRIVE_ACTIVE && cam_body() && !cam_wait_crash() &&
                 g_draw_state == 0 && !g_live_suspended;
    if (active && g_camhook_logged < 1) {
        g_camhook_logged = 1;
        log_line("camera : calcul du jeu A [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                 a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
        log_line("camera : calcul du jeu B [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        log_line("camera : resultat [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                 out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7], out[8], out[9], out[10], out[11],
                 out[12], out[13], out[14], out[15]);
    }
    if (active && g_cam_rows_ok) {
        memcpy(out, g_cam_rows, sizeof(g_cam_rows));
        ++g_camhook_writes;
        /* v1.8.166 : à pied en 1re personne : champ de vision donné au jeu lui-même (juste après la matrice,
           +0x40 : 75 degrés, mesurés sur un écran 4:3), pour que ses ombres et ce qu'il dessine suivent
           (1.8.165 : élargi après coup dans la perspective : ombres fausses sur les bords) */
        if (g_first_person && g_foot_fov_want > 20.0f && g_foot_fov_want < 170.0f) {
            float asp = glcam_aspect(), h = g_foot_fov_want * 0.5f * 0.01745329f;
            float f43 = 2.0f * atanf(tanf(h) * (4.0f / 3.0f) / (asp > 0.5f ? asp : 1.7778f)) * 57.29578f;
            if (f43 > 20.0f && f43 < 170.0f) {
                static float s_last = 0.0f;
                if (fabsf(s_last - f43) > 0.5f) {
                    log_line("champ de vision a pied : jeu %.1f -> %.1f (4:3), voulu %.1f horizontal, ecran %.3f",
                             out[16], f43, g_foot_fov_want, asp);
                    s_last = f43;
                }
                out[16] = f43;
            }
        }
    }
}

/* v1.8.31 : casque de notre pilote (le plus proche de la moto), position dessinée */
typedef struct {
    float ref[3], best, pos[3];
} HelmetFind;
static void helmet_find_cb(const char *path, void *model, void *user)
{
    HelmetFind *h = (HelmetFind *)user;
    float M[12], d;
    if (!strstr(path, "\\helmets\\") || strstr(path, "c_helmet") || !skelscan_model_world(model, M))
        return;
    d = (M[3] - h->ref[0]) * (M[3] - h->ref[0]) + (M[7] - h->ref[1]) * (M[7] - h->ref[1]) + (M[11] - h->ref[2]) * (M[11] - h->ref[2]);
    if (d < h->best) {
        h->best = d;
        h->pos[0] = M[3], h->pos[1] = M[7], h->pos[2] = M[11];
    }
}

static int walk_camera_fn(const float old[16], float out[16])
{
    float fwd[3], target[3], look[3], f[3], r[3], u[3], l, gy, k, kl, dt;
    double now;
    int i;
    if (g_drive_state != DRIVE_ACTIVE && !g_dismounted && g_draw_state == 0 && g_riding_ok) {
        /* v1.8.6 : il roule : la caméra du jeu est-elle dans la tête du pilote ou derrière la moto ?
           v1.8.24 : seulement quand il roule vraiment (pas tombé) : à la chute, la caméra de chute
           du jeu, près du pilote, passait pour la vue 1re personne */
        float gp[3], dx, dy, dz, al = 0.0f, la = 0.0f, upo = 0.0f;
        int have_ms = 0;
        for (i = 0; i < 3; ++i)
            gp[i] = -(old[i * 4 + 0] * old[12] + old[i * 4 + 1] * old[13] + old[i * 4 + 2] * old[14]);
        {
            /* v1.8.8 : guidon de la moto tel qu'il est dessiné à cette image (la position de la
               télémétrie est en retard : à 60 km/h la caméra casque semblait à 1 m de la moto) */
            float Ms[12];
            char sp[300];
            if (ridepose_find_steer(g_tele_pos, Ms, sp, sizeof(sp))) {
                dx = gp[0] - Ms[3];
                dy = gp[1] - Ms[7] + 0.6f; /* guidon ~0,6 m plus haut que le point de la télémétrie */
                dz = gp[2] - Ms[11];
                {
                    /* v1.8.30 : place de la caméra dans le repère de la moto (devant, côté, haut) */
                    float fx = Ms[2], fz = Ms[10], fl = sqrtf(fx * fx + fz * fz);
                    if (fl > 0.3f) {
                        fx /= fl;
                        fz /= fl;
                        al = dx * fx + dz * fz;
                        la = dx * fz - dz * fx;
                        upo = gp[1] - Ms[7];
                        have_ms = 1;
                    }
                }
            } else {
                dx = gp[0] - g_tele_pos[0];
                dy = gp[1] - g_tele_pos[1];
                dz = gp[2] - g_tele_pos[2];
            }
        }
        if (isfinite(dx) && isfinite(dy) && isfinite(dz)) {
            /* la vue doit tenir 1 s : à la chute, le jeu passe à sa caméra de chute juste avant nous */
            static int cand = -1;
            static double cand_t = 0.0;
            /* v1.8.11 : caméra à moins de 1,5 m du guidon = vue de pilote (casque, guidon) */
            int fp = dx * dx + (dy - 0.6f) * (dy - 0.6f) + dz * dz < 1.5f * 1.5f;
            /* v1.8.30 : 1re personne = caméra là où est la tête du pilote (derrière et au-dessus
               du guidon). Les caméras posées sur la moto (garde-boue, guidon, arrière) sont aussi
               à moins de 1,5 m du guidon et passaient pour la 1re personne */
            float dh = -1.0f;
            {
                /* v1.8.31 : 1re personne = caméra dans le casque du pilote (à moins de 35 cm de son
                   centre). La 1.8.30 devinait la place de la tête : ta vue casque n'y était pas */
                HelmetFind hf;
                hf.ref[0] = g_tele_pos[0], hf.ref[1] = g_tele_pos[1] + 1.0f, hf.ref[2] = g_tele_pos[2];
                hf.best = 2.5f * 2.5f;
                equip_each_gear(helmet_find_cb, &hf);
                if (hf.best < 2.5f * 2.5f)
                    dh = sqrtf((gp[0] - hf.pos[0]) * (gp[0] - hf.pos[0]) + (gp[1] - hf.pos[1]) * (gp[1] - hf.pos[1]) +
                               (gp[2] - hf.pos[2]) * (gp[2] - hf.pos[2]));
            }
            if (dh >= 0.0f)
                fp = dh < 0.65f; /* v1.8.39 : vue dynamique : la caméra flotte jusqu'à ~50 cm du casque */
            else if (have_ms)
                fp = al > -0.6f && al < 0.25f && upo > 0.2f && upo < 0.9f && fabsf(la) < 0.6f; /* mesuré en vue casque (18/11) */
            {
                static double rep_t = 0.0;
                if (now_seconds() - rep_t > 10.0) {
                    rep_t = now_seconds();
                    log_line("camera : en roulant, camera a %.2f m du casque (%.2f devant le guidon, %.2f cote, %.2f haut) -> %s",
                             dh, al, la, upo, fp ? "1re" : "3e");
                }
            }
            if (!fp && g_game_fp == 0) {
                /* vue extérieure : on retient sa distance et sa hauteur (moyenne glissante) */
                float hd = sqrtf(dx * dx + dz * dz), hh = dy - 1.0f + 0.35f; /* hauteur / bassin debout */
                if (hd > 1.2f && hd < 12.0f && hh > -0.5f && hh < 5.0f) {
                    if (!g_gcam_ok) {
                        g_gcam_d = hd;
                        g_gcam_h = hh;
                        g_gcam_ok = 1;
                        log_line("camera : vue exterieure du jeu a %.1f m, %.1f m de haut (reprise a pied)", hd, dy);
                    }
                    g_gcam_d += (hd - g_gcam_d) * 0.02f;
                    g_gcam_h += (hh - g_gcam_h) * 0.02f;
                }
            }
            /* v1.8.32 : vote sur 0,5 s au lieu de « 0,5 s sans un seul écart » : une image sur
               quelques-unes sans casque trouvé suffisait à bloquer le passage en 1re personne */
            static float score = -1.0f;
            static double sc_t = 0.0;
            double tn2 = now_seconds();
            float sdt2 = (float)(tn2 - sc_t);
            int want_fp;
            if (score < 0.0f || sdt2 > 1.0f || sdt2 < 0.0f)
                score = (float)fp;
            else
                score += ((float)fp - score) * (1.0f - expf(-5.0f * sdt2));
            sc_t = tn2;
            want_fp = g_game_fp == 1 ? (score > 0.3f) : (score > 0.7f);
            (void)cand;
            (void)cand_t;
            fp = want_fp;
            if (fp != g_game_fp) {
                g_game_fp_prev = g_game_fp;
                g_game_fp_t = now_seconds();
                /* v1.8.117 : touche de vue appuyée juste avant : c'est le joueur, pas la caméra de chute */
                g_game_fp_player = now_seconds() - g_view_key_t < 1.5;
                g_game_fp = fp;
                log_line("camera : vue du jeu en roulant : %s (boutons manette juste avant : %04x ; camera %.2f m devant le "
                         "guidon, %.2f m sur le cote, %.2f m au-dessus%s)",
                         fp ? "pilote (1re personne)" : "exterieure (3e personne)", g_btn_recent, al, la, upo,
                         have_ms ? "" : ", guidon pas vu");
            }
        }
    }
    if (!g_cfg.walk_camera || g_drive_state != DRIVE_ACTIVE || !cam_body() || cam_wait_crash() ||
        g_draw_state != 0 || g_live_suspended) {
        g_cam_on = 0;
        g_cam_rows_ok = 0;
        g_cam_yaw_ok = 0;
        return 0;
    }
    now = now_seconds();
    g_foot_cam_t = now;
    {
        /* v1.8.162 : molette : zoom / dézoom à pied */
        LONG w = InterlockedExchange(&g_wheel_acc, 0);
        if (w) {
            float steps = (float)w / 120.0f;
            /* v1.8.163 : en 1re personne, pas de dézoom de caméra ;
               v1.8.167 : 3 niveaux de zoom (molette avant : zoome, arrière : revient) */
            if (!g_first_person)
                g_zoom_dist *= powf(0.85f, steps);
            else if (steps > 0.0f)
                fp_zoom_next(); /* v1.8.168 : molette avant : cran suivant, après le 3e retour normal */
            else if (g_fp_zoom > 0)
                g_fp_zoom = g_fp_zoom - 1;
        }
        {
            /* v1.8.163 : croix de la manette (Haut : rapproche, Bas tenue : éloigne) */
            static double pt = 0.0;
            double pdt = now - pt;
            pt = now;
            if (pdt > 0.1)
                pdt = 0.1;
            static int prev = 0;
            if (g_pad_zoom && !g_first_person)
                g_zoom_dist *= powf(2.0f, (float)g_pad_zoom * (float)pdt);
            /* v1.8.167 : en 1re personne, un cran par appui (Haut : zoome, Bas tenue : revient) */
            if (g_first_person && g_pad_zoom != prev && g_pad_zoom) {
                if (g_pad_zoom < 0)
                    fp_zoom_next(); /* v1.8.168 : Haut : cran suivant, après le 3e retour normal */
                else if (g_fp_zoom > 0)
                    g_fp_zoom = g_fp_zoom - 1;
            }
            prev = g_pad_zoom;
        }
        if (InterlockedExchange(&g_mclick_acc, 0) && g_first_person)
            fp_zoom_next();
        if (g_zoom_dist < 0.35f) g_zoom_dist = 0.35f;
        if (g_zoom_dist > 3.0f) g_zoom_dist = 3.0f;
    }
    if (g_cam_on) {
        /* la vue du jeu est-elle déjà la nôtre ? */
        float gp[3], d = 0.0f;
        for (i = 0; i < 3; ++i) {
            gp[i] = -(old[i * 4 + 0] * old[12] + old[i * 4 + 1] * old[13] + old[i * 4 + 2] * old[14]);
            d += (gp[i] - g_cam_pos[i]) * (gp[i] - g_cam_pos[i]);
        }
        ++g_cam_stat_frames;
        if (d < 0.05f * 0.05f)
            ++g_cam_stat_used;
    }
    if (!g_cam_on) {
        /* départ : on part de la caméra du jeu, puis on glisse vers le pilote */
        for (i = 0; i < 3; ++i) {
            g_cam_pos[i] = -(old[i * 4 + 0] * old[12] + old[i * 4 + 1] * old[13] + old[i * 4 + 2] * old[14]);
            g_cam_look[i] = g_cam_pos[i] + 5.0f * old[i * 4 + 2];
        }
        g_cam_on = 1;
        g_cam_last = now;
        g_cam_yaw = g_walker.heading; /* derrière le pilote au départ */
        if (g_walker.state == WALK_OFF && !g_knocked && g_tele_R[2] * g_tele_R[2] + g_tele_R[8] * g_tele_R[8] > 0.09f)
            g_cam_yaw = atan2f(g_tele_R[2], g_tele_R[8]); /* v1.8.7 : chute : on regarde où allait la moto */
        g_cam_pitch = atan2f(cam_height() - 0.35f, cam_dist());
        g_cam_age = 0.0f;
        g_cam_yaw_ok = 1;
    }
    dt = (float)(now - g_cam_last);
    g_cam_last = now;
    if (dt < 0.0f)
        dt = 0.0f;
    if (dt > 0.1f)
        dt = 0.1f;

    g_cam_age += dt;
#ifdef _WIN32
    /* caméra libre : souris (le curseur est ramené au centre de la fenêtre) et stick droit */
    {
        static int mouse_was = 0;
        HWND hw = GetForegroundWindow();
        static int esc_prev = 0;
        int esc = game_focused() && key_down(VK_ESCAPE);
        int grab;
        if (esc && !esc_prev && !equip_menu_open())
            g_mouse_free = !g_mouse_free; /* Échap : menu du jeu (en ligne, le jeu ne se met pas en pause) */
        esc_prev = esc;
        if (g_mouse_free && input_ok() && (bind_key(g_cfg.key_forward) || bind_key(g_cfg.key_back) ||
                                           bind_key(g_cfg.key_left) || bind_key(g_cfg.key_right)))
            g_mouse_free = 0; /* il remarche : la caméra reprend la souris */
        /* v0.40.25 : clavier prioritaire (F7) = la caméra prend la souris ; sinon souris libre */
        {
            /* v0.40.42 : à la manette aussi, il remarche (stick gauche) : la caméra reprend */
            PadState mps;
            if (g_mouse_free && input_ok() && padinject_read(&mps) && mps.lx * mps.lx + mps.ly * mps.ly > 0.25f)
                g_mouse_free = 0;
        }
        /* v1.8.46 : roue des cônes ouverte : la souris vise dans la roue au lieu de tourner la caméra */
        grab = g_running && !g_mouse_free && g_kbd_prio && (input_ok() || (g_cone_menu && game_focused())) && hw;
        /* curseur caché à pied même sans clavier prioritaire (à la manette, il restait au milieu) */
        if (g_running && !g_mouse_free && (input_ok() || (g_cone_menu && game_focused())) && hw)
            cursor_hide(hw);
        {
            /* v0.40.53 : diagnostic du curseur toutes les 5 s à pied */
            static DWORD last = 0;
            DWORD now = GetTickCount();
            if (now - last > 5000) {
                CURSORINFO ci;
                last = now;
                memset(&ci, 0, sizeof(ci));
                ci.cbSize = sizeof(ci);
                GetCursorInfo(&ci);
                log_line("souris : curseur %s (en marche %d, souris libre %d, entree ok %d, fenetre %d, cache %d/%d)",
                         (ci.flags & CURSOR_SHOWING) ? "VISIBLE" : "cache", g_running, g_mouse_free, input_ok(), hw != NULL,
                         (int)g_cur_hidden, g_cur_wnd_hidden);
            }
        }
        if (!(g_running && !g_mouse_free && (input_ok() || (g_cone_menu && game_focused())) && hw))
            cursor_restore();
        if (grab) {
            RECT rc;
            POINT c, p;
            if (GetClientRect(hw, &rc)) {
                c.x = (rc.right - rc.left) / 2;
                c.y = (rc.bottom - rc.top) / 2;
                ClientToScreen(hw, &c);
                if (mouse_was && GetCursorPos(&p)) {
                    if (g_cone_menu) {
                        float wx = g_wheel_px + (float)(p.x - c.x), wy = g_wheel_py + (float)(p.y - c.y),
                              l = sqrtf(wx * wx + wy * wy);
                        if (l > 120.0f) {
                            wx *= 120.0f / l;
                            wy *= 120.0f / l;
                        }
                        g_wheel_px = wx;
                        g_wheel_py = wy;
                    } else {
                        /* v1.8.171 : zoomé, la caméra tourne moins vite (autant que le zoom) */
                        float zs = g_first_person && g_fp_zoom_cur > 1.0f ? 1.0f / g_fp_zoom_cur : 1.0f;
                        g_cam_yaw += (float)(p.x - c.x) * g_cfg.mouse_sensitivity * zs;
                        g_cam_pitch += (float)(p.y - c.y) * g_cfg.mouse_sensitivity * zs;
                    }
                }
                SetCursorPos(c.x, c.y);
                /* curseur caché tant que la caméra prend la souris (v0.40.24 : en ligne, après le
                   menu Échap, le jeu le laissait affiché au milieu de l'écran) */
                SetCursor(NULL);
            }
        }
        mouse_was = grab;
    }
#endif
    {
        PadState ps;
        if (!g_cone_menu && padinject_read(&ps)) {
            float rx = fabsf(ps.rx) > 0.2f ? (ps.rx - (ps.rx > 0 ? 0.2f : -0.2f)) / 0.8f : 0.0f;
            float ry = fabsf(ps.ry) > 0.2f ? (ps.ry - (ps.ry > 0 ? 0.2f : -0.2f)) / 0.8f : 0.0f;
            float zs = g_first_person && g_fp_zoom_cur > 1.0f ? 1.0f / g_fp_zoom_cur : 1.0f; /* v1.8.171 */
            g_cam_yaw += rx * 3.0f * dt * zs;
            g_cam_pitch -= ry * 2.0f * dt * zs;
        }
    }
    if (g_walker.state == WALK_OFF && !g_first_person) {
        /* v1.8.11 : au sol en 3e personne : derrière le pilote, en regardant vers la moto
           (le pilote au centre de l'image, la moto au fond) */
        float bx = g_tele_pos[0] - g_rd.pos[RD_PELVIS][0], bz = g_tele_pos[2] - g_rd.pos[RD_PELVIS][2];
        if (bx * bx + bz * bz > 0.8f * 0.8f) {
            float want = atan2f(bx, bz), dy = want - g_cam_yaw;
            while (dy > 3.14159265f)
                dy -= 6.2831853f;
            while (dy < -3.14159265f)
                dy += 6.2831853f;
            g_cam_yaw += dy * (1.0f - expf(-4.0f * dt));
        }
        g_cam_pitch += (atan2f(cam_height() - 0.35f, cam_dist()) - g_cam_pitch) * (1.0f - expf(-3.0f * dt));
    }
    if (g_cam_pitch > (g_first_person ? 1.35f : 1.2f))
        g_cam_pitch = g_first_person ? 1.35f : 1.2f;
    if (g_cam_pitch < (g_first_person ? -1.2f : -0.3f))
        g_cam_pitch = g_first_person ? -1.2f : -0.3f;
    while (g_cam_yaw > 3.14159265f)
        g_cam_yaw -= 6.2831853f;
    while (g_cam_yaw < -3.14159265f)
        g_cam_yaw += 6.2831853f;

    {
        /* orbite : la caméra regarde un point au-dessus du bassin, à distance fixe */
        float dist = sqrtf(cam_dist() * cam_dist() + (cam_height() - 0.35f) * (cam_height() - 0.35f));
        float cp = cosf(g_cam_pitch);
        fwd[0] = sinf(g_cam_yaw) * cp;
        fwd[1] = -sinf(g_cam_pitch);
        fwd[2] = cosf(g_cam_yaw) * cp;
        for (i = 0; i < 3; ++i)
            look[i] = g_rd.pos[RD_PELVIS][i];
        look[1] += 0.35f;
        for (i = 0; i < 3; ++i)
            target[i] = look[i] - fwd[i] * dist;
    }
    if (ground_fn(target[0], target[2], &gy) && target[1] < gy + 0.3f)
        target[1] = gy + 0.3f;

    if (g_first_person) {
        /* 1re personne : on ne regarde pas derrière soi ; au-delà de ce que le cou permet,
           c'est le corps qui tourne */
        float dy = g_cam_yaw - g_walker.heading;
        while (dy > 3.14159265f)
            dy -= 6.2831853f;
        while (dy < -3.14159265f)
            dy += 6.2831853f;
        if (dy > HEAD_YAW_MAX)
            g_walker.heading = g_cam_yaw - HEAD_YAW_MAX;
        else if (dy < -HEAD_YAW_MAX)
            g_walker.heading = g_cam_yaw + HEAD_YAW_MAX;
    }
    if (g_first_person) {
        /* 1re personne : les yeux, un peu devant la tête (pour ne pas voir l'intérieur du casque) */
        const float *h = g_rd.pos[RD_HEAD];
        float hx = sinf(g_cam_yaw), hz = cosf(g_cam_yaw);
        g_cam_pos[0] = h[0] + hx * 0.17f;
        g_cam_pos[1] = h[1] + 0.04f;
        g_cam_pos[2] = h[2] + hz * 0.17f;
        for (i = 0; i < 3; ++i) {
            target[i] = g_cam_pos[i];
            look[i] = g_cam_pos[i] + fwd[i] * 5.0f;
            g_cam_look[i] = look[i];
        }
        g_cam_age = 2.0f;
    }
    /* au départ on glisse depuis la caméra du jeu, ensuite la caméra suit de près */
    k = 1.0f - expf(-(g_cam_age < 1.0f ? 3.0f : 14.0f) * dt);
    kl = 1.0f - expf(-8.0f * dt);
    for (i = 0; i < 3; ++i) {
        g_cam_pos[i] += (target[i] - g_cam_pos[i]) * k;
        g_cam_look[i] += (look[i] - g_cam_look[i]) * kl;
    }

    /* v1.8.9 : chute en 1re personne : la vue est attachée au corps (épaules et dos), elle
       tourne avec lui comme si on avait la tête dans le casque */
    {
        static int body_on = 0;
        static float sgn = 1.0f, bf[3], bu[3];
        static int free_look = 0;
        static double slow_t = 0.0;
        if (g_first_person && g_walker.state == WALK_OFF) {
            /* v1.8.12 : corps presque arrêté au sol : on ne reste pas le nez dans la terre ou les
               yeux au ciel ; la vue se libère (souris / stick), un peu au-dessus de la tête */
            float spd = rd_max_speed(&g_rd);
            /* v1.8.14 : plus de vue libre au sol : la caméra reste dans les yeux du pilote */
            (void)spd;
            if (1) {
                free_look = 0;
                slow_t = now;
            } else if (!free_look && now - slow_t > 0.6) {
                float hx = bf[0], hz = bf[2], hl = sqrtf(hx * hx + hz * hz);
                free_look = 1;
                if (body_on && hl > 0.2f)
                    g_cam_yaw = atan2f(hx, hz);
                g_cam_pitch = 0.2f;
            }
            g_cam_free_look = free_look;
            if (free_look) {
                const float *H = g_rd.pos[RD_HEAD];
                float cp = cosf(g_cam_pitch), gy2;
                f[0] = sinf(g_cam_yaw) * cp;
                f[1] = -sinf(g_cam_pitch);
                f[2] = cosf(g_cam_yaw) * cp;
                r[0] = f[2];
                r[1] = 0.0f;
                r[2] = -f[0];
                l = sqrtf(r[0] * r[0] + r[2] * r[2]);
                if (l > 1e-3f) {
                    r[0] /= l;
                    r[2] /= l;
                    u[0] = f[1] * r[2] - f[2] * r[1];
                    u[1] = f[2] * r[0] - f[0] * r[2];
                    u[2] = f[0] * r[1] - f[1] * r[0];
                    g_cam_pos[0] = H[0];
                    g_cam_pos[1] = H[1] + 0.25f;
                    g_cam_pos[2] = H[2];
                    if (ground_walk_fn(g_cam_pos[0], g_cam_pos[2], &gy2) && g_cam_pos[1] < gy2 + 0.45f)
                        g_cam_pos[1] = gy2 + 0.45f;
                    body_on = 1;
                    goto cam_out;
                }
            }
        } else {
            free_look = 0;
            g_cam_free_look = 0;
            slow_t = now;
        }
        if (g_first_person && g_walker.state == WALK_OFF) {
            const float *P = g_rd.pos[RD_PELVIS], *N = g_rd.pos[RD_NECK], *SL = g_rd.pos[RD_SHOULDER_L],
                        *SR = g_rd.pos[RD_SHOULDER_R], *H = g_rd.pos[RD_HEAD];
            float up[3], sd[3], ff[3], lu, ls, lf, kb;
            for (i = 0; i < 3; ++i) {
                up[i] = (H[i] - N[i]) * 0.6f + (N[i] - P[i]) * 0.4f; /* v1.8.14 : surtout le cou (la tête) */
                sd[i] = SR[i] - SL[i];
            }
            lu = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
            ls = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
            if (lu > 1e-3f && ls > 1e-3f) {
                for (i = 0; i < 3; ++i) {
                    up[i] /= lu;
                    sd[i] /= ls;
                }
                /* avant = droite x haut */
                ff[0] = sd[1] * up[2] - sd[2] * up[1];
                ff[1] = sd[2] * up[0] - sd[0] * up[2];
                ff[2] = sd[0] * up[1] - sd[1] * up[0];
                if (!body_on) {
                    /* v1.8.13 : l'avant du corps = épaule droite x dos (fixe pour un corps humain), à
                       l'envers si le squelette est en miroir. Avant : pris sur le sens de la moto au
                       début de la chute, faux quand le pilote était déjà retourné (oeil dans le dos) */
                    sgn = g_walker.mirror ? -1.0f : 1.0f;
                    memcpy(bf, ff, sizeof(bf));
                    for (i = 0; i < 3; ++i)
                        bf[i] *= sgn;
                    memcpy(bu, up, sizeof(bu));
                    body_on = 1;
                    log_line("camera : chute en 1re personne, vue attachee au corps");
                }
                kb = 1.0f - expf(-18.0f * dt); /* un peu lissée : pas de tremblements */
                for (i = 0; i < 3; ++i) {
                    bf[i] += (ff[i] * sgn - bf[i]) * kb;
                    bu[i] += (up[i] - bu[i]) * kb;
                }
                lf = sqrtf(bf[0] * bf[0] + bf[1] * bf[1] + bf[2] * bf[2]);
                if (lf > 1e-3f) {
                    for (i = 0; i < 3; ++i)
                        f[i] = bf[i] / lf;
                    /* r = haut x avant, u = avant x r */
                    r[0] = bu[1] * f[2] - bu[2] * f[1];
                    r[1] = bu[2] * f[0] - bu[0] * f[2];
                    r[2] = bu[0] * f[1] - bu[1] * f[0];
                    l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                    if (l > 1e-3f) {
                        for (i = 0; i < 3; ++i)
                            r[i] /= l;
                        u[0] = f[1] * r[2] - f[2] * r[1];
                        u[1] = f[2] * r[0] - f[0] * r[2];
                        u[2] = f[0] * r[1] - f[1] * r[0];
                        {
                            /* yeux devant la tête, mais jamais dans la terre : visage contre le sol,
                               on recule l'oeil et on le garde 22 cm au-dessus du sol */
                            /* v1.8.14 : les yeux, dans le casque (8 cm devant le centre de la tête,
                               4 cm plus haut) ; contre le sol on reste au centre de la tête (jamais
                               au-dessus d'elle : on voyait ses cheveux) */
                            float gy2, gmax = -1e9f, sx, sz;
                            int s3;
                            for (i = 0; i < 3; ++i)
                                g_cam_pos[i] = H[i] + f[i] * 0.08f + u[i] * 0.04f;
                            /* v1.8.18 : rendu blanc = caméra sous la surface affichée de la piste. On
                               garde l'œil 30 cm au-dessus du sol le plus haut autour de lui (bosses,
                               terre de la déformation) et du point qu'il regarde de près */
                            for (s3 = 0; s3 < 5; ++s3) {
                                sx = g_cam_pos[0] + (s3 == 1 ? 0.25f : s3 == 2 ? -0.25f : 0.0f) + (s3 == 0 ? f[0] * 0.3f : 0.0f);
                                sz = g_cam_pos[2] + (s3 == 3 ? 0.25f : s3 == 4 ? -0.25f : 0.0f) + (s3 == 0 ? f[2] * 0.3f : 0.0f);
                                if (ground_walk_fn(sx, sz, &gy2) && gy2 > gmax)
                                    gmax = gy2;
                            }
                            if (ground_walk_fn(g_cam_pos[0], g_cam_pos[2], &gy2) && gy2 > gmax)
                                gmax = gy2;
                            if (gmax > -1e8f && g_cam_pos[1] < gmax + 0.12f) {
                                /* d'abord sans l'avancer devant la tête, puis on le monte.
                                   v1.8.42 : 12 cm au-dessus du sol (au lieu de 30) et jamais plus de
                                   9 cm au-dessus du centre de la tête : monté plus haut, l'œil sortait
                                   de la tête pendant la chute et on voyait les cheveux et le dos */
                                float top = H[1] + 0.09f;
                                for (i = 0; i < 3; ++i)
                                    g_cam_pos[i] = H[i] + u[i] * 0.04f;
                                if (g_cam_pos[1] < gmax + 0.12f)
                                    g_cam_pos[1] = gmax + 0.12f;
                                if (g_cam_pos[1] > top && top > gmax + 0.03f)
                                    g_cam_pos[1] = top;
                                /* visage contre le sol : on ne regarde pas la terre à 10 cm, la vue
                                   remonte vers l'horizon */
                                if (f[1] < -0.5f) {
                                    float k3 = (-0.5f - f[1]) / 0.5f;
                                    f[1] += (-0.5f - f[1]) * (k3 > 1.0f ? 1.0f : k3);
                                    l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                                    for (i = 0; i < 3; ++i)
                                        f[i] /= l;
                                    r[0] = bu[1] * f[2] - bu[2] * f[1];
                                    r[1] = bu[2] * f[0] - bu[0] * f[2];
                                    r[2] = bu[0] * f[1] - bu[1] * f[0];
                                    l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                                    if (l > 1e-3f)
                                        for (i = 0; i < 3; ++i)
                                            r[i] /= l;
                                    u[0] = f[1] * r[2] - f[2] * r[1];
                                    u[1] = f[2] * r[0] - f[0] * r[2];
                                    u[2] = f[0] * r[1] - f[1] * r[0];
                                }
                            }
                        }
                        goto cam_out;
                    }
                }
            }
        } else {
            body_on = 0;
        }
    }
    /* axes : avant f, droite r = haut x f, haut u = f x r */
    for (i = 0; i < 3; ++i)
        f[i] = g_cam_look[i] - g_cam_pos[i];
    l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (l < 1e-3f)
        return 0;
    for (i = 0; i < 3; ++i)
        f[i] /= l;
    r[0] = f[2];
    r[1] = 0.0f;
    r[2] = -f[0];
    l = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (l < 1e-3f)
        return 0;
    r[0] /= l;
    r[2] /= l;
    u[0] = f[1] * r[2] - f[2] * r[1];
    u[1] = f[2] * r[0] - f[0] * r[2];
    u[2] = f[0] * r[1] - f[1] * r[0];

cam_out:
    if (g_first_person) {
        /* v1.8.21 : 1re personne à pied ou au sol : l'œil suit la tête lissé. La tête du pilote
           bouge d'une image à l'autre pendant les gestes (s'accroupir, saisir, relever, monter) :
           la vue tremblait, et la moto avec elle à l'écran (clip du 18/11 17:09) */
        /* v1.8.41 : on lisse la place de l'œil PAR RAPPORT AU BASSIN, pas sa place dans le monde :
           en courant (5-6 m/s), le lissage laissait la caméra 25 cm derrière la tête (on voyait
           les cheveux et le dos) */
        static float sm[3];
        static double sm_t = 0.0;
        float rel[3], d2 = 0.0f, ks, sdt = (float)(now - sm_t);
        const float *pv = g_rd.pos[RD_PELVIS];
        for (i = 0; i < 3; ++i)
            rel[i] = g_cam_pos[i] - pv[i];
        for (i = 0; i < 3; ++i)
            d2 += (rel[i] - sm[i]) * (rel[i] - sm[i]);
        if (sm_t == 0.0 || sdt > 0.25f || sdt < 0.0f || d2 > 0.5f * 0.5f)
            memcpy(sm, rel, sizeof(sm));
        else {
            ks = 1.0f - expf(-((bike_lift_active() || g_gesture.on || g_mount_t > 0.0f) ? 9.0f : 22.0f) * sdt);
            for (i = 0; i < 3; ++i)
                sm[i] += (rel[i] - sm[i]) * ks;
        }
        sm_t = now;
        {
            /* diagnostic : la tête fait-elle des allers-retours d'une image à l'autre ? */
            static float lastp[3], lastd[3], acc = 0.0f;
            static int nrev = 0, nfr = 0;
            static double rep_t = 0.0;
            float dd[3], dot = 0.0f;
            for (i = 0; i < 3; ++i) {
                dd[i] = g_cam_pos[i] - lastp[i];
                dot += dd[i] * lastd[i];
            }
            if (dot < 0.0f) {
                ++nrev;
                acc += sqrtf(dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2]);
            }
            ++nfr;
            memcpy(lastd, dd, sizeof(dd));
            memcpy(lastp, g_cam_pos, sizeof(lastp));
            if (now - rep_t > 2.0) {
                if (nfr > 20 && nrev * 3 > nfr && acc / (nrev ? nrev : 1) > 0.004f)
                    log_line("camera : la tete fait des allers-retours (%d sur %d passes, %.1f cm en moyenne ; marche %d, "
                             "geste %d, moto tenue %d, montee %.2f s) : vue lissee",
                             nrev, nfr, 100.0f * acc / (nrev ? nrev : 1), (int)g_walker.state, g_gesture.on,
                             bike_lift_active(), g_mount_t);
                rep_t = now;
                nrev = nfr = 0;
                acc = 0.0f;
            }
        }
        for (i = 0; i < 3; ++i)
            g_cam_pos[i] = pv[i] + sm[i];
    }
    for (i = 0; i < 3; ++i) {
        out[i * 4 + 0] = r[i];
        out[i * 4 + 1] = u[i];
        out[i * 4 + 2] = f[i];
        out[i * 4 + 3] = 0.0f;
    }
    out[12] = -(r[0] * g_cam_pos[0] + r[1] * g_cam_pos[1] + r[2] * g_cam_pos[2]);
    out[13] = -(u[0] * g_cam_pos[0] + u[1] * g_cam_pos[1] + u[2] * g_cam_pos[2]);
    out[14] = -(f[0] * g_cam_pos[0] + f[1] * g_cam_pos[1] + f[2] * g_cam_pos[2]);
    out[15] = 1.0f;
    for (i = 0; i < 3; ++i) {
        g_cam_rows[i * 4 + 0] = r[i];
        g_cam_rows[i * 4 + 1] = u[i];
        g_cam_rows[i * 4 + 2] = f[i];
        g_cam_rows[i * 4 + 3] = g_cam_pos[i];
    }
    g_cam_rows_ok = 1;
    game_camera_write(2);
    return 1;
}

/* Tête en marchant : comme un vrai coureur, le regard reste stable vers l'avant (la tête ne
   suit pas chaque à-coup du buste), elle se penche un peu en avant en courant, et elle tourne
   un peu avant le corps dans les virages (on regarde où on va). Tout est lissé. */
static void walk_head(const WalkInput *in, float dt)
{
    /* la tête garde l'orientation de la pose de marche (cou -> tête), mais lissée : elle ne
       suit plus chaque à-coup du pas ; dans les virages elle tourne un peu avant le corps */
    static float dir[3] = { 0, 1, 0 }, right[3] = { 1, 0, 0 }, lead = 0.0f;
    static double last = 0.0;
    float want_lead = 0.0f, d[3], r[3], l, k, c, s_;
    const float *ph = g_rd.pos[RD_HEAD], *pn = g_rd.pos[RD_NECK];
    int i;
    double now = now_seconds();
    for (i = 0; i < 3; ++i)
        d[i] = ph[i] - pn[i];
    r[0] = cosf(g_walker.heading);
    r[1] = 0.0f;
    r[2] = -sinf(g_walker.heading);
    if (g_walker.mirror) {
        r[0] = -r[0];
        r[2] = -r[2];
    }
    if (g_first_person && g_cam_yaw_ok) {
        /* 1re personne : la tête regarde où regarde la caméra (le cou tourne de 77° au plus) */
        want_lead = g_cam_yaw - g_walker.heading;
        while (want_lead > 3.14159265f)
            want_lead -= 6.2831853f;
        while (want_lead < -3.14159265f)
            want_lead += 6.2831853f;
        if (want_lead > HEAD_YAW_MAX) want_lead = HEAD_YAW_MAX;
        if (want_lead < -HEAD_YAW_MAX) want_lead = -HEAD_YAW_MAX;
    } else if (in->steer) {
        want_lead = in->steer_heading + (in->steer_back ? 3.14159265f : 0.0f) - g_walker.heading;
        while (want_lead > 3.14159265f)
            want_lead -= 6.2831853f;
        while (want_lead < -3.14159265f)
            want_lead += 6.2831853f;
        if (want_lead > 0.5f) want_lead = 0.5f;
        if (want_lead < -0.5f) want_lead = -0.5f;
    } else if (in->left || in->right) {
        want_lead = in->right ? 0.3f : -0.3f;
    }
    if (now - last > 0.5) {
        memcpy(dir, d, sizeof(dir));
        memcpy(right, r, sizeof(right));
        lead = 0.0f;
    }
    last = now;
    lead += (want_lead - lead) * (1.0f - expf(-(g_first_person ? 25.0f : 5.0f) * dt));
    /* rotation autour de la verticale (même sens que le cap) */
    c = cosf(lead);
    s_ = sinf(lead);
    {
        float x = d[0], z = d[2];
        d[0] = x * c + z * s_;
        d[2] = -x * s_ + z * c;
        x = r[0];
        z = r[2];
        r[0] = x * c + z * s_;
        r[2] = -x * s_ + z * c;
    }
    {
        /* tête un peu baissée, comme sur la moto (sinon le casque regarde vers le ciel) ;
           1re personne : elle se penche aussi comme le regard (le cou fait l'essentiel) */
        float a = g_cfg.head_down * g_walk_style + (g_first_person && g_cam_yaw_ok ? g_cam_pitch * 0.8f : 0.0f), fx = sinf(g_walker.heading + lead), fz = cosf(g_walker.heading + lead);
        float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), ca, sa;
        if (a > 0.75f) a = 0.75f;
        if (a < -0.6f) a = -0.6f;
        ca = cosf(a);
        sa = sinf(a);
        d[0] = d[0] * ca + fx * dl * sa;
        d[1] = d[1] * ca;
        d[2] = d[2] * ca + fz * dl * sa;
    }
    k = 1.0f - expf(-(g_first_person ? 25.0f : 6.0f) * dt);
    for (i = 0; i < 3; ++i) {
        dir[i] += (d[i] - dir[i]) * k;
        right[i] += (r[i] - right[i]) * k;
    }
    l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (l > 1e-4f)
        for (i = 0; i < 3; ++i)
            dir[i] /= l;
    l = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (l > 1e-4f) {
        right[0] /= l;
        right[2] /= l;
    }
    drive_set_head(dir, right);
}

/* Le pilote est au sol et ne roule plus vite : bassin près du sol, vitesse modérée */
static int on_ground_slow(const Ragdoll *rd)
{
    const float *p = rd->pos[RD_PELVIS], *q = rd->prev[RD_PELVIS], *n = rd->ground_n;
    float h = n[0] * p[0] + n[1] * p[1] + n[2] * p[2] - rd->ground_y;
    float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    float sub = g_cfg.physics.substep > 1e-4f ? g_cfg.physics.substep : 1.0f / 240.0f;
    float v = sqrtf(dx * dx + dy * dy + dz * dz) / sub;
    static double last_log = 0.0;
    if (rd->resting)
        return 1;
    /* seule vraie raison d'attendre : être encore en l'air */
    if (h < 1.0f)
        return 1;
    if (now_seconds() - last_log > 1.0) {
        last_log = now_seconds();
        log_line("marche : pas encore au sol (bassin a %.2f m du sol, %.1f m/s)", h, v);
    }
    return 0;
}

/* Touches : ZQSD (AZERTY) ou WASD (QWERTY), Maj = trottiner, X = se relever */
static int key_down(int vk)
{
#ifdef _WIN32
    return (GetAsyncKeyState(vk) & 0x8000) != 0 || kbhook_down(vk); /* (touche retenue pour le plugin) */
#else
    (void)vk;
    return 0;
#endif
}

/* Les touches ne comptent que si MX Bikes est la fenêtre active */
static int game_focused(void)
{
#ifdef _WIN32
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w)
        return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
#else
    return 1;
#endif
}

/* Lecture du clavier ; remplaçable pour les tests */
static void (*g_read_input)(WalkInput *in, int *getup) = NULL;

/* jeu au premier plan et menu P fermé : les touches du plugin comptent */
static int cone_menu_busy(void);
static int input_ok(void)
{
    return game_focused() && !equip_menu_open() && !cone_menu_busy() && !g_tuto_open && !g_upd_open;
}

/* touches réglables (MXRP_Ragdoll.ini : key_*, pad_*) */
static int bind_key(const int k[2])
{
    return (k[0] > 0 && key_down(k[0])) || (k[1] > 0 && key_down(k[1]));
}

static int bind_pad(int mask, const PadState *ps)
{
    int b = mask & 0xFFFF;
    if (!mask)
        return 0;
    if ((mask & PAD_RT) && ps->rt < 0.5f)
        return 0;
    if ((mask & PAD_LT) && ps->lt < 0.5f)
        return 0;
    return (ps->buttons & b) == b;
}

static int g_crouch_on = 0; /* v1.8.170 : accroupi (Maj / clic du stick droit : bascule) */
/* v1.8.181 : assis sur une chaise : la touche d'accroupi près d'une chaise (ou la roue) ; se relève en bougeant */
static struct {
    int on, idx, up, req;
    float t, from[2], from_h;
} g_seat = { 0, -1, 0, 0, 0.0f, { 0.0f, 0.0f }, 0.0f };
static void read_keyboard(WalkInput *in, int *getup)
{
    memset(in, 0, sizeof(*in));
    *getup = 0;
    if (g_walker.state != WALK_ON)
        g_crouch_on = 0;
    in->crouch = g_crouch_on;
    if (!input_ok())
        return;
    in->forward = bind_key(g_cfg.key_forward);
    in->back = bind_key(g_cfg.key_back);
    in->left = bind_key(g_cfg.key_left);
    in->right = bind_key(g_cfg.key_right);
    in->run = bind_key(g_cfg.key_run);
    in->jump = bind_key(g_cfg.key_jump);
    *getup = bind_key(g_cfg.key_getup);
    /* manette : stick gauche pour marcher, gâchette droite (l'accélérateur) pour courir,
       bouton A pour se relever */
    {
        PadState ps;
        float x = (float)(in->right - in->left), y = (float)(in->forward - in->back);
        if (padinject_read(&ps)) {
            if (fabsf(ps.lx) > 0.3f || fabsf(ps.ly) > 0.3f) {
                x = ps.lx;
                y = ps.ly;
            }
            if (bind_pad(g_cfg.pad_run, &ps))
                in->run = 1;
            if (bind_pad(g_cfg.pad_getup, &ps))
                *getup = 1;
            if (bind_pad(g_cfg.pad_jump, &ps))
                in->jump = 1;
        }
        {
            /* v1.8.170 : s'accroupir : un appui pour se baisser, un autre pour se relever
               (courir ou sauter le relève aussi) */
            static int c_prev = 1, r_prev = 1, j_prev = 1;
            int c = bind_key(g_cfg.key_crouch) || (padinject_read(&ps) && g_cfg.pad_crouch && bind_pad(g_cfg.pad_crouch, &ps));
            if (c && !c_prev && g_walker.state == WALK_ON) {
                /* v1.8.181 : près d'une chaise (ou assis) : s'asseoir / se lever */
                if (g_seat.on || bales_nearest_chair(g_walker.pos[0], g_walker.pos[1], 1.2f) >= 0)
                    g_seat.req = 1;
                else {
                    g_crouch_on = !g_crouch_on;
                    log_line("accroupi : %s", g_crouch_on ? "oui" : "non");
                }
            }
            if ((in->run && !r_prev) || (in->jump && !j_prev))
                g_crouch_on = 0;
            c_prev = c;
            r_prev = in->run;
            j_prev = in->jump;
            in->crouch = g_crouch_on;
            if (g_seat.on) {
                /* assis : bouger ou sauter le relève ; il ne marche pas */
                if (!g_seat.up && (in->steer || in->forward || in->back || in->left || in->right || in->jump))
                    g_seat.req = 1;
                in->steer = in->forward = in->back = in->left = in->right = in->jump = in->run = 0;
                in->crouch = 0;
                g_crouch_on = 0;
            }
        }
        if (g_cam_yaw_ok && (fabsf(x) > 0.3f || fabsf(y) > 0.3f)) {
            /* déplacement relatif à la caméra : avant = là où elle regarde */
            float h = g_cam_yaw, dx = y * sinf(h) + x * cosf(h), dz = y * cosf(h) - x * sinf(h);
            in->steer = 1;
            in->steer_heading = atan2f(dx, dz);
            /* en arrière (S, stick vers soi) : il recule en regardant devant lui */
            in->steer_back = y < -0.3f;
            in->forward = 1; /* compte aussi comme « bouger » pour se relever */
        } else if (fabsf(x) > 0.3f || fabsf(y) > 0.3f) {
            in->forward = y > 0.3f;
            in->back = y < -0.3f;
            in->left = x < -0.3f;
            in->right = x > 0.3f;
        }
    }
}

static double now_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static int g_font_loaded = 0;
static char g_font_names[64];

static SPluginQuad_t   g_quads[MAX_QUADS];
static SPluginString_t g_strings[MAX_STRINGS];

static void set_quad(SPluginQuad_t *q, float x0, float y0, float x1, float y1, unsigned long color);
static void set_string(SPluginString_t *s, const char *text, float x, float y, float size, unsigned long color);

/* v1.8.47 : le jeu ne dessine que les quadrilatères tournés dans le même sens que set_quad
   (haut gauche, bas gauche, bas droite : aire négative, y vers le bas). Les autres sont remis
   dans ce sens (vu sur clip : la roue et le corps des cônes n'apparaissaient pas). */
static void quad_wind(SPluginQuad_t *q)
{
    float a = 0.0f, t[2];
    int k;
    for (k = 0; k < 4; ++k) {
        int n = (k + 1) & 3;
        a += q->m_aafPos[k][0] * q->m_aafPos[n][1] - q->m_aafPos[n][0] * q->m_aafPos[k][1];
    }
    if (a > 0.0f) {
        memcpy(t, q->m_aafPos[1], sizeof(t));
        memcpy(q->m_aafPos[1], q->m_aafPos[3], sizeof(t));
        memcpy(q->m_aafPos[3], t, sizeof(t));
    }
}

/* Flèche au-dessus de la moto pendant qu'on marche : où est la moto ?
   Projection avec notre caméra et la perspective du jeu (glFrustum). */
static int project_rows(const float rows[12], const float p[3], float *sx, float *sy)
{
    float fr[6], d[3], x, y, z;
    int i;
    if (!glcam_frustum(fr) || fr[1] == fr[0] || fr[3] == fr[2] || fr[4] <= 0.0f)
        return 0;
    for (i = 0; i < 3; ++i)
        d[i] = p[i] - rows[i * 4 + 3];
    x = rows[0] * d[0] + rows[4] * d[1] + rows[8] * d[2];
    y = rows[1] * d[0] + rows[5] * d[1] + rows[9] * d[2];
    z = rows[2] * d[0] + rows[6] * d[1] + rows[10] * d[2];
    if (z < 0.3f)
        return 0; /* derrière la caméra */
    x = (2.0f * fr[4] / (fr[1] - fr[0])) * x / z + (fr[1] + fr[0]) / (fr[1] - fr[0]);
    y = (2.0f * fr[4] / (fr[3] - fr[2])) * y / z + (fr[3] + fr[2]) / (fr[3] - fr[2]);
    *sx = 0.5f * (x + 1.0f);
    *sy = 0.5f * (1.0f - y);
    return isfinite(*sx) && isfinite(*sy);
}

static int project_cam(const float p[3], float *sx, float *sy)
{
    if (!g_cam_rows_ok)
        return 0;
    return project_rows(g_cam_rows, p, sx, sy);
}

/* v1.8.45 : caméra de l'image (la nôtre à pied, sinon celle du jeu) pour dessiner les cônes */
static float g_draw_rows[12];
static int draw_rows_update(void)
{
    float V[16];
    int i;
    if (g_cam_rows_ok) {
        memcpy(g_draw_rows, g_cam_rows, sizeof(g_draw_rows));
        return 1;
    }
    if (!glcam_game_view(V, NULL))
        return 0;
    for (i = 0; i < 3; ++i) {
        float l = sqrtf(V[i] * V[i] + V[4 + i] * V[4 + i] + V[8 + i] * V[8 + i]);
        if (!(l > 0.97f && l < 1.03f))
            return 0; /* pas une vue simple (échelle) */
    }
    for (i = 0; i < 3; ++i) {
        g_draw_rows[i * 4 + 0] = V[i * 4 + 0];
        g_draw_rows[i * 4 + 1] = V[i * 4 + 1];
        g_draw_rows[i * 4 + 2] = V[i * 4 + 2];
    }
    /* position : -(R^T t) */
    for (i = 0; i < 3; ++i)
        g_draw_rows[i * 4 + 3] = -(V[i * 4 + 0] * V[12] + V[i * 4 + 1] * V[13] + V[i * 4 + 2] * V[14]);
    return 1;
}

static int project_draw(const float p[3], float *sx, float *sy)
{
    return project_rows(g_draw_rows, p, sx, sy);
}

static void set_tri(SPluginQuad_t *q, float ax, float ay, float bx, float by, float cx, float cy, unsigned long color)
{
    q->m_aafPos[0][0] = ax; q->m_aafPos[0][1] = ay;
    q->m_aafPos[1][0] = bx; q->m_aafPos[1][1] = by;
    q->m_aafPos[2][0] = cx; q->m_aafPos[2][1] = cy;
    q->m_aafPos[3][0] = cx; q->m_aafPos[3][1] = cy;
    q->m_iSprite = 0;
    q->m_ulColor = color;
    quad_wind(q);
}

static int draw_bike_marker(SPluginQuad_t *q, int max, int *ns)
{
    float p[3], sx, sy, s, dx, dz, dist, asp;
    int n = 0;
    *ns = 0;
    if (!g_cfg.bike_marker || g_drive_state != DRIVE_ACTIVE || g_walker.state != WALK_ON || bike_lift_active() ||
        max < 4)
        return 0;
    dx = g_last_bike_pos[0] - g_rd.pos[RD_PELVIS][0];
    dz = g_last_bike_pos[2] - g_rd.pos[RD_PELVIS][2];
    dist = sqrtf(dx * dx + dz * dz);
    p[0] = g_last_bike_pos[0];
    p[1] = g_last_bike_pos[1] + 1.3f + 0.08f * sinf((float)now_seconds() * 4.0f); /* elle flotte un peu */
    p[2] = g_last_bike_pos[2];
    if (!project_cam(p, &sx, &sy))
        return 0;
    {
        static int logged = 0;
        float fr[6];
        if (!logged && glcam_frustum(fr)) {
            logged = 1;
            log_line("fleche moto : perspective du jeu l %.3f r %.3f b %.3f t %.3f n %.3f f %.1f, moto a l'ecran (%.2f, %.2f)",
                     fr[0], fr[1], fr[2], fr[3], fr[4], fr[5], sx, sy);
        }
    }
    /* reste visible au bord de l'écran */
    if (sx < 0.03f) sx = 0.03f;
    if (sx > 0.97f) sx = 0.97f;
    if (sy < 0.06f) sy = 0.06f;
    if (sy > 0.94f) sy = 0.94f;
    asp = g_cfg.screen_aspect > 0.1f ? g_cfg.screen_aspect : 1.7778f;
    s = 0.028f; /* taille fixe à l'écran */
    /* contour sombre puis flèche orange qui pointe vers le bas */
    set_quad(&q[n++], sx - 0.35f * s / asp - 0.004f / asp, sy - 2.1f * s - 0.004f, sx + 0.35f * s / asp + 0.004f / asp,
             sy - 0.9f * s, 0xC0000000);
    set_tri(&q[n++], sx - s / asp - 0.007f / asp, sy - 1.0f * s - 0.004f, sx, sy + 0.009f,
            sx + s / asp + 0.007f / asp, sy - 1.0f * s - 0.004f, 0xC0000000);
    set_quad(&q[n++], sx - 0.35f * s / asp, sy - 2.1f * s, sx + 0.35f * s / asp, sy - 0.95f * s, 0xFF0090FF);
    set_tri(&q[n++], sx - s / asp, sy - 1.0f * s, sx, sy, sx + s / asp, sy - 1.0f * s, 0xFF0090FF);
    if (g_font_loaded && dist > 3.0f) {
        char t[32];
        snprintf(t, sizeof(t), "%.0f m", dist);
        set_string(&g_strings[0], t, sx, sy - 2.1f * s - 0.03f, 0.022f, 0xFFFFFFFF);
        g_strings[0].m_iJustify = 1;
        *ns = 1;
    }
    return n;
}

/* ---------- v1.8.45 : cônes ; v1.8.46 : roue comme les danses de Fortnite ----------
   Touche B du clavier ou flèche Bas de la croix de la manette, à pied :
   - maintenue : la roue s'ouvre, on vise une case (souris, stick, flèches), on relâche : c'est fait ;
   - appui court : la roue reste ouverte, on vise puis clic / A / Entrée (ou B / Bas encore). */

static int g_cone_qbase = 0;
static double g_cone_close_t = -10.0, g_wheel_t = 0.0;
static int g_wheel_hold = 0;
/* v1.8.54 : pages comme la roue des danses de Fortnite (LB / RB pour changer de page) */
/* v1.8.55 : premier écran : la roue principale (le cône) ; puis les options des cônes en pages */
/* v1.8.82 : deux groupes de pages : cônes (options, variantes), bottes de foin (options, variantes) */
enum { WL_CONES, WL_VAR, WL_BALES, WL_BVAR, WL_RAMPS, WL_RVAR, WL_FURN, WL_FVAR, WL_PAGES, WL_MAIN = WL_PAGES }; /* v1.8.179 : mobilier */ /* v1.8.119 : rampes */
#define WL_GROUP(l) ((l) / 2)
static int g_wheel_lvl = WL_MAIN;
/* v1.8.84 : petite botte de foin portée par le pilote */
enum { BC_CARRY, BC_PICK, BC_PUT };
static struct {
    int on, idx, mode;
    float t;
    float from[4]; /* ramasser : botte au sol (x, y, z, cap) */
    float to[4];   /* poser : là où elle va (x, y du sol, z, cap) */
} g_bcarry;
static double g_replay_rc; /* (défini plus bas : temps de la rediffusion) */
static int g_replay_have_rc;

static int cone_ground(float x, float z, float *h)
{
    float n[3];
    return ragdoll_ground(x, z, h, n);
}

/* v1.8.82 : sol des bottes de foin : relief actuel et objets de la piste (pas les autres bottes :
   bales.c les compte lui-même, et pas la botte elle-même) */
static int bale_ground(float x, float z, float *h)
{
    float ho, hl;
    if (!g_terrain_ok || !terrain_height(x, z, h, NULL))
        return 0;
    if (terrain_height_live(x, z, &hl) && hl > *h)
        *h = hl;
    *h += g_terrain_c;
    if (objects_height_raw(x, z, *h + 2.0f, &ho) && ho > *h)
        *h = ho;
    return 1;
}

/* v1.8.182 : sol du mobilier : le relief seul */
static int furn_ground(float x, float z, float *h)
{
    float hl;
    if (!g_terrain_ok || !terrain_height(x, z, h, NULL))
        return 0;
    if (terrain_height_live(x, z, &hl) && hl > *h)
        *h = hl;
    *h += g_terrain_c + 0.05f; /* v1.8.185 : 5 cm plus haut (les pieds s'enfonçaient un peu dans le sol) */
    return 1;
}

/* v1.8.65 : cônes des autres joueurs reçus par le réseau du plugin */
static volatile double g_cone_now = 0.0;
/* v1.8.66 : demandes entre joueurs (se faire donner un cône, actions sur tous les cônes) : reçues
   par le fil du réseau, traitées à l'image suivante */
#define CQ 32
static struct {
    uint32_t id;
    int type, len;
    unsigned char d[320];
} g_cq[CQ];
static volatile LONG g_cq_w = 0, g_cq_r = 0;
enum { CB_STATE = 1, CB_TAKE, CB_GIVE, CB_OP, CB_REMOVE, CB_BALES, CB_VER, CB_BPUSH, CB_RAMPS };
/* v1.8.94 : version du plugin d'un autre joueur (reçue par le fil du réseau) */
static char g_peer_ver[16];
static volatile int g_peer_ver_new = 0;
enum { COP_UP = 1, COP_SPAWN, COP_CLEAR };
static void cone_net_blob(uint32_t id, int type, const unsigned char *data, int len)
{
    if (type == CB_STATE) {
        cones_net_recv(id, data, len, g_cone_now);
    } else if (type == CB_BALES) {
        bales_net_recv(id, data, len, g_cone_now); /* v1.8.82 */
    } else if (type == CB_RAMPS) {
        ramps_net_recv(id, data, len, g_cone_now); /* v1.8.119 */
    } else if (type == CB_BPUSH && len >= 16) {
        /* v1.8.96 : un autre joueur pousse à pied une de nos petites bottes */
        uint32_t to;
        float dx, dz;
        memcpy(&to, data, 4);
        memcpy(&dx, data + 8, 4);
        memcpy(&dz, data + 12, 4);
        if (to == net_my_id())
            bales_net_push_apply(data[4], dx, dz);
    } else if (type == CB_VER && len > 0 && len < (int)sizeof(g_peer_ver)) {
        if (memcmp(g_peer_ver, data, (size_t)len) || g_peer_ver[len]) {
            memcpy(g_peer_ver, data, (size_t)len);
            g_peer_ver[len] = '\0';
            g_peer_ver_new = 1;
        }
    } else if (type >= CB_TAKE && type <= CB_REMOVE && len <= (int)sizeof(g_cq[0].d) && g_cq_w - g_cq_r < CQ) {
        int k = (int)(g_cq_w % CQ);
        g_cq[k].id = id;
        g_cq[k].type = type;
        g_cq[k].len = len;
        memcpy(g_cq[k].d, data, (size_t)len);
        InterlockedIncrement(&g_cq_w);
    }
}

/* demande au propriétaire de nous donner son cône (why 1 : pour le ramasser) */
static void cone_net_take(uint32_t owner, int slot, int why)
{
    static uint32_t last_o = 0;
    static int last_s = -1;
    static double last_t = 0.0;
    unsigned char b[8];
    double nw = now_seconds();
    if (owner == last_o && slot == last_s && nw - last_t < 0.5 && !why)
        return;
    last_o = owner;
    last_s = slot;
    last_t = nw;
    memcpy(b, &owner, 4);
    b[4] = (unsigned char)slot;
    b[5] = (unsigned char)why;
    b[6] = b[7] = 0;
    net_send_blob(CB_TAKE, b, 8);
}

static void cone_net_op(int op)
{
    unsigned char b[4] = { (unsigned char)op, 0, 0, 0 };
    net_send_blob(CB_OP, b, 4);
}

/* v1.8.48 : cônes en 3D (fil de rendu, dans la passe en perspective du jeu) */
/* v1.8.97 : état à dessiner (vivant, rediffusion) et caméra ; 0 : rien à dessiner */
static int scene_prepare(void)
{
    static int rlog = 0;
    if (g_draw_state == 2) {
        /* v1.8.52 : rediffusion : les cônes tels qu'ils étaient à ce moment */
        if (g_replay_have_rc && cones_hist_count()) {
            cones_show_replay(g_replay_rc);
            if (!rlog) {
                rlog = 1;
                log_line("cones : rediffusion : etats retenus rejoues (%d etats)", cones_hist_count());
            }
        } else {
            cones_show_live();
        }
    } else {
        /* en piste, et v1.8.161 : en spectateur aussi (avant : rien n'était dessiné, ni cônes, ni
           bottes, ni rampes, quand on regarde la course) */
        cones_show_live();
        rlog = 0;
    }
    {
        /* v1.8.91 : objets derrière la caméra ou trop loin : pas calculés. Le sens du regard est
           vérifié sur la moto (devant la caméra) : tant qu'il est douteux, seulement la distance */
        static int sgn = 0, votes = 0;
        float eye[3], fwd[3];
        if (glcam_scene_eye(eye, fwd)) {
            /* la caméra regarde le pilote : à pied son bassin, sinon la moto */
            const float *tg = g_drive_state == DRIVE_ACTIVE ? g_rd.pos[RD_PELVIS] : g_last_bike_pos;
            float d[3] = { tg[0] - eye[0], tg[1] - eye[1], tg[2] - eye[2] };
            float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            float dot = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
            {
                static int dlog = 0;
                static double dt0 = 0.0;
                if (dlog < 6 && now_seconds() - dt0 > 2.0) {
                    dt0 = now_seconds();
                    ++dlog;
                    log_line("objets 3D : camera (%.1f %.1f %.1f) regard (%.2f %.2f %.2f), pilote a %.1f m, produit %.2f, etat %d",
                             eye[0], eye[1], eye[2], fwd[0], fwd[1], fwd[2], dl, dot, g_draw_state);
                }
            }
            if (g_draw_state == 0 && dl > 1.5f && dl < 30.0f && fabsf(dot) > 0.6f * dl) {
                int s1 = dot > 0.0f ? 1 : -1;
                if (s1 == sgn)
                    votes = votes < 100 ? votes + 1 : 100;
                else if (--votes < 0) {
                    sgn = s1;
                    votes = 0;
                    log_line("objets 3D : sens du regard de la camera %+d", sgn);
                }
            }
            if (votes >= 20) {
                fwd[0] *= (float)sgn;
                fwd[1] *= (float)sgn;
                fwd[2] *= (float)sgn;
            } else {
                fwd[0] = fwd[1] = fwd[2] = 0.0f; /* (pas encore sûr) */
            }
            if (0) { /* v1.8.108 : option « objets optimisés » retirée : toujours la haute qualité */
                cones_set_view(eye, fwd, 150.0f);
                bales_set_view(eye, fwd, 300.0f);
            } else {
                /* v1.8.92 : haute qualité : tout est dessiné ; v1.8.97 : la caméra sert à choisir la
                   botte ronde complète (près) ou simplifiée (loin) */
                static const float none[3] = { 0.0f, 0.0f, 0.0f };
                cones_set_view(NULL, NULL, 0.0f);
                bales_set_view(eye, none, 1e9f);
            }
        } else {
            cones_set_view(NULL, NULL, 0.0f);
            bales_set_view(NULL, NULL, 0.0f);
        }
    }
    if (g_draw_state == 2 && g_replay_have_rc && bales_hist_count())
        bales_show_replay(g_replay_rc);
    else
        bales_show_live();
    /* v1.8.159 : les rampes aussi (en rediffusion, celles des autres ne sont plus reçues) */
    if (g_draw_state == 2 && ramps_hist_count())
        ramps_show_replay(g_replay_have_rc ? g_replay_rc : 1e18); /* (temps pas trouvé : les dernières) */
    else
        ramps_show_live();
    return 1;
}

static int cone_scene(float (*v)[3], float (*uv)[2], unsigned long *col, unsigned char *tex, int max_verts)
{
    static int logged = 0;
    int n;
    if (!scene_prepare())
        return 0;
    n = cones_shown_count() ? cones_mesh(v, uv, col, tex, max_verts, NULL) : 0;
    if (g_draw_state == 0) /* v1.8.65 : en ligne, les cônes des autres joueurs */
        n += cones_mesh_remote(v + n, uv + n, col + n, tex + n, max_verts - n);
    /* v1.8.82 : bottes de foin (textures après celles des cônes) */
    if (bales_shown_count())
        n += bales_mesh(v + n, uv + n, col + n, tex + n, max_verts - n, NULL, cones_variants());
    if (g_draw_state == 0)
        n += bales_mesh_remote(v + n, uv + n, col + n, tex + n, max_verts - n, cones_variants());
    if (!logged && n > 0) {
        logged = 1;
        log_line("cones : dessines en 3D dans la scene (%d triangles)", n);
    }
    return n;
}

/* v1.8.97 : objets par modèle (une matrice par objet, modèles dans la carte graphique) */
static int obj_scene(ObjInst *out, int max)
{
    static int logged = 0;
    int n;
    if (!scene_prepare())
        return 0;
    n = cones_shown_count() ? cones_instances(out, max, 0) : 0;
    if (g_draw_state == 0)
        n += cones_instances_remote(out + n, max - n, 0);
    if (bales_shown_count())
        n += bales_instances(out + n, max - n, cones_variants());
    if (g_draw_state == 0)
        n += bales_instances_remote(out + n, max - n, cones_variants());
    /* v1.8.119 : rampes (les nôtres et celles des autres) */
    n += ramps_instances(out + n, max - n, cones_variants() + bales_textures());
    if (!logged && n > 0) {
        logged = 1;
        log_line("objets 3D : dessines par modele (%d objets)", n);
    }
    return n;
}

static void cone_put_quad(int idx, const float x[4], const float y[4], unsigned long col)
{
    SPluginQuad_t *q = &g_quads[g_cone_qbase + idx];
    int k;
    for (k = 0; k < 4; ++k) {
        q->m_aafPos[k][0] = x[k];
        q->m_aafPos[k][1] = y[k];
    }
    q->m_iSprite = 0;
    q->m_ulColor = col;
    quad_wind(q);
}

/* roue ouverte, ou fermée il y a moins de 0,3 s (le bouton qui valide ne fait pas sauter) */
/* v1.8.62 : le pilote pose le cône (animation) ; v1.8.63 : il ramasse un cône posé, le porte en
   marchant, puis le repose ou le supprime */
enum { PL_NONE, PL_PLACE, PL_PICK, PL_CARRY };
static struct {
    int on, idx, mode;
    float t, x, y, z, yaw;
    float from[3]; /* ramasser : socle du cône au sol */
} g_place;

static int cone_menu_busy(void)
{
    /* (en portant un cône, il marche normalement) */
    return g_cone_menu || (g_place.on && g_place.mode != PL_CARRY) || now_seconds() - g_cone_close_t < 0.3;
}

static void cone_menu_close(void)
{
    if (g_cone_menu)
        g_cone_close_t = now_seconds();
    g_cone_menu = 0;
}

static void cone_msg(const char *fmt, int v)
{
    snprintf(g_msg, sizeof(g_msg), tr(fmt), v);
    g_msg_t = now_seconds();
}

/* ---------- roue : cases de chaque niveau ---------- */
enum {
    WA_NONE, WA_SUB_CONES, WA_ADD, WA_DEL, WA_UP, WA_CLEAR, WA_SUB_VAR, WA_BACK, WA_SPAWN, WA_PICK, WA_DROP,
    WA_TRASH, WA_SUB_BALES, WA_BADD, WA_BDEL, WA_BSPAWN, WA_BCLEAR, WA_BPICK, WA_BDROP, WA_BTRASH,
    WA_SUB_RAMPS, WA_RADD, WA_RDEL, WA_RCLEAR, WA_RFORM, WA_RCOLNEXT, WA_SUB_FURN, WA_FSIT, WA_BVAR0 = 40,
    WA_VAR0 = 60, WA_RSHAPE0 = 80, WA_RCOL0 = 90 /* v1.8.119 : rampes : forme, couleur */
};
typedef struct {
    const char *l1, *l2;
    int act;  /* WA_* (WA_VAR0 + n : variante n) */
    int icon; /* 0 aucune, 1 cône (couleur de la variante choisie), 2 + n : cône de la variante n,
                 30 : botte (variante choisie), 31 + n : botte de la variante n */
} WheelItem;

#define WHEEL_MAXN 10 /* v1.8.123 : 9 formes de rampe */
/* cases du niveau en cours ; rend leur nombre */
static int wheel_items(WheelItem *it)
{
    int n = 0, k, f0, fn, furn = g_wheel_lvl != WL_MAIN && WL_GROUP(g_wheel_lvl) == 3;
    bales_set_cat(furn); /* v1.8.179 : bottes de foin ou mobilier */
    switch (g_wheel_lvl) {
    case WL_MAIN:
        /* haut : les cônes ; les autres cases sont libres pour la suite */
        it[n++] = (WheelItem){ "Cones", "", WA_SUB_CONES, 1 };
        it[n++] = (WheelItem){ "Bottes", "de foin", WA_SUB_BALES, 30 }; /* v1.8.82 */
        it[n++] = (WheelItem){ "Rampes", "FMX", WA_SUB_RAMPS, 70 };   /* v1.8.119 */
        it[n++] = (WheelItem){ "Mobilier", "table, chaises", WA_SUB_FURN, 33 }; /* v1.8.179 */
        for (k = 0; k < 2; ++k)
            it[n++] = (WheelItem){ "", "", WA_NONE, 0 };
        break;
    case WL_BALES:
    case WL_FURN:
        if (g_bcarry.on) {
            /* v1.8.84 : il porte une petite botte ; v1.8.179 : ou un meuble */
            it[n++] = (WheelItem){ "Reposer", furn ? "l'objet" : "la botte", WA_BDROP, furn ? 30 : 32 };
            it[n++] = (WheelItem){ "Remettre", "au depart", WA_BSPAWN, 0 };
            it[n++] = (WheelItem){ "Supprimer", furn ? "l'objet tenu" : "la botte tenue", WA_BTRASH, 0 };
            it[n++] = (WheelItem){ "Retirer", furn ? "tout le mobilier" : "toutes les bottes", WA_BCLEAR, 0 };
            break;
        }
        it[n++] = (WheelItem){ "Poser", bales_variant_name(bales_variant()), WA_BADD, 30 };
        it[n++] = (WheelItem){ furn ? "Porter" : "Porter la", furn ? "le plus proche" : "petite botte", WA_BPICK, furn ? 30 : 32 };
        if (furn)
            it[n++] = (WheelItem){ "S'asseoir", "sur la chaise", WA_FSIT, 0 }; /* v1.8.181 */
        it[n++] = (WheelItem){ "Remettre", "au depart", WA_BSPAWN, 0 };
        it[n++] = (WheelItem){ "Retirer la", "plus proche", WA_BDEL, 0 };
        it[n++] = (WheelItem){ "Retirer", furn ? "tout le mobilier" : "toutes les bottes", WA_BCLEAR, 0 };
        break;
    case WL_BVAR:
    case WL_FVAR:
        bales_cat_range(furn, &f0, &fn);
        for (k = f0; k < f0 + fn && n < WHEEL_MAXN; ++k)
            it[n++] = (WheelItem){ bales_variant_name(k), "", WA_BVAR0 + k, 31 + k };
        break;
    case WL_RAMPS: /* v1.8.119 ; v1.8.122 : refait : poser (forme choisie), forme, couleur, retirer */
        it[n++] = (WheelItem){ "Poser", ramps_shape_name(ramps_shape()), WA_RADD, 70 };
        it[n++] = (WheelItem){ "Forme", "changer", WA_RFORM, 0 };
        it[n++] = (WheelItem){ "Couleur", ramps_shape_fixed(ramps_shape()) ? "texture d'origine" : ramps_color_name(ramps_color()),
                               WA_RCOLNEXT, 69 };
        it[n++] = (WheelItem){ "Retirer", "toutes les rampes", WA_RCLEAR, 0 };
        it[n++] = (WheelItem){ "Retirer la", "plus proche", WA_RDEL, 0 };
        break;
    case WL_RVAR: /* v1.8.122 : les formes seulement (la couleur : page d'avant) */
        for (k = 0; k < ramps_shapes_menu() && n < WHEEL_MAXN; ++k) {
            static char dims[RAMP_SHAPES_MAX][24];
            snprintf(dims[k], sizeof(dims[k]), "%.0f x %.1f m", ramps_length(k), ramps_shape_height(k));
            it[n++] = (WheelItem){ ramps_shape_name(k), dims[k], WA_RSHAPE0 + k, 80 + k };
        }
        break;
    case WL_CONES:
        if (g_place.on && g_place.mode == PL_CARRY) {
            /* il porte un cône : le reposer ou le supprimer */
            it[n++] = (WheelItem){ "Reposer", "le cone", WA_DROP, 1 };
            it[n++] = (WheelItem){ "Relever", "sur place", WA_UP, 0 };
            it[n++] = (WheelItem){ "Remettre", "au depart", WA_SPAWN, 0 };
            it[n++] = (WheelItem){ "Supprimer", "le cone tenu", WA_TRASH, 0 };
            it[n++] = (WheelItem){ "Retirer", "tous les cones", WA_CLEAR, 0 };
            break;
        }
        it[n++] = (WheelItem){ "Poser", "un cone", WA_ADD, 1 };
        it[n++] = (WheelItem){ "Relever", "sur place", WA_UP, 0 };
        it[n++] = (WheelItem){ "Remettre", "au depart", WA_SPAWN, 0 };
        it[n++] = (WheelItem){ "Retirer", "tous les cones", WA_CLEAR, 0 };
        it[n++] = (WheelItem){ "Retirer le", "plus proche", WA_DEL, 0 };
        it[n++] = (WheelItem){ "Ramasser le", "plus proche", WA_PICK, 1 };
        break;
    default: {
        static const char *k_vn[5] = { "Classique", "Tres sale", "Use", "Delave", "Rouge vif" };
        int nv = cones_variants();
        for (k = 0; k < nv && n < WHEEL_MAXN; ++k)
            it[n++] = (WheelItem){ k < 5 ? k_vn[k] : "Variante", "", WA_VAR0 + k, 2 + k };
        break;
    }
    }
    return n;
}

static void wheel_reset_aim(void)
{
    g_wheel_sel = -1;
    g_wheel_px = g_wheel_py = 0.0f;
}

static void start_pick(int best)
{
    float cp[3];
    if (!cones_get(best, cp, NULL))
        return;
    g_walker.heading = atan2f(cp[0] - g_walker.pos[0], cp[2] - g_walker.pos[1]); /* il se tourne vers le cône */
    g_place.on = 1;
    g_place.mode = PL_PICK;
    g_place.idx = best;
    g_place.t = 0.0f;
    memcpy(g_place.from, cp, sizeof(cp));
    {
        float h;
        if (!cone_ground(cp[0], cp[2], &h))
            h = walk_ground_height(&g_rd, cp[0], cp[2]);
        g_place.from[1] = h;
    }
    g_place.yaw = g_walker.heading + 0.7854f;
    log_line("cones : ramasse le n %d", best);
}

/* v1.8.66 : ramassage demandé à un autre joueur (en attente de son cône) */
static struct {
    int on;
    uint32_t owner;
    int slot;
    double t;
} g_pick_wait;

/* à chaque image : demandes des autres joueurs, cônes donnés, contacts avec leurs cônes */
static void cone_net_update(void)
{
    uint32_t me = net_my_id();
    while (g_cq_r < g_cq_w) {
        int k = (int)(g_cq_r % CQ);
        const unsigned char *d = g_cq[k].d;
        int len = g_cq[k].len;
        switch (g_cq[k].type) {
        case CB_TAKE:
            if (len >= 6 && me && memcmp(d, &me, 4) == 0) {
                /* on nous demande un de nos cônes : on le donne */
                unsigned char out[8 + 320];
                int gs = cones_net_give_size();
                if (8 + gs <= (int)sizeof(out) && !(g_place.on && g_place.idx == d[4]) &&
                    cones_net_give(d[4], out + 8)) {
                    memcpy(out, &g_cq[k].id, 4);
                    out[4] = d[4];
                    out[5] = d[5];
                    out[6] = out[7] = 0;
                    net_send_blob(CB_GIVE, out, 8 + gs);
                    log_line("cones : en ligne : cone %d donne a un autre joueur", d[4]);
                }
            }
            break;
        case CB_GIVE:
            if (len >= 8 && me && memcmp(d, &me, 4) == 0) {
                int idx = cones_net_adopt(g_cq[k].id, d[4], d + 8, len - 8);
                log_line("cones : en ligne : cone d'un autre joueur recu (n %d)", idx);
                if (idx >= 0 && d[5] == 1 && g_pick_wait.on && g_pick_wait.owner == g_cq[k].id) {
                    g_pick_wait.on = 0;
                    if (!g_place.on && g_drive_state == DRIVE_ACTIVE && g_walker.state == WALK_ON && !g_gesture.on)
                        start_pick(idx);
                }
            }
            break;
        case CB_OP:
            if (len >= 1) {
                if (d[0] == COP_UP)
                    cones_reset_upright();
                else if (d[0] == COP_SPAWN)
                    cones_reset_spawn();
                else if (d[0] == COP_CLEAR) {
                    if (g_place.on) {
                        g_place.on = 0;
                        g_walker.carry = 0.0f;
                    }
                    cones_clear();
                }
                log_line("cones : en ligne : action %d d'un autre joueur", d[0]);
            }
            break;
        case CB_REMOVE:
            if (len >= 5 && me && memcmp(d, &me, 4) == 0 && !(g_place.on && g_place.idx == d[4]))
                cones_remove(d[4]);
            break;
        default:
            break;
        }
        InterlockedIncrement(&g_cq_r);
    }
    if (g_pick_wait.on && now_seconds() - g_pick_wait.t > 2.0) {
        g_pick_wait.on = 0;
        cone_msg("L'autre joueur n'a pas donne son cone", 0);
    }
    /* notre pilote (à pied ou au sol) touche un cône d'un autre joueur : on se le fait donner, notre
       simulation le pousse ensuite */
    if (me && cones_net_remote_count() && g_drive_state == DRIVE_ACTIVE) {
        static const int k_pts[] = { RD_FOOT_L, RD_FOOT_R, RD_KNEE_L, RD_KNEE_R, RD_PELVIS, RD_HAND_L, RD_HAND_R,
                                     RD_CHEST, RD_HEAD };
        int i;
        for (i = 0; i < (int)(sizeof(k_pts) / sizeof(k_pts[0])); ++i) {
            uint32_t owner;
            int slot;
            if (cones_net_touching(g_rd.pos[k_pts[i]], 0.25f, &owner, &slot)) {
                cone_net_take(owner, slot, 0);
                break;
            }
        }
    }
}

/* rend 1 si la roue reste ouverte */
static void bale_start_put(int idx);
static void bale_carry_spot(int var, float c[3], float *yaw);
static int cone_menu_action(int act)
{
    float fx = sinf(g_walker.heading), fz = cosf(g_walker.heading);
    log_line("cones : roue, niveau %d, choix %d", g_wheel_lvl, act);
    bales_set_cat(g_wheel_lvl != WL_MAIN && WL_GROUP(g_wheel_lvl) == 3); /* v1.8.179 */
    switch (act) {
    case WA_SUB_CONES:
        g_wheel_lvl = WL_CONES;
        wheel_reset_aim();
        g_wheel_sel = 0; /* « Poser » surligné */
        return 1;
    case WA_SUB_VAR:
        g_wheel_lvl = WL_VAR;
        wheel_reset_aim();
        return 1;
    case WA_SUB_BALES:
        g_wheel_lvl = WL_BALES;
        wheel_reset_aim();
        g_wheel_sel = 0; /* « Poser » surligné */
        return 1;
    case WA_FSIT: /* v1.8.181 */
        if (bales_nearest_chair(g_walker.pos[0], g_walker.pos[1], 1.6f) < 0)
            cone_msg("Aucune chaise a moins de %d m", 2);
        else
            g_seat.req = 1;
        return 0;
    case WA_SUB_FURN: /* v1.8.179 : mobilier, page à part */
        g_wheel_lvl = WL_FURN;
        wheel_reset_aim();
        g_wheel_sel = 0;
        return 1;
    case WA_BADD: {
        /* v1.8.82 : botte posée devant le pilote, en travers (trop lourde pour la porter) */
        float hx, hy, hh, d, x, z, h;
        int idx;
        bales_variant_size(bales_variant(), &hx, &hy, &hh);
        d = 0.45f + (bales_variant() >= 2 ? (hx > hy ? hx : hy) : hx); /* v1.8.182 : mobilier posé devant lui */
        x = g_walker.pos[0] + fx * d;
        z = g_walker.pos[1] + fz * d;
        if (!(bales_variant() >= 2 ? furn_ground(x, z, &h) : bale_ground(x, z, &h)))
            h = walk_ground_height(&g_rd, x, z);
        if (g_bcarry.on || g_place.on)
            return 0;
        {
            /* v1.8.182 : table en travers devant lui ; v1.8.184 : chaise, dossier vers lui */
            float yaw = g_walker.heading, cfx, cfy;
            if (bales_variant() == 2) {
                yaw += 1.5707963f;
                d = 0.45f + hy;
                x = g_walker.pos[0] + fx * d;
                z = g_walker.pos[1] + fz * d;
                if (!furn_ground(x, z, &h))
                    h = walk_ground_height(&g_rd, x, z);
            } else if (bales_chair_front(bales_variant(), &cfx, &cfy))
                yaw -= atan2f(cfy, cfx); /* v1.8.184 : dossier devant lui (la chaise regarde dans son sens) */
            idx = bales_add(x, h, z, yaw, bale_ground);
        }
        if (idx < 0)
            cone_msg("Plus de place : %d bottes au plus", BALES_MAX);
        else {
            cone_msg(bales_variant() >= 2 ? "Objet pose (%d)" : "Botte posee (%d)", bales_count_cat());
            if (bales_get_var(idx) == 1) { /* v1.8.182 : le mobilier est posé directement (pas d'abord dans les mains) */
                /* v1.8.85 : la petite : il l'a dans les mains et la pose devant lui */
                float cc[3], cy;
                g_bcarry.on = 1;
                g_bcarry.idx = idx;
                bale_carry_spot(bales_get_var(idx), cc, &cy);
                bales_hold(idx, cc[0], cc[1], cc[2], cy); /* dans ses mains (pas compté sous lui-même) */
                bale_start_put(idx);
            }
        }
        return 0;
    }
    case WA_BPICK: {
        /* v1.8.84 : porter la petite botte la plus proche (2 m) */
        int i;
        if (g_place.on || g_bcarry.on)
            return 0;
        i = bales_carry_nearest(g_walker.pos[0], g_walker.pos[1], 2.0f);
        if (i < 0) {
            cone_msg("Rien a porter a moins de %d m", 2);
            return 0;
        }
        {
            float bp[3];
            if (bales_get(i, bp, NULL))
                g_walker.heading = atan2f(bp[0] - g_walker.pos[0], bp[2] - g_walker.pos[1]);
        }
        g_bcarry.on = 1;
        g_bcarry.idx = i;
        g_bcarry.mode = BC_PICK; /* v1.8.85 : il se baisse et la soulève */
        g_bcarry.t = 0.0f;
        {
            float bp[3], yaw;
            bales_get(i, bp, &yaw);
            memcpy(g_bcarry.from, bp, sizeof(bp));
            g_bcarry.from[3] = yaw;
        }
        log_line("bottes : petite botte %d ramassee", i);
        return 0;
    }
    case WA_BDROP:
        if (g_bcarry.on && g_bcarry.mode == BC_CARRY) {
            /* v1.8.85 : il se baisse et la pose devant lui */
            bale_start_put(g_bcarry.idx);
            cone_msg(bales_get_var(g_bcarry.idx) >= 2 ? "Objet pose (%d)" : "Botte posee (%d)", bales_count_cat());
        }
        return 0;
    case WA_BTRASH:
        if (g_bcarry.on) {
            bales_remove(g_bcarry.idx);
            g_bcarry.on = 0;
            g_walker.carry = 0.0f;
            g_walker.grab = 0.0f;
            g_walker.crouch = 0.0f;
            cone_msg(WL_GROUP(g_wheel_lvl) == 3 ? "Objet retire (%d)" : "Botte retiree (%d)", bales_count_cat());
        }
        return 0;
    case WA_BDEL:
        if (bales_remove_nearest(g_walker.pos[0], g_walker.pos[1], 15.0f))
            cone_msg(WL_GROUP(g_wheel_lvl) == 3 ? "Objet retire (%d)" : "Botte retiree (%d)", bales_count_cat());
        else
            cone_msg(WL_GROUP(g_wheel_lvl) == 3 ? "Aucun objet a moins de %d m" : "Aucune botte a moins de %d m", 15);
        return 0;
    case WA_BSPAWN:
        cone_msg(WL_GROUP(g_wheel_lvl) == 3 ? "%d objets remis au depart" : "%d bottes remises au depart", bales_moved_count());
        bales_reset_spawn();
        return 0;
    case WA_BCLEAR:
        cone_msg(WL_GROUP(g_wheel_lvl) == 3 ? "%d objets retires" : "%d bottes retirees", bales_count_cat());
        if (g_bcarry.on && (bales_get_var(g_bcarry.idx) >= 2) == (WL_GROUP(g_wheel_lvl) == 3)) {
            g_bcarry.on = 0;
            g_walker.carry = 0.0f;
        }
        bales_clear_cat(); /* v1.8.179 : seulement les bottes, ou seulement le mobilier */
        return 0;
    case WA_SUB_RAMPS: /* v1.8.119 */
        g_wheel_lvl = WL_RAMPS;
        wheel_reset_aim();
        g_wheel_sel = 0;
        return 1;
    case WA_RADD: {
        /* v1.8.119 : rampe posée devant le pilote, le saut dans la direction où il regarde */
        float x, z, h;
        if (g_bcarry.on || g_place.on)
            return 0;
        x = g_walker.pos[0] + fx * 2.0f;
        z = g_walker.pos[1] + fz * 2.0f;
        if (!bale_ground(x, z, &h))
            h = walk_ground_height(&g_rd, x, z);
        {
            int ri = ramps_add(x, h, z, g_walker.heading, ground_walk_fn);
            if (ri < 0)
                cone_msg("Plus de place : %d rampes au plus", RAMPS_MAX);
            else {
                /* v1.8.141 : sur cette piste, les roues ne voient rien au-dessus du point le plus haut du terrain */
                float lim = terrain_grid_top() + g_terrain_c, top = ramps_top(ri);
                cone_msg("Rampe posee (%d)", ramps_count());
                if (g_terrain_ok && top > lim + 0.05f && !gamever_ode(NULL)->create) { /* (v1.8.146 : en maillage, pas de limite) */
                    snprintf(g_msg, sizeof(g_msg), tr("Rampe trop haute ici : roues sans contact au-dessus de %.1f m"),
                             (double)(lim - h));
                    g_msg_t = now_seconds();
                    log_line("rampes : rampe %d : sommet %.2f, au-dessus du plus haut point de la piste (%.2f) : roues sans contact au-dessus de %.2f m",
                             ri, top, lim, lim - h);
                }
            }
        }
        return 0;
    }
    case WA_RFORM: /* v1.8.122 */
        g_wheel_lvl = WL_RVAR;
        wheel_reset_aim();
        g_wheel_sel = ramps_shape();
        return 1;
    case WA_RCOLNEXT: /* v1.8.122 : couleur suivante ; la roue reste ouverte */
        if (ramps_shape_fixed(ramps_shape())) {
            cone_msg("Cette forme garde sa texture d'origine", 0);
            return 1;
        }
        ramps_set_color((ramps_color() + 1) % ramps_colors());
        snprintf(g_msg, sizeof(g_msg), "%s : %s", tr("Rampe"), tr(ramps_color_name(ramps_color())));
        g_msg_t = now_seconds();
        return 1;
    case WA_RDEL:
        if (ramps_remove_nearest(g_walker.pos[0], g_walker.pos[1], 6.0f))
            cone_msg("Rampe retiree (%d)", ramps_count());
        else
            cone_msg("Aucune rampe a moins de %d m", 6);
        return 0;
    case WA_RCLEAR:
        cone_msg("%d rampes retirees", ramps_count());
        ramps_clear();
        return 0;
    case WA_BACK:
        g_wheel_lvl = WL_MAIN;
        wheel_reset_aim();
        return 1;
    case WA_ADD: {
        /* v1.8.62 : le pilote le pose : il se baisse, le cône dans les mains, et le met au sol devant lui */
        float x = g_walker.pos[0] + fx * 0.62f, z = g_walker.pos[1] + fz * 0.62f, h;
        int idx;
        if (g_place.on || g_bcarry.on)
            return 0;
        if (!cone_ground(x, z, &h))
            h = walk_ground_height(&g_rd, x, z);
        idx = cones_add(x, h, z);
        if (idx < 0) {
            cone_msg("Plus de place : %d cones au plus", CONES_MAX);
        } else {
            g_place.on = 1;
            g_place.mode = PL_PLACE;
            g_place.idx = idx;
            g_place.t = 0.0f;
            g_place.x = x;
            g_place.y = h;
            g_place.z = z;
            g_place.yaw = g_walker.heading + 0.7854f; /* socle de biais par rapport au pilote */
            cones_set_spawn(idx, x, h, z, g_place.yaw);
            cone_msg("Cone pose (%d)", cones_count());
        }
        return 0;
    }
    case WA_DEL: {
        /* le plus proche, à nous ou à un autre joueur */
        uint32_t owner;
        int slot, i, mine = 0;
        float rp[3], cp[3], dm = 1e9f, dr = 1e9f;
        for (i = 0; i < CONES_MAX; ++i)
            if (cones_get(i, cp, NULL)) {
                float d = (cp[0] - g_walker.pos[0]) * (cp[0] - g_walker.pos[0]) +
                          (cp[2] - g_walker.pos[1]) * (cp[2] - g_walker.pos[1]);
                if (d < dm)
                    dm = d;
                mine = 1;
            }
        if (cones_net_nearest(g_walker.pos[0], g_walker.pos[1], 15.0f, &owner, &slot, rp))
            dr = (rp[0] - g_walker.pos[0]) * (rp[0] - g_walker.pos[0]) + (rp[2] - g_walker.pos[1]) * (rp[2] - g_walker.pos[1]);
        if (dr < dm && dr < 15.0f * 15.0f) {
            unsigned char b[8];
            memcpy(b, &owner, 4);
            b[4] = (unsigned char)slot;
            b[5] = b[6] = b[7] = 0;
            net_send_blob(CB_REMOVE, b, 8);
            cones_net_forget(owner, slot);
            cone_msg("Cone retire (%d)", cones_count() + cones_net_remote_count());
        } else if (mine && cones_remove_nearest(g_walker.pos[0], g_walker.pos[1], 15.0f)) {
            cone_msg("Cone retire (%d)", cones_count() + cones_net_remote_count());
        } else {
            cone_msg("Aucun cone a moins de %d m", 15);
        }
        return 0;
    }
    case WA_PICK: {
        /* ramasser le cône le plus proche (2 m au plus), à nous ou à un autre joueur */
        int i, best = -1, slot;
        uint32_t owner;
        float bd = 2.0f * 2.0f, cp[3], rp[3];
        if (g_bcarry.on)
            return 0;
        for (i = 0; i < CONES_MAX; ++i) {
            float dx, dz;
            if (!cones_get(i, cp, NULL))
                continue;
            dx = cp[0] - g_walker.pos[0];
            dz = cp[2] - g_walker.pos[1];
            if (dx * dx + dz * dz < bd) {
                bd = dx * dx + dz * dz;
                best = i;
            }
        }
        if (cones_net_nearest(g_walker.pos[0], g_walker.pos[1], 2.0f, &owner, &slot, rp) &&
            (rp[0] - g_walker.pos[0]) * (rp[0] - g_walker.pos[0]) + (rp[2] - g_walker.pos[1]) * (rp[2] - g_walker.pos[1]) <
                bd) {
            /* à un autre joueur : on le lui demande ; il est ramassé quand il arrive */
            g_walker.heading = atan2f(rp[0] - g_walker.pos[0], rp[2] - g_walker.pos[1]);
            g_pick_wait.on = 1;
            g_pick_wait.owner = owner;
            g_pick_wait.slot = slot;
            g_pick_wait.t = now_seconds();
            cone_net_take(owner, slot, 1);
            return 0;
        }
        if (best < 0) {
            cone_msg("Aucun cone a moins de %d m", 2);
            return 0;
        }
        start_pick(best);
        return 0;
    }
    case WA_DROP: {
        /* reposer le cône porté, devant lui */
        float x = g_walker.pos[0] + fx * 0.62f, z = g_walker.pos[1] + fz * 0.62f, h;
        if (!g_place.on || g_place.mode != PL_CARRY)
            return 0;
        if (!cone_ground(x, z, &h))
            h = walk_ground_height(&g_rd, x, z);
        g_place.mode = PL_PLACE;
        g_place.t = 0.0f;
        g_place.x = x;
        g_place.y = h;
        g_place.z = z;
        g_place.yaw = g_walker.heading + 0.7854f;
        cones_set_spawn(g_place.idx, x, h, z, g_place.yaw); /* nouveau point de départ */
        return 0;
    }
    case WA_TRASH:
        if (g_place.on && g_place.mode == PL_CARRY) {
            cones_remove(g_place.idx);
            g_place.on = 0;
            g_walker.carry = 0.0f;
            cone_msg("Cone supprime (%d)", cones_count());
        }
        return 0;
    case WA_UP:
        /* v1.8.66 : en ligne, aussi ceux des autres joueurs */
        cone_msg("%d cones releves", cones_down_count());
        cones_reset_upright();
        cone_net_op(COP_UP);
        return 0;
    case WA_SPAWN:
        /* v1.8.57 : remis debout là où ils ont été posés */
        cone_msg("%d cones remis au depart", cones_moved_count());
        cones_reset_spawn();
        cone_net_op(COP_SPAWN);
        return 0;
    case WA_CLEAR:
        cone_msg("%d cones retires", cones_count() + cones_net_remote_count());
        cones_clear();
        cone_net_op(COP_CLEAR);
        return 0;
    default:
        if (act >= WA_BVAR0 && act < WA_BVAR0 + bales_variants()) {
            bales_set_variant(act - WA_BVAR0);
            cone_msg(tr(bales_variant_name(act - WA_BVAR0)), 0);
            g_wheel_lvl = WL_GROUP(g_wheel_lvl) * 2; /* v1.8.179 : bottes ou mobilier */
            wheel_reset_aim();
            g_wheel_sel = 0;
            return 1;
        }
        if (act >= WA_RSHAPE0 && act < WA_RSHAPE0 + ramps_shapes_menu()) {
            /* v1.8.122 : forme choisie : retour à la page des rampes, « Poser » surligné */
            ramps_set_shape(act - WA_RSHAPE0);
            snprintf(g_msg, sizeof(g_msg), "%s : %s", tr("Rampe"), tr(ramps_shape_name(act - WA_RSHAPE0)));
            g_msg_t = now_seconds();
            g_wheel_lvl = WL_RAMPS;
            wheel_reset_aim();
            g_wheel_sel = 0;
            return 1;
        }
        if (act >= WA_RCOL0 && act < WA_RCOL0 + ramps_colors()) {
            ramps_set_color(act - WA_RCOL0);
            snprintf(g_msg, sizeof(g_msg), "%s : %s", tr("Rampe"), tr(ramps_color_name(act - WA_RCOL0)));
            g_msg_t = now_seconds();
            return 1;
        }
        if (act >= WA_VAR0 && act < WA_VAR0 + cones_variants()) {
            cones_set_variant(act - WA_VAR0);
            cone_msg("Variante %d choisie", act - WA_VAR0 + 1);
            g_wheel_lvl = WL_CONES; /* page des cônes, « Poser » surligné */
            wheel_reset_aim();
            g_wheel_sel = 0;
            return 1;
        }
        return 1;
    }
}

/* direction (x à droite, y en haut) -> case de la roue (n cases, la 0 en haut) */
static int wheel_sector(float x, float y, int n)
{
    float a = atan2f(x, y), span = 6.2831853f / (float)n; /* 0 = haut, sens des aiguilles d'une montre */
    int k;
    if (a < 0.0f)
        a += 6.2831853f;
    k = (int)floorf((a + span * 0.5f) / span);
    return k % n;
}

/* case choisie : vide -> rien */
static int wheel_pick(const WheelItem *it, int n, int k)
{
    return k >= 0 && k < n && it[k].act != WA_NONE ? k : -1;
}

/* v1.8.62 : animation de pose (1,1 s) : il se penche et s'accroupit, le cône dans les mains (tenu
   par le haut), le pose devant lui, le lâche et se relève */
#define PLACE_DOWN 0.45f
#define PLACE_LET  0.58f
#define PLACE_END  1.10f
static void walker_axes(float f[3], float r[3])
{
    f[0] = sinf(g_walker.heading);
    f[1] = 0.0f;
    f[2] = cosf(g_walker.heading);
    r[0] = f[2];
    r[1] = 0.0f;
    r[2] = -f[0];
    if (g_walker.mirror) {
        r[0] = -r[0];
        r[2] = -r[2];
    }
}

/* v1.8.181 : s'asseoir sur une chaise / se lever */
static void seat_end(void)
{
    g_seat.on = 0;
    g_seat.up = 0;
    g_seat.idx = -1;
    g_walker.sit = 0.0f;
    g_walker.pegs = 0.0f;
    g_walker.lean = 0.0f;
    g_walker.carry = 0.0f;
    bales_set_ignore(-1);
}

static int seat_start(void)
{
    int i = bales_nearest_chair(g_walker.pos[0], g_walker.pos[1], 1.6f);
    if (i < 0 || g_bcarry.on || g_gesture.on || g_walker.state != WALK_ON)
        return 0;
    g_seat.on = 1;
    g_seat.up = 0;
    g_seat.idx = i;
    g_seat.t = 0.0f;
    g_seat.from[0] = g_walker.pos[0];
    g_seat.from[1] = g_walker.pos[1];
    g_seat.from_h = g_walker.heading;
    g_crouch_on = 0;
    log_line("chaise : le pilote s'assoit (objet %d)", i);
    return 1;
}

static void seat_update(float dt)
{
    float seat[3], fr[3], f[3], r[3], tx, tz, s;
    int k;
    if (g_seat.req) {
        g_seat.req = 0;
        if (!g_seat.on)
            seat_start();
        else if (!g_seat.up) {
            g_seat.up = 1;
            g_seat.t = 0.0f;
            log_line("chaise : le pilote se leve");
        }
    }
    if (!g_seat.on)
        return;
    if (g_walker.state != WALK_ON || g_bcarry.on || g_gesture.on || !bales_chair_seat(g_seat.idx, seat, fr)) {
        seat_end();
        return;
    }
    bales_set_ignore(g_seat.idx);
    g_seat.t += dt;
    /* v1.8.190 : chaise rouge : toile creuse et dossier très incliné : assis plus au fond, plus bas,
       dos bien en arrière (clip : assis au bord, buste devant le dossier) */
    int red = bales_get_var(g_seat.idx) == 4;
    /* v1.8.192 : chaise rouge mesurée sur le modèle : toile plate à 0,35 m, dossier presque droit (11 degrés),
       sa surface 14 cm derrière le centre à hauteur du bassin : bassin 11 cm devant, buste presque droit
       (1.8.190/191 : buste couché à 25-30 degrés : il passait à travers le dossier) */
    /* v1.8.193 : clip de dos : tout le dos du pilote passait derrière la toile (le dos du corps est ~15 cm
       derrière le bassin, le dossier penche plus en jeu) : bassin 19 cm plus en avant, buste qui suit le dossier */
    float back = red ? -0.10f : 0.08f, sit_up = 0.10f, lean = red ? -0.28f : -0.32f;
    float feet = red ? 0.46f : 0.40f;
    tx = seat[0] - fr[0] * back; /* v1.8.183 : fesses au fond de la chaise */
    tz = seat[2] - fr[2] * back;
    if (!g_seat.up) {
        float a = smooth01(g_seat.t / 0.5f);
        g_walker.pos[0] = g_seat.from[0] + (tx - g_seat.from[0]) * a;
        g_walker.pos[1] = g_seat.from[1] + (tz - g_seat.from[1]) * a;
        g_walker.heading = lerp_angle(g_seat.from_h, atan2f(fr[0], fr[2]), smooth01(g_seat.t / 0.35f));
        s = smooth01((g_seat.t - 0.25f) / 0.6f);
    } else {
        float a = smooth01(g_seat.t / 0.6f);
        g_walker.pos[0] = tx + fr[0] * 0.42f * a;
        g_walker.pos[1] = tz + fr[2] * 0.42f * a;
        g_walker.heading = atan2f(fr[0], fr[2]);
        s = 1.0f - a;
        if (g_seat.t >= 0.6f) {
            seat_end();
            return;
        }
    }
    if (!g_seat.up && g_seat.t >= 1.2f && g_seat.t - dt < 1.2f) {
        float gs = 0.0f;
        furn_ground(seat[0], seat[2], &gs);
        log_line("chaise : assise %.2f m au-dessus du sol, bassin %.2f, tete %.2f, avant (%.2f %.2f)", seat[1] - gs,
                 g_rd.pos[RD_PELVIS][1] - gs, g_rd.pos[RD_HEAD][1] - gs, fr[0], fr[2]);
    }
    g_walker.speed = 0.0f;
    g_walker.sit = s;
    g_walker.pegs = s;
    g_walker.sit_y = seat[1] + sit_up; /* bassin au-dessus de la toile */
    g_walker.lean = lean * s;          /* v1.8.183 : dos contre le dossier (-0,10 : assis tout droit) */
    walker_axes(f, r);
    /* v1.8.185 : mains posées sur les accoudoirs, près du corps (v1.8.183 : sur les genoux, bras tendus) */
    g_walker.carry = s;
    for (k = 0; k < 2; ++k) {
        float sg = k == 0 ? -1.0f : 1.0f;
        if (red) {
            /* v1.8.194 : chaise rouge (pas d'accoudoir) : mains posées sur le milieu des cuisses, près du corps
               (avant : au niveau du bassin, repoussées devant le buste : bras tendus) */
            g_walker.grab_hand[k][0] = tx + fr[0] * 0.24f + r[0] * sg * 0.13f;
            g_walker.grab_hand[k][1] = seat[1] + 0.17f;
            g_walker.grab_hand[k][2] = tz + fr[2] * 0.24f + r[2] * sg * 0.13f;
            continue;
        }
        g_walker.grab_hand[k][0] = seat[0] + fr[0] * 0.10f + r[0] * sg * 0.25f;
        g_walker.grab_hand[k][1] = seat[1] + 0.21f;
        g_walker.grab_hand[k][2] = seat[2] + fr[2] * 0.10f + r[2] * sg * 0.25f;
    }
    for (k = 0; k < 2; ++k) {
        float sg = k == 0 ? -1.0f : 1.0f, px, pz;
        px = seat[0] + fr[0] * feet + r[0] * sg * 0.14f;
        pz = seat[2] + fr[2] * feet + r[2] * sg * 0.14f;
        g_walker.peg[k][0] = px;
        g_walker.peg[k][1] = walk_ground_height(&g_rd, px, pz) + g_walker.ankle_height;
        g_walker.peg[k][2] = pz;
    }
}

/* cône porté : socle devant lui, à hauteur de ventre ; mains sur le haut */
#define CARRY_FWD 0.50f
#define CARRY_UP 0.52f
static void carry_base(float out[3])
{
    float f[3], r[3], g = walk_ground_height(&g_rd, g_walker.pos[0], g_walker.pos[1]);
    walker_axes(f, r);
    out[0] = g_walker.pos[0] + f[0] * CARRY_FWD;
    out[1] = g + CARRY_UP;
    out[2] = g_walker.pos[1] + f[2] * CARRY_FWD;
}

static void hands_on_top(const float top[3])
{
    float f[3], r[3];
    int k;
    walker_axes(f, r);
    for (k = 0; k < 3; ++k) {
        g_walker.grab_hand[0][k] = top[k] - r[k] * 0.075f - f[k] * 0.03f;
        g_walker.grab_hand[1][k] = top[k] + r[k] * 0.075f - f[k] * 0.03f;
    }
}

static void cone_place_update(float dt)
{
    float t, grab, crouch, top[3], base[3];
    int k;
    if (!g_place.on)
        return;
    if (g_drive_state != DRIVE_ACTIVE || g_walker.state != WALK_ON || g_gesture.on || !cones_get(g_place.idx, base, NULL)) {
        /* interrompu (chute, moto, cône retiré…) : le cône est lâché là où il est */
        cones_hold_end(g_place.idx);
        if (!g_gesture.on) {
            g_walker.grab = 0.0f;
            g_walker.crouch = 0.0f;
        }
        g_walker.carry = 0.0f;
        g_place.on = 0;
        return;
    }
    g_place.t += dt;
    t = g_place.t;
    if (g_place.mode == PL_CARRY) {
        /* il marche en le portant devant lui */
        carry_base(base);
        cones_hold(g_place.idx, base[0], base[1], base[2], g_walker.heading + 0.7854f);
        for (k = 0; k < 3; ++k)
            top[k] = base[k];
        top[1] += 0.44f;
        hands_on_top(top);
        g_walker.carry = 1.0f;
        g_walker.grab = 0.0f;
        return;
    }
    if (g_place.mode == PL_PICK) {
        /* il se baisse, prend le cône par le haut (remis debout dans ses mains), se relève avec */
        float up = smooth01((t - PLACE_DOWN) / (PLACE_END - PLACE_LET)), cb[3];
        carry_base(cb);
        for (k = 0; k < 3; ++k)
            base[k] = g_place.from[k] + (cb[k] - g_place.from[k]) * up;
        base[1] = g_place.from[1] + g_cfg.cones_lift + (cb[1] - g_place.from[1] - g_cfg.cones_lift) * up;
        if (t >= PLACE_DOWN * 0.8f)
            cones_hold(g_place.idx, base[0], base[1], base[2], g_place.yaw);
        for (k = 0; k < 3; ++k)
            top[k] = base[k];
        top[1] += 0.44f;
        hands_on_top(top);
        grab = t < PLACE_DOWN ? smooth01(t / 0.2f) : 1.0f;
        crouch = 0.42f * (t < PLACE_DOWN ? smooth01(t / PLACE_DOWN) : 1.0f - up);
        g_walker.grab = grab * (1.0f - up);
        g_walker.carry = up;
        g_walker.crouch = crouch;
        if (t >= PLACE_DOWN + (PLACE_END - PLACE_LET)) {
            g_place.mode = PL_CARRY;
            g_walker.grab = 0.0f;
            g_walker.crouch = 0.0f;
            cone_msg("Cone en main (%d)", g_place.idx + 1);
        }
        return;
    }
    {
        /* poser : le cône descend des mains (à hauteur de poitrine) jusqu'au sol */
        float drop = 0.55f * (1.0f - smooth01(t / PLACE_DOWN));
        if (t < PLACE_LET)
            cones_hold(g_place.idx, g_place.x, g_place.y + g_cfg.cones_lift + drop, g_place.z, g_place.yaw);
        else if (t - dt < PLACE_LET)
            cones_hold_end(g_place.idx); /* lâché, posé */
        top[0] = g_place.x;
        top[1] = g_place.y + g_cfg.cones_lift + (t < PLACE_LET ? drop : 0.0f) + 0.44f;
        top[2] = g_place.z;
        hands_on_top(top);
        if (t < PLACE_DOWN)
            grab = smooth01(t / 0.2f);
        else if (t < PLACE_LET)
            grab = 1.0f;
        else
            grab = 1.0f - smooth01((t - PLACE_LET) / 0.35f);
        crouch = 0.42f * (t < PLACE_LET ? smooth01(t / PLACE_DOWN)
                                        : 1.0f - smooth01((t - PLACE_LET) / (PLACE_END - PLACE_LET)));
        g_walker.grab = grab;
        g_walker.carry = g_walker.carry > 0.0f ? (1.0f - smooth01(t / 0.2f)) : 0.0f; /* (s'il le portait) */
        g_walker.crouch = crouch;
        if (t >= PLACE_END) {
            g_walker.grab = 0.0f;
            g_walker.crouch = 0.0f;
            g_walker.carry = 0.0f;
            g_place.on = 0;
            log_line("cones : pose par le pilote (n %d)", g_place.idx);
        }
    }
}

/* v1.8.84 : petite botte portée : devant lui, à hauteur de ventre, en travers ; une main à chaque bout */
/* v1.8.85 : animations : il se baisse pour la ramasser (mains aux deux bouts, il la soulève), et pour la
   poser (il la descend devant lui, la lâche, se relève) */
#define BC_DOWN 0.45f /* se baisser */
#define BC_LIFT 0.65f /* soulever (ou se relever après l'avoir lâchée) */
#define BC_CROUCH 0.55f

static void bale_carry_spot(int var, float c[3], float *yaw)
{
    float f[3], r[3], g, hx, hy, hh;
    bales_variant_size(var, &hx, &hy, &hh);
    walker_axes(f, r);
    (void)r;
    g = walk_ground_height(&g_rd, g_walker.pos[0], g_walker.pos[1]);
    c[0] = g_walker.pos[0] + f[0] * (0.30f + hy);
    c[1] = g + 0.72f - (var >= 2 ? hh * 0.5f : 0.0f); /* v1.8.178 : mobilier (haut) porté plus bas */
    c[2] = g_walker.pos[1] + f[2] * (0.30f + hy);
    *yaw = g_walker.heading + 1.5707963f; /* axe long (e1) en travers */
}

/* mains aux deux bouts de la botte (centre du dessous c, cap yaw) */
static void bale_hands(int var, const float c[3], float yaw)
{
    float hx, hy, hh, e1[3] = { sinf(yaw), 0.0f, cosf(yaw) }, f[3], r[3];
    int k;
    bales_variant_size(var, &hx, &hy, &hh);
    walker_axes(f, r);
    /* la main gauche du côté gauche du pilote */
    if (e1[0] * r[0] + e1[2] * r[2] < 0.0f) {
        e1[0] = -e1[0];
        e1[2] = -e1[2];
    }
    for (k = 0; k < 3; ++k) {
        float m = c[k] + (k == 1 ? hh * 0.55f : 0.0f), sp = hx - 0.05f;
        if (var >= 2) {
            /* v1.8.178 : mobilier : mains sur le bord près de lui, écartées comme les épaules */
            if (sp > 0.32f)
                sp = 0.32f;
            m -= f[k] * (hy - 0.08f);
        }
        g_walker.grab_hand[0][k] = m - e1[k] * sp;
        g_walker.grab_hand[1][k] = m + e1[k] * sp;
    }
}

static void bale_start_put(int idx)
{
    float f[3], r[3], hx, hy, hh, h, x, z;
    int var = bales_get_var(idx);
    if (var < 0)
        return;
    bales_variant_size(var, &hx, &hy, &hh);
    walker_axes(f, r);
    (void)r;
    x = g_walker.pos[0] + f[0] * (0.38f + hy);
    z = g_walker.pos[1] + f[2] * (0.38f + hy);
    if (!(var >= 2 ? furn_ground(x, z, &h) : bale_ground(x, z, &h))) /* v1.8.186 : mobilier : le relief seul */
        h = walk_ground_height(&g_rd, x, z);
    {
        /* sur une autre botte s'il y en a une là */
        float hb;
        if (bales_height(x, z, h + 1.4f, &hb) && hb > h)
            h = hb;
    }
    g_bcarry.to[0] = x;
    g_bcarry.to[1] = h;
    g_bcarry.to[2] = z;
    g_bcarry.to[3] = g_walker.heading + 1.5707963f;
    g_bcarry.mode = BC_PUT;
    g_bcarry.t = 0.0f;
    log_line("bottes : petite botte %d posee par le pilote", idx);
}

static void bale_carry_update(float dt)
{
    float c[3], yaw, t;
    int k, var;
    if (!g_bcarry.on)
        return;
    var = bales_get_var(g_bcarry.idx);
    if (var < 0 || g_drive_state != DRIVE_ACTIVE || g_walker.state != WALK_ON || g_gesture.on) {
        /* chute, moto, botte retirée : lâchée là où elle est */
        bales_hold_end(g_bcarry.idx);
        g_bcarry.on = 0;
        g_walker.carry = 0.0f;
        g_walker.grab = 0.0f;
        g_walker.crouch = 0.0f;
        return;
    }
    g_bcarry.t += dt;
    t = g_bcarry.t;
    if (g_bcarry.mode == BC_PICK) {
        /* se baisse (mains sur les bouts au sol), puis se relève en la soulevant */
        float up = smooth01((t - BC_DOWN) / BC_LIFT), cc[3], cy;
        bale_carry_spot(var, cc, &cy);
        {
            float dy = cy - g_bcarry.from[3];
            while (dy > 3.14159265f)
                dy -= 6.2831853f;
            while (dy < -3.14159265f)
                dy += 6.2831853f;
            if (fabsf(dy) > 1.5707963f) /* (une botte est pareille retournée) */
                dy += dy > 0.0f ? -3.14159265f : 3.14159265f;
            yaw = g_bcarry.from[3] + dy * up;
        }
        for (k = 0; k < 3; ++k)
            c[k] = g_bcarry.from[k] + (cc[k] - g_bcarry.from[k]) * up;
        if (t >= BC_DOWN * 0.9f)
            bales_hold(g_bcarry.idx, c[0], c[1], c[2], yaw);
        bale_hands(var, c, yaw);
        g_walker.crouch = BC_CROUCH * (t < BC_DOWN ? smooth01(t / BC_DOWN) : 1.0f - up);
        g_walker.grab = (t < BC_DOWN ? smooth01(t / 0.25f) : 1.0f) * (1.0f - up);
        g_walker.carry = up;
        if (t >= BC_DOWN + BC_LIFT) {
            g_bcarry.mode = BC_CARRY;
            g_walker.crouch = 0.0f;
            g_walker.grab = 0.0f;
            snprintf(g_msg, sizeof(g_msg), tr("%s en main"), tr(bales_variant_name(bales_get_var(g_bcarry.idx)))); /* v1.8.186 */
            g_msg_t = now_seconds();
        }
        return;
    }
    if (g_bcarry.mode == BC_PUT) {
        /* la descend devant lui jusqu'au sol, la lâche, se relève */
        float down = smooth01(t / BC_DOWN), cc[3], cy;
        bale_carry_spot(var, cc, &cy);
        if (t < BC_DOWN) {
            for (k = 0; k < 3; ++k)
                c[k] = cc[k] + (g_bcarry.to[k] - cc[k]) * down;
            yaw = g_bcarry.to[3];
            bales_hold(g_bcarry.idx, c[0], c[1], c[2], yaw);
        } else {
            if (t - dt < BC_DOWN) {
                bales_hold(g_bcarry.idx, g_bcarry.to[0], g_bcarry.to[1], g_bcarry.to[2], g_bcarry.to[3]);
                bales_hold_end(g_bcarry.idx); /* lâchée, posée */
                bales_set_spawn(g_bcarry.idx, g_bcarry.to[0], g_bcarry.to[1], g_bcarry.to[2], g_bcarry.to[3]);
            }
            memcpy(c, g_bcarry.to, sizeof(c));
            yaw = g_bcarry.to[3];
        }
        bale_hands(var, c, yaw);
        {
            float rise = smooth01((t - BC_DOWN) / BC_LIFT);
            g_walker.crouch = BC_CROUCH * (t < BC_DOWN ? down : 1.0f - rise);
            g_walker.carry = t < BC_DOWN ? 1.0f - smooth01(t / 0.2f) : 0.0f;
            g_walker.grab = t < BC_DOWN ? smooth01(t / 0.2f) : 1.0f - smooth01((t - BC_DOWN) / 0.3f);
        }
        if (t >= BC_DOWN + BC_LIFT) {
            g_walker.crouch = 0.0f;
            g_walker.grab = 0.0f;
            g_walker.carry = 0.0f;
            g_bcarry.on = 0;
        }
        return;
    }
    /* portée en marchant */
    bale_carry_spot(var, c, &yaw);
    bales_hold(g_bcarry.idx, c[0], c[1], c[2], yaw);
    bale_hands(var, c, yaw);
    g_walker.carry = 1.0f;
    g_walker.grab = 0.0f;
    g_walker.crouch = 0.0f;
}

/* à chaque image : ouvrir, viser, choisir */
static void cone_menu_update(void)
{
    static int prev = 0;
    int on_foot = g_drive_state == DRIVE_ACTIVE && g_walker.state == WALK_ON && !g_gesture.on && g_visual_allowed;
    int cur = 0, edge, open_btn, n;
    WheelItem it[WHEEL_MAXN];
    PadState ps;
    int pr = 0;
    enum { B_OPEN = 1, B_UP = 2, B_DOWN = 4, B_OK = 8, B_BACK = 16, B_PREV = 32, B_NEXT = 64 };
    if (g_cone_menu && (!on_foot || equip_menu_open()))
        cone_menu_close();
    if (!game_focused() || !on_foot)
        g_pad_zoom = 0;
    if (game_focused()) {
        /* B du clavier (sans Ctrl), flèche Bas de la croix de la manette */
        if (key_down('B') && !(GetAsyncKeyState(VK_CONTROL) & 0x8000))
            cur |= B_OPEN;
        if (key_down(VK_UP))
            cur |= B_UP;
        if (key_down(VK_DOWN))
            cur |= B_DOWN;
        if (key_down(VK_RETURN) || key_down(VK_LBUTTON))
            cur |= B_OK;
        if (key_down(VK_BACK) || key_down(VK_RBUTTON))
            cur |= B_BACK;
        pr = padinject_read(&ps);
        if (pr) {
            /* v1.8.163 : croix Bas : appui court = roue ; tenue = zoom arrière (Haut = zoom avant) */
            static double dn_t = -1.0;
            int dn = (ps.buttons & 0x0002) != 0;
            double tnow = now_seconds();
            if (g_cone_menu) {
                if (dn)
                    cur |= B_OPEN;
                dn_t = -1.0;
                g_pad_zoom = 0;
            } else {
                if (dn && dn_t < 0.0)
                    dn_t = tnow;
                if (!dn && dn_t >= 0.0) {
                    if (tnow - dn_t < 0.3)
                        cur |= B_OPEN; /* relâchée vite : la roue s'ouvre */
                    dn_t = -1.0;
                }
                g_pad_zoom = on_foot ? ((ps.buttons & 0x0001) ? -1 : (dn && dn_t >= 0.0 && tnow - dn_t >= 0.3) ? 1 : 0) : 0;
            }
            if (ps.buttons & 0x1000)
                cur |= B_OK;
            if (ps.buttons & 0x2000)
                cur |= B_BACK;
        }
    }
    open_btn = (cur & B_OPEN) != 0;
    (void)open_btn;
    if (pr) {
        if (ps.buttons & 0x0100)
            cur |= B_PREV; /* LB */
        if (ps.buttons & 0x0200)
            cur |= B_NEXT; /* RB */
    }
    if (game_focused()) {
        if (key_down(VK_LEFT) || key_down(VK_PRIOR))
            cur |= B_PREV;
        if (key_down(VK_RIGHT) || key_down(VK_NEXT))
            cur |= B_NEXT;
    }
    edge = cur & ~prev;
    prev = cur;
    if (!g_cone_menu) {
        /* un appui ouvre la roue ; elle reste ouverte jusqu'au choix (comme Fortnite) */
        if (on_foot && !equip_menu_open() && (edge & B_OPEN)) {
            g_cone_menu = 1;
            g_wheel_lvl = WL_MAIN; /* toujours le premier écran */
            wheel_reset_aim();
            g_wheel_t = now_seconds();
            log_line("cones : roue ouverte, page %d (%d cones)", g_wheel_lvl + 1, cones_count());
        }
        return;
    }
    /* rappuyer sur Bas / B : fermer ; B (manette), clic droit, Retour arrière : retour au premier écran,
       puis fermer */
    if (edge & B_OPEN) {
        cone_menu_close();
        return;
    }
    if (edge & B_BACK) {
        if (g_wheel_lvl == WL_MAIN)
            cone_menu_close();
        else
            cone_menu_action(WA_BACK);
        return;
    }
    /* options des cônes : pages avec LB / RB, flèches Gauche / Droite, Page préc. / suiv. */
    if ((edge & (B_PREV | B_NEXT)) && !(cur & B_OK) && g_wheel_lvl != WL_MAIN) {
        /* (v1.8.56 : pas en même temps que A : chez le joueur, valider « Poser » changeait de page) */
        log_line("cones : roue, page %s (manette %04x, gauche %d droite %d page prec %d suiv %d)",
                 (edge & B_NEXT) ? "suivante" : "precedente", pr ? ps.buttons : 0, key_down(VK_LEFT) ? 1 : 0,
                 key_down(VK_RIGHT) ? 1 : 0, key_down(VK_PRIOR) ? 1 : 0, key_down(VK_NEXT) ? 1 : 0);
        /* v1.8.82 : pages du même groupe (cônes, ou bottes de foin) */
        g_wheel_lvl = WL_GROUP(g_wheel_lvl) * 2 + (g_wheel_lvl % 2 + 1) % 2;
        wheel_reset_aim();
        return;
    }
    n = wheel_items(it);
    /* surligner : stick (droit ou gauche) ou souris ; la case reste surlignée quand on lâche */
    if (pr) {
        if (ps.rx * ps.rx + ps.ry * ps.ry > 0.3f)
            g_wheel_sel = wheel_pick(it, n, wheel_sector(ps.rx, ps.ry, n));
        else if (ps.lx * ps.lx + ps.ly * ps.ly > 0.3f)
            g_wheel_sel = wheel_pick(it, n, wheel_sector(ps.lx, ps.ly, n));
    }
    if (g_wheel_px * g_wheel_px + g_wheel_py * g_wheel_py > 30.0f * 30.0f)
        g_wheel_sel = wheel_pick(it, n, wheel_sector(g_wheel_px, -g_wheel_py, n));
    if (edge & B_UP)
        g_wheel_sel = wheel_pick(it, n, wheel_sector(0.0f, 1.0f, n));
    if (edge & B_DOWN)
        g_wheel_sel = wheel_pick(it, n, wheel_sector(0.0f, -1.0f, n));
    {
        /* journal : case surlignée */
        static int last_sel = -2;
        if (g_wheel_sel != last_sel) {
            last_sel = g_wheel_sel;
            log_line("cones : roue, ecran %d, case surlignee %d", g_wheel_lvl, g_wheel_sel);
        }
    }
    /* valider : A, clic gauche, Entrée */
    if (edge & B_OK) {
        log_line("cones : roue, valider (case %d)", g_wheel_sel);
        if (g_wheel_sel >= 0 && !cone_menu_action(it[g_wheel_sel].act))
            cone_menu_close();
    }
}

/* petit cône dessiné à plat (icône), pointe en (x, y0), base en y1, couleur du corps rgb (0xBBGGRR) */
static int wheel_icon(SPluginQuad_t *q, float x, float y0, float y1, float asp, unsigned long rgb)
{
    float h = y1 - y0, w = h * 0.42f / asp;
    float yb = y0 + h * 0.30f, yb2 = y0 + h * 0.42f, yc = y0 + h * 0.58f, yc2 = y0 + h * 0.70f;
    int n = 0, k;
    set_tri(&q[n++], x, y0, x + w * 0.85f, y1 - h * 0.08f, x - w * 0.85f, y1 - h * 0.08f, 0xFF000000UL | rgb);
    /* deux bandes blanches */
    for (k = 0; k < 2; ++k) {
        float ya = k ? yc : yb, yz = k ? yc2 : yb2;
        float wa = w * 0.85f * (ya - y0) / (h * 0.92f), wb = w * 0.85f * (yz - y0) / (h * 0.92f);
        SPluginQuad_t *t = &q[n++];
        t->m_aafPos[0][0] = x - wa; t->m_aafPos[0][1] = ya;
        t->m_aafPos[1][0] = x + wa; t->m_aafPos[1][1] = ya;
        t->m_aafPos[2][0] = x + wb; t->m_aafPos[2][1] = yz;
        t->m_aafPos[3][0] = x - wb; t->m_aafPos[3][1] = yz;
        t->m_iSprite = 0;
        t->m_ulColor = 0xFFE8E8E8;
        quad_wind(t);
    }
    set_quad(&q[n++], x - w, y1 - h * 0.08f, x + w, y1, 0xFF151515);
    return n;
}

/* v1.8.82 : petite botte de foin dessinée à plat (icône) : ronde (debout, dessus plus clair) ou petite
   (bloc avec deux ficelles) ; haut en y0, bas en y1 ; v1.8.83 : plus que ces deux-là */
static int wheel_icon_bale(SPluginQuad_t *q, float x, float y0, float y1, float asp, unsigned long rgb, int var)
{
    float h = y1 - y0, w = h * 1.1f / asp;
    int n = 0;
    if (var == 0) {
        /* ronde, debout : rectangle aux coins arrondis, dessus plus clair */
        set_quad(&q[n++], x - w * 0.75f, y0 + h * 0.18f, x + w * 0.75f, y1, 0xFF000000UL | rgb);
        set_quad(&q[n++], x - w * 0.75f, y0, x + w * 0.75f, y0 + h * 0.22f, 0xFF000000UL | ((rgb >> 1) & 0x7F7F7FUL) | 0x404040UL);
        return n;
    }
    if (var == 2) {
        /* v1.8.178 : table : plateau et pieds */
        w *= 1.1f;
        set_quad(&q[n++], x - w, y0 + h * 0.15f, x + w, y0 + h * 0.32f, 0xFF000000UL | rgb);
        set_quad(&q[n++], x - w * 0.8f, y0 + h * 0.32f, x - w * 0.68f, y1, 0xFF505050UL);
        set_quad(&q[n++], x + w * 0.68f, y0 + h * 0.32f, x + w * 0.8f, y1, 0xFF505050UL);
        return n;
    }
    if (var >= 3) {
        /* chaise : dossier, assise, pieds */
        w *= 0.6f;
        set_quad(&q[n++], x - w * 0.9f, y0, x - w * 0.55f, y0 + h * 0.62f, 0xFF000000UL | rgb);
        set_quad(&q[n++], x - w * 0.9f, y0 + h * 0.5f, x + w, y0 + h * 0.65f, 0xFF000000UL | rgb);
        set_quad(&q[n++], x - w * 0.85f, y0 + h * 0.65f, x - w * 0.7f, y1, 0xFF404040UL);
        set_quad(&q[n++], x + w * 0.75f, y0 + h * 0.65f, x + w * 0.9f, y1, 0xFF404040UL);
        return n;
    }
    w *= 0.75f; /* petite */
    set_quad(&q[n++], x - w, y0 + h * 0.25f, x + w, y1, 0xFF000000UL | rgb);
    set_quad(&q[n++], x - w * 0.85f, y0, x + w * 1.0f, y0 + h * 0.28f, 0xFF000000UL | ((rgb >> 1) & 0x7F7F7FUL) | 0x505050UL);
    /* ficelles */
    set_quad(&q[n++], x - w * 0.45f, y0 + h * 0.05f, x - w * 0.33f, y1, 0xFF303848UL);
    set_quad(&q[n++], x + w * 0.33f, y0 + h * 0.05f, x + w * 0.45f, y1, 0xFF303848UL);
    return n;
}

/* v1.8.122 : icône de rampe : le vrai profil de la forme (vu de côté, élan à gauche), remplie de sa couleur,
   dessus clair (visible même en noir) ; boîte : centre x, haut y0, bas y1, largeur w (déjà divisée par l'aspect) */
static int wheel_icon_ramp(SPluginQuad_t *q, float x, float y0, float y1, float w, unsigned long rgb, int s)
{
    const int seg = 16;
    float h = y1 - y0, line = h * 0.09f;
    unsigned long r = rgb & 0xFF, g = (rgb >> 8) & 0xFF, b = (rgb >> 16) & 0xFF;
    unsigned long fill, top;
    int n = 0, k;
    /* trop sombre (noir) : gris, pour qu'on la voie sur le fond de la roue */
    if (r * 3 + g * 6 + b < 700)
        r = g = b = 0x58;
    fill = 0xFF000000UL | r | (g << 8) | (b << 16);
    top = 0xFFF0F0F0UL;
    /* sol */
    set_quad(&q[n++], x - w * 0.5f - 0.004f, y1, x + w * 0.5f + 0.004f, y1 + 0.003f, 0xA0C8C8C8UL);
    for (k = 0; k < seg; ++k) {
        float u0 = (float)k / seg, u1 = (float)(k + 1) / seg;
        float xa = x - w * 0.5f + w * u0, xb = x - w * 0.5f + w * u1;
        float ha = h * ramps_icon_prof(s, u0), hb = h * ramps_icon_prof(s, u1);
        SPluginQuad_t *p = &q[n++];
        p->m_aafPos[0][0] = xa; p->m_aafPos[0][1] = y1 - ha;
        p->m_aafPos[1][0] = xa; p->m_aafPos[1][1] = y1;
        p->m_aafPos[2][0] = xb; p->m_aafPos[2][1] = y1;
        p->m_aafPos[3][0] = xb; p->m_aafPos[3][1] = y1 - hb;
        p->m_iSprite = 0;
        p->m_ulColor = fill;
        quad_wind(p);
        p = &q[n++];
        p->m_aafPos[0][0] = xa; p->m_aafPos[0][1] = y1 - ha - line;
        p->m_aafPos[1][0] = xa; p->m_aafPos[1][1] = y1 - ha;
        p->m_aafPos[2][0] = xb; p->m_aafPos[2][1] = y1 - hb;
        p->m_aafPos[3][0] = xb; p->m_aafPos[3][1] = y1 - hb - line;
        p->m_iSprite = 0;
        p->m_ulColor = top;
        quad_wind(p);
    }
    return n;
}

static int cone_menu_draw(SPluginQuad_t *q, int max_q, SPluginString_t *s, int max_s, int *ns)
{
    const int SUB = 6;
    float asp = g_cfg.screen_aspect > 0.1f ? g_cfg.screen_aspect : 1.7778f;
    const float cx = 0.5f, cy = 0.5f, ri = 0.085f, ro = 0.245f;
    WheelItem it[WHEEL_MAXN];
    int nq = 0, n = 0, k, j, ni;
    float span;
    char t[64];
    static const char *k_title[WL_PAGES + 1] = { "Cones", "Variantes", "Bottes de foin", "Bottes", "Rampes FMX", "Forme", "Mobilier", "Objet", "Menu" };
    *ns = 0;
    if (!g_cone_menu)
        return 0;
    ni = wheel_items(it);
    if (max_q < ni * (SUB + 4) + 40)
        return 0;
    span = 6.2831853f / (float)ni;
    for (k = 0; k < ni; ++k) {
        int sel = k == g_wheel_sel, empty = it[k].act == WA_NONE;
        int cur_var = (it[k].act >= WA_VAR0 && it[k].act < WA_RSHAPE0 && it[k].act - WA_VAR0 == cones_variant()) ||
                      (it[k].act >= WA_BVAR0 && it[k].act < WA_VAR0 && it[k].act - WA_BVAR0 == bales_variant()) ||
                      (it[k].act >= WA_RSHAPE0 && it[k].act < WA_RCOL0 && it[k].act - WA_RSHAPE0 == ramps_shape()) ||
                      0;
        float a0 = k * span - span * 0.5f + 0.03f, a1 = k * span + span * 0.5f - 0.03f;
        float r1 = sel ? ro + 0.015f : ro, r0 = ri + 0.006f;
        unsigned long col = sel ? 0xF00080FFUL : (empty ? 0x90181410UL : (cur_var ? 0xD0303C46UL : 0xC8201A16UL));
        for (j = 0; j < SUB; ++j) {
            float b0 = a0 + (a1 - a0) * j / SUB, b1 = a0 + (a1 - a0) * (j + 1) / SUB;
            SPluginQuad_t *t2 = &q[nq++];
            t2->m_aafPos[0][0] = cx + r0 * sinf(b0) / asp; t2->m_aafPos[0][1] = cy - r0 * cosf(b0);
            t2->m_aafPos[1][0] = cx + r1 * sinf(b0) / asp; t2->m_aafPos[1][1] = cy - r1 * cosf(b0);
            t2->m_aafPos[2][0] = cx + r1 * sinf(b1) / asp; t2->m_aafPos[2][1] = cy - r1 * cosf(b1);
            t2->m_aafPos[3][0] = cx + r0 * sinf(b1) / asp; t2->m_aafPos[3][1] = cy - r0 * cosf(b1);
            t2->m_iSprite = 0;
            t2->m_ulColor = col;
            quad_wind(t2);
        }
        if (empty)
            continue;
        {
            float am = k * span, rm = (ri + ro) * 0.5f + (sel ? 0.008f : 0.0f);
            float mx = cx + rm * sinf(am) / asp, my = cy - rm * cosf(am);
            int has_icon = it[k].icon > 0;
            if (has_icon && it[k].icon == 69) {
                /* v1.8.122 : couleur : pastille */
                if (nq + 2 < max_q - 40) {
                    float d = 0.016f;
                    set_quad(&q[nq++], mx - (d + 0.003f) / asp, my - 0.050f - 0.003f, mx + (d + 0.003f) / asp, my - 0.050f + 2 * d + 0.003f,
                             0xFFD8D8D8UL);
                    set_quad(&q[nq++], mx - d / asp, my - 0.050f, mx + d / asp, my - 0.050f + 2 * d,
                             ramps_shape_fixed(ramps_shape()) ? 0xFF6A6A6AUL : 0xFF000000UL | ramps_color_rgb(ramps_color()));
                }
            } else if (has_icon && it[k].icon >= 70) {
                /* v1.8.122 : rampe : 70 forme et couleur choisies, 80 + s forme s ; profil réel, taille selon la case */
                int shp = it[k].icon >= 80 ? it[k].icon - 80 : ramps_shape();
                float bw = (ni >= 8 ? 0.058f : ni >= 6 ? 0.070f : 0.095f);
                if (nq + 40 < max_q - 40)
                    nq += wheel_icon_ramp(&q[nq], mx, my - 0.052f, my - 0.010f, bw / asp,
                                          ramps_shape_fixed(shp) ? 0x3C78B4UL : ramps_color_rgb(ramps_color()), shp);
            } else if (has_icon && it[k].icon >= 30) {
                unsigned long rgb = bales_variant_rgb(it[k].icon >= 31 ? it[k].icon - 31 : bales_variant());
                nq += wheel_icon_bale(&q[nq], mx, my - 0.055f, my - 0.012f, asp, rgb,
                                      it[k].icon >= 31 ? it[k].icon - 31 : bales_variant());
            } else if (has_icon) {
                unsigned long rgb = cones_variant_rgb(it[k].icon >= 2 ? it[k].icon - 2 : cones_variant());
                nq += wheel_icon(&q[nq], mx, my - 0.060f, my - 0.010f, asp, rgb);
            }
            if (g_font_loaded && n + 3 <= max_s) {
                unsigned long tc = sel ? 0xFFFFFFFF : 0xFFD8D8D8;
                float ty = has_icon ? my + 0.002f : my - 0.012f;
                set_string(&s[n], tr(it[k].l1), mx, ty, 0.018f, tc);
                s[n++].m_iJustify = 1;
                if (it[k].l2[0]) {
                    set_string(&s[n], tr(it[k].l2), mx, ty + 0.022f, 0.018f, it[k].icon >= 69 && !sel ? 0xFF60D0FF : tc);
                    s[n++].m_iJustify = 1;
                    if (cur_var && n < max_s) {
                        set_string(&s[n], tr("(choisie)"), mx, ty + 0.042f, 0.016f, 0xFF60D0FF);
                        s[n++].m_iJustify = 1;
                    }
                } else if (cur_var) {
                    set_string(&s[n], tr("(choisie)"), mx, ty + 0.022f, 0.016f, 0xFF60D0FF);
                    s[n++].m_iJustify = 1;
                }
            }
        }
    }
    /* centre : disque, titre du niveau, nombre de cônes */
    for (j = 0; j < 16; ++j) {
        float b0 = j * 0.3926991f, b1 = (j + 1) * 0.3926991f, r = ri - 0.006f;
        set_tri(&q[nq++], cx, cy, cx + r * sinf(b0) / asp, cy - r * cosf(b0), cx + r * sinf(b1) / asp, cy - r * cosf(b1),
                g_wheel_sel < 0 ? 0xE0303030UL : 0xD0141414UL);
    }
    if (g_wheel_sel >= 0) {
        float am = g_wheel_sel * span, r = ri - 0.018f;
        float px = cx + r * sinf(am) / asp, py = cy - r * cosf(am), d = 0.010f;
        set_tri(&q[nq++], px + d * sinf(am) / asp, py - d * cosf(am), px + d * cosf(am) / asp, py + d * sinf(am),
                px - d * cosf(am) / asp, py - d * sinf(am), 0xFF0080FF);
    }
    if (g_font_loaded && n + 3 <= max_s) {
        set_string(&s[n], tr(k_title[g_wheel_lvl]), cx, cy - 0.028f, 0.022f, 0xFFFFFFFF);
        s[n++].m_iJustify = 1;
        {
            snprintf(t, sizeof(t), "%d", g_wheel_lvl != WL_MAIN && WL_GROUP(g_wheel_lvl) == 3 ? (bales_set_cat(1), bales_count_cat())
                                         : g_wheel_lvl != WL_MAIN && WL_GROUP(g_wheel_lvl) == 2 ? ramps_count()
                                         : g_wheel_lvl != WL_MAIN && WL_GROUP(g_wheel_lvl) == 1 ? (bales_set_cat(0), bales_count_cat()) : cones_count());
            set_string(&s[n], t, cx, cy - 0.002f, 0.026f, 0xFF60B0FF);
            s[n++].m_iJustify = 1;
        }
        /* v1.8.109 : avec une manette PlayStation, symboles dessinés (croix, rond...) */
        padicon_text(q, max_q, &nq, s, max_s, &n,
                     tr(g_wheel_lvl == WL_MAIN ? "Vise (stick, souris), A / clic : choisir   -   B : fermer"
                                               : "Vise (stick, souris), A / clic : choisir   -   LB / RB, fleches : page   -   B : retour"),
                     cx, cy + ro + 0.03f, 0.016f, 0xFFE0E0E0, 1, asp);
    }
    /* v1.8.54 : pages : points au-dessus de la roue, page en cours en orange, flèches LB / RB */
    if (g_wheel_lvl != WL_MAIN) {
        int pg;
        float dy = cy - ro - 0.035f, gap = 0.022f / asp, x0 = cx - gap * (2 - 1) * 0.5f, d = 0.006f;
        for (pg = 0; pg < 2 && nq + 1 < max_q; ++pg) {
            int lv = WL_GROUP(g_wheel_lvl) * 2 + pg;
            float x = x0 + gap * pg, dd = lv == g_wheel_lvl ? d * 1.4f : d;
            set_quad(&q[nq++], x - dd / asp, dy - dd, x + dd / asp, dy + dd, lv == g_wheel_lvl ? 0xFF0080FFUL : 0xC0A0A0A0UL);
        }
        if (g_font_loaded && n + 2 <= max_s) {
            set_string(&s[n], padstyle_text("LB  <"), x0 - 0.03f / asp - 0.03f, dy - 0.011f, 0.018f, 0xFFE0E0E0);
            s[n++].m_iJustify = 1;
            set_string(&s[n], padstyle_text(">  RB"), x0 + gap * (2 - 1) + 0.03f / asp + 0.03f, dy - 0.011f, 0.018f,
                       0xFFE0E0E0);
            s[n++].m_iJustify = 1;
        }
    }
    *ns = n;
    return nq;
}

/* ---------- journal ---------- */

static void log_line(const char *fmt, ...)
{
    va_list args;
    if (!g_log)
        return;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
#ifdef _WIN32
    {
        /* v1.8.116 : écrit sur le disque au plus toutes les 0,5 s (avant : à chaque ligne ; une chute en
           écrit plus de 100 d'un coup : à-coup de l'image). Le reste part à l'image suivante (Draw). */
        static DWORD last = 0;
        DWORD now = GetTickCount();
        if (now - last > 500) {
            last = now;
            fflush(g_log);
        }
    }
#else
    fflush(g_log);
#endif
}
static void log_flush_tick(void)
{
#ifdef _WIN32
    static DWORD last = 0;
    if (g_log && GetTickCount() - last > 500) {
        last = GetTickCount();
        fflush(g_log);
    }
#endif
}

/* ---------- exports obligatoires ---------- */

MXB_EXPORT char *GetModID(void)
{
    static char id[] = MXB_MOD_ID;
    return id;
}

MXB_EXPORT int GetModDataVersion(void)
{
    return MXB_MOD_DATA_VERSION;
}

MXB_EXPORT int GetInterfaceVersion(void)
{
    return MXB_INTERFACE_VERSION;
}

/* Appelé par l'interception après chaque mise à jour de modèle par le jeu */
/* v1.8.116 : temps passé dans le plugin pendant l'image en cours (pour trouver les à-coups) */
static double g_fr_tele = 0.0, g_fr_skel = 0.0, g_fr_tele_max = 0.0;
static double fr_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return f.QuadPart ? (double)c.QuadPart * 1e6 / (double)f.QuadPart : 0.0;
#else
    return 0.0;
#endif
}
static void on_skeleton_impl(void *skeleton);
static void on_skeleton(void *skeleton)
{
    double t0 = fr_now_us();
    on_skeleton_impl(skeleton);
    g_fr_skel += fr_now_us() - t0;
}
static void on_skeleton_impl(void *skeleton)
{
    skelscan_record(skeleton);
    /* v1.7.30 : appris seulement hors ligne : en ligne, le modèle 3D trouvé n'est pas toujours celui de
       sa moto (vu sur une Fantic : bassin appris 0,2 m trop en arrière et trop haut) */
    if (g_drive_state == DRIVE_IDLE && !g_dismounted && g_ride_ok && !g_learn_off)
        ridepose_sample(skeleton, g_last_bike_pos, g_own_skel); /* v1.7 : le jeu pose le pilote sur la moto */
    if (g_drive_state == DRIVE_IDLE && !g_dismounted) {
        /* v1.8.80 : os de la tête de notre pilote sur la moto (le casque apprend sa place dessus) */
        int kind = skelscan_kind(skelscan_record(skeleton));
        float mw[12], hw[12];
        if ((kind == SKEL_KIND_RIDER_HRC || kind == SKEL_KIND_RIDER_EDF) && skelscan_model_world(skeleton, mw)) {
            float dx = mw[3] - g_last_bike_pos[0], dy = mw[7] - g_last_bike_pos[1], dz = mw[11] - g_last_bike_pos[2];
            if ((skeleton == g_own_skel || dx * dx + dy * dy + dz * dz < 1.5f * 1.5f) &&
                skelscan_node_world(skeleton, "riderRIG_Head", hw))
                equip_set_ride_head(hw);
        }
    } else
        equip_set_ride_head(NULL);
    equip_after_update(skeleton, g_visual_allowed); /* casque et bottes : décalage réglé (touche P) */
    if (g_drive_state == DRIVE_ACTIVE && g_walker.state == WALK_ON && skeleton == drive_skeleton() && !g_gesture.on &&
        !bike_lift_active())
        drive_wrists_post(skeleton, fabsf(g_walker.speed) > WALK_SPEED + 0.4f); /* poignets de la marche en courant */
    /* (v0.29.4 : la copie dans +0x1C0 cassait le pilote : ce n'est pas une matrice de rendu) */
    {
        /* pilote en mod : 1 s après le départ, le jeu a-t-il gardé nos os ? (une fois par modèle) */
        static void *checked_mod = NULL, *seen_mod = NULL;
        static double seen_t = 0.0;
        if (g_drive_state == DRIVE_ACTIVE && skeleton == drive_skeleton() && skeleton != seen_mod) {
            seen_mod = skeleton;
            seen_t = now_seconds();
        }
        if (g_drive_state == DRIVE_ACTIVE && skeleton == drive_skeleton() && skeleton != checked_mod &&
            skelscan_kind(skelscan_record(skeleton)) == SKEL_KIND_RIDER_EDF && now_seconds() - seen_t > 1.5) {
            static int nodes_logged = 0; /* v1.8.116 : liste des os une fois par partie */
            checked_mod = skeleton;
            drive_check_overwritten(skeleton, log_line);
            if (!nodes_logged++)
                drive_log_nodes(log_line);
        }
    }
    {
        /* descendu sans chute : le jeu réécrit-il des os (bras collés au guidon) ? une fois par
           descente, 1 s après qu'il ait lâché */
        static int checked = 0;
        if (!g_dismounted)
            checked = 0;
        else if (!checked && g_vol_phase == 2 && g_vol_free > 1.0f && skeleton == drive_skeleton()) {
            checked = 1;
            drive_check_overwritten(skeleton, log_line);
            skelscan_log_wrists(g_rd.pos[RD_PELVIS], 4.0f, log_line);
            {
                /* où le jeu garde-t-il les mains sur les poignées ? (leviers du guidon) */
                float pts[2][3];
                int np = 0;
                if (skelscan_find_node(g_tele_pos, 2.5f, "clutch_lever", pts[np]))
                    ++np;
                if (skelscan_find_node(g_tele_pos, 2.5f, "brake_lever", pts[np]))
                    ++np;
                log_line("main : recherche des poignees (%d leviers) dans les modeles du pilote", np);
                if (np)
                    skelscan_find_points(g_rd.pos[RD_PELVIS], 4.0f, (const float(*)[3])pts, np, 0.12f, log_line);
            }
            {
                const float *pv = g_rd.pos[RD_PELVIS];
                log_line("bras : grab %.2f, epaule g %+.2f %+.2f %+.2f, coude g %+.2f %+.2f %+.2f, main g %+.2f %+.2f %+.2f, "
                         "main d %+.2f %+.2f %+.2f (par rapport au bassin), cadre marche %d",
                         g_walker.grab, g_rd.pos[RD_SHOULDER_L][0] - pv[0], g_rd.pos[RD_SHOULDER_L][1] - pv[1],
                         g_rd.pos[RD_SHOULDER_L][2] - pv[2], g_rd.pos[RD_ELBOW_L][0] - pv[0], g_rd.pos[RD_ELBOW_L][1] - pv[1],
                         g_rd.pos[RD_ELBOW_L][2] - pv[2], g_rd.pos[RD_HAND_L][0] - pv[0], g_rd.pos[RD_HAND_L][1] - pv[1],
                         g_rd.pos[RD_HAND_L][2] - pv[2], g_rd.pos[RD_HAND_R][0] - pv[0], g_rd.pos[RD_HAND_R][1] - pv[1],
                         g_rd.pos[RD_HAND_R][2] - pv[2], g_walker.grab < 0.01f);
                {
                    float w[12];
                    void *sk = drive_skeleton();
                    (void)sk;
                    (void)w;
                }
            }
        }
    }
}

/* Tests d'écriture des versions 0.3.x (write_test = 1) */
static void write_test_pre(void *skeleton, int phase)
{
    const float quarter = 1.5707963f; /* 90 degrés */
    int idx = skelscan_record(skeleton);
    if (skelscan_kind(idx) != SKEL_KIND_RIDER_HRC)
        return;
    if (phase == 1) {
        g_write_counts[1] = skelscan_rotate_local(skeleton, 24, quarter) +
                            skelscan_rotate_local(skeleton, 27, quarter) +
                            skelscan_rotate_local(skeleton, 50, quarter);
    } else if (phase == 2) {
        g_write_counts[2] = skelscan_rotate_local(skeleton, 18, quarter);
    }
}

/* Ragdoll 3D : départ, simulation et écriture des os, juste avant la mise à jour */
static void drive_pre(void *skeleton)
{
    int state = g_drive_state;
    int idx = skelscan_record(skeleton);
    {
        /* le rider.hrc du pilote ; un mod de pilote peut charger directement son rider.edf :
           on le prend s'il n'y a pas de rider.hrc (petite attente) */
        /* sans rider.hrc (mods) : le rider.edf du mod */
        int kind = skelscan_kind(idx);
        {
            /* diagnostic : modèles du pilote vus pendant l'attente du squelette (une fois chacun) */
            static void *seen[8];
            static int nseen = 0, req_id = -1;
            int i, known = 0;
            if (state == DRIVE_REQUESTED && kind >= SKEL_KIND_RIDER_EDF) {
                if (req_id != g_drive_req_id) {
                    req_id = g_drive_req_id;
                    nseen = 0;
                }
                for (i = 0; i < nseen; ++i)
                    known |= seen[i] == skeleton;
                if (!known && nseen < 8) {
                    seen[nseen++] = skeleton;
                    log_line("pilotage : candidat %s (type %d), chute %d, attente %.2f s", skelscan_name(idx), kind,
                             g_was_crashed, g_drive_wait);
                }
            }
        }
        if (kind != SKEL_KIND_RIDER_HRC &&
            !(kind == SKEL_KIND_RIDER_EDF && /* c_rider.edf n'est pas mis à jour : modèle de collision */
              (state != DRIVE_REQUESTED || g_drive_wait > 0.12f)) &&
            !(kind != SKEL_KIND_OTHER && kind != SKEL_KIND_RIDER_MESH && kind != SKEL_KIND_RIDER_CEDF && state != DRIVE_REQUESTED))
            return;
    }

    if (state == DRIVE_REQUESTED) {
        float m[3], dx, dz;
        /* notre pilote : le rider.hrc qui est à côté de la moto */
        if (!skelscan_model_pos(skeleton, m))
            return;
        dx = m[0] - g_drive_bike[0];
        dz = m[2] - g_drive_bike[2];
        if (dx * dx + dz * dz > 3.0f * 3.0f)
            return;
        equip_set_rider_model(skelscan_name(idx));
        g_drive_is_mod = skelscan_kind(idx) == SKEL_KIND_RIDER_EDF;
        drive_set_model(g_drive_is_mod, equip_rider_up());
        if (drive_spawn(skeleton, &g_rd, g_drive_vel, g_drive_ground)) {
            g_rd.ground_fn = ragdoll_ground; /* chaque point touche le vrai relief sous lui */
            g_rd.obstacle_fn = ragdoll_obstacle; /* v0.41.27 : murs et boîtes de la piste */
            walk_calibrate(&g_walker, &g_rd);
            g_terrain_calib_logged = g_terrain_ok && !g_terrain_from_cache; /* une fois recalé, on garde le recalage */
            g_walker.state = WALK_OFF;
            g_walk_logged = 0;
            log_line("marche : repere %s", g_walker.mirror ? "miroir" : "direct");
            {
                static char last_model[64] = "";
                const char *nm = skelscan_name(idx);
                if (strncmp(last_model, nm, sizeof(last_model) - 1)) {
                    snprintf(last_model, sizeof(last_model), "%s", nm);
                    log_line("pilote : modele %s", nm);
                }
            }
            g_drive_last = now_seconds();
            g_drive_state = DRIVE_ACTIVE;
            {
                /* v1.8.27 : vue changée moins de 2,5 s avant la chute : c'est le jeu qui passe à sa
                   caméra de chute, pas le joueur ; on garde la vue d'avant */
                int gv = g_game_fp;
                if (g_game_fp_prev >= 0 && now_seconds() - g_game_fp_t < 2.5 && !g_game_fp_player) {
                    gv = g_game_fp_prev;
                    log_line("camera : vue du jeu changee %.1f s avant la chute : vue d'avant gardee", now_seconds() - g_game_fp_t);
                }
                if (gv >= 0)
                    g_game_fp = gv;
            }
            if (g_game_fp >= 0 && g_first_person != g_game_fp) {
                /* v1.8.6 : à pied, même vue qu'en roulant (la touche de vue change ensuite comme avant) */
                g_first_person = g_game_fp;
                log_line("camera : a pied en %s personne, comme en roulant", g_first_person ? "1re" : "3e");
            }
            if (g_visual_allowed && g_cfg.replay_record) {
                float mw[12];
                double rcl = -1.0;
                if (!rclock_live_time(&rcl))
                    rcl = -1.0;
                if (skelscan_model_world(skeleton, mw))
                    replay_rec_begin(0, mw, rcl); /* pour la rediffusion */
            }
            if (g_dismount_pending) {
                /* descente volontaire : debout tout de suite (l'animation part de la pose assise) */
                g_dismount_pending = 0;
                g_walker.ankle_height = g_cfg.foot_height;
                walk_begin(&g_walker, &g_rd);
                /* v0.40.45 : premier sol = celui de la moto (ponton), pas le sable dessous */
                walk_set_start_ref(g_bike_obj_ref);
                {
                    float th = 0.0f, oh = 0.0f;
                    int tok = ground_fn(g_rd.pos[RD_PELVIS][0], g_rd.pos[RD_PELVIS][2], &th);
                    int ook = objects_height_fn(g_rd.pos[RD_PELVIS][0], g_rd.pos[RD_PELVIS][2], g_bike_obj_ref, &oh);
                    log_line("descente : bassin y %.2f, moto y %.2f, sol %s %.2f, objet %s %.2f", g_rd.pos[RD_PELVIS][1],
                             g_bike_obj_ref - 0.1f, tok ? "" : "(inconnu)", th, ook ? "" : "(aucun)", oh);
                }
                g_walker.state = WALK_ON;
                g_walk_logged = 2;
            }
            g_drive_writes = drive_apply(skeleton, &g_rd);
        } else {
            g_drive_state = DRIVE_IDLE; /* la télémétrie reprend la main */
            g_knocked = 0;
        }
        return;
    }
    if (skeleton != drive_skeleton())
        return;
    if (state == DRIVE_END_REQUESTED) {
        replay_rec_end(0);
        drive_restore();
        rd_stop(&g_rd);
        g_drive_state = DRIVE_IDLE;
        g_knocked = 0;
        return;
    }
    if (state == DRIVE_ACTIVE) {
        double t = now_seconds();
        float dt = (float)(t - g_drive_last);
        if (dt > 0.0005f) {
            WalkInput in;
            int getup = 0;
            if (dt > 0.05f)
                dt = 0.05f;
            (g_read_input ? g_read_input : read_keyboard)(&in, &getup);
            if (g_bike_lock)
                memset(&in, 0, sizeof(in)); /* il relève la moto */
            if (g_walker.state == WALK_OFF) {
                drive_set_feet(NULL, NULL);
                drive_set_walk_frame(NULL, NULL);
                drive_set_grip(NULL);
                drive_set_head(NULL, NULL);
                drive_anchor(skeleton, &g_rd, dt, g_cfg.anchor_strength, g_cfg.body_ground);
                if (g_rd.time > 0.6f)
                    terrain_plane(&g_rd); /* le relief remplace l'estimation par le corps du jeu */
                players_refresh(); /* v1.8.1 */
                rd_step(&g_rd, dt);
                equip_ground_fix(&g_rd, dt); /* v1.8.20, v1.8.23 */
                if (g_rd.resting && g_rd.time > 1.5f)
                    terrain_calibrate(skeleton);
                /* se relever : dès qu'il touche le sol (plus en l'air, plus en train de rouler
                   vite), avec une touche de déplacement ou X */
                /* v0.40.31 : il faut garder la touche (ou le stick) 2 s pour se relever */
                static float up_hold = 0.0f;
                /* (v0.40.32 : seulement la touche « se relever » du plugin, plus les déplacements) */
                int want_up = g_cfg.walking && g_rd.time > g_cfg.getup_delay && on_ground_slow(&g_rd) && getup;
                /* v1.8.114 : « se relever » affiché dès la chute (gris tant que ce n'est pas encore possible) */
                g_prompt_up = !g_cfg.walking ? 0 : (g_rd.time > g_cfg.getup_delay && on_ground_slow(&g_rd)) ? 2 : 1;
                up_hold = want_up ? up_hold + dt : 0.0f;
                if (want_up && up_hold < g_cfg.getup_hold) {
                    hold_progress("Garde appuye pour te relever", up_hold, g_cfg.getup_hold, g_cfg.key_getup, g_cfg.pad_getup, NULL);
                    g_msg_t = now_seconds();
                }
                if (want_up && up_hold >= g_cfg.getup_hold) {
                    up_hold = 0.0f;
                    g_getup_release = 1; /* même touche que relever la moto : à relâcher d'abord */
                    g_walker.ankle_height = g_cfg.foot_height;
                    terrain_calibrate(skeleton);
                    walk_begin(&g_walker, &g_rd);
                    g_walk_logged = 1;
                    {
                        float dn = 0.0f, dh = 0.0f, ds = 0.0f;
                        int k2;
                        for (k2 = 0; k2 < 3; ++k2) {
                            dn += (g_rd.pos[RD_CHEST][k2] - g_rd.pos[RD_NECK][k2]) * (g_rd.pos[RD_CHEST][k2] - g_rd.pos[RD_NECK][k2]);
                            dh += (g_rd.pos[RD_NECK][k2] - g_rd.pos[RD_HEAD][k2]) * (g_rd.pos[RD_NECK][k2] - g_rd.pos[RD_HEAD][k2]);
                            ds += (g_rd.pos[RD_PELVIS][k2] - g_rd.pos[RD_CHEST][k2]) * (g_rd.pos[RD_PELVIS][k2] - g_rd.pos[RD_CHEST][k2]);
                        }
                        log_line("marche : releve, os du corps couche / d'origine : dos %.3f / %.3f, cou %.3f / %.3f, tete %.3f / %.3f",
                                 sqrtf(ds), g_walker.spine, sqrtf(dn), g_walker.neck, sqrtf(dh), g_walker.head);
                    }
                }
            } else {
                /* le relief peut finir d'être trouvé après le relevé : le corps du jeu
                   est toujours posé au point de chute, on recale dessus */
                terrain_calibrate(skeleton);
                if (g_walker.state == WALK_ON)
                    g_knocked = 0; /* v1.8.3 : relevé */
                players_refresh(); /* v1.8.1 : à chaque image (le jeu n'envoie plus la télémétrie à pied) */
                seat_update(dt); /* v1.8.181 : chaise */
                walk_update(&g_walker, &g_rd, &in, dt);
                /* v1.8.1 : un autre pilote à pied (ou une moto arrêtée) nous rentre dedans : on se pousse */
                if (g_walker.state == WALK_ON && !g_bike_lock) {
                    /* v1.8.5 : sorti en entier à chaque image (avant : 1,5 m/s au plus, il
                       s'enfonçait en marchant, puis passait au travers) */
                    float pp[2], feet = walk_support_y(&g_walker);
                    if (feet < -1e8f)
                        feet = walk_ground_height(&g_rd, g_walker.pos[0], g_walker.pos[1]);
                    if (players_push_walker(g_walker.pos[0], g_walker.pos[1], feet, 0.28f, pp)) {
                        float m = sqrtf(pp[0] * pp[0] + pp[1] * pp[1]);
                        if (m > 0.3f) {
                            pp[0] *= 0.3f / m;
                            pp[1] *= 0.3f / m;
                        }
                        g_walker.pos[0] += pp[0];
                        g_walker.pos[1] += pp[1];
                    }
                    /* v1.8.45 : cônes : il les pousse en marchant, les renverse en courant */
                    if (cones_push_walker(g_walker.pos[0], g_walker.pos[1], feet, 0.25f,
                                          g_walker.speed * sinf(g_walker.heading), g_walker.speed * cosf(g_walker.heading),
                                          pp)) {
                        g_walker.pos[0] += pp[0];
                        g_walker.pos[1] += pp[1];
                    }
                    /* v1.8.82 : bottes de foin : il bute dedans (trop lourdes pour les pousser) */
                    if (bales_push_walker(g_walker.pos[0], g_walker.pos[1], feet, 0.25f, pp)) {
                        g_walker.pos[0] += pp[0];
                        g_walker.pos[1] += pp[1];
                    }
                }
                /* v1.8.1 : journal : moto qui passe près du pilote à pied */
                if (g_walker.state == WALK_ON) {
                    static double last_near = 0.0;
                    float nd, ns;
                    int nn;
                    if (now_seconds() - last_near > 1.0 && players_nearest(g_rd.pos[RD_PELVIS], &nd, &ns, &nn) && nd < 1.5f && ns > 1.0f) {
                        last_near = now_seconds();
                        log_line("collision : moto n%d a %.2f m du pilote a pied, %.0f km/h, bassin %+.2f m / moto", nn, nd, ns * 3.6f,
                                 players_near_dy());
                    }
                }
                /* v1.8 : une moto d'un autre joueur qui roule nous percute : on tombe */
                {
                    static double last_hit = 0.0;
                    float bv[3];
                    int bn = 0;
                    double tn = now_seconds();
                    if (g_walker.state == WALK_ON && !g_bike_lock && tn - last_hit > 2.0 &&
                        (players_hit(g_rd.pos[RD_PELVIS], 0.30f, 2.5f, bv, &bn) ||
                         players_hit(g_rd.pos[RD_CHEST], 0.30f, 2.5f, bv, &bn))) {
                        float st = g_rd.cfg.substep > 0.0f ? g_rd.cfg.substep : 1.0f / 240.0f;
                        float hs = sqrtf(bv[0] * bv[0] + bv[2] * bv[2]);
                        int k2;
                        last_hit = tn;
                        g_walker.state = WALK_OFF;
                        g_knocked = 1; /* v1.8.3 : la caméra reste la nôtre */
                        g_walker.grab = 0.0f; /* il lâche le guidon */
                        if (g_gesture.on && !g_gesture.mount)
                            gesture_clear();
                        for (k2 = 0; k2 < RD_NUM_POINTS; ++k2) {
                            /* le haut du corps part plus vite que les pieds : il bascule */
                            float up = g_rd.pos[k2][1] - g_rd.pos[RD_PELVIS][1];
                            float f = 0.65f + 0.25f * (up > 0.0f ? up : 0.0f);
                            g_rd.prev[k2][0] = g_rd.pos[k2][0] - bv[0] * f * st;
                            g_rd.prev[k2][1] = g_rd.pos[k2][1] - (1.5f + 0.1f * hs) * st;
                            g_rd.prev[k2][2] = g_rd.pos[k2][2] - bv[2] * f * st;
                        }
                        g_rd.time = 0.0f;
                        g_rd.still_time = 0.0f;
                        g_rd.resting = 0;
                        log_line("collision : renverse par la moto n%d a %.0f km/h", bn, hs * 3.6f);
                    }
                }
                /* pieds à plat dans la direction de la marche, pointe qui pousse et talon
                   qui se pose ; pendant le relevé, ils suivent la jambe */
                if (g_walker.state == WALK_ON) {
                    float pitch[2], fl[3], fr[3], c, s_, fx = sinf(g_walker.heading), fz = cosf(g_walker.heading);
                    walk_foot_pitch(pitch);
                    c = cosf(pitch[0]);
                    s_ = sinf(pitch[0]);
                    fl[0] = fx * c; fl[1] = -s_; fl[2] = fz * c;
                    c = cosf(pitch[1]);
                    s_ = sinf(pitch[1]);
                    fr[0] = fx * c; fr[1] = -s_; fr[2] = fz * c;
                    drive_set_feet(fl, fr);
                    /* mains et tête : seulement quand il ne tient pas le guidon */
                    if (g_walker.grab < 0.01f) {
                        float rr[3] = { cosf(g_walker.heading), 0.0f, -sinf(g_walker.heading) }, ff[3] = { fx, 0.0f, fz };
                        if (g_walker.mirror) {
                            rr[0] = -rr[0];
                            rr[2] = -rr[2];
                        }
                        drive_set_walk_frame(rr, ff);
                        walk_head(&in, dt);
                    } else {
                        drive_set_walk_frame(NULL, NULL);
                        drive_set_head(NULL, NULL);
                    }
                    if (g_walker.grab > 0.5f) {
                        float bar[3];
                        int k;
                        for (k = 0; k < 3; ++k)
                            bar[k] = g_gesture.grip_ok ? g_gesture.grip[1][k] - g_gesture.grip[0][k]
                                                       : g_walker.grab_hand[1][k] - g_walker.grab_hand[0][k];
                        drive_set_grip(bar);
                    } else {
                        drive_set_grip(NULL);
                    }
                } else {
                    drive_set_feet(NULL, NULL);
                    drive_set_walk_frame(NULL, NULL);
                    drive_set_grip(NULL);
                    drive_set_head(NULL, NULL);
                }
            }
            {
                /* poings fermés quand il marche ou court ; mains ouvertes (pose du jeu) quand il
                   tient le guidon ou qu'il est au sol */
                static float fist = 0.0f;
                float target = g_walker.state == WALK_ON ? 1.0f - g_walker.grab
                             : g_walker.state == WALK_GETTING_UP ? 0.5f : 0.0f;
                float k = 1.0f - expf(-6.0f * dt);
                if (!g_cfg.fists)
                    target = 0.0f;
                fist += (target - fist) * k;
                drive_set_fist(fist);
            }
            g_drive_last = t;
        }
        g_drive_writes = drive_apply(skeleton, &g_rd);
        {
            DriveSnap sn;
            drive_get_snap(&sn);
            float mw[12];
            int have_mw = skelscan_model_world(skeleton, mw);
            replay_rec_frame(0, now_seconds(), &g_rd, &sn, have_mw ? mw : NULL);
            if (g_visual_allowed && net_running() && have_mw)
                net_send_pose(mw, &g_rd, &sn);
        }
    }
}

/* Appelé par l'interception AVANT chaque mise à jour de modèle */
/* Descendu sans chute : le jeu place les mains d'après le modèle c_rider.edf (figé sur les
   poignées). On y recopie la pose des bras de notre pilote avant chaque mise à jour. */
static void crider_arms(void)
{
    static const char *k_bones[] = { "riderRIG_LeftShoulder", "riderRIG_LeftElbow", "riderRIG_LeftWrist",
                                      "riderRIG_RightShoulder", "riderRIG_RightElbow", "riderRIG_RightWrist" };
    static int logged = 0;
    static uint8_t *nodes[6];
    static int tries = 0;
    int i;
    if (!g_vol_phase) {
        if (tries) {
            memset(nodes, 0, sizeof(nodes));
            tries = 0;
        }
        return;
    }
    if (!logged) {
        logged = 1;
        skelscan_log_model_all(g_rd.pos[RD_PELVIS], 4.0f, "c_rider", log_line);
    }
    for (i = 0; i < 6; ++i) {
        float w[12];
        if (!nodes[i] && tries < 6 * 60) {
            ++tries;
            nodes[i] = skelscan_node_addr(g_rd.pos[RD_PELVIS], 4.0f, "c_rider", k_bones[i]);
        }
        if (nodes[i] && drive_expected_world(k_bones[i], w))
            safe_write(nodes[i] + 0x100, w, sizeof(w));
    }
}

/* Rediffusion : rejoue sur le pilote du jeu un passage enregistré quand il repasse au même
   point de départ */
static void *g_live_skel = NULL;
static Ragdoll g_rd_saved;
static DriveSnap g_snap_saved;
static double g_replay_vt = 0.0, g_replay_rc = 0.0; /* (déclaré plus haut aussi) */
static int g_replay_have_rc = 0;
/* pilotes rejoués : un par squelette de la rediffusion (le joueur et ses potes), chacun dans sa
   place de pilotage (en rediffusion, le pilotage du joueur est en attente) */
typedef struct {
    void *skel;
    Ragdoll rd;
} ReplaySlot;
static ReplaySlot g_rps[REPLAY_PLAYERS];

static void replay_release(int p, int restore)
{
    if (!g_rps[p].skel)
        return;
    drive_select(p);
    if (restore)
        drive_restore();
    drive_set_head(NULL, NULL);
    drive_set_feet(NULL, NULL);
    drive_set_walk_frame(NULL, NULL);
    drive_set_grip(NULL);
    drive_select(0);
    g_rps[p].skel = NULL;
}

static void replay_pre(void *skeleton)
{
    int idx = skelscan_record(skeleton), kind = skelscan_kind(idx), starting = 0, p = -1;
    float mw[12], pel[3];
    DriveSnap sn;
    static Ragdoll tmp;
    if (kind != SKEL_KIND_RIDER_HRC && kind != SKEL_KIND_RIDER_EDF)
        return;
    if (!skelscan_model_world(skeleton, mw))
        return;
    {
        /* temps joué : celui de la rediffusion (s'arrête en pause) dès qu'on l'a trouvé dans
           le jeu, sinon le temps réel */
        static double vt = 0.0, last_wall = 0.0, last_clock = -1.0;
        double wall = now_seconds(), rc, d;
        int have = rclock_tick(wall) && rclock_time(&rc);
        g_replay_have_rc = have;
        if (have)
            g_replay_rc = rc;
        if (last_wall == 0.0)
            last_wall = wall;
        if (have) {
            d = last_clock >= 0.0 ? rc - last_clock : 0.0;
            last_clock = rc;
            if (d < 0.0 || d > 2.0)
                d = 0.0; /* saut dans la rediffusion : géré par le temps de la rediffusion */
        } else {
            d = wall - last_wall;
            last_clock = -1.0;
        }
        last_wall = wall;
        vt += d;
        g_replay_vt = vt;
    }
    if (!skelscan_node_pos(skeleton, "riderRIG_Pelvis", pel))
        return;
    {
        int i;
        for (i = 0; i < REPLAY_PLAYERS; ++i)
            if (g_rps[i].skel == skeleton) {
                memcpy(tmp.pos, g_rps[i].rd.pos, sizeof(tmp.pos));
                break;
            }
    }
    if (!replay_play(skeleton, mw, pel, g_replay_vt, g_replay_have_rc, g_replay_rc, &tmp, &sn, &starting, &p)) {
        int i;
        for (i = 0; i < REPLAY_PLAYERS; ++i)
            if (g_rps[i].skel == skeleton)
                replay_release(i, 1);
        return;
    }
    {
        /* place de pilotage : celle de ce squelette, sinon une libre */
        int i, q = -1;
        (void)p;
        for (i = 0; i < REPLAY_PLAYERS; ++i)
            if (g_rps[i].skel == skeleton)
                q = i;
        for (i = 0; i < REPLAY_PLAYERS && q < 0; ++i)
            if (!g_rps[i].skel)
                q = i;
        if (q < 0) {
            replay_stop(skeleton);
            return;
        }
        p = q;
    }
    if (g_rps[p].skel != skeleton) {
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        int i;
        (void)i;
        rd_init(&g_rps[p].rd, &g_cfg.physics);
        drive_select(p);
        drive_set_model(kind == SKEL_KIND_RIDER_EDF, 0.0f);
        if (!drive_spawn(skeleton, &g_rps[p].rd, zero, mw[7] - 1.0f)) {
            drive_select(0);
            replay_stop(skeleton);
            return;
        }
        drive_select(0);
        g_rps[p].skel = skeleton;
        memcpy(g_rps[p].rd.prev, tmp.pos, sizeof(tmp.pos));
    } else {
        memcpy(g_rps[p].rd.prev, g_rps[p].rd.pos, sizeof(tmp.pos));
    }
    memcpy(g_rps[p].rd.pos, tmp.pos, sizeof(tmp.pos));
    g_rps[p].rd.active = 1;
    drive_select(p);
    drive_set_snap(&sn);
    drive_apply(skeleton, &g_rps[p].rd);
    drive_select(0);
}

/* ---------- pilotes des autres joueurs (réseau du plugin) ----------
 * Chaque joueur dont le plugin pilote le pilote envoie sa pose. Chez nous, on retrouve son
 * pilote (le modèle du jeu le plus près de son corps de chute) et on le pilote avec sa pose,
 * dans une place de pilotage à part (drive_select). */
typedef struct {
    void *skel;          /* son squelette chez nous (NULL : pas encore trouvé) */
    uint32_t id;
    double seen;         /* dernière mise à jour du jeu vue pour ce squelette */
    Ragdoll rd;
} RemoteSlot;
static RemoteSlot g_remote[NET_MAX_PEERS];

static void remote_release(int i, int restore)
{
    RemoteSlot *rs = &g_remote[i];
    replay_rec_end(i + 1);
    if (rs->skel) {
        drive_select(i + 1);
        if (restore)
            drive_restore();
        drive_set_head(NULL, NULL);
        drive_set_feet(NULL, NULL);
        drive_set_walk_frame(NULL, NULL);
        drive_set_grip(NULL);
        drive_select(0);
        log_line("reseau : pilote d'un autre joueur rendu au jeu");
    }
    rs->skel = NULL;
}

static void remote_pre(void *skeleton)
{
    static double last_check = 0.0;
    double now = now_seconds();
    int i, kind;
    float mw[12];
    if (!net_running())
        return;
    if (now - last_check > 0.05) {
        /* places dont le joueur ne pilote plus : rendues au jeu */
        last_check = now;
        for (i = 0; i < NET_MAX_PEERS; ++i) {
            uint32_t id;
            char nm[32];
            float m[12], pts[RD_NUM_POINTS][3];
            DriveSnap sn;
            RemoteSlot *rs = &g_remote[i];
            if (!rs->skel)
                continue;
            if (now - rs->seen > 1.0) {
                remote_release(i, 0); /* le jeu ne met plus ce modèle à jour (joueur parti ?) */
                continue;
            }
            if (!net_peer_pose(i, &id, nm, m, pts, &sn) || id != rs->id)
                remote_release(i, 1);
        }
    }
    for (i = 0; i < NET_MAX_PEERS; ++i)
        if (g_remote[i].skel == skeleton)
            break;
    if (i == NET_MAX_PEERS) {
        /* squelette pas encore attribué : est-ce le pilote d'un joueur qui marche ? */
        int idx = skelscan_record(skeleton), j, best = -1;
        float bd = 2.0f * 2.0f, here[3];
        kind = skelscan_kind(idx);
        if (kind != SKEL_KIND_RIDER_HRC && kind != SKEL_KIND_RIDER_EDF)
            return;
        if (skeleton == drive_skeleton())
            return; /* le nôtre */
        if (drive_skeleton())
            g_own_skel = drive_skeleton();
        if (skeleton == g_own_skel)
            return; /* le nôtre (retenu à notre première chute) */
        if (!skelscan_model_world(skeleton, mw))
            return;
        here[0] = mw[3];
        here[1] = mw[7];
        here[2] = mw[11];
        if (!g_own_skel) {
            /* v1.3.4 : seulement tant qu'on ne connaît pas encore notre pilote, et à 1 m (avant 3 m) :
               un ami qui descendait avec F à côté de notre moto n'était jamais repris (« il me voit
               pas faire F ») */
            float dx = here[0] - g_last_bike_pos[0], dy = here[1] - g_last_bike_pos[1], dz = here[2] - g_last_bike_pos[2];
            if (g_drive_state == DRIVE_IDLE && dx * dx + dy * dy + dz * dz < 1.0f * 1.0f)
                return; /* notre pilote sur sa moto */
        }
        for (j = 0; j < NET_MAX_PEERS; ++j) {
            uint32_t id;
            char nm[32];
            float m[12], pts[RD_NUM_POINTS][3];
            DriveSnap sn;
            float dx, dy, dz, d;
            if (g_remote[j].skel || !net_peer_pose(j, &id, nm, m, pts, &sn))
                continue;
            dx = m[3] - here[0];
            dy = m[7] - here[1];
            dz = m[11] - here[2];
            d = dx * dx + dy * dy + dz * dz;
            if (d < bd) {
                bd = d;
                best = j;
            }
        }
        if (best < 0)
            return;
        {
            RemoteSlot *rs = &g_remote[best];
            uint32_t id;
            char nm[32];
            float m[12];
            DriveSnap sn;
            const float zero[3] = { 0.0f, 0.0f, 0.0f };
            if (!net_peer_pose(best, &id, nm, m, rs->rd.pos, &sn))
                return;
            {
                float keep[RD_NUM_POINTS][3];
                memcpy(keep, rs->rd.pos, sizeof(keep));
                rd_init(&rs->rd, &g_cfg.physics);
                drive_select(best + 1);
                drive_set_model(kind == SKEL_KIND_RIDER_EDF, 0.0f);
                if (!drive_spawn(skeleton, &rs->rd, zero, mw[7] - 1.0f)) {
                    drive_select(0);
                    return;
                }
                drive_select(0);
                memcpy(rs->rd.pos, keep, sizeof(keep));
                memcpy(rs->rd.prev, keep, sizeof(keep));
            }
            rs->skel = skeleton;
            rs->id = id;
            rs->seen = now;
            if (g_cfg.replay_record) {
                double rcl = -1.0;
                if (!rclock_live_time(&rcl))
                    rcl = -1.0;
                if (!replay_rec_active(best + 1)) /* (déjà en cours : commencé pendant la rediffusion) */
                    replay_rec_begin(best + 1, mw, rcl); /* pour le revoir dans la rediffusion */
            }
            log_line("reseau : pilote de %s retrouve a %.1f m de son corps de chute, pilote par son plugin", nm,
                     sqrtf(bd));
            i = best;
        }
    }
    {
        RemoteSlot *rs = &g_remote[i];
        uint32_t id;
        char nm[32];
        float m[12];
        DriveSnap sn;
        rs->seen = now;
        if (!net_peer_pose(i, &id, nm, m, rs->rd.pos, &sn) || id != rs->id)
            return;
        if (skelscan_model_world(skeleton, mw)) {
            float dx = mw[3] - m[3], dz = mw[11] - m[11];
            if (dx * dx + dz * dz > 6.0f * 6.0f) {
                remote_release(i, 1); /* ce n'était pas le sien (son corps de chute est ailleurs) */
                return;
            }
        }
        memcpy(rs->rd.prev, rs->rd.pos, sizeof(rs->rd.prev));
        rs->rd.active = 1;
        drive_select(i + 1);
        drive_set_snap(&sn);
        drive_apply(skeleton, &rs->rd);
        drive_select(0);
        replay_rec_frame(i + 1, now, &rs->rd, &sn, mw);
    }
}

/* Pendant la rediffusion : les autres joueurs continuent de marcher en direct ; on continue
   d'enregistrer ce qu'ils envoient (pour le revoir ensuite), sans piloter leur modèle */
static double g_live_rc_open = -1.0, g_live_wall_open = 0.0; /* temps de la session à l'ouverture */
static void remote_record_live(void)
{
    static double last = 0.0;
    double now = now_seconds();
    int i;
    if (!net_running() || !g_cfg.replay_record || now - last < 1.0 / 60.0)
        return;
    last = now;
    for (i = 0; i < NET_MAX_PEERS; ++i) {
        uint32_t id;
        char nm[32];
        float m[12];
        DriveSnap sn;
        static Ragdoll rd;
        if (!net_peer_pose(i, &id, nm, m, rd.pos, &sn)) {
            if (replay_rec_active(i + 1))
                replay_rec_end(i + 1);
            continue;
        }
        if (!replay_rec_active(i + 1)) {
            /* (l'horloge lue en rediffusion est celle de la rediffusion : on compte depuis l'ouverture) */
            double rcl = g_live_rc_open >= 0.0 ? g_live_rc_open + (now - g_live_wall_open) : -1.0;
            replay_rec_begin(i + 1, m, rcl);
            if (rcl >= 0.0)
                replay_rec_share(i + 1, rcl); /* visible tout de suite dans cette rediffusion */
            log_line("reseau : %s enregistre en direct pendant la rediffusion", nm);
        }
        replay_rec_frame(i + 1, now, &rd, &sn, m);
    }
}

static volatile uintptr_t g_calm_chassis = 0; /* moto dont on arrête les roues (0 : aucune) */
static void on_skeleton_pre_impl(void *skeleton);
static void on_skeleton_pre(void *skeleton)
{
    double t0 = fr_now_us();
    on_skeleton_pre_impl(skeleton);
    g_fr_skel += fr_now_us() - t0;
}
static void on_skeleton_pre_impl(void *skeleton)
{
    if (g_calm_chassis && g_running) {
        /* roues de la moto couchée / tenue : arrêtées aussi à chaque image (le moteur en prise
           les relance entre deux pas de la télémétrie et elles creusent la piste) */
        static double last = 0.0;
        double now = now_seconds();
        if (now - last > 0.001) {
            uintptr_t cb = g_calm_chassis;
            last = now;
            if (cb)
                bike_calm_wheels(cb);
        }
    }
    if (g_live_suspended && g_draw_state == 0) {
        /* retour de la rediffusion : on reprend le pilote là où il était */
        if (skeleton != g_live_skel)
            return;
        {
            const float zero[3] = { 0.0f, 0.0f, 0.0f };
            Ragdoll keep = g_rd_saved;
            g_live_suspended = 0;
            drive_set_model(g_drive_is_mod, g_snap_saved.root_up);
            if (drive_spawn(skeleton, &g_rd, zero, g_drive_ground)) {
                g_rd = keep;
                drive_set_snap(&g_snap_saved);
                {
                    float mw[12];
                    double rcl = -1.0;
                    if (!rclock_live_time(&rcl))
                        rcl = -1.0;
                    if (g_visual_allowed && g_cfg.replay_record && skelscan_model_world(skeleton, mw))
                        replay_rec_begin(0, mw, rcl);
                }
                log_line("rediffusion : pilotage repris");
            } else {
                g_drive_state = DRIVE_IDLE;
                log_line("rediffusion : pilote pas repris, le jeu reprend la main");
            }
        }
    }
    if (g_draw_state == 2) {
        if (g_mode3d && g_cfg.replay_play)
            replay_pre(skeleton);
        return;
    }
    if (g_mode3d && g_visual_allowed)
        remote_pre(skeleton);
    int phase = g_write_phase;
    if (g_lift_hold_render && g_running && bike_lift_active()) {
        /* moto tenue debout : on la remet aussi juste avant l'image (pas seulement 100 fois par
           seconde), sinon entre deux corrections la physique la fait trembler à l'écran */
        static double last = 0.0;
        double now = now_seconds();
        if (now - last > 0.003) {
            last = now;
            /* v1.8.23 : le geste avance 100 fois par seconde, l'image 60 fois : la moto avançait
               d'un pas sur une image, de deux sur la suivante (à-coups = tremblement). Avant chaque
               image, on la place là où elle en est à cet instant précis. */
            double since = now - g_lift_wall;
            if (g_lift_wall > 0.0 && since >= 0.0 && since < 0.05 && g_lift_lp > -0.5f && g_lift_lp < 1.0f) {
                float lp = g_lift_lp + (float)since / 1.3f;
                bike_lift_apply(lp < 0.0f ? 0.0f : (lp > 1.0f ? 1.0f : lp));
            } else
                bike_lift_reapply(); /* v1.8.20 : pendant tout le geste, pas seulement une fois debout */
        }
    }
    if (g_running && bike_lift_active()) {
        /* v1.8.24 : diagnostic du tremblement : à-coups (2e différence) d'une image à l'autre, du
           guidon tel qu'il est dessiné et du châssis physique, et du pilote (tête) */
        static double last_img = 0.0, rep = 0.0;
        static float pm[2][3], pb[2][3], ph[2][3];
        static int nimg = 0;
        static float am = 0.0f, ab = 0.0f, ah = 0.0f;
        double now = now_seconds();
        if (now - last_img > 0.006) {
            float Ms[12], cb[3], m[3], hd[3];
            char sp2[300];
            uintptr_t ch = bike_lift_chassis();
            last_img = now;
            if (ridepose_find_steer(g_tele_pos, Ms, sp2, sizeof(sp2)) && ch &&
                safe_read((const void *)(ch + 0xB8), cb, sizeof(cb))) {
                int k2;
                m[0] = Ms[3], m[1] = Ms[7], m[2] = Ms[11];
                for (k2 = 0; k2 < 3; ++k2)
                    hd[k2] = g_rd.pos[RD_HEAD][k2];
                if (nimg >= 2) {
                    float a1 = 0, a2 = 0, a3 = 0;
                    for (k2 = 0; k2 < 3; ++k2) {
                        float d1 = m[k2] - 2 * pm[1][k2] + pm[0][k2], d2 = cb[k2] - 2 * pb[1][k2] + pb[0][k2],
                              d3 = hd[k2] - 2 * ph[1][k2] + ph[0][k2];
                        a1 += d1 * d1, a2 += d2 * d2, a3 += d3 * d3;
                    }
                    am += sqrtf(a1), ab += sqrtf(a2), ah += sqrtf(a3);
                }
                memcpy(pm[0], pm[1], sizeof(pm[0]));
                memcpy(pm[1], m, sizeof(m));
                memcpy(pb[0], pb[1], sizeof(pb[0]));
                memcpy(pb[1], cb, sizeof(cb));
                memcpy(ph[0], ph[1], sizeof(ph[0]));
                memcpy(ph[1], hd, sizeof(hd));
                ++nimg;
            }
            if (now - rep > 1.0 && nimg > 10) {
                log_line("tremblement (moyenne par image, 2e difference) : guidon dessine %.1f mm, chassis physique %.1f mm, "
                         "tete du pilote %.1f mm (geste %.2f s, montee %.2f s)",
                         1000.0f * am / (nimg - 2), 1000.0f * ab / (nimg - 2), 1000.0f * ah / (nimg - 2), g_lift_t, g_mount_t);
                rep = now;
                nimg = 0;
                am = ab = ah = 0.0f;
            }
        }
    }
    if (g_cfg.crider_arms && ((g_dismounted && g_drive_state == DRIVE_ACTIVE && skeleton == drive_skeleton()) || !g_vol_phase))
        crider_arms();
    if (phase)
        write_test_pre(skeleton, phase);
    else if (g_mode3d && g_drive_state != DRIVE_IDLE)
        drive_pre(skeleton);
}

/* v0.41.30 : le projet s'appelle MXRP Ragdoll. Les fichiers et dossiers de l'ancien nom
   (mxb_ragdoll_*) dans Documents\PiBoSo\MX Bikes sont renommés une fois (réglages,
   rediffusions et reliefs gardés), seulement si le nouveau n'existe pas encore. */
static int g_migrated = 0;
static void migrate_old_names(const char *save)
{
#ifdef _WIN32
    static const char *k_names[] = { "_relief", "_replay", "_menu.ini", "_touches.ini", "_equipement.ini" };
    size_t i, l;
    if (!save || !save[0])
        return;
    l = strlen(save);
    for (i = 0; i < sizeof(k_names) / sizeof(k_names[0]); ++i) {
        char from[600], to[600];
        const char *sep = (save[l - 1] != '\\' && save[l - 1] != '/') ? "\\" : "";
        snprintf(from, sizeof(from), "%s%smxb_ragdoll%s", save, sep, k_names[i]);
        snprintf(to, sizeof(to), "%s%sMXRP_Ragdoll%s", save, sep, k_names[i]);
        if (GetFileAttributesA(to) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(from) != INVALID_FILE_ATTRIBUTES &&
            MoveFileA(from, to))
            ++g_migrated;
    }
#else
    (void)save;
#endif
}


/* v1.8.106 : tout ce qui est au plugin va dans Documents\PiBoSo\MX Bikes\MXRP_Ragdoll\ (le joueur
   trouvait trop de fichiers et dossiers MXRP_Ragdoll_* à côté de ceux du jeu). Déplacés une fois
   (déplacement, rien de supprimé) ; un élément déjà présent dans le dossier n'est pas remplacé. */
static int g_moved_in = 0;
static void make_rdir(const char *save)
{
    size_t l;
    if (!save || !save[0])
        return;
    l = strlen(save);
    snprintf(g_rdir, sizeof(g_rdir), "%s%sMXRP_Ragdoll\\", save, (save[l - 1] != '\\' && save[l - 1] != '/') ? "\\" : "");
#ifdef _WIN32
    CreateDirectoryA(g_rdir, NULL);
    {
        static const char *k_from[] = { "MXRP_Ragdoll.log", "MXRP_Ragdoll_1.log", "MXRP_Ragdoll_2.log", "MXRP_Ragdoll_3.log",
                                        "MXRP_Ragdoll_4.log", "MXRP_Ragdoll_5.log", "MXRP_Ragdoll_equipement.ini",
                                        "MXRP_Ragdoll_hauteur_moto.ini", "MXRP_Ragdoll_menu.ini", "MXRP_Ragdoll_menus",
                                        "MXRP_Ragdoll_pose_moto.ini", "MXRP_Ragdoll_relief", "MXRP_Ragdoll_replay",
                                        "MXRP_Ragdoll_serveur.ini", "MXRP_Ragdoll_touches.ini", "MXRP_Ragdoll_ui_amis",
                                        "MXRP_Ragdoll_sauvegarde_profils", "mxb_ragdoll.log" };
        static const char *k_to[] = { "MXRP_Ragdoll.log", "MXRP_Ragdoll_1.log", "MXRP_Ragdoll_2.log", "MXRP_Ragdoll_3.log",
                                      "MXRP_Ragdoll_4.log", "MXRP_Ragdoll_5.log", "MXRP_Ragdoll_equipement.ini",
                                      "MXRP_Ragdoll_hauteur_moto.ini", "MXRP_Ragdoll_menu.ini", "MXRP_Ragdoll_menus",
                                      "MXRP_Ragdoll_pose_moto.ini", "MXRP_Ragdoll_relief", "MXRP_Ragdoll_replay",
                                      "MXRP_Ragdoll_serveur.ini", "MXRP_Ragdoll_touches.ini", "MXRP_Ragdoll_ui_amis",
                                      "sauvegarde_profils", "mxb_ragdoll.log" };
        size_t i;
        for (i = 0; i < sizeof(k_from) / sizeof(k_from[0]); ++i) {
            char from[700], to[700];
            snprintf(from, sizeof(from), "%s%s%s", save, (save[l - 1] != '\\' && save[l - 1] != '/') ? "\\" : "", k_from[i]);
            snprintf(to, sizeof(to), "%s%s", g_rdir, k_to[i]);
            if (GetFileAttributesA(from) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA(to) == INVALID_FILE_ATTRIBUTES &&
                MoveFileA(from, to))
                ++g_moved_in;
        }
    }
#endif
}

/* v1.2 (préparation de l'expulsion automatique) : où le code du jeu utilise les noms des boutons
   « Kick » / « Eject » de la liste des joueurs. Lecture seule, une fois par lancement, journal. */
static DWORD WINAPI kick_probe_thread(LPVOID unused)
{
    static const char *k_names[] = { "ID_KICK", "ID_EJECT", "idd_clients", "ID_LIST" };
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    IMAGE_SECTION_HEADER sec[16];
    uintptr_t text = 0, rdata = 0;
    size_t text_n = 0, rdata_n = 0;
    uintptr_t str[4] = { 0, 0, 0, 0 };
    int i, j, nsec;
    unsigned char *buf = NULL;
    (void)unused;
    Sleep(8000); /* le jeu est déchiffré et chargé */
    if (!safe_read((const void *)base, &dos, sizeof(dos)) ||
        !safe_read((const void *)(base + (uintptr_t)dos.e_lfanew), &nt, sizeof(nt)))
        return 0;
    nsec = nt.FileHeader.NumberOfSections < 16 ? nt.FileHeader.NumberOfSections : 16;
    if (!safe_read((const void *)(base + (uintptr_t)dos.e_lfanew + sizeof(nt)), sec, sizeof(sec[0]) * (size_t)nsec))
        return 0;
    for (i = 0; i < nsec; ++i) {
        if (!memcmp(sec[i].Name, ".text", 5)) {
            text = base + sec[i].VirtualAddress;
            text_n = sec[i].Misc.VirtualSize;
        } else if (!memcmp(sec[i].Name, ".rdata", 6)) {
            rdata = base + sec[i].VirtualAddress;
            rdata_n = sec[i].Misc.VirtualSize;
        }
    }
    if (!text || !rdata || text_n > 64u << 20 || rdata_n > 16u << 20)
        return 0;
    buf = (unsigned char *)malloc(text_n > rdata_n ? text_n : rdata_n);
    if (!buf)
        return 0;
    if (safe_read((const void *)rdata, buf, rdata_n))
        for (j = 0; j < 4; ++j) {
            size_t L = strlen(k_names[j]) + 1, k;
            for (k = 1; k + L <= rdata_n; ++k)
                if (buf[k - 1] == 0 && !memcmp(buf + k, k_names[j], L)) {
                    str[j] = rdata + k;
                    break;
                }
        }
    if (safe_read((const void *)text, buf, text_n)) {
        size_t k;
        int found = 0;
        for (k = 0; k + 7 <= text_n && found < 24; ++k) {
            if ((buf[k] == 0x48 || buf[k] == 0x4C) && buf[k + 1] == 0x8D && (buf[k + 2] & 0xC7) == 0x05) {
                int32_t d;
                uintptr_t tgt;
                memcpy(&d, buf + k + 3, 4);
                tgt = text + k + 7 + (intptr_t)d;
                for (j = 0; j < 4; ++j)
                    if (str[j] && tgt == str[j]) {
                        char hex[3 * 96 + 1];
                        size_t a = k >= 48 ? k - 48 : 0, b2;
                        int h = 0;
                        for (b2 = a; b2 < k + 48 && b2 < text_n; ++b2)
                            h += snprintf(hex + h, sizeof(hex) - (size_t)h, "%02x", buf[b2]);
                        log_line("expulsion : %s utilise en jeu+0x%llX : %s", k_names[j],
                                 (unsigned long long)(text + k - base), hex);
                        ++found;
                    }
            }
        }
        log_line("expulsion : %d utilisation(s) des boutons trouvee(s)", found);
    }
    free(buf);
    return 0;
}

/* Appelé au lancement du jeu. Retour : 0 = télémétrie à 100 Hz. */
/* v1.7.23 : deux copies du plugin dans plugins (ex. « MXRP_Ragdoll (1).dlo » après un 2e téléchargement) :
   la première chargée prend les interceptions, l'autre ne peut plus rien faire. On le dit clairement. */
static char g_dup_name[128] = "";
static int dup_check(void)
{
    HMODULE self = NULL;
    HANDLE sn;
    MODULEENTRY32 me;
    int n = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)dup_check, &self);
    sn = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (sn == INVALID_HANDLE_VALUE)
        return 0;
    me.dwSize = sizeof(me);
    if (Module32First(sn, &me)) {
        do {
            const char *m = me.szModule;
            size_t l = strlen(m);
            if (me.hModule != self && _strnicmp(m, "MXRP_Ragdoll", 12) == 0 && l > 4 && _stricmp(m + l - 4, ".dlo") == 0) {
                snprintf(g_dup_name, sizeof(g_dup_name), "%s", m);
                ++n;
            }
        } while (Module32Next(sn, &me));
    }
    CloseHandle(sn);
    if (n)
        log_line("ATTENTION : %d autre(s) copie(s) du plugin chargee(s) : \"%s\" ; supprime-la du dossier plugins et garde seulement MXRP_Ragdoll.dlo (sinon la marche, le ragdoll et le reseau ne marchent pas)",
                 n, g_dup_name);
    return n;
}
MXB_EXPORT int Startup(char *_szSavePath)
{

    char path[512];
    int ini_ok;

    migrate_old_names(_szSavePath);
    make_rdir(_szSavePath); /* v1.8.106 : dossier MXRP_Ragdoll (et déplacement des anciens fichiers) */
    CloseHandle(CreateThread(NULL, 0, kick_probe_thread, NULL, 0, NULL));
    cfg_default(&g_cfg);
    ini_ok = cfg_load(&g_cfg, INI_PATH, CFG_USER);
    if (!ini_ok && cfg_load(&g_cfg, OLD_INI_PATH, CFG_USER))
        ini_ok = 2; /* ancien nom */
    equip_init(g_rdir[0] ? g_rdir : _szSavePath, log_line);
    ridepose_init(g_rdir[0] ? g_rdir : _szSavePath, log_line);
    replay_init(g_rdir[0] ? g_rdir : _szSavePath, log_line);
    rclock_init(log_line);
    if (_szSavePath) {
        char rp[700];
        snprintf(rp, sizeof(rp), "%sMXRP_Ragdoll_replay\\horloge.txt", g_rdir);
        rclock_set_file(rp);
    }
    equip_set_config(&g_cfg, g_rdir[0] ? g_rdir : _szSavePath); /* touches réglées dans le menu P */

    if (g_cfg.log && _szSavePath) {
        snprintf(path, sizeof(path), "%sMXRP_Ragdoll.log", g_rdir);
#ifdef _WIN32
        {
            /* v1.8.29 : on garde les 5 journaux d'avant (MXRP_Ragdoll_1.log = la partie d'avant) :
               relancer le jeu effaçait le journal de l'essai */
            char a[600], b[600];
            int k;
            size_t l = strlen(path) - 4; /* sans « .log » */
            for (k = 4; k >= 1; --k) {
                snprintf(a, sizeof(a), "%.*s_%d.log", (int)l, path, k);
                snprintf(b, sizeof(b), "%.*s_%d.log", (int)l, path, k + 1);
                MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING);
            }
            snprintf(a, sizeof(a), "%.*s_1.log", (int)l, path);
            MoveFileExA(path, a, MOVEFILE_REPLACE_EXISTING);
        }
#endif
        g_log = fopen(path, "w");
    }
    dup_check();
    players_init(log_line);
    update_set_test(g_cfg.update_test);
    update_set_ask(g_cfg.update_ask);
    if (g_cfg.auto_update)
        update_start(PLUGIN_VERSION, log_line); /* v1.8.172 */
    else
        log_line("mise a jour : automatique desactivee (menu P)");
    cones_init(log_line); /* v1.8.45 : cônes */
    net_set_blob_fn(cone_net_blob); /* v1.8.65 : cônes en ligne */
    bales_init(log_line); /* v1.8.82 : bottes de foin */
    ramps_init(log_line); /* v1.8.119 : rampes FMX */
    objects_obstacle_log(log_line); /* v1.8.87 */
    net_game_hooks_install(log_line); /* v1.3 : paquets du plugin par le port du jeu (54210) */
    uipatch_install(g_rdir[0] ? g_rdir : _szSavePath, log_line); /* v1.6 : bouton du plugin dans les menus, dossier ui pas touché */
    if (_szSavePath) {
        size_t l = strlen(_szSavePath);
        snprintf(g_user_dir, sizeof(g_user_dir), "%s", _szSavePath);
        (void)l;
        snprintf(g_save_dir, sizeof(g_save_dir), "%sMXRP_Ragdoll_relief", g_rdir);
#ifdef _WIN32
        CreateDirectoryA(g_save_dir, NULL);
#endif
    }

    log_line("MXRP Ragdoll %s", PLUGIN_VERSION);
    profsave_start(_szSavePath, g_rdir, log_line);
    padstyle_set_mode(g_cfg.pad_style); /* v1.8.107 */
    padstyle_start(log_line);
    if (g_moved_in)
        log_line("dossier : %d fichier(s) / dossier(s) du plugin ranges dans %s", g_moved_in, g_rdir); /* v1.8.105 : copie des profils du jeu (lecture seule cote profil) */
    /* v0.40.87 : case « Plugin Ragdoll » des menus du jeu (sous Crash sur saut) */
    uiswitch_start(log_line, (volatile int *)&g_cfg.enabled, _szSavePath, g_rdir);
    uiswitch_set_online(g_cfg.online_ok); /* v1.8.204 */
    log_line("configuration : %s", ini_ok == 2 ? OLD_INI_PATH " (ancien nom, a renommer en MXRP_Ragdoll.ini)" : ini_ok ? INI_PATH : "valeurs par defaut (ini introuvable)");
    if (g_migrated)
        log_line("nom : %d fichier(s) / dossier(s) mxb_ragdoll_* renommes en MXRP_Ragdoll_*", g_migrated);

    rd_init(&g_rd, &g_cfg.physics);
    skelscan_init(log_line);
    drive_init(log_line);
    walk_set_ground_fn(ground_walk_fn);
    walk_set_object_fn(objects_height_fn);
    walk_set_block_fn(objects_blocked_fn);
    bike_set_ground_fn(ground_now_fn); /* hauteur actuelle du sol (déformation comprise) */

#ifdef _WIN32
    {
        /* v1.8.136 : version du jeu inconnue (mise à jour) : image du jeu en mémoire copiée dans le dossier du plugin
           (une fois par version), pour retrouver les endroits du jeu utilisés par le plugin */
        uintptr_t mb = (uintptr_t)GetModuleHandleA(NULL);
        DWORD pe = 0, st = 0, isz = 0;
        if (safe_read((const void *)(mb + 0x3C), &pe, 4) && pe < 4096 && safe_read((const void *)(mb + pe + 8), &st, 4) &&
            safe_read((const void *)(mb + pe + 24 + 56), &isz, 4) && st != 0x6A21833Du && isz > 0 && isz < 64u * 1024u * 1024u &&
            g_rdir[0]) {
            char dp[700];
            FILE *df;
            snprintf(dp, sizeof(dp), "%s\\mxbikes_image_%08lX.bin", g_rdir, (unsigned long)st);
            if ((df = fopen(dp, "rb")) != NULL)
                fclose(df);
            else if ((df = fopen(dp, "wb")) != NULL) {
                static uint8_t pg[4096];
                DWORD o;
                unsigned bad = 0;
                for (o = 0; o < isz; o += 4096) {
                    if (!safe_read((const void *)(mb + o), pg, 4096)) {
                        memset(pg, 0, 4096);
                        ++bad;
                    }
                    fwrite(pg, 1, 4096, df);
                }
                fclose(df);
                log_line("jeu : version inconnue (date %08lX) : image du jeu copiee (%lu Ko, %u pages illisibles) dans %s",
                         (unsigned long)st, (unsigned long)(isz / 1024), bad, dp);
            }
        }
    }
#endif
    gamever_get(log_line); /* v1.8.137 */
    gamever_ode(log_line); /* v1.8.146 */
    groundhook_install(NULL, log_line); /* v1.8.148 (v1.8.149 : seulement pour les données de la piste) */
    terrain_set_geom_fns(objects_hf_data_off, objects_geom_dirty); /* v1.8.138 */
    /* Phase 2 : vérification (lecture seule), puis interception si demandée */
    {
        char info[200];
        HookResult r = hook_check(info, sizeof(info));
        log_line("squelette : %s", info);
        log_line("squelette : verification = %s", hook_result_text(r));
        if (g_cfg.skeleton_hook) {
            r = hook_install(on_skeleton, info, sizeof(info));
            if (r == HOOK_OK)
                hook_set_pre_callback(on_skeleton_pre);
            log_line("squelette : interception %s (%s)", r == HOOK_OK ? "INSTALLEE" : "ECHEC",
                     hook_result_text(r));
            log_line("test d'ecriture : %s", g_cfg.write_test ? "ACTIF" : "inactif");
            g_mode3d = r == HOOK_OK && g_cfg.ragdoll_3d && !g_cfg.write_test;
            log_line("ragdoll 3D : %s", g_mode3d ? "ACTIF" : "inactif");
            /* chaque partie peut être coupée dans le .ini ; on n'intercepte que ce qui sert */
            if (g_mode3d && g_cfg.walk_camera && glcam_install(log_line) > 0) {
                glcam_set_view_fn(walk_camera_fn);
                if (g_cfg.cones_3d) {
                    /* v1.8.48 : cônes en 3D dans la scène ; v1.8.49 : modèle et texture du joueur */
                    /* v1.8.52 : une texture par variante */
                    static unsigned char *tex[8];
                    int v;
                    for (v = 0; v < cones_variants() && v < 8; ++v) {
                        int zl, tw, th;
                        const unsigned char *z = cones_texture_z(v, &zl, &tw, &th);
                        size_t got = 0;
                        if (!tex[v])
                            tex[v] = (unsigned char *)malloc((size_t)tw * th * 3);
                        if (tex[v] && inflate_raw(tex[v], (size_t)tw * th * 3, z, (size_t)zl, &got) == 0 &&
                            got == (size_t)tw * th * 3)
                            glcam_set_scene_texture(v, tex[v], tw, th);
                        else
                            log_line("cones : texture %d illisible (%u octets)", v + 1, (unsigned)got);
                    }
                    {
                        /* v1.8.82 : textures des bottes de foin, après celles des cônes */
                        static unsigned char *btex[8];
                        int k, base = cones_variants();
                        for (k = 0; k < bales_textures() && k < 8 && base + k < 16; ++k) {
                            int zl, tw, th;
                            const unsigned char *z = bales_texture_z(k, &zl, &tw, &th);
                            size_t got = 0;
                            if (!btex[k])
                                btex[k] = (unsigned char *)malloc((size_t)tw * th * 3);
                            if (btex[k] && inflate_raw(btex[k], (size_t)tw * th * 3, z, (size_t)zl, &got) == 0 &&
                                got == (size_t)tw * th * 3)
                                glcam_set_scene_texture(base + k, btex[k], tw, th);
                            else
                                log_line("bottes : texture %d illisible (%u octets)", k + 1, (unsigned)got);
                        }
                    }
                    {
                        /* v1.8.119 : textures des rampes, après celles des bottes */
                        static unsigned char *rtex[16];
                        int k, base = cones_variants() + bales_textures();
                        for (k = 0; k < ramps_textures() && k < 16 && base + k < 24; ++k) {
                            int zl, tw, th;
                            const unsigned char *z = ramps_texture_z(k, &zl, &tw, &th);
                            size_t got = 0;
                            if (!rtex[k])
                                rtex[k] = (unsigned char *)malloc((size_t)tw * th * 3);
                            if (rtex[k] && inflate_raw(rtex[k], (size_t)tw * th * 3, z, (size_t)zl, &got) == 0 &&
                                got == (size_t)tw * th * 3)
                                glcam_set_scene_texture(base + k, rtex[k], tw, th);
                            else
                                log_line("rampes : texture %d illisible (%u octets)", k + 1, (unsigned)got);
                        }
                    }
                    glcam_set_scene_fn(cone_scene);
                    {
                        /* v1.8.97 : modèles envoyés une fois à la carte graphique */
                        int nv;
                        const float *md = cones_model_data(&nv);
                        glcam_set_model(OBJM_CONE, md, 8, nv);
                        md = bales_model_data(OBJM_BALE_ROUND, &nv);
                        glcam_set_model(OBJM_BALE_ROUND, md, 9, nv);
                        md = bales_model_data(OBJM_BALE_ROUND_LOW, &nv);
                        glcam_set_model(OBJM_BALE_ROUND_LOW, md, 9, nv);
                        md = bales_model_data(OBJM_BALE_SMALL, &nv);
                        glcam_set_model(OBJM_BALE_SMALL, md, 9, nv);
                        {
                            int fm; /* v1.8.178 : mobilier */
                            for (fm = OBJM_FURN0; fm <= OBJM_FURN2; ++fm) {
                                md = bales_model_data(fm, &nv);
                                if (md)
                                    glcam_set_model(fm, md, 9, nv);
                            }
                        }
                        {
                            int s; /* v1.8.119 : rampes */
                            for (s = 0; s < ramps_shapes() && OBJM_RAMP0 + s < OBJM_COUNT; ++s) {
                                md = ramps_model_data(s, &nv);
                                glcam_set_model(OBJM_RAMP0 + s, md, 9, nv);
                            }
                        }
                        glcam_set_inst_fn(obj_scene);
                    }
                }
                camhook_install(cam_matmul_cb, log_line);
            }
            /* la manette : pour marcher avec le stick, et pour le respawn en remontant */
            if (g_mode3d && (g_cfg.walking || (g_cfg.bike_lift && g_cfg.pad_respawn)))
                padinject_install(log_line);
            if (g_mode3d && g_cfg.walking)
                kbhook_install(log_line); /* clavier prioritaire à pied */
            log_line("camera qui suit le pilote : %s, relever la moto : %s, respawn par la manette : %s",
                     g_cfg.walk_camera ? "oui" : "non", g_cfg.bike_lift ? "oui" : "non",
                     g_cfg.pad_respawn ? "oui" : "non");
        } else {
            log_line("squelette : interception desactivee (skeleton_hook = 0)");
        }
    }
    return 0;
}

MXB_EXPORT void Shutdown(void)
{
    ridepose_save();
    equip_reset(); /* enregistre les réglages du casque et des bottes */
#ifdef _WIN32
    cursor_restore();
    cursor_unsubclass(); /* la fenêtre du jeu ne doit plus appeler notre code */
#endif
    if (g_drive_state == DRIVE_ACTIVE || g_drive_state == DRIVE_END_REQUESTED) {
        replay_rec_end(0);
        drive_restore();
    }
    g_drive_state = DRIVE_IDLE;
    replay_rec_end_all();
    net_stop();
    hook_uninstall();
    camhook_uninstall();
    padinject_uninstall();
    kbhook_uninstall();
    cursor_restore();
    glcam_uninstall();
    log_line("arret du jeu");
    update_on_shutdown(); /* v1.8.172 : installateur téléchargé (dossier plugins non modifiable) */
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}

/* ---------- événements ---------- */

static volatile DWORD g_rt_tick = 0; /* v1.8.197 */
static int g_obj_session = 0;
MXB_EXPORT void EventInit(void *_pData, int _iDataSize)
{
    g_obj_session = 1; /* v1.8.197 : objets tenus au stand pendant la session */
    terrain_live_reset(); /* v0.41.15 : grille du jeu recréée à chaque session */
    objects_obstacles_reset(); /* v1.8.95 : bottes de foin : physique recréée */
    g_own_skel = NULL;
    bike_release_all(NULL); /* v0.41.26 : pièces de la session précédente oubliées */
    ++g_event_count;
    if (g_event_count == 1)
        uipatch_report(); /* v1.6 : les menus ont-ils été pris dans les copies ? */
    uiswitch_event(1);
    SPluginsBikeEvent_t ev;
    memset(&ev, 0, sizeof(ev));
    if (_pData && _iDataSize > 0)
        memcpy(&ev, _pData, (size_t)_iDataSize < sizeof(ev) ? (size_t)_iDataSize : sizeof(ev));
    ev.m_szTrackName[sizeof(ev.m_szTrackName) - 1] = '\0';
    ev.m_szBikeName[sizeof(ev.m_szBikeName) - 1] = '\0';
    log_line("evenement : piste \"%s\", moto \"%s\", type %d",
             ev.m_szTrackName, ev.m_szBikeName, ev.m_iType);
    equip_set_track(ev.m_szTrackName); /* v1.1.4 */
    {
        /* v1.8.45 : les cônes restent sur la même piste, retirés sur une autre */
        static char last_track[100] = "";
        if (strcmp(last_track, ev.m_szTrackName) != 0) {
            if (cones_count())
                log_line("cones : autre piste, %d cones retires", cones_count());
            cones_clear();
            if (bales_count())
                log_line("bottes : autre piste, %d bottes retirees", bales_count());
            bales_clear();
            snprintf(last_track, sizeof(last_track), "%s", ev.m_szTrackName);
        }
    }
    ev.m_szBikeID[sizeof(ev.m_szBikeID) - 1] = '\0';
    refh_select(ev.m_szBikeID[0] ? ev.m_szBikeID : ev.m_szBikeName); /* v1.7.4 */
    ridepose_set_bike(ev.m_szBikeID); /* v1.7.32 */
    load_reset_binding(); /* l'utilisateur a pu changer sa touche de respawn */
    /* sécurité : le mode Essai (type 1) est toujours hors ligne. Une course (type 2) peut être
       en ligne : on n'y touche à rien, sauf si testing_only = 0 dans le .ini */
    /* v0.41.06 : un serveur en ligne en essai libre est aussi de type 1 : le nom du serveur
       (vide hors ligne) dit si on est en ligne. En ligne, jamais de modification de la moto. */
    {
        int on_srv = ev.m_szServerName[0] != '\0' || ev.m_iServerType != 0;
        g_event_allowed = !on_srv && (ev.m_iType == 1 || !g_cfg.testing_only);
    }
    /* en ligne (course) : seulement ce qui s'affiche chez soi (ragdoll, marche, caméra, vue 1re
       personne). La moto n'est jamais touchée : remonter = respawn normal du jeu */
    /* v0.41.04 : en ligne, le plugin (ragdoll, marche, caméra) suit le serveur : actif
       seulement si le nom du serveur contient « ragdoll » (le créateur du serveur l'annonce
       ainsi). online_visual : 0 = jamais en ligne, 1 = selon le serveur, 2 = toujours. */
    {
        char sv[sizeof(ev.m_szServerName)];
        int i, online, tagged;
        memcpy(sv, ev.m_szServerName, sizeof(sv));
        sv[sizeof(sv) - 1] = '\0';
        online = sv[0] != '\0' || ev.m_iServerType != 0;
        g_learn_off = online;
        for (i = 0; sv[i]; ++i)
            if (sv[i] >= 'A' && sv[i] <= 'Z')
                sv[i] = (char)(sv[i] - 'A' + 'a');
        tagged = strstr(sv, "ragdoll") != NULL;
        log_line("serveur : \"%s\" (type %d) : %s", ev.m_szServerName, ev.m_iServerType,
                 !online ? "hors ligne" : tagged ? "plugin Ragdoll annonce dans le nom" : "pas de plugin Ragdoll annonce dans le nom");
        /* v0.41.09 : demandé par l'utilisateur : sur un serveur « ragdoll », tout comme en solo
           (descente avec F, moto relevée, remontée). online_full = 0 pour revenir à
           l'affichage seulement. Jamais sur les autres serveurs. */
        g_srv_ragdoll = online && tagged;
        g_srv_tagged = g_srv_ragdoll; /* v1.8.204 : serveur RAGDOLL, même si notre multi est coupé */
        g_session_online = online;
        if (online && !g_cfg.online_ok) {
            /* v1.8.204 : multi coupé (de base) : en ligne, le plugin est en pause pour la session, sur tous les
               serveurs (rien n'est envoyé ni reçu, rien n'est changé dans le jeu) ; il s'active quand le joueur
               active le multi (question au premier lancement, ou menu P) */
            g_srv_ragdoll = 0;
            log_line("en ligne : multi coupe par le joueur (menu P) : plugin en pause pour la session");
            if (g_cfg.online_asked) {
                snprintf(g_msg, sizeof(g_msg), "%s", tr("En ligne : plugin en pause, ton multi est coupe (menu P pour l'activer)."));
                g_msg_t = now_seconds() + 8.0;
            }
        }
        uiswitch_pause(online && !g_cfg.online_ok);
        uiswitch_force(g_srv_ragdoll); /* v1.7.25 */
        players_reset(); /* v1.8 */
        g_gfx_dir[0] = '\0'; /* v1.7.27 : gfx.cfg de la moto relu à chaque session */
        ridepose_reload_gfx();
        if (g_srv_ragdoll && !uiswitch_button_on()) {
            g_cfg.enabled = 1;
            log_line("serveur RAGDOLL : plugin active pour cette session (le bouton reste COUPE pour le solo)");
        }
        net_game_hooks_install(log_line); /* v1.3.1 : réessai (imports du jeu prêts plus tard) */
        if (!net_steam_install(log_line)) /* v1.7.22 */
            log_line("reseau : canal Steam pas trouve");
        net_game_channel(g_srv_ragdoll && g_cfg.net_on);
        log_line("reseau : canal du jeu (port du serveur) %s", g_srv_ragdoll && g_cfg.net_on ? "utilise" : "non utilise");
        g_entries_clear = 1;
        /* v1.8.157 : sur un serveur RAGDOLL, aussi en course (type 2), pas seulement en essai : en course,
           la moto n'était jamais cherchée (« moto : pas trouvee » en boucle, pas de relevage) */
        if (online && tagged && g_cfg.online_visual >= 1 && g_cfg.online_full)
            g_event_allowed = 1;
        log_line("serveur : epreuve type %d (%s), physique du plugin %s", ev.m_iType,
                 ev.m_iType == 1 ? "essai" : ev.m_iType == 2 ? "course" : "autre", g_event_allowed ? "PERMISE" : "pas permise");
        g_visual_allowed = g_event_allowed || g_cfg.online_visual >= 2 ||
                           (g_cfg.online_visual == 1 && (!online || tagged));
        /* v1.8.158 : demandé par le joueur : quand le plugin est activé (bouton ACTIF), tout marche partout :
           hors ligne en course aussi, et sur tous les serveurs (avec ou sans RAGDOLL dans le nom). Bouton
           COUPE : le plugin ne fait rien (RunTelemetry s'arrête tout de suite), même s'il est coupé en route */
        g_event_allowed = 1;
        g_visual_allowed = 1;
        log_line("serveur : plugin %s, physique du plugin %s", g_cfg.enabled ? "ACTIF" : "COUPE",
                 g_event_allowed ? "PERMISE" : "pas permise");
        if (0 && online && g_cfg.online_visual == 1) {
            snprintf(g_msg, sizeof(g_msg), "%s",
                     tr(tagged ? "Serveur avec le plugin Ragdoll : marche activee" : "Serveur sans le plugin Ragdoll : marche desactivee"));
            g_msg_t = now_seconds() + 4.0; /* affiché 8 s */
        }
    }
    if (g_dup_name[0] || dup_check()) {
        snprintf(g_msg, sizeof(g_msg), "2 copies du plugin ! Supprime %.60s", g_dup_name);
        g_msg_t = now_seconds() + 8.0;
    }
    net_set_name(ev.m_szRiderName);
    memcpy(g_my_name, ev.m_szRiderName, sizeof(g_my_name) < sizeof(ev.m_szRiderName) ? sizeof(g_my_name) : sizeof(ev.m_szRiderName));
    g_my_name[sizeof(g_my_name) - 1] = '\0';
    if (g_cfg.net_on && g_cfg.online_ok && g_mode3d && g_visual_allowed && !net_running()) /* v1.8.204 */
        net_start(g_cfg.net_host, g_cfg.net_ip, g_cfg.net_port, log_line);
    log_line("modifications du jeu : %s", g_event_allowed ? "autorisees (comme en solo)"
                                          : g_visual_allowed ? "affichage seulement (pas en mode Essai : moto non touchee, respawn du jeu)"
                                                             : "desactivees (pas en mode Essai)");
    terrain_clear();
    g_terrain_ok = 0;
    g_terrain_c = 0.0f;
    g_terrain_bike_cal = 0;
    g_terrain_from_cache = 0;
    g_terrain_prov = 0;
    g_cache_path[0] = '\0';
    if (g_save_dir[0] && g_mode3d) {
        char id[100];
        const char *src = ev.m_szTrackID[0] ? ev.m_szTrackID : ev.m_szTrackName;
        size_t i;
        float c = 0.0f;
        for (i = 0; src[i] && i + 1 < sizeof(id); ++i) {
            char ch = src[i];
            id[i] = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                     ch == '-' || ch == '_') ? ch : '_';
        }
        id[i] = '\0';
        replay_set_track(id);
        replay_new_session();
        cones_hist_clear(); /* v1.8.52 : nouvelle session : nouvelle rediffusion */
        bales_hist_clear();
        ramps_hist_clear(); /* v1.8.159 */
        if (id[0]) {
            snprintf(g_cache_path, sizeof(g_cache_path), "%s\\%s.relief", g_save_dir, id);
            if (terrain_load(g_cache_path, &c)) {
                g_terrain_c = c;
                g_terrain_ok = 1;
                g_terrain_from_cache = 1;
                log_line("relief : charge depuis le cache (instantane), recalage %+.2f m", c);
            }
        }
    }
}

MXB_EXPORT void EventDeinit(void)
{
    objects_obstacles_reset(); /* v1.8.95 */
    g_calm_chassis = 0; /* v1.7.17 */
    g_lift_hold_render = 0;
    bike_release_all("fin de la session");
    release_inputs(NULL);
    uiswitch_event(0);
    uiswitch_force(0); /* v1.7.25 */
    uiswitch_pause(0); /* v1.8.204 */
    g_session_online = 0;
    net_game_channel(0); /* v1.7.25 : plus rien vers la liste des serveurs */
    net_sticky_clear();  /* v1.8.196 : fin de session : nos objets ne sont plus renvoyés */
    g_obj_session = 0;   /* v1.8.197 */
    rd_stop(&g_rd);
    g_hide_timer = 0.0f;
}

MXB_EXPORT void RunInit(void *_pData, int _iDataSize)
{
    bike_release_all(NULL); /* rien d'une sortie précédente (pièces peut-être libérées) */
    uiswitch_on_track(1); /* v0.40.92 : en piste (EventInit arrive déjà sur l'écran de préparation) */
    (void)_pData;
    (void)_iDataSize;
    g_was_crashed = 0;
    g_has_time = 0;
    g_hide_timer = 0.0f;
    equip_reset();
    g_next_scan = NUM_SCANS;
    g_write_phase = 0;
    g_drive_state = DRIVE_IDLE;
    rd_stop(&g_rd);
    skelscan_reset();
    /* le relief de la piste est cherché en arrière-plan, une fois par piste */
    if (g_mode3d && !terrain_ready() && !terrain_scanning())
        terrain_scan_async(log_line);
    else if (g_mode3d && terrain_ready() && !terrain_live_ready())
        terrain_find_live_async(log_line); /* relief du cache : grille du jeu pour la hauteur actuelle */
}

MXB_EXPORT void RunDeinit(void)
{
    if (g_ride_n[0])
        log_line("pose sur la moto : %u mesures, %u sur la moto, %u a plus de 15 km/h, %u en ligne droite", g_ride_n[0],
                 g_ride_n[1], g_ride_n[2], g_ride_n[3]);
    memset(g_ride_n, 0, sizeof(g_ride_n));
    ridepose_report();
    ridepose_save();
    g_calm_chassis = 0; /* v1.7.17 */
    g_lift_hold_render = 0;
    bike_release_all("sortie de piste");
    release_inputs(NULL);
    uiswitch_on_track(0);
    equip_reset();
    dismount_reset("sortie de piste");
    if (g_drive_state == DRIVE_ACTIVE || g_drive_state == DRIVE_END_REQUESTED) {
        replay_rec_end(0);
        drive_restore();
    }
    g_drive_state = DRIVE_IDLE;
    g_write_phase = 0;
    rd_stop(&g_rd);
    g_hide_timer = 0.0f;
    g_next_scan = NUM_SCANS;
    if (hook_installed())
        skelscan_log_table();
}

/* ---------- v1.2 : joueurs du serveur sans le plugin (serveur RAGDOLL) ---------- */
typedef struct {
    int m_iRaceNum;
    char m_szName[100];
} RaceEntryHead; /* début de SPluginsRaceAddEntry_t (le reste ne sert pas) */

#define MAX_ENTRIES 64
static struct { int num; char name[100]; double t; } g_entries[MAX_ENTRIES];
static int g_nentries = 0;
static char g_noplug[96] = "";    /* « Sans plugin : … » affiché */
#define NOPLUG_GRACE 30.0         /* temps laissé au plugin d'un joueur qui arrive pour se signaler */

MXB_EXPORT void RaceAddEntry(void *_pData, int _iDataSize)
{
    RaceEntryHead e;
    int i;
    if (!_pData || _iDataSize < (int)sizeof(e))
        return;
    memcpy(&e, _pData, sizeof(e));
    e.m_szName[sizeof(e.m_szName) - 1] = '\0';
    if (g_entries_clear) {
        g_entries_clear = 0;
        g_nentries = 0;
    }
    for (i = 0; i < g_nentries; ++i)
        if (g_entries[i].num == e.m_iRaceNum)
            break;
    if (i == g_nentries) {
        if (g_nentries >= MAX_ENTRIES)
            return;
        ++g_nentries;
        g_entries[i].t = now_seconds();
    }
    g_entries[i].num = e.m_iRaceNum;
    memcpy(g_entries[i].name, e.m_szName, sizeof(g_entries[i].name));
}

/* v1.8 : positions de toutes les motos (le jeu l'appelle souvent pendant la session) */
static void objects_idle_tick(void); /* v1.8.197 */
MXB_EXPORT void RaceTrackPosition(int _iNumVehicles, void *_pArray, int _iElemSize)
{
    static int logged_n = -1;
    int i;
    if (_iNumVehicles != logged_n) {
        logged_n = _iNumVehicles;
        log_line("collisions : positions des motos recues du jeu (%d moto(s), %d octets chacune)", _iNumVehicles, _iElemSize);
    }
    for (i = 0; i < g_nentries; ++i)
        if (g_my_name[0] && !strcmp(g_entries[i].name, g_my_name)) {
            players_set_self(g_entries[i].num);
            break;
        }
    players_track(_iNumVehicles, _pArray, _iElemSize, now_seconds());
    objects_idle_tick(); /* v1.8.197 : au stand, nos objets vivent toujours */
}

MXB_EXPORT void RaceRemoveEntry(void *_pData, int _iDataSize)
{
    int num, i;
    if (!_pData || _iDataSize < (int)sizeof(int))
        return;
    memcpy(&num, _pData, sizeof(int));
    for (i = 0; i < g_nentries; ++i)
        if (g_entries[i].num == num) {
            g_entries[i] = g_entries[--g_nentries];
            break;
        }
}

/* une fois par seconde : liste des joueurs dont le plugin ne s'est pas signalé */
static char g_admin_said[512] = ""; /* v1.8.204 : joueurs déjà signalés à l'hôte (« |nom| ») */
static int admin_said(const char *nm)
{
    char k[40];
    snprintf(k, sizeof(k), "|%.32s|", nm);
    if (strstr(g_admin_said, k))
        return 1;
    if (strlen(g_admin_said) + strlen(k) < sizeof(g_admin_said))
        strcat(g_admin_said, k);
    return 0;
}

static void noplug_update(void)
{
    static double last = 0.0;
    static char logged[96] = "";
    double now = now_seconds();
    char out[96] = "";
    int i, n = 0, extra = 0;
    if (now - last < 1.0)
        return;
    last = now;
    if (!g_srv_ragdoll || !net_running()) {
        g_noplug[0] = '\0';
        g_admin_said[0] = '\0';
        return;
    }
    for (i = 0; i < g_nentries; ++i) {
        const char *nm = g_entries[i].name;
        if (!nm[0] || !strcmp(nm, g_my_name) || now - g_entries[i].t < NOPLUG_GRACE || net_name_seen(nm, 30.0)) /* v1.7.20 : 30 s (signaux perdus) */
            continue;
        if (g_is_host && !admin_said(nm)) {
            /* v1.8.204 : l'hôte est prévenu (une fois par joueur et par session) : plugin pas installé ou
               multi coupé ; à lui de voir (l'expulsion automatique reste coupée de base) */
            snprintf(g_msg, sizeof(g_msg), "%s %.32s %s", tr("Admin : plugin pas detecte chez"), nm,
                     tr("(pas installe ou multi coupe)"));
            g_msg_t = now + 8.0;
            log_line("admin : plugin pas detecte chez %s", nm);
        }
        if (strlen(out) + strlen(nm) + 4 < 70) {
            if (n)
                strcat(out, ", ");
            strcat(out, nm);
            ++n;
        } else {
            ++extra;
        }
    }
    if (!n) {
        g_noplug[0] = '\0';
    } else if (extra) {
        snprintf(g_noplug, sizeof(g_noplug), "%s : %s +%d", tr("Sans plugin"), out, extra);
    } else {
        snprintf(g_noplug, sizeof(g_noplug), "%s : %s", tr("Sans plugin"), out);
    }
    if (strcmp(logged, g_noplug)) {
        memcpy(logged, g_noplug, sizeof(logged));
        log_line("serveur : %s", g_noplug[0] ? g_noplug : "tous les joueurs ont le plugin");
    }
}

/* v1.7.36 : expulsion automatique des joueurs sans le plugin, sur SON serveur RAGDOLL.
   Trouvé dans le jeu (beta21e, menu idd_clients, bouton ID_KICK) : la liste des joueurs connectés
   est un tableau de 100 cases de 0x40 octets (occupée, n° du joueur, nom…) et le bouton « Kick »
   appelle la fonction du jeu (pointeur jeu+0x566C48) avec 0x350 puis 0x370 / 0x357. On fait pareil.
   Sécurités : seulement si on héberge (la liste n'est remplie que chez l'hôte), serveur « RAGDOLL »,
   canal du jeu en marche, jamais soi-même ; 60 s pour que son plugin se signale, puis 30 s
   d'avertissement à l'écran, puis Kick (il peut revenir avec le plugin). */
#define AK_RVA_FN (gamever_get(NULL)->ak_fn)
#define AK_RVA_TAB (gamever_get(NULL)->ak_tab)
#define AK_N 100
#define AK_GRACE 60.0
#define AK_WARN 30.0
typedef long long(__cdecl *AkFn)(int, long long, long long, long long, long long, long long);
static int g_ak_state = -1; /* -1 pas vérifié, 0 jeu différent, 1 prêt */
static struct { int id; double first; int warned; double kicked; } g_ak[AK_N];
static int ak_check(void)
{
    /* v1.8.137 : endroits et codes selon la version du jeu (gamever.c) */
    const GameAddrs *ga = gamever_get(NULL);
    unsigned char kick_call[] = { 0xb9, 0x70, 0x03, 0x00, 0x00, 0x8d, 0x57, 0x02, 0x44, 0x8d, 0x4f, 0x3c };
    unsigned char b[16];
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (g_ak_state >= 0)
        return g_ak_state;
    g_ak_state = 0;
    kick_call[1] = (unsigned char)(ga->ak_c370 & 0xFF);
    kick_call[2] = (unsigned char)((ga->ak_c370 >> 8) & 0xFF);
    if (ga->ak_call && safe_read((const void *)(base + ga->ak_call), b, sizeof(kick_call)) && !memcmp(b, kick_call, sizeof(kick_call)) &&
        safe_read((const void *)(base + ga->ak_lea), b, 7) && b[0] == 0x48 && b[1] == 0x8d && b[2] == 0x1d) {
        int32_t rel;
        memcpy(&rel, b + 3, 4);
        if ((uint32_t)(ga->ak_lea + 7 + rel) == ga->ak_tab + 4u)
            g_ak_state = 1;
    }
    log_line("expulsion automatique : %s", g_ak_state ? "prete (bouton Kick du jeu trouve)" : "coupee (autre version du jeu)");
    return g_ak_state;
}
static void ak_kick(int id, const char *name)
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    AkFn fn = NULL;
    int ded = 0;
    if (!safe_read((const void *)(base + AK_RVA_FN), &fn, sizeof(fn)) || !fn) {
        log_line("expulsion : fonction du jeu introuvable, %s pas expulse", name);
        return;
    }
    fn(gamever_get(NULL)->ak_c350, (long long)(intptr_t)&ded, 0, 0, 0, 0);
    if (ded < 1)
        fn(gamever_get(NULL)->ak_c370, 2, id, 60, 0, 0);
    else
        fn(gamever_get(NULL)->ak_c357, id, 0, 0, 0, 0);
    log_line("expulsion : %s (joueur %d) expulse : pas de plugin Ragdoll (mode %d)", name, id, ded);
}
static void autokick_update(void)
{
    static double last = 0.0;
    unsigned char tab[AK_N * 0x40];
    uintptr_t base;
    double now = now_seconds();
    int i, k;
    if (now - last < 1.0)
        return;
    last = now;
    if (!(g_cfg.auto_kick_info & 1) && g_font_loaded) {
        /* v1.8.200 : premier lancement : l'expulsion est coupée de base, on le dit une fois */
        g_cfg.auto_kick_info |= 1;
        equip_save_options();
        snprintf(g_msg, sizeof(g_msg), "%s", tr("Expulsion auto des joueurs sans plugin : COUPEE. A activer dans le menu P si tu heberges."));
        g_msg_t = now + 10.0;
        log_line("expulsion : message du premier lancement montre (expulsion %s)", g_cfg.auto_kick ? "active" : "coupee");
    }
    if (!g_srv_ragdoll || !net_running() || !net_game_ok() || ak_check() != 1)
        return;
    base = (uintptr_t)GetModuleHandleA(NULL);
    if (!safe_read((const void *)(base + AK_RVA_TAB), tab, sizeof(tab)))
        return;
    for (i = 0; i < AK_N; ++i) {
        int used;
        memcpy(&used, tab + i * 0x40, 4);
        if (used && tab[i * 0x40 + 8])
            break;
    }
    g_is_host = i < AK_N; /* v1.8.204 : la liste des joueurs n'est remplie que chez l'hôte */
    if (!(g_cfg.auto_kick_info & 2) && g_font_loaded) {
        /* v1.8.200 : premier serveur hébergé */
        if (g_is_host) {
            g_cfg.auto_kick_info |= 3;
            equip_save_options();
            snprintf(g_msg, sizeof(g_msg), "%s",
                     tr(g_cfg.auto_kick ? "Tu heberges : les joueurs sans plugin seront expulses apres 90 s (menu P pour couper)."
                                        : "Tu heberges : expulsion auto des joueurs sans plugin COUPEE (menu P pour l'activer)."));
            g_msg_t = now + 10.0;
            log_line("expulsion : premier serveur heberge, message montre (expulsion %s)", g_cfg.auto_kick ? "active" : "coupee");
        }
    }
    if (!g_cfg.auto_kick)
        return; /* v1.8.200 : coupée de base (menu P) */
    for (i = 0; i < AK_N; ++i) {
        const unsigned char *e = tab + i * 0x40;
        int used, id, slot = -1, freeslot = -1;
        char name[33];
        memcpy(&used, e, 4);
        memcpy(&id, e + 4, 4);
        memcpy(name, e + 8, 32);
        name[32] = '\0';
        if (!used || !name[0] || !strcmp(name, g_my_name))
            continue;
        for (k = 0; k < AK_N; ++k) {
            if (g_ak[k].first > 0.0 && g_ak[k].id == id)
                slot = k;
            else if (g_ak[k].first <= 0.0 && freeslot < 0)
                freeslot = k;
        }
        if (slot < 0) {
            if (freeslot < 0)
                continue;
            slot = freeslot;
            g_ak[slot].id = id;
            g_ak[slot].first = now;
            g_ak[slot].warned = 0;
            g_ak[slot].kicked = 0.0;
            log_line("expulsion : %s (joueur %d) connecte, %.0f s pour que son plugin se signale", name, id, AK_GRACE);
        }
        if (net_name_seen(name, 30.0)) {
            if (g_ak[slot].warned)
                log_line("expulsion : %s a le plugin, pas d'expulsion", name);
            g_ak[slot].first = now; /* compte remis à zéro tant que son plugin parle */
            g_ak[slot].warned = 0;
            continue;
        }
        if (now - g_ak[slot].first >= AK_GRACE && !g_ak[slot].warned) {
            g_ak[slot].warned = 1;
            log_line("expulsion : %s n'a pas le plugin, expulse dans %.0f s", name, AK_WARN);
            snprintf(g_msg, sizeof(g_msg), "%.40s sans plugin : expulse dans %.0f s", name, AK_WARN);
            g_msg_t = now + 4.0;
        }
        if (now - g_ak[slot].first >= AK_GRACE + AK_WARN && now - g_ak[slot].kicked > 20.0) {
            g_ak[slot].kicked = now;
            ak_kick(id, name);
            snprintf(g_msg, sizeof(g_msg), "%.40s expulse (pas de plugin)", name);
            g_msg_t = now + 4.0;
        }
    }
    /* joueurs partis : cases libérées */
    for (k = 0; k < AK_N; ++k) {
        int found = 0;
        if (g_ak[k].first <= 0.0)
            continue;
        for (i = 0; i < AK_N && !found; ++i) {
            int used, id;
            memcpy(&used, tab + i * 0x40, 4);
            memcpy(&id, tab + i * 0x40 + 4, 4);
            found = used && id == g_ak[k].id;
        }
        if (!found)
            g_ak[k].first = 0.0;
    }
}

MXB_EXPORT void RunStart(void) { g_running = 1; }
MXB_EXPORT void RunStop(void)
{
    g_running = 0;
    g_has_time = 0;
    g_write_phase = 0;
    /* v1.7.17 : plus rien n'est écrit dans la moto hors de la piste (crash dans les menus 17 s
       après la sortie : roues de la moto couchée encore « calmées » à chaque image, dans une
       moto déjà détruite) */
    g_calm_chassis = 0;
    g_lift_hold_render = 0;
    cursor_restore();
}
MXB_EXPORT void RunLap(void *_pData, int _iDataSize) { (void)_pData; (void)_iDataSize; }
MXB_EXPORT void RunSplit(void *_pData, int _iDataSize) { (void)_pData; (void)_iDataSize; }

/* ---------- chute ---------- */

static void start_crash(const SPluginsBikeData_t *bike)
{
    const float pos[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ };
    const float vel[3] = { bike->m_fVelocityX, bike->m_fVelocityY, bike->m_fVelocityZ };
    float hspeed = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);

    g_body_searched = 0; /* la moto sera recherchée une fois le pilote debout */
    log_line("moto : rotation au moment de la chute [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f], cap %.1f deg",
             bike->m_aafRot[0][0], bike->m_aafRot[0][1], bike->m_aafRot[0][2], bike->m_aafRot[1][0],
             bike->m_aafRot[1][1], bike->m_aafRot[1][2], bike->m_aafRot[2][0], bike->m_aafRot[2][1],
             bike->m_aafRot[2][2], bike->m_fYaw);

    if (g_mode3d && !g_visual_allowed) {
        /* sécurité : hors du mode Essai (Testing), on ne modifie rien dans le jeu */
        log_line("chute : pilote 3D non pilote (pas en mode Essai, online_visual = 0)");
    }
    if (g_mode3d && g_visual_allowed) {
        /* le rappel de rendu lancera le ragdoll depuis la pose réelle du pilote */
        memcpy(g_drive_vel, vel, sizeof(g_drive_vel));
        /* la moto qui monte une bosse ne doit pas catapulter le pilote */
        if (g_drive_vel[1] > 2.5f)
            g_drive_vel[1] = 2.5f;
        {
            /* vitesse absurde (moto éjectée par la physique) : on limite, le ragdoll reste raisonnable */
            float v = sqrtf(g_drive_vel[0] * g_drive_vel[0] + g_drive_vel[1] * g_drive_vel[1] + g_drive_vel[2] * g_drive_vel[2]);
            if (!isfinite(v)) {
                g_drive_vel[0] = g_drive_vel[1] = g_drive_vel[2] = 0.0f;
            } else if (v > 25.0f) {
                g_drive_vel[0] *= 25.0f / v;
                g_drive_vel[1] *= 25.0f / v;
                g_drive_vel[2] *= 25.0f / v;
                log_line("chute : vitesse %.0f m/s limitee a 25 m/s", v);
            }
        }
        memcpy(g_drive_bike, pos, sizeof(g_drive_bike));
        g_drive_ground = pos[1] + g_cfg.physics.ground_offset;
        g_drive_wait = 0.0f;
        g_drive_log_timer = 0.0f;
        g_drive_state = DRIVE_REQUESTED;
        ++g_drive_req_id;
    } else {
        rd_spawn(&g_rd, pos, bike->m_aafRot, vel);
    }
    g_rest_logged = 0;
    g_hide_timer = 0.0f;

    /* Vue de côté : on regarde perpendiculairement à la trajectoire */
    if (hspeed > 0.5f) {
        g_view_dir[0] = vel[0] / hspeed;
        g_view_dir[1] = vel[2] / hspeed;
    } else {
        /* moto presque à l'arrêt : axe avant du châssis */
        float fx = bike->m_aafRot[0][2], fz = bike->m_aafRot[2][2];
        float n = sqrtf(fx * fx + fz * fz);
        if (n > 1e-3f) {
            g_view_dir[0] = fx / n;
            g_view_dir[1] = fz / n;
        }
    }
    g_view_center = pos[0] * g_view_dir[0] + pos[2] * g_view_dir[1];

    log_line("CHUTE : vitesse %.1f km/h, position (%.1f, %.1f, %.1f)",
             bike->m_fSpeedometer * 3.6f, pos[0], pos[1], pos[2]);

    skelscan_rotate_reset();
    if (hook_installed() && g_cfg.skeleton_scan) {
        skelscan_log_table();
        g_next_scan = 0;
    }
}

/* Choisit la phase du test d'écriture selon le temps écoulé depuis la chute */
static void update_write_test(void)
{
    int phase = 0;
    if (g_cfg.write_test && hook_installed()) {
        if (g_rd.time >= WRITE_PHASE1_START && g_rd.time < WRITE_PHASE2_START)
            phase = 1;
        else if (g_rd.time >= WRITE_PHASE2_START && g_rd.time < WRITE_PHASE2_END)
            phase = 2;
    }
    if (phase != g_write_phase) {
        if (g_write_phase)
            log_line("test d'ecriture : fin de la phase %d (%d os modifies a chaque image, "
                     "matrice non reecrite par le jeu %d fois)",
                     g_write_phase, g_write_counts[g_write_phase], skelscan_rotate_reused());
        if (phase)
            log_line("test d'ecriture : debut de la phase %d (%s) a %.2f s", phase,
                     phase == 1 ? "tete + epaules a 90 degres" : "dos plie a 90 degres",
                     g_rd.time);
        g_write_phase = phase;
    }
    (void)g_write_logged;
}

static void end_crash(void)
{
    log_line("respawn apres %.1f s de chute", g_rd.time);
    g_write_phase = 0;
    /* v0.40.28 : respawn fait par le jeu pendant une descente (touche de respawn) : la descente est
       finie. Sinon elle continuait sur la moto remise en piste (tenue, bascule) : moto éjectée à
       582 km/h. La moto tenue est lâchée, ses roues ne sont plus touchées. */
    if (bike_lift_active())
        bike_lift_end();
    g_calm_chassis = 0;
    g_lift_stand = 0;
    g_vol_tip = 0;
    dismount_reset("respawn");
    if (g_drive_state == DRIVE_ACTIVE) {
        log_line("ragdoll 3D : %d os ecrits a la derniere image", g_drive_writes);
        if (g_walker.state != WALK_OFF)
            log_line("marche : %.1f m du point de chute au respawn",
                     sqrtf((g_walker.pos[0] - g_drive_bike[0]) * (g_walker.pos[0] - g_drive_bike[0]) +
                           (g_walker.pos[1] - g_drive_bike[2]) * (g_walker.pos[1] - g_drive_bike[2])));
        g_drive_state = DRIVE_END_REQUESTED; /* le rappel de rendu remet les os */
        g_hide_timer = g_cfg.hide_delay;
        return;
    }
    g_drive_state = DRIVE_IDLE;
    rd_stop(&g_rd);
    g_hide_timer = g_cfg.hide_delay;
}

/* Descendre de la moto (sans chute) */
/* v0.41.07 : descente « en chute » (la moto basculée une fraction de seconde pour que le jeu
   lâche les mains et la caméra) : seulement quand le plugin peut toucher la moto (Essai hors
   ligne). En ligne : le pilote descend, la moto reste où le jeu la tient. */
static int dismount_crash_on(void) { return g_cfg.dismount_crash && g_event_allowed; }

static void start_dismount(const SPluginsBikeData_t *bike)
{
    const float pos[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ };
    memset(g_drive_vel, 0, sizeof(g_drive_vel));
    memcpy(g_drive_bike, pos, sizeof(g_drive_bike));
    g_drive_ground = pos[1] + g_cfg.physics.ground_offset;
    g_drive_wait = 0.0f;
    g_drive_log_timer = 0.0f;
    gesture_clear();
    g_dismounted = 1;
    g_dismount_pending = 1;
    g_vol_phase = 1;
    g_vol_t = 0.0f;
    g_vol_tip = 0;
    g_fade_want = dismount_crash_on() && g_cfg.dismount_fade;
    padinject_block(1); /* la moto ne doit plus obéir à la manette */
    /* pilote en mod (pas de rider.hrc) : le jeu change de modèle en passant en chute
       (c_rider.edf) ; on bascule donc d'abord, et on prend le squelette après */
    g_vol_early = dismount_crash_on() && g_cfg.bike_lift && !skelscan_kind_near(SKEL_KIND_RIDER_HRC, pos, 3.0f);
    g_vol_wait = 0.0f;
    if (g_vol_early) {
        g_vol_tip = 1;
        log_line("moto : pilote en mod, passage en chute avant la descente");
    } else {
        g_drive_state = DRIVE_REQUESTED;
        ++g_drive_req_id;
    }
    if (dismount_crash_on() && g_cfg.bike_lift && !mem_bike_searching() && !mem_bike_revalidate(pos)) {
        /* ses pièces physiques serviront à la tenir droite pendant la bascule */
        float rot[9], vel[3] = { bike->m_fVelocityX, bike->m_fVelocityY, bike->m_fVelocityZ };
        int a;
        for (a = 0; a < 9; ++a)
            rot[a] = bike->m_aafRot[a / 3][a % 3];
        bike_find(rot, pos, vel);
    }
    log_line("moto : le pilote descend de la moto (%s ou %s)", cfg_key_name(g_cfg.key_dismount[0]),
             cfg_pad_name(g_cfg.pad_dismount));
}

/* Les pièces physiques trouvées sont-elles bien la moto du joueur, là où la télémétrie la
   voit et tournée comme elle ? (v0.32.3 : un châssis périmé, relevé « droit » alors que la moto
   était couchée, a fait exploser la physique et planter le jeu) */
/* Recherche rapide (v0.40.19) : le châssis parmi les pièces du monde ODE déjà trouvé (même
   rotation que la télémétrie, près d'elle ; la plus lourde). Chez certains joueurs, fouiller
   toute la mémoire prend 14 s. */
static int bike_fast_find(const float rot[9], const float pos[3])
{
    uintptr_t b[64];
    int n = mem_world_bodies(b, 64), i, best = -1;
    float best_m = 0.0f, best_dp = 1e9f;
    if (mem_bike_searching())
        return 0;
    for (i = 0; i < n; ++i) {
        float st[16], m = 0.0f, d = 0.0f, dp;
        int j, k;
        if (!b[i] || !safe_read((const void *)(b[i] + 0xB8), st, sizeof(st)))
            continue;
        safe_read((const void *)(b[i] + 0x40), &m, 4);
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                d += fabsf(st[4 + j * 4 + k] - rot[j * 3 + k]);
        dp = (st[0] - pos[0]) * (st[0] - pos[0]) + (st[1] - pos[1]) * (st[1] - pos[1]) + (st[2] - pos[2]) * (st[2] - pos[2]);
        /* v0.41.31 : la plus proche de la télémétrie, plus la plus lourde : le pilote (80 kg),
           tourné comme la moto quand il est assis, passait devant le châssis (~60 kg) */
        if (d < 0.6f && dp < 2.0f * 2.0f && m > 30.0f && m < 200.0f && dp < best_dp) {
            best_dp = dp;
            best_m = m;
            best = i;
        }
    }
    if (best < 0)
        return 0;
    mem_set_bike_body(b[best]);
    log_line("moto : chassis retrouve tout de suite parmi les pieces connues (%.0f kg)", best_m);
    return 1;
}

static void bike_find(const float rot[9], const float pos[3], const float vel[3])
{
    if (!bike_fast_find(rot, pos))
        mem_find_bike_body_async(rot, pos, vel, log_line);
}

static void bike_research(void)
{
    float rot[9], vel[3] = { 0.0f, 0.0f, 0.0f };
    if (mem_bike_searching())
        return;
    memcpy(rot, g_tele_R, sizeof(rot));
    bike_find(rot, g_tele_pos, vel);
}

static int body_matches(uintptr_t body)
{
    float cp[3], R9[9], d = 0.0f, dp;
    int i;
    if (!g_tele_ok || !body || !chassis_frame(body, cp, R9))
        return 0;
    for (i = 0; i < 9; ++i)
        d += fabsf(R9[i] - g_tele_R[i]);
    dp = (cp[0] - g_tele_pos[0]) * (cp[0] - g_tele_pos[0]) + (cp[1] - g_tele_pos[1]) * (cp[1] - g_tele_pos[1]) +
         (cp[2] - g_tele_pos[2]) * (cp[2] - g_tele_pos[2]);
    if (d < 0.6f && dp < 2.0f * 2.0f) /* la télémétrie donne un point ~1 m sous le centre de gravité */
        return 1;
    log_line("moto : pieces trouvees qui ne sont plus la moto (ecart rotation %.2f, position %.2f m) : nouvelle recherche",
             d, sqrtf(dp));
    bike_research();
    return 0;
}

/* Monter sur la moto : touche du clavier = tout de suite ; bouton de la manette = le garder
   appuyé 2 s (v0.40.30 : évite de remonter par erreur). 1 au moment où ça se déclenche.
   slot : un compteur par endroit du code qui demande la montée. */
#define MOUNT_HOLD 0.0f /* v0.40.31 : remis à immédiat (le joueur voulait l'appui long pour se relever) */
static int mount_trigger(int slot, int key, int pad, float dt)
{
    static int kprev[3] = { 1, 1, 1 }, fired[3];
    static float held[3];
    int fire = 0;
    if (slot < 0 || slot > 2)
        return 0;
    if (key && !kprev[slot])
        fire = 1;
    kprev[slot] = key;
    if (pad) {
        held[slot] += dt;
        if (!fired[slot] && held[slot] > 0.15f && held[slot] < MOUNT_HOLD) {
            hold_progress("Garde appuye pour monter sur la moto", held[slot], MOUNT_HOLD, g_cfg.key_bike, g_cfg.pad_bike, NULL);
            g_msg_t = now_seconds();
        }
        if (!fired[slot] && held[slot] >= MOUNT_HOLD) {
            fired[slot] = 1;
            fire = 1;
        }
    } else {
        held[slot] = 0.0f;
        fired[slot] = 0;
    }
    return fire;
}

static void dismount_reset(const char *why)
{
    g_fade_want = 0;
    if (!g_dismounted)
        return;
    g_dismounted = 0;
    g_dismount_pending = 0;
    g_vol_phase = 0;
    padinject_block(0);
    if (g_gesture.on)
        gesture_clear();
    g_bike_lock = 0;
    if (why)
        log_line("moto : fin de la descente (%s)", why);
}

#define VOL_T 1.85f /* durée de la montée (et de la descente, jouée à l'envers), s */

/* À chaque pas de télémétrie : descente, marche à côté de la moto, remontée */
static void dismount_update(const SPluginsBikeData_t *bike, float dt)
{
    static int key_prev = 1, pad_prev = 1;
    PadState ps;
    int pad_ok = padinject_read(&ps);
    int k = input_ok() && bind_key(g_cfg.key_dismount);
    float sp = bike->m_fSpeedometer;
    int i, j;
    {
        /* v1.8.24 : il roule vraiment depuis au moins 1 s (pas tombé, pas arrêté) */
        static float ride_t = 0.0f;
        int r = !bike->m_iCrashed && sp > 2.0f && g_drive_state == DRIVE_IDLE && !g_dismounted && !bike_lift_active();
        {
            static unsigned acc = 0;
            static float acc_t = 0.0f;
            PadState bp;
            if (padinject_read(&bp)) {
                acc |= bp.buttons;
                if (g_view_pad_game && (bp.buttons & g_view_pad_game))
                    g_view_key_t = now_seconds(); /* v1.8.117 */
            }
#ifdef _WIN32
            if (g_view_vk_game > 0 && (GetAsyncKeyState(g_view_vk_game) & 0x8000) && game_focused())
                g_view_key_t = now_seconds(); /* v1.8.117 : touche de vue du jeu au clavier */
#endif
            acc_t += dt;
            if (acc_t > 1.0f) {
                g_btn_recent = acc;
                acc = 0;
                acc_t = 0.0f;
            } else
                g_btn_recent |= acc;
        }
        ride_t = r ? ride_t + dt : 0.0f;
        g_riding_ok = ride_t > 1.0f;
    }
#ifdef _WIN32
    {
        /* v1.8.33 : essais au clavier (pour tester sans manette) : Ctrl+Haut tenu = gaz (gâchette droite),
           Ctrl+Bas = bouton de vue du jeu, Ctrl+Page haut / bas = vitesse + / - */
        static int f11_prev = 0;
        static float f11_t = 0.0f;
        /* v1.8.35 : seulement avec Ctrl (F12 seul = capture d'écran de Steam) */
        int foc = g_cfg.test_keys && game_focused() && (GetAsyncKeyState(VK_CONTROL) & 0x8000); /* v1.8.38 : coupées par défaut */
        int f10 = foc && (GetAsyncKeyState(VK_UP) & 0x8000); /* v1.8.36 : Ctrl+Haut (F10 ouvrait Medal / Discord) */
        int f11 = foc && (GetAsyncKeyState(VK_DOWN) & 0x8000); /* Ctrl+Bas */
        if (!g_essai_on)
            padinject_force_rt(f10 ? 200 : 0);
        if (f11 && !f11_prev && g_view_kind >= 0) {
            padinject_press_extra(g_view_kind, g_view_code, 1);
            f11_t = 0.15f;
            log_line("essai : Ctrl+Bas -> bouton de vue du jeu");
        }
        if (f11_t > 0.0f) {
            f11_t -= dt;
            if (f11_t <= 0.0f)
                padinject_press_extra(g_view_kind, g_view_code, 0);
        }
        f11_prev = f11;
        {
            /* v1.8.34 : F12 = passer la vitesse (bouton du profil), F9 = rétrograder, F8... non : F7 = embrayage */
            static float up_t = 0.0f, dn_t = 0.0f;
            static int f12p = 0, f9p = 0;
            int f12 = foc && (GetAsyncKeyState(VK_PRIOR) & 0x8000), f9 = foc && (GetAsyncKeyState(VK_NEXT) & 0x8000); /* Ctrl+Page haut / bas */
            unsigned short m = 0;
            if (f12 && !f12p)
                up_t = 0.12f;
            if (f9 && !f9p)
                dn_t = 0.12f;
            f12p = f12, f9p = f9;
            if (up_t > 0.0f) {
                up_t -= dt;
                m |= padinject_game_mask(PAD_RESET_BUTTON, 5);
            }
            if (dn_t > 0.0f) {
                dn_t -= dt;
                m |= padinject_game_mask(PAD_RESET_BUTTON, 6);
            }
            if (!g_essai_on)
                padinject_force_buttons(m);
        }
    }
#endif
    {
        /* v1.8.24 : vue choisie à pied gardée en roulant : on appuie sur le bouton de vue du jeu
           jusqu'à ce que sa caméra soit la bonne (1re ou 3e personne) */
        static float t_next = 0.0f, press_t = 0.0f;
        static int presses = 0;
        /* v1.8.26 : coupé : le jeu a plusieurs caméras (guidon, garde-boue, extérieures...) et
           l'appui automatique les faisait défiler toutes seules en roulant (clip du 18/11 18:06) */
        if (0 && g_view_want >= 0 && g_riding_ok && g_game_fp >= 0 && g_view_kind >= 0 && g_mode3d) {
            t_next -= dt;
            if (press_t > 0.0f) {
                press_t -= dt;
                if (press_t <= 0.0f)
                    padinject_press_extra(g_view_kind, g_view_code, 0);
            } else if (g_game_fp == g_view_want) {
                if (presses)
                    log_line("camera : vue du jeu remise en %s personne (%d appui(s))", g_view_want ? "1re" : "3e", presses);
                g_view_want = -1;
                presses = 0;
            } else if (t_next <= 0.0f) {
                if (presses >= 8) {
                    log_line("camera : vue du jeu pas trouvee apres %d appuis, laissee", presses);
                    g_view_want = -1;
                    presses = 0;
                } else {
                    padinject_press_extra(g_view_kind, g_view_code, 1);
                    press_t = 0.12f;
                    t_next = 1.0f; /* la détection de la vue demande 0,5 s */
                    ++presses;
                }
            }
        } else if (press_t > 0.0f) {
            press_t = 0.0f;
            padinject_press_extra(g_view_kind, g_view_code, 0);
        } else
            t_next = 0.3f;
    }
    /* repère de la moto (télémétrie) */
    g_tele_pos[0] = bike->m_fPosX;
    g_tele_pos[1] = bike->m_fPosY;
    g_tele_pos[2] = bike->m_fPosZ;
    players_set_self_pos(g_tele_pos); /* v1.8 */
    g_bike_obj_ref = bike->m_fPosY + 0.1f; /* objets sous la moto (ponton) : sol de la moto */
    terrain_raise_set_bike(bike->m_fPosX, bike->m_fPosZ, 1); /* v1.8.129 */
#ifdef _WIN32
    {
        /* v1.8.132 : essai de rampe demandé par un fichier « essai_rampe.txt » (vitesse en km/h) dans le dossier
           du plugin : gaz et vitesses passés par le plugin, guidon droit (le joueur ne touche à rien), 12 s au plus,
           arrêt à la chute ou dès que le joueur freine */
        static double chk = 0.0, t0 = 0.0, shift_t = 0.0;
        static int on = 0;
        static float target = 60.0f;
        double nw = now_seconds();
        if (MXRP_DIAG && !on && nw - chk > 0.5 && g_rdir[0]) {
            char fp[600], fd[600];
            FILE *ef;
            chk = nw;
            snprintf(fp, sizeof(fp), "%s\\essai_rampe.txt", g_rdir);
            if ((ef = fopen(fp, "r")) != NULL) {
                float v = 0.0f;
                if (fscanf(ef, "%f", &v) == 1 && v >= 10.0f && v <= 120.0f)
                    target = v;
                fclose(ef);
                snprintf(fd, sizeof(fd), "%s\\essai_rampe_fait.txt", g_rdir);
                remove(fd);
                rename(fp, fd);
                if (!bike->m_iCrashed && g_tele_ok) {
                    on = 1;
                    g_essai_on = 1;
                    t0 = nw;
                    shift_t = 0.0;
                    log_line("essai rampe : depart, vitesse voulue %.0f km/h (gaz et vitesses par le plugin)", target);
                } else
                    log_line("essai rampe : demande ignoree (pas sur la moto)");
            }
        }
        if (on) {
            float kmh = bike->m_fSpeedometer * 3.6f;
            int want_gear = 1 + (kmh > 22.0f) + (kmh > 38.0f) + (kmh > 54.0f) + (kmh > 70.0f);
            unsigned short btn = 0;
            PadState ps;
            int braking = padinject_read(&ps) && ps.lt > 0.3f;
            if (bike->m_iCrashed || nw - t0 > 12.0 || braking) {
                on = 0;
                g_essai_on = 0;
                padinject_force_rt(0);
                padinject_force_buttons(0);
                log_line("essai rampe : fin (%s) a %.0f km/h", bike->m_iCrashed ? "chute" : braking ? "frein du joueur" : "12 s", kmh);
            } else {
                padinject_force_rt(kmh < target - 4.0f ? 255 : kmh < target ? 170 : 70);
                if (g_shiftup_mask && bike->m_iGear < want_gear && nw - shift_t > 0.45) {
                    shift_t = nw;
                    log_line("essai rampe : vitesse %d -> %d (%.0f km/h)", bike->m_iGear, bike->m_iGear + 1, kmh);
                }
                if (nw - shift_t < 0.12)
                    btn = g_shiftup_mask;
                padinject_force_buttons(btn);
            }
        }
    }
#endif
    {
        /* v1.8.128 : sonde : la moto sur une rampe (hauteurs vues par le plugin et dans les grilles du jeu) */
        static double pt = 0.0, on_t = -10.0;
        static int nlog = 0;
        float top = 0.0f, lx, ly, W, L, d;
        int on = ramps_height(bike->m_fPosX, bike->m_fPosZ, 1e9f, &top);
        if (on) {
            if (now_seconds() - on_t > 3.0)
                nlog = 0; /* v1.8.133 : nouveau passage : sonde remise à zéro (avant : 600 lignes pour toute la session) */
            on_t = now_seconds();
        }
        /* v1.8.131 : aussi 1,5 s après (sortie : par le bout ou par le côté), et position dans la rampe */
        if (MXRP_DIAG && nlog < 600 && now_seconds() - pt > 0.04 && now_seconds() - on_t < 1.5 &&
            (d = ramps_local(bike->m_fPosX, bike->m_fPosZ, &lx, &ly, &W, &L)) >= 0.0f) {
            char gb[400];
            pt = now_seconds();
            ++nlog;
            terrain_raise_probe(bike->m_fPosX, bike->m_fPosZ, gb, sizeof(gb));
            log_line("rampe sonde : %.0f km/h, moto (%.1f %.2f %.1f), %s, cote %.2f / %.2f m, long %.2f / %.2f m, rampe %.2f, "
                     "roues sol av %d ar %d, susp av %.3f ar %.3f, tangage %.0f roulis %.0f :%s",
                     bike->m_fSpeedometer * 3.6f, bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ, on ? "dessus" : "hors rampe",
                     lx, W, ly, L, top, bike->m_aiWheelMaterial[0], bike->m_aiWheelMaterial[1], bike->m_afSuspLength[0],
                     bike->m_afSuspLength[1], bike->m_fPitch, bike->m_fRoll, gb);
        }
    }
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            g_tele_R[i * 3 + j] = bike->m_aafRot[i][j];
    g_tele_ok = 1;

    if (g_tip_t > 0.0f) {
        /* chute douce : dès que la moto est trouvée, on la fait basculer vers la gauche */
        uintptr_t body = mem_bike_body();
        g_tip_t += dt;
        if (bike->m_iCrashed || g_tip_t > 4.0f) {
            if (!bike->m_iCrashed)
                log_line("moto : la moto ne s'est pas couchee (corps %s)", body ? "trouve" : "introuvable");
            g_tip_t = 0.0f;
        } else if (body) {
            float l = sqrtf(g_tele_R[2] * g_tele_R[2] + g_tele_R[8] * g_tele_R[8]);
            if (l > 0.3f) {
                const float ax[3] = { g_tele_R[2] / l, 0.0f, g_tele_R[8] / l };
                bike_tip(body, ax, -1.6f, 0.55f);
            }
        }
    }
    /* v0.41.17 : relief d'une piste nouvelle actif tout de suite. Dès qu'il est lu : hauteur
       provisoire (le décalage vu sur les autres pistes, ~ -0,18 m), puis mesurée sur la moto qui
       roule doucement, droite (médiane de 25 mesures sur plus d'une seconde). La moto arrêtée
       donne ensuite la valeur exacte, comme avant. */
    if (terrain_ready() && !g_terrain_bike_cal && !g_terrain_from_cache && g_mode3d) {
        static float samp[64], last_t = 0.0f, first_t = 0.0f, clock = 0.0f;
        static int ns = 0, ev_seen = -1;
        float h;
        clock += dt;
        if (ev_seen != g_event_count) {
            ev_seen = g_event_count;
            ns = 0;
        }
        if (!g_terrain_ok && g_terrain_prov == 0) {
            g_terrain_c = -0.18f;
            g_terrain_ok = 1;
            g_terrain_prov = 1;
            log_line("relief : actif tout de suite avec une hauteur provisoire (%+.2f m), mesure en roulant", g_terrain_c);
        }
        if (g_terrain_prov == 1 && !g_dismounted && !bike->m_iCrashed && g_tele_ok && g_tele_R[4] > 0.97f &&
            sp > 0.5f && sp < 6.0f && clock - last_t >= 0.05f &&
            terrain_height(bike->m_fPosX, bike->m_fPosZ, &h, NULL)) {
            float c = (bike->m_fPosY - 0.08f) - h;
            if (fabsf(c) < 1.5f && ns < 64) {
                if (!ns)
                    first_t = clock;
                samp[ns++] = c;
                last_t = clock;
            }
            if (ns >= 25 && clock - first_t >= 1.0f) {
                int i, j;
                for (i = 1; i < ns; ++i) /* tri pour la médiane */
                    for (j = i; j > 0 && samp[j - 1] > samp[j]; --j) {
                        float t = samp[j];
                        samp[j] = samp[j - 1];
                        samp[j - 1] = t;
                    }
                g_terrain_c = samp[ns / 2];
                g_terrain_prov = 2;
                log_line("relief : hauteur mesuree sur la moto en roulant : %+.2f m (de %+.2f a %+.2f), arret pour la valeur exacte",
                         g_terrain_c, samp[ns / 4], samp[3 * ns / 4]);
                ns = 0;
            }
        }
    }
    if (!g_dismounted) {
        static float hold = 0.0f, still = 0.0f;
        /* moto arrêtée : on retrouve ses pièces physiques à l'avance, pour que la descente
           (et le passage en chute qui libère la caméra) commence dès l'appui */
        still = (sp < 0.5f && !bike->m_iCrashed) ? still + dt : 0.0f;
        if (still >= 1.0f && g_event_allowed && g_tele_ok && g_tele_R[4] > 0.97f) {
            const float bp[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ };
            terrain_calibrate_stopped(bp);
            {
                /* hauteur des roues posées : sert à voir si elles touchent quand on tient la moto */
                static double last = 0.0;
                uintptr_t cb = mem_bike_body();
                if (cb && now_seconds() - last > 5.0 && body_matches(cb)) {
                    float h = bike_measure_wheels(cb);
                    if (last == 0.0 && h > 0.0f)
                        log_line("moto : roues posees, centre a %.2f m au-dessus du sol", h);
                    /* v1.7.1 : le centre d'une roue posée est à son rayon (0,345 m) du vrai sol :
                       on recale le relief dessus. Avant, recalé sur un point de la moto supposé à
                       8 cm du sol : 15 cm trop bas sur la KTM (pieds dans le sol, moto tenue qui
                       flottait, pieds trop bas sur les repose-pieds) */
                    if (h > 0.25f && h < 0.75f && g_terrain_ok && fabsf(h - 0.345f) > 0.03f) {
                        float c2 = g_terrain_c + (h - 0.345f);
                        log_line("relief : recale sur les roues de la moto arretee : %+.2f m (avant %+.2f m)", c2, g_terrain_c);
                        g_terrain_c = c2;
                        cache_save_async(c2);
                        bike_measure_wheels(cb); /* hauteur des roues avec le nouveau relief */
                    }
                    if (h > 0.25f && h < 0.75f && g_terrain_ok) {
                        float th;
                        if (terrain_height(bp[0], bp[2], &th, NULL))
                            refh_store(bp[1] - (th + g_terrain_c));
                        else
                            log_line("moto : hauteur au-dessus du sol pas mesuree (relief inconnu sous la moto)");
                    }
                    last = now_seconds();
                }
            }
        }
        if (dismount_crash_on() && g_cfg.bike_lift && g_mode3d && g_event_allowed && still >= 0.6f &&
            still - dt < 0.6f && !mem_bike_searching()) {
            const float bp[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ };
            if (!mem_bike_revalidate(bp)) {
                float rot[9], vel[3] = { bike->m_fVelocityX, bike->m_fVelocityY, bike->m_fVelocityZ };
                int a;
                for (a = 0; a < 9; ++a)
                    rot[a] = bike->m_aafRot[a / 3][a % 3];
                bike_find(rot, bp, vel);
            }
        }
        int p = pad_ok && input_ok() && bind_pad(g_cfg.pad_dismount, &ps);
        hold = p ? hold + dt : 0.0f;
        if (!bike->m_iCrashed && g_drive_state == DRIVE_IDLE && g_mode3d && g_visual_allowed && !g_event_allowed &&
            g_cfg.walking && ((k && !key_prev) || (p && hold >= g_cfg.dismount_hold && hold - dt < g_cfg.dismount_hold))) {
            /* v0.41.08 : en ligne, descendre avec F ne marche pas : sans passer en chute (il
               faudrait basculer la moto), le jeu garde sa caméra et colle les mains au guidon */
            snprintf(g_msg, sizeof(g_msg), "%s", tr("En ligne : marche seulement apres une chute (F seulement hors ligne)"));
            g_msg_t = now_seconds();
            log_line("moto : descente avec F refusee en ligne (le jeu garderait sa camera et le pilote sur la moto)");
        }
        if (!bike->m_iCrashed && g_drive_state == DRIVE_IDLE && g_mode3d && g_event_allowed && g_cfg.walking &&
            ((k && !key_prev) || (p && hold >= g_cfg.dismount_hold && hold - dt < g_cfg.dismount_hold))) {
            if (sp < 1.0f && g_cfg.dismount_fall) {
                /* descente par une chute douce : on couche la moto, le jeu passe en chute */
                const float bp[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ };
                g_tip_t = 0.001f;
                if (!mem_bike_revalidate(bp)) {
                    float rot[9], vel[3] = { bike->m_fVelocityX, bike->m_fVelocityY, bike->m_fVelocityZ };
                    int a, b2;
                    for (a = 0; a < 3; ++a)
                        for (b2 = 0; b2 < 3; ++b2)
                            rot[a * 3 + b2] = bike->m_aafRot[a][b2];
                    bike_find(rot, bp, vel);
                }
                log_line("moto : descente par une chute douce (la moto se couche)");
            } else if (sp < 1.0f)
                start_dismount(bike);
            else
                log_line("moto : arrete-toi pour descendre (%.0f km/h)", sp * 3.6f);
        }
        key_prev = k;
        pad_prev = p;
        return;
    }
    if (bike->m_iCrashed && g_vol_tip == 2) {
        /* le jeu est en chute : il ne colle plus les mains au guidon et sa caméra ne gêne plus.
           On remet la moto droite tout de suite et on la garde là ; le pilote descend ensuite */
        bike_lift_apply(1.0f);
        g_lift_t = 2.45f;
        g_lift_stand = 1;
        g_body_searched = 1;
        g_soft_crash = 1;
        g_vol_tip = 3;
        g_fade_want = 0; /* le jeu est en chute : on rouvre l'image */
        log_line("moto : le jeu est en chute apres %.2f s de bascule, moto remise droite", g_vol_wait);
        if (g_vol_early) {
            memcpy(g_drive_bike, g_tele_pos, sizeof(g_drive_bike));
            g_drive_wait = 0.0f;
            g_drive_state = DRIVE_REQUESTED;
            ++g_drive_req_id;
        }
        return;
    }
    if (bike->m_iCrashed && g_vol_tip != 3) {
        if (g_vol_tip == 2)
            bike_lift_end();
        dismount_reset("chute de la moto");
        return;
    }
    if (g_vol_early && (g_vol_tip == 1 || g_vol_tip == 2)) {
        /* pilote en mod : bascule avant d'avoir le squelette (pilote encore assis, géré par le jeu) */
        uintptr_t body = mem_bike_body();
        float l = sqrtf(g_tele_R[2] * g_tele_R[2] + g_tele_R[8] * g_tele_R[8]);
        g_vol_wait += dt;
        if (g_vol_tip == 1) {
            float cp[3], R9[9];
            if (body && chassis_frame(body, cp, R9)) {
                float pitch = asinf(R9[5] > 1.0f ? 1.0f : (R9[5] < -1.0f ? -1.0f : R9[5]));
                if (body_matches(body) && g_tele_R[4] > 0.9f)
                    terrain_recal_wheels(body, "avant de soulever la moto"); /* v1.7.34 */
                if (body_matches(body) && bike_lift_begin(body, cp[1], pitch, NULL, log_line)) {
                    g_vol_tip = 2;
                    g_vol_wait = 0.0f;
                    log_line("moto : bascule pour passer le jeu en chute");
                }
            }
            if (g_vol_tip == 1 && g_vol_wait > 3.0f) {
                g_vol_tip = 0;
                g_fade_want = 0;
                log_line("moto : pieces de la moto pas trouvees (recherche %s), descente sans passer en chute",
                         mem_bike_searching() ? "en cours" : "finie");
                g_drive_state = DRIVE_REQUESTED;
                ++g_drive_req_id;
            }
        } else if (g_vol_wait > 2.0f || !body) {
            bike_lift_apply(1.0f);
            bike_lift_end();
            g_vol_tip = 0;
            g_fade_want = 0;
            log_line("moto : le jeu n'est pas passe en chute (%.1f s), moto remise droite", g_vol_wait);
            g_drive_state = DRIVE_REQUESTED;
            ++g_drive_req_id;
        } else if (l > 0.3f && g_tele_R[4] > 0.15f) {
            const float ax[3] = { g_tele_R[2] / l, 0.0f, g_tele_R[8] / l };
            bike_tip(body, ax, 3.0f, 0.55f); /* vers la droite : le pilote descend à gauche */
        }
        return;
    }
    if (g_drive_state != DRIVE_ACTIVE || g_walker.state != WALK_ON)
        return;
    if (g_vol_phase == 1) {
        /* descente : l'animation de montée à l'envers, côté gauche de la moto */
        if (!g_gesture.on) {
            float l = sqrtf(g_tele_R[2] * g_tele_R[2] + g_tele_R[8] * g_tele_R[8]), h;
            if (l < 0.3f) {
                dismount_reset("moto pas droite");
                return;
            }
            h = atan2f(g_tele_R[2] / l, g_tele_R[8] / l);
            memset(&g_gesture, 0, sizeof(g_gesture));
            g_gesture.fwd[0] = sinf(h); g_gesture.fwd[2] = cosf(h);
            g_gesture.right[0] = cosf(h); g_gesture.right[2] = -sinf(h);
            g_gesture.side = -1.0f;
            memcpy(g_gesture.pivot, g_tele_pos, sizeof(g_gesture.pivot));
            g_gesture.from_heading = h;
            g_gesture.on = 1;
            g_gesture.mount = 1;
            /* point d'arrivée : à côté de la selle (pose à 0,45 s), un pas plus loin */
            g_gesture.from[0] = g_walker.pos[0];
            g_gesture.from[1] = g_walker.pos[1];
            g_gesture.t = 0.45f;
            gesture_update(0, 0.0f, 1.0f);
            /* il finit debout à côté de la moto */
            g_gesture.from[0] = g_walker.pos[0] + g_gesture.right[0] * g_gesture.side * 0.15f;
            g_gesture.from[1] = g_walker.pos[1] + g_gesture.right[2] * g_gesture.side * 0.15f;
            g_vol_t = 0.0f;
            if (g_vol_tip != 3)
                g_vol_tip = dismount_crash_on() && !g_vol_early ? 1 : 0;
            g_vol_wait = 0.0f;
        }
        if (g_vol_tip == 1 || g_vol_tip == 2) {
            /* avant de descendre : le jeu passe en chute (moto basculée une fraction de seconde
               puis remise droite), pour qu'il lâche les mains et sa caméra. Le pilote reste assis */
            uintptr_t body = mem_bike_body();
            g_vol_wait += dt;
            g_gesture.t = VOL_T;
            gesture_update(0, 0.0f, 1.0f);
            g_walker.grab = 1.0f;
            g_bike_lock = 1;
            if (g_vol_tip == 1) {
                float cp[3], R9[9];
                if (body && chassis_frame(body, cp, R9)) {
                    float pitch = asinf(R9[5] > 1.0f ? 1.0f : (R9[5] < -1.0f ? -1.0f : R9[5]));
                    if (body_matches(body) && g_tele_R[4] > 0.9f)
                        terrain_recal_wheels(body, "avant de soulever la moto"); /* v1.7.34 */
                    if (body_matches(body) && bike_lift_begin(body, cp[1], pitch, NULL, log_line)) {
                        g_vol_tip = 2;
                        g_vol_wait = 0.0f;
                        log_line("moto : bascule pour passer le jeu en chute");
                    }
                }
                if (g_vol_tip == 1 && g_vol_wait > 1.5f) {
                    g_vol_tip = 0;
                    g_fade_want = 0;
                    log_line("moto : pieces de la moto pas trouvees, descente sans passer en chute");
                }
            } else {
                float l = sqrtf(g_tele_R[2] * g_tele_R[2] + g_tele_R[8] * g_tele_R[8]);
                if (g_vol_wait > 2.0f || !body) {
                    bike_lift_apply(1.0f);
                    bike_lift_end();
                    g_vol_tip = 0;
                    g_fade_want = 0;
                    log_line("moto : le jeu n'est pas passe en chute (%.1f s), moto remise droite", g_vol_wait);
                } else if (l > 0.3f && g_tele_R[4] > 0.15f) {
                    /* on arrête de la pousser une fois couchée (~80°) : la faire tourner plus loin
                       puis la remettre droite pouvait planter le jeu (v0.31.2) */
                    const float ax[3] = { g_tele_R[2] / l, 0.0f, g_tele_R[8] / l };
                    bike_tip(body, ax, -g_gesture.side * 3.0f, 0.55f); /* v0.30.4 : 9 rad/s retournait la moto et a planté le jeu */
                }
            }
            return;
        }
        g_vol_t += dt;
        g_gesture.t = VOL_T - g_vol_t > 0.0f ? VOL_T - g_vol_t : 0.0f;
        gesture_update(0, 0.0f, 1.0f);
        g_bike_lock = 1;
        if (g_vol_t < 0.3f)
            g_walker.grab = 1.0f;
        /* un seul appui : il lâche le guidon dès qu'il a les deux pieds au sol (fin de la
           descente), puis s'écarte d'un pas */
        if (g_vol_t > VOL_T - 0.55f)
            g_walker.grab = (VOL_T - g_vol_t) / 0.55f > 0.0f ? (VOL_T - g_vol_t) / 0.55f : 0.0f;
        if (g_vol_t >= VOL_T) {
            g_vol_phase = 5;
            g_vol_t = 0.0f;
        }
    } else if (g_vol_phase == 4) {
        /* debout à côté de la moto, mains au guidon : touche de descente = lâcher, E / Y = remonter */
        static int rel_prev = 1;
        int rel = (input_ok() && bind_key(g_cfg.key_dismount)) || (input_ok() && pad_ok && bind_pad(g_cfg.pad_dismount, &ps));
        int ek4 = input_ok() && bind_key(g_cfg.key_bike), ep4 = input_ok() && pad_ok && bind_pad(g_cfg.pad_bike, &ps);
        int e = ek4 || ep4;
        int go4 = mount_trigger(0, ek4, ep4, dt);
        g_vol_t += dt;
        g_gesture.t = 0.0f;
        gesture_update(0, 0.0f, 1.0f);
        g_bike_lock = 1;
        if (g_vol_t < 0.05f) {
            rel_prev = 1; /* la touche de descente doit être relâchée puis rappuyée */
        } else if (rel && !rel_prev) {
            g_vol_phase = 5;
            g_vol_t = 0.0f;
            log_line("moto : le pilote lache le guidon");
        } else if (go4 && g_vol_t > 0.3f) {
            g_gesture.from[0] = g_walker.pos[0];
            g_gesture.from[1] = g_walker.pos[1];
            g_gesture.from_heading = g_walker.heading;
            g_gesture.t = 0.0f;
            g_vol_t = 0.0f;
            g_vol_phase = 3;
            log_line("moto : le pilote remonte sur la moto");
        }
        rel_prev = rel;
        g_vol_e_prev = e;
    } else if (g_vol_phase == 5) {
        /* il lâche le guidon et fait un pas en arrière, à l'écart de la moto */
        float k = g_vol_t / 0.45f;
        g_vol_t += dt;
        if (k > 1.0f)
            k = 1.0f;
        g_walker.grab = 0.0f;
        g_walker.pos[0] += (g_gesture.right[0] * g_gesture.side * 0.30f - g_gesture.fwd[0] * 0.10f) * dt / 0.45f * (k < 1.0f);
        g_walker.pos[1] += (g_gesture.right[2] * g_gesture.side * 0.30f - g_gesture.fwd[2] * 0.10f) * dt / 0.45f * (k < 1.0f);
        g_bike_lock = 1;
        if (g_vol_t >= 0.45f && g_vol_tip == 3) {
            /* le jeu est en chute : le pilote est libre, la moto reste tenue droite */
            gesture_clear();
            g_bike_lock = 0;
            log_line("moto : moto lachee, pilote libre ; %s / %s a cote de la moto pour remonter",
                     cfg_key_name(g_cfg.key_bike[0]), cfg_pad_name(g_cfg.pad_bike));
            dismount_reset(NULL);
        } else if (g_vol_t >= 0.45f) {
            gesture_clear();
            g_bike_lock = 0;
            g_vol_phase = 2;
            g_vol_e_prev = 1;
            g_vol_free = 0.0f;
            log_line("moto : moto lachee, pilote libre ; %s / %s a cote de la moto pour remonter",
                     cfg_key_name(g_cfg.key_bike[0]), cfg_pad_name(g_cfg.pad_bike));
        }
    } else if (g_vol_phase == 2) {
        int ek = input_ok() && bind_key(g_cfg.key_bike), ep = input_ok() && pad_ok && bind_pad(g_cfg.pad_bike, &ps);
        int e = ek || ep;
        int go2 = mount_trigger(1, ek, ep, dt);
        g_vol_t += dt;
        if (go2 && g_vol_free > 0.5f) {
            log_line("moto : remonter demande (%s)", ek ? "clavier" : "manette");
            float dx = g_walker.pos[0] - g_tele_pos[0], dz = g_walker.pos[1] - g_tele_pos[2];
            float l = sqrtf(g_tele_R[2] * g_tele_R[2] + g_tele_R[8] * g_tele_R[8]), h;
            if (dx * dx + dz * dz > 2.2f * 2.2f) {
                log_line("moto : trop loin pour remonter (%.1f m)", sqrtf(dx * dx + dz * dz));
            } else if (l > 0.3f) {
                h = atan2f(g_tele_R[2] / l, g_tele_R[8] / l);
                memset(&g_gesture, 0, sizeof(g_gesture));
                g_gesture.fwd[0] = sinf(h); g_gesture.fwd[2] = cosf(h);
                g_gesture.right[0] = cosf(h); g_gesture.right[2] = -sinf(h);
                g_gesture.side = (dx * g_gesture.right[0] + dz * g_gesture.right[2]) >= 0.0f ? 1.0f : -1.0f;
                memcpy(g_gesture.pivot, g_tele_pos, sizeof(g_gesture.pivot));
                g_gesture.from[0] = g_walker.pos[0];
                g_gesture.from[1] = g_walker.pos[1];
                g_gesture.from_heading = g_walker.heading;
                g_gesture.on = 1;
                g_gesture.mount = 1;
                g_gesture.t = 0.0f;
                g_vol_t = 0.0f;
                g_vol_phase = 3;
                log_line("moto : le pilote remonte sur la moto");
            }
        }
        g_vol_e_prev = e;
        g_vol_free += dt; /* temps à pied depuis qu'il a lâché la moto */
    } else if (g_vol_phase == 3) {
        g_vol_t += dt;
        g_gesture.t = g_vol_t;
        gesture_update(0, 0.0f, 1.0f);
        g_bike_lock = 1;
        if (g_vol_t >= VOL_T + 0.15f) {
            /* assis, pieds sur les repose-pieds : on rend le pilote au jeu */
            gesture_clear();
            g_walker.state = WALK_OFF;
            g_drive_state = DRIVE_END_REQUESTED;
            dismount_reset("remonte sur la moto");
        }
    }
    (void)pad_prev;
}

/* Appelé 100 fois par seconde pendant que le joueur est en piste */
/* v0.41.02 : moto tenue sans gravité : ses roues ne touchent plus le sol (collisions coupées).
   Les suspensions bougent entre deux images (la physique tourne plus vite que nous) : les
   roues pendaient, touchaient la terre et le jeu creusait des trous. Remises dès que la
   gravité revient (pilote qui monte, respawn) ou que la moto est lâchée. */
static uintptr_t g_nocol_body[64];
static int g_nocol_n = 0;
static void bike_nocollide_update(int force_on)
{
    int want = !force_on && bike_lift_frozen();
    if (want && !g_nocol_n) {
        int n = bike_lift_bodies(g_nocol_body, 64), r = objects_body_collide(g_nocol_body, n, 0);
        if (r > 0) {
            g_nocol_n = n;
            log_line("moto : tenue, %d objet(s) de collision coupes (les roues ne creusent plus)", r);
        } else if (r == 0) {
            g_nocol_n = n; /* rien trouvé : on ne réessaie pas à chaque image */
            log_line("moto : tenue, objets de collision de la moto pas trouves");
        }
    } else if (!want && g_nocol_n) {
        int r = objects_body_collide(g_nocol_body, g_nocol_n, 1);
        g_nocol_n = 0;
        log_line("moto : collisions remises (%d objet(s))", r);
    }
}

/* v0.41.26 : moto tenue rendue au jeu tant que ses pièces existent encore (fin de session ou
   sortie de piste), puis tout est oublié : plus aucune écriture dans des pièces ou des objets
   de collision libérés (crash du jeu plus tard, mémoire abîmée) */
static void bike_release_all(const char *why)
{
    int was = bike_lift_active();
    if (why) {
        bike_nocollide_update(1);
        if (was)
            bike_lift_end();
    }
    g_nocol_n = 0;
    bike_lift_forget();
    objects_forget();
    g_mount_t = 0.0f;
    if (why && was)
        log_line("moto : tenue lachee et oubliee (%s)", why);
}

/* v0.41.05 : rend au jeu toutes les commandes que le plugin retenait (pilote à pied :
   clavier et manette réservés au plugin). Sans ça, plugin coupé par le bouton du menu après
   une session finie à pied = plus aucune commande dans la session suivante. */
static void release_inputs(const char *why)
{
    static int logged = 0;
    padinject_block(0);
    padinject_block_mask(0);
    padinject_block_reset(0);
    kbhook_block(0);
    kbhook_block_extra(0);
    if (g_respawn_key_down)
        press_respawn_key(0);
    if (why && logged++ < 20)
        log_line("commandes rendues au jeu (%s)", why);
}

/* v1.8.4 : motos vues par leur modèle 3D (le cadre, d'après le gfx.cfg de chaque moto) */
static void bike_model_cb(const char *low, const float M[12], void *user)
{
    char dir[400];
    const char *file;
    char *sl;
    float dx = M[3] - g_tele_pos[0], dy = M[7] - g_tele_pos[1], dz = M[11] - g_tele_pos[2];
    (void)user;
    snprintf(dir, sizeof(dir), "%s", low);
    sl = strrchr(dir, '\\');
    if (!sl)
        return;
    file = sl + 1;
    *sl = '\0';
    if (strcmp(file, ridepose_chassis_of(dir)))
        return;
    players_model_add(M, dx * dx + dy * dy + dz * dz < 1.2f * 1.2f);
}

/* v1.8 : pilotes à pied des autres joueurs = obstacles pour le nôtre */
static void players_refresh(void)
{
    int i;
    /* v1.8.4 : notre moto compte aussi, sauf pendant les gestes avec elle (la relever, monter,
       la tenir à la main) et au début d'une vraie chute (le corps part de dessus) */
    /* v1.8.16 : jamais quand il la tient (même debout sur sa « béquille » après F) ni juste après un
       geste avec elle : le pilote était repoussé de la moto qu'il tenait, moto et caméra tremblaient */
    static double busy_t = 0.0;
    /* v1.8.40 : moto lâchée debout (béquille) ou après une descente : elle redevient un obstacle
       (avant : on la traversait tant qu'elle restait debout) ; seulement pendant les gestes */
    int busy = g_bike_lock || g_gesture.on || g_mount_t > 0.0f || (bike_lift_active() && !g_lift_stand);
    int own;
    if (busy)
        busy_t = now_seconds();
    own = !busy && now_seconds() - busy_t > 1.5 && (g_walker.state == WALK_ON || g_knocked || g_rd.time > 1.2f);
    players_set_now(now_seconds()); /* v1.8.2 : position des motos prévue à cet instant */
    players_models_clear(own);
    skelscan_each_bike_model(g_rd.pos[RD_PELVIS], 6.0f, bike_model_cb, NULL);
    {
        static int last_m = -1;
        if (players_model_count() != last_m && (last_m < 0 || players_model_count() > last_m)) {
            log_line("collisions : %d moto(s) vue(s) par leur modele pres du pilote", players_model_count());
        }
        last_m = players_model_count();
    }
    players_walkers_clear();
    for (i = 0; i < NET_MAX_PEERS; ++i) {
        uint32_t id;
        char nm[32];
        float m[12], pts[RD_NUM_POINTS][3];
        DriveSnap sn;
        if (net_peer_pose(i, &id, nm, m, pts, &sn)) {
            players_walker_add((const float(*)[3])pts);
            /* v1.8.65 : leurs jambes poussent nos cônes (on simule nos cônes, ils les voient bouger) */
            cones_body_touch(pts[RD_FOOT_L], 0.12f);
            cones_body_touch(pts[RD_FOOT_R], 0.12f);
            cones_body_touch(pts[RD_KNEE_L], 0.12f);
            cones_body_touch(pts[RD_KNEE_R], 0.12f);
        }
    }
    {
        static int last = -1;
        if (players_walker_count() != last) {
            last = players_walker_count();
            log_line("collisions : %d autre(s) pilote(s) a pied", last);
        }
    }
}

/* v1.8.99 : temps passé dans le plugin (processeur), pour le journal */
static double g_tm_us[6];
static int g_tm_n[6];
/* v1.8.148 : dessus de rampe sous (x, z) pour les roues du jeu (coordonnées du jeu) */
static int ramp_ground_fn(float x, float z, float *h, float n[3])
{
    const float d = 0.25f;
    float c, a, b, e, f, l;
    if (!g_ramps_phys || !ramps_height(x, z, 1e9f, &c))
        return 0;
    if (!ramps_height(x + d, z, 1e9f, &a)) a = c;
    if (!ramps_height(x - d, z, 1e9f, &b)) b = c;
    if (!ramps_height(x, z + d, 1e9f, &e)) e = c;
    if (!ramps_height(x, z - d, 1e9f, &f)) f = c;
    n[0] = -(a - b) / (2.0f * d);
    n[1] = 1.0f;
    n[2] = -(e - f) / (2.0f * d);
    l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    n[0] /= l;
    n[1] /= l;
    n[2] /= l;
    *h = c - g_terrain_c;
    return 1;
}

static double tm_now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return f.QuadPart ? (double)c.QuadPart * 1e6 / (double)f.QuadPart : 0.0;
}
static void tm_add(int k, double t0)
{
    g_tm_us[k] += tm_now_us() - t0;
    g_tm_n[k]++;
}

/* v1.8.101 : temps de chaque partie de RunTelemetry (depuis la marque précédente) */
static double g_sec_us[16], g_sec_t;
static void tm_sec(int k)
{
    double t = tm_now_us();
    g_sec_us[k] += t - g_sec_t;
    g_sec_t = t;
}

static void run_telemetry(void *_pData, int _iDataSize, float _fTime, float _fPos);
/* v1.8.65 ... v1.8.197 : nos objets envoyés aux autres plugins (en piste depuis RunTelemetry, au stand
   depuis RaceTrackPosition : avant, au stand, ils s'effaçaient ou restaient figés chez les autres) */
static void objects_net_send(void)
{
    {
        /* v1.8.65 : en ligne : nos cônes envoyés aux autres plugins (souvent s'ils bougent, sinon 1 fois
           par seconde, et tout de suite quand on en pose / retire) */
        static double last = 0.0;
        static int last_n = -1;
        double nw = now_seconds();
        int cnt = cones_count();
        g_cone_now = nw;
        cones_net_clock(nw);
        if (net_running() &&
            (nw - last > (cones_net_moving() ? 0.05 : 1.0) || cnt != last_n)) {
            unsigned char buf[1100];
            int part = 0, np = 1, len;
            last = nw;
            last_n = cnt;
            do {
                len = cones_net_pack(buf, sizeof(buf), part, &np);
                if (len > 0) {
                    net_send_blob(1, buf, len);
                    net_sticky(1, part, np, buf, len); /* v1.8.196 : renvoyés au stand */
                }
            } while (++part < np && part < 12); /* v1.8.89 : 250 cônes : 11 parties */
        }
    }
    {
        /* v1.8.82 : nos bottes de foin envoyées aux autres plugins */
        static double last = 0.0;
        static int last_n = -1;
        double nw = now_seconds();
        int cnt = bales_count();
        bales_net_clock(nw);
        if (net_running() && (cnt || last_n > 0) &&
            (nw - last > (bales_net_moving() ? 0.1 : 1.0) || cnt != last_n)) {
            unsigned char buf[1100];
            int part = 0, np = 1, len;
            last = nw;
            last_n = cnt;
            do {
                len = bales_net_pack(buf, sizeof(buf), part, &np);
                if (len > 0) {
                    net_send_blob(CB_BALES, buf, len);
                    net_sticky(CB_BALES, part, np, buf, len);
                }
            } while (++part < np && part < 8); /* v1.8.89 : 200 bottes : 7 parties */
        }
    }
    {
        /* v1.8.119 : nos rampes envoyées aux autres plugins (fixes : toutes les secondes, ou dès un changement) */
        static double last = 0.0;
        static int last_n = -1;
        double nw = now_seconds();
        int cnt = ramps_count();
        ramps_net_clock(nw);
        if (net_running() && (cnt || last_n > 0) && (nw - last > 1.0 || cnt != last_n)) {
            unsigned char buf[700];
            int len = ramps_net_pack(buf, sizeof(buf));
            last = nw;
            last_n = cnt;
            if (len > 0) {
                net_send_blob(CB_RAMPS, buf, len);
                net_sticky(CB_RAMPS, 0, 1, buf, len);
            }
        }
    }
}

/* v1.8.197 : au stand (le jeu n'appelle plus RunTelemetry) : nos bottes, meubles et cônes continuent de
   vivre : poussés par les motos des autres, ils bougent, glissent et sont renvoyés */
static void objects_idle_tick(void)
{
    static double last = 0.0;
    double now = now_seconds();
    float dt;
    int i;
    if (!g_obj_session || GetTickCount() - g_rt_tick < 1500 || !(bales_count() || cones_count()))
        return;
    dt = (float)(now - last);
    last = now;
    if (dt <= 0.0f)
        return;
    if (dt > 0.1f)
        dt = 0.1f;
    if (bales_count()) {
        bales_set_furn_ground(furn_ground);
        bales_update(dt, bale_ground);
        for (i = 0; i < BALES_MAX; ++i) {
            float cp[3], yaw, v[3];
            int num, k;
            if (!bales_get(i, cp, &yaw))
                continue;
            for (k = -1; k <= 1; ++k) {
                float q[3] = { cp[0] + cosf(yaw) * 0.7f * k, cp[1] + 0.45f, cp[2] - sinf(yaw) * 0.7f * k };
                if (players_hit(q, 0.6f, 2.0f, v, &num)) {
                    float mp[3] = { q[0] - v[0] * 0.02f, q[1], q[2] - v[2] * 0.02f };
                    bales_bike_pass(mp, v, 0.3f);
                    break;
                }
            }
        }
    }
    if (cones_count()) {
        cones_update(dt, cone_ground);
        for (i = 0; i < CONES_MAX; ++i) {
            float cp[3], v[3];
            int down, num;
            if (!cones_get(i, cp, &down))
                continue;
            cp[1] += 0.35f;
            if (players_hit(cp, 0.18f, 1.5f, v, &num)) {
                v[0] *= 1.1f;
                v[2] *= 1.1f;
                cones_knock(i, v);
            }
        }
    }
    objects_net_send();
    {
        static double lg = 0.0;
        if (now - lg > 30.0) {
            lg = now;
            log_line("objets : au stand : %d botte(s)/meuble(s), %d cone(s) toujours simules et envoyes", bales_count(),
                     cones_count());
        }
    }
}

MXB_EXPORT void RunTelemetry(void *_pData, int _iDataSize, float _fTime, float _fPos)
{
    g_rt_tick = GetTickCount(); /* v1.8.197 : en piste (sinon les objets sont tenus par RaceTrackPosition) */
    double t0 = tm_now_us();
    static int said = 0;
    g_sec_t = t0;
    run_telemetry(_pData, _iDataSize, _fTime, _fPos);
    tm_sec(11);
    {
        double d = tm_now_us() - t0;
        g_fr_tele += d;
        if (d > g_fr_tele_max)
            g_fr_tele_max = d;
    }
    tm_add(0, t0);
    if (!said && glcam_render_thread()) {
        said = 1;
        log_line("temps plugin : RunTelemetry sur le fil %lu, image dessinee sur le fil %lu (%s)", GetCurrentThreadId(),
                 glcam_render_thread(), GetCurrentThreadId() == glcam_render_thread() ? "le meme : il compte dans les FPS" : "un autre");
    }
}

static void run_telemetry(void *_pData, int _iDataSize, float _fTime, float _fPos)
{
    static int was_enabled = 1;
    {
        /* v1.8.126 : casque et bottes lus à l'avance (toutes les 5 s, seulement s'il en manque) */
        static double et = 0.0;
        if (now_seconds() - et > 5.0) {
            et = now_seconds();
            edf_warm_start();
        }
    }
    bike_nocollide_update(0);
    players_refresh(); /* v1.8 */
    if (!g_cfg.enabled) {
        if (was_enabled)
            release_inputs("plugin coupe");
        was_enabled = 0;
        return;
    }
    was_enabled = 1;
    const SPluginsBikeData_t *bike = (const SPluginsBikeData_t *)_pData;
    float dt = 0.0f;
    (void)_fPos;

    if (!g_cfg.enabled || !bike || _iDataSize < (int)sizeof(SPluginsBikeData_t))
        return;

    if (g_has_time)
        dt = _fTime - g_last_time;
    g_last_time = _fTime;
    g_has_time = 1;
    if (dt < 0.0f)
        dt = 0.0f;

    tm_sec(0);
    bike_set_ground_trusted(g_terrain_ok && !g_terrain_prov); /* v1.7.16 */
    dismount_update(bike, dt);
    {
        /* réglages de la marche (buste, tête, hauteur) : pas pendant un geste avec la moto
           (monter, descendre, relever), dont l'animation est réglée sans eux */
        static float k = 1.0f;
        float want = (g_gesture.on || g_dismounted) ? 0.0f : 1.0f;
        k += (want - k) * (1.0f - expf(-4.0f * dt));
        g_walk_style = k;
    }
    g_walker.stand_lean = g_cfg.stand_lean * g_walk_style;
    /* à pied, les commandes du plugin passent en premier : le jeu ne voit plus la manette
       (sinon Y relevait la moto ET faisait l'action du jeu). La touche de respawn que le plugin
       presse passe quand même. */
    tm_sec(1);
    {
        /* à pied : touches et boutons du plugin prioritaires (réglable : clavier et manette à part,
           bascule par une touche du menu P). Le bouton de respawn du jeu passe toujours. */
        static int init = 0, kb_prev = 0, pd_prev = 0;
        static double keys_t = -10.0;
        /* v0.40.34 : aussi au sol, avant de se relever (E / Y pour se relever allaient aussi au jeu) */
        int on_foot = g_dismounted || g_drive_state == DRIVE_ACTIVE;
        int kb, pd = 0;
        PadState pps;
        if (!init) {
            init = 1;
            g_kbd_prio = g_cfg.kbd_prio;
            g_pad_prio = g_cfg.pad_prio;
        }
        kb = input_ok() && bind_key(g_cfg.key_prio_kbd);
        if (input_ok() && (bind_key(g_cfg.key_prio_pad) || (padinject_read(&pps) && bind_pad(g_cfg.pad_prio_pad, &pps))))
            pd = 1;
        if (kb && !kb_prev) {
            g_kbd_prio = !g_kbd_prio;
            snprintf(g_msg, sizeof(g_msg), "%s", tr(g_kbd_prio ? "Clavier prioritaire a pied : OUI (souris pour la camera)" : "Clavier prioritaire a pied : NON (souris libre, le jeu voit les touches)"));
            g_msg_t = now_seconds();
            log_line("touches : %s", g_msg);
        }
        if (pd && !pd_prev) {
            g_pad_prio = !g_pad_prio;
            snprintf(g_msg, sizeof(g_msg), "%s", tr(g_pad_prio ? "Manette prioritaire a pied : OUI" : "Manette prioritaire a pied : NON (le jeu voit aussi la manette)"));
            g_msg_t = now_seconds();
            log_line("touches : %s", g_msg);
        }
        kb_prev = kb;
        pd_prev = pd;
        if (now_seconds() - keys_t > 1.0) {
            /* touches du plugin à garder pour lui (relues : elles peuvent changer dans le menu P) */
            int v[32], n = 0, a;
            const int *lists[] = { g_cfg.key_forward, g_cfg.key_back, g_cfg.key_left, g_cfg.key_right, g_cfg.key_run,
                                   g_cfg.key_getup, g_cfg.key_bike, g_cfg.key_dismount, g_cfg.key_view, g_cfg.key_jump,
                                   g_cfg.key_crouch };
            keys_t = now_seconds();
            for (a = 0; a < 11; ++a) {
                if (lists[a][0] > 0)
                    v[n++] = lists[a][0];
                if (lists[a][1] > 0)
                    v[n++] = lists[a][1];
            }
            if (g_view_vk_game > 0)
                v[n++] = g_view_vk_game; /* touche de vue du jeu : à pied, pour la vue du plugin */
            /* v1.8.45 : menu des cônes (flèches, Entrée, Retour arrière) */
            v[n++] = VK_DOWN;
            v[n++] = VK_UP;
            v[n++] = VK_LEFT;
            v[n++] = VK_RIGHT;
            v[n++] = VK_RETURN;
            v[n++] = VK_BACK;
            v[n++] = 'B'; /* v1.8.46 : roue des cônes */
            kbhook_set_keys(v, n);
        }
        if (!on_foot || g_draw_state != 0) {
            if (g_was_foot)
                g_t_back = GetTickCount() | 1;
            g_was_foot = 0;
            cursor_restore(); /* plus à pied : curseur rendu au jeu */
            if (g_t_back && GetTickCount() - g_t_back > 2000) {
                CURSORINFO ci;
                memset(&ci, 0, sizeof(ci));
                ci.cbSize = sizeof(ci);
                GetCursorInfo(&ci);
                g_t_back = 0;
                log_line("souris : 2 s apres le retour sur la moto, curseur Windows %s",
                         (ci.flags & CURSOR_SHOWING) ? "VISIBLE" : "cache (normal)");
            }
        } else
            g_was_foot = 1;
        {
            /* marche forcée : à terre ou à pied, le respawn du jeu est bloqué ; un appui long sur
               la touche de respawn le fait quand même (retour sur la piste) */
            static float held = 0.0f, pulse = 0.0f;
            /* (aussi après une descente avec F : depuis la v0.40.28, le respawn y finit proprement la descente) */
            /* v0.40.54 : si le bouton de respawn du profil est aussi une touche du plugin (Y pour
               remonter chez certains), le jeu ne le voit jamais tant que le pilote est à pied ou
               remonte : appuyer sur Y pour monter faisait respawn tout de suite (montée coupée) */
            unsigned short rm = padinject_reset_mask();
            int conflict = rm && ((g_cfg.pad_getup | g_cfg.pad_bike | g_cfg.pad_dismount | g_cfg.pad_jump) & rm);
            int vk = padinject_reset_vk();
            int kconflict = vk && (vk == g_cfg.key_getup[0] || vk == g_cfg.key_getup[1] || vk == g_cfg.key_bike[0] ||
                                   vk == g_cfg.key_bike[1] || vk == g_cfg.key_jump[0] || vk == g_cfg.key_jump[1]);
            int real_block = g_visual_allowed && g_drive_state == DRIVE_ACTIVE && (g_cfg.force_walk || conflict || kconflict);
            int block = g_cfg.force_walk && g_visual_allowed && g_drive_state == DRIVE_ACTIVE && g_mount_t == 0.0f;
            int down = padinject_reset_held() || (vk && kbhook_down(vk));
            static int logged_conflict = 0;
            if ((conflict || kconflict) && !logged_conflict) {
                logged_conflict = 1;
                log_line("respawn : la touche de respawn du profil sert aussi au plugin : a pied, le jeu ne la voit plus "
                         "(respawn par appui long ou en remontant sur la moto)");
            }
            /* le plugin rejoue lui-même le respawn (montée, appui long) : il passe malgré le blocage */
            padinject_block_reset(real_block);
            kbhook_block_extra(real_block ? vk : 0);
            if ((block || (real_block && g_mount_t == 0.0f)) && down) {
                /* sans marche forcée : appui court (1,5 s) suffit */
                float need = g_cfg.force_walk ? g_cfg.respawn_hold : 1.5f;
                held += dt;
                if (held > 0.3f && held < need) {
                    /* compte à rebours à l'écran (appui long de 10 s par défaut) */
                    hold_progress("Garde appuye pour le respawn sur la piste", held, need, NULL, g_reset_pad, g_reset_keys);
                    g_msg_t = now_seconds();
                }
                if (held >= need && pulse == 0.0f) {
                    pulse = 0.001f;
                    press_respawn_key(1);
                    snprintf(g_msg, sizeof(g_msg), "%s", tr("Respawn sur la piste"));
                    g_msg_t = now_seconds();
                    log_line("respawn : appui long (%.1f s), retour sur la piste", held);
                }
            } else {
                held = 0.0f;
            }
            if (pulse > 0.0f) {
                pulse += dt;
                if (pulse > 0.3f) {
                    press_respawn_key(0);
                    pulse = 0.0f;
                    held = -100.0f; /* pas de second respawn tant qu'il ne relâche pas */
                }
            }
            if (!down && held < 0.0f)
                held = 0.0f;
        }
        padinject_block(g_pad_prio && on_foot && g_visual_allowed);
        /* à pied : le bouton de vue (celui du plugin et celui du jeu) sert à la vue du plugin */
        padinject_block_mask(on_foot && g_visual_allowed
                                 ? (unsigned short)((g_view_pad_game ? g_view_pad_game : (g_cfg.pad_view & 0xFFFF)) |
                                                    (g_cfg.pad_jump & 0xFFFF) | (g_cfg.pad_crouch & 0xFFFF) | 0x0002 /* croix Bas : cônes */ | 0x0001 /* v1.8.163 : Haut : zoom */ |
                                                    (g_cone_menu ? 0x330F : 0)) : 0); /* v1.8.54 : LB / RB : pages */
        kbhook_block(g_kbd_prio && on_foot && g_visual_allowed && !equip_menu_open());
    }
    tm_sec(2);
    if (g_drive_state == DRIVE_ACTIVE)
        /* v1.7.12 : assis sur la moto, aucun décalage de hauteur : le bassin et les pieds sont posés
           là où le jeu les met quand il roule (avant : la hauteur du menu P, -10 cm, le faisait
           s'enfoncer dans la selle et mettait les pieds presque au sol, vu de côté en jeu) */
        /* v1.7.13 : même hauteur partout à pied (marche, moto tenue, montée, assis) : la hauteur
           du menu P est réglée pour la marche ; sans elle pendant les gestes, il s'enfonçait de
           10 cm dans le sol en tenant la moto (vu en jeu) */
        drive_set_model(g_drive_is_mod, (g_walker.state != WALK_OFF ? g_cfg.rider_lift : 0.0f) + equip_rider_up()); /* réglage du menu P appliqué tout de suite */
    equip_hide_helmet(g_first_person && !g_cam_free_look && g_cfg.walk_camera && g_drive_state == DRIVE_ACTIVE && cam_body() && g_cam_on);
    equip_hide_cockpit(g_cfg.walk_camera && g_drive_state == DRIVE_ACTIVE && cam_body() && g_cam_on);
    {
        /* à pied : V (ou clic du stick droit) = vue 1re / 3e personne */
        static int v_prev = 1;
        PadState vps;
        int pr = padinject_read(&vps);
        /* manette : le bouton de vue du jeu s'il en a un (un seul bouton), sinon celui du plugin */
        int v = input_ok() && (bind_key(g_cfg.key_view) || (pr && !g_view_pad_game && bind_pad(g_cfg.pad_view, &vps)) ||
                               (pr && g_view_pad_game && !(g_view_pad_game & g_cfg.pad_crouch & 0xFFFF) && (vps.buttons & g_view_pad_game)) ||
                               (g_view_vk_game > 0 && (kbhook_down(g_view_vk_game) || (GetAsyncKeyState(g_view_vk_game) & 0x8000))));
        /* v1.8.25 : le bouton qui relève la moto / fait monter / se relever ne change jamais la vue
           (journal du 18/11 : « vue 1re personne » juste avant « E appuye » à chaque relevé) */
        unsigned act = (unsigned)((g_cfg.pad_bike | g_cfg.pad_getup | g_cfg.pad_dismount | g_cfg.pad_jump) & 0xFFFF);
        int act_down = (pr && (vps.buttons & act)) || (input_ok() && (bind_key(g_cfg.key_bike) || bind_key(g_cfg.key_getup) ||
                                                                      bind_key(g_cfg.key_dismount)));
        static double act_t = -10.0;
        if (act_down)
            act_t = now_seconds();
        if (v && !v_prev && (act_down || now_seconds() - act_t < 1.5)) {
            static int said = 0;
            if (said < 3) {
                ++said;
                log_line("camera : bouton de vue appuye avec un bouton d'action (relever / monter) : vue gardee");
            }
        } else if (v && !v_prev && g_drive_state == DRIVE_ACTIVE) { /* v1.8.11 : aussi au sol */
            log_line("camera : touche de vue : %s%s%s (boutons manette %04x)", bind_key(g_cfg.key_view) ? "touche du plugin " : "",
                     (pr && g_view_pad_game && (vps.buttons & g_view_pad_game)) ? "bouton de vue du jeu " : "",
                     (g_view_vk_game > 0 && (GetAsyncKeyState(g_view_vk_game) & 0x8000)) ? "touche de vue du jeu" : "",
                     pr ? vps.buttons : 0);
            g_first_person = !g_first_person;
            g_view_want = g_first_person; /* v1.8.24 : gardée pour rouler */
            log_line("camera : vue %s personne", g_first_person ? "1re" : "3e");
        }
        v_prev = v;
    }
    /* fondu au noir de la descente : vite vers le noir, plus doucement au retour */
    {
        static float fade_t = 0.0f; /* sécurité : jamais plus de 3 s d'écran noir */
        fade_t = g_fade_want ? fade_t + dt : 0.0f;
        if (fade_t > 3.0f)
            g_fade_want = 0;
    }
    g_fade = g_fade_want ? (g_fade + dt / 0.12f > 1.0f ? 1.0f : g_fade + dt / 0.12f)
                         : (g_fade - dt / 0.4f < 0.0f ? 0.0f : g_fade - dt / 0.4f);
    if (bike->m_iCrashed && !g_was_crashed) {
        if (g_soft_crash)
            log_line("CHUTE (descente volontaire) : pas de ragdoll, le pilote reste debout");
        else
            start_crash(bike);
        g_soft_crash = 0;
    }
    else if (!bike->m_iCrashed && g_was_crashed)
        end_crash();
    g_was_crashed = bike->m_iCrashed;

    g_last_bike_pos[0] = bike->m_fPosX;
    g_last_bike_pos[1] = bike->m_fPosY;
    g_last_bike_pos[2] = bike->m_fPosZ;
    g_ride_speed = bike->m_fSpeedometer;
    g_ride_steer = bike->m_fSteer;
    g_ride_roll = bike->m_fRoll;
    g_ride_crashed = bike->m_iCrashed;
    ++g_ride_n[0];
    if (!bike->m_iCrashed && !g_dismounted && g_drive_state == DRIVE_IDLE) {
        ++g_ride_n[1];
        if (bike->m_fSpeedometer > 4.0f) {
            ++g_ride_n[2];
            if (fabsf(bike->m_fSteer) < 3.0f && fabsf(bike->m_fRoll) < 6.0f)
                ++g_ride_n[3];
        }
    }
    g_ride_ok = !bike->m_iCrashed && bike->m_fSpeedometer > 4.0f && fabsf(bike->m_fSteer) < 3.0f && fabsf(bike->m_fRoll) < 6.0f;

    tm_sec(3);
    /* casque et bottes : menu (touche P), position du joueur, repère quand il roule droit */
    equip_menu_keys(game_focused(), g_cfg.key_menu[0], g_cfg.key_menu[1]);
    {
        double t0 = tm_now_us();
    /* v1.8.45 : cônes : menu (flèche Bas ou B à pied), chute, motos qui passent dessus */
    cone_menu_update();
    cone_net_update(); /* v1.8.66 */
    cone_place_update(dt);
    bale_carry_update(dt); /* v1.8.84 ; v1.8.85 : animations */
        tm_add(4, t0);
    }
    tm_sec(4);
    objects_net_send(); /* v1.8.197 : aussi au stand (RaceTrackPosition) */
    {
        /* v1.8.94 : versions du plugin comparées entre joueurs (même physique des objets pour tous) */
        static double vt = 0.0;
        if (net_running() && now_seconds() - vt > 5.0) {
            vt = now_seconds();
            net_send_blob(CB_VER, (const unsigned char *)PLUGIN_VERSION, (int)strlen(PLUGIN_VERSION));
        }
        if (g_peer_ver_new) {
            g_peer_ver_new = 0;
            log_line("reseau : plugin d'un autre joueur en version %s (la mienne : %s)", g_peer_ver, PLUGIN_VERSION);
            if (strcmp(g_peer_ver, PLUGIN_VERSION)) {
                snprintf(g_msg, sizeof(g_msg), "Autre joueur : plugin %s (toi %s) : installez la meme version", g_peer_ver,
                         PLUGIN_VERSION);
                g_msg_t = now_seconds() + 4.0;
            }
        }
    }
    glcam_set_scene_active((cones_count() > 0 || cones_net_remote_count() > 0 || bales_count() > 0 ||
                            bales_net_remote_count() > 0 || ramps_count() > 0 || ramps_net_remote_count() > 0) &&
                           g_cfg.cones_3d);
    cones_set_lift(g_cfg.cones_lift);
    bales_set_quality(0); /* v1.8.108 : option « objets optimisés » retirée (toujours haute qualité) */
    glcam_set_scene_fast(0);
    tm_sec(5);
    {
        /* v1.8.52 : état des cônes retenu au temps de la rediffusion (pour les y revoir) */
        double rcl;
        static int warned = 0;
        if (rclock_live_time(&rcl)) {
            cones_hist_record(rcl);
            bales_hist_record(rcl); /* v1.8.82 */
            ramps_hist_record(rcl); /* v1.8.159 */
        }
        else if ((cones_count() || cones_hist_count()) && !warned) {
            warned = 1;
            log_line("cones : horloge de la rediffusion pas encore connue : cones pas retenus pour la rediffusion");
        }
    }
    {
        /* v1.8.87 : bottes de foin dans la physique du jeu : la moto (et le pilote du jeu) butent
           dedans. Seulement quand le plugin peut toucher à la physique (hors ligne, serveur ragdoll). */
        static int had = 0;
        static unsigned char was_on[1024]; /* v1.8.119 : 512 et plus : rampes */
        static BaleBox bx[1024];
        static int sl[1024];
        static float bkp[3];
        int want = bales_count() > 0 || bales_net_remote_count() > 0;
        int active = want && g_event_allowed && g_cfg.enabled && objects_obstacle_ready();
        {
            static double lt = 0.0;
            if (want && !active && now_seconds() - lt > 15.0) {
                lt = now_seconds();
                log_line("bottes : pas dans la physique du jeu (physique permise %d, plugin %d)", g_event_allowed, g_cfg.enabled);
            }
        }
        bkp[0] = bike->m_fPosX;
        bkp[1] = bike->m_fPosY;
        bkp[2] = bike->m_fPosZ;
        bales_set_bike(g_drive_state == DRIVE_IDLE && !g_dismounted ? bkp : NULL,
                       sqrtf(bike->m_fVelocityX * bike->m_fVelocityX + bike->m_fVelocityZ * bike->m_fVelocityZ));
        if (active || had) {
            double t0 = tm_now_us();
            int nb = active ? bales_boxes(bx, sl, 512) : 0, k;
            unsigned char now_on[1024];
            for (k = 0; k < nb; ++k)
                if (sl[k] >= 512)
                    sl[k] = -1; /* (bottes : jamais dans les numéros des rampes) */
            /* v1.8.120 : rampes : plus de boîtes (la moto ne roulait pas dessus : seules les sphères du châssis
               les touchaient) ; elles sont écrites dans le relief du jeu, voir plus bas */
            /* v1.8.143 : essai : boîtes en plus du relief (au-dessus du plus haut point de la piste, le relief ne suffit pas) */
            /* (v1.8.144 : coupé : essai du 23/11 : roues toujours sans contact sur les boîtes, seul le châssis les touche) */
            memset(now_on, 0, sizeof(now_on));
            for (k = 0; k < nb; ++k) {
                float c[3] = { bx[k].c[0], bx[k].c[1] - g_terrain_c, bx[k].c[2] };
                if (sl[k] < 0 || sl[k] >= 1024)
                    continue;
                if (objects_obstacle(sl[k], 1, c, bx[k].R, bx[k].h))
                    now_on[sl[k]] = 1;
            }
            for (k = 0; k < 1024; ++k)
                if (was_on[k] && !now_on[k])
                    objects_obstacle(k, 0, NULL, NULL, NULL);
            memcpy(was_on, now_on, sizeof(was_on));
            had = nb > 0;
            tm_add(2, t0);
        }
    }
    {
        /* v1.8.120 : rampes FMX dans le relief du jeu : les roues roulent dessus comme sur la terre (suspensions,
           pneus). Réécrites 4 fois par seconde (les ornières les creusent aussi) ; sol d'origine remis quand une
           rampe est retirée. Seulement quand le plugin peut toucher à la physique (hors ligne, serveur ragdoll). */
        /* v1.8.121 : liste refaite seulement quand les rampes changent ; entretien à chaque appel */
        static uint32_t sig = 0;
        static int had = 0;
        static double lt = 0.0;
        /* v1.8.151 : plus de rampes dans le relief quand les roues les ont par les maillages de la piste (le relief,
           en cases de plusieurs dizaines de cm, faisait des rampes en escalier et plus larges, mal pour les étroites) */
        int want_r = (ramps_count() > 0 || ramps_net_remote_count() > 0) && g_event_allowed && g_cfg.enabled && g_terrain_ok &&
                     !groundhook_ready();
        uint32_t s = want_r ? ramps_signature() : 1u;
        if (s && (s != sig || want_r != had)) {
            sig = s;
            terrain_raise_begin();
            if (want_r)
                ramps_terrain(g_terrain_c);
            if (terrain_raise_end(log_line))
                objects_relief_moved();
            had = want_r;
        } else if (want_r && terrain_raise_keep(log_line))
            objects_relief_moved();
        if ((want_r || ((ramps_count() > 0 || ramps_net_remote_count() > 0) && g_event_allowed)) && now_seconds() - lt > 10.0) {
            int r = terrain_raise_resets();
            lt = now_seconds();
            if (r)
                log_line("rampes : %d points remis a la hauteur de la rampe en 10 s (le jeu les avait rabaisses)", r);
            {
                static unsigned long lc = 0, lh = 0;
                unsigned long c = groundhook_calls(), hh = groundhook_hits();
                log_line("rampes : roues : %lu demandes de surface en 10 s, %lu sur une rampe", c - lc, hh - lh);
                {
                    static unsigned long pc[6];
                    unsigned long cs[6];
                    groundhook_contact_stats(cs);
                    log_line("rampes : contacts ODE en 10 s : dessus %lu (refuses %lu, gardes %lu), murs %lu (refuses %lu, gardes %lu)",
                             cs[0] - pc[0], cs[1] - pc[1], cs[2] - pc[2], cs[3] - pc[3], cs[4] - pc[4], cs[5] - pc[5]);
                    memcpy(pc, cs, sizeof(pc));
                    groundhook_contact_log(log_line);
                    log_line("rampes : moto en (%.2f %.2f %.2f) (jeu : y %.2f)", bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ,
                             bike->m_fPosY - g_terrain_c);
                }
                lc = c;
                lh = hh;
            }
            terrain_raise_diag(log_line);
        }
    }
    {
        /* v1.8.146 : rampes en maillage de triangles dans la physique du jeu, comme les passerelles des pistes :
           les roues roulent dessus à n'importe quelle hauteur (le relief s'arrête au plus haut point de la piste) */
        static uint32_t msig = 0;
        static float mc = 0.0f;
        static double mt = 0.0;
        int want_m = (ramps_count() > 0 || ramps_net_remote_count() > 0) && g_event_allowed && g_cfg.enabled && g_terrain_ok;
        g_ramps_phys = want_m;
        uint32_t s = want_m ? ramps_signature() : 0u;
        int redo = (want_m && s && (s != msig || mc != g_terrain_c)) || (!want_m && objects_ramp_meshes_count() > 0);
        static int gr_done = 0;
        groundhook_poll();
        if (!redo && want_m && (objects_ramp_meshes_stale() || !gr_done || groundhook_stale()) && groundhook_ready() &&
            now_seconds() - mt > 2.0)
            redo = 1;
        if (!redo && want_m && objects_ramp_meshes_stale() && now_seconds() - mt > 2.0)
            redo = 1;
        if (redo) {
            static float mv[RAMP_MESH_V * 3];
            static int mi[RAMP_MESH_T * 3];
            int n = want_m ? ramps_mesh_count() : 0, k, ok = 0, nt = 0;
            double t0 = tm_now_us();
            mt = now_seconds();
            objects_ramp_meshes_log(log_line);
            objects_ramp_meshes_clear();
            for (k = 0; k < n; ++k) {
                int nv = ramps_mesh(k, g_terrain_c, mv, mi, &nt);
                if (nv > 0)
                    ok += objects_ramp_mesh_add(mv, nv, mi, nt);
            }
            {
                /* v1.8.152 : murs (côtés, dos) : la moto et le pilote ne traversent plus la rampe par les côtés */
                int w = 0;
                for (k = 0; k < n; ++k) {
                    int nv = ramps_mesh_walls(k, g_terrain_c, mv, mi, &nt);
                    if (nv > 0)
                        w += objects_ramp_wall_add(mv, nv, mi, nt);
                }
                if (n)
                    log_line("rampes : %d/%d murs dans la physique du jeu", w, n);
                {
                    uintptr_t st, sw;
                    objects_ramp_spaces(&st, &sw);
                    groundhook_contact_watch(st, sw);
                }
            }
            {
                /* v1.8.149 : les roues : rampes ajoutées aux maillages de la piste (comme la passerelle d'Italy) */
                static float tv[RAMP_MESH_V * 3];
                static int ti[RAMP_MESH_T * 3];
                int tnt = 0;
                groundhook_ramps_begin();
                for (k = 0; k < n; ++k) {
                    int nv = ramps_mesh_top(k, g_terrain_c, tv, ti, &tnt);
                    if (nv > 0)
                        groundhook_ramps_add(tv, nv, ti, tnt);
                }
                gr_done = groundhook_ramps_commit(log_line) && n > 0;
            }
            msig = s;
            mc = g_terrain_c;
            if (n || ok)
                log_line("rampes : %d/%d en maillage dans la physique du jeu (%.1f ms)", ok, n, (tm_now_us() - t0) / 1000.0);
        }
    }
    {
        /* v1.8.96 : poussées à pied des petites bottes des autres joueurs : envoyées au propriétaire */
        uint32_t pid;
        int ps;
        float pdx, pdz;
        while (bales_net_push_take(&pid, &ps, &pdx, &pdz)) {
            unsigned char b[16];
            memcpy(b, &pid, 4);
            b[4] = (unsigned char)ps;
            b[5] = b[6] = b[7] = 0;
            memcpy(b + 8, &pdx, 4);
            memcpy(b + 12, &pdz, 4);
            if (net_running())
                net_send_blob(CB_BPUSH, b, 16);
        }
    }
    tm_sec(6);
    if (g_event_allowed && !bike->m_iCrashed && !g_dismounted && g_drive_state == DRIVE_IDLE &&
        (bales_count() > 0 || bales_net_remote_count() > 0)) {
        /* v1.8.96 : la botte ronde est un mur pour notre moto : contrôlé par le plugin à chaque image
           (avant, l'avant et l'arrière de la moto), en plus de la physique du jeu */
        float fw[2] = { bike->m_aafRot[0][2], bike->m_aafRot[2][2] }, l = sqrtf(fw[0] * fw[0] + fw[1] * fw[1]);
        float best = 0.0f, bo[2] = { 0.0f, 0.0f }, bn[2] = { 0.0f, 0.0f };
        int k;
        if (l > 0.1f) {
            fw[0] /= l;
            fw[1] /= l;
        }
        for (k = -1; k <= 1; ++k) {
            float p[3] = { bike->m_fPosX + fw[0] * 0.65f * k, bike->m_fPosY, bike->m_fPosZ + fw[1] * 0.65f * k }, o[2], n[2];
            float d = bales_round_block(p, 0.35f, o, n);
            if (d > best) {
                best = d;
                bo[0] = o[0];
                bo[1] = o[1];
                bn[0] = n[0];
                bn[1] = n[1];
            }
        }
        if (best > 0.01f) {
            uintptr_t body = mem_bike_body();
            static double lt = 0.0;
            if (body && body_matches(body)) {
                float sp = sqrtf(bike->m_fVelocityX * bike->m_fVelocityX + bike->m_fVelocityZ * bike->m_fVelocityZ);
                bike_block(body, bo[0], bo[1], bn[0], bn[1]);
                if (now_seconds() - lt > 2.0) {
                    lt = now_seconds();
                    log_line("bottes : moto arretee par la ronde (entree de %.2f m, %.1f m/s)", best, sp);
                }
            }
        }
    }
    if (g_event_allowed && g_cfg.enabled && !bike->m_iCrashed && !g_dismounted && g_drive_state == DRIVE_IDLE &&
        (ramps_count() > 0 || ramps_net_remote_count() > 0)) {
        /* v1.8.155 : côtés et dos des rampes : murs pour notre moto (les murs ODE ne la retiennent pas : contacts
           gardés par le jeu mais trop mous) ; contrôlé par le plugin à chaque image, comme la botte ronde */
        /* (v1.8.156 : points le long de la moto en 3D : penchée en montant la rampe, son avant est plus haut) */
        float fw[3] = { bike->m_aafRot[0][2], bike->m_aafRot[1][2], bike->m_aafRot[2][2] };
        float l = sqrtf(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
        float best = 0.0f, bo[2] = { 0.0f, 0.0f }, bn[2] = { 0.0f, 0.0f };
        int k;
        if (l > 0.1f) {
            fw[0] /= l;
            fw[1] /= l;
            fw[2] /= l;
        }
        for (k = -1; k <= 1; ++k) {
            float p[3] = { bike->m_fPosX + fw[0] * 0.65f * k, bike->m_fPosY + fw[1] * 0.65f * k, bike->m_fPosZ + fw[2] * 0.65f * k };
            float o[2], n[2];
            float d = ramps_wall_block(p, 0.3f, o, n);
            if (d > best) {
                best = d;
                bo[0] = o[0];
                bo[1] = o[1];
                bn[0] = n[0];
                bn[1] = n[1];
            }
        }
        {
            /* v1.8.156 : moto sur le tablier (son centre pas sous la surface de la rampe à sa place) : jamais bloquée */
            float dk;
            if (best > 0.0f && ramps_height(bike->m_fPosX, bike->m_fPosZ, 1e9f, &dk) && bike->m_fPosY > dk - 0.6f)
                best = 0.0f;
        }
        if (best > 0.01f && best < 1.5f) {
            uintptr_t body = mem_bike_body();
            static double lt = 0.0;
            if (body && body_matches(body)) {
                float sp = sqrtf(bike->m_fVelocityX * bike->m_fVelocityX + bike->m_fVelocityZ * bike->m_fVelocityZ);
                bike_block(body, bo[0], bo[1], bn[0], bn[1]);
                if (now_seconds() - lt > 2.0) {
                    float top = 0.0f;
                    lt = now_seconds();
                    ramps_height(bike->m_fPosX, bike->m_fPosZ, 1e9f, &top);
                    log_line("rampes : moto arretee par le cote d'une rampe (entree de %.2f m, %.1f m/s, moto y %.2f, tablier %.2f, "
                             "penchee %.2f)", best, sp, bike->m_fPosY, top, fw[1]);
                }
            }
        }
    }
    if (g_event_allowed && !bike->m_iCrashed && !g_dismounted && g_drive_state == DRIVE_IDLE &&
        (bales_net_remote_count() > 0 || cones_net_remote_count() > 0)) {
        /* v1.8.94 : en ligne : les petites bottes et les plots des autres joueurs freinent aussi notre moto
           (leur propriétaire les fait glisser chez lui : notre moto y est vue comme une moto d'un autre) */
        static uint32_t last_key = 0xFFFFFFFFu;
        static double last_t = 0.0;
        float bp[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ };
        float sp = sqrtf(bike->m_fVelocityX * bike->m_fVelocityX + bike->m_fVelocityZ * bike->m_fVelocityZ);
        uint32_t key = 0, cid = 0;
        int cslot = 0;
        float k = 1.0f;
        if (sp > 1.0f) {
            if (bales_net_bike_touch(bp, 0.7f, &key))
                k = 0.55f;
            else if (cones_net_touching(bp, 0.5f, &cid, &cslot)) {
                key = cid * 256u + (uint32_t)cslot + 0x80000000u;
                k = 0.92f;
            }
        }
        if (k < 1.0f && (key != last_key || now_seconds() - last_t > 1.5)) {
            uintptr_t body = mem_bike_body();
            last_key = key;
            last_t = now_seconds();
            if (body && body_matches(body)) {
                bike_slow(body, k);
                log_line("%s d'un autre joueur : moto freinee (%.1f m/s)", k < 0.9f ? "bottes : botte" : "cones : plot", sp);
            }
        }
    }
    tm_sec(7);
    if (bales_count()) {
        /* v1.8.82 : bottes de foin : glissent, suivent le sol ; motos qui les percutent */
        int i;
        double t0 = tm_now_us();
        bales_set_furn_ground(furn_ground);
        bales_update(dt, bale_ground);
        tm_add(3, t0);
        if (!bike->m_iCrashed && !g_dismounted && g_drive_state == DRIVE_IDLE) {
            float bp[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ };
            float bv[3] = { bike->m_fVelocityX, bike->m_fVelocityY, bike->m_fVelocityZ };
            /* v1.8.89 : une petite botte percutée freine un peu la moto (elle est lourde) */
            if (bales_bike_pass(bp, bv, 0.7f) > 0 && g_event_allowed) {
                uintptr_t body = mem_bike_body();
                if (body && body_matches(body)) {
                    int nb2 = bike_slow(body, 0.55f); /* v1.8.90 : 0,78 : pas assez */
                    log_line("bottes : moto freinee par une petite botte (%d pieces)", nb2);
                }
            }
        }
        for (i = 0; i < BALES_MAX; ++i) {
            float cp[3], yaw, v[3];
            int num, k;
            if (!bales_get(i, cp, &yaw))
                continue;
            /* trois points le long de la botte, à mi-hauteur : une moto d'un autre joueur la percute */
            for (k = -1; k <= 1; ++k) {
                float q[3] = { cp[0] + cosf(yaw) * 0.7f * k, cp[1] + 0.45f, cp[2] - sinf(yaw) * 0.7f * k };
                if (players_hit(q, 0.6f, 2.0f, v, &num)) {
                    float mp[3] = { q[0] - v[0] * 0.02f, q[1], q[2] - v[2] * 0.02f };
                    bales_bike_pass(mp, v, 0.3f);
                    break;
                }
            }
        }
    }
    if (cones_count() || bales_count() || ramps_count() || bales_net_remote_count() || cones_net_remote_count() ||
        ramps_net_remote_count()) { /* v1.8.197 : aussi pour les objets des autres (journal du dessin 3D) */
        static double st_t = 0.0;
        if (now_seconds() - st_t > 10.0) {
            /* v1.8.49 : dessin 3D des objets : ce qui s'est passé pendant les 10 s */
            char st[300], pi[900];
            st_t = now_seconds();
            glcam_scene_stats(st, sizeof(st));
            log_line("cones : 3D : %s", st);
            glcam_pass_info(pi, sizeof(pi));
            log_line("objets 3D : %s", pi);
            /* v1.8.99 : temps du plugin pendant ces 10 s */
            log_line("temps plugin (10 s) : RunTelemetry %d appels %.0f us, Draw %d appels %.0f us, "
                     "bottes dans la physique %d fois %.0f us, mise a jour des objets %d fois %.0f us, menus/pose/port %.0f us",
                     g_tm_n[0], g_tm_us[0], g_tm_n[1], g_tm_us[1], g_tm_n[2], g_tm_us[2], g_tm_n[3], g_tm_us[3], g_tm_us[4]);
            log_line("temps RunTelemetry par partie (us / 10 s) : debut %.0f, descente %.0f, marche/commandes %.0f, "
                     "casque/camera/fondu %.0f, menus/pose/port %.0f, objets/physique %.0f, (suite) %.0f, freinage %.0f, "
                     "mise a jour bottes/cones %.0f, (suite) %.0f, debout/moto/camera %.0f [recherche %.0f, roues %.0f, touche E %.0f, "
                     "tenue %.0f], ragdoll/pilotage %.0f ; bottes qui bougent %d / %d",
                     g_sec_us[0], g_sec_us[1], g_sec_us[2], g_sec_us[3], g_sec_us[4], g_sec_us[5], g_sec_us[6],
                     g_sec_us[7], g_sec_us[8], g_sec_us[9], g_sec_us[10], g_sec_us[12], g_sec_us[13], g_sec_us[14],
                     g_sec_us[15], g_sec_us[11], bales_awake(), bales_count());
            memset(g_sec_us, 0, sizeof(g_sec_us));
            memset(g_tm_us, 0, sizeof(g_tm_us));
            memset(g_tm_n, 0, sizeof(g_tm_n));
        }
    }
    if (cones_count()) {
        int i;
        cones_update(dt, cone_ground);
        if (!bike->m_iCrashed && !g_dismounted && g_drive_state == DRIVE_IDLE) {
            float bp[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ };
            float bv[3] = { bike->m_fVelocityX, bike->m_fVelocityY, bike->m_fVelocityZ };
            int kn = cones_bike_pass(bp, bv, 0.8f);
            if (kn) {
                log_line("cones : renverse par notre moto (%.1f m/s)", sqrtf(bv[0] * bv[0] + bv[2] * bv[2]));
                /* v1.8.90 : un plot percuté freine un peu la moto */
                if (g_event_allowed) {
                    uintptr_t body = mem_bike_body();
                    if (body && body_matches(body))
                        bike_slow(body, kn > 1 ? 0.86f : 0.92f);
                }
            }
        }
        for (i = 0; i < CONES_MAX; ++i) {
            float cp[3], v[3];
            int down, num;
            if (!cones_get(i, cp, &down))
                continue;
            cp[1] += 0.35f;
            if (players_hit(cp, 0.18f, 1.5f, v, &num)) {
                v[0] *= 1.1f;
                v[2] *= 1.1f;
                if (!down)
                    log_line("cones : n %d renverse par la moto n %d", i, num);
                cones_knock(i, v);
            }
        }
    }
    tm_sec(8);
    {
        float pp[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ };
        if (g_drive_state == DRIVE_ACTIVE)
            memcpy(pp, g_rd.pos[RD_PELVIS], sizeof(pp));
        equip_set_player_pos(pp);
        {
            float mp[3];
            void *sk = g_drive_state == DRIVE_ACTIVE ? drive_skeleton() : NULL;
            equip_set_player_model_pos(sk && skelscan_model_pos(sk, mp) ? mp : NULL);
            equip_set_player_head(g_drive_state == DRIVE_ACTIVE ? g_rd.pos[RD_HEAD] : NULL);
            {
                /* v1.8.79 : le casque suit l'os de la tête (regard devant après F ou une chute) */
                float hs[12], hn[12];
                int hn_n = g_drive_state == DRIVE_ACTIVE ? drive_head_frames(hs, hn) : 0;
                equip_set_head_frames(hs, hn, hn_n);
            }
            equip_set_on_foot(g_drive_state == DRIVE_ACTIVE && g_walker.state != WALK_OFF);
        }
        {
            /* avant : la moto quand il roule, sinon là où il regarde en marchant */
            const float up[3] = { 0.0f, 1.0f, 0.0f };
            float fw[3] = { bike->m_aafRot[0][2], 0.0f, bike->m_aafRot[2][2] };
            if (g_drive_state == DRIVE_ACTIVE && g_walker.state != WALK_OFF) {
                fw[0] = sinf(g_walker.heading);
                fw[2] = cosf(g_walker.heading);
            }
            equip_set_rider_frame(up, fw);
        }
    }

    /* pilote debout depuis 1 s : recherche de la moto (pour la relever), statistiques caméra */
    tm_sec(9);
    {
        static int walk_cap = 0;
        static float retry_t = 0.0f;
        static float walk_t = 0.0f;
        if (g_drive_state == DRIVE_ACTIVE && g_walker.state == WALK_ON) {
            walk_t += dt;
            /* la moto est couchée : on retrouve ses pièces physiques. Sur une pente elle peut
               encore glisser et la recherche échouer : on réessaie toutes les 1,5 s */
            if (walk_cap && !mem_bike_searching() && !mem_bike_body() && walk_t > retry_t) {
                walk_cap = 0;
                if (g_cfg.bike_lift && g_event_allowed)
                    log_line("moto : pas trouvee, nouvelle recherche");
            }
            if (!walk_cap && g_cfg.bike_lift) {
                /* d'abord le châssis de la chute précédente (mêmes corps) : instantané */
                const float bp[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ };
                if (mem_bike_revalidate(bp) && body_matches(mem_bike_body())) {
                    walk_cap = 1;
                    g_body_searched = 1;
                    retry_t = 1e9f;
                }
            }
            if (walk_t > 0.5f && !walk_cap) {
                walk_cap = 1;
                retry_t = walk_t + 0.8f;
                if (g_cfg.bike_lift && g_event_allowed) {
                    float rot[9], pos[3] = { bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ },
                          vel[3] = { bike->m_fVelocityX, bike->m_fVelocityY, bike->m_fVelocityZ };
                    int i;
                    for (i = 0; i < 9; ++i)
                        rot[i] = bike->m_aafRot[i / 3][i % 3];
                    bike_find(rot, pos, vel);
                    g_body_searched = 1;
                }
            }
        } else {
            walk_t = 0.0f;
            if (g_drive_state != DRIVE_ACTIVE)
                walk_cap = 0;
        }
        tm_sec(12);
        /* moto couchée pendant qu'on marche : sa roue arrière tourne (moteur en prise) et creuse
           la piste ; on l'arrête (v0.40.8) */
        if (!(bike->m_iCrashed && g_event_allowed && g_drive_state == DRIVE_ACTIVE))
            g_calm_chassis = 0;
        if (bike->m_iCrashed && g_event_allowed && g_drive_state == DRIVE_ACTIVE) { /* (aussi tenue et pendant la montée) */
            static int logged = 0;
            uintptr_t cb = bike_lift_active() ? bike_lift_chassis() : mem_bike_body();
            if (cb && !bike_lift_active() && !body_matches(cb))
                cb = 0;
            g_calm_chassis = cb; /* aussi à chaque image (rendu) */
            if (cb && bike_calm_wheels(cb) && !logged) {
                logged = 1;
                log_line("moto : roues de la moto couchee arretees (elles creusaient la piste)");
            }
        }
        tm_sec(13);
        /* moto : E près de la moto couchée = le pilote la relève, puis la tient debout */
        {
            static uintptr_t listed = 0;
            static int e_prev = 0, pad_free = 0;
            int e = 0, ek_l = 0, ep_l = 0, go_l;
            uintptr_t body = mem_bike_body();
            /* moto tenue debout : on garde ses pièces même si une recherche est en cours
               (v0.35.12 : E ne faisait plus rien après une 2e descente) */
            if (!body && bike_lift_active())
                body = bike_lift_chassis();
#ifdef _WIN32
            {
                PadState ps;
                int ok = g_cfg.bike_lift && g_event_allowed && !g_dismounted && input_ok();
                ek_l = ok && bind_key(g_cfg.key_bike);
                ep_l = ok && padinject_read(&ps) && bind_pad(g_cfg.pad_bike, &ps);
                e = ek_l || ep_l;
                if (g_getup_release) {
                    if (!e)
                        g_getup_release = 0;
                    ek_l = ep_l = e = 0;
                }
            }
#endif
            if (body && body != listed) {
                listed = body;
                bike_log_bodies(body, log_line);
            }
            /* monter : manette = bouton gardé 2 s, relâché depuis qu'on a relevé la moto */
            if (!bike_lift_active() || g_lift_t < 0.2f)
                pad_free = 0;
            if (!ep_l)
                pad_free = 1;
            go_l = mount_trigger(2, ek_l, ep_l && pad_free && bike_lift_active() && g_lift_t > 1.9f, dt);
            if (e && !e_prev)
                log_line("moto : %s appuye (moto %s, tenue %d depuis %.1f s, montee %.1f s, recherche %d)",
                         cfg_key_name(g_cfg.key_bike[0]), body ? "trouvee" : "pas trouvee", bike_lift_active(), g_lift_t,
                         g_mount_t, mem_bike_searching());
            /* remonter : relâcher la touche, puis replacer la moto après le respawn */
            if (g_mount_t > 0.0f) {
                g_mount_t += dt;
                /* pas de gel de la moto (drapeau « désactivé » d'ODE) : le respawn fait pendant
                   le gel faisait exploser la physique (v0.19.2 à v0.20.0, jeu planté) */
                /* respawn quand il est assis depuis un moment (2,1 s), redemandé si le jeu refuse */
                if ((g_mount_t >= 2.1f && g_mount_t - dt < 2.1f) || (g_mount_t >= 3.35f && g_mount_t - dt < 3.35f) ||
                    (g_mount_t >= 4.6f && g_mount_t - dt < 4.6f)) {
                    log_line("moto : respawn demande a %.2f s de la montee (touche de respawn du profil%s), vitesse %.2f m/s",
                             g_mount_t, padinject_pad_seen() ? "" : ", mais aucune manette lue", bike->m_fSpeedometer);
                    if (g_cfg.pad_respawn)
                        press_respawn_key(1);
                    else
                        log_line("moto : respawn par la manette coupe (pad_respawn = 0), fais-le toi-meme");
                }
                if (g_respawn_key_down && (g_mount_t > 4.9f || (g_mount_t > 3.65f && g_mount_t < 4.55f) ||
                                           (g_mount_t > 2.4f && g_mount_t < 3.3f)))
                    press_respawn_key(0);
                if (!bike->m_iCrashed) {
                    bike_lift_freeze(0); /* respawn fait : la moto redevient libre avant qu'on la replace */
                    bike_nocollide_update(0);
                }
                if (!bike->m_iCrashed && bike_has_target()) {
                    /* quelques pas de suite : le jeu peut finir de placer la moto */
                    bike_move_to_target(body, g_mount_moves == 0 ? log_line : NULL);
                    if (!bike_has_target() || ++g_mount_moves >= 8) { /* (une seule correction, v0.40.4) */
                        bike_clear_target();
                        g_mount_t = 0.0f;
                        g_mount_moves = 0;
                    }
                } else if (g_mount_t > 5.9f) {
                    bike_lift_freeze(0);
                    bike_nocollide_update(0);
                    log_line("moto : pas de respawn apres 5 s (la croix de la manette n'a pas marche ?)");
                    snprintf(g_msg, sizeof(g_msg), "%s", tr("Respawn non fait : appuie sur ta touche de respawn"));
                    g_msg_t = now_seconds() + 4.0;
                    snprintf(g_msg, sizeof(g_msg), "%s", tr("Respawn pas fait : appuie toi-meme sur ta touche de respawn"));
                    g_msg_t = now_seconds();
                    if (g_respawn_key_down)
                        press_respawn_key(0);
                    bike_clear_target();
                    g_mount_t = 0.0f;
                    g_mount_moves = 0;
                }
            }
            if (!bike_lift_active())
                g_lift_stand = 0;
            /* v1.8.20 : aussi pendant qu'il la relève (avant : seulement une fois debout, à 1,95 s) :
               entre deux corrections à 100 Hz la physique la déplaçait, elle tremblait à l'écran */
            g_lift_hold_render = bike_lift_active() && !(g_dismounted && g_vol_tip == 2) &&
                                 bike->m_iCrashed && !(g_mount_t > 1.9f); /* jamais pendant le respawn */
            tm_sec(14);
            if ((g_drive_state != DRIVE_ACTIVE || g_walker.state != WALK_ON) && !(g_dismounted && (g_vol_tip == 2 || g_vol_tip == 3)) &&
                !g_lift_stand) {
                /* (pendant une descente, la moto reste tenue le temps que le pilote soit repris)
                   v0.40.55 : la moto posée sur sa « béquille » après F reste debout même si le
                   pilotage redémarre (pilote en mod) : elle tombait chez certains joueurs */
                if (bike_lift_active()) {
                    bike_lift_end();
                    log_line("moto : lachee (pilote plus a pied : pilotage %d, marche %d)", (int)g_drive_state, (int)g_walker.state);
                }
                if (g_gesture.on)
                    gesture_clear();
                g_bike_lock = 0;
            } else if (go_l && body && bike_lift_active() && g_lift_t > 1.9f && !g_mount_t && g_walker.state == WALK_ON) { /* v1.8.3 : pas au sol */
                /* deuxième appui : il monte, on demande le respawn au jeu (flèche du haut) */
                padinject_install_xinput(log_line); /* au cas où le plugin manette s'est chargé après nous */
                float sdx = g_walker.pos[0] - g_tele_pos[0], sdz = g_walker.pos[1] - g_tele_pos[2];
                if (g_lift_stand && sdx * sdx + sdz * sdz > 2.2f * 2.2f) {
                    log_line("moto : trop loin pour remonter (%.1f m)", sqrtf(sdx * sdx + sdz * sdz));
                } else if (g_lift_stand) {
                    /* moto tenue droite après une descente : on prépare le geste de montée du côté
                       où se trouve le pilote */
                    float l = sqrtf(g_tele_R[2] * g_tele_R[2] + g_tele_R[8] * g_tele_R[8]), h;
                    if (l > 0.3f) {
                        h = atan2f(g_tele_R[2] / l, g_tele_R[8] / l);
                        memset(&g_gesture, 0, sizeof(g_gesture));
                        g_gesture.fwd[0] = sinf(h); g_gesture.fwd[2] = cosf(h);
                        g_gesture.right[0] = cosf(h); g_gesture.right[2] = -sinf(h);
                        g_gesture.side = (sdx * g_gesture.right[0] + sdz * g_gesture.right[2]) >= 0.0f ? 1.0f : -1.0f;
                        memcpy(g_gesture.pivot, g_tele_pos, sizeof(g_gesture.pivot));
                        g_gesture.on = 1;
                    }
                }
                if ((!g_lift_stand || g_gesture.on) && sdx * sdx + sdz * sdz <= (g_lift_stand ? 2.2f * 2.2f : 1e9f) &&
                    bike_capture_target(body)) {
                    log_line("moto : le pilote enjambe la moto");
                    g_gesture.mount = 1;
                    g_gesture.t = 0.0f;
                    g_gesture.from[0] = g_walker.pos[0];
                    g_gesture.from[1] = g_walker.pos[1];
                    g_gesture.from_heading = g_walker.heading;
                    g_bike_lock = 1;
                    g_mount_t = 0.001f;
                }
            } else if (e && !e_prev && !body && !bike_lift_active() && !g_mount_t) {
                log_line("moto : bouton releve appuye mais moto pas encore trouvee (%s)",
                         mem_bike_searching() ? "recherche en cours" : "nouvel essai bientot");
            } else if (e && !e_prev && body && !bike_lift_active() && !g_body_searched) {
                log_line("moto : encore en recherche, reessaie dans une seconde");
            } else if (e && !e_prev && body && !bike_lift_active()) {
                float cpos[3], dx, dz;
                if (safe_read((const void *)(body + 0xB8), cpos, sizeof(cpos))) {
                    dx = cpos[0] - g_rd.pos[RD_PELVIS][0];
                    dz = cpos[2] - g_rd.pos[RD_PELVIS][2];
                    if (dx * dx + dz * dz < 2.2f * 2.2f) {
                        float ex[3], gy, cg, *exp = NULL;
                        void *sk = drive_skeleton();
                        if (sk && skelscan_model_pos(sk, ex))
                            exp = ex;
                        float pitch = 0.0f;
                        {
                            /* pente sous les roues, dans l'axe de la moto (roues à ~0,7 m devant et
                               derrière le centre de gravité) */
                            float R9[9], cp9[3], ffx, ffz, ll, gf, gr;
                            if (chassis_frame(body, cp9, R9)) {
                                ffx = R9[2];
                                ffz = R9[8];
                                ll = sqrtf(ffx * ffx + ffz * ffz);
                                if (ll > 0.2f && ground_fn(cpos[0] + ffx / ll * 0.7f, cpos[2] + ffz / ll * 0.7f, &gf) &&
                                    ground_fn(cpos[0] - ffx / ll * 0.7f, cpos[2] - ffz / ll * 0.7f, &gr))
                                    pitch = atanf((gf - gr) / 1.4f);
                            }
                            if (pitch > 0.6f)
                                pitch = 0.6f;
                            if (pitch < -0.6f)
                                pitch = -0.6f;
                        }
                        /* +5 cm : roues juste au-dessus du sol (v0.40.5) ; dans le sol, la roue arrière qui tourne
                           (moteur au ralenti) creusait d'énormes trous et bloquait le respawn */
                        cg = ground_fn(cpos[0], cpos[2], &gy) ? gy + 0.67f / cosf(pitch) : cpos[1] + 0.45f;
                        if (body_matches(body) && bike_lift_begin(body, cg, pitch, exp, log_line)) {
                            g_lift_t = 0.0f;
                            g_bike_lock = 1;
                            gesture_lift_begin(body);

                        }
                    } else {
                        log_line("moto : trop loin pour la relever (%.1f m)", sqrtf(dx * dx + dz * dz));
                    }
                }
            }
            /* pendant la bascule de la descente, on ne tient pas la moto (sinon elle ne penche
               pas et le jeu ne passe pas en chute) ; elle est remise droite dès la chute */
            {
                /* moto tenue debout, pilote pas encore dessus : pièces endormies dans la physique
                   (moteur en prise, la roue arrière patinait et creusait la piste) ; réveillées
                   dès qu'il monte, bien avant le respawn (respawn pendant le sommeil = physique
                   cassée, v0.19) */
                static int frozen = 0;
                /* v0.40.9 : dès qu'on la redresse (sa roue arrière qui tourne creusait un éventail de
                   sillons) et pendant la montée, jusqu'à 0,2 s avant le respawn */
                /* v0.40.11 : coupé. Sortie de pause = physique qui explose (moto éjectée à 118 km/h
                   au respawn) ; les roues sont arrêtées à chaque image à la place (calm) */
                /* (pas pour la moto tenue après une descente : elle est sur ses roues, et le pilote
                   du jeu peut y être encore accroché — explosion chez un joueur, v0.40.18) */
                /* v0.41.13 : moto bloquée (sans gravité, sans collisions, suspensions figées) jusqu'à
                   l'image du respawn (2,1 s) : lâchée à 1,9 s, elle tombait sur ses suspensions et
                   creusait pendant 0,2 s, surtout en pente */
                /* v0.41.24 : bloquée jusqu'au respawn réel (le jeu le fait 0,2 à 0,3 s après
                   l'appui, ou au 2e appui à 3,35 s s'il refuse le 1er) : lâchée à 2,09 s, elle
                   retombait sur ses fourches et creusait pendant ce temps (log du joueur : 1,3 s
                   libre quand le 1er respawn est refusé). Lâchée de toute façon après 5,2 s. */
                /* v1.1.3 : moto posée debout après F : bloquée aussi pendant la montée (jusqu'au
                   respawn). Sans ça, ses suspensions bougeaient et elle creusait en remontant
                   (journal : ni « tenue sans gravité » ni « pièces bloquées » après F) */
                int want = bike_lift_active() && (!g_lift_stand || g_mount_t > 0.0f) && !(g_dismounted && g_vol_tip == 2) &&
                           (g_mount_t < 2.09f || (bike->m_iCrashed && g_mount_t < 5.2f));
                bike_lift_keep_wheels(!g_lift_stand);
                /* (v0.40.13 : « en pause » veut dire maintenant « sans gravité », voir bike.c) */
                if (want != frozen) {
                    bike_lift_freeze(want);
                    frozen = want;
                    log_line("moto : %s", want ? "tenue sans gravite (les roues ne retombent pas)" : "gravite remise");
                    bike_nocollide_update(0);
                }
            }
            if (bike_lift_active() && !(g_dismounted && g_vol_tip == 2)) {
                float cpos[3];
                g_lift_t += dt;
                /* 0,35 s pour s'accroupir et saisir le guidon, puis 1,3 s pour relever */
                {
                    float lp = (g_lift_t - 0.60f) / 1.3f; /* v0.40.67 : la moto monte quand les mains la tiennent */
                    bike_lift_apply(lp < 0.0f ? 0.0f : lp);
                    g_lift_lp = lp;
                    g_lift_wall = now_seconds();
                    if (!g_dismounted)
                        gesture_update(body, dt, lp < 0.0f ? 0.0f : lp);
                }
                if (g_lift_t > 2.4f && !g_gesture.mount)
                    g_bike_lock = 0; /* debout et calé aux poignées : le pilote peut bouger, la moto reste tenue */
                /* le pilote s'éloigne : il lâche la moto */
                if (g_lift_t > 1.9f && !g_lift_stand && safe_read((const void *)(body + 0xB8), cpos, sizeof(cpos))) {
                    float dx = cpos[0] - g_rd.pos[RD_PELVIS][0], dz = cpos[2] - g_rd.pos[RD_PELVIS][2];
                    if (dx * dx + dz * dz > 2.5f * 2.5f) {
                        bike_lift_end();
                        gesture_clear();
                        log_line("moto : lachee");
                    }
                }
            }
            /* v1.8.113 : près de la moto, à pied : ce qu'on peut faire et avec quelles touches */
            {
                int pr = 0;
                if (g_drive_state == DRIVE_ACTIVE && g_walker.state == WALK_ON && g_cfg.bike_lift && g_event_allowed &&
                    !(g_dismounted && g_vol_tip == 2)) {
                    float bp[3] = { g_last_bike_pos[0], g_last_bike_pos[1], g_last_bike_pos[2] }, dx, dz;
                    if (body)
                        safe_read((const void *)(body + 0xB8), bp, sizeof(bp));
                    dx = bp[0] - g_rd.pos[RD_PELVIS][0];
                    dz = bp[2] - g_rd.pos[RD_PELVIS][2];
                    if (dx * dx + dz * dz < 2.3f * 2.3f) {
                        if (!bike_lift_active() && bike->m_iCrashed)
                            pr = 1;
                        else if (bike_lift_active() && g_lift_t > 1.9f && g_mount_t <= 0.0f && !g_gesture.mount)
                            pr = 2;
                    }
                }
                g_prompt = pr;
            }
            e_prev = e;
        }
        tm_sec(15);
        {
            static float stat_t = 0.0f;
            stat_t += dt;
            if (stat_t >= 10.0f) {
                stat_t = 0.0f;
                if (g_cam_stat_frames > 0)
                    log_line("camera : %d passes, %d deja avec notre camera, %ld remplacements dans le calcul du jeu",
                             g_cam_stat_frames, g_cam_stat_used, g_camhook_writes);
                g_camhook_writes = 0;
                g_cam_stat_frames = g_cam_stat_used = g_cam_stat_writes = g_cam_stat_bad = 0;
            }
        }
    }

    tm_sec(10);
    /* ragdoll 3D : attente du squelette, sinon simulation classique */
    if (g_drive_state == DRIVE_REQUESTED) {
        g_drive_wait += dt;
        if (g_drive_wait > 0.5f) {
            log_line("ragdoll 3D : squelette du pilote introuvable pres de la moto, "
                     "simulation sans pilote 3D");
            skelscan_log_riders(g_drive_bike, log_line);
            g_drive_state = DRIVE_IDLE;
            rd_spawn(&g_rd, g_drive_bike, bike->m_aafRot, g_drive_vel);
        }
    }

    if (g_drive_state == DRIVE_ACTIVE) {
        if (g_walk_logged == 1) {
            g_walk_logged = 2;
            log_line("marche : le pilote se releve apres %.1f s", g_rd.time);
        }
        g_drive_log_timer += dt;
        if (g_drive_log_timer >= 0.5f && g_walker.state == WALK_OFF) {
            g_drive_log_timer = 0.0f;
            const float *n = g_rd.ground_n, *pv = g_rd.pos[RD_PELVIS];
            float floor_y = n[1] > 0.1f ? (g_rd.ground_y - n[0] * pv[0] - n[2] * pv[2]) / n[1] : 0.0f;
            log_line("  %.1f s : bassin (%.2f %.2f %.2f), sol sous le bassin %.2f, pente %.0f deg, "
                     "corps du jeu y %.2f, moto (%.2f %.2f %.2f), vitesse max %.2f m/s%s",
                     g_rd.time, pv[0], pv[1], pv[2], floor_y, acosf(n[1] < 1.0f ? n[1] : 1.0f) * 57.3f,
                     drive_model_y(), bike->m_fPosX, bike->m_fPosY, bike->m_fPosZ, rd_max_speed(&g_rd),
                     g_rd.ground_fn && g_terrain_ok ? ", relief par point" : ", plan unique");
            {
                float bp[3];
                void *sk = drive_skeleton();
                if (sk && skelscan_model_pos(sk, bp))
                    log_line("      corps du jeu (%.2f %.2f %.2f), ecart au bassin %.1f m", bp[0], bp[1], bp[2],
                             sqrtf((bp[0] - pv[0]) * (bp[0] - pv[0]) + (bp[2] - pv[2]) * (bp[2] - pv[2])));
            }
        }
    }

    if (g_rd.active) {
        if (g_drive_state == DRIVE_IDLE)
            rd_step(&g_rd, dt); /* en mode 3D, c'est le rappel de rendu qui avance la simulation */
        update_write_test();
        if (g_next_scan < NUM_SCANS && g_rd.time >= k_scan_times[g_next_scan]) {
            char label[48];
            snprintf(label, sizeof(label), "%d/%d (%.2f s apres la chute)", g_next_scan + 1,
                     NUM_SCANS, g_rd.time);
            skelscan_scan(g_last_bike_pos, g_cfg.scan_radius, 0, label);
            /* hiérarchie complète : au début et à la fin des 2 premières chutes */
            if ((g_next_scan == 0 || g_next_scan == NUM_SCANS - 1) && g_hier_dumps < 4) {
                skelscan_dump_rider();
                ++g_hier_dumps;
            }
            ++g_next_scan;
        }
        if (g_rd.resting && !g_rest_logged && g_walker.state == WALK_OFF) {
            g_rest_logged = 1;
            log_line("au repos apres %.1f s : impact max %.1f m/s, impact tete %.1f m/s%s",
                     g_rd.time, g_rd.max_impact, g_rd.head_impact,
                     g_rd.head_impact > 6.0f ? " (K.O. probable)" : "");
        }
    } else if (g_hide_timer > 0.0f) {
        g_hide_timer -= dt;
    }
}

/* ---------- dessin ---------- */

MXB_EXPORT int DrawInit(int *_piNumSprites, char **_pszSpriteName, int *_piNumFonts,
                        char **_pszFontName)
{
    FILE *f;
    if (_piNumSprites)
        *_piNumSprites = 0;
    if (_pszSpriteName)
        *_pszSpriteName = NULL;
    if (_piNumFonts)
        *_piNumFonts = 0;
    if (_pszFontName)
        *_pszFontName = NULL;

    /* La police est facultative : sans elle, la fenêtre s'affiche sans texte */
    {
        const char *fname = FONT_FILE;
        f = fopen(FONT_PATH, "rb");
        if (!f && (f = fopen(OLD_FONT_PATH, "rb")) != NULL)
            fname = OLD_FONT_FILE; /* ancien nom */
        if (!f && uipatch_font(FONT_PATH)) { /* v1.6.1 : police prise dans misc.pkz du jeu */
            log_line("police %s : copiee depuis misc.pkz du jeu", FONT_FILE);
            f = fopen(FONT_PATH, "rb");
        }
    if (f && _piNumFonts && _pszFontName) {
        fclose(f);
        memset(g_font_names, 0, sizeof(g_font_names));
        strcpy(g_font_names, fname);
        *_piNumFonts = 1;
        *_pszFontName = g_font_names;
        g_font_loaded = 1;
        if (padicon_load_font(FONT_PATH)) /* v1.8.109 : largeur des lettres (symboles au milieu du texte) */
            log_line("police : largeurs des caracteres lues (symboles de la manette)");
    } else if (f) {
        fclose(f);
    }
    }
    log_line("police %s : %s", g_font_loaded ? g_font_names : FONT_FILE, g_font_loaded ? "chargee" : "absente (pas de texte)");
    return 0;
}

static void set_quad(SPluginQuad_t *q, float x0, float y0, float x1, float y1, unsigned long color)
{
    /* rectangle aligné : haut gauche, bas gauche, bas droite, haut droite */
    q->m_aafPos[0][0] = x0; q->m_aafPos[0][1] = y0;
    q->m_aafPos[1][0] = x0; q->m_aafPos[1][1] = y1;
    q->m_aafPos[2][0] = x1; q->m_aafPos[2][1] = y1;
    q->m_aafPos[3][0] = x1; q->m_aafPos[3][1] = y0;
    q->m_iSprite = 0;
    q->m_ulColor = color;
}

/* Trait épais entre deux points écran, épaisseur constante en pixels */
static void set_line(SPluginQuad_t *q, float ax, float ay, float bx, float by,
                     float thickness, unsigned long color)
{
    const float aspect = g_cfg.screen_aspect;
    float dx = (bx - ax) * aspect, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    float nx, ny;
    if (len < 1e-6f) {
        nx = 0.0f;
        ny = thickness;
    } else {
        nx = -dy / len * thickness;
        ny = dx / len * thickness;
    }
    nx /= aspect; /* retour en coordonnées écran 0..1 */
    q->m_aafPos[0][0] = ax + nx; q->m_aafPos[0][1] = ay + ny;
    q->m_aafPos[1][0] = ax - nx; q->m_aafPos[1][1] = ay - ny;
    q->m_aafPos[2][0] = bx - nx; q->m_aafPos[2][1] = by - ny;
    q->m_aafPos[3][0] = bx + nx; q->m_aafPos[3][1] = by + ny;
    q->m_iSprite = 0;
    q->m_ulColor = color;
}

static void set_string(SPluginString_t *s, const char *text, float x, float y, float size,
                       unsigned long color)
{
    snprintf(s->m_szString, sizeof(s->m_szString), "%s", text);
    s->m_afPos[0] = x;
    s->m_afPos[1] = y;
    s->m_iFont = 1;
    s->m_fSize = size;
    s->m_iJustify = 0;
    s->m_ulColor = color;
}

/* Projection d'un point du monde dans la fenêtre (vue de côté) */
static void project(const float p[3], float *sx, float *sy)
{
    const PluginConfig *c = &g_cfg;
    float along = p[0] * g_view_dir[0] + p[2] * g_view_dir[1] - g_view_center;
    float height = p[1] - g_rd.ground_y;
    float per_meter_x = c->window_w / c->window_scale;
    float per_meter_y = per_meter_x * c->screen_aspect;
    *sx = c->window_x + c->window_w * 0.5f + along * per_meter_x;
    *sy = c->window_y + c->window_h * 0.85f - height * per_meter_y;
}

static int inside_window(float x, float y)
{
    const PluginConfig *c = &g_cfg;
    return x >= c->window_x && x <= c->window_x + c->window_w &&
           y >= c->window_y && y <= c->window_y + c->window_h;
}

/* Couleurs au format ABGR */
#define COL_BACKGROUND 0xA0000000UL
#define COL_GROUND     0xFF2D5A8BUL
#define COL_BONE       0xFFFFFFFFUL
#define COL_HEAD       0xFF2080FFUL
#define COL_TEXT       0xFFFFFFFFUL
#define COL_TEXT_ALERT 0xFF3030FFUL

static void draw_impl(int _iState, int *_piNumQuads, void **_ppQuad, int *_piNumString, void **_ppString);
MXB_EXPORT void Draw(int _iState, int *_piNumQuads, void **_ppQuad, int *_piNumString,
                     void **_ppString)
{
    double t0 = tm_now_us();
    draw_impl(_iState, _piNumQuads, _ppQuad, _piNumString, _ppString);
    tm_add(1, t0);
    {
        /* v1.8.116 : image lente (plus de 50 ms depuis la précédente, alors que d'habitude moins de 25) :
           dans le journal, avec le temps passé dans le plugin pendant ce temps-là */
        static double last = 0.0, avg = 0.0;
        static int logged = 0;
        double now = tm_now_us(), gap = last > 0.0 ? now - last : 0.0, dr = now - t0;
        if (gap > 50000.0 && avg > 0.0 && avg < 25000.0 && logged < 40) {
            ++logged;
            log_line("image lente : %.0f ms (plugin : telemetrie %.1f ms (appel le plus long %.1f), squelette %.1f ms, dessin %.1f ms)",
                     gap / 1000.0, g_fr_tele / 1000.0, g_fr_tele_max / 1000.0, g_fr_skel / 1000.0, dr / 1000.0);
        }
        if (gap > 0.0 && gap < 200000.0)
            avg = avg > 0.0 ? avg * 0.95 + gap * 0.05 : gap;
        last = now;
        g_fr_tele = g_fr_skel = g_fr_tele_max = 0.0;
        log_flush_tick();
    }
}

/* ---------- v1.8.110 : tutoriel (fenêtre au premier lancement ; menu P « Afficher le tutoriel ») ---------- */
static int g_tuto_page = 0;

/* « touches / bouton » d'une action : texte des touches, puis bouton de la manette (symbole PlayStation) */
static void tuto_keys(char *out, size_t n, const int k[2])
{
    if (k[0] > 0 && k[1] > 0)
        snprintf(out, n, "%s %s", cfg_key_name(k[0]), cfg_key_name(k[1]));
    else
        snprintf(out, n, "%s", k[0] > 0 ? cfg_key_name(k[0]) : "-");
}

static void tuto_line(int *nq, int *ns, float x, float y, const char *label, const int *keys, int pad, float asp)
{
    char t[100], ks[40];
    const float sz = 0.019f;
    if (*ns >= MAX_STRINGS)
        return;
    if (keys) {
        tuto_keys(ks, sizeof(ks), keys);
        snprintf(t, sizeof(t), "%s  %s%s", label, ks, pad ? "   /   " : "");
    } else
        snprintf(t, sizeof(t), "%s", label);
    set_string(&g_strings[*ns], t, x, y, sz, 0xFFFFFFFF);
    g_strings[(*ns)++].m_iJustify = 0;
    if (pad)
        padicon_text(g_quads, MAX_QUADS, nq, g_strings, MAX_STRINGS, ns, cfg_pad_name(pad),
                     x + padicon_text_width(t, sz, asp), y, sz, 0xFFFFFFFF, 0, asp);
}

/* v1.8.175 : mise à jour téléchargée : le joueur choisit (Entrée / A : installer, Retour / B : plus tard) */
static void upd_draw(int *nq, int *ns, int on_track)
{
    static int prev = 0x3;
    static double since = -1.0;
    const PluginConfig *c = &g_cfg;
    float asp = c->screen_aspect > 0.1f ? c->screen_aspect : 1.7778f;
    float x0 = 0.5f - 0.30f / asp * 1.7778f * 0.5f * 1.6f, x1 = 1.0f - x0, y0 = 0.34f, y1 = 0.56f;
    char ver[32], t[160];
    int cur = 0, edge;
    if (!update_pending(ver, sizeof(ver)) || !on_track || !g_font_loaded || g_tuto_open || equip_menu_open() ||
        cone_menu_busy() || (g_drive_state != DRIVE_ACTIVE && g_ride_speed > 3.0f)) {
        g_upd_open = 0;
        since = -1.0;
        prev = 0x3;
        return;
    }
    if (since < 0.0)
        since = now_seconds();
    if (now_seconds() - since < 1.0)
        return;
    if (!g_upd_open)
        log_line("mise a jour : fenetre ouverte (%s)", ver);
    g_upd_open = 1;
    if (game_focused()) {
        PadState ps;
        int have = padinject_read(&ps);
        if (key_down(VK_RETURN) || (have && (ps.buttons & 0x1000)))
            cur |= 1;
        if (key_down(VK_BACK) || (have && (ps.buttons & 0x2000)))
            cur |= 2;
    }
    edge = cur & ~prev;
    prev = cur;
    if (edge & 3) {
        update_answer((edge & 1) ? 1 : 0);
        g_upd_open = 0;
        return;
    }
    if (*nq + 2 > MAX_QUADS || *ns + 4 > MAX_STRINGS)
        return;
    set_quad(&g_quads[(*nq)++], x0, y0, x1, y1, 0xE8101418UL);
    set_quad(&g_quads[(*nq)++], x0, y0, x1, y0 + 0.05f, 0xFF0078D0UL);
    set_string(&g_strings[*ns], tr("Mise a jour disponible"), 0.5f, y0 + 0.011f, 0.026f, 0xFFFFFFFF);
    g_strings[(*ns)++].m_iJustify = 1;
    snprintf(t, sizeof(t), tr("MXRP Ragdoll %s est disponible (tu as la %s)."), ver, PLUGIN_VERSION);
    set_string(&g_strings[*ns], t, 0.5f, y0 + 0.075f, 0.022f, 0xFFFFFFFF);
    g_strings[(*ns)++].m_iJustify = 1;
    set_string(&g_strings[*ns], tr("Elle sera active au prochain lancement du jeu."), 0.5f, y0 + 0.112f, 0.020f, 0xFFC0C0C0);
    g_strings[(*ns)++].m_iJustify = 1;
    padicon_text(g_quads, MAX_QUADS, nq, g_strings, MAX_STRINGS, ns, tr("Entree / A : installer   -   Retour / B : plus tard"),
                 0.5f, y1 - 0.05f, 0.019f, 0xFFFFFFFF, 1, asp);
}

/* v1.8.204 : multi changé (question ou menu P) : appliqué tout de suite à la session en cours */
static void online_apply(void)
{
    static int last = -1;
    uiswitch_set_online(g_cfg.online_ok);
    if (last == g_cfg.online_ok)
        return;
    if (last >= 0) {
        g_srv_ragdoll = g_srv_tagged && g_cfg.online_ok;
        uiswitch_force(g_srv_ragdoll);
        uiswitch_pause(g_session_online && !g_cfg.online_ok);
        if (g_srv_ragdoll && !uiswitch_button_on())
            g_cfg.enabled = 1;
        net_game_channel(g_srv_ragdoll && g_cfg.net_on);
        if (!g_cfg.online_ok && net_running())
            net_stop();
        else if (g_cfg.online_ok && g_cfg.net_on && g_mode3d && g_visual_allowed && !net_running())
            net_start(g_cfg.net_host, g_cfg.net_ip, g_cfg.net_port, log_line);
        log_line("multi : %s par le joueur", g_cfg.online_ok ? "ACTIVE" : "COUPE");
    }
    last = g_cfg.online_ok;
}

/* v1.8.204 : au premier lancement, on demande si le joueur veut le multi (coupé tant qu'il n'a pas dit oui) */
static void mlt_draw(int *nq, int *ns, int on_track)
{
    static int prev = 0x3;
    static double since = -1.0;
    const PluginConfig *c = &g_cfg;
    float asp = c->screen_aspect > 0.1f ? c->screen_aspect : 1.7778f;
    float x0 = 0.5f - 0.30f / asp * 1.7778f * 0.5f * 1.6f, x1 = 1.0f - x0, y0 = 0.34f, y1 = 0.56f;
    int cur = 0, edge;
    if (c->online_asked || !on_track || !g_font_loaded || g_tuto_open || equip_menu_open() || cone_menu_busy() ||
        (g_upd_open && !g_mlt_open) || (g_drive_state != DRIVE_ACTIVE && g_ride_speed > 3.0f)) {
        if (g_mlt_open)
            g_upd_open = 0;
        g_mlt_open = 0;
        since = -1.0;
        prev = 0x3;
        return;
    }
    if (since < 0.0)
        since = now_seconds();
    if (now_seconds() - since < 1.0)
        return;
    if (!g_mlt_open)
        log_line("multi : question ouverte");
    g_mlt_open = 1;
    g_upd_open = 1; /* (mêmes touches bloquées que la fenêtre de mise à jour) */
    if (game_focused()) {
        PadState ps;
        int have = padinject_read(&ps);
        if (key_down(VK_RETURN) || (have && (ps.buttons & 0x1000)))
            cur |= 1;
        if (key_down(VK_BACK) || (have && (ps.buttons & 0x2000)))
            cur |= 2;
    }
    edge = cur & ~prev;
    prev = cur;
    if (edge & 3) {
        g_cfg.online_ok = (edge & 1) ? 1 : 0;
        g_cfg.online_asked = 1;
        equip_save_options();
        log_line("multi : reponse du joueur : %s", g_cfg.online_ok ? "oui" : "non");
        g_mlt_open = 0;
        g_upd_open = 0;
        return;
    }
    if (*nq + 2 > MAX_QUADS || *ns + 4 > MAX_STRINGS)
        return;
    set_quad(&g_quads[(*nq)++], x0, y0, x1, y1, 0xE8101418UL);
    set_quad(&g_quads[(*nq)++], x0, y0, x1, y0 + 0.05f, 0xFF0078D0UL);
    set_string(&g_strings[*ns], tr("Activer le multi ?"), 0.5f, y0 + 0.011f, 0.026f, 0xFFFFFFFF);
    g_strings[(*ns)++].m_iJustify = 1;
    set_string(&g_strings[*ns], tr("En ligne, le plugin ne marche qu'avec le multi : sur les serveurs RAGDOLL, ton pilote,"), 0.5f, y0 + 0.075f,
               0.020f, 0xFFFFFFFF);
    g_strings[(*ns)++].m_iJustify = 1;
    set_string(&g_strings[*ns], tr("tes chutes et tes objets sont partages avec les autres joueurs du plugin."), 0.5f, y0 + 0.108f, 0.020f, 0xFFFFFFFF);
    g_strings[(*ns)++].m_iJustify = 1;
    padicon_text(g_quads, MAX_QUADS, nq, g_strings, MAX_STRINGS, ns,
                 tr("Entree / A : activer   -   Retour / B : non (menu P pour changer)"), 0.5f, y1 - 0.05f, 0.019f, 0xFFFFFFFF, 1,
                 asp);
}

static void tuto_draw(int *nq, int *ns, int on_track)
{
    static int prev = 0, ready = 0;
    static double since = 0.0;
    const PluginConfig *c = &g_cfg;
    float asp = c->screen_aspect > 0.1f ? c->screen_aspect : 1.7778f;
    float x0 = 0.5f - 0.36f / asp * 1.7778f * 0.5f * 1.6f, x1 = 1.0f - x0, y0 = 0.22f, y1 = 0.70f, lx, y, dy = 0.034f;
    int cur = 0, edge;
    const int npages = 3;
    if (!c->tutorial || !on_track || !g_font_loaded || equip_menu_open() || cone_menu_busy() ||
        (g_drive_state != DRIVE_ACTIVE && g_ride_speed > 3.0f)) {
        g_tuto_open = 0;
        ready = 0;
        if (!c->tutorial)
            g_tuto_page = 0;
        return;
    }
    if (equip_tutorial_requested()) {
        /* v1.8.111 : demandé dans le menu P : tout de suite, à la première page */
        ready = 1;
        since = now_seconds() - 1.0;
        prev = 0x7;
        g_tuto_page = 0;
    }
    if (!ready) {
        /* ouverte 1 s après l'arrivée en piste ; touches déjà enfoncées ignorées */
        ready = 1;
        since = now_seconds();
        prev = 0x7;
    }
    if (now_seconds() - since < 1.0)
        return;
    if (!g_tuto_open)
        log_line("tutoriel : ouvert");
    g_tuto_open = 1;
    /* entrées : Entrée / A = suivant, Retour arrière / B = fermer */
    if (game_focused()) {
        PadState ps;
        int have = padinject_read(&ps);
        if (key_down(VK_RETURN) || (have && (ps.buttons & 0x1000)))
            cur |= 1;
        if (key_down(VK_BACK) || (have && (ps.buttons & 0x2000)))
            cur |= 2;
    }
    edge = cur & ~prev;
    prev = cur;
    if ((edge & 2) || ((edge & 1) && g_tuto_page >= npages - 1)) {
        g_cfg.tutorial = 0;
        g_tuto_page = 0;
        g_tuto_open = 0;
        equip_save_options();
        log_line("tutoriel : ferme (revoir : menu P, Afficher le tutoriel)");
        return;
    }
    if (edge & 1)
        ++g_tuto_page;
    if (*nq + 3 > MAX_QUADS)
        return;
    set_quad(&g_quads[(*nq)++], x0, y0, x1, y1, 0xE8101418UL);
    set_quad(&g_quads[(*nq)++], x0, y0, x1, y0 + 0.05f, 0xFF0078D0UL);
    {
        /* points des pages */
        int pg;
        for (pg = 0; pg < npages && *nq < MAX_QUADS; ++pg) {
            float px = 0.5f + (pg - (npages - 1) * 0.5f) * 0.02f, d = pg == g_tuto_page ? 0.006f : 0.004f;
            set_quad(&g_quads[(*nq)++], px - d / asp, y1 - 0.075f - d, px + d / asp, y1 - 0.075f + d,
                     pg == g_tuto_page ? 0xFF0080FFUL : 0xC0A0A0A0UL);
        }
    }
    lx = x0 + 0.02f;
    y = y0 + 0.075f;
    if (*ns + 2 > MAX_STRINGS)
        return;
    {
        static const char *k_title[3] = { "Bienvenue dans MXRP Ragdoll", "A pied", "Objets et reglages" };
        set_string(&g_strings[*ns], tr(k_title[g_tuto_page]), 0.5f, y0 + 0.011f, 0.026f, 0xFFFFFFFF);
        g_strings[(*ns)++].m_iJustify = 1;
    }
    if (g_tuto_page == 0) {
        tuto_line(nq, ns, lx, y, tr("Quand tu chutes, ton pilote tombe vraiment et roule sur la piste."), NULL, 0, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Ensuite il se releve, marche jusqu'a sa moto, la releve et remonte."), NULL, 0, asp);
        tuto_line(nq, ns, lx, y += dy * 1.6f, tr("Se relever (garder appuye) :"), c->key_getup, c->pad_getup, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Respawn sur la piste : garde la touche de respawn du jeu."), NULL, 0, asp);
    } else if (g_tuto_page == 1) {
        char t[100], a[20], b[20], cc[20], d[20];
        int k1[2] = { c->key_forward[0], 0 }, k2[2] = { c->key_left[0], 0 }, k3[2] = { c->key_back[0], 0 }, k4[2] = { c->key_right[0], 0 };
        tuto_keys(a, sizeof(a), k1);
        tuto_keys(b, sizeof(b), k2);
        tuto_keys(cc, sizeof(cc), k3);
        tuto_keys(d, sizeof(d), k4);
        snprintf(t, sizeof(t), "%s  %s %s %s %s   /   %s", tr("Marcher :"), a, b, cc, d, tr("stick gauche"));
        tuto_line(nq, ns, lx, y, t, NULL, 0, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Courir :"), c->key_run, c->pad_run, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Sauter :"), c->key_jump, c->pad_jump, asp);
        tuto_line(nq, ns, lx, y += dy, tr("S'accroupir :"), c->key_crouch, c->pad_crouch, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Vue 1re / 3e personne :"), c->key_view, c->pad_view, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Relever la moto (a cote) :"), c->key_bike, c->pad_bike, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Monter : appuie encore (manette : garde appuye)."), NULL, 0, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Descendre (moto arretee, hors ligne) :"), c->key_dismount, c->pad_dismount, asp);
    } else {
        tuto_line(nq, ns, lx, y, tr("Roue des objets (a pied) : B   -   manette : croix bas"), NULL, 0, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Plots et bottes de foin : poser, ramasser, porter, pousser."), NULL, 0, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Menu P : touches, langue, manette, pilote et equipement."), NULL, 0, asp);
        tuto_line(nq, ns, lx, y += dy, tr("En ligne : serveurs avec RAGDOLL dans le nom."), NULL, 0, asp);
        tuto_line(nq, ns, lx, y += dy, tr("Serveurs officiels : sors MXRP_Ragdoll.dlo du dossier plugins avant."), NULL, 0, asp);
        tuto_line(nq, ns, lx, y += dy * 1.4f, tr("Revoir ce tutoriel : menu P, Afficher le tutoriel."), NULL, 0, asp);
    }
    padicon_text(g_quads, MAX_QUADS, nq, g_strings, MAX_STRINGS, ns,
                 tr(g_tuto_page >= npages - 1 ? "Entree / A : fermer" : "Entree / A : suivant   -   Retour / B : fermer"),
                 0.5f, y1 - 0.05f, 0.017f, 0xFFC0C0C0, 1, asp);
}

static void draw_impl(int _iState, int *_piNumQuads, void **_ppQuad, int *_piNumString, void **_ppString)
{
    const PluginConfig *c = &g_cfg;
    int nq = 0, ns = 0, k;
    float center[3];
    char text[100];
    reset_binding_poll();
    lang_set(g_cfg.language);
    objects_poll(log_line); /* objets de la piste (pour marcher dessus) */
    if (g_draw_state == 2 && _iState == 2 && g_mode3d && g_visual_allowed)
        remote_record_live();

    *_piNumQuads = 0;
    *_ppQuad = NULL;
    *_piNumString = 0;
    *_ppString = NULL;

    if (_iState != g_draw_state) {
        log_line("affichage : %s", _iState == 2 ? "rediffusion" : _iState == 1 ? "spectateur" : "en piste");
        if (_iState == 2 && g_draw_state != 2 && g_drive_state == DRIVE_ACTIVE && !g_live_suspended) {
            /* rediffusion ouverte pendant que le plugin pilote (marche...) : on enregistre ce qui
               a été fait (pour le revoir tout de suite) et on rend le pilote au jeu ; la marche
               reprend là où elle était au retour en piste */
            g_live_skel = drive_skeleton();
            g_rd_saved = g_rd;
            drive_get_snap(&g_snap_saved);
            replay_rec_end(0);
            drive_restore();
            g_live_suspended = 1;
            equip_hide_helmet(0);
            equip_hide_cockpit(0);
            log_line("rediffusion : pilotage mis en attente (marche en cours enregistree)");
        }
        if (_iState == 2 && g_draw_state != 2) {
            /* pilotes des autres joueurs : rendus au jeu (et leur passage enregistré) */
            int i;
            /* passages en cours des autres joueurs : écrits tout de suite, pour les voir dans cette
               rediffusion ; la suite est enregistrée en direct pendant qu'on regarde */
            if (!rclock_live_time(&g_live_rc_open))
                g_live_rc_open = -1.0;
            g_live_wall_open = now_seconds();
            for (i = 0; i < NET_MAX_PEERS; ++i)
                remote_release(i, 1);
        }
        if (_iState == 2 && g_cfg.replay_play) {
            replay_load();
            rclock_reset();
        }
        if (g_draw_state == 2 && _iState != 2) {
            int i;
            replay_stop(NULL);
            for (i = 0; i < REPLAY_PLAYERS; ++i)
                replay_release(i, 1);
        }
        g_draw_state = _iState;
    }
    {
        /* v1.8.162 : champ de vision en 1re personne (moto et à pied) et zoom à pied */
        int foot = now_seconds() - g_foot_cam_t < 0.3;
        int fp = foot ? g_first_person : g_game_fp == 1;
        {
            /* v1.8.172 : mise à jour automatique faite */
            char uv[32];
            int um = update_take_message(uv, sizeof(uv));
            if (um) {
                snprintf(g_msg, sizeof(g_msg), tr(um == 1   ? "MXRP Ragdoll %s installe : actif au prochain lancement du jeu"
                                                  : um == 3 ? "Mise a jour %s : plus tard (redemandee au prochain lancement)"
                                                            : "MXRP Ragdoll %s disponible : l'installateur s'ouvrira quand tu quitteras le jeu"), uv);
                g_msg_t = now_seconds();
            }
        }
        float want = g_cfg.fov_fp;
        /* v1.8.163 : à pied en 1re personne : le même champ de vision qu'en roulant (le réglé, sinon celui
           que le jeu avait en roulant en 1re personne) */
        if (!foot && fp && now_seconds() - g_foot_cam_t > 1.0 && glcam_game_hfov() > 20.0f) {
            g_ride_fp_fov = glcam_game_hfov();
            glcam_note_ride_fov(g_ride_fp_fov);
        }
        if (foot && fp && want < 1.0f)
            want = g_ride_fp_fov;
        /* v1.8.165 : sur la moto, jamais de changement (le réglage du menu P ne vaut qu'à pied) ;
           v1.8.166 : à pied, donné au jeu dans sa caméra (cam_matmul_cb), plus dans la perspective */
        /* v1.8.167 : zoom 1re personne : champ de vision resserré (base : réglage, moto, sinon celui du jeu à pied) */
        /* v1.8.168 : passage d'un cran à l'autre en douceur (~0,25 s) */
        static float zcur = 1.0f;
        static double zt = 0.0;
        double znow = now_seconds(), zdt = znow - zt;
        zt = znow;
        if (zdt > 0.1)
            zdt = 0.1;
        if (!(foot && fp)) {
            g_fp_zoom = 0;
            zcur = 1.0f;
        } else
            zcur += (k_fp_zoom[g_fp_zoom % 5] - zcur) * (1.0f - expf(-(float)zdt / 0.08f));
        g_fp_zoom_cur = zcur;
        if (foot && fp && zcur > 1.002f) {
            float z = zcur;
            if (want < 1.0f) {
                float asp = glcam_aspect();
                want = 2.0f * atanf(tanf(37.5f * 0.01745329f) * (asp > 0.5f ? asp : 1.7778f) * 0.75f) * 57.29578f;
            }
            want = 2.0f * atanf(tanf(want * 0.5f * 0.01745329f) / z) * 57.29578f;
        }
        g_foot_fov_want = (_iState == 0 && c->enabled && foot && fp) ? want : 0.0f;
        glcam_set_fov(0.0f, 1.0f);
        {
            /* v1.8.164 : diagnostic, au changement (moto / à pied, 1re / 3e) puis toutes les 15 s */
            static int pst = -1;
            static double plt = 0.0;
            int st = foot * 2 + fp;
            if (_iState == 0 && (st != pst || now_seconds() - plt > 15.0)) {
                if (st != pst)
                    plt = now_seconds() - 14.0; /* (une image plus tard : les passes de la nouvelle vue) */
                else {
                    log_line("champ de vision : %s, %s, regle %.0f, vu en roulant en 1re %.1f", foot ? "a pied" : "sur la moto",
                             fp ? "1re personne" : "3e personne", g_cfg.fov_fp, g_ride_fp_fov);
                    glcam_fov_log(log_line);
                    plt = now_seconds();
                }
                pst = st;
            }
        }
    }
    if (_iState != 0)
        /* v1.8.161 : rediffusion et spectateur : aussi les rampes enregistrées et celles des autres */
        glcam_set_scene_active(g_cfg.cones_3d && (cones_hist_count() || cones_count() || cones_net_remote_count() ||
                                                  bales_hist_count() || bales_count() || bales_net_remote_count() ||
                                                  ramps_hist_count() || ramps_count() || ramps_net_remote_count()));
    if (!c->enabled || _iState != 0) {
        /* v1.8.204 : en ligne sans le multi (plugin en pause) : la question du multi s'affiche quand même */
        if (_iState == 0 && g_session_online && !g_cfg.online_ok) {
            nq = 0;
            ns = 0;
            mlt_draw(&nq, &ns, 1);
            online_apply();
            if (nq || ns) {
                *_piNumQuads = nq;
                *_ppQuad = g_quads;
                *_piNumString = ns;
                *_ppString = g_strings;
            }
        }
        return;
    }
    nq = 0;
    if (cones_count() && !glcam_scene_recent() && draw_rows_update()) {
        /* (secours : si le dessin 3D dans la scène ne s'est pas fait) */
        /* v1.8.45 : les cônes en premier (sous les flèches et les menus) */
        float eye[3] = { g_draw_rows[3], g_draw_rows[7], g_draw_rows[11] };
        g_cone_qbase = 0;
        nq = cones_draw(MAX_QUADS - 110, eye, project_draw, cone_put_quad, cone_ground);
    }
    {
        int ms = 0;
        nq += draw_bike_marker(g_quads + nq, MAX_QUADS - nq, &ms);
        ns = ms;
    }
    if (g_fade > 0.0f && nq < MAX_QUADS) {
        /* descente : on cache la bascule de la moto */
        unsigned long a = (unsigned long)(g_fade * 255.0f + 0.5f);
        set_quad(&g_quads[nq++], 0.0f, 0.0f, 1.0f, 1.0f, (a << 24));
    }
    {
        int ms = 0;
        nq += equip_menu_draw(g_quads + nq, MAX_QUADS - nq, g_strings + ns, MAX_STRINGS - ns, &ms, g_font_loaded,
                              c->screen_aspect);
        ns += ms;
        nq += cone_menu_draw(g_quads + nq, MAX_QUADS - nq, g_strings + ns, MAX_STRINGS - ns, &ms);
        ns += ms;
        tuto_draw(&nq, &ns, _iState == 0 && g_cfg.enabled); /* v1.8.110 */
        upd_draw(&nq, &ns, _iState == 0 && g_cfg.enabled); /* v1.8.175 */
        mlt_draw(&nq, &ns, _iState == 0 && g_cfg.enabled); /* v1.8.204 */
        online_apply();
    }
    {
        /* v0.40.80 : message centré ; pendant un appui long : barre de progression et
           secondes restantes (le message disparaît dès qu'on relâche) */
        double now = now_seconds();
        int prog_on = g_msg_prog >= 0.0f && now - g_msg_prog_t < 0.15;
        if (g_msg_prog >= 0.0f && !prog_on) {
            g_msg_prog = -1.0f;
            g_msg_t = 0.0; /* relâché : on efface */
        }
        noplug_update();
        autokick_update(); /* v1.7.36 */
        if (g_noplug[0] && g_font_loaded && ns < MAX_STRINGS) {
            /* v1.2 : en haut à gauche, petit, sans gêner */
            set_string(&g_strings[ns], g_noplug, 0.01f, 0.01f, 0.018f, 0xFF60C0FF);
            g_strings[ns++].m_iJustify = 0;
        }
        {
            int up = g_drive_state == DRIVE_ACTIVE && g_walker.state == WALK_OFF && g_rd.active ? g_prompt_up : 0;
            if (g_drive_state != DRIVE_ACTIVE || g_walker.state != WALK_OFF)
                g_prompt_up = 0;
        if ((g_prompt || up) && g_cfg.hold_msgs && g_font_loaded && !prog_on && !(now - g_msg_t < 4.0 && g_msg[0]) &&
            ns + 4 <= MAX_STRINGS && _iState == 0 && !g_tuto_open && !g_upd_open) {
            /* v1.8.115 : même taille que les messages à barre (0,025). v1.8.113 : à côté de la moto : « Relever la moto   E / [triangle] » ; v1.8.114 : au sol : se relever */
            char t[100], pn[48];
            float asp = c->screen_aspect > 0.1f ? c->screen_aspect : 1.7778f, w1, w2, x;
            const int *kk = up ? g_cfg.key_getup : g_cfg.key_bike;
            int pad = up ? g_cfg.pad_getup : g_cfg.pad_bike;
            unsigned long col = up == 1 ? 0xFFA0A0A0UL : 0xFFFFFFFFUL;
            const char *lbl = tr(up ? "Se relever (garder appuye) :" : g_prompt == 1 ? "Relever la moto" : "Monter sur la moto (manette : garder appuye)");
            snprintf(pn, sizeof(pn), "%s", pad ? pad_shown(pad) : "");
            snprintf(t, sizeof(t), "%s     %s%s", lbl, kk[0] > 0 ? cfg_key_name(kk[0]) : "", kk[0] > 0 && pn[0] ? " / " : "");
            w1 = padicon_text_width(t, 0.025f, asp);
            w2 = pn[0] ? padicon_text_width_rich(pn, 0.025f, asp) : 0.0f;
            x = 0.5f - (w1 + w2) * 0.5f;
            set_string(&g_strings[ns], t, x, 0.12f, 0.025f, col);
            g_strings[ns++].m_iJustify = 0;
            if (pn[0])
                padicon_text(g_quads, MAX_QUADS, &nq, g_strings, MAX_STRINGS, &ns, pn, x + w1, 0.12f, 0.025f, 0xFF60D0FF, 0, asp);
        }
        }
        if (now - g_msg_t < (prog_on ? 1.0 : 4.0) && g_msg[0] && g_font_loaded && ns < MAX_STRINGS) {
            if (prog_on && (g_msg_keys[0] || g_msg_pad) && ns + 4 <= MAX_STRINGS) {
                /* v1.8.112 : « Garde appuye pour te relever   E / [triangle] » (bouton : symbole PlayStation) */
                char t[100], pn[48];
                float asp = c->screen_aspect > 0.1f ? c->screen_aspect : 1.7778f, w1, w2, x;
                snprintf(pn, sizeof(pn), "%s", g_msg_pad ? pad_shown(g_msg_pad) : "");
                snprintf(t, sizeof(t), "%s     %s%s", g_msg, g_msg_keys, g_msg_keys[0] && pn[0] ? " / " : "");
                w1 = padicon_text_width(t, 0.025f, asp);
                w2 = pn[0] ? padicon_text_width_rich(pn, 0.025f, asp) : 0.0f;
                x = 0.5f - (w1 + w2) * 0.5f;
                set_string(&g_strings[ns], t, x, 0.12f, 0.025f, 0xFFFFFFFF);
                g_strings[ns++].m_iJustify = 0;
                if (pn[0])
                    padicon_text(g_quads, MAX_QUADS, &nq, g_strings, MAX_STRINGS, &ns, pn, x + w1, 0.12f, 0.025f,
                                 0xFF60D0FF, 0, asp);
            } else {
                set_string(&g_strings[ns], g_msg, 0.5f, 0.12f, 0.025f, 0xFFFFFFFF);
                g_strings[ns++].m_iJustify = 1;
            }
        }
        if (prog_on && nq + 2 <= MAX_QUADS) {
            float p = g_msg_prog > 1.0f ? 1.0f : g_msg_prog, x0 = 0.38f, x1 = 0.62f, y0 = 0.162f, y1 = 0.176f;
            set_quad(&g_quads[nq++], x0, y0, x1, y1, 0xA0000000UL);
            set_quad(&g_quads[nq++], x0 + 0.002f, y0 + 0.002f, x0 + 0.002f + (x1 - x0 - 0.004f) * p, y1 - 0.002f, 0xFF00A0FFUL);
            if (g_font_loaded && ns < MAX_STRINGS) {
                char cd[16];
                snprintf(cd, sizeof(cd), "%.1f s", g_msg_left);
                set_string(&g_strings[ns], cd, 0.5f, 0.182f, 0.022f, 0xFFFFFFFF);
                g_strings[ns++].m_iJustify = 1;
            }
        }
    }
    if (!c->debug_window) {
        *_piNumQuads = nq;
        *_ppQuad = g_quads;
        *_piNumString = ns;
        *_ppString = g_strings;
        return;
    }
    if (!g_rd.active && g_hide_timer <= 0.0f)
        return;

    /* la vue suit le centre du corps */
    if (g_rd.active) {
        rd_center(&g_rd, center);
        g_view_center = center[0] * g_view_dir[0] + center[2] * g_view_dir[1];
    }

    /* fond + sol */
    set_quad(&g_quads[nq++], c->window_x, c->window_y, c->window_x + c->window_w,
             c->window_y + c->window_h, COL_BACKGROUND);
    set_quad(&g_quads[nq++], c->window_x, c->window_y + c->window_h * 0.85f,
             c->window_x + c->window_w, c->window_y + c->window_h * 0.87f, COL_GROUND);

    /* os */
    for (k = 0; k < g_rd.num_constraints && nq < MAX_QUADS - 1; ++k) {
        const RdConstraint *bone = &g_rd.constraints[k];
        float ax, ay, bx, by;
        if (!bone->visible)
            continue;
        project(g_rd.pos[bone->a], &ax, &ay);
        project(g_rd.pos[bone->b], &bx, &by);
        if (!inside_window(ax, ay) && !inside_window(bx, by))
            continue;
        set_line(&g_quads[nq++], ax, ay, bx, by, 0.004f, COL_BONE);
    }

    /* tête */
    {
        float hx, hy, r = 0.010f;
        project(g_rd.pos[RD_HEAD], &hx, &hy);
        if (inside_window(hx, hy))
            set_quad(&g_quads[nq++], hx - r / c->screen_aspect, hy - r,
                     hx + r / c->screen_aspect, hy + r, COL_HEAD);
    }

    /* texte (uniquement si la police est présente) */
    if (g_font_loaded) {
        const char *state = !g_rd.active ? "respawn"
                          : g_rd.resting ? "au repos"
                          : g_rd.max_impact > 0.0f ? "au sol" : "en l'air";
        snprintf(text, sizeof(text), "RAGDOLL - %s", state);
        set_string(&g_strings[ns++], text, c->window_x + 0.008f, c->window_y + 0.008f,
                   0.025f, COL_TEXT);
        snprintf(text, sizeof(text), "impact %.1f m/s  tete %.1f m/s", g_rd.max_impact,
                 g_rd.head_impact);
        set_string(&g_strings[ns++], text, c->window_x + 0.008f, c->window_y + 0.040f,
                   0.020f, g_rd.head_impact > 6.0f ? COL_TEXT_ALERT : COL_TEXT);
    }

    *_piNumQuads = nq;
    *_ppQuad = g_quads;
    *_piNumString = ns;
    *_ppString = g_strings;
}

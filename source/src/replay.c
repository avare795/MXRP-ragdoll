/*
 * replay.c - enregistrement et lecture des segments pilotés (voir replay.h)
 */
#include "replay.h"
#include "rclock.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define RR_MAGIC   0x52525852u /* "RXRR" */
#define RR_VERSION 4 /* 2 : + temps de la session au départ (double) ; 3 : + numéro de session ;
                      4 : + position du corps de chute du jeu à chaque image */
#define MAX_REC_FRAMES 30 * 120 /* 2 min à 30 images/s */
#define MAX_SEGS 64

typedef struct {
    float t;
    float pos[RD_NUM_POINTS][3];
    DriveSnap snap;
    float mpos[3];   /* corps de chute du jeu (modèle du pilote) ; mpos[1] = NAN : inconnu (v1 à v3) */
} Frame;

typedef struct {
    float t;
    float pos[RD_NUM_POINTS][3];
    DriveSnap snap;
} FrameV3;

typedef struct {
    float start[12]; /* matrice monde du modèle du pilote au départ (3x4 par lignes) */
    int n;
    Frame *f;
    int cooling;     /* déjà joué : attendre que le pilote s'éloigne */
    float prev_d;    /* distance au départ à la mise à jour précédente */
    double rc_start; /* temps de la rediffusion au départ (-1 : pas encore vu) */
    double rc_live;  /* même horloge lue pendant la session, au départ (-1 : inconnue) */
    long long session; /* session de jeu où il a été enregistré (0 : inconnue) */
    int by;          /* joueur de la rediffusion qui le joue ou vient de le jouer (-1 : aucun) */
    int live;        /* 1 = encore en cours d'enregistrement (pote qui marche pendant qu'on regarde) */
} Segment;

/* Plusieurs pilotes peuvent être rejoués en même temps (le joueur et ses potes) : un « joueur »
   par squelette de la rediffusion */
typedef struct {
    void *key;
    double seen;
    int play;
    double t0;
    float prev_d[64];
} Player;

static char g_dir[600] = "", g_track[120] = "";
static ReplayLogFn g_log = NULL;

/* enregistrements en cours : 0 = le joueur, 1.. = pilotes des autres joueurs (réseau) */
typedef struct {
    Segment seg;
    double t0, last;
    int on;
    double rc;
    int live_seg;    /* place dans les passages chargés quand il est lu en direct (-1 : non) */
} Rec;
static Rec g_recs[REPLAY_CH];
static long long g_session = 0, g_latest = 0;

void replay_new_session(void)
{
    g_session = (long long)time(NULL);
}

/* segments chargés pour la rediffusion */
static Segment g_segs[MAX_SEGS];
static int g_nsegs = 0;
#define RP_WATCH 48 /* pilotes suivis dans la rediffusion (tous, même ceux sans passage) */
static Player g_pl[RP_WATCH];

void replay_init(const char *save_dir, ReplayLogFn log)
{
    g_log = log;
    if (save_dir) {
        size_t l = strlen(save_dir);
        snprintf(g_dir, sizeof(g_dir), "%s%sMXRP_Ragdoll_replay", save_dir,
                 (l && save_dir[l - 1] != '\\' && save_dir[l - 1] != '/') ? "\\" : "");
#ifdef _WIN32
        CreateDirectoryA(g_dir, NULL);
#endif
    }
}

void replay_set_track(const char *track_id)
{
    snprintf(g_track, sizeof(g_track), "%s", track_id ? track_id : "");
}

void replay_rec_begin(int ch, const float model_world[12], double rc_live)
{
    Rec *rc;
    if (!g_dir[0] || !g_track[0] || ch < 0 || ch >= REPLAY_CH)
        return;
    rc = &g_recs[ch];
    if (!rc->seg.f)
        rc->seg.f = (Frame *)malloc(sizeof(Frame) * MAX_REC_FRAMES);
    if (!rc->seg.f)
        return;
    memcpy(rc->seg.start, model_world, sizeof(rc->seg.start));
    rc->rc = rc_live;
    rc->seg.n = 0;
    rc->on = 1;
    rc->live_seg = -1;
    rc->t0 = -1.0;
    rc->last = -1.0;
}

void replay_rec_frame(int ch, double t, const Ragdoll *rd, const DriveSnap *snap, const float model_world[12])
{
    Frame *f;
    Rec *rc;
    if (ch < 0 || ch >= REPLAY_CH)
        return;
    rc = &g_recs[ch];
    if (!rc->on || rc->seg.n >= MAX_REC_FRAMES)
        return;
    if (rc->t0 < 0.0)
        rc->t0 = t;
    if (rc->last >= 0.0 && t - rc->last < 1.0 / 30.0)
        return; /* 30 images par seconde suffisent (on interpole) */
    rc->last = t;
    f = &rc->seg.f[rc->seg.n++];
    f->t = (float)(t - rc->t0);
    memcpy(f->pos, rd->pos, sizeof(f->pos));
    f->snap = *snap;
    if (rc->live_seg >= 0 && rc->live_seg < g_nsegs)
        g_segs[rc->live_seg].n = rc->seg.n; /* lu en direct : une image de plus */
    if (model_world) {
        f->mpos[0] = model_world[3];
        f->mpos[1] = model_world[7];
        f->mpos[2] = model_world[11];
    } else {
        f->mpos[0] = f->mpos[2] = 0.0f;
        f->mpos[1] = NAN;
    }
}

void replay_rec_end(int ch)
{
    char path[800];
    FILE *fp;
    unsigned hdr[3];
    Rec *rc;
    if (ch < 0 || ch >= REPLAY_CH)
        return;
    rc = &g_recs[ch];
    if (!rc->on)
        return;
    rc->on = 0;
    if (rc->live_seg >= 0 && rc->live_seg < g_nsegs) {
        /* il était lu en direct : le passage chargé garde sa propre copie des images */
        Segment *ls = &g_segs[rc->live_seg];
        Frame *cp = rc->seg.n > 0 ? (Frame *)malloc(sizeof(Frame) * (size_t)rc->seg.n) : NULL;
        if (cp) {
            memcpy(cp, rc->seg.f, sizeof(Frame) * (size_t)rc->seg.n);
            ls->f = cp;
            ls->n = rc->seg.n;
        } else {
            ls->n = 0;
            ls->f = NULL;
        }
        ls->live = 0;
    }
    rc->live_seg = -1;
    if (rc->seg.n < 10)
        return; /* trop court pour être utile */
    snprintf(path, sizeof(path), "%s\\%s.rdr", g_dir, g_track);
    fp = fopen(path, "ab");
    if (!fp)
        return;
    hdr[0] = RR_MAGIC;
    hdr[1] = RR_VERSION;
    hdr[2] = (unsigned)rc->seg.n;
    fwrite(hdr, sizeof(hdr), 1, fp);
    fwrite(rc->seg.start, sizeof(rc->seg.start), 1, fp);
    fwrite(&rc->rc, sizeof(rc->rc), 1, fp);
    fwrite(&g_session, sizeof(g_session), 1, fp);
    fwrite(rc->seg.f, sizeof(Frame), (size_t)rc->seg.n, fp);
    fclose(fp);
    if (g_log)
        g_log("rediffusion : %.1f s %s enregistres (%s)", rc->seg.f[rc->seg.n - 1].t,
              ch ? "d'un autre joueur" : "du pilote", path);
}

void replay_rec_share(int ch, double rc_start)
{
    Rec *rc;
    Segment *s;
    if (ch < 0 || ch >= REPLAY_CH)
        return;
    rc = &g_recs[ch];
    if (!rc->on || rc->live_seg >= 0 || g_nsegs >= MAX_SEGS || !rc->seg.f)
        return;
    s = &g_segs[g_nsegs];
    memset(s, 0, sizeof(*s));
    memcpy(s->start, rc->seg.start, sizeof(s->start));
    s->f = rc->seg.f; /* mêmes images que l'enregistrement, tant qu'il dure */
    s->n = rc->seg.n;
    s->prev_d = 1e9f;
    s->rc_start = rc_start;
    s->rc_live = rc_start;
    s->session = g_session;
    s->by = -1;
    s->live = 1;
    rc->live_seg = g_nsegs++;
}

int replay_rec_active(int ch)
{
    return ch >= 0 && ch < REPLAY_CH && g_recs[ch].on;
}

void replay_rec_end_all(void)
{
    int i;
    for (i = 0; i < REPLAY_CH; ++i)
        replay_rec_end(i);
}

static void free_segs(void)
{
    int i, c;
    for (i = 0; i < g_nsegs; ++i) {
        int shared = 0;
        for (c = 0; c < REPLAY_CH; ++c)
            if (g_recs[c].seg.f && g_segs[i].f == g_recs[c].seg.f)
                shared = 1; /* images d'un enregistrement encore en cours */
        if (!shared)
            free(g_segs[i].f);
    }
    for (c = 0; c < REPLAY_CH; ++c)
        g_recs[c].live_seg = -1;
    g_nsegs = 0;
    memset(g_pl, 0, sizeof(g_pl));
}

static void load_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    unsigned hdr[3];
    if (!fp)
        return;
    while (fread(hdr, sizeof(hdr), 1, fp) == 1) {
        Segment *s;
        if (hdr[0] != RR_MAGIC || hdr[1] < 1 || hdr[1] > 4 || hdr[2] == 0 || hdr[2] > MAX_REC_FRAMES)
            break;
        if (g_nsegs >= MAX_SEGS) {
            /* on garde les plus récents : le plus ancien sort */
            free(g_segs[0].f);
            memmove(&g_segs[0], &g_segs[1], sizeof(Segment) * (MAX_SEGS - 1));
            --g_nsegs;
        }
        s = &g_segs[g_nsegs];
        memset(s, 0, sizeof(*s));
        s->prev_d = 1e9f;
        s->rc_start = -1.0;
        s->rc_live = -1.0;
        s->by = -1;
        s->n = (int)hdr[2];
        s->f = (Frame *)malloc(sizeof(Frame) * (size_t)s->n);
        if (!s->f || fread(s->start, sizeof(s->start), 1, fp) != 1 ||
            (hdr[1] >= 2 && fread(&s->rc_live, sizeof(s->rc_live), 1, fp) != 1) ||
            (hdr[1] >= 3 && fread(&s->session, sizeof(s->session), 1, fp) != 1)) {
            free(s->f);
            break;
        }
        if (hdr[1] >= 4) {
            if (fread(s->f, sizeof(Frame), (size_t)s->n, fp) != (size_t)s->n) {
                free(s->f);
                break;
            }
        } else {
            /* anciennes images, sans le corps de chute */
            int k, bad = 0;
            for (k = 0; k < s->n && !bad; ++k) {
                FrameV3 o;
                if (fread(&o, sizeof(o), 1, fp) != 1) {
                    bad = 1;
                    break;
                }
                s->f[k].t = o.t;
                memcpy(s->f[k].pos, o.pos, sizeof(o.pos));
                s->f[k].snap = o.snap;
                s->f[k].mpos[0] = s->f[k].mpos[2] = 0.0f;
                s->f[k].mpos[1] = NAN;
            }
            if (bad) {
                free(s->f);
                break;
            }
        }
        if (s->session > g_latest)
            g_latest = s->session;
        ++g_nsegs;
    }
    fclose(fp);
}

void replay_load(void)
{
    free_segs();
    g_latest = 0;
    if (!g_dir[0])
        return;
    if (g_track[0]) {
        char path[800];
        snprintf(path, sizeof(path), "%s\\%s.rdr", g_dir, g_track);
        load_file(path);
    }
#ifdef _WIN32
    if (!g_nsegs) {
        /* piste inconnue en rediffusion : tous les enregistrements (le point de départ trie) */
        char pat[800], path[800];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof(pat), "%s\\*.rdr", g_dir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                snprintf(path, sizeof(path), "%s\\%s", g_dir, fd.cFileName);
                load_file(path);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#endif
    if (g_log)
        g_log("rediffusion : %d passage(s) du pilote charges", g_nsegs);
}

/* départ : le bassin du pilote du jeu au même endroit (40 cm) que celui enregistré au départ,
   et le modèle orienté pareil */
static int same_start(const float a[12], const float pelvis[3], const Segment *sg, float *prev_d)
{
    const float *b = sg->start, *sp = sg->f[0].pos[RD_PELVIS];
    float dx = pelvis[0] - sp[0], dy = pelvis[1] - sp[1], dz = pelvis[2] - sp[2], dr = 0.0f;
    int i;
    /* le pilote du jeu ne repasse pas exactement au même point (1,4 m vu en rediffusion) :
       on prend le moment où il est passé au plus près, à moins de 1,8 m */
    float d = sqrtf(dx * dx + dy * dy + dz * dz), prev = *prev_d;
    *prev_d = d;
    if (prev > 1.8f || !(d > prev + 0.005f)) /* on vient de passer au plus près (même si le pilote est déjà loin : éjecté vite) */
        return 0;
    for (i = 0; i < 3; ++i)
        dr += fabsf(a[i * 4 + 0] - b[i * 4 + 0]) + fabsf(a[i * 4 + 1] - b[i * 4 + 1]) + fabsf(a[i * 4 + 2] - b[i * 4 + 2]);
    return dr < 2.0f;
}

/* corps de chute enregistré à l'instant t du passage (NAN si inconnu) */
static float seg_model_dist(const Segment *sg, float t, const float mw[12])
{
    int a = 0;
    float dx, dz;
    while (a + 1 < sg->n && sg->f[a + 1].t <= t)
        ++a;
    if (isnan(sg->f[a].mpos[1]))
        return NAN;
    dx = sg->f[a].mpos[0] - mw[3];
    dz = sg->f[a].mpos[2] - mw[11];
    return sqrtf(dx * dx + dz * dz);
}

static int player_of(void *key, double now)
{
    int i, free_i = -1;
    for (i = 0; i < RP_WATCH; ++i)
        if (g_pl[i].key == key) {
            g_pl[i].seen = now;
            return i;
        }
    for (i = 0; i < RP_WATCH; ++i)
        if (!g_pl[i].key || now - g_pl[i].seen > 2.0 || now < g_pl[i].seen) {
            free_i = i;
            break;
        }
    if (free_i < 0)
        return -1;
    for (i = 0; i < g_nsegs; ++i)
        if (g_segs[i].by == free_i)
            g_segs[i].by = -1;
    memset(&g_pl[free_i], 0, sizeof(g_pl[free_i]));
    g_pl[free_i].key = key;
    g_pl[free_i].seen = now;
    g_pl[free_i].play = -1;
    for (i = 0; i < 64; ++i)
        g_pl[free_i].prev_d[i] = 1e9f;
    return free_i;
}

int replay_play(void *key, const float model_world[12], const float pelvis[3], double now, int have_rc, double rc,
                Ragdoll *rd, DriveSnap *snap, int *starting, int *player)
{
    int i, p;
    Player *pl;
    *starting = 0;
    p = player_of(key, now);
    *player = p;
    if (p < 0)
        return 0;
    pl = &g_pl[p];
    /* segments joués par ce pilote : de nouveau jouables une fois qu'il est parti ailleurs */
    for (i = 0; i < g_nsegs; ++i)
        if (g_segs[i].cooling && g_segs[i].by == p && pl->play != i && g_segs[i].n > 0) {
            const float *sp = g_segs[i].f[0].pos[RD_PELVIS];
            float dx = pelvis[0] - sp[0], dz = pelvis[2] - sp[2];
            if (dx * dx + dz * dz > 6.0f * 6.0f) {
                g_segs[i].cooling = 0;
                g_segs[i].by = -1;
            }
        }
    if (have_rc) {
        /* temps de la rediffusion connu : un passage déjà vu se rejoue d'après lui, même après
           un retour en arrière ou une avance rapide */
        int found = -1;
        float best = 1e9f;
        if (rclock_verified())
            for (i = 0; i < g_nsegs; ++i)
                /* seulement la dernière session : chaque session repart de 0 */
                if (g_segs[i].rc_start < 0.0 && g_segs[i].rc_live >= 0.0 && g_segs[i].session && g_segs[i].session == g_latest)
                    g_segs[i].rc_start = g_segs[i].rc_live; /* même horloge en session et en rediffusion */
        for (i = 0; i < g_nsegs; ++i)
            if (g_segs[i].n >= 2 && g_segs[i].rc_start >= 0.0 && rc >= g_segs[i].rc_start &&
                rc < g_segs[i].rc_start + g_segs[i].f[g_segs[i].n - 1].t + (g_segs[i].live ? 1.0 : 0.0) &&
                (g_segs[i].by < 0 || g_segs[i].by == p || !g_segs[i].cooling || g_pl[g_segs[i].by].play != i)) {
                /* plusieurs pilotes : celui dont le corps de chute est au bon endroit */
                float d = seg_model_dist(&g_segs[i], (float)(rc - g_segs[i].rc_start), model_world);
                if (isnan(d))
                    d = 3.9f; /* ancien enregistrement : accepté, mais après les autres */
                if (d < 4.0f && d < best) {
                    best = d;
                    found = i;
                }
            }
        if (pl->play >= 0 && g_segs[pl->play].rc_start < 0.0)
            g_segs[pl->play].rc_start = rc - (now - pl->t0); /* horloge trouvée en cours de passage */
        if (found >= 0) {
            if (pl->play != found) {
                *starting = 1;
                if (g_log)
                    g_log("rediffusion : passage %d repris a %.1f s", found, rc - g_segs[found].rc_start);
            }
            pl->play = found;
            g_segs[found].by = p;
            g_segs[found].cooling = 1;
            pl->t0 = now - (rc - g_segs[found].rc_start);
        } else if (pl->play >= 0 && g_segs[pl->play].rc_start >= 0.0) {
            if (g_log)
                g_log("rediffusion : passage %d quitte (temps %.1f s, depart %.1f s)", pl->play, rc, g_segs[pl->play].rc_start);
            pl->play = -1; /* avant ou après le passage */
            return 0;
        }
    }
    if (pl->play < 0) {
        static double last_log = 0.0;
        static float best_d = 1e9f;
        static int best_i = -1;
        for (i = 0; i < g_nsegs; ++i) {
            const float *sp;
            float dx, dy, dz, d;
            if (g_segs[i].n < 2 || g_segs[i].live)
                continue;
            sp = g_segs[i].f[0].pos[RD_PELVIS];
            dx = pelvis[0] - sp[0];
            dy = pelvis[1] - sp[1];
            dz = pelvis[2] - sp[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < best_d) {
                best_d = d;
                best_i = i;
            }
        }
        if (g_log && ++last_log > 200.0 && g_nsegs) { /* toutes les ~200 images */
            last_log = 0.0;
            g_log("rediffusion : depart le plus proche %.2f m (passage %d%s)", best_d, best_i,
                  best_i >= 0 && g_segs[best_i].cooling ? ", deja joue" : "");
            best_d = 1e9f;
        }
        for (i = 0; i < g_nsegs && i < 64; ++i)
            if (!g_segs[i].cooling && !g_segs[i].live && g_segs[i].n >= 2 &&
                !(have_rc && rclock_verified() && g_segs[i].rc_start >= 0.0) && /* (son moment est connu) */
                same_start(model_world, pelvis, &g_segs[i], &pl->prev_d[i]))
                break;
        if (i >= g_nsegs || i >= 64)
            return 0;
        pl->play = i;
        pl->t0 = now;
        g_segs[i].cooling = 1;
        g_segs[i].by = p;
        if (have_rc) {
            g_segs[i].rc_start = rc;
            if (g_segs[i].rc_live >= 0.0) {
                /* l'horloge lue pendant la session est-elle celle de la rediffusion ? */
                if (g_log)
                    g_log("rediffusion : temps au depart %.2f s, enregistre en session %.2f s", rc, g_segs[i].rc_live);
                if (fabs(rc - g_segs[i].rc_live) < 1.5)
                    rclock_set_verified();
            }
        }
        *starting = 1;
        if (g_log)
            g_log("rediffusion : passage %d rejoue (%.1f s)", i, g_segs[i].f[g_segs[i].n - 1].t);
    }
    {
        const Segment *s = &g_segs[pl->play];
        float t = (float)(now - pl->t0), u;
        int a = 0, b;
        if (s->n < 1) {
            pl->play = -1;
            return 0;
        }
        if (s->live && t >= s->f[s->n - 1].t)
            t = s->f[s->n - 1].t; /* en direct : on reste sur la dernière image reçue */
        else if (t >= s->f[s->n - 1].t) {
            if (g_log)
                g_log("rediffusion : passage %d fini (%.1f s)", pl->play, t);
            pl->play = -1;
            return 0;
        }
        while (a + 1 < s->n && s->f[a + 1].t <= t)
            ++a;
        b = a + 1 < s->n ? a + 1 : a;
        u = s->f[b].t > s->f[a].t ? (t - s->f[a].t) / (s->f[b].t - s->f[a].t) : 0.0f;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
        for (i = 0; i < RD_NUM_POINTS; ++i) {
            int k;
            for (k = 0; k < 3; ++k) {
                float v = s->f[a].pos[i][k] + (s->f[b].pos[i][k] - s->f[a].pos[i][k]) * u;
                rd->prev[i][k] = rd->pos[i][k];
                rd->pos[i][k] = v;
            }
        }
        *snap = u < 0.5f ? s->f[a].snap : s->f[b].snap;
    }
    return 1;
}

void replay_stop(void *key)
{
    int i;
    for (i = 0; i < RP_WATCH; ++i)
        if (!key || g_pl[i].key == key)
            g_pl[i].play = -1;
}

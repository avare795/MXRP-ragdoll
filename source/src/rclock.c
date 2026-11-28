/*
 * rclock.c - horloge de la rediffusion (voir rclock.h)
 *
 * Le jeu ne donne pas aux plugins le temps de la rediffusion. On le cherche dans les données
 * du programme (sections modifiables de mxbikes.exe) : des nombres qui avancent d'une seconde
 * par seconde (réel, double, ou millisecondes entières). L'horloge de la rediffusion est celle
 * qui s'ARRÊTE pendant une pause alors qu'une autre horloge continue ; tant qu'aucune pause n'a
 * été vue, on prend la première trouvée (ou le temps réel).
 */
#include "rclock.h"
#include "hook.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

#define MAX_CAND 4096
#define MAX_REGIONS 16

typedef struct {
    const unsigned char *addr;
    int type;          /* 0 = float (s), 1 = double (s), 2 = entier (ms) */
    double last;       /* dernière valeur, secondes */
    double moving;     /* temps passé à avancer à vitesse ~1 */
    int stalled;       /* vu arrêté alors qu'une autre horloge avançait */
    int dead;
} Cand;

static struct {
    const unsigned char *base;
    size_t size;
    unsigned char *snap;
} g_reg[MAX_REGIONS];
static int g_nreg = 0;
static Cand g_cand[MAX_CAND];
static int g_ncand = 0;
static int g_phase = 0; /* 0 = rien, 1 = instantané pris, 2 = candidats suivis */
static double g_snap_t = 0.0, g_upd_t = 0.0;
static int g_best = -1;
static RclockLogFn g_log = NULL;
static char g_file[800] = "";
static long long g_known_off = -1; /* décalage dans mxbikes.exe de l'horloge retenue */
static int g_known_type = 0, g_verified = 0;

static const unsigned char *module_base(void)
{
    return (const unsigned char *)GetModuleHandleA(NULL);
}

static void save_known(void)
{
    FILE *f;
    if (!g_file[0] || g_known_off < 0)
        return;
    if ((f = fopen(g_file, "w")) != NULL) {
        fprintf(f, "%lld %d %d\n", g_known_off, g_known_type, g_verified);
        fclose(f);
    }
}

void rclock_set_file(const char *path)
{
    FILE *f;
    long long off;
    int type, ver;
    snprintf(g_file, sizeof(g_file), "%s", path ? path : "");
    if (g_file[0] && (f = fopen(g_file, "r")) != NULL) {
        if (fscanf(f, "%lld %d %d", &off, &type, &ver) == 3 && off >= 0 && type >= 0 && type <= 2) {
            g_known_off = off;
            g_known_type = type;
            g_verified = ver;
        }
        fclose(f);
    }
}

int rclock_verified(void)
{
    return g_verified && g_best >= 0 && g_cand[g_best].addr == module_base() + g_known_off;
}

void rclock_set_verified(void)
{
    if (g_best < 0 || g_verified)
        return;
    g_verified = 1;
    save_known();
    if (g_log)
        g_log("rediffusion : horloge verifiee (meme temps en session et en rediffusion)");
}

static double read_val(const unsigned char *p, int type, int *ok)
{
    union { float f; double d; int i; } u;
    *ok = safe_read(p, &u, type == 1 ? 8 : 4);
    if (!*ok)
        return 0.0;
    if (type == 0)
        return (double)u.f;
    if (type == 1)
        return u.d;
    return u.i / 1000.0;
}

static void find_regions(void)
{
    const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;
    int i;
    g_nreg = 0;
    if (!base || ((IMAGE_DOS_HEADER *)base)->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections && g_nreg < MAX_REGIONS; ++i, ++sec) {
        size_t sz = sec->Misc.VirtualSize;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE) || sz < 16 || sz > (64u << 20))
            continue;
        g_reg[g_nreg].base = base + sec->VirtualAddress;
        g_reg[g_nreg].size = sz & ~(size_t)7;
        g_reg[g_nreg].snap = (unsigned char *)malloc(g_reg[g_nreg].size);
        if (g_reg[g_nreg].snap)
            ++g_nreg;
    }
}

void rclock_init(RclockLogFn log)
{
    g_log = log;
}

static double read_val(const unsigned char *p, int type, int *ok);

void rclock_reset(void)
{
    int i;
    for (i = 0; i < g_nreg; ++i)
        free(g_reg[i].snap);
    g_nreg = 0;
    g_ncand = 0;
    g_phase = 0;
    g_best = -1;
    if (g_known_off >= 0) {
        /* horloge déjà trouvée une autre fois : reprise tout de suite */
        int ok;
        const unsigned char *a = module_base() + g_known_off;
        double v = read_val(a, g_known_type, &ok);
        if (ok && isfinite(v)) {
            memset(&g_cand[0], 0, sizeof(g_cand[0]));
            g_cand[0].addr = a;
            g_cand[0].type = g_known_type;
            g_cand[0].last = v;
            g_cand[0].stalled = 1;
            g_cand[0].moving = 1.0;
            g_ncand = 1;
            g_best = 0;
            g_phase = 2;
            if (g_log)
                g_log("rediffusion : horloge reprise (%.1f s)", v);
        }
    }
}

int rclock_live_time(double *t)
{
    int ok;
    double v;
    if (g_known_off < 0)
        return 0;
    v = read_val(module_base() + g_known_off, g_known_type, &ok);
    if (!ok || !isfinite(v))
        return 0;
    *t = v;
    return 1;
}

static void take_snapshot(double now)
{
    int i;
    if (!g_nreg)
        find_regions();
    for (i = 0; i < g_nreg; ++i)
        if (!safe_read(g_reg[i].base, g_reg[i].snap, g_reg[i].size))
            memset(g_reg[i].snap, 0, g_reg[i].size);
    g_snap_t = now;
    g_phase = 1;
}

/* deuxième instantané : ce qui a avancé d'environ dt (vitesse 0,8 à 1,25) */
static void collect(double now)
{
    double dt = now - g_snap_t;
    int r;
    size_t o;
    g_ncand = 0;
    for (r = 0; r < g_nreg; ++r) {
        const unsigned char *b = g_reg[r].base, *s = g_reg[r].snap;
        unsigned char *cur = (unsigned char *)malloc(g_reg[r].size);
        if (!cur)
            continue;
        if (!safe_read(b, cur, g_reg[r].size)) {
            free(cur);
            continue;
        }
        for (o = 0; o + 8 <= g_reg[r].size && g_ncand < MAX_CAND; o += 4) {
            int t;
            for (t = 0; t < 3 && g_ncand < MAX_CAND; ++t) {
                double a, c;
                if (t == 1 && (o & 7))
                    continue;
                if (t == 0) {
                    float fa, fc;
                    memcpy(&fa, s + o, 4);
                    memcpy(&fc, cur + o, 4);
                    a = fa;
                    c = fc;
                } else if (t == 1) {
                    memcpy(&a, s + o, 8);
                    memcpy(&c, cur + o, 8);
                } else {
                    int ia, ic;
                    memcpy(&ia, s + o, 4);
                    memcpy(&ic, cur + o, 4);
                    a = ia / 1000.0;
                    c = ic / 1000.0;
                }
                if (!isfinite(a) || !isfinite(c) || a < 0.0 || c > 100000.0)
                    continue;
                if (c - a > 0.8 * dt && c - a < 1.25 * dt) {
                    Cand *k = &g_cand[g_ncand++];
                    memset(k, 0, sizeof(*k));
                    k->addr = b + o;
                    k->type = t;
                    k->last = c;
                }
            }
        }
        free(cur);
    }
    g_phase = 2;
    g_upd_t = now;
    if (g_log)
        g_log("rediffusion : %d horloge(s) candidate(s) dans le jeu", g_ncand);
}

static void update(double now)
{
    double dt = now - g_upd_t;
    int i, any_moving = 0;
    static double rate[MAX_CAND];
    if (dt < 0.2)
        return;
    g_upd_t = now;
    for (i = 0; i < g_ncand; ++i) {
        Cand *k = &g_cand[i];
        int ok;
        double v;
        rate[i] = -1.0;
        if (k->dead)
            continue;
        v = read_val(k->addr, k->type, &ok);
        if (!ok || !isfinite(v)) {
            k->dead = 1;
            continue;
        }
        rate[i] = (v - k->last) / dt;
        k->last = v;
        if (rate[i] > 0.8 && rate[i] < 1.25) {
            k->moving += dt;
            any_moving = 1;
        }
    }
    /* une horloge arrêtée pendant qu'une autre avance : c'est la pause de la rediffusion */
    for (i = 0; i < g_ncand; ++i)
        if (!g_cand[i].dead && any_moving && fabs(rate[i]) < 1e-6 && g_cand[i].moving > 0.5 && !g_cand[i].stalled) {
            g_cand[i].stalled = 1;
            /* parmi celles qui s'arrêtent : celle qui a le plus avancé */
            if (g_best < 0 || !g_cand[g_best].stalled || g_cand[i].moving > g_cand[g_best].moving) {
                g_best = i;
                if (g_cand[i].addr - module_base() != g_known_off) {
                    g_known_off = (long long)(g_cand[i].addr - module_base());
                    g_known_type = g_cand[i].type;
                    g_verified = 0;
                    save_known();
                }
                if (g_log)
                    g_log("rediffusion : horloge de la rediffusion trouvee (pause vue, type %d)", g_cand[i].type);
            }
        }
}

int rclock_tick(double now)
{
    if (g_phase == 0) {
        take_snapshot(now);
        return 0;
    }
    if (g_phase == 1) {
        if (now - g_snap_t >= 1.0)
            collect(now);
        return 0;
    }
    update(now);
    return g_best >= 0;
}

int rclock_time(double *t)
{
    int ok;
    double v;
    if (g_best < 0)
        return 0;
    v = read_val(g_cand[g_best].addr, g_cand[g_best].type, &ok);
    if (!ok || !isfinite(v))
        return 0;
    *t = v;
    return 1;
}

#else
void rclock_init(RclockLogFn log) { (void)log; }
void rclock_reset(void) {}
int rclock_tick(double now) { (void)now; return 0; }
int rclock_time(double *t) { (void)t; return 0; }
void rclock_set_file(const char *path) { (void)path; }
int rclock_live_time(double *t) { (void)t; return 0; }
int rclock_verified(void) { return 0; }
void rclock_set_verified(void) {}
#endif

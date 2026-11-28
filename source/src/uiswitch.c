/*
 * uiswitch.c - bouton « Plugin Ragdoll : ACTIF / COUPE » dans les menus du jeu
 * (ui/testsetup.mnu, hostsetup.mnu : un item_checkbox ID_RAGDOLL_PLUGIN sans case dessinée).
 *
 * v0.40.99 : le jeu gère lui-même la valeur de la case (nom + 64) et la remet à zéro à
 * Démarrer : on ne s'en sert que pour voir un clic (changement de valeur avec la souris sur la
 * ligne du bouton). L'état choisi est gardé dans MXRP_Ragdoll_menu.ini et affiché en réécrivant
 * en mémoire le texte du bouton (ACTIF / COUPE).
 * v0.41.00 : la recherche en mémoire (plusieurs secondes) tourne dans son propre fil et
 * recommence d'abord là où la case était : les clics ne sont plus manqués.
 */
#include "uiswitch.h"
#include "glcam.h"
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define ID_NAME "ID_RAGDOLL_PLUGIN"
#define STATE_OFF 64
#define LABEL "Plugin Ragdoll : "
#define MAXHIT 16
#define MAXTXT 32
#define MAXREG 16

static UiLogFn g_log = NULL;
static volatile LONG g_run = 0;
static volatile int *g_enabled = NULL;
static volatile int g_on_track = 0;
static volatile int g_in_event = 0;
static volatile int g_on = 1;
static volatile int g_online = 0; /* v1.8.204 : multi activé par le joueur */
static volatile int g_pause = 0; /* v1.8.204 : en ligne sans le multi : plugin en pause pour la session */
static volatile int g_force = 0; /* v1.7.25 : serveur RAGDOLL : actif pour la session, bouton pas changé */
static volatile int g_need_full = 1;
static volatile int g_need_txt = 1;
static CRITICAL_SECTION g_cs;

/* cases trouvées (adresse du nom) et dernière valeur vue */
static uintptr_t g_hit[MAXHIT];
static unsigned g_last[MAXHIT];
static int g_nhit = 0;
static uintptr_t g_txt[MAXTXT];
static int g_ntxt = 0;
static uintptr_t g_reg[MAXREG]; /* zones où des cases ont été vues */
static int g_nreg = 0;
static uintptr_t g_treg[MAXREG]; /* zones où des textes du bouton ont été vus */
static int g_ntreg = 0;
static int g_nlog = 0;
static volatile unsigned g_val = 2;       /* valeur de la case la plus récente */
static volatile DWORD g_changed = 0;      /* moment du dernier changement de case vu */

void uiswitch_event(int on) { g_in_event = on; }
void uiswitch_on_track(int on) { g_on_track = on; }

static int safe_copy(void *dst, uintptr_t src, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (void *)src, dst, n, &got) && got == n;
}

static int state_at(uintptr_t name, unsigned *v)
{
    char nm[sizeof(ID_NAME)];
    *v = 2;
    return safe_copy(nm, name, sizeof(nm)) && !memcmp(nm, ID_NAME, sizeof(nm)) && safe_copy(v, name + STATE_OFF, 4) && *v <= 1;
}

static char g_file[600] = "";

static void save_state(int on)
{
    FILE *f;
    if (!g_file[0] || (f = fopen(g_file, "w")) == NULL)
        return;
    fprintf(f, "; bouton Plugin Ragdoll des menus du jeu (1 = actif)\nenabled = %d\n", on ? 1 : 0);
    fclose(f);
}

static int load_state(int def)
{
    FILE *f;
    int v = def;
    char line[128];
    if (!g_file[0] || (f = fopen(g_file, "r")) == NULL)
        return def;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, " enabled = %d", &v) == 1)
            break;
    fclose(f);
    return v ? 1 : 0;
}

static const float g_rects[][4] = {
    {0.4125f, 0.5556f, 0.5975f, 0.5778f}, /* testsetup.mnu */
    {0.70625f, 0.7667f, 0.89125f, 0.7889f}, /* hostsetup.mnu */
};
/* v1.4 : bouton dessiné par le plugin dans les menus, quand les fichiers de menu n'ont pas le
   bouton (aucune case trouvée par le balayage) */
static volatile LONG g_scans_done = 0;
static const float g_ov_rect[4] = { 0.805f, 0.012f, 0.990f, 0.052f };
static volatile int g_ov_vis = 0;
static volatile LONG g_ov_hover = 0;

static int mouse_norm(float *x, float *y)
{
    POINT pt;
    RECT rc;
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w)
        GetWindowThreadProcessId(w, &pid);
    if (!w || pid != GetCurrentProcessId() || !GetCursorPos(&pt) || !ScreenToClient(w, &pt) || !GetClientRect(w, &rc) ||
        rc.right <= 0 || rc.bottom <= 0)
        return 0;
    *x = (float)pt.x / rc.right;
    *y = (float)pt.y / rc.bottom;
    return 1;
}

static int mouse_on_overlay(void)
{
    float x, y;
    return g_ov_vis && mouse_norm(&x, &y) && x >= g_ov_rect[0] && x <= g_ov_rect[2] && y >= g_ov_rect[1] &&
           y <= g_ov_rect[3];
}

static int mouse_on_button(void)
{
    POINT pt;
    RECT rc;
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    unsigned i;
    if (w)
        GetWindowThreadProcessId(w, &pid);
    if (!w || pid != GetCurrentProcessId() || !GetCursorPos(&pt) || !ScreenToClient(w, &pt) || !GetClientRect(w, &rc) || rc.right <= 0 || rc.bottom <= 0)
        return 0;
    {
        float x = (float)pt.x / rc.right, y = (float)pt.y / rc.bottom;
        for (i = 0; i < sizeof(g_rects) / sizeof(g_rects[0]); ++i)
            if (x >= g_rects[i][0] - 0.02f && x <= g_rects[i][2] + 0.02f && y >= g_rects[i][1] - 0.012f && y <= g_rects[i][3] + 0.012f)
                return 1;
    }
    return 0;
}

/* mots affichés après LABEL (actif, coupé) ; le mot court finit par un zéro */
static const char *g_words[][2] = {{"ACTIF", "COUPE"}, {"ON", "OFF"}};
#define NWORDS ((int)(sizeof(g_words) / sizeof(g_words[0])))
static int word_set(const char *t)
{
    int i, j;
    for (i = 0; i < NWORDS; ++i)
        for (j = 0; j < 2; ++j)
            if (!memcmp(t, g_words[i][j], strlen(g_words[i][j]) + 1))
                return i;
    return -1;
}

static int region_ok(const MEMORY_BASIC_INFORMATION *m)
{
    return m->State == MEM_COMMIT && m->Type == MEM_PRIVATE && (m->Protect & PAGE_READWRITE) && !(m->Protect & PAGE_GUARD) &&
           m->RegionSize < ((size_t)64 << 20);
}

typedef struct {
    uintptr_t hit[MAXHIT];
    int nhit;
    uintptr_t txt[MAXTXT];
    int ntxt;
    unsigned long long bytes;
} Scan;

/* v0.41.10 : la zone est copiée par morceaux (ReadProcessMemory) avant d'être lue. Lire la
   mémoire du jeu directement plantait le jeu quand il libérait la zone pendant la recherche
   (vu deux fois : msvcrt+0x61e30 dans ce fil). Une page disparue = morceau ignoré. */
#define CHUNK (256 * 1024)
#define OVL 64 /* recouvrement entre morceaux : un nom à cheval n'est pas perdu */
static unsigned char g_chunk[CHUNK + OVL];
static void scan_region(uintptr_t base, size_t sz, int want_txt, Scan *r)
{
    size_t pl = sizeof(ID_NAME) - 1, tl = sizeof(LABEL) - 1, off;
    int got_any = 0;
    for (off = 0; off < sz; off += CHUNK) {
        size_t n = sz - off < CHUNK + OVL ? sz - off : CHUNK + OVL, i;
        SIZE_T got = 0;
        const unsigned char *p = g_chunk;
        uintptr_t cb = base + off;
        if (!ReadProcessMemory(GetCurrentProcess(), (void *)cb, g_chunk, n, &got) || got != n) {
            /* morceau partiellement lisible : on prend ce qui a été copié */
            if (!got)
                continue;
            n = got;
        }
        got_any = 1;
        r->bytes += n < CHUNK ? n : CHUNK;
        for (i = 0; i + pl + 1 <= n && r->nhit < MAXHIT;) {
            const unsigned char *q = (const unsigned char *)memchr(p + i, 'I', n - pl - i);
            unsigned v;
            if (!q)
                break;
            i = (size_t)(q - p);
            if (i >= CHUNK && off + CHUNK < sz)
                break; /* dans le recouvrement : vu au morceau suivant */
            if (i + pl + 1 <= n && !memcmp(p + i, ID_NAME, pl + 1) && state_at(cb + i, &v)) {
                int k, dup = 0;
                r->hit[r->nhit++] = cb + i;
                for (k = 0; k < g_nreg; ++k)
                    dup |= g_reg[k] == base;
                if (!dup && g_nreg < MAXREG)
                    g_reg[g_nreg++] = base;
            }
            ++i;
        }
        if (want_txt)
            for (i = 0; i + tl + 8 <= n && r->ntxt < MAXTXT;) {
                const unsigned char *q = (const unsigned char *)memchr(p + i, 'P', n - tl - 8 - i + 1);
                if (!q)
                    break;
                i = (size_t)(q - p);
                if (i >= CHUNK && off + CHUNK < sz)
                    break;
                if (i + tl + 8 <= n && !memcmp(p + i, LABEL, tl) && word_set((const char *)p + i + tl) >= 0) {
                    int k, dup = 0;
                    r->txt[r->ntxt++] = cb + i + tl;
                    for (k = 0; k < g_ntreg; ++k)
                        dup |= g_treg[k] == base;
                    if (!dup && g_ntreg < MAXREG)
                        g_treg[g_ntreg++] = base;
                }
                ++i;
            }
    }
    (void)got_any;
}

static void scan_full(int want_txt, Scan *r)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = 0x10000;
    /* les cases sont sous 4 Go ; au-dessus, de grandes zones réservées (très lentes à lire) */
    while (a < 0x100000000ULL && VirtualQuery((void *)a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        if (region_ok(&mbi))
            scan_region(base, mbi.RegionSize, want_txt, r);
        if (end <= a)
            break;
        a = end;
    }
}

/* recherche rapide : seulement les zones où cases et textes ont déjà été vus */
static void scan_known(Scan *r)
{
    uintptr_t z[MAXREG * 2];
    int n = 0, k, j;
    for (k = 0; k < g_nreg; ++k)
        z[n++] = g_reg[k];
    for (k = 0; k < g_ntreg; ++k) {
        int dup = 0;
        for (j = 0; j < n; ++j)
            dup |= z[j] == g_treg[k];
        if (!dup)
            z[n++] = g_treg[k];
    }
    for (k = 0; k < n; ++k) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)z[k], &mbi, sizeof(mbi)) == sizeof(mbi) && (uintptr_t)mbi.BaseAddress == z[k] && region_ok(&mbi))
            scan_region(z[k], mbi.RegionSize, 1, r);
    }
}

/* nouvelle liste de cases : on garde la dernière valeur des cases déjà connues */
static void publish(const Scan *r, int with_txt)
{
    int k, j;
    unsigned last[MAXHIT];
    for (k = 0; k < r->nhit; ++k) {
        last[k] = 2;
        for (j = 0; j < g_nhit; ++j)
            if (g_hit[j] == r->hit[k])
                last[k] = g_last[j];
        if (last[k] > 1 && state_at(r->hit[k], &last[k])) {
            /* case nouvelle : le jeu recrée la case quand on clique dessus */
            if (g_val <= 1 && last[k] != g_val)
                g_changed = GetTickCount() | 1;
            g_val = last[k];
        }
    }
    EnterCriticalSection(&g_cs);
    memcpy(g_hit, r->hit, sizeof(g_hit));
    memcpy(g_last, last, sizeof(g_last));
    g_nhit = r->nhit;
    if (with_txt) {
        for (k = 0; k < r->ntxt; ++k) {
            int dup = 0;
            for (j = 0; j < g_ntxt; ++j)
                dup |= g_txt[j] == r->txt[k];
            if (!dup && g_ntxt < MAXTXT) {
                g_txt[g_ntxt++] = r->txt[k];
                if (g_log && g_nlog++ < 300)
                    g_log("menu : texte du bouton en %p", (void *)r->txt[k]);
            }
        }
        if (g_ntxt)
            g_need_txt = 0;
    }
    LeaveCriticalSection(&g_cs);
}


/* v0.41.14 : nom du serveur. Le jeu garde le nom choisi sur l'écran « Configuration Serveur »
   dans ses propres données (mxbikes.exe, tableau de caractères fixe) et le reprend en
   lançant le serveur. Bouton ACTIF : on y ajoute « RAGDOLL » s'il n'y a pas déjà
   « ragdoll » (les plugins des joueurs qui rejoignent s'activent alors tout seuls) ; COUPE :
   on retire ce « RAGDOLL » ajouté. Le profil n'est jamais modifié par le plugin (lu seulement
   pour connaître le nom de départ). */
#define SRV_MAX 60          /* place sûre dans le tableau du jeu (64 octets au moins) */
#define SRV_TAG " RAGDOLL"
static char g_save_dir[600] = "";
static char g_own_dir[600] = ""; /* v1.8.106 : nos fichiers (Documents\PiBoSo\MX Bikes\MXRP_Ragdoll\) */
static uintptr_t g_srv = 0;
static int g_srv_tries = 0;

static int read_ini_value(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "rb");
    char line[512];
    size_t kl = strlen(key);
    int ok = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            char *v = line + kl + 1;
            size_t L = strcspn(v, "\r\n");
            if (L >= n)
                L = n - 1;
            memcpy(out, v, L);
            out[L] = 0;
            ok = 1;
            break;
        }
    fclose(f);
    return ok;
}

static int has_ragdoll(const char *s)
{
    char t[SRV_MAX + 8];
    int i;
    for (i = 0; s[i] && i < (int)sizeof(t) - 1; ++i)
        t[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] - 'A' + 'a') : s[i];
    t[i] = 0;
    return strstr(t, "ragdoll") != NULL;
}

/* cherche le nom (suivi d'un zéro) dans les zones écrivables de mxbikes.exe */
/* v1.1.5 : l'adresse du nom dans le jeu ne change pas d'une partie à l'autre (tableau fixe de
   mxbikes.exe) : retenue (décalage depuis le début du jeu + date de compilation du jeu) dans
   MXRP_Ragdoll_serveur.ini, elle sert dès le lancement suivant. Avant, le nom n'était trouvé
   qu'après le gros balayage des menus (5 s), souvent trop tard (serveur lancé sans RAGDOLL). */
static DWORD exe_stamp(uintptr_t base)
{
    DWORD pe = 0, st = 0;
    if (!safe_copy(&pe, base + 0x3C, 4) || pe > 4096 || !safe_copy(&st, base + pe + 8, 4))
        return 0;
    return st;
}

static int srv_rw_ok(uintptr_t a)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    return VirtualQuery((void *)a, &mbi, sizeof(mbi)) == sizeof(mbi) && (uintptr_t)mbi.AllocationBase == base &&
           mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) &&
           a + SRV_MAX <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static void srv_cache_path(char *out, size_t n)
{
    snprintf(out, n, "%sMXRP_Ragdoll_serveur.ini", g_own_dir[0] ? g_own_dir : g_save_dir);
}

static int srv_from_cache(void)
{
    char path[700], line[128];
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    unsigned long long rva = 0;
    unsigned long st = 0;
    FILE *f;
    if (!g_save_dir[0])
        return 0;
    srv_cache_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "rva = %llx", &rva);
        sscanf(line, "jeu = %lx", &st);
    }
    fclose(f);
    if (!rva || st != exe_stamp(base) || !srv_rw_ok(base + (uintptr_t)rva))
        return 0;
    g_srv = base + (uintptr_t)rva;
    if (g_log)
        g_log("serveur : emplacement du nom repris (MXRP_Ragdoll_serveur.ini), RAGDOLL ajoute des que le bouton est ACTIF");
    return 1;
}

static void srv_cache_save(void)
{
    char path[700];
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    FILE *f;
    if (!g_srv || !g_save_dir[0])
        return;
    srv_cache_path(path, sizeof(path));
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "; MXRP_Ragdoll : emplacement du nom du serveur dans le jeu (ne pas modifier)\n");
    fprintf(f, "rva = %llx\njeu = %lx\n", (unsigned long long)(g_srv - base), (unsigned long)exe_stamp(base));
    fclose(f);
}

static void srv_find(void)
{
    char gpath[700], ppath[900], prof[128], name[SRV_MAX + 8];
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL), a = base;
    size_t nl;
    static unsigned char buf[256 * 1024 + 80];
    int found = 0;
    if (g_srv || g_srv_tries >= 1000 || !g_save_dir[0])
        return;
    if (g_srv_tries == 0 && srv_from_cache())
        return;
    ++g_srv_tries; /* réessayé à chaque recherche : le jeu ne remplit le nom qu'après le profil chargé */
    snprintf(gpath, sizeof(gpath), "%sglobal.ini", g_save_dir);
    if (!read_ini_value(gpath, "lastprofile", prof, sizeof(prof)))
        return;
    snprintf(ppath, sizeof(ppath), "%sprofiles\\%s\\profile.ini", g_save_dir, prof);
    if (!read_ini_value(ppath, "hostsetup_name", name, sizeof(name)) || !name[0]) {
        if (g_log)
            g_log("serveur : nom du serveur pas trouve dans le profil %s", prof);
        g_srv_tries = 1000;
        return;
    }
    nl = strlen(name) + 1; /* avec le zéro */
    while (VirtualQuery((void *)a, &mbi, sizeof(mbi)) == sizeof(mbi) && (uintptr_t)mbi.AllocationBase == base) {
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) {
            uintptr_t rb = (uintptr_t)mbi.BaseAddress;
            size_t off;
            for (off = 0; off < mbi.RegionSize; off += 256 * 1024) {
                size_t n = mbi.RegionSize - off < sizeof(buf) ? mbi.RegionSize - off : sizeof(buf), i;
                SIZE_T got = 0;
                ReadProcessMemory(GetCurrentProcess(), (void *)(rb + off), buf, n, &got);
                for (i = 1; i + nl + 4 <= got; ++i)
                    if (buf[i] == (unsigned char)name[0] && buf[i - 1] == 0 && !memcmp(buf + i, name, nl)) {
                        /* place libre derrière (zéros) : c'est bien un tableau, pas une chaîne serrée */
                        size_t k, room = 0;
                        for (k = nl; i + k < got && k < SRV_MAX && buf[i + k] == 0; ++k)
                            ++room;
                        if (nl + room >= SRV_MAX && !found) {
                            g_srv = rb + off + i;
                            found = 1;
                        }
                    }
            }
        }
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    if (g_srv)
        srv_cache_save();
    if (g_log && (g_srv || g_srv_tries == 1))
        g_log("serveur : nom \"%s\" %s", name, g_srv ? "trouve dans le jeu (RAGDOLL ajoute si le bouton est ACTIF)" : "pas encore trouve dans le jeu");
}

/* v1.1.6 : autres copies du nom dans le jeu. Le jeu recopie le nom en quittant l'écran du nom :
   un clic sur ACTIF / COUPÉ à l'écran suivant ne changeait que la première copie, et le serveur
   gardait l'ancien nom (journal : COUPÉ, mais serveur encore « … RAGDOLL »). */
#define MAX_SRV_COPIES 6
static uintptr_t g_srv_copy[MAX_SRV_COPIES];
static int g_nsrv_copy = 0;
static volatile int g_srv_rescan = 1;

static size_t base_len(const char *s)
{
    size_t L = strlen(s), tl = strlen(SRV_TAG);
    if (L > tl && !strcmp(s + L - tl, SRV_TAG))
        return L - tl;
    return L;
}

static void srv_find_copies(void)
{
    char cur[SRV_MAX + 1];
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL), a = base;
    static unsigned char buf[256 * 1024 + 80];
    size_t bl, tl = strlen(SRV_TAG);
    int n = 0;
    if (!g_srv || !safe_copy(cur, g_srv, SRV_MAX))
        return;
    cur[SRV_MAX] = 0;
    bl = base_len(cur);
    if (bl < 3)
        return;
    while (VirtualQuery((void *)a, &mbi, sizeof(mbi)) == sizeof(mbi) && (uintptr_t)mbi.AllocationBase == base) {
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) {
            uintptr_t rb = (uintptr_t)mbi.BaseAddress;
            size_t off;
            for (off = 0; off < mbi.RegionSize; off += 256 * 1024) {
                size_t nn = mbi.RegionSize - off < sizeof(buf) ? mbi.RegionSize - off : sizeof(buf), i;
                SIZE_T got = 0;
                ReadProcessMemory(GetCurrentProcess(), (void *)(rb + off), buf, nn, &got);
                for (i = 1; i + bl + tl + 2 <= got && n < MAX_SRV_COPIES; ++i) {
                    uintptr_t at = rb + off + i;
                    size_t k, L;
                    if (buf[i] != (unsigned char)cur[0] || buf[i - 1] != 0 || at == g_srv || memcmp(buf + i, cur, bl))
                        continue;
                    /* même nom, avec ou sans RAGDOLL, puis des zéros (place pour l'ajout) */
                    if (buf[i + bl] == 0)
                        L = bl;
                    else if (!memcmp(buf + i + bl, SRV_TAG, tl) && buf[i + bl + tl] == 0)
                        L = bl + tl;
                    else
                        continue;
                    for (k = L; i + k < got && k < bl + tl + 1 && buf[i + k] == 0; ++k)
                        ;
                    if (k >= bl + tl + 1)
                        g_srv_copy[n++] = at;
                }
            }
        }
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    g_nsrv_copy = n;
    if (g_log)
        g_log("serveur : %d autre(s) copie(s) du nom dans le jeu", n);
}

static void srv_sync_copies(const char *want)
{
    int i;
    size_t bl = base_len(want), wl = strlen(want);
    for (i = 0; i < g_nsrv_copy; ++i) {
        char c[SRV_MAX + 1];
        if (!safe_copy(c, g_srv_copy[i], SRV_MAX))
            continue;
        c[SRV_MAX] = 0;
        if (base_len(c) != bl || memcmp(c, want, bl) || !strcmp(c, want))
            continue;
        WriteProcessMemory(GetCurrentProcess(), (void *)g_srv_copy[i], want, wl + 1, NULL);
    }
}

static void srv_update(int on)
{
    static char logged[SRV_MAX + 1] = "";
    char cur[SRV_MAX + 1];
    size_t L, tl = strlen(SRV_TAG);
    if (!g_srv || !safe_copy(cur, g_srv, SRV_MAX))
        return;
    if (g_srv_rescan) {
        g_srv_rescan = 0;
        srv_find_copies();
    }
    cur[SRV_MAX] = 0;
    L = strlen(cur);
    if (L >= SRV_MAX - 1)
        return; /* pas un nom terminé : on ne touche à rien */
    if (on && L > 0 && !has_ragdoll(cur) && L + tl < SRV_MAX) {
        /* l'écran « Configuration Serveur » ouvert réécrit ce nom à chaque image : l'ajout ne
           tient qu'une fois cet écran quitté ; on le refait donc sans rien dire */
        WriteProcessMemory(GetCurrentProcess(), (void *)(g_srv + L), SRV_TAG, tl + 1, NULL);
    } else if (on && has_ragdoll(cur) && strcmp(cur, logged)) {
        memcpy(logged, cur, sizeof(logged));
        if (g_log)
            g_log("serveur : nom pour le serveur \"%s\"", cur);
    } else if (!on && L > tl && !strcmp(cur + L - tl, SRV_TAG)) {
        static const char z = 0;
        WriteProcessMemory(GetCurrentProcess(), (void *)(g_srv + L - tl), &z, 1, NULL);
        if (g_log)
            g_log("serveur : RAGDOLL retire du nom (bouton COUPE)");
    }
    if (g_nsrv_copy && safe_copy(cur, g_srv, SRV_MAX)) {
        cur[SRV_MAX] = 0;
        if (strlen(cur) < SRV_MAX - 1)
            srv_sync_copies(cur);
    }
}

static DWORD WINAPI scan_thread(LPVOID unused)
{
    int it = 0, prev = -1;
    (void)unused;
    while (g_run) {
        Scan r;
        if (g_on_track || g_in_event) {
            Sleep(200);
            continue;
        }
        memset(&r, 0, sizeof(r));
        if (!g_srv && it % 8 == 0)
            srv_find(); /* v1.1.5 : sans attendre le gros balayage */
        if (g_nreg && !g_need_full && !g_need_txt && it % 4) {
            scan_known(&r);
            if (r.nhit)
                publish(&r, 1);
            else
                g_need_full = 1;
        } else {
            DWORD t0 = GetTickCount();
            int want_txt = 1;
            scan_full(want_txt, &r);
            publish(&r, want_txt);
            g_need_full = 0;
            srv_find();

            if (g_log && g_nlog < 300 && (r.nhit != prev || it < 3)) {
                ++g_nlog;
                g_log("menu : %d case(s), %d texte(s), %.0f Mo lus en %lu ms", r.nhit, r.ntxt, r.bytes / 1048576.0,
                      (unsigned long)(GetTickCount() - t0));
            }
            prev = r.nhit;
            InterlockedIncrement(&g_scans_done);
            if (!r.nhit) {
                /* v1.4 : pas de bouton dans les fichiers de menu (bouton superposé) : on ne relit
                   pas 1,8 Go toutes les 10 s, une fois toutes les 30 s suffit */
                int k;
                for (k = 0; k < 120 && g_run; ++k)
                    Sleep(250);
            }
        }
        ++it;
        Sleep(250);
    }
    return 0;
}

static void show_state(int on)
{
    int k;
    for (k = 0; k < g_ntxt; ++k) {
        char cur[8];
        int set;
        const char *w;
        if (!safe_copy(cur, g_txt[k], sizeof(cur)) || (set = word_set(cur)) < 0) {
            g_txt[k--] = g_txt[--g_ntxt]; /* plus notre texte */
            if (!g_ntxt)
                g_need_txt = 1;
            continue;
        }
        w = g_words[set][on ? 0 : 1];
        if (memcmp(cur, w, strlen(w) + 1))
            WriteProcessMemory(GetCurrentProcess(), (void *)g_txt[k], w, strlen(w) + 1, NULL);
    }
}

/* v0.41.00 : on voit le clic à la souris (bouton relâché sur la ligne du bouton), on change
   tout de suite, et on annule si la valeur du jeu (nom + 64) n'a pas bougé dans les 12 s
   (clic au même endroit sur un autre écran que celui de préparation). */
static volatile DWORD g_menu_case_t = 0; /* dernière fois qu'une case du bouton a été vue */

static int overlay_fn(GlcamOverlay *ov)
{
    DWORD now = GetTickCount();
    /* menus seulement ; pas si le bouton des fichiers de menu existe (vu il y a moins de 60 s) */
    /* v1.4.1 : affiché pour tout le monde dans les menus (aussi avec le bouton des fichiers de
       menu : les deux restent d'accord) */
    (void)now;
    g_ov_vis = !g_on_track && !g_in_event && g_scans_done > 0;
    if (!g_ov_vis)
        return 0;
    ov->visible = 1;
    ov->on = g_on;
    ov->hover = mouse_on_overlay();
    memcpy(ov->rect, g_ov_rect, sizeof(ov->rect));
    snprintf(ov->text, sizeof(ov->text), "MXRP Ragdoll : %s", g_on ? "ON" : "OFF");
    return 1;
}

static DWORD WINAPI watch_thread(LPVOID unused)
{
    int was_down = 0, down_on = 0, pending = 0;
    DWORD t_pend = 0;
    (void)unused;
    while (g_run) {
        int k, bad = 0, down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        DWORD now = GetTickCount();
        if (g_enabled && (*g_enabled ? 1 : 0) != ((g_on || g_force) && !g_pause))
            *g_enabled = (g_on || g_force) && !g_pause; /* le plugin suit toujours le bouton (ou un serveur RAGDOLL) ;
                                                           v1.8.204 : en pause en ligne sans le multi */
        if (!g_on_track && !g_in_event) {
            if (g_nhit > 0)
                g_menu_case_t = now | 1;
            if (down && !was_down)
                down_on = mouse_on_overlay() ? 2 : mouse_on_button();
            if (!down && was_down && down_on == 2 && mouse_on_overlay()) {
                /* bouton dessiné par le plugin : changé et enregistré tout de suite */
                g_on = !g_on;
                g_srv_rescan = 1;
                save_state(g_on);
                if (g_log && g_nlog++ < 300)
                    g_log("menu : clic sur le bouton du plugin (superpose) -> %s (enregistre)", g_on ? "ACTIF" : "COUPE");
                down_on = 0;
            }
            if (!down && was_down && down_on && mouse_on_button()) {
                g_on = !g_on;
                pending = 1;
                t_pend = now;
                g_srv_rescan = 1;
                if (g_log && g_nlog++ < 300)
                    g_log("menu : clic sur Plugin Ragdoll -> %s", g_on ? "ACTIF" : "COUPE");
            }
            EnterCriticalSection(&g_cs);
            for (k = 0; k < g_nhit; ++k) {
                unsigned v;
                if (!state_at(g_hit[k], &v)) {
                    g_hit[k] = g_hit[--g_nhit];
                    g_last[k] = g_last[g_nhit];
                    --k;
                    bad = 1;
                    continue;
                }
                if (g_last[k] <= 1 && v != g_last[k]) {
                    g_changed = now | 1;
                    g_val = v;
                }
                g_last[k] = v;
            }
            show_state(g_on);
            LeaveCriticalSection(&g_cs);
            srv_update(g_on && g_online); /* v1.8.204 : RAGDOLL ajouté au nom seulement avec le multi */
            if (bad)
                g_need_full = 1;
            if (pending) {
                if (g_changed && (LONG)(g_changed - t_pend) >= -200) {
                    pending = 0;
                    save_state(g_on);
                    if (g_log && g_nlog++ < 300)
                        g_log("menu : bouton Plugin Ragdoll %s (enregistre)", g_on ? "ACTIF" : "COUPE");
                } else if (now - t_pend > 12000) {
                    pending = 0;
                    g_on = !g_on;
                    if (g_log && g_nlog++ < 300)
                        g_log("menu : clic non confirme par le jeu, annule");
                }
            }
        } else if (pending) {
            /* Démarrer juste après le clic : on garde le choix */
            pending = 0;
            save_state(g_on);
        }
        was_down = down;
        Sleep(15);
    }
    return 0;
}

void uiswitch_start(UiLogFn log, volatile int *enabled, const char *save_dir, const char *own_dir)
{
    /* v1.5 : bouton superposé abandonné (le bouton est ajouté aux vrais menus, uipatch.c) */
    (void)overlay_fn;
    g_log = log;
    g_enabled = enabled;
    if (save_dir && save_dir[0])
        snprintf(g_save_dir, sizeof(g_save_dir), "%s%s", save_dir,
                 save_dir[strlen(save_dir) - 1] != '\\' && save_dir[strlen(save_dir) - 1] != '/' ? "\\" : "");
    if (own_dir && own_dir[0])
        snprintf(g_own_dir, sizeof(g_own_dir), "%s%s", own_dir,
                 own_dir[strlen(own_dir) - 1] != '\\' && own_dir[strlen(own_dir) - 1] != '/' ? "\\" : "");
    if (g_own_dir[0] || g_save_dir[0])
        snprintf(g_file, sizeof(g_file), "%sMXRP_Ragdoll_menu.ini", g_own_dir[0] ? g_own_dir : g_save_dir);
    g_on = load_state(1);
    if (g_enabled)
        *g_enabled = g_on;
    if (g_log)
        g_log("menu : bouton Plugin Ragdoll %s (MXRP_Ragdoll_menu.ini)", g_on ? "ACTIF" : "COUPE");
    if (InterlockedExchange(&g_run, 1))
        return;
    InitializeCriticalSection(&g_cs);
    CloseHandle(CreateThread(NULL, 0, scan_thread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, watch_thread, NULL, 0, NULL));
}

void uiswitch_stop(void)
{
    g_run = 0;
}
#else
void uiswitch_start(UiLogFn log, volatile int *enabled, const char *save_dir, const char *own_dir) { (void)log; (void)enabled; (void)save_dir; (void)own_dir; }
void uiswitch_stop(void) {}
void uiswitch_on_track(int on) { (void)on; }
void uiswitch_event(int on) { (void)on; }
#endif

void uiswitch_force(int on) { g_force = on; }
void uiswitch_set_online(int on) { g_online = on; } /* v1.8.204 */
void uiswitch_pause(int on) /* v1.8.204 */
{
    g_pause = on;
    if (g_enabled)
        *g_enabled = (g_on || g_force) && !g_pause;
}
int uiswitch_button_on(void) { return g_on; }

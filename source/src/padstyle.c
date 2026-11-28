/*
 * padstyle.c - v1.8.107 : noms des boutons de la manette affichés selon la manette du joueur
 *
 * Windows donne le fabricant de chaque manette branchée (liste « Raw Input », sans rien lire de la
 * manette elle-même) : Sony = PlayStation (DualShock 4, DualSense), sinon Xbox. Même à travers Steam
 * Input ou DS4Windows (qui la montrent au jeu comme une manette Xbox), la vraie manette Sony reste
 * dans la liste. Réglage du menu P : Auto / Xbox / PlayStation.
 * Les noms enregistrés dans les réglages (MXRP_Ragdoll.ini) restent ceux de Xbox : seul l'affichage change.
 */
#include "padstyle.h"
#include "lang.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static volatile int g_mode = 0;      /* 0 auto, 1 Xbox, 2 PlayStation */
static volatile int g_detected = 0;  /* 0 rien vu, 1 Xbox (ou autre), 2 PlayStation */
static PadStyleLogFn g_log = NULL;

#ifdef _WIN32
static int g_listed = 0;
static int detect_now(int *n_pads)
{
    UINT n = 0, i;
    RAWINPUTDEVICELIST *list;
    int sony = 0, other = 0;
    if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || n == 0 || n > 512)
        return 0;
    list = (RAWINPUTDEVICELIST *)HeapAlloc(GetProcessHeap(), 0, n * sizeof(RAWINPUTDEVICELIST));
    if (!list)
        return 0;
    n = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
    if (n == (UINT)-1)
        n = 0;
    for (i = 0; i < n; ++i) {
        RID_DEVICE_INFO info;
        UINT sz = sizeof(info);
        if (list[i].dwType != RIM_TYPEHID)
            continue;
        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        char nm[256];
        UINT nl = sizeof(nm);
        if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1)
            continue;
        nm[0] = 0;
        if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, nm, &nl) == (UINT)-1)
            nm[0] = 0;
        nm[sizeof(nm) - 1] = 0;
        /* v1.8.108 : liste des appareils HID dans le journal (une fois), pour comprendre ce que voit Windows */
        if (!g_listed && g_log) {
            g_log("manette : appareil HID fabricant %04lX produit %04lX, usage %u:%u (%.90s)", (unsigned long)info.hid.dwVendorId,
                  (unsigned long)info.hid.dwProductId, info.hid.usUsagePage, info.hid.usUsage, nm);
        }
        /* manette Sony : quel que soit l'usage annoncé (DualSense / DualShock 4 en ont plusieurs).
           v1.8.109 : avec Steam Input, la vraie manette est cachée ; Steam montre une manette Xbox
           virtuelle (fabricant 28DE) dont le nom garde celui de la vraie : « ...IG_00#054C&0CE6&... » */
        if (info.hid.dwVendorId == 0x054C || strstr(nm, "#054C&") || strstr(nm, "#054c&") || strstr(nm, "VID_054C") ||
            strstr(nm, "vid_054c")) {
            ++sony;
            continue;
        }
        /* page 1 (bureau), usage 4 (manette de jeu) ou 5 (gamepad) */
        if (info.hid.usUsagePage != 1 || (info.hid.usUsage != 4 && info.hid.usUsage != 5))
            continue;
        ++other;
    }
    g_listed = 1;
    HeapFree(GetProcessHeap(), 0, list);
    *n_pads = sony + other;
    return sony ? 2 : other ? 1 : 0;
}

static DWORD WINAPI detect_thread(LPVOID p)
{
    int last = -1;
    (void)p;
    for (;;) {
        int np = 0, d = detect_now(&np);
        if (d != last) {
            last = d;
            g_detected = d;
            if (g_log)
                g_log("manette : %s (%d manette(s) vue(s) par Windows) ; boutons affiches : %s", d == 2 ? "PlayStation (Sony)"
                      : d == 1 ? "Xbox ou autre" : "aucune", np, padstyle_is_ps() ? "PlayStation" : "Xbox");
        }
        Sleep(3000); /* manette branchée en cours de partie */
    }
    return 0;
}
#endif

void padstyle_start(PadStyleLogFn log)
{
#ifdef _WIN32
    static int started = 0;
    g_log = log;
    if (!started) {
        started = 1;
        CloseHandle(CreateThread(NULL, 0, detect_thread, NULL, 0, NULL));
    }
#else
    (void)log;
#endif
}

void padstyle_set_mode(int mode)
{
    g_mode = mode >= 0 && mode <= 2 ? mode : 0;
}

int padstyle_is_ps(void)
{
    return g_mode == 2 || (g_mode == 0 && g_detected == 2);
}

const char *padstyle_mode_name(int mode)
{
    if (mode == 1)
        return "Xbox";
    if (mode == 2)
        return "PlayStation";
    return g_detected == 2 ? tr("Auto (PlayStation)") : tr("Auto (Xbox)");
}

/* nom PlayStation d'un nom Xbox (NULL : pas un bouton) */
static const char *ps_name(const char *w, size_t l)
{
    static const struct { const char *x, *ps; } k[] = {
        { "A", "Croix" }, { "B", "Rond" }, { "X", "Carre" }, { "Y", "Triangle" }, { "LB", "L1" }, { "RB", "R1" },
        { "LT", "L2" }, { "RT", "R2" }, { "LS", "L3" }, { "RS", "R3" }, { "BACK", "Share" }, { "Back", "Share" },
        { "START", "Options" }, { "Start", "Options" },
    };
    size_t i;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); ++i)
        if (strlen(k[i].x) == l && !memcmp(k[i].x, w, l))
            return tr(k[i].ps);
    return NULL;
}

static int is_word(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

const char *padstyle_text(const char *in)
{
    static char buf[4][600];
    static int k = 0;
    char *out;
    size_t o = 0, i = 0;
    if (!in || !padstyle_is_ps())
        return in;
    out = buf[k++ & 3];
    while (in[i] && o + 1 < sizeof(buf[0])) {
        if (is_word(in[i]) && (i == 0 || !is_word(in[i - 1]))) {
            size_t j = i;
            const char *r;
            while (is_word(in[j]))
                ++j;
            r = ps_name(in + i, j - i);
            if (r) {
                size_t rl = strlen(r);
                if (o + rl + 1 >= sizeof(buf[0]))
                    break;
                memcpy(out + o, r, rl);
                o += rl;
            } else {
                size_t wl = j - i;
                if (o + wl + 1 >= sizeof(buf[0]))
                    break;
                memcpy(out + o, in + i, wl);
                o += wl;
            }
            i = j;
            continue;
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
    return out;
}

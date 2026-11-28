/*
 * padinject.c - appuyer sur un bouton de la manette à la place du joueur
 *
 * MX Bikes lit la manette avec DirectInput (dinput8.dll) : la touche de
 * respawn du joueur est la croix directionnelle (POV 0) vers le haut. On ne
 * peut pas simuler une manette avec SendInput. On intercepte donc la lecture
 * de l'état des manettes (IDirectInputDevice8::GetDeviceState, emplacement 9
 * de la table des fonctions, partagée par tous les périphériques DirectInput)
 * et, pendant un court instant, on y met la croix vers le haut.
 *
 * Le profil de commandes du joueur n'est pas modifié.
 */
#include "padinject.h"

#ifdef _WIN32
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <string.h>

typedef HRESULT(WINAPI *PFN_DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HRESULT(STDMETHODCALLTYPE *PFN_GetDeviceState)(void *self, DWORD cb, LPVOID data);

#define SLOT_GETDEVICESTATE 9
#define POV_OFFSET 32 /* DIJOYSTATE et DIJOYSTATE2 : 6 axes + 2 curseurs, puis les POV */

static PFN_GetDeviceState g_orig[2];
static void **g_vtbl[2];
static volatile LONG g_pov_up = 0;
/* touche de respawn du profil du joueur (CTRL_RESET) : croix, bouton ou touche du clavier */
static volatile LONG g_reset_kind = PAD_RESET_POV, g_reset_code = 0;
static const WORD k_pov_mask[4] = { 0x0001, 0x0008, 0x0002, 0x0004 }; /* haut, droite, bas, gauche */
/* numéros des boutons du plugin manette du jeu (xinput64.dli, lu dans son code) : 0 Start, 1 Back,
   2 clic gauche, 3 clic droit, 4 LB, 5 RB, 6 A, 7 B, 8 X, 9 Y (v0.40.21 : l'ordre A, B, X, Y supposé
   avant faisait appuyer B au lieu de Back pour le respawn) */
static const WORD k_btn_mask[10] = { 0x0010, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200, 0x1000, 0x2000, 0x4000, 0x8000 };
static volatile LONG g_block = 0;
static volatile LONG g_block_reset = 0; /* 1 : le bouton de respawn du joueur ne va plus au jeu (marche forcée) */ /* 1 : le jeu ne voit plus la manette (pilote descendu de la moto) */
static volatile LONG g_seen_pad = 0;
static volatile LONG g_inj_di = 0, g_inj_x = 0, g_inj_fake = 0; /* v1.3.3 : appuis vus par chaque chemin */
static volatile LONG g_x_on = 0, g_x_kind = 0, g_x_code = 0; /* v1.8.24 : 2e bouton appuyé par le plugin (vue du jeu) */
static volatile LONG g_force_rt = 0;
static volatile LONG g_force_btn = 0; /* v1.8.34 : boutons XInput forcés (essais au clavier) */ /* v1.8.33 : gâchette droite forcée (essais au clavier) */
static volatile LONG g_block_mask = 0; /* boutons gardés pour le plugin (vue du jeu, à pied) */

/* ---------- v1.8.195 : manette lue par DirectInput (PlayStation sans Steam Input, etc.) ----------
   Le jeu lit ces manettes lui-même par DirectInput ; le plugin ne voyait que les manettes XInput (Xbox,
   ou PlayStation par Steam Input). On garde le dernier état lu par le jeu et on le traduit en XInput
   (disposition DualShock / DualSense : 0 carré, 1 croix, 2 rond, 3 triangle, 4 L1, 5 R1, 6 L2, 7 R2,
   8 Share/Create, 9 Options, 10 L3, 11 R3 ; sticks X/Y et Z/Rz, gâchettes Rx/Ry). */
typedef struct {
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
} DiPad;
static volatile LONG g_di_have = 0;
static DiPad g_di_last;
static void *g_di_dev = NULL;   /* la manette suivie (le jeu peut en lire plusieurs) */
static DWORD g_di_dev_t = 0;    /* dernier mouvement de cette manette */
static volatile LONG g_x_t = 0; /* v1.8.195 : dernière lecture XInput réussie (GetTickCount) */
static PadLogFn g_pad_log = NULL;
/* boutons DualShock -> XInput */
static const WORD k_ds_to_x[12] = { 0x4000, 0x1000, 0x2000, 0x8000, 0x0100, 0x0200, 0, 0, 0x0020, 0x0010, 0x0040, 0x0080 };

/* axe DirectInput -> -1..1 (plage 0..65535 par défaut, ou -1000..1000 / -32768..32767 si le jeu l'a réglée) */
static float di_axis(LONG v)
{
    if (v >= -1100 && v <= 1100 && v != 0) {
        float f = v / 1000.0f;
        return f < -1.0f ? -1.0f : f > 1.0f ? 1.0f : f;
    }
    if (v < 0)
        return v / 32768.0f;
    return (v - 32767.5f) / 32767.5f;
}

static void di_read_state(void *self, const DIJOYSTATE *js)
{
    DiPad p;
    int i, moved;
    DWORD now = GetTickCount();
    float t;
    memset(&p, 0, sizeof(p));
    for (i = 0; i < 12; ++i)
        if (js->rgbButtons[i] & 0x80)
            p.buttons |= k_ds_to_x[i];
    {
        DWORD pov = js->rgdwPOV[0];
        if ((pov & 0xFFFF) != 0xFFFF && pov < 36000) {
            if (pov >= 31500 || pov <= 4500) p.buttons |= 0x0001;
            if (pov >= 4500 && pov <= 13500) p.buttons |= 0x0008;
            if (pov >= 13500 && pov <= 22500) p.buttons |= 0x0002;
            if (pov >= 22500 && pov <= 31500) p.buttons |= 0x0004;
        }
    }
    p.lx = (SHORT)(di_axis(js->lX) * 32767.0f);
    p.ly = (SHORT)(-di_axis(js->lY) * 32767.0f); /* XInput : haut positif */
    p.rx = (SHORT)(di_axis(js->lZ) * 32767.0f);
    p.ry = (SHORT)(-di_axis(js->lRz) * 32767.0f);
    t = (di_axis(js->lRx) + 1.0f) * 0.5f; /* L2 : -1 relâchée, +1 enfoncée */
    p.lt = (BYTE)(t < 0.0f ? 0 : t > 1.0f ? 255 : t * 255.0f);
    t = (di_axis(js->lRy) + 1.0f) * 0.5f;
    p.rt = (BYTE)(t < 0.0f ? 0 : t > 1.0f ? 255 : t * 255.0f);
    if (js->rgbButtons[6] & 0x80) p.lt = 255; /* L2 / R2 en bouton aussi */
    if (js->rgbButtons[7] & 0x80) p.rt = 255;
    moved = p.buttons || p.lt > 60 || p.rt > 60 || p.lx > 12000 || p.lx < -12000 || p.ly > 12000 || p.ly < -12000;
    /* une manette n'est suivie qu'après l'appui d'un de ses boutons (volant, palonnier ou autre appareil au
       repos bizarre : le pilote ne doit pas marcher tout seul) */
    if (self == g_di_dev || (p.buttons && (!g_di_dev || now - g_di_dev_t > 1500))) {
        if (g_di_dev != self && g_pad_log)
            g_pad_log("manette : DirectInput suivie (appareil %p ; axes X %ld Y %ld Z %ld Rx %ld Ry %ld Rz %ld)", self,
                      js->lX, js->lY, js->lZ, js->lRx, js->lRy, js->lRz);
        g_di_dev = self;
        if (moved)
            g_di_dev_t = now;
        g_di_last = p;
        InterlockedExchange(&g_di_have, 1);
    }
}

/* le jeu ne doit pas voir ces boutons (masque XInput) sur la manette DirectInput */
static void di_strip(DIJOYSTATE *js, WORD mask, int all)
{
    int i;
    if (all) {
        int keep = (g_reset_kind == PAD_RESET_BUTTON && !g_block_reset) ? (int)g_reset_code : -1;
        DWORD pov = js->rgdwPOV[0];
        for (i = 0; i < 32; ++i)
            if (i != keep)
                js->rgbButtons[i] = 0;
        js->lX = js->lY = js->lZ = js->lRz = 32767;
        js->lRx = js->lRy = 0;
        if (!(g_reset_kind == PAD_RESET_POV && !g_block_reset && (pov & 0xFFFF) != 0xFFFF &&
              pov / 9000 == (DWORD)(g_reset_code & 3)))
            js->rgdwPOV[0] = 0xFFFFFFFF;
        return;
    }
    for (i = 0; i < 12; ++i)
        if (k_ds_to_x[i] & mask)
            js->rgbButtons[i] = 0;
    if (mask & 0x000F) {
        DWORD pov = js->rgdwPOV[0];
        if ((pov & 0xFFFF) != 0xFFFF && pov < 36000) {
            WORD d = 0;
            if (pov >= 31500 || pov <= 4500) d |= 0x0001;
            if (pov >= 4500 && pov <= 13500) d |= 0x0008;
            if (pov >= 13500 && pov <= 22500) d |= 0x0002;
            if (pov >= 22500 && pov <= 31500) d |= 0x0004;
            if (d & mask)
                js->rgdwPOV[0] = 0xFFFFFFFF;
        }
    }
}

static HRESULT inject_dev(void *self, void *data, DWORD cb, HRESULT r);
static HRESULT inject(void *data, DWORD cb, HRESULT r)
{
    return inject_dev(NULL, data, cb, r);
}
static volatile LONG g_di_reset_t = 0; /* v1.8.203 : dernier appui vu sur le bouton DirectInput de respawn */
static HRESULT inject_dev(void *self, void *data, DWORD cb, HRESULT r)
{
    if (SUCCEEDED(r) && data && (cb == sizeof(DIJOYSTATE) || cb == sizeof(DIJOYSTATE2))) {
        InterlockedExchange(&g_seen_pad, 1);
        /* v1.8.203 : respawn du profil sur une manette DirectInput (« BUTTON n » / « POV n ») : avant, le
           plugin ne le connaissait pas (croix vers le haut à la place) et ne pouvait pas le bloquer : avec la
           marche forcée, un appui court ramenait sur la moto de n'importe où. Vu, puis caché au jeu à pied
           (l'appui long du plugin le rejoue juste après). */
        if (g_reset_kind == PAD_RESET_DIBUTTON || g_reset_kind == PAD_RESET_DIPOV) {
            DIJOYSTATE *js = (DIJOYSTATE *)data;
            int c = (int)g_reset_code, down = 0;
            if (g_reset_kind == PAD_RESET_DIBUTTON && c >= 0 && c < 32)
                down = (js->rgbButtons[c] & 0x80) != 0;
            else if (g_reset_kind == PAD_RESET_DIPOV) {
                DWORD pov = js->rgdwPOV[0];
                down = (pov & 0xFFFF) != 0xFFFF && pov < 36000 && ((pov + 4500) / 9000) % 4 == (DWORD)(c & 3);
            }
            if (down)
                InterlockedExchange(&g_di_reset_t, (LONG)GetTickCount());
            if (down && g_block_reset) {
                if (g_reset_kind == PAD_RESET_DIBUTTON)
                    js->rgbButtons[c] = 0;
                else
                    js->rgdwPOV[0] = 0xFFFFFFFF;
            }
        }
        /* v1.8.195 : sans manette XInput depuis 1 s : on lit celle-ci (et on la bloque pour le jeu à pied) */
        if (self && GetTickCount() - (DWORD)g_x_t > 1000) {
            di_read_state(self, (const DIJOYSTATE *)data);
            if (self == g_di_dev) {
                if (g_block)
                    di_strip((DIJOYSTATE *)data, 0, 1);
                else if (g_block_mask)
                    di_strip((DIJOYSTATE *)data, (WORD)g_block_mask, 0);
            }
        }
        if (g_pov_up) {
            InterlockedIncrement(&g_inj_di);
            if (g_reset_kind == PAD_RESET_POV || g_reset_kind == PAD_RESET_DIPOV)
                *(DWORD *)((BYTE *)data + POV_OFFSET) = (DWORD)((g_reset_code & 3) * 9000); /* 0 haut, 9000 droite... */
            else if ((g_reset_kind == PAD_RESET_BUTTON || g_reset_kind == PAD_RESET_DIBUTTON) && g_reset_code >= 0 &&
                     g_reset_code < 32)
                ((BYTE *)data)[POV_OFFSET + 16 + g_reset_code] = 0x80; /* rgbButtons (après les 4 POV) ; v0.40.18 */
        }
        if (g_x_on) {
            if (g_x_kind == PAD_RESET_POV)
                *(DWORD *)((BYTE *)data + POV_OFFSET) = (DWORD)((g_x_code & 3) * 9000);
            else if (g_x_kind == PAD_RESET_BUTTON && g_x_code >= 0 && g_x_code < 32)
                ((BYTE *)data)[POV_OFFSET + 16 + g_x_code] = 0x80;
        }
    }
    return r;
}

static HRESULT STDMETHODCALLTYPE hk_state_a(void *self, DWORD cb, LPVOID data)
{
    return inject_dev(self, data, cb, g_orig[0](self, cb, data));
}

static HRESULT STDMETHODCALLTYPE hk_state_w(void *self, DWORD cb, LPVOID data)
{
    return inject_dev(self, data, cb, g_orig[1](self, cb, data));
}

static int patch(int which, const IID *iid, void *hook, PadLogFn log)
{
    HMODULE dll = LoadLibraryA("dinput8.dll");
    PFN_DI8Create create = dll ? (PFN_DI8Create)(void *)GetProcAddress(dll, "DirectInput8Create") : NULL;
    IUnknown *di = NULL;
    void *dev = NULL;
    void **vtbl;
    DWORD old;
    HRESULT hr;
    if (!create)
        return 0;
    hr = create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, iid, (void **)&di, NULL);
    if (FAILED(hr) || !di)
        return 0;
    /* CreateDevice est l'emplacement 3 de IDirectInput8 (A et W) */
    {
        typedef HRESULT(STDMETHODCALLTYPE * PFN_CreateDevice)(void *, REFGUID, void **, LPUNKNOWN);
        PFN_CreateDevice cd = (PFN_CreateDevice)(*(void ***)di)[3];
        hr = cd(di, &GUID_SysKeyboard, &dev, NULL);
    }
    if (FAILED(hr) || !dev) {
        di->lpVtbl->Release(di);
        return 0;
    }
    vtbl = *(void ***)dev;
    if (vtbl[SLOT_GETDEVICESTATE] != hook && VirtualProtect(&vtbl[SLOT_GETDEVICESTATE], sizeof(void *), PAGE_READWRITE, &old)) {
        g_orig[which] = (PFN_GetDeviceState)vtbl[SLOT_GETDEVICESTATE];
        vtbl[SLOT_GETDEVICESTATE] = hook;
        VirtualProtect(&vtbl[SLOT_GETDEVICESTATE], sizeof(void *), old, &old);
        g_vtbl[which] = vtbl;
    }
    ((IUnknown *)dev)->lpVtbl->Release((IUnknown *)dev);
    di->lpVtbl->Release(di);
    if (log)
        log("manette : lecture DirectInput %s interceptee", which ? "W" : "A");
    return g_vtbl[which] != NULL;
}

/* ---------- manette XInput, lue par le plugin d'entrée du jeu (xinput64.dli) ---------- */

typedef struct {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
} XState;
typedef DWORD(WINAPI *PFN_XInputGetState)(DWORD, XState *);

static PFN_XInputGetState g_x_orig = NULL;
static void **g_x_slot = NULL;
static volatile LONG g_x_seen = 0;
static DWORD g_x_bump = 0;

/* dernier état réel de la manette 0 (avant notre ajout), pour marcher avec le stick */
static volatile XState g_x_last;
static volatile LONG g_x_have = 0;
static volatile DWORD g_x_user = 3;

static DWORD WINAPI hk_XInputGetState(DWORD user, XState *st)
{
    DWORD r = g_x_orig(user, st);
    /* v1.3.2 : pas de manette branchée (joueur au clavier, respawn réglé sur un bouton de
       manette) : pendant l'appui du plugin, une manette 0 « branchée » avec seulement ce bouton.
       Avant, le respawn ne partait jamais (journal d'evann : 3 essais, pas de respawn). */
    if (r != 0 && st && user == 0 && g_pov_up && !g_x_seen) {
        static DWORD pk = 1;
        memset(st, 0, sizeof(*st));
        if (g_reset_kind == PAD_RESET_POV)
            st->buttons = k_pov_mask[g_reset_code & 3];
        else if (g_reset_kind == PAD_RESET_BUTTON && g_reset_code >= 0 && g_reset_code < 10)
            st->buttons = k_btn_mask[g_reset_code];
        st->packet = ++pk;
        InterlockedIncrement(&g_inj_fake);
        return 0;
    }
    if (r != 0 && st && user == 0 && (g_force_rt || g_force_btn || g_x_on)) {
        /* v1.8.37 : essais au clavier sans manette branchée : une manette 0 simulée */
        static DWORD pk2 = 1;
        memset(st, 0, sizeof(*st));
        st->rt = (BYTE)g_force_rt;
        st->buttons = (WORD)g_force_btn;
        if (g_x_on && g_x_kind == PAD_RESET_BUTTON && g_x_code >= 0 && g_x_code < 10)
            st->buttons |= k_btn_mask[g_x_code];
        st->packet = ++pk2;
        return 0;
    }
    if (r == 0 && st) {
        InterlockedExchange(&g_x_seen, 1);
        InterlockedExchange(&g_x_t, (LONG)GetTickCount());
        /* la première manette branchée (quel que soit son numéro) */
        if (user <= g_x_user) {
            g_x_user = user;
            g_x_last = *st;
            InterlockedExchange(&g_x_have, 1);
        }
        static LONG last = 0;
        LONG on = g_pov_up;
        if (g_block) {
            /* pilote à pied : la moto ne doit ni accélérer ni tourner avec le stick. Seul le
               bouton de respawn du profil passe encore (v0.40.17 : en ligne, le joueur ne
               pouvait plus faire respawn) */
            WORD keep = 0;
            if (g_reset_kind == PAD_RESET_POV)
                keep = k_pov_mask[g_reset_code & 3];
            else if (g_reset_kind == PAD_RESET_BUTTON && g_reset_code >= 0 && g_reset_code < 10)
                keep = k_btn_mask[g_reset_code];
            if (g_block_reset)
                keep = 0; /* marche forcée : respawn seulement par appui long (le plugin le rejoue) */
            st->buttons &= keep;
            st->lt = st->rt = 0;
            st->lx = st->ly = st->rx = st->ry = 0;
        }
        if (g_block_reset && !g_block) {
            if (g_reset_kind == PAD_RESET_POV)
                st->buttons &= (WORD)~k_pov_mask[g_reset_code & 3];
            else if (g_reset_kind == PAD_RESET_BUTTON && g_reset_code >= 0 && g_reset_code < 10)
                st->buttons &= (WORD)~k_btn_mask[g_reset_code];
        }
        st->buttons &= (WORD)~(WORD)g_block_mask; /* à pied : ces boutons sont au plugin */
        if (on != last) {
            last = on;
            g_x_bump += 0x10000; /* l'état change : nouveau numéro de paquet, pour que le plugin le relise */
        }
        if (on) {
            InterlockedIncrement(&g_inj_x);
            if (g_reset_kind == PAD_RESET_POV)
                st->buttons |= k_pov_mask[g_reset_code & 3];
            else if (g_reset_kind == PAD_RESET_BUTTON && g_reset_code >= 0 && g_reset_code < 10)
                st->buttons |= k_btn_mask[g_reset_code];
        }
        {
            static LONG xl = 0;
            LONG xo = g_x_on;
            if (xo != xl) {
                xl = xo;
                g_x_bump += 0x10000;
            }
            if (xo) {
                if (g_x_kind == PAD_RESET_POV)
                    st->buttons |= k_pov_mask[g_x_code & 3];
                else if (g_x_kind == PAD_RESET_BUTTON && g_x_code >= 0 && g_x_code < 10)
                    st->buttons |= k_btn_mask[g_x_code];
            }
        }
        if (g_force_btn) {
            st->buttons |= (WORD)g_force_btn;
            g_x_bump += 0x10000;
        }
        if (g_force_rt && st->rt < (BYTE)g_force_rt) {
            st->rt = (BYTE)g_force_rt;
            g_x_bump += 0x10000;
        }
        st->packet += g_x_bump;
    }
    return r;
}

/* Remplace une fonction importée par un module (sa table d'imports) */
static void **iat_slot(HMODULE mod, const char *dll, const char *fn)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!base || ((IMAGE_DOS_HEADER *)base)->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return NULL;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        IMAGE_THUNK_DATA64 *names, *slots;
        if (_stricmp((const char *)(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk)
            continue;
        names = (IMAGE_THUNK_DATA64 *)(base + imp->OriginalFirstThunk);
        slots = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            if (strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, fn) == 0)
                return (void **)&slots->u1.Function;
        }
    }
    return NULL;
}

int padinject_install_xinput(PadLogFn log)
{
    HMODULE dli;
    void **slot;
    DWORD old;
    if (g_x_slot)
        return 1;
    dli = GetModuleHandleA("xinput64.dli");
    if (!dli)
        return 0;
    slot = iat_slot(dli, "XINPUT9_1_0.dll", "XInputGetState");
    if (!slot || !*slot || !VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return 0;
    g_x_orig = (PFN_XInputGetState)*slot;
    *slot = (void *)hk_XInputGetState;
    VirtualProtect(slot, sizeof(void *), old, &old);
    g_x_slot = slot;
    if (log)
        log("manette : lecture XInput du plugin xinput64.dli interceptee");
    return 1;
}

int padinject_install(PadLogFn log)
{
    int n = 0;
    /* le jeu lit la manette par le plugin xinput64.dli : c'est la seule interception utile
       (l'ancienne interception DirectInput de la v0.12.1 est retirée) */
    n += padinject_install_xinput(log);
    /* v1.8.195 : aussi DirectInput : manettes que le jeu lit sans XInput (PlayStation sans Steam Input) */
    g_pad_log = log;
    n += patch(0, &IID_IDirectInput8A, (void *)hk_state_a, log);
    n += patch(1, &IID_IDirectInput8W, (void *)hk_state_w, log);
    (void)inject;
    return n;
}

void padinject_uninstall(void)
{
    int i;
    if (g_x_slot) {
        DWORD old;
        if (VirtualProtect(g_x_slot, sizeof(void *), PAGE_READWRITE, &old)) {
            *g_x_slot = (void *)g_x_orig;
            VirtualProtect(g_x_slot, sizeof(void *), old, &old);
        }
        g_x_slot = NULL;
    }
    for (i = 0; i < 2; ++i) {
        DWORD old;
        void **v = g_vtbl[i];
        if (v && VirtualProtect(&v[SLOT_GETDEVICESTATE], sizeof(void *), PAGE_READWRITE, &old)) {
            v[SLOT_GETDEVICESTATE] = (void *)g_orig[i];
            VirtualProtect(&v[SLOT_GETDEVICESTATE], sizeof(void *), old, &old);
        }
        g_vtbl[i] = NULL;
    }
}

void padinject_block(int on)
{
    InterlockedExchange(&g_block, on ? 1 : 0);
}

void padinject_set_reset(int kind, int code)
{
    InterlockedExchange(&g_reset_kind, kind);
    InterlockedExchange(&g_reset_code, code);
}

/* v1.8.24 : appuie (on = 1) ou relâche un autre bouton du jeu : croix, bouton ou touche */
void padinject_press_extra(int kind, int code, int on)
{
    LONG was;
    InterlockedExchange(&g_x_kind, kind);
    InterlockedExchange(&g_x_code, code);
    was = InterlockedExchange(&g_x_on, on ? 1 : 0);
    if (kind == PAD_RESET_KEY && (was != 0) != (on != 0)) {
        INPUT in;
        memset(&in, 0, sizeof(in));
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = (WORD)(code & 0x7F);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (code & 0x80 ? KEYEVENTF_EXTENDEDKEY : 0) | (on ? 0 : KEYEVENTF_KEYUP);
        SendInput(1, &in, sizeof(in));
    }
}

void padinject_force_buttons(unsigned short m)
{
    InterlockedExchange(&g_force_btn, m);
}

void padinject_force_rt(int v)
{
    InterlockedExchange(&g_force_rt, v < 0 ? 0 : (v > 255 ? 255 : v));
}

void padinject_pov_up(int on)
{
    LONG was = InterlockedExchange(&g_pov_up, on ? 1 : 0);
    if (g_reset_kind == PAD_RESET_KEY && (was != 0) != (on != 0)) {
        /* touche du clavier (code DirectInput = code de balayage) */
        INPUT in;
        memset(&in, 0, sizeof(in));
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = (WORD)(g_reset_code & 0x7F);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (g_reset_code & 0x80 ? KEYEVENTF_EXTENDEDKEY : 0) | (on ? 0 : KEYEVENTF_KEYUP);
        SendInput(1, &in, sizeof(in));
    }
}

int padinject_read(PadState *out)
{
    XState s;
    if (!out)
        return 0;
    if (g_di_have && GetTickCount() - (DWORD)g_x_t > 1000) {
        /* v1.8.195 : pas de manette XInput : celle lue par DirectInput */
        DiPad d = g_di_last;
        out->buttons = d.buttons;
        out->lx = d.lx / 32767.0f;
        out->ly = d.ly / 32767.0f;
        out->rx = d.rx / 32767.0f;
        out->ry = d.ry / 32767.0f;
        out->lt = d.lt / 255.0f;
        out->rt = d.rt / 255.0f;
        return 1;
    }
    if (!g_x_have)
        return 0;
    s = g_x_last;
    out->buttons = s.buttons;
    out->lx = s.lx / 32767.0f;
    out->ly = s.ly / 32767.0f;
    out->rx = s.rx / 32767.0f;
    out->ry = s.ry / 32767.0f;
    out->lt = s.lt / 255.0f;
    out->rt = s.rt / 255.0f;
    return 1;
}

void padinject_block_mask(unsigned short mask)
{
    InterlockedExchange(&g_block_mask, (LONG)mask);
}

/* bouton ou croix du profil -> masque XInput (kind : PAD_RESET_BUTTON ou PAD_RESET_POV) */
unsigned short padinject_game_mask(int kind, int code)
{
    if (kind == PAD_RESET_POV)
        return k_pov_mask[code & 3];
    if (kind == PAD_RESET_BUTTON && code >= 0 && code < 10)
        return k_btn_mask[code];
    return 0;
}

unsigned short padinject_reset_mask(void)
{
    return padinject_game_mask(g_reset_kind, g_reset_code);
}

void padinject_block_reset(int on)
{
    InterlockedExchange(&g_block_reset, on ? 1 : 0);
}

/* le joueur tient-il son bouton de respawn (manette) ? */
int padinject_reset_held(void)
{
    WORD m = 0;
    if (g_reset_kind == PAD_RESET_DIBUTTON || g_reset_kind == PAD_RESET_DIPOV)
        return GetTickCount() - (DWORD)g_di_reset_t < 200; /* v1.8.203 */
    if (!g_x_have)
        return 0;
    if (GetTickCount() - (DWORD)g_x_t > 1000)
        return 0; /* v1.8.195 : manette DirectInput : le bouton du profil n'a pas la même numérotation */
    if (g_reset_kind == PAD_RESET_POV)
        m = k_pov_mask[g_reset_code & 3];
    else if (g_reset_kind == PAD_RESET_BUTTON && g_reset_code >= 0 && g_reset_code < 10)
        m = k_btn_mask[g_reset_code];
    return m && (g_x_last.buttons & m) != 0;
}

int padinject_reset_vk(void)
{
    if (g_reset_kind != PAD_RESET_KEY)
        return 0;
    return (int)MapVirtualKeyA((UINT)(g_reset_code & 0x7F), 1 /* MAPVK_VSC_TO_VK */);
}

void padinject_inject_counts(long *di, long *x, long *fake)
{
    *di = InterlockedExchange(&g_inj_di, 0);
    *x = InterlockedExchange(&g_inj_x, 0);
    *fake = InterlockedExchange(&g_inj_fake, 0);
}

int padinject_pad_seen(void)
{
    return g_seen_pad != 0 || g_x_seen != 0;
}

#else

int padinject_install(PadLogFn log) { (void)log; return 0; }
void padinject_uninstall(void) {}
void padinject_pov_up(int on) { (void)on; }
void padinject_force_rt(int v) { (void)v; }
void padinject_force_buttons(unsigned short m) { (void)m; }
void padinject_press_extra(int kind, int code, int on) { (void)kind; (void)code; (void)on; }
void padinject_set_reset(int kind, int code) { (void)kind; (void)code; }
void padinject_block(int on) { (void)on; }
void padinject_block_reset(int on) { (void)on; }
void padinject_block_mask(unsigned short mask) { (void)mask; }
unsigned short padinject_reset_mask(void) { return 0; }
unsigned short padinject_game_mask(int kind, int code) { (void)kind; (void)code; return 0; }
void padinject_inject_counts(long *di, long *x, long *fake) { *di = *x = *fake = 0; }
int padinject_reset_held(void) { return 0; }
int padinject_reset_vk(void) { return 0; }
int padinject_pad_seen(void) { return 0; }
int padinject_read(PadState *out) { (void)out; return 0; }
int padinject_install_xinput(PadLogFn log) { (void)log; return 0; }

#endif

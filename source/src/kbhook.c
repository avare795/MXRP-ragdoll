/*
 * kbhook.c - clavier prioritaire pour le plugin (voir kbhook.h)
 *
 * Crochet clavier bas niveau (WH_KEYBOARD_LL) dans un fil à lui (il lui faut une boucle de
 * messages). Les touches du plugin, quand il les demande, n'arrivent plus au jeu ; le plugin
 * garde leur état lui-même. Les touches envoyées par programme (respawn du plugin) passent.
 */
#include "kbhook.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>

static HHOOK g_hook = NULL;
static HANDLE g_thread = NULL;
static DWORD g_thread_id = 0;
static volatile LONG g_on = 0;
static volatile unsigned char g_block[256];
static volatile unsigned char g_down[256];
static volatile int g_extra = 0; /* touche retenue en plus (respawn du jeu en marche forcée) */
static KbLogFn g_log = NULL;

static int our_window(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w)
        return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static LRESULT CALLBACK ll_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        DWORD vk = k->vkCode & 0xFF;
        int up = (k->flags & LLKHF_UP) != 0;
        int injected = (k->flags & LLKHF_INJECTED) != 0;
        /* Maj gauche / droite : le plugin lit VK_SHIFT */
        DWORD alias = (vk == VK_LSHIFT || vk == VK_RSHIFT) ? VK_SHIFT
                    : (vk == VK_LCONTROL || vk == VK_RCONTROL) ? VK_CONTROL : vk;
        if (!injected) {
            g_down[vk] = (unsigned char)!up;
            if (alias != vk)
                g_down[alias] = (unsigned char)!up;
        }
        if (!injected && ((g_on && (g_block[vk] || g_block[alias])) || (g_extra && (int)vk == g_extra)) && our_window())
            return 1; /* le jeu ne la voit pas */
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static DWORD WINAPI hook_thread(LPVOID arg)
{
    MSG m;
    (void)arg;
    g_hook = SetWindowsHookExA(WH_KEYBOARD_LL, ll_proc, GetModuleHandleA(NULL), 0);
    if (g_log)
        g_log("clavier : crochet %s", g_hook ? "installe (touches du plugin prioritaires a pied)" : "impossible");
    if (!g_hook)
        return 0;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    UnhookWindowsHookEx(g_hook);
    g_hook = NULL;
    return 0;
}

int kbhook_install(KbLogFn log)
{
    if (g_thread)
        return 1;
    g_log = log;
    memset((void *)g_block, 0, sizeof(g_block));
    memset((void *)g_down, 0, sizeof(g_down));
    g_thread = CreateThread(NULL, 0, hook_thread, NULL, 0, &g_thread_id);
    return g_thread != NULL;
}

void kbhook_uninstall(void)
{
    if (!g_thread)
        return;
    PostThreadMessageA(g_thread_id, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 1000);
    CloseHandle(g_thread);
    g_thread = NULL;
}

void kbhook_set_keys(const int *vks, int n)
{
    int i;
    unsigned char nb[256];
    memset(nb, 0, sizeof(nb));
    for (i = 0; i < n; ++i)
        if (vks[i] > 0 && vks[i] < 256 && vks[i] != VK_ESCAPE)
            nb[vks[i]] = 1;
    for (i = 0; i < 256; ++i)
        g_block[i] = nb[i];
}

void kbhook_block_extra(int vk)
{
    g_extra = vk;
}

void kbhook_block(int on)
{
    InterlockedExchange(&g_on, on ? 1 : 0);
}

int kbhook_down(int vk)
{
    return g_hook && vk > 0 && vk < 256 && g_down[vk];
}

#else
int kbhook_install(KbLogFn log) { (void)log; return 0; }
void kbhook_uninstall(void) {}
void kbhook_set_keys(const int *vks, int n) { (void)vks; (void)n; }
void kbhook_block(int on) { (void)on; }
void kbhook_block_extra(int vk) { (void)vk; }
int kbhook_down(int vk) { (void)vk; return 0; }
#endif

/*
 * camhook.c - interception du calcul de la caméra du jeu (beta21e)
 *
 * Trouvé avec le point d'arrêt matériel de la v0.9.5 : chaque image, la
 * matrice caméra -> monde est écrite par une fonction générique de produit de
 * matrices 4x4 (vers jeu+0x266180..0x2665A0), appelée depuis jeu+0xCFDD
 * (retour en +0xCFE2). Ses arguments : rcx = résultat, rdx = A, r8 = B
 * (r9 n'est que le registre de travail de la copie finale).
 *
 * On remplace la cible de ce CALL par un trampoline : le jeu calcule sa
 * caméra, puis notre fonction peut écrire la nôtre dans le résultat, avant
 * que le jeu s'en serve pour trier et dessiner les objets.
 */
#include "camhook.h"
#include "gamever.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef void *(*MatMulFn)(void *a, void *b, void *c, void *d);

static MatMulFn g_orig = NULL;
static volatile CamHookFn g_cb = NULL;
static uint8_t *g_site = NULL;
static int32_t g_orig_rel = 0;
static volatile LONG64 g_calls = 0;

static void *matmul_hook(void *a, void *b, void *c, void *d)
{
    void *r = g_orig(a, b, c, d);
    (void)d;
    CamHookFn cb = g_cb;
    InterlockedIncrement64(&g_calls);
    if (cb)
        cb((const float *)b, (const float *)c, (float *)a);
    return r;
}

static uint8_t *alloc_near(uint8_t *near_addr)
{
    SYSTEM_INFO si;
    uintptr_t gran, start, addr;
    const uintptr_t range = 0x70000000;
    GetSystemInfo(&si);
    gran = si.dwAllocationGranularity;
    start = ((uintptr_t)near_addr) & ~(gran - 1);
    for (addr = start; addr > gran && start - addr < range; addr -= gran) {
        void *p = VirtualAlloc((void *)addr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p)
            return (uint8_t *)p;
    }
    for (addr = start + gran; addr - start < range; addr += gran) {
        void *p = VirtualAlloc((void *)addr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p)
            return (uint8_t *)p;
    }
    return NULL;
}

static int write_rel32(uint8_t *site, int32_t rel)
{
    DWORD old, tmp;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(site + 1, &rel, sizeof(rel));
    VirtualProtect(site, 5, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 1;
}

int camhook_install(CamHookFn cb, CamHookLogFn log)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    uint8_t *site = base + gamever_get(NULL)->cam_site;
    uint8_t b[0x60], *tramp;
    uint64_t dest = (uint64_t)(uintptr_t)&matmul_hook;
    int32_t rel;
    uintptr_t target;
    SIZE_T got = 0;
    if (g_site)
        return 1;
    if (!ReadProcessMemory(GetCurrentProcess(), site - 0x40, b, sizeof(b), &got) || got != sizeof(b)) {
        if (log)
            log("camera : code illisible a jeu+0x%X", gamever_get(NULL)->cam_site);
        return 0;
    }
    if (log) {
        char line[260];
        size_t n = 0;
        int i;
        for (i = 0; i < (int)sizeof(b); ++i)
            n += (size_t)snprintf(line + n, sizeof(line) - n, "%02x%s", b[i], i == 0x3F ? " | " : "");
        log("camera : code jeu+0x%X : %s", gamever_get(NULL)->cam_site - 0x40, line);
    }
    memcpy(&rel, b + 0x41, 4);
    target = (uintptr_t)(site + 5 + rel) - (uintptr_t)base;
    if (b[0x40] != 0xE8 || target < gamever_get(NULL)->cam_min || target > gamever_get(NULL)->cam_max) {
        if (log)
            log("camera : jeu+0x%X n'est pas l'appel attendu (octet %02X, cible +0x%llX) : camera du jeu non interceptee",
                gamever_get(NULL)->cam_site, b[0x40], (unsigned long long)target);
        return 0;
    }
    tramp = alloc_near(site);
    if (!tramp)
        return 0;
    tramp[0] = 0x48;
    tramp[1] = 0xB8;
    memcpy(tramp + 2, &dest, sizeof(dest));
    tramp[10] = 0xFF;
    tramp[11] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), tramp, 12);
    g_orig = (MatMulFn)(void *)(base + target);
    g_orig_rel = rel;
    g_cb = cb;
    if (!write_rel32(site, (int32_t)((intptr_t)tramp - (intptr_t)(site + 5)))) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    g_site = site;
    if (log)
        log("camera : calcul de la camera du jeu intercepte (appel jeu+0x%X -> +0x%llX)", gamever_get(NULL)->cam_site,
            (unsigned long long)target);
    return 1;
}

void camhook_uninstall(void)
{
    if (!g_site)
        return;
    g_cb = NULL;
    write_rel32(g_site, g_orig_rel);
    g_site = NULL;
}

long long camhook_calls(void)
{
    return (long long)g_calls;
}

#else

int camhook_install(CamHookFn cb, CamHookLogFn log) { (void)cb; (void)log; return 0; }
void camhook_uninstall(void) {}
long long camhook_calls(void) { return 0; }

#endif

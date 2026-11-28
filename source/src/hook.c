/*
 * hook.c - interception de la mise à jour du squelette du pilote (Windows)
 */
#include "hook.h"
#include "gamever.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef uint64_t (*SkeletonFn)(void *a, void *b, void *c, void *d);

static SkeletonFn       g_original = NULL;
static SkeletonCallback g_callback = NULL;
static SkeletonCallback g_pre_callback = NULL;
static uint8_t         *g_site = NULL;
static int32_t          g_original_rel = 0;
static uint8_t         *g_trampoline = NULL;
static volatile LONG64  g_calls = 0;

int safe_read(const void *addr, void *out, size_t size)
{
    SIZE_T done = 0;
    if (!addr)
        return 0;
    if (!ReadProcessMemory(GetCurrentProcess(), addr, out, size, &done))
        return 0;
    return done == size;
}

int safe_write(void *addr, const void *data, size_t size)
{
    SIZE_T done = 0;
    if (!addr)
        return 0;
    if (!WriteProcessMemory(GetCurrentProcess(), addr, data, size, &done))
        return 0;
    return done == size;
}

uintptr_t hook_module_base(void)
{
    return (uintptr_t)GetModuleHandleA(NULL);
}

const char *hook_result_text(HookResult r)
{
    switch (r) {
    case HOOK_OK:               return "ok";
    case HOOK_ERR_NOT_CALL:     return "pas d'instruction CALL a cette adresse";
    case HOOK_ERR_WRONG_TARGET: return "le CALL vise une autre fonction (autre version du jeu ou autre mod actif)";
    case HOOK_ERR_READ:         return "memoire illisible";
    case HOOK_ERR_ALLOC:        return "impossible d'allouer le trampoline";
    case HOOK_ERR_PROTECT:      return "impossible de modifier le code du jeu";
    case HOOK_ERR_ALREADY:      return "deja installe";
    }
    return "?";
}

/* Notre remplaçant : appelle le jeu, puis nous passe le squelette */
static uint64_t skeleton_hook(void *a, void *b, void *c, void *d)
{
    uint64_t ret;
    if (g_pre_callback)
        g_pre_callback(a);
    ret = g_original(a, b, c, d);
    InterlockedIncrement64(&g_calls);
    if (g_callback)
        g_callback(a);
    return ret;
}

HookResult hook_check(char *info, size_t info_size)
{
    uint8_t *base = (uint8_t *)hook_module_base();
    uint8_t *site = base + gamever_get(NULL)->skel_site;
    uint8_t *expected = base + gamever_get(NULL)->skel_target;
    uint8_t bytes[5];
    int32_t rel;
    uint8_t *target;

    if (!gamever_get(NULL)->skel_site || !safe_read(site, bytes, sizeof(bytes))) {
        snprintf(info, info_size, "base %p, lecture impossible a +0x%X", (void *)base,
                 gamever_get(NULL)->skel_site);
        return HOOK_ERR_READ;
    }
    memcpy(&rel, bytes + 1, sizeof(rel));
    target = site + 5 + rel;
    snprintf(info, info_size,
             "base %p, +0x%X : %02X %02X %02X %02X %02X -> cible +0x%llX (attendu +0x%X)",
             (void *)base, gamever_get(NULL)->skel_site, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
             (unsigned long long)(target - base), gamever_get(NULL)->skel_target);
    if (bytes[0] != 0xE8)
        return HOOK_ERR_NOT_CALL;
    if (target != expected)
        return HOOK_ERR_WRONG_TARGET;
    return HOOK_OK;
}

/* Alloue une page exécutable à moins de 2 Go de l'adresse (portée d'un CALL relatif) */
static uint8_t *alloc_near(uint8_t *near_addr)
{
    SYSTEM_INFO si;
    uintptr_t gran, start, addr;
    const uintptr_t range = 0x70000000; /* ~1,75 Go */

    GetSystemInfo(&si);
    gran = si.dwAllocationGranularity;
    start = ((uintptr_t)near_addr) & ~(gran - 1);

    for (addr = start; addr > gran && start - addr < range; addr -= gran) {
        void *p = VirtualAlloc((void *)addr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
        if (p)
            return (uint8_t *)p;
    }
    for (addr = start + gran; addr - start < range; addr += gran) {
        void *p = VirtualAlloc((void *)addr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
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

HookResult hook_install(SkeletonCallback cb, char *info, size_t info_size)
{
    uint8_t *base = (uint8_t *)hook_module_base();
    uint8_t *site = base + gamever_get(NULL)->skel_site;
    uint64_t dest = (uint64_t)(uintptr_t)&skeleton_hook;
    HookResult r;
    intptr_t rel;

    if (g_site)
        return HOOK_ERR_ALREADY;

    r = hook_check(info, info_size);
    if (r != HOOK_OK)
        return r;

    g_trampoline = alloc_near(site);
    if (!g_trampoline)
        return HOOK_ERR_ALLOC;

    /* mov rax, skeleton_hook ; jmp rax */
    g_trampoline[0] = 0x48;
    g_trampoline[1] = 0xB8;
    memcpy(g_trampoline + 2, &dest, sizeof(dest));
    g_trampoline[10] = 0xFF;
    g_trampoline[11] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), g_trampoline, 12);

    memcpy(&g_original_rel, site + 1, sizeof(g_original_rel));
    g_original = (SkeletonFn)(void *)(base + gamever_get(NULL)->skel_target);
    g_callback = cb;

    rel = (intptr_t)g_trampoline - (intptr_t)(site + 5);
    if (!write_rel32(site, (int32_t)rel)) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = NULL;
        return HOOK_ERR_PROTECT;
    }
    g_site = site;
    return HOOK_OK;
}

void hook_uninstall(void)
{
    if (!g_site)
        return;
    g_callback = NULL;
    g_pre_callback = NULL;
    write_rel32(g_site, g_original_rel);
    g_site = NULL;
    /* Le trampoline n'est pas libéré : un autre thread pourrait encore être dedans. */
}

void hook_set_pre_callback(SkeletonCallback cb)
{
    g_pre_callback = cb;
}

int hook_installed(void)
{
    return g_site != NULL;
}

uint64_t hook_call_count(void)
{
    return (uint64_t)g_calls;
}

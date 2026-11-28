/*
 * watch.c - diagnostic : qui écrit la caméra du jeu ?
 *
 * Pose un point d'arrêt matériel en écriture (registres de débogage du
 * processeur, DR0/DR7) sur une adresse, dans le fil de rendu du jeu. Un
 * gestionnaire d'exceptions note l'instruction qui écrit (RIP) et les adresses
 * de retour sur la pile, puis laisse le jeu continuer. Aucune modification du
 * code du jeu.
 */
#include "watch.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_SITES 8
#define STACK_QW 48

typedef struct {
    uintptr_t rip;
    int hits;
    uintptr_t stack[STACK_QW];
} Site;

static Site g_sites[MAX_SITES];
static volatile LONG g_num_sites = 0;
static volatile LONG g_total_hits = 0;
static volatile int g_armed = 0;
static PVOID g_veh = NULL;
static uintptr_t g_img_lo, g_img_hi;
static DWORD g_tid = 0;
static uintptr_t g_addr = 0;

static LONG CALLBACK veh(PEXCEPTION_POINTERS ep)
{
    PCONTEXT c = ep->ContextRecord;
    int i, n;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    InterlockedIncrement(&g_total_hits);
    n = g_num_sites;
    for (i = 0; i < n; ++i)
        if (g_sites[i].rip == (uintptr_t)c->Rip) {
            g_sites[i].hits++;
            break;
        }
    if (i == n && n < MAX_SITES) {
        Site *s = &g_sites[n];
        s->rip = (uintptr_t)c->Rip;
        s->hits = 1;
        memcpy(s->stack, (const void *)c->Rsp, sizeof(s->stack));
        InterlockedExchange(&g_num_sites, n + 1);
    }
    if (g_total_hits >= 2000 || !g_armed) {
        c->Dr7 &= ~(uintptr_t)0xF0003; /* désarme DR0 */
        g_armed = 0;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI arm_thread(LPVOID arg)
{
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_tid);
    CONTEXT c;
    (void)arg;
    if (!t)
        return 0;
    if (SuspendThread(t) != (DWORD)-1) {
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = g_addr;
            c.Dr7 = (c.Dr7 & ~(uintptr_t)0xF0003) | 1 | (1u << 16) | (3u << 18); /* L0, écriture, 4 octets */
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            SetThreadContext(t, &c);
        }
        ResumeThread(t);
    }
    CloseHandle(t);
    return 0;
}

int watch_start(uintptr_t addr, unsigned long thread_id)
{
    HANDLE th;
    uint8_t *img = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    if (g_armed || !addr || !thread_id || (addr & 3))
        return 0;
    g_img_lo = (uintptr_t)img;
    g_img_hi = g_img_lo + nt->OptionalHeader.SizeOfImage;
    if (!g_veh)
        g_veh = AddVectoredExceptionHandler(1, veh);
    if (!g_veh)
        return 0;
    g_addr = addr;
    g_tid = thread_id;
    g_num_sites = 0;
    g_total_hits = 0;
    g_armed = 1;
    th = CreateThread(NULL, 0, arm_thread, NULL, 0, NULL);
    if (!th) {
        g_armed = 0;
        return 0;
    }
    CloseHandle(th);
    return 1;
}

void watch_stop(void)
{
    g_armed = 0; /* le gestionnaire désarme au prochain passage */
}

int watch_hits(void)
{
    return g_total_hits;
}

static void hex_dump(WatchLogFn log, uintptr_t from, size_t len)
{
    uint8_t b[32];
    size_t off;
    for (off = 0; off < len; off += 32) {
        char line[120];
        size_t n = 0;
        int i;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void *)(from + off), b, sizeof(b), &got) || got != sizeof(b))
            break;
        for (i = 0; i < 32; ++i)
            n += (size_t)snprintf(line + n, sizeof(line) - n, "%02x", b[i]);
        log("    code +0x%06llx %s", (unsigned long long)(from + off - g_img_lo), line);
    }
}

void watch_report(WatchLogFn log)
{
    int i, j, n = g_num_sites;
    if (!log)
        return;
    log("camera : %ld ecriture(s) de la camera du jeu, %d endroit(s) du code", (long)g_total_hits, n);
    for (i = 0; i < n; ++i) {
        const Site *s = &g_sites[i];
        char line[600];
        size_t k = 0;
        int in_img = s->rip >= g_img_lo && s->rip < g_img_hi;
        for (j = 0; j < STACK_QW && k + 24 < sizeof(line); ++j)
            if (s->stack[j] >= g_img_lo && s->stack[j] < g_img_hi)
                k += (size_t)snprintf(line + k, sizeof(line) - k, " [%d]+0x%llx", j,
                                      (unsigned long long)(s->stack[j] - g_img_lo));
        line[k] = '\0';
        if (in_img)
            log("  apres l'instruction a jeu+0x%llx : %d fois ; pile :%s",
                (unsigned long long)(s->rip - g_img_lo), s->hits, line);
        else
            log("  apres l'instruction a %p (hors du jeu) : %d fois ; pile :%s", (void *)s->rip, s->hits, line);
        if (in_img)
            hex_dump(log, (s->rip & ~(uintptr_t)31) - 0x400, 0x500);
    }
}

#else

int watch_start(uintptr_t addr, unsigned long thread_id) { (void)addr; (void)thread_id; return 0; }
void watch_stop(void) {}
int watch_hits(void) { return 0; }
void watch_report(WatchLogFn log) { (void)log; }

#endif

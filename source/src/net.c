/*
 * net.c - voir net.h
 */
#include "net.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#define NET_MAGIC 0x4452584Du /* "MXRD" */
#define NET_VER 1
#define K_HELLO 0
#define K_POSE 1
#define K_BLOB 2 /* v1.8.65 : données du plugin (cônes...) : Head, puis 1 octet de type, puis les données */
#define RING 16
#define MAX_CLIENTS 16
#define DELAY 0.10      /* retard de l'affichage, s : laisse le temps aux paquets d'arriver */
#define TIMEOUT 0.6     /* plus rien reçu : le pilote est rendu au jeu */

typedef struct {
    uint32_t magic;
    uint8_t ver, kind, pad[2];
    uint32_t id;
    uint32_t seq;
    double t;           /* temps de l'envoyeur, s */
    char name[32];
} Head;

typedef struct {
    Head h;
    float mw[12];
    float pos[RD_NUM_POINTS][3];
    DriveSnap snap;
} Pose;

typedef struct {
    double t;
    float mw[12];
    float pos[RD_NUM_POINTS][3];
    DriveSnap snap;
} Frame;

typedef struct {
    uint32_t id;
    char name[32];
    double last_recv;   /* temps local de la dernière pose reçue */
    double offset;      /* temps local - temps de l'envoyeur (le plus petit vu = latence la plus faible) */
    int have_offset;
    uint32_t last_seq;
    Frame ring[RING];
    int n, head;        /* poses gardées ; head = prochaine place */
    int logged;
} Peer;

typedef struct {
    struct sockaddr_in addr;
    double seen;
} Client;

static SOCKET g_sock = INVALID_SOCKET;
static HANDLE g_thread = NULL;
static volatile int g_run = 0;
static int g_host = 0;
static int g_auto = 0;  /* 1 = sans hôte : chaque plugin se signale sur les réseaux du PC (Radmin...) */
static volatile int g_gchan_req; /* (plus bas) 1 = sur un serveur RAGDOLL */
/* v1.8.201 : réseau local automatique (sans hôte réglé à la main) : seulement sur un serveur RAGDOLL.
   Ailleurs (solo, autres serveurs) le plugin ne se signale plus et ignore les autres plugins du réseau.
   Un hôte ou une IP réglés à la main dans le .ini restent comme avant (choix du joueur). */
#define LAN_OFF (g_auto && !g_gchan_req)
static struct sockaddr_in g_bcast[16];
static int g_nbcast = 0;
static int g_port = 47731;
static struct sockaddr_in g_host_addr;
static NetLogFn g_log = NULL;
static CRITICAL_SECTION g_cs;
static int g_cs_ok = 0;
static Peer g_peers[NET_MAX_PEERS];
static Client g_clients[MAX_CLIENTS];
static uint32_t g_id = 0, g_seq = 0;
static char g_name[32] = "?";
static double g_last_send = 0.0;
static LARGE_INTEGER g_freq;

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g_freq.QuadPart;
}

static void head_fill(Head *h, int kind)
{
    memset(h, 0, sizeof(*h));
    h->magic = NET_MAGIC;
    h->ver = NET_VER;
    h->kind = (uint8_t)kind;
    h->id = g_id;
    h->seq = ++g_seq;
    h->t = now_s();
    memcpy(h->name, g_name, sizeof(h->name));
}

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* hôte : envoie à tous les joueurs connus, sauf except */
static void host_broadcast(const void *buf, int len, const struct sockaddr_in *except)
{
    int i;
    double now = now_s();
    if (LAN_OFF)
        return; /* v1.8.201 */
    for (i = 0; i < MAX_CLIENTS; ++i) {
        Client *c = &g_clients[i];
        if (c->seen == 0.0 || now - c->seen > 10.0)
            continue;
        if (except && same_addr(&c->addr, except))
            continue;
        sendto(g_sock, (const char *)buf, len, 0, (const struct sockaddr *)&c->addr, sizeof(c->addr));
    }
}

static void host_note_client(const struct sockaddr_in *from)
{
    int i, free_i = -1, old_i = 0;
    double now = now_s();
    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (g_clients[i].seen != 0.0 && same_addr(&g_clients[i].addr, from)) {
            g_clients[i].seen = now;
            return;
        }
        if (free_i < 0 && (g_clients[i].seen == 0.0 || now - g_clients[i].seen > 10.0))
            free_i = i;
        if (g_clients[i].seen < g_clients[old_i].seen)
            old_i = i;
    }
    if (free_i < 0)
        free_i = old_i;
    g_clients[free_i].addr = *from;
    g_clients[free_i].seen = now;
    if (g_log) {
        char ip[64];
        inet_ntop(AF_INET, (void *)&from->sin_addr, ip, sizeof(ip));
        g_log("reseau : %s %s:%d", g_auto ? "plugin d'un autre joueur trouve sur" : "joueur connecte depuis", ip,
              ntohs(from->sin_port));
    }
}

/* v1.2 : noms des joueurs dont le plugin s'est signalé (n'importe quel paquet) */
#define MAX_SEEN 32
static struct { char name[32]; double t; } g_seen[MAX_SEEN];

static void note_seen(const char *name, double now)
{
    int i, slot = -1, old = 0;
    char nm[32];
    memcpy(nm, name, 31);
    nm[31] = '\0';
    if (!nm[0] || !g_cs_ok)
        return;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < MAX_SEEN; ++i) {
        if (!strcmp(g_seen[i].name, nm)) {
            slot = i;
            break;
        }
        if (g_seen[i].t < g_seen[old].t)
            old = i;
    }
    if (slot < 0) {
        slot = old;
        memcpy(g_seen[slot].name, nm, sizeof(nm));
        if (g_log)
            g_log("reseau : plugin de \"%s\" recu", nm); /* v1.6.2 : diagnostic */
    }
    g_seen[slot].t = now;
    LeaveCriticalSection(&g_cs);
}

int net_name_seen(const char *name, double max_age)
{
    int i, r = 0;
    double now = now_s();
    if (!g_run || !g_cs_ok || !name || !name[0])
        return 0;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < MAX_SEEN; ++i)
        if (g_seen[i].name[0] && !strncmp(g_seen[i].name, name, 31) && now - g_seen[i].t < max_age) {
            r = 1;
            break;
        }
    LeaveCriticalSection(&g_cs);
    return r;
}

static void store_pose(const Pose *p, double now)
{
    int i, slot = -1, old = 0;
    Peer *pe;
    Frame *f;
    if (p->h.id == g_id)
        return;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < NET_MAX_PEERS; ++i)
        if (g_peers[i].id == p->h.id) {
            slot = i;
            break;
        }
    if (slot < 0) {
        for (i = 0; i < NET_MAX_PEERS; ++i) {
            if (g_peers[i].id == 0 || now - g_peers[i].last_recv > 30.0) {
                slot = i;
                break;
            }
            if (g_peers[i].last_recv < g_peers[old].last_recv)
                old = i;
        }
        if (slot < 0)
            slot = old;
        memset(&g_peers[slot], 0, sizeof(g_peers[slot]));
        g_peers[slot].id = p->h.id;
    }
    pe = &g_peers[slot];
    memcpy(pe->name, p->h.name, sizeof(pe->name));
    pe->name[31] = '\0';
    if (pe->n > 0 && p->h.seq <= pe->last_seq && pe->last_seq - p->h.seq < 1000) {
        LeaveCriticalSection(&g_cs); /* paquet en retard ou en double */
        return;
    }
    /* nouvelle chute après un silence : on repart de zéro */
    if (now - pe->last_recv > TIMEOUT)
        pe->n = 0;
    pe->last_seq = p->h.seq;
    if (!pe->have_offset || now - p->h.t < pe->offset || pe->n == 0) {
        pe->offset = now - p->h.t;
        pe->have_offset = 1;
    } else {
        pe->offset += 0.0005; /* les horloges dérivent : on laisse le minimum remonter doucement */
    }
    pe->last_recv = now;
    f = &pe->ring[pe->head];
    f->t = p->h.t;
    memcpy(f->mw, p->mw, sizeof(f->mw));
    memcpy(f->pos, p->pos, sizeof(f->pos));
    f->snap = p->snap;
    pe->head = (pe->head + 1) % RING;
    if (pe->n < RING)
        ++pe->n;
    if (!pe->logged && g_log) {
        pe->logged = 1;
        g_log("reseau : %s est a pied (ou tombe) avec le plugin", pe->name);
    }
    LeaveCriticalSection(&g_cs);
}

/* adresses de diffusion de chaque réseau du PC (Radmin VPN : 26.255.255.255, box : 192.168.x.255...) */
static void find_broadcasts(void)
{
    ULONG sz = 16384;
    IP_ADAPTER_ADDRESSES *aa = (IP_ADAPTER_ADDRESSES *)malloc(sz), *a;
    g_nbcast = 0;
    if (!aa)
        return;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa,
                             &sz) != NO_ERROR) {
        free(aa);
        return;
    }
    for (a = aa; a && g_nbcast < 16; a = a->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (u = a->FirstUnicastAddress; u && g_nbcast < 16; u = u->Next) {
            struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
            ULONG pl = u->OnLinkPrefixLength, ip, mask;
            if (!sin || sin->sin_family != AF_INET || pl == 0 || pl > 30)
                continue;
            ip = ntohl(sin->sin_addr.s_addr);
            mask = pl >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> pl);
            memset(&g_bcast[g_nbcast], 0, sizeof(g_bcast[0]));
            g_bcast[g_nbcast].sin_family = AF_INET;
            g_bcast[g_nbcast].sin_port = htons((u_short)g_port);
            g_bcast[g_nbcast].sin_addr.s_addr = htonl(ip | ~mask);
            ++g_nbcast;
        }
    }
    free(aa);
}


/* ---------- v1.3 : canal du jeu (port du serveur MX Bikes, 54210 par défaut) ----------
   Les paquets du plugin passent par la connexion du jeu : aucun autre port à ouvrir, pas
   besoin de Radmin. sendto/recvfrom du jeu sont interceptés (table d'imports de mxbikes.exe) :
   - à l'envoi, on note la connexion du jeu et ses correspondants (le serveur, ou les joueurs
     si on héberge) ;
   - à la réception, les paquets « MXRD » sont retirés avant que le jeu les voie.
   Seulement sur un serveur RAGDOLL (net_game_channel). */
typedef int(WSAAPI *PFN_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int(WSAAPI *PFN_recvfrom)(SOCKET, char *, int, int, struct sockaddr *, int *);
static PFN_sendto g_real_sendto = NULL;
static PFN_recvfrom g_real_recvfrom = NULL;
static volatile SOCKET g_gsock = INVALID_SOCKET;
static volatile int g_gchan = 0; /* 1 : envoyer par le canal du jeu */
/* v1.7.19 : diagnostic du canal du jeu (liste « Sans plugin » vide de vrais plugins en ligne) */
static volatile LONG g_gc_sent = 0, g_gc_recv = 0, g_gc_game = 0;
#define MAX_GEP 24
/* v1.7.24 : adresses IPv4 ET IPv6. En ligne, le jeu peut parler en IPv6 (getaddrinfo/inet_pton) :
   pas de NAT en IPv6, donc pas de port à ouvrir. Avant, seuls les paquets IPv4 étaient vus. */
typedef union { struct sockaddr sa; struct sockaddr_in v4; struct sockaddr_in6 v6; } GAddr;
static int gaddr_ok(const struct sockaddr *a, int len)
{
    return a && ((a->sa_family == AF_INET && len >= (int)sizeof(struct sockaddr_in)) ||
                 (a->sa_family == AF_INET6 && len >= (int)sizeof(struct sockaddr_in6)));
}
static int gaddr_len(const GAddr *a) { return a->sa.sa_family == AF_INET6 ? (int)sizeof(struct sockaddr_in6) : (int)sizeof(struct sockaddr_in); }
static void gaddr_set(GAddr *d, const struct sockaddr *a)
{
    memset(d, 0, sizeof(*d));
    memcpy(d, a, a->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
}
static int gaddr_eq(const GAddr *a, const GAddr *b)
{
    if (a->sa.sa_family != b->sa.sa_family)
        return 0;
    if (a->sa.sa_family == AF_INET6)
        return a->v6.sin6_port == b->v6.sin6_port && !memcmp(&a->v6.sin6_addr, &b->v6.sin6_addr, 16);
    return a->v4.sin_port == b->v4.sin_port && a->v4.sin_addr.s_addr == b->v4.sin_addr.s_addr;
}
static void gaddr_fmt(const GAddr *a, char *out, int n)
{
    char ip[64] = "?";
    if (a->sa.sa_family == AF_INET6) {
        inet_ntop(AF_INET6, (void *)&a->v6.sin6_addr, ip, sizeof(ip));
        snprintf(out, (size_t)n, "[%s]:%u (IPv6)", ip, (unsigned)ntohs(a->v6.sin6_port));
    } else {
        inet_ntop(AF_INET, (void *)&a->v4.sin_addr, ip, sizeof(ip));
        snprintf(out, (size_t)n, "%s:%u", ip, (unsigned)ntohs(a->v4.sin_port));
    }
}
static volatile LONG g_gc_all = 0, g_gc_v6 = 0;
typedef struct { GAddr a; double sent, plug; SOCKET s; int logged; long n; } GEp; /* v1.6.2 : socket du jeu propre à ce correspondant */
static GEp g_gep[MAX_GEP];
#define GQ 64
static struct { GAddr from; int n; char buf[sizeof(Pose) > 1400 ? sizeof(Pose) : 1400]; } g_gq[GQ];
static volatile LONG g_gq_w = 0, g_gq_r = 0;
static CRITICAL_SECTION g_gcs;
static int g_gcs_ok = 0;

static GEp *gep_find(const GAddr *a, int create)
{
    int i, old = 0;
    for (i = 0; i < MAX_GEP; ++i) {
        if ((g_gep[i].sent || g_gep[i].plug) && gaddr_eq(&g_gep[i].a, a))
            return &g_gep[i];
        if (g_gep[i].sent + g_gep[i].plug < g_gep[old].sent + g_gep[old].plug)
            old = i;
    }
    if (!create)
        return NULL;
    memset(&g_gep[old], 0, sizeof(g_gep[old]));
    g_gep[old].a = *a;
    return &g_gep[old];
}

static int WSAAPI hk_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    if (s != g_sock && g_gcs_ok && InterlockedIncrement(&g_gc_all) == 1 && g_log)
        g_log("reseau : premier envoi du jeu par sendto : famille %d, %d octets", to ? (int)to->sa_family : -1, len);
    if (gaddr_ok(to, tolen) && s != g_sock && g_gcs_ok &&
        !(len >= 4 && *(const uint32_t *)buf == NET_MAGIC)) {
        GEp *e;
        GAddr ga;
        gaddr_set(&ga, to);
        g_gsock = s;
        InterlockedIncrement(&g_gc_game);
        if (to->sa_family == AF_INET6)
            InterlockedIncrement(&g_gc_v6);
        EnterCriticalSection(&g_gcs);
        e = gep_find(&ga, 1);
        e->sent = now_s();
        e->n++;
        e->s = s;
        LeaveCriticalSection(&g_gcs);
    }
    return g_real_sendto(s, buf, len, flags, to, tolen);
}

static int WSAAPI hk_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    for (;;) {
        GAddr tmp;
        int tl = sizeof(tmp);
        struct sockaddr *f = from ? from : (struct sockaddr *)&tmp;
        int *fl = from ? fromlen : &tl;
        int r = g_real_recvfrom(s, buf, len, flags, f, fl);
        if (r < (int)sizeof(Head) || (flags & MSG_PEEK) || s == g_sock || *(const uint32_t *)buf != NET_MAGIC ||
            !gaddr_ok(f, *fl))
            return r;
        /* paquet du plugin : pour nous, pas pour le jeu */
        if (InterlockedIncrement(&g_gc_recv) == 1 && g_log) {
            GAddr ga;
            char t[96];
            gaddr_set(&ga, f);
            gaddr_fmt(&ga, t, sizeof(t));
            g_log("reseau : premier paquet d'un autre plugin recu par le canal du jeu (%s)", t);
        }
        if (g_gcs_ok && r <= (int)sizeof(g_gq[0].buf)) {
            LONG w = g_gq_w;
            if (w - g_gq_r < GQ) {
                int k = (int)(w % GQ);
                gaddr_set(&g_gq[k].from, f);
                g_gq[k].n = r;
                memcpy(g_gq[k].buf, buf, (size_t)r);
                InterlockedIncrement(&g_gq_w);
            }
            EnterCriticalSection(&g_gcs);
            {
                GAddr ga;
                GEp *e;
                gaddr_set(&ga, f);
                e = gep_find(&ga, 1);
                e->plug = now_s();
                e->s = s; /* répondre par la socket qui a reçu : celle que la box connaît */
            }
            LeaveCriticalSection(&g_gcs);
        }
        /* on lit le paquet suivant (socket non bloquante : « rien » est rendu tel quel) */
    }
}

static int iat_hook(const char *dll_name, const char *fn_name, void *hook, void **orig)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        IMAGE_THUNK_DATA64 *names, *slots;
        if (_stricmp((const char *)(base + imp->Name), dll_name) || !imp->OriginalFirstThunk || !imp->FirstThunk)
            continue;
        names = (IMAGE_THUNK_DATA64 *)(base + imp->OriginalFirstThunk);
        slots = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            void **slot = (void **)&slots->u1.Function;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) ||
                strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, fn_name))
                continue;
            *orig = *slot;
            if (!*orig || !VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
                return 0;
            InterlockedExchangePointer(slot, hook);
            VirtualProtect(slot, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

/* v1.3.1 : par l'adresse de la fonction : les noms des imports de mxbikes.exe peuvent manquer
   (protection Steam), mais la case de la table contient l'adresse réelle de sendto/recvfrom */
static int iat_hook_by_addr(void *target, void *hook, void **orig)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int n = 0;
    if (!base || !target || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        void **slot;
        if (!imp->FirstThunk)
            continue;
        for (slot = (void **)(base + imp->FirstThunk); *slot; ++slot) {
            DWORD old;
            if (*slot != target)
                continue;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
                continue;
            *orig = target;
            InterlockedExchangePointer(slot, hook);
            VirtualProtect(slot, sizeof(void *), old, &old);
            ++n;
        }
    }
    return n;
}

/* diagnostic : la table d'imports de ws2_32 dans mxbikes.exe */
static void iat_diag(NetLogFn log)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    HMODULE ws = GetModuleHandleA("ws2_32.dll");
    void *st = ws ? (void *)GetProcAddress(ws, "sendto") : NULL, *rf = ws ? (void *)GetProcAddress(ws, "recvfrom") : NULL;
    if (!log || !dir.VirtualAddress)
        return;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        const char *dll = (const char *)(base + imp->Name);
        void **slot;
        int n = 0, ws_n = 0, other = 0;
        char who[260] = "";
        if (_stricmp(dll, "WS2_32.dll"))
            continue;
        for (slot = (void **)(base + imp->FirstThunk); imp->FirstThunk && *slot && n < 64; ++slot, ++n) {
            HMODULE m = NULL;
            if (*slot == st || *slot == rf)
                ++ws_n;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)*slot, &m) && m != ws) {
                ++other;
                if (!who[0])
                    GetModuleFileNameA(m, who, sizeof(who));
            }
        }
        log("reseau : imports ws2_32 du jeu : noms %s, %d cases, sendto/recvfrom trouves %d, %d cases vers un autre module (%s)",
            imp->OriginalFirstThunk ? "presents" : "absents", n, ws_n, other, who[0] ? who : "-");
    }
}

int net_game_hooks_install(NetLogFn log)
{
    static int done = 0, tries = 0;
    int a, b;
    HMODULE ws;
    if (done)
        return 1;
    if (!g_gcs_ok) {
        QueryPerformanceFrequency(&g_freq);
        InitializeCriticalSection(&g_gcs);
    }
    ++tries;
    a = g_real_sendto ? 1 : iat_hook("WS2_32.dll", "sendto", (void *)hk_sendto, (void **)&g_real_sendto);
    b = g_real_recvfrom ? 1 : iat_hook("WS2_32.dll", "recvfrom", (void *)hk_recvfrom, (void **)&g_real_recvfrom);
    ws = GetModuleHandleA("ws2_32.dll");
    if (ws && !a)
        a = iat_hook_by_addr((void *)GetProcAddress(ws, "sendto"), (void *)hk_sendto, (void **)&g_real_sendto);
    if (ws && !b)
        b = iat_hook_by_addr((void *)GetProcAddress(ws, "recvfrom"), (void *)hk_recvfrom, (void **)&g_real_recvfrom);
    g_gcs_ok = 1;
    done = a && b;
    if (!done && tries == 1)
        iat_diag(log);
    if (log && (done || tries == 1))
        log("reseau : canal du jeu %s (envoi %d, reception %d, essai %d)", done ? "pret" : "pas encore pret", a, b, tries);
    return done;
}

/* ---- v1.7.22 : canal Steam ----
   En ligne « normal » (serveur rejoint par Steam), le jeu ne passe pas par des sockets UDP mais par
   ISteamNetworking (SteamNetworking005 : paquets P2P relayés par Steam, sans port à ouvrir). Vu en
   jeu : 16 paquets UDP du jeu sur toute la partie, aucun paquet du plugin. On apprend à qui le jeu
   envoie (SendP2PPacket) et on envoie nos paquets aux mêmes joueurs sur un canal à nous, que le jeu
   ne lit pas. */
#define ST_CH 37
#define MAX_SP 24
typedef unsigned char(__cdecl *StSend_t)(void *self, uint64_t id, const void *data, uint32_t n, int type, int ch);
typedef unsigned char(__cdecl *StAvail_t)(void *self, uint32_t *sz, int ch);
typedef unsigned char(__cdecl *StRead_t)(void *self, void *dst, uint32_t cap, uint32_t *sz, uint64_t *id, int ch);
static void *volatile g_st_obj = NULL;
static StSend_t g_st_send = NULL;
static StAvail_t g_st_avail = NULL;
static StRead_t g_st_read = NULL;
static struct { uint64_t id; double sent, plug; int logged; } g_sp[MAX_SP];
static volatile LONG g_st_game = 0, g_st_sent = 0, g_st_recv = 0;
static volatile int g_gchan_req = 0; /* (voir LAN_OFF) */

static int sp_find(uint64_t id, int create)
{
    int i, old = 0;
    for (i = 0; i < MAX_SP; ++i) {
        if (g_sp[i].id == id && id)
            return i;
        if (g_sp[i].sent + g_sp[i].plug < g_sp[old].sent + g_sp[old].plug)
            old = i;
    }
    if (!create)
        return -1;
    memset(&g_sp[old], 0, sizeof(g_sp[old]));
    g_sp[old].id = id;
    return old;
}

static unsigned char __cdecl hk_st_send(void *self, uint64_t id, const void *data, uint32_t n, int type, int ch)
{
    if (ch != ST_CH && g_gcs_ok && id) {
        int k;
        InterlockedIncrement(&g_st_game);
        EnterCriticalSection(&g_gcs);
        k = sp_find(id, 1);
        g_sp[k].sent = now_s();
        LeaveCriticalSection(&g_gcs);
    }
    return g_st_send(self, id, data, n, type, ch);
}

/* à appeler quand le jeu a démarré Steam (au début d'une session) */
int net_steam_install(NetLogFn log)
{
    HMODULE sa;
    void *(*client)(void);
    int (*huser)(void);
    int (*hpipe)(void);
    void *(*getnet)(void *, int, int, const char *);
    void *obj, **vt;
    DWORD old;
    if (g_st_obj)
        return 1;
    if (!g_gcs_ok) {
        InitializeCriticalSection(&g_gcs);
        g_gcs_ok = 1;
    }
    sa = GetModuleHandleA("steam_api64.dll");
    if (!sa)
        return 0;
    client = (void *(*)(void))GetProcAddress(sa, "SteamClient");
    huser = (int (*)(void))GetProcAddress(sa, "SteamAPI_GetHSteamUser");
    hpipe = (int (*)(void))GetProcAddress(sa, "SteamAPI_GetHSteamPipe");
    getnet = (void *(*)(void *, int, int, const char *))GetProcAddress(sa, "SteamAPI_ISteamClient_GetISteamNetworking");
    if (!client || !huser || !hpipe || !getnet || !client() || !hpipe())
        return 0;
    obj = getnet(client(), huser(), hpipe(), "SteamNetworking005");
    if (!obj)
        obj = getnet(client(), huser(), hpipe(), "SteamNetworking006");
    if (!obj)
        return 0;
    vt = *(void ***)obj;
    g_st_avail = (StAvail_t)vt[1];
    g_st_read = (StRead_t)vt[2];
    if (vt[0] != (void *)hk_st_send) {
        if (!VirtualProtect(&vt[0], sizeof(void *), PAGE_READWRITE, &old))
            return 0;
        g_st_send = (StSend_t)vt[0];
        InterlockedExchangePointer(&vt[0], (void *)hk_st_send);
        VirtualProtect(&vt[0], sizeof(void *), old, &old);
    }
    g_st_obj = obj;
    if (log)
        log("reseau : canal Steam pret (ISteamNetworking, canal %d du plugin)", ST_CH);
    return 1;
}

static void steam_send(const void *buf, int len, int only_plug, uint64_t except)
{
    int i;
    double now;
    void *obj = g_st_obj;
    if (!g_gchan_req || !obj || !g_st_send)
        return;
    now = now_s();
    EnterCriticalSection(&g_gcs);
    for (i = 0; i < MAX_SP; ++i) {
        int active, plug;
        unsigned char ok;
        if (!g_sp[i].id || g_sp[i].id == except)
            continue;
        active = now - g_sp[i].sent < 10.0;
        plug = now - g_sp[i].plug < 10.0;
        if (!(only_plug ? plug && (active || now - g_sp[i].plug < 3.0) : active || plug))
            continue;
        ok = g_st_send(obj, g_sp[i].id, buf, (uint32_t)len, len <= 1100 ? 0 : 2, ST_CH);
        if (ok)
            InterlockedIncrement(&g_st_sent);
        if (!g_sp[i].logged && g_log) {
            g_sp[i].logged = 1;
            g_log("reseau : canal Steam : plugin envoye au joueur Steam %llu (%s)", (unsigned long long)g_sp[i].id, ok ? "ok" : "refuse");
        }
    }
    LeaveCriticalSection(&g_gcs);
}

static void note_seen(const char *name, double now);
static void store_pose(const Pose *p, double now);
static volatile NetBlobFn g_blob_fn = NULL;
static void got_blob(const char *buf, int n)
{
    const Head *h = (const Head *)buf;
    NetBlobFn fn = g_blob_fn;
    {
        /* v1.8.67 : un même paquet arrive par plusieurs chemins (direct, canal du jeu, Steam, renvoyé
           par un autre joueur) : traité une seule fois (sinon un cône donné était créé 2 ou 3 fois) */
        static uint32_t seen_id[128], seen_seq[128];
        static int seen_w = 0;
        static CRITICAL_SECTION cs;
        static LONG cs_init = 0;
        int i, dup = 0;
        if (InterlockedCompareExchange(&cs_init, 1, 0) == 0)
            InitializeCriticalSection(&cs), cs_init = 2;
        while (cs_init != 2)
            Sleep(0);
        EnterCriticalSection(&cs);
        for (i = 0; i < 128; ++i)
            if (seen_id[i] == h->id && seen_seq[i] == h->seq) {
                dup = 1;
                break;
            }
        if (!dup) {
            seen_id[seen_w] = h->id;
            seen_seq[seen_w] = h->seq;
            seen_w = (seen_w + 1) % 128;
        }
        LeaveCriticalSection(&cs);
        if (dup)
            return;
    }
    if (fn && n > (int)sizeof(Head))
        fn(h->id, (unsigned char)buf[sizeof(Head)], (const unsigned char *)buf + sizeof(Head) + 1,
           n - (int)sizeof(Head) - 1);
}

static void steam_poll(void)
{
    void *obj = g_st_obj;
    uint32_t sz = 0;
    int guard = 0;
    if (!g_gchan_req || !obj || !g_st_avail || !g_st_read)
        return;
    while (guard++ < 64 && g_st_avail(obj, &sz, ST_CH) && sz) {
        char buf[2048];
        uint64_t id = 0;
        uint32_t n = 0;
        if (!g_st_read(obj, buf, sizeof(buf), &n, &id, ST_CH))
            break;
        if (n < sizeof(Head) || n > sizeof(buf))
            continue;
        {
            const Head *h = (const Head *)buf;
            int k;
            if (h->magic != NET_MAGIC || h->ver != NET_VER || h->id == g_id)
                continue;
            if (InterlockedIncrement(&g_st_recv) == 1 && g_log)
                g_log("reseau : premier paquet d'un autre plugin recu par Steam (joueur %llu)", (unsigned long long)id);
            EnterCriticalSection(&g_gcs);
            k = sp_find(id, 1);
            g_sp[k].plug = now_s();
            LeaveCriticalSection(&g_gcs);
            note_seen(h->name, now_s());
            steam_send(buf, (int)n, 1, id); /* on héberge : aux autres joueurs */
            if (h->kind == K_POSE && n == sizeof(Pose))
                store_pose((const Pose *)buf, now_s());
            else if (h->kind == K_BLOB)
                got_blob(buf, (int)n);
        }
    }
}

void net_game_channel(int on)
{
    g_gchan_req = on;
    g_gchan = on && g_gcs_ok && g_real_sendto && g_real_recvfrom;
}

/* envoi par le canal du jeu. only_plug : seulement aux correspondants qui ont le plugin */
static void game_send(const void *buf, int len, int only_plug, const GAddr *except)
{
    int i;
    double now;
    steam_send(buf, len, only_plug, 0); /* v1.7.22 */
    if (!g_gchan || g_gsock == INVALID_SOCKET)
        return;
    now = now_s();
    EnterCriticalSection(&g_gcs);
    for (i = 0; i < MAX_GEP; ++i) {
        GEp *e = &g_gep[i];
        /* v1.7.25 : le jeu parle vraiment à ce correspondant (pas un simple ping de la liste des serveurs) */
        int active = now - e->sent < 10.0 && e->n >= 30, plug = now - e->plug < 10.0;
        if (!(only_plug ? plug && (active || now - e->plug < 3.0) : active || plug))
            continue;
        if (except && gaddr_eq(&e->a, except))
            continue;
        /* v1.6.2 : par la socket que le jeu utilise avec CE correspondant. Avant, la dernière socket
           du jeu (liste des serveurs…) : la box d'en face jetait les paquets sans port ouvert. */
        {
            int rr = g_real_sendto(e->s != INVALID_SOCKET && e->s ? e->s : g_gsock, (const char *)buf, len, 0,
                                   &e->a.sa, gaddr_len(&e->a));
            if (rr > 0)
                InterlockedIncrement(&g_gc_sent);
            if (!e->logged && g_log) {
                /* v1.7.21 : diagnostic : à qui on parle par le canal du jeu */
                char t[96];
                gaddr_fmt(&e->a, t, sizeof(t));
                e->logged = 1;
                g_log("reseau : canal du jeu : plugin envoye a %s (%s, socket %llu)", t, rr > 0 ? "ok" : "refuse",
                      (unsigned long long)e->s);
            }
        }
    }
    LeaveCriticalSection(&g_gcs);
}

static void stk_resend(double now); /* v1.8.196 */
static DWORD WINAPI net_thread(LPVOID arg)
{
    char buf[2048];
    double last_hello = 0.0;
    (void)arg;
    while (g_run) {
        fd_set rs;
        {
            /* v1.7.19 : bilan du canal du jeu toutes les 60 s quand il sert */
            static double last_rep = 0.0;
            double t = now_s();
            if ((g_gchan || g_gchan_req) && g_gcs_ok && g_log && t - last_rep > 60.0) {
                int i, act = 0, plug = 0;
                last_rep = t;
                EnterCriticalSection(&g_gcs);
                for (i = 0; i < MAX_GEP; ++i) {
                    if (t - g_gep[i].sent < 10.0)
                        ++act;
                    if (t - g_gep[i].plug < 10.0)
                        ++plug;
                }
                LeaveCriticalSection(&g_gcs);
                g_log("reseau : canal du jeu : %ld paquets du jeu, %ld du plugin envoyes, %ld du plugin recus ; %d adresse(s) du jeu, %d avec le plugin",
                      (long)g_gc_game, (long)g_gc_sent, (long)g_gc_recv, act, plug);
                g_log("reseau : sendto du jeu : %ld appels, dont %ld en IPv6", (long)g_gc_all, (long)g_gc_v6);
                g_log("reseau : canal Steam : %s, %ld paquets du jeu, %ld du plugin envoyes, %ld du plugin recus",
                      g_st_obj ? "pret" : "pas trouve", (long)g_st_game, (long)g_st_sent, (long)g_st_recv);
            }
        }
        struct timeval tv = { 0, g_st_obj && g_gchan_req ? 15000 : 100000 }; /* v1.7.22 : Steam lu souvent */
        double now = now_s();
        if (g_auto && !LAN_OFF && now - last_hello > 1.0) {
            /* sans hôte : on se signale sur chaque réseau ; les autres plugins répondent pareil */
            static double last_find = -100.0;
            static int logged = 0;
            Head h;
            int i;
            if (now - last_find > 10.0) {
                last_find = now;
                find_broadcasts();
                if (!logged && g_log) {
                    logged = 1;
                    for (i = 0; i < g_nbcast; ++i) {
                        char ip[64];
                        inet_ntop(AF_INET, (void *)&g_bcast[i].sin_addr, ip, sizeof(ip));
                        g_log("reseau : recherche des autres plugins sur %s", ip);
                    }
                }
            }
            head_fill(&h, K_HELLO);
            for (i = 0; i < g_nbcast; ++i)
                sendto(g_sock, (const char *)&h, sizeof(h), 0, (const struct sockaddr *)&g_bcast[i], sizeof(g_bcast[i]));
            /* v1.7.20 : aussi directement aux plugins déjà trouvés (la diffusion sur Radmin VPN se
               perd souvent : joueur tantôt « avec », tantôt « sans plugin ») */
            for (i = 0; i < MAX_CLIENTS; ++i)
                if (g_clients[i].seen != 0.0 && now - g_clients[i].seen < 120.0)
                    sendto(g_sock, (const char *)&h, sizeof(h), 0, (const struct sockaddr *)&g_clients[i].addr,
                           sizeof(g_clients[i].addr));
            last_hello = now;
        } else if (!g_host && !g_auto && now - last_hello > 1.0) {
            /* client : l'hôte doit nous connaître pour nous renvoyer les autres */
            Head h;
            head_fill(&h, K_HELLO);
            sendto(g_sock, (const char *)&h, sizeof(h), 0, (const struct sockaddr *)&g_host_addr, sizeof(g_host_addr));
            last_hello = now;
        }
        steam_poll(); /* v1.7.22 */
        stk_resend(now); /* v1.8.196 */
        if (g_gchan || (g_gchan_req && g_st_obj)) {
            static double last_ghello = 0.0;
            if (now - last_ghello > 2.0) {
                Head h;
                head_fill(&h, K_HELLO);
                game_send(&h, sizeof(h), 0, NULL);
                last_ghello = now;
            }
        }
        while (g_gq_r < g_gq_w) {
            int k = (int)(g_gq_r % GQ);
            const Head *h = (const Head *)g_gq[k].buf;
            int n = g_gq[k].n;
            if (h->magic == NET_MAGIC && h->ver == NET_VER && h->id != g_id) {
                note_seen(h->name, now_s());
                /* on héberge (plusieurs correspondants) : on fait suivre aux autres joueurs */
                if (g_gchan)
                    game_send(g_gq[k].buf, n, 1, &g_gq[k].from);
                if (h->kind == K_POSE && n == (int)sizeof(Pose))
                    store_pose((const Pose *)g_gq[k].buf, now_s());
                else if (h->kind == K_BLOB)
                    got_blob(g_gq[k].buf, n);
            }
            InterlockedIncrement(&g_gq_r);
        }
        FD_ZERO(&rs);
        FD_SET(g_sock, &rs);
        if (select(0, &rs, NULL, NULL, &tv) <= 0)
            continue;
        for (;;) {
            struct sockaddr_in from;
            int fl = sizeof(from), n;
            u_long avail = 0;
            if (ioctlsocket(g_sock, FIONREAD, &avail) != 0 || avail == 0)
                break;
            n = recvfrom(g_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n < (int)sizeof(Head))
                continue;
            {
                const Head *h = (const Head *)buf;
                if (h->magic != NET_MAGIC || h->ver != NET_VER || h->id == g_id)
                    continue; /* (nos propres signaux reviennent par la diffusion) */
                if (LAN_OFF)
                    continue; /* v1.8.201 : pas sur un serveur RAGDOLL : réseau local ignoré */
                note_seen(h->name, now_s());
                if (g_auto)
                    host_note_client(&from); /* chacun envoie directement à tous les autres */
                else if (g_host) {
                    host_note_client(&from);
                    if (h->kind == K_POSE || h->kind == K_BLOB)
                        host_broadcast(buf, n, &from); /* aux autres joueurs */
                }
                if (h->kind == K_POSE && n == (int)sizeof(Pose))
                    store_pose((const Pose *)buf, now_s());
                else if (h->kind == K_BLOB)
                    got_blob(buf, n);
            }
        }
    }
    return 0;
}

int net_start(int host, const char *ip, int port, NetLogFn log)
{
    WSADATA wd;
    struct sockaddr_in a;
    u_long nb = 1;
    if (g_run)
        return 1;
    g_log = log;
    QueryPerformanceFrequency(&g_freq);
    if (!g_cs_ok) {
        InitializeCriticalSection(&g_cs);
        g_cs_ok = 1;
    }
    if (port <= 0 || port > 65535)
        port = 47731;
    g_port = port;
    g_auto = !host && (!ip || !ip[0]);
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
        return 0;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)(host || g_auto ? port : 0));
    if (g_auto) {
        BOOL on = TRUE;
        setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
    }
    {
        int ok = bind(g_sock, (struct sockaddr *)&a, sizeof(a)) == 0;
        if (!ok && g_auto) {
            /* port pris par un autre programme : port au hasard ; les autres plugins nous trouvent
               quand même dès qu'ils reçoivent nos signaux */
            if (log)
                log("reseau : port UDP %d deja pris, port au hasard", port);
            a.sin_port = 0;
            ok = bind(g_sock, (struct sockaddr *)&a, sizeof(a)) == 0;
        }
        if (!ok) {
            if (log)
                log("reseau : port UDP %d deja pris (erreur %d)", port, WSAGetLastError());
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
            WSACleanup();
            return 0;
        }
    }
    ioctlsocket(g_sock, FIONBIO, &nb);
    {
        /* sans ça, un « port injoignable » (hôte pas encore lancé) casse la réception sous Windows */
        DWORD off = FALSE, ret = 0;
        WSAIoctl(g_sock, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
    }
    g_host = host;
    if (!host && !g_auto) {
        memset(&g_host_addr, 0, sizeof(g_host_addr));
        g_host_addr.sin_family = AF_INET;
        g_host_addr.sin_port = htons((u_short)port);
        if (inet_pton(AF_INET, ip, &g_host_addr.sin_addr) != 1) {
            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(ip, NULL, &hints, &res) != 0 || !res) {
                if (log)
                    log("reseau : adresse de l'hote \"%s\" incomprise", ip);
                closesocket(g_sock);
                g_sock = INVALID_SOCKET;
                WSACleanup();
                return 0;
            }
            g_host_addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
    }
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        g_id = (uint32_t)(c.QuadPart ^ (c.QuadPart >> 32) ^ GetCurrentProcessId() * 2654435761u);
        if (!g_id)
            g_id = 1;
    }
    memset(g_peers, 0, sizeof(g_peers));
    memset(g_clients, 0, sizeof(g_clients));
    g_run = 1;
    g_thread = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
    if (!g_thread) {
        g_run = 0;
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }
    if (log) {
        if (g_auto)
            log("reseau : automatique (Radmin VPN / reseau local), port UDP %d", port);
        else if (host)
            log("reseau : HOTE, en attente des joueurs sur le port UDP %d", port);
        else
            log("reseau : envoi a l'hote %s port UDP %d", ip, port);
    }
    return 1;
}

void net_stop(void)
{
    if (!g_run)
        return;
    g_run = 0;
    if (g_thread) {
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    WSACleanup();
}

/* v1.7.36 : le canal du jeu marche (paquets du jeu vus, plugin envoyé par lui) */
int net_game_ok(void)
{
    return g_gchan && g_gc_game > 50;
}

int net_running(void)
{
    return g_run;
}

void net_set_name(const char *name)
{
    snprintf(g_name, sizeof(g_name), "%s", name && name[0] ? name : "?");
}

void net_send_pose(const float model_world[12], const Ragdoll *rd, const DriveSnap *snap)
{
    Pose p;
    double now;
    if (!g_run)
        return;
    now = now_s();
    if (now - g_last_send < 1.0 / 30.0)
        return;
    g_last_send = now;
    head_fill(&p.h, K_POSE);
    memcpy(p.mw, model_world, sizeof(p.mw));
    memcpy(p.pos, rd->pos, sizeof(p.pos));
    p.snap = *snap;
    if (g_host || g_auto)
        host_broadcast(&p, sizeof(p), NULL);
    else
        sendto(g_sock, (const char *)&p, sizeof(p), 0, (const struct sockaddr *)&g_host_addr, sizeof(g_host_addr));
    game_send(&p, sizeof(p), 1, NULL); /* v1.3 : aussi par le canal du jeu */
}

void net_set_blob_fn(NetBlobFn fn)
{
    g_blob_fn = fn;
}

uint32_t net_my_id(void)
{
    return g_run ? g_id : 0;
}

/* v1.8.196 : objets (cônes, bottes, mobilier, rampes) renvoyés par le fil du réseau quand le jeu ne les
   envoie plus (joueur au stand : le plugin n'est plus appelé en piste, ses objets disparaissaient chez
   les autres au bout de 3 s) */
#define STK_MAX 24
typedef struct {
    int type, part, len;
    unsigned char data[1100];
} Sticky;
static Sticky g_stk[STK_MAX];
static int g_nstk = 0;
static double g_stk_main[32];
static CRITICAL_SECTION g_stk_cs;
static volatile LONG g_stk_init = 0;

static void stk_lock(int on)
{
    if (InterlockedCompareExchange(&g_stk_init, 1, 0) == 0)
        InitializeCriticalSection(&g_stk_cs);
    if (on)
        EnterCriticalSection(&g_stk_cs);
    else
        LeaveCriticalSection(&g_stk_cs);
}

void net_sticky(int type, int part, int nparts, const void *data, int len)
{
    int i, k = -1;
    if (len < 0 || len > (int)sizeof(g_stk[0].data) || type < 0 || type >= 32)
        return;
    stk_lock(1);
    /* parties au-delà du nombre actuel : retirées (moins d'objets) */
    for (i = 0; i < g_nstk;)
        if (g_stk[i].type == type && g_stk[i].part >= nparts)
            g_stk[i] = g_stk[--g_nstk];
        else
            ++i;
    for (i = 0; i < g_nstk; ++i)
        if (g_stk[i].type == type && g_stk[i].part == part)
            k = i;
    if (k < 0 && g_nstk < STK_MAX)
        k = g_nstk++;
    if (k >= 0) {
        g_stk[k].type = type;
        g_stk[k].part = part;
        g_stk[k].len = len;
        memcpy(g_stk[k].data, data, (size_t)len);
    }
    stk_lock(0);
}

void net_sticky_clear(void)
{
    stk_lock(1);
    g_nstk = 0;
    stk_lock(0);
}

static int send_blob_raw(int type, const void *data, int len);
int net_send_blob(int type, const void *data, int len)
{
    if (type >= 0 && type < 32)
        g_stk_main[type] = now_s();
    return send_blob_raw(type, data, len);
}

/* fil du réseau : les objets dont le jeu n'a rien envoyé depuis 1,5 s, une fois par seconde */
static void stk_resend(double now)
{
    static double last = 0.0;
    int i;
    if (now - last < 1.0)
        return;
    last = now;
    stk_lock(1);
    for (i = 0; i < g_nstk; ++i)
        if (now - g_stk_main[g_stk[i].type] > 1.5)
            send_blob_raw(g_stk[i].type, g_stk[i].data, g_stk[i].len);
    stk_lock(0);
}

static int send_blob_raw(int type, const void *data, int len)
{
    char buf[1400];
    if (!g_run || len < 0 || len + (int)sizeof(Head) + 1 > (int)sizeof(buf))
        return 0;
    head_fill((Head *)buf, K_BLOB);
    buf[sizeof(Head)] = (char)type;
    memcpy(buf + sizeof(Head) + 1, data, (size_t)len);
    len += (int)sizeof(Head) + 1;
    if (g_host || g_auto)
        host_broadcast(buf, len, NULL);
    else
        sendto(g_sock, buf, len, 0, (const struct sockaddr *)&g_host_addr, sizeof(g_host_addr));
    game_send(buf, len, 1, NULL);
    return 1;
}

static void lerp3(float *o, const float *a, const float *b, float k, int n)
{
    int i;
    for (i = 0; i < n; ++i)
        o[i] = a[i] + (b[i] - a[i]) * k;
}

int net_peer_pose(int i, uint32_t *id, char name[32], float model_world[12], float pos[RD_NUM_POINTS][3],
                  DriveSnap *snap)
{
    Peer *pe;
    double now, target;
    int k, a = -1, b = -1;
    if (!g_run || !g_cs_ok || i < 0 || i >= NET_MAX_PEERS)
        return 0;
    now = now_s();
    EnterCriticalSection(&g_cs);
    pe = &g_peers[i];
    if (!pe->id || pe->n == 0 || now - pe->last_recv > TIMEOUT) {
        LeaveCriticalSection(&g_cs);
        return 0;
    }
    *id = pe->id;
    memcpy(name, pe->name, 32);
    /* instant affiché, dans le temps de l'envoyeur */
    target = now - pe->offset - DELAY;
    for (k = 0; k < pe->n; ++k) {
        int idx = (pe->head + RING - 1 - k) % RING;
        if (pe->ring[idx].t <= target) {
            a = idx;
            break;
        }
        b = idx;
    }
    if (a < 0) {
        /* pas encore assez de poses : la plus ancienne */
        int oldest = (pe->head + RING - pe->n) % RING;
        a = b = oldest;
    }
    if (b < 0)
        b = a; /* au-delà de la dernière : on garde la dernière */
    {
        const Frame *fa = &pe->ring[a], *fb = &pe->ring[b];
        float kk = 0.0f;
        if (a != b && fb->t > fa->t)
            kk = (float)((target - fa->t) / (fb->t - fa->t));
        if (kk < 0.0f)
            kk = 0.0f;
        if (kk > 1.0f)
            kk = 1.0f;
        lerp3(model_world, fa->mw, fb->mw, kk, 12);
        lerp3(&pos[0][0], &fa->pos[0][0], &fb->pos[0][0], kk, RD_NUM_POINTS * 3);
        *snap = kk < 0.5f ? fa->snap : fb->snap;
        if (fa->snap.head_on && fb->snap.head_on) {
            lerp3(snap->head_dir, fa->snap.head_dir, fb->snap.head_dir, kk, 3);
            lerp3(snap->head_right, fa->snap.head_right, fb->snap.head_right, kk, 3);
        }
        if (fa->snap.feet_on && fb->snap.feet_on)
            lerp3(&snap->feet[0][0], &fa->snap.feet[0][0], &fb->snap.feet[0][0], kk, 6);
        if (fa->snap.frame_on && fb->snap.frame_on) {
            lerp3(snap->frame_right, fa->snap.frame_right, fb->snap.frame_right, kk, 3);
            lerp3(snap->frame_fwd, fa->snap.frame_fwd, fb->snap.frame_fwd, kk, 3);
        }
        snap->fist = fa->snap.fist + (fb->snap.fist - fa->snap.fist) * kk;
        snap->root_up = fa->snap.root_up + (fb->snap.root_up - fa->snap.root_up) * kk;
    }
    LeaveCriticalSection(&g_cs);
    return 1;
}

#else
int net_start(int host, const char *ip, int port, NetLogFn log) { (void)host; (void)ip; (void)port; (void)log; return 0; }
void net_stop(void) {}
int net_running(void) { return 0; }
int net_game_ok(void) { return 0; }
void net_set_name(const char *name) { (void)name; }
int net_name_seen(const char *name, double max_age) { (void)name; (void)max_age; return 0; }
int net_game_hooks_install(NetLogFn log) { (void)log; return 0; }
int net_steam_install(NetLogFn log) { (void)log; return 0; }
void net_game_channel(int on) { (void)on; }
void net_send_pose(const float model_world[12], const Ragdoll *rd, const DriveSnap *snap) { (void)model_world; (void)rd; (void)snap; }
void net_set_blob_fn(NetBlobFn fn) { (void)fn; }
uint32_t net_my_id(void) { return 0; }
int net_send_blob(int type, const void *data, int len) { (void)type; (void)data; (void)len; return 0; }
void net_sticky(int type, int part, int nparts, const void *data, int len) { (void)type; (void)part; (void)nparts; (void)data; (void)len; }
void net_sticky_clear(void) {}
int net_peer_pose(int i, uint32_t *id, char name[32], float model_world[12], float pos[RD_NUM_POINTS][3], DriveSnap *snap)
{ (void)i; (void)id; (void)name; (void)model_world; (void)pos; (void)snap; return 0; }
#endif

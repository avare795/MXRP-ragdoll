/*
 * gamever.c - v1.8.137 : voir gamever.h
 */
#include "gamever.h"
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>

static const GameAddrs k_known[] = {
    /* beta21e (2026-11) */
    { 0x6A21833Du, 0x221BC3u, 0x243220u, 0xCFDDu, 0x265E00u, 0x26659Fu, 0x8B93Bu, 0x8B90Eu, 0x566C48u, 0xE5A6A0u,
      0x350, 0x370, 0x357, 0 },
    /* mise à jour du 23/11/2026 */
    { 0x6B032CE7u, 0x221E03u, 0x243460u, 0xCEADu, 0x266040u, 0x2667DFu, 0x8B8FBu, 0x8B8CEu, 0x566C48u, 0xE5A6A0u,
      0x351, 0x371, 0x358, 0 },
};

/* motifs : octets en hexadécimal, ?? = n'importe lequel ; @ marque le CALL visé */
static const char k_skel_sig[] =
    "0f 84 be 04 00 00 4c 89 a4 24 28 01 00 00 45 33 e4 44 89 a1 e8 ?? ?? ?? ?? 10 e2 ff ff 48 8b cf @e8 ?? ?? ?? ?? "
    "48 8b cf e8 ?? ?? ?? ?? 4c 63 9f e8 ?? ?? ?? ?? 8b c4 4a";
static const char k_cam_sig[] =
    "8d 94 24 30 01 00 00 48 8b cd 89 75 54 c7 45 40 00 00 96 42 @e8 ?? ?? ?? ?? 8b 47 44 48 8d 8c 24 10 02 00 00 89 45 44 "
    "8b 47 48 89 45 48 8b 47 4c 89 45 4c e8 ?? ?? ?? ?? 48 8d 8c 24";

static int parse_sig(const char *s, uint8_t *b, uint8_t *m, int *at)
{
    int n = 0;
    *at = -1;
    while (*s && n < 128) {
        while (*s == ' ')
            ++s;
        if (!*s)
            break;
        if (*s == '@') {
            *at = n;
            ++s;
            continue;
        }
        if (s[0] == '?' && s[1] == '?') {
            b[n] = 0;
            m[n++] = 0;
        } else {
            unsigned v = 0;
            sscanf(s, "%2x", &v);
            b[n] = (uint8_t)v;
            m[n++] = 1;
        }
        s += 2;
    }
    return n;
}

/* une seule correspondance dans [lo, hi) : rend l'adresse du @, sinon 0 */
static uintptr_t find_sig(uintptr_t lo, uintptr_t hi, const char *sig)
{
    uint8_t b[128], m[128];
    int at, n = parse_sig(sig, b, m, &at), k;
    uintptr_t p, hit = 0;
    int hits = 0;
    if (n < 16 || at < 0)
        return 0;
    for (p = lo; p + (uintptr_t)n <= hi; ++p) {
        const uint8_t *q = (const uint8_t *)p;
        if (q[0] != b[0])
            continue;
        for (k = 1; k < n; ++k)
            if (m[k] && q[k] != b[k])
                break;
        if (k == n) {
            hit = p + (uintptr_t)at;
            if (++hits > 1)
                return 0;
        }
    }
    return hits == 1 ? hit : 0;
}

const GameAddrs *gamever_get(void (*log)(const char *fmt, ...))
{
    static GameAddrs g;
    static int done = 0;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    size_t i;
    if (done)
        return &g;
    done = 1;
    memset(&g, 0, sizeof(g));
    nt = (IMAGE_NT_HEADERS *)(base + (uintptr_t)dos->e_lfanew);
    g.stamp = nt->FileHeader.TimeDateStamp;
    for (i = 0; i < sizeof(k_known) / sizeof(k_known[0]); ++i)
        if (k_known[i].stamp == g.stamp) {
            g = k_known[i];
            if (log)
                log("jeu : version connue (date %08X)", g.stamp);
            return &g;
        }
    /* version inconnue : recherche dans le code (section .text) */
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        uintptr_t lo, hi, s;
        if (memcmp(sec->Name, ".text", 5))
            continue;
        lo = base + sec->VirtualAddress;
        hi = lo + sec->Misc.VirtualSize;
        if ((s = find_sig(lo, hi, k_skel_sig)) != 0) {
            int32_t rel;
            memcpy(&rel, (const void *)(s + 1), 4);
            g.skel_site = (uint32_t)(s - base);
            g.skel_target = (uint32_t)(s + 5 + (intptr_t)rel - base);
        }
        if ((s = find_sig(lo, hi, k_cam_sig)) != 0) {
            int32_t rel;
            uint32_t t;
            memcpy(&rel, (const void *)(s + 1), 4);
            t = (uint32_t)(s + 5 + (intptr_t)rel - base);
            g.cam_site = (uint32_t)(s - base);
            g.cam_min = t > 0x800u ? t - 0x800u : 0;
            g.cam_max = t + 0x800u;
        }
    }
    g.by_sig = 1;
    if (log)
        log("jeu : version inconnue (date %08X) : squelette %s (+0x%X -> +0x%X), camera %s (+0x%X), expulsion auto coupee",
            g.stamp, g.skel_site ? "trouve" : "PAS trouve", g.skel_site, g.skel_target, g.cam_site ? "trouvee" : "PAS trouvee",
            g.cam_site);
    return &g;
}
/* v1.8.146 : chargement d'une piste : ses maillages (passerelles...) créés par le jeu :
   dGeomTriMeshDataCreate, dGeomTriMeshDataBuildSingle, dCreateTriMesh (2 fois dans le jeu : la 1re suffit) */
static const char k_tm_sig[] =
    "@e8 ?? ?? ?? ?? c7 44 24 30 0c 00 00 00 41 b8 0c 00 00 00 48 89 87 ?? ?? 00 00 8b 8d ?? ?? 00 00 44 8b 8d ?? ?? 00 00 "
    "8d 14 49 48 8b 8d ?? ?? 00 00 89 54 24 28 48 8b 95 ?? ?? 00 00 48 89 4c 24 20 48 8b c8 e8 ?? ?? ?? ?? 48 8b 97 ?? ?? 00 00 "
    "48 8b 8f ?? ?? 00 00 45 33 c9 45 33 c0 4c 89 6c 24 20 e8";
/* fin de piste : physique de la piste détruite (espace, puis données des maillages) */
static const char k_tmd_sig[] =
    "48 8b 8b ?? ?? 00 00 48 85 c9 74 0c @e8 ?? ?? ?? ?? 48 89 bb ?? ?? 00 00 48 8b 8b ?? ?? 00 00 e8 ?? ?? ?? ?? "
    "48 8b 8b ?? ?? 00 00 48 89 bb ?? ?? 00 00 89 bb ?? ?? 00 00 e8";

/* objets de la piste : un espace par groupe (dSimpleSpaceCreate dans l'espace de la piste) */
static const char k_sp_sig[] =
    "48 8b 8b ?? ?? 00 00 @e8 ?? ?? ?? ?? ba 02 00 00 00 48 8b c8 48 89 83 ?? ?? 00 00 e8 ?? ?? ?? ?? 48 8b 8b ?? ?? 00 00 33 d2 e8";

static uintptr_t find_sig_first(uintptr_t lo, uintptr_t hi, const char *sig)
{
    uint8_t b[128], m[128];
    int at, n = parse_sig(sig, b, m, &at), k;
    uintptr_t p;
    if (n < 16 || at < 0)
        return 0;
    for (p = lo; p + (uintptr_t)n <= hi; ++p) {
        const uint8_t *q = (const uint8_t *)p;
        if (q[0] != b[0])
            continue;
        for (k = 1; k < n; ++k)
            if (m[k] && q[k] != b[k])
                break;
        if (k == n)
            return p + (uintptr_t)at;
    }
    return 0;
}

static uintptr_t call_target(uintptr_t site)
{
    int32_t rel;
    if (*(const uint8_t *)site != 0xE8)
        return 0;
    memcpy(&rel, (const void *)(site + 1), 4);
    return site + 5 + (intptr_t)rel;
}

const GameOde *gamever_ode(void (*log)(const char *fmt, ...))
{
    static GameOde o;
    static int done = 0;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    size_t i;
    if (done)
        return &o;
    done = 1;
    memset(&o, 0, sizeof(o));
    nt = (IMAGE_NT_HEADERS *)(base + (uintptr_t)dos->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        uintptr_t lo, hi, s;
        if (memcmp(sec->Name, ".text", 5))
            continue;
        lo = base + sec->VirtualAddress;
        hi = lo + sec->Misc.VirtualSize;
        if ((s = find_sig_first(lo, hi, k_tm_sig)) != 0) {
            o.data_create = call_target(s);
            o.build_single = call_target(s + 0x44);
            o.create = call_target(s + 0x62);
        }
        if ((s = find_sig_first(lo, hi, k_sp_sig)) != 0)
            o.space_create = call_target(s);
        if ((s = find_sig(lo, hi, k_tmd_sig)) != 0) {
            o.destroy = call_target(s);
            o.data_destroy = call_target(s + 0x2c);
        }
        {
            /* toutes dans le code du jeu, sinon rien */
            uintptr_t *f = &o.data_create;
            int k;
            for (k = 0; k < 6; ++k)
                if (f[k] < lo || f[k] >= hi) {
                    memset(&o, 0, sizeof(o));
                    break;
                }
        }
        break;
    }
    if (log)
        log("jeu : fonctions des maillages %s (+0x%llX +0x%llX +0x%llX, effacer +0x%llX +0x%llX, espace +0x%llX)",
            o.create ? "trouvees" : "PAS trouvees", (unsigned long long)(o.data_create ? o.data_create - base : 0),
            (unsigned long long)(o.build_single ? o.build_single - base : 0), (unsigned long long)(o.create ? o.create - base : 0),
            (unsigned long long)(o.destroy ? o.destroy - base : 0), (unsigned long long)(o.data_destroy ? o.data_destroy - base : 0),
            (unsigned long long)(o.space_create ? o.space_create - base : 0));
    return &o;
}
#else
const GameAddrs *gamever_get(void (*log)(const char *fmt, ...))
{
    static GameAddrs g;
    (void)log;
    return &g;
}
const GameOde *gamever_ode(void (*log)(const char *fmt, ...))
{
    static GameOde o;
    (void)log;
    return &o;
}
#endif

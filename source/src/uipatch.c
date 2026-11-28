/*
 * uipatch.c - v1.6 : bouton du plugin dans les menus du jeu, sans toucher au dossier ui
 *
 * Le bouton « Plugin Ragdoll : ACTIF / COUPE » est une case des menus du jeu (testsetup.mnu :
 * préparation du mode Essai, hostsetup.mnu : options du serveur hébergé), avec son texte dans les
 * fichiers de langue (.str). Le plugin ne modifie aucun fichier du jeu : au lancement il prépare des
 * copies avec la case (dans Documents\PiBoSo\MX Bikes\MXRP_Ragdoll_menus) et, quand le jeu ouvre
 * ses menus (fichiers du dossier ui ou archive ui.pkz), il lui donne ces copies à la place.
 * Rien n'est fait pour un menu qui a déjà la case (mod de menus avec le bouton).
 */
#include "uipatch.h"

#ifdef _WIN32
#include "inflate.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static UiPatchLogFn g_log = NULL;

static unsigned char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *b;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (32L << 20)) {
        fclose(f);
        return NULL;
    }
    b = (unsigned char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[n] = 0;
        *len = (size_t)n;
    }
    return b;
}

static int write_all(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f)
        return 0;
    ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    return ok;
}

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24); }

/* une entrée de l'archive zip ui.pkz (méthode 0 ou 8) */
static char *pkz_extract(const unsigned char *z, size_t zl, const char *name, size_t *outlen)
{
    size_t i, cd, n, k, nl = strlen(name);
    if (zl < 22)
        return NULL;
    for (i = zl - 22;; --i) {
        if (rd32(z + i) == 0x06054b50UL)
            break;
        if (i == 0 || zl - i > 70000)
            return NULL;
    }
    n = rd16(z + i + 10);
    cd = rd32(z + i + 16);
    for (k = 0; k < n && cd + 46 <= zl; ++k) {
        unsigned method = rd16(z + cd + 10), fnl = rd16(z + cd + 28), exl = rd16(z + cd + 30), cml = rd16(z + cd + 32);
        unsigned long csz = rd32(z + cd + 20), usz = rd32(z + cd + 24), loc = rd32(z + cd + 42);
        if (rd32(z + cd) != 0x02014b50UL || cd + 46 + fnl > zl)
            return NULL;
        if (fnl == nl && !_strnicmp((const char *)z + cd + 46, name, nl)) {
            size_t data;
            char *out;
            if (loc + 30 > zl || rd32(z + loc) != 0x04034b50UL)
                return NULL;
            data = loc + 30 + rd16(z + loc + 26) + rd16(z + loc + 28);
            if (data + csz > zl || usz > (16UL << 20))
                return NULL;
            out = (char *)malloc(usz + 1);
            if (!out)
                return NULL;
            if (method == 0 && csz == usz) {
                memcpy(out, z + data, usz);
                *outlen = usz;
            } else if (method != 8 || inflate_raw((unsigned char *)out, usz, z + data, csz, outlen) != 0 || *outlen != usz) {
                free(out);
                return NULL;
            }
            out[usz] = 0;
            return out;
        }
        cd += 46 + fnl + exl + cml;
    }
    return NULL;
}

/* la case du plugin dans la boîte de dialogue « dialog » ; nl = fin de ligne du fichier */
static char *patch_mnu(const char *txt, const char *dialog, const char *rect)
{
    const char *nl = strstr(txt, "\r\n") ? "\r\n" : "\n";
    char key[128], color[96] = "color 255 60 60 60", block[2048];
    const char *d, *p, *end;
    int depth = 0;
    size_t pre, len = strlen(txt);
    char *out;
    if (strstr(txt, "ID_RAGDOLL_PLUGIN"))
        return NULL;
    snprintf(key, sizeof(key), "name %s", dialog);
    for (d = strstr(txt, key); d; d = strstr(d + 1, key)) {
        char after = d[strlen(key)];
        if (after == '\r' || after == '\n' || after == ' ' || after == '\t')
            break; /* le nom entier (idd_testing_set, pas idd_testing_setup) */
    }
    if (!d)
        return NULL;
    /* début de la boîte : l'accolade avant le nom */
    p = d;
    while (p > txt && *p != '{')
        --p;
    if (*p != '{')
        return NULL;
    for (end = p; *end; ++end) {
        if (*end == '{')
            ++depth;
        else if (*end == '}' && --depth == 0)
            break;
    }
    if (!*end)
        return NULL;
    {
        /* couleur d'une case de la même boîte : même style que le mod de menus */
        const char *c = strstr(p, "item_checkbox");
        if (c && c < end) {
            const char *cl = strstr(c, "\tcolor ");
            if (cl && cl < end) {
                size_t l = strcspn(cl + 1, "\r\n");
                if (l < sizeof(color)) {
                    memcpy(color, cl + 1, l);
                    color[l] = 0;
                }
            }
        }
    }
    snprintf(block, sizeof(block),
             "\titem_checkbox%s\t{%s\t\tname ID_RAGDOLL_PLUGIN%s\t\trect %s%s\t\tspriteposx 0.000000%s"
             "\t\tspriteposy 0.000000%s\t\tspritesizex 0.000000%s\t\tspritesizey 0.000000%s"
             "\t\tsprite1 check1.tga%s\t\tsprite2 check2.tga%s\t\tsample2 ui_checkbox.wav%s\t\ttext%s\t\t{%s"
             "\t\t\tfont main.fnt%s\t\t\tpos 0.015625 0.000000%s\t\t\tfontsize 0.022222%s\t\t\talign left%s\t\t}%s"
             "\t\ttextid ragdoll_plugin%s\t\t%s%s\t}%s",
             nl, nl, nl, rect, nl, nl, nl, nl, nl, nl, nl, nl, nl, nl, nl, nl, nl, nl, nl, nl, color, nl, nl);
    pre = (size_t)(end - txt);
    out = (char *)malloc(len + strlen(block) + 1);
    if (!out)
        return NULL;
    memcpy(out, txt, pre);
    strcpy(out + pre, block);
    strcat(out + pre, txt + pre);
    return out;
}


static int file_exists(const char *p)
{
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

/* ---- copies préparées ---- */

typedef struct {
    char rel[64];       /* chemin dans le dossier du jeu, en minuscules : « ui\testsetup.mnu », « ui.pkz » */
    char path[MAX_PATH]; /* copie du plugin */
} Redirect;

static Redirect g_red[40];
static int g_nred = 0;
static char g_game[MAX_PATH]; /* dossier du jeu, minuscules */
static size_t g_gamel = 0;
static char g_outdir[MAX_PATH];
static volatile LONG g_seen = 0, g_served = 0;

static void add_redirect(const char *rel, const char *path)
{
    size_t i;
    if (g_nred >= (int)(sizeof(g_red) / sizeof(g_red[0])))
        return;
    snprintf(g_red[g_nred].rel, sizeof(g_red[g_nred].rel), "%s", rel);
    for (i = 0; g_red[g_nred].rel[i]; ++i)
        g_red[g_nred].rel[i] = (char)tolower((unsigned char)g_red[g_nred].rel[i]);
    snprintf(g_red[g_nred].path, sizeof(g_red[g_nred].path), "%s", path);
    ++g_nred;
}

/* écrit la copie et la donne au jeu à la place de « rel » */
static int serve(const char *rel, const char *file, const char *data, size_t len)
{
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\%s", g_outdir, file);
    if (!write_all(p, data, len)) {
        if (g_log)
            g_log("menus : copie %s pas ecrite", p);
        return 0;
    }
    add_redirect(rel, p);
    return 1;
}

static char *str_patch(const char *t, size_t len, int french)
{
    const char *nl;
    char add[128], *out;
    if (strstr(t, "ragdoll_plugin"))
        return NULL;
    nl = strstr(t, "\r\n") ? "\r\n" : "\n";
    snprintf(add, sizeof(add), "%sragdoll_plugin%sPlugin Ragdoll : %s%s", (len && t[len - 1] != '\n') ? nl : "", nl,
             french ? "COUPE" : "OFF", nl);
    out = (char *)malloc(len + strlen(add) + 1);
    if (!out)
        return NULL;
    memcpy(out, t, len);
    strcpy(out + len, add);
    return out;
}

static const struct {
    const char *file, *dialog, *rect;
} MENUS[] = {
    {"testsetup.mnu", "idd_testing_set", "0.412500 0.555555 0.597500 0.577777"},
    {"hostsetup.mnu", "idd_host_racesetup", "0.706250 0.766666 0.891250 0.788889"},
};

/* contenu d'origine + case, pour une entrée « ui/xxx » ; NULL = rien à changer */
static char *patched_entry(const char *file, const char *txt, size_t len)
{
    size_t i;
    for (i = 0; i < sizeof(MENUS) / sizeof(MENUS[0]); ++i)
        if (!_stricmp(file, MENUS[i].file))
            return patch_mnu(txt, MENUS[i].dialog, MENUS[i].rect);
    if (strlen(file) > 4 && !_stricmp(file + strlen(file) - 4, ".str"))
        return str_patch(txt, len, !_strnicmp(file, "fran", 4));
    return NULL;
}

static unsigned long g_crc[256];
static unsigned long crc32_of(const unsigned char *p, size_t n)
{
    unsigned long c = 0xFFFFFFFFUL;
    if (!g_crc[1]) {
        unsigned long k, v;
        int j;
        for (k = 0; k < 256; ++k) {
            v = k;
            for (j = 0; j < 8; ++j)
                v = (v & 1) ? 0xEDB88320UL ^ (v >> 1) : v >> 1;
            g_crc[k] = v;
        }
    }
    while (n--)
        c = g_crc[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, unsigned long v) { wr16(p, (unsigned)(v & 0xFFFF)); wr16(p + 2, (unsigned)(v >> 16)); }

/* copie de ui.pkz où les menus et textes sans le bouton sont remplacés (rangés sans compression).
   Retour : nombre d'entrées changées, -1 = archive illisible. */
static int pkz_rebuild(const unsigned char *z, size_t zl, unsigned char **out, size_t *outl)
{
    size_t i, cd, n, k, pos, cpos, cap;
    unsigned char *o, *c;
    int changed = 0;
    *out = NULL;
    if (zl < 22)
        return -1;
    for (i = zl - 22;; --i) {
        if (rd32(z + i) == 0x06054b50UL)
            break;
        if (i == 0 || zl - i > 70000)
            return -1;
    }
    n = rd16(z + i + 10);
    cd = rd32(z + i + 16);
    cap = zl * 2 + (1u << 20);
    o = (unsigned char *)malloc(cap);
    c = (unsigned char *)malloc(n * (46 + 512) + 64);
    if (!o || !c) {
        free(o);
        free(c);
        return -1;
    }
    pos = cpos = 0;
    for (k = 0; k < n; ++k) {
        unsigned method, fnl, exl, cml, flags;
        unsigned long csz, usz, loc, crc;
        const unsigned char *data;
        char name[256], *rep = NULL;
        size_t dl;
        if (cd + 46 > zl || rd32(z + cd) != 0x02014b50UL)
            goto bad;
        flags = rd16(z + cd + 8);
        method = rd16(z + cd + 10);
        crc = rd32(z + cd + 16);
        csz = rd32(z + cd + 20);
        usz = rd32(z + cd + 24);
        fnl = rd16(z + cd + 28);
        exl = rd16(z + cd + 30);
        cml = rd16(z + cd + 32);
        loc = rd32(z + cd + 42);
        if (fnl >= sizeof(name) || cd + 46 + fnl > zl || loc + 30 > zl || rd32(z + loc) != 0x04034b50UL)
            goto bad;
        memcpy(name, z + cd + 46, fnl);
        name[fnl] = 0;
        dl = loc + 30 + rd16(z + loc + 26) + rd16(z + loc + 28);
        if (dl + csz > zl)
            goto bad;
        data = z + dl;
        if (!_strnicmp(name, "ui/", 3) && !strchr(name + 3, '/') && name[3]) {
            size_t l = 0;
            char *t = pkz_extract(z, zl, name, &l);
            if (t) {
                rep = patched_entry(name + 3, t, l);
                free(t);
            }
        }
        if (rep) {
            dl = strlen(rep);
            data = (const unsigned char *)rep;
            method = 0;
            csz = usz = (unsigned long)dl;
            crc = crc32_of(data, dl);
            ++changed;
        }
        flags &= ~8u; /* tailles écrites dans l'en-tête */
        if (pos + 30 + fnl + csz > cap) {
            free(rep);
            goto bad;
        }
        /* en-tête local */
        wr32(o + pos, 0x04034b50UL);
        wr16(o + pos + 4, 20);
        wr16(o + pos + 6, flags);
        wr16(o + pos + 8, method);
        memcpy(o + pos + 10, z + cd + 12, 4); /* heure, date */
        wr32(o + pos + 14, crc);
        wr32(o + pos + 18, csz);
        wr32(o + pos + 22, usz);
        wr16(o + pos + 26, fnl);
        wr16(o + pos + 28, 0);
        memcpy(o + pos + 30, name, fnl);
        memcpy(o + pos + 30 + fnl, data, csz);
        /* entrée du répertoire central */
        memcpy(c + cpos, z + cd, 46);
        wr16(c + cpos + 8, flags);
        wr16(c + cpos + 10, method);
        wr32(c + cpos + 16, crc);
        wr32(c + cpos + 20, csz);
        wr32(c + cpos + 24, usz);
        wr16(c + cpos + 30, 0);
        wr16(c + cpos + 32, 0);
        wr32(c + cpos + 42, (unsigned long)pos);
        memcpy(c + cpos + 46, name, fnl);
        cpos += 46 + fnl;
        pos += 30 + fnl + csz;
        free(rep);
        cd += 46 + fnl + exl + cml;
    }
    if (pos + cpos + 22 > cap)
        goto bad;
    memcpy(o + pos, c, cpos);
    memset(o + pos + cpos, 0, 22);
    wr32(o + pos + cpos, 0x06054b50UL);
    wr16(o + pos + cpos + 8, (unsigned)n);
    wr16(o + pos + cpos + 10, (unsigned)n);
    wr32(o + pos + cpos + 12, (unsigned long)cpos);
    wr32(o + pos + cpos + 16, (unsigned long)pos);
    free(c);
    *out = o;
    *outl = pos + cpos + 22;
    return changed;
bad:
    free(o);
    free(c);
    return -1;
}

/* ---- ouverture des fichiers par le jeu ---- */

/* copie à donner pour ce chemin (lecture seule), sinon NULL */
static const char *redirect_for(const char *p, int *watch)
{
    char b[MAX_PATH], *r;
    size_t i, l;
    int k;
    *watch = 0;
    if (!p || !g_nred)
        return NULL;
    l = strlen(p);
    if (l >= sizeof(b))
        return NULL;
    for (i = 0; i <= l; ++i)
        b[i] = p[i] == '/' ? '\\' : (char)tolower((unsigned char)p[i]);
    *watch = l > 4 && (!strcmp(b + l - 4, ".mnu") || !strcmp(b + l - 4, ".str") || !strcmp(b + l - 4, ".pkz"));
    r = b;
    if (strchr(b, ':') || (b[0] == '\\' && b[1] == '\\')) {
        if (strncmp(b, g_game, g_gamel) || b[g_gamel] != '\\')
            return NULL;
        r = b + g_gamel + 1;
    }
    while (r[0] == '.' && r[1] == '\\')
        r += 2;
    for (k = 0; k < g_nred; ++k)
        if (!strcmp(r, g_red[k].rel))
            return g_red[k].path;
    return NULL;
}

static void note_open(const char *what, const char *p, const char *to)
{
    LONG n = InterlockedIncrement(&g_seen);
    if (to)
        InterlockedIncrement(&g_served);
    if (g_log && n <= 40)
        g_log("menus : le jeu ouvre %s (%s)%s", p, what, to ? " -> copie avec le bouton du plugin" : "");
}

typedef FILE *(__cdecl *FopenFn)(const char *, const char *);
typedef HANDLE(WINAPI *CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static FopenFn g_real_fopen = NULL;
static CreateFileAFn g_real_cfa = NULL;

static FILE *__cdecl hk_fopen(const char *p, const char *mode)
{
    int watch;
    const char *to = redirect_for(p, &watch);
    if (!mode || strpbrk(mode, "wa+"))
        to = NULL;
    if (watch)
        note_open("fopen", p, to);
    if (to) {
        FILE *f = g_real_fopen(to, mode);
        if (f)
            return f;
    }
    return g_real_fopen(p, mode);
}

static HANDLE WINAPI hk_cfa(LPCSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE tpl)
{
    int watch;
    const char *to = redirect_for(p, &watch);
    if ((acc & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) || disp != OPEN_EXISTING)
        to = NULL;
    if (watch)
        note_open("CreateFileA", p, to);
    if (to) {
        HANDLE h = g_real_cfa(to, acc, share, sa, disp, fl, tpl);
        if (h != INVALID_HANDLE_VALUE)
            return h;
    }
    return g_real_cfa(p, acc, share, sa, disp, fl, tpl);
}

static int iat_slot(const char *dll_name, const char *fn_name, void *hook, void **orig)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    void *target = NULL;
    HMODULE m = GetModuleHandleA(dll_name);
    int n = 0;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    if (m)
        target = (void *)GetProcAddress(m, fn_name);
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        IMAGE_THUNK_DATA64 *names, *slots;
        int same = !_stricmp((const char *)(base + imp->Name), dll_name);
        if (!imp->FirstThunk)
            continue;
        slots = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        names = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA64 *)(base + imp->OriginalFirstThunk) : NULL;
        for (; slots->u1.Function; ++slots, names = names ? names + 1 : NULL) {
            void **slot = (void **)&slots->u1.Function;
            DWORD old;
            int match = target && *slot == target;
            if (!match && same && names && names->u1.AddressOfData && !IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) &&
                !strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, fn_name))
                match = 1;
            if (!match || *slot == hook || !VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
                continue;
            if (!*orig)
                *orig = *slot;
            InterlockedExchangePointer(slot, hook);
            VirtualProtect(slot, sizeof(void *), old, &old);
            ++n;
        }
    }
    return n;
}

int uipatch_install(const char *save_path, UiPatchLogFn log)
{
    char exe[MAX_PATH], uidir[MAX_PATH], pk[MAX_PATH], *s;
    unsigned char *pkz;
    size_t pkzl = 0, i, sl;
    int nmenu = 0, a, b;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    g_log = log;
    if (!save_path || !GetModuleFileNameA(NULL, exe, sizeof(exe)) || !(s = strrchr(exe, '\\')))
        return -1;
    *s = 0;
    for (i = 0; exe[i]; ++i)
        g_game[i] = exe[i] == '/' ? '\\' : (char)tolower((unsigned char)exe[i]);
    g_game[i] = 0;
    g_gamel = i;
    sl = strlen(save_path);
    snprintf(g_outdir, sizeof(g_outdir), "%s%sMXRP_Ragdoll_menus", save_path,
             (sl && save_path[sl - 1] != '\\' && save_path[sl - 1] != '/') ? "\\" : "");
    CreateDirectoryA(g_outdir, NULL);
    snprintf(uidir, sizeof(uidir), "%s\\ui", exe);
    snprintf(pk, sizeof(pk), "%s\\ui.pkz", exe);
    pkz = read_all(pk, &pkzl);

    /* menus et texte anglais : fichier du dossier ui (mod de menus) s'il existe, sinon celui de ui.pkz */
    {
        static const char *const files[] = {"testsetup.mnu", "hostsetup.mnu", "english.str"};
        for (i = 0; i < 3; ++i) {
            char path[MAX_PATH], entry[64], rel[64], *t = NULL, *p;
            size_t l = 0;
            snprintf(path, sizeof(path), "%s\\%s", uidir, files[i]);
            snprintf(entry, sizeof(entry), "ui/%s", files[i]);
            snprintf(rel, sizeof(rel), "ui\\%s", files[i]);
            if (file_exists(path))
                t = (char *)read_all(path, &l);
            else if (pkz)
                t = pkz_extract(pkz, pkzl, entry, &l);
            if (!t) {
                if (log)
                    log("menus : %s illisible", files[i]);
                continue;
            }
            p = patched_entry(files[i], t, l);
            if (p) {
                nmenu += serve(rel, files[i], p, strlen(p));
                free(p);
            } else if (i < 2 && log) {
                log("menus : %s %s", files[i], strstr(t, "ID_RAGDOLL_PLUGIN") ? "a deja le bouton du plugin" : "sans la boite attendue");
            }
            free(t);
        }
    }
    /* autres langues du dossier ui */
    {
        char pat[MAX_PATH];
        snprintf(pat, sizeof(pat), "%s\\*.str", uidir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char p[MAX_PATH], rel[64], *t, *q;
                size_t l = 0;
                if (!_stricmp(fd.cFileName, "english.str") || strlen(fd.cFileName) > 50)
                    continue;
                snprintf(p, sizeof(p), "%s\\%s", uidir, fd.cFileName);
                snprintf(rel, sizeof(rel), "ui\\%s", fd.cFileName);
                t = (char *)read_all(p, &l);
                if (t && (q = str_patch(t, l, !_strnicmp(fd.cFileName, "fran", 4))) != NULL) {
                    nmenu += serve(rel, fd.cFileName, q, strlen(q));
                    free(q);
                }
                free(t);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    /* archive du jeu */
    if (pkz) {
        unsigned char *o = NULL;
        size_t ol = 0;
        int r = pkz_rebuild(pkz, pkzl, &o, &ol);
        if (r > 0 && serve("ui.pkz", "ui.pkz", (const char *)o, ol))
            ++nmenu;
        else if (r < 0 && log)
            log("menus : ui.pkz illisible");
        free(o);
        free(pkz);
    }
    a = iat_slot("MSVCR90.dll", "fopen", (void *)hk_fopen, (void **)&g_real_fopen);
    b = iat_slot("KERNEL32.dll", "CreateFileA", (void *)hk_cfa, (void **)&g_real_cfa);
    if (log)
        log("menus : %d copie(s) avec le bouton du plugin dans %s, dossier ui du jeu pas modifie (ouverture : fopen %d, CreateFileA %d)",
            nmenu, g_outdir, a, b);
    return nmenu;
}

/* v1.6.1 : police du texte du plugin tirée de misc.pkz du jeu (misc/misc.fnt), sur le PC du joueur */
/* v1.7.33 : une entrée d'une archive .pkz lue sans charger toute l'archive (bikes.pkz du jeu : 450 Mo).
   want : nom en minuscules avec « / » ; trouvé si le nom de l'entrée vaut want ou finit par « /want ». */
static char *pkz_stream_find_ex(const char *pk, const char *want, size_t *outlen, size_t head);
/* v1.8.19 : head > 0 : seulement les head premiers octets de l'entrée (en-tête d'un .edf de 100 Mo) */
static char *pkz_stream_find_ex(const char *pk, const char *want, size_t *outlen, size_t head)
{
    FILE *f = fopen(pk, "rb");
    long fsz, tail, i;
    unsigned char *b = NULL, *cdb = NULL, lh[30];
    unsigned long cdsz, cdoff;
    unsigned n, k;
    size_t pos, wl = strlen(want);
    char *out = NULL;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    fsz = ftell(f);
    tail = fsz < 70000 + 22 ? fsz : 70000 + 22;
    if (tail < 22 || !(b = (unsigned char *)malloc((size_t)tail)))
        goto done;
    fseek(f, fsz - tail, SEEK_SET);
    if (fread(b, 1, (size_t)tail, f) != (size_t)tail)
        goto done;
    for (i = tail - 22; i >= 0; --i)
        if (rd32(b + i) == 0x06054b50UL)
            break;
    if (i < 0)
        goto done;
    n = rd16(b + i + 10);
    cdsz = rd32(b + i + 12);
    cdoff = rd32(b + i + 16);
    if (cdsz > (64UL << 20) || (long)(cdoff + cdsz) > fsz || !(cdb = (unsigned char *)malloc(cdsz)))
        goto done;
    fseek(f, (long)cdoff, SEEK_SET);
    if (fread(cdb, 1, cdsz, f) != cdsz)
        goto done;
    for (k = 0, pos = 0; k < n && pos + 46 <= cdsz; ++k) {
        unsigned method = rd16(cdb + pos + 10), fnl = rd16(cdb + pos + 28), exl = rd16(cdb + pos + 30), cml = rd16(cdb + pos + 32);
        unsigned long csz = rd32(cdb + pos + 20), usz = rd32(cdb + pos + 24), loc = rd32(cdb + pos + 42);
        char nm[300];
        unsigned c;
        if (rd32(cdb + pos) != 0x02014b50UL || pos + 46 + fnl > cdsz)
            break;
        for (c = 0; c < fnl && c + 1 < sizeof(nm); ++c) {
            char ch = (char)cdb[pos + 46 + c];
            nm[c] = ch == '\\' ? '/' : (char)tolower((unsigned char)ch);
        }
        nm[c] = 0;
        if ((c == wl && !strcmp(nm, want)) || (c > wl && nm[c - wl - 1] == '/' && !strcmp(nm + c - wl, want))) {
            unsigned char *cd;
            if (head) {
                if (usz > head)
                    usz = (unsigned long)head;
                if (csz > (1UL << 20))
                    csz = 1UL << 20;
            }
            if (usz > (16UL << 20) || csz > (16UL << 20))
                break;
            fseek(f, (long)loc, SEEK_SET);
            if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50UL)
                break;
            fseek(f, (long)(loc + 30 + rd16(lh + 26) + rd16(lh + 28)), SEEK_SET);
            if (!(cd = (unsigned char *)malloc(csz + 1)))
                break;
            if (fread(cd, 1, csz, f) == csz && (out = (char *)malloc(usz + 1)) != NULL) {
                if (method == 0 && csz >= usz) {
                    memcpy(out, cd, usz);
                    *outlen = usz;
                } else if (head && method == 8) {
                    inflate_raw((unsigned char *)out, usz, cd, csz, outlen); /* tronqué : erreur normale */
                    if (*outlen != usz) {
                        free(out);
                        out = NULL;
                    }
                } else if (method != 8 || inflate_raw((unsigned char *)out, usz, cd, csz, outlen) != 0 || *outlen != usz) {
                    free(out);
                    out = NULL;
                }
                if (out)
                    out[usz] = 0;
            }
            free(cd);
            break;
        }
        pos += 46 + fnl + exl + cml;
    }
done:
    free(b);
    free(cdb);
    fclose(f);
    return out;
}

/* v1.7.31/1.7.33 : fichier d'une moto qui n'est pas dans son dossier. Ordre (le dossier passe avant,
   comme pour le jeu, c'est l'appelant qui l'essaie d'abord) :
   1. <dossier>.pkz ; 2. les autres .pkz du dossier parent (mods\bikes) ; 3. bikes.pkz du jeu.
   Dans l'archive : « <nom de la moto>/<fichier> » (ou « <fichier> » seul dans <dossier>.pkz).
   Copié dans un fichier temporaire (chemin dans out). */
static char *pkz_search(const char *dir, const char *file, size_t *len, size_t head);
int uipatch_pkz_to_temp(const char *dir, const char *file, char *out, size_t outsz)
{
    char tmp[MAX_PATH], *t;
    size_t l = 0;
    int ok = 0;
    t = pkz_search(dir, file, &l, 0);
    if (t) {
        DWORD n = GetTempPathA(sizeof(tmp), tmp);
        if (n && n < sizeof(tmp)) {
            snprintf(out, outsz, "%sMXRP_Ragdoll_%s", tmp, file);
            ok = write_all(out, t, l);
        }
        free(t);
    }
    return ok;
}

/* v1.8.19 : premiers n octets d'un fichier d'une moto : dossier d'abord, puis les .pkz (même ordre) */
int uipatch_bike_file_head(const char *dir, const char *file, unsigned char *buf, size_t n)
{
    char path[MAX_PATH * 2], *t;
    size_t l = 0;
    FILE *f;
    snprintf(path, sizeof(path), "%s\\%s", dir, file);
    f = fopen(path, "rb");
    if (f) {
        size_t got = fread(buf, 1, n, f);
        fclose(f);
        if (got == n)
            return 1;
    }
    t = pkz_search(dir, file, &l, n);
    if (t && l >= n) {
        memcpy(buf, t, n);
        free(t);
        return 1;
    }
    free(t);
    return 0;
}

static char *pkz_search(const char *dir, const char *file, size_t *len, size_t head)
{
    char pk[MAX_PATH * 2], want[300], parent[MAX_PATH * 2], *c, *t = NULL, *sl;
    const char *name = strrchr(dir, '\\'), *n2 = strrchr(dir, '/');
    if (n2 > name)
        name = n2;
    name = name ? name + 1 : dir;
    snprintf(want, sizeof(want), "%s/%s", name, file);
    for (c = want; *c; ++c)
        *c = *c == '\\' ? '/' : (char)tolower((unsigned char)*c);
    /* 1 */
    snprintf(pk, sizeof(pk), "%s.pkz", dir);
    t = pkz_stream_find_ex(pk, want, len, head);
    if (!t) {
        char w2[64];
        snprintf(w2, sizeof(w2), "%s", file);
        for (c = w2; *c; ++c)
            *c = (char)tolower((unsigned char)*c);
        t = pkz_stream_find_ex(pk, w2, len, head);
    }
    /* 2 */
    snprintf(parent, sizeof(parent), "%s", dir);
    sl = strrchr(parent, '\\');
    if (strrchr(parent, '/') > sl)
        sl = strrchr(parent, '/');
    if (!t && sl) {
        WIN32_FIND_DATAA fd;
        HANDLE h;
        *sl = 0;
        snprintf(pk, sizeof(pk), "%s\\*.pkz", parent);
        h = FindFirstFileA(pk, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                snprintf(pk, sizeof(pk), "%s\\%s", parent, fd.cFileName);
                t = pkz_stream_find_ex(pk, want, len, head);
            } while (!t && FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    /* 3 */
    if (!t) {
        char exe[MAX_PATH], *s;
        if (GetModuleFileNameA(NULL, exe, sizeof(exe)) && (s = strrchr(exe, '\\')) != NULL) {
            *s = 0;
            snprintf(pk, sizeof(pk), "%s\\bikes.pkz", exe);
            t = pkz_stream_find_ex(pk, want, len, head);
        }
    }
    return t;
}

int uipatch_font(const char *dest)
{
    char exe[MAX_PATH], pk[MAX_PATH], *s, *t;
    unsigned char *z;
    size_t zl = 0, l = 0;
    int ok = 0;
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe)) || !(s = strrchr(exe, '\\')))
        return 0;
    *s = 0;
    snprintf(pk, sizeof(pk), "%s\\misc.pkz", exe);
    z = read_all(pk, &zl);
    if (!z)
        return 0;
    t = pkz_extract(z, zl, "misc/misc.fnt", &l);
    if (t) {
        ok = write_all(dest, t, l);
        free(t);
    }
    free(z);
    return ok;
}

void uipatch_report(void)
{
    if (g_log)
        g_log("menus : %ld fichier(s) de menu ouverts par le jeu depuis le lancement du plugin, %ld donne(s) avec le bouton",
              (long)g_seen, (long)g_served);
}

#else
int uipatch_install(const char *save_path, UiPatchLogFn log) { (void)save_path; (void)log; return 0; }
void uipatch_report(void) {}
int uipatch_font(const char *dest) { (void)dest; return 0; }
int uipatch_pkz_to_temp(const char *dir, const char *file, char *out, size_t outsz) { (void)dir; (void)file; (void)out; (void)outsz; return 0; }
int uipatch_bike_file_head(const char *dir, const char *file, unsigned char *buf, size_t n) { (void)dir; (void)file; (void)buf; (void)n; return 0; }
#endif

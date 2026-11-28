/*
 * installer.c - installeur de MXRP Ragdoll (un seul .exe à donner aux amis)
 *
 * - trouve MX Bikes (Steam : registre, toutes les bibliothèques Steam), sinon demande le dossier ;
 * - attend que le jeu soit fermé ;
 * - supprime les anciennes versions et copies en double (« MXRP_Ragdoll (1).dlo », *.ancien…) ;
 * - installe MXRP_Ragdoll.dlo et le LISEZ-MOI, garde les réglages (MXRP_Ragdoll.ini) ou met ceux de base ;
 * - v1.7.26 : police : copiée depuis misc.pkz DU JOUEUR (le fichier du jeu n'est pas dans l'installeur).
 *   Le jeu n'a pas le droit d'écrire dans Program Files : le plugin ne pouvait pas le faire lui-même.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/inflate.h"

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
    b = n > 0 && n < (256L << 20) ? (unsigned char *)malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b)
        *len = (size_t)n;
    return b;
}
static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24); }
/* une entrée d'une archive .pkz (zip, méthode 0 ou 8) */
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
            if (data + csz > zl || usz > (16UL << 20) || !(out = (char *)malloc(usz + 1)))
                return NULL;
            if (method == 0 && csz == usz) {
                memcpy(out, z + data, usz);
                *outlen = usz;
            } else if (method != 8 || inflate_raw((unsigned char *)out, usz, z + data, csz, outlen) != 0 || *outlen != usz) {
                free(out);
                return NULL;
            }
            return out;
        }
        cd += 46 + fnl + exl + cml;
    }
    return NULL;
}
/* police du plugin : la police du jeu (misc/misc.fnt), prise dans le misc.pkz du joueur */
static int install_font(const char *dir, const char *dest)
{
    char pk[MAX_PATH * 2];
    unsigned char *z;
    char *t;
    size_t zl = 0, l = 0;
    int ok = 0;
    if (GetFileAttributesA(dest) != INVALID_FILE_ATTRIBUTES)
        return 1;
    snprintf(pk, sizeof(pk), "%s\\misc.pkz", dir);
    if (!(z = read_all(pk, &zl)))
        return 0;
    if ((t = pkz_extract(z, zl, "misc/misc.fnt", &l)) != NULL) {
        FILE *f = fopen(dest, "wb");
        if (f) {
            ok = fwrite(t, 1, l, f) == l;
            ok = fclose(f) == 0 && ok;
        }
        free(t);
    }
    free(z);
    return ok;
}

#ifndef VER
#define VER "?"
#endif
#ifndef TITLE_NAME
#define TITLE_NAME "MXRP Ragdoll " VER
#endif
#define TITLE TITLE_NAME " - setup"

static int is_game_dir(const char *d)
{
    char p[MAX_PATH * 2];
    snprintf(p, sizeof(p), "%s\\mxbikes.exe", d);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static int reg_str(HKEY root, const char *key, const char *val, char *out, DWORD n, REGSAM extra)
{
    HKEY h;
    DWORD t, sz = n;
    if (RegOpenKeyExA(root, key, 0, KEY_READ | extra, &h) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(h, val, NULL, &t, (BYTE *)out, &sz) != ERROR_SUCCESS || t != REG_SZ) {
        RegCloseKey(h);
        return 0;
    }
    out[n - 1] = 0;
    RegCloseKey(h);
    return 1;
}

static void fix_slashes(char *s)
{
    for (; *s; ++s)
        if (*s == '/')
            *s = '\\';
}

/* v1.8.43 : un dossier de bibliothèque Steam contient-il MX Bikes ? D'abord le manifeste du jeu
   (steamapps\appmanifest_655500.acf, ligne "installdir"), puis le nom habituel « MX Bikes » */
static int try_library(const char *lib, char *out)
{
    char p[MAX_PATH * 2], line[1024];
    FILE *f;
    snprintf(p, sizeof(p), "%s\\steamapps\\appmanifest_655500.acf", lib);
    fix_slashes(p);
    f = fopen(p, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *a = strstr(line, "\"installdir\""), *b, *e;
            if (!a)
                continue;
            b = strchr(a + 12, '"');
            e = b ? strchr(b + 1, '"') : NULL;
            if (b && e && e > b + 1) {
                *e = 0;
                snprintf(out, MAX_PATH, "%s\\steamapps\\common\\%s", lib, b + 1);
                fix_slashes(out);
                if (is_game_dir(out)) {
                    fclose(f);
                    return 1;
                }
            }
        }
        fclose(f);
    }
    snprintf(out, MAX_PATH, "%s\\steamapps\\common\\MX Bikes", lib);
    fix_slashes(out);
    return is_game_dir(out);
}

/* bibliothèques Steam : steamapps\libraryfolders.vdf (et l'ancien config\libraryfolders.vdf),
   lignes "path"  "D:\\SteamLibrary" (anciens formats : "1"  "D:\\SteamLibrary") */
static int try_steam(const char *steam, char *out)
{
    static const char *vdf[] = { "steamapps\\libraryfolders.vdf", "config\\libraryfolders.vdf" };
    char p[MAX_PATH * 2], line[1024];
    int v;
    if (try_library(steam, out))
        return 1;
    for (v = 0; v < 2; ++v) {
        FILE *f;
        snprintf(p, sizeof(p), "%s\\%s", steam, vdf[v]);
        fix_slashes(p);
        f = fopen(p, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof(line), f)) {
            char *q[4], lib[MAX_PATH], *a;
            int nq = 0, k = 0;
            for (a = line; *a && nq < 4; ++a)
                if (*a == '"' && (a == line || a[-1] != '\\'))
                    q[nq++] = a;
            if (nq < 4)
                continue;
            /* clé "path" ou numéro de bibliothèque ; valeur qui ressemble à un chemin (X:) */
            if (!(q[1] - q[0] == 5 && !strncmp(q[0] + 1, "path", 4)) && !(q[0][1] >= '0' && q[0][1] <= '9'))
                continue;
            for (a = q[2] + 1; a < q[3] && k < MAX_PATH - 1; ++a) {
                if (a[0] == '\\' && a[1] == '\\')
                    ++a;
                lib[k++] = *a;
            }
            lib[k] = 0;
            if (k < 3 || lib[1] != ':')
                continue;
            if (try_library(lib, out)) {
                fclose(f);
                return 1;
            }
        }
        fclose(f);
    }
    return 0;
}

/* v1.8.43 : la version Steam d'abord (dossier de Steam et ses bibliothèques) ; la clé de
   désinstallation de Steam ensuite ; puis les emplacements habituels sur chaque disque */
static int find_game(char *out)
{
    char s[MAX_PATH];
    char d;
    if (reg_str(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", s, sizeof(s), 0) && (fix_slashes(s), try_steam(s, out)))
        return 1;
    if (reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath", s, sizeof(s), KEY_WOW64_32KEY) && try_steam(s, out))
        return 1;
    if (reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath", s, sizeof(s), KEY_WOW64_64KEY) && try_steam(s, out))
        return 1;
    if (reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 655500",
                "InstallLocation", out, MAX_PATH, KEY_WOW64_64KEY) && is_game_dir(out))
        return 1;
    if (reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 655500",
                "InstallLocation", out, MAX_PATH, KEY_WOW64_32KEY) && is_game_dir(out))
        return 1;
    for (d = 'C'; d <= 'Z'; ++d) {
        static const char *where[] = { "%c:\\Program Files (x86)\\Steam", "%c:\\Program Files\\Steam", "%c:\\Steam",
                                       "%c:\\SteamLibrary", "%c:\\Games\\Steam", "%c:\\Jeux\\Steam" };
        int w;
        char root[8];
        snprintf(root, sizeof(root), "%c:\\", d);
        if (GetDriveTypeA(root) != DRIVE_FIXED)
            continue;
        for (w = 0; w < 6; ++w) {
            snprintf(s, sizeof(s), where[w], d);
            if (try_steam(s, out))
                return 1;
        }
    }
    return 0;
}

static int browse(char *out)
{
    BROWSEINFOA bi;
    LPITEMIDLIST id;
    memset(&bi, 0, sizeof(bi));
    bi.lpszTitle = "Choose the MX Bikes folder (the one that contains mxbikes.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    id = SHBrowseForFolderA(&bi);
    if (!id)
        return 0;
    SHGetPathFromIDListA(id, out);
    CoTaskMemFree(id);
    return 1;
}

/* v1.8.199 : un mxb_ragdoll*.dlo n'est supprimé que si c'est bien notre ancien plugin (avant le
   renommage MXRP Ragdoll de la v0.41.30) : il contient nos noms de fichiers d'alors. Le fichier
   d'un autre auteur qui porterait le même nom n'est jamais touché. */
static int mem_has(const char *buf, size_t n, const char *pat)
{
    size_t k = strlen(pat), i;
    if (k == 0 || n < k)
        return 0;
    for (i = 0; i + k <= n; ++i)
        if (buf[i] == pat[0] && !memcmp(buf + i, pat, k))
            return 1;
    return 0;
}

static int old_dlo_is_ours(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    char *buf;
    int ok = 0;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    size = GetFileSize(h, NULL);
    if (size != INVALID_FILE_SIZE && size > 0 && size < 64u * 1024u * 1024u && (buf = (char *)malloc(size)) != NULL) {
        if (ReadFile(h, buf, size, &got, NULL) && got == size)
            ok = mem_has(buf, size, "mxb_ragdoll.fnt") && mem_has(buf, size, "plugins\\mxb_ragdoll.ini");
        free(buf);
    }
    CloseHandle(h);
    return ok;
}

static int game_running(void)
{
    PROCESSENTRY32 pe;
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    int r = 0;
    if (sn == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof(pe);
    if (Process32First(sn, &pe))
        do
            if (_stricmp(pe.szExeFile, "mxbikes.exe") == 0)
                r = 1;
        while (!r && Process32Next(sn, &pe));
    CloseHandle(sn);
    return r;
}

static int res_write(int id, const char *path)
{
    HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(id), (LPCSTR)RT_RCDATA);
    HGLOBAL g;
    DWORD n, w = 0;
    HANDLE h;
    char tmp[MAX_PATH * 2];
    if (!r || !(g = LoadResource(NULL, r)))
        return 0;
    n = SizeofResource(NULL, r);
    snprintf(tmp, sizeof(tmp), "%s.nouveau", path);
    h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    WriteFile(h, LockResource(g), n, &w, NULL);
    CloseHandle(h);
    if (w != n || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        return 0;
    }
    return 1;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    char dir[MAX_PATH] = "", plug[MAX_PATH * 2], p[MAX_PATH * 3], msg[2048], moved[1024] = "";
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    int nmoved = 0, font_ok = 0;
    (void)hi; (void)hp; (void)cmd; (void)show;
    CoInitialize(NULL);

    /* v1.8.43 : version Steam trouvée toute seule : installée directement (le dossier est écrit
       dans le message de fin). Pas trouvée : on demande le dossier. */
    /* v1.8.44 : en anglais ; le dossier trouvé est montré, avec le choix d'un autre dossier */
    if (find_game(dir)) {
        snprintf(msg, sizeof(msg),
                 "MX Bikes (Steam) was found here:\n\n%s\n\nYes: install " TITLE_NAME " in this folder\nNo: choose another folder\nCancel: quit",
                 dir);
        switch (MessageBoxA(NULL, msg, TITLE, MB_YESNOCANCEL | MB_ICONQUESTION)) {
        case IDYES: break;
        case IDNO: dir[0] = 0; break;
        default: return 0;
        }
    } else {
        MessageBoxA(NULL, "The MX Bikes folder (Steam) was not found automatically.\n\nPlease choose the folder that contains mxbikes.exe.", TITLE, MB_ICONINFORMATION);
        dir[0] = 0;
    }
    while (!dir[0] || !is_game_dir(dir)) {
        if (dir[0])
            MessageBoxA(NULL, "This folder does not contain mxbikes.exe.", TITLE, MB_ICONWARNING);
        dir[0] = 0;
        if (!browse(dir))
            return 0;
    }

    while (game_running())
        if (MessageBoxA(NULL, "MX Bikes is running.\nClose the game, then click Retry.", TITLE,
                        MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            return 0;

    snprintf(plug, sizeof(plug), "%s\\plugins", dir);
    CreateDirectoryA(plug, NULL);

    /* anciennes versions et copies en double : supprimées (MXRP_Ragdoll.dlo est remplacé juste après) */
    {
        static const char *pat[] = { "MXRP_Ragdoll*.dlo", "MXRP_Ragdoll*.dlo.ancien", "MXRP_Ragdoll*.dlo.nouveau", "mxb_ragdoll*.dlo" };
        int pi;
        for (pi = 0; pi < 4; ++pi) {
    snprintf(p, sizeof(p), "%s\\%s", plug, pat[pi]);
    fh = FindFirstFileA(p, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            char from[MAX_PATH * 3];
            if (_stricmp(fd.cFileName, "MXRP_Ragdoll.dlo") == 0)
                continue;
            snprintf(from, sizeof(from), "%s\\%s", plug, fd.cFileName);
            if (pi == 3 && !old_dlo_is_ours(from))
                continue; /* v1.8.199 : pas notre ancien plugin : on n'y touche pas */
            if (DeleteFileA(from)) {
                ++nmoved;
                if (strlen(moved) + strlen(fd.cFileName) + 8 < sizeof(moved)) {
                    strcat(moved, "\n  ");
                    strcat(moved, fd.cFileName);
                }
            }
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
        }
    }

    snprintf(p, sizeof(p), "%s\\MXRP_Ragdoll.dlo", plug);
    if (!res_write(1, p)) {
        snprintf(msg, sizeof(msg),
                 "Could not write:\n%s\n\nThe game may still be running, or the setup must be run as administrator.", p);
        MessageBoxA(NULL, msg, TITLE, MB_ICONERROR);
        return 1;
    }
    snprintf(p, sizeof(p), "%s\\MXRP_Ragdoll_LISEZ-MOI_README.txt", plug);
    res_write(2, p);
    snprintf(p, sizeof(p), "%s\\MXRP_Ragdoll.ini", plug);
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES)
        res_write(3, p); /* réglages de base, seulement s'il n'en a pas */
    snprintf(p, sizeof(p), "%s\\MXRP_Ragdoll.fnt", plug);
    font_ok = install_font(dir, p);

    snprintf(msg, sizeof(msg),
             TITLE_NAME " is installed!\n\n%s\n\nYour settings (MXRP_Ragdoll.ini) are kept.\nPlugin text (font): %s%s%s\n\n"
             "Start MX Bikes: the \"Plugin Ragdoll\" button is in the menus.",
             plug, font_ok ? "OK" : "NOT FOUND (misc.pkz missing?)", nmoved ? "\n\nOld versions removed:" : "", moved);
    MessageBoxA(NULL, msg, TITLE, MB_ICONINFORMATION);
    return 0;
}

/*
 * profsave.c - v1.8.105 : sauvegarde automatique des profils MX Bikes au lancement du jeu
 *
 * Un joueur a perdu son profil (manette, tenue, équipement remis par défaut). Le plugin n'écrit
 * jamais dans les profils, mais un plantage du jeu pendant qu'il enregistre peut laisser un
 * profile.ini abîmé. Au lancement, on COPIE (lecture seule côté profil) chaque profil dans
 *   <Documents\PiBoSo\MX Bikes>\MXRP_Ragdoll\sauvegarde_profils\<profil>\1, 2, 3 (1 = la plus récente)
 * et une fois pour toutes dans ...\<profil>\premiere. Seulement si le profil a changé depuis la
 * dernière copie (relancer le jeu ne pousse pas les bonnes copies dehors).
 * Copié : les fichiers du profil et les dossiers cockpit et setups (pas tracks ni trainers, énormes).
 * Fait dans un fil à part : rien de plus lent au lancement.
 */
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "profsave.h"

#define PS_FILE_MAX (4u * 1024u * 1024u)   /* fichier plus gros : pas copié */
#define PS_TOTAL_MAX (40u * 1024u * 1024u) /* par profil */

static char g_save[600], g_root[700];
static ProfsaveLogFn g_log = NULL;

static int under_root(const char *p)
{
    size_t l = strlen(g_root);
    return l > 10 && !_strnicmp(p, g_root, l) && (p[l] == '\\' || p[l] == 0);
}

/* supprime un dossier de NOS copies (jamais ailleurs) */
static void rm_tree(const char *dir)
{
    char pat[900], p[900];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (!under_root(dir))
        return;
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            snprintf(p, sizeof(p), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                rm_tree(p);
            else {
                SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(p);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

/* copie src -> dst ; sous-dossiers seulement si deep (cockpit, setups) ; rend le nombre de fichiers */
static int copy_dir(const char *src, const char *dst, int top, unsigned long long *total)
{
    char pat[900], a[900], b[900];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;
    if (!under_root(dst))
        return 0;
    CreateDirectoryA(dst, NULL);
    snprintf(pat, sizeof(pat), "%s\\*", src);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        snprintf(a, sizeof(a), "%s\\%s", src, fd.cFileName);
        snprintf(b, sizeof(b), "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (top && (!_stricmp(fd.cFileName, "cockpit") || !_stricmp(fd.cFileName, "setups")))
                n += copy_dir(a, b, 0, total);
            else if (!top)
                n += copy_dir(a, b, 0, total); /* (sous-dossiers de cockpit / setups) */
            continue;
        }
        if (fd.nFileSizeHigh || fd.nFileSizeLow > PS_FILE_MAX || *total + fd.nFileSizeLow > PS_TOTAL_MAX)
            continue;
        if (CopyFileA(a, b, FALSE)) {
            *total += fd.nFileSizeLow;
            ++n;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* même contenu ? (fichier absent des deux côtés : pareil) */
static int same_file(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int same = 1;
    char x[8192], y[8192];
    size_t na, nb;
    if (!fa || !fb) {
        same = !fa && !fb;
        if (fa)
            fclose(fa);
        if (fb)
            fclose(fb);
        return same;
    }
    do {
        na = fread(x, 1, sizeof(x), fa);
        nb = fread(y, 1, sizeof(y), fb);
        if (na != nb || memcmp(x, y, na)) {
            same = 0;
            break;
        }
    } while (na);
    fclose(fa);
    fclose(fb);
    return same;
}

static void write_readme(void)
{
    char p[800];
    FILE *f;
    snprintf(p, sizeof(p), "%s\\LISEZ-MOI_README.txt", g_root);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES || !(f = fopen(p, "w")))
        return;
    fputs("MXRP Ragdoll - sauvegarde automatique de tes profils MX Bikes\n"
          "==============================================================\n\n"
          "FR : a chaque lancement du jeu, le plugin COPIE ton profil ici (il n'ecrit jamais dans le profil).\n"
          "  <profil>\\1 = la copie la plus recente, 2 et 3 = les precedentes, premiere = la toute premiere.\n"
          "  Une nouvelle copie n'est faite que si le profil a change.\n"
          "  Profil perdu (manette, tenue, equipement remis par defaut) ? Ferme le jeu, puis copie le contenu\n"
          "  d'un de ces dossiers dans Documents\\PiBoSo\\MX Bikes\\profiles\\<profil>\\ (remplace les fichiers).\n"
          "  Prends 2 ou 3 si le jeu a deja ete relance avec le profil remis a zero.\n\n"
          "EN: every time the game starts, the plugin COPIES your profile here (it never writes to the profile).\n"
          "  <profile>\\1 = newest copy, 2 and 3 = older ones, premiere = the very first one.\n"
          "  A new copy is only made when the profile has changed.\n"
          "  Lost your profile (controls, outfit, gear back to default)? Close the game, then copy the content\n"
          "  of one of these folders into Documents\\PiBoSo\\MX Bikes\\profiles\\<profile>\\ (replace the files).\n"
          "  Use 2 or 3 if the game was already restarted with the reset profile.\n",
          f);
    fclose(f);
}

static void save_profile(const char *name)
{
    char src[800], dir[800], d1[800], d2[800], d3[800], dp[800], a[900], b[900];
    unsigned long long total = 0;
    int n;
    snprintf(src, sizeof(src), "%s\\profiles\\%s", g_save, name);
    snprintf(a, sizeof(a), "%s\\profile.ini", src);
    if (GetFileAttributesA(a) == INVALID_FILE_ATTRIBUTES)
        return; /* pas un profil */
    snprintf(dir, sizeof(dir), "%s\\%s", g_root, name);
    snprintf(d1, sizeof(d1), "%s\\1", dir);
    snprintf(d2, sizeof(d2), "%s\\2", dir);
    snprintf(d3, sizeof(d3), "%s\\3", dir);
    snprintf(dp, sizeof(dp), "%s\\premiere", dir);
    CreateDirectoryA(dir, NULL);
    if (GetFileAttributesA(dp) == INVALID_FILE_ATTRIBUTES) {
        total = 0;
        n = copy_dir(src, dp, 1, &total);
        if (g_log)
            g_log("profils : premiere copie de \"%s\" (%d fichiers, %.1f Mo)", name, n, total / 1048576.0);
    }
    /* rien de changé depuis la dernière copie : on garde les 3 copies telles quelles */
    {
        static const char *k[] = { "profile.ini", "controls.txt", "records.ini" };
        int i, same = GetFileAttributesA(d1) != INVALID_FILE_ATTRIBUTES;
        for (i = 0; i < 3 && same; ++i) {
            snprintf(a, sizeof(a), "%s\\%s", src, k[i]);
            snprintf(b, sizeof(b), "%s\\%s", d1, k[i]);
            same = same_file(a, b);
        }
        if (same)
            return;
    }
    rm_tree(d3);
    MoveFileA(d2, d3);
    MoveFileA(d1, d2);
    total = 0;
    n = copy_dir(src, d1, 1, &total);
    if (g_log)
        g_log("profils : copie de \"%s\" sauvegardee (%d fichiers, %.1f Mo) dans %s", name, n,
              total / 1048576.0, g_root);
}

static DWORD WINAPI save_thread(LPVOID p)
{
    char pat[800];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    (void)p;
    CreateDirectoryA(g_root, NULL);
    write_readme();
    snprintf(pat, sizeof(pat), "%s\\profiles\\*", g_save);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
            save_profile(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

void profsave_start(const char *save_path, const char *own_dir, ProfsaveLogFn log)
{
    size_t l;
    if (!save_path || !save_path[0])
        return;
    g_log = log;
    snprintf(g_save, sizeof(g_save), "%s", save_path);
    l = strlen(g_save);
    while (l && (g_save[l - 1] == '\\' || g_save[l - 1] == '/'))
        g_save[--l] = 0;
    if (own_dir && own_dir[0]) {
        /* v1.8.106 : dans Documents\PiBoSo\MX Bikes\MXRP_Ragdoll\ */
        l = strlen(own_dir);
        snprintf(g_root, sizeof(g_root), "%.*s\\sauvegarde_profils", (int)(own_dir[l - 1] == '\\' ? l - 1 : l), own_dir);
    } else
        snprintf(g_root, sizeof(g_root), "%s\\MXRP_Ragdoll_sauvegarde_profils", g_save);
    CloseHandle(CreateThread(NULL, 0, save_thread, NULL, 0, NULL));
}
#else
#include "profsave.h"
void profsave_start(const char *save_path, const char *own_dir, ProfsaveLogFn log) { (void)save_path; (void)own_dir; (void)log; }
#endif

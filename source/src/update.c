/*
 * update.c - v1.8.172 : mises à jour automatiques (voir update.h)
 */
#include "update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>

#define UPD_HOST L"api.github.com"
#define UPD_PATH L"/repos/avare795/MXRP-ragdoll/releases/latest"
#define UPD_PATH_ALL L"/repos/avare795/MXRP-ragdoll/releases?per_page=3"
#define UPD_MAX_JSON (512 * 1024)
#define UPD_MAX_FILE (32 * 1024 * 1024)

/* v1.8.202 : clé publique (ECDSA P-256) : chaque fichier téléchargé doit être signé avec la clé secrète de
   l'auteur (fichier « .sig » à côté dans la release). Sans signature valide : refusé, même si l'empreinte
   SHA-256 de GitHub correspond (un compte GitHub piraté ne suffit pas pour pousser un faux plugin). */
static const unsigned char k_pub_x[32] = { 0x60, 0xd7, 0x8d, 0xbe, 0x23, 0x97, 0xfb, 0x63, 0x10, 0x31, 0x91, 0x8a, 0x96, 0x35, 0x11, 0xc2, 0x5c, 0x8f, 0xfe, 0xbc, 0x61, 0x03, 0x75, 0xe9, 0x97, 0xdc, 0x7a, 0xd5, 0x25, 0x3a, 0x07, 0xb5 };
static const unsigned char k_pub_y[32] = { 0x73, 0x48, 0x71, 0x81, 0x44, 0x9d, 0xdc, 0x3e, 0x31, 0x7e, 0x47, 0x3a, 0xba, 0xd3, 0xb1, 0x51, 0xb6, 0x80, 0x11, 0x87, 0xab, 0xf7, 0x05, 0x44, 0xd6, 0x02, 0x03, 0xfc, 0x17, 0x46, 0x85, 0x24 };

static UpdateLogFn g_log = NULL;
static char g_cur[32];
static volatile LONG g_msg = 0;
static char g_msg_ver[32];
static char g_installer[MAX_PATH];
static volatile LONG g_run_installer = 0;
static int g_test = 0;
static int g_ask = 1;
static volatile LONG g_pending = 0; /* v1.8.175 : 1 = attend la réponse du joueur */
static volatile LONG g_answer = -1;
static HANDLE g_answer_ev = NULL;

#define LOG(...)                    \
    do {                            \
        if (g_log)                  \
            g_log(__VA_ARGS__);     \
    } while (0)

/* « 1.8.172 » -> comparable ; 0 si illisible */
static int parse_ver(const char *s, int v[3])
{
    return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}
static int newer(const int a[3], const int b[3])
{
    int i;
    for (i = 0; i < 3; ++i)
        if (a[i] != b[i])
            return a[i] > b[i];
    return 0;
}

/* GET https : tout le corps en mémoire (redirections suivies) */
static unsigned char *http_get(const wchar_t *host, const wchar_t *path, size_t max, size_t *out_n)
{
    HINTERNET s = NULL, c = NULL, r = NULL;
    unsigned char *buf = NULL;
    size_t n = 0, cap = 0;
    DWORD status = 0, sl = sizeof(status), avail;
    *out_n = 0;
    s = WinHttpOpen(L"MXRP-Ragdoll-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(L"MXRP-Ragdoll-updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        goto done;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    if (!(c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0)))
        goto done;
    if (!(r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE)))
        goto done;
    if (!WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r, NULL))
        goto done;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &sl, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        LOG("mise a jour : reponse %lu", (unsigned long)status);
        goto done;
    }
    for (;;) {
        DWORD got = 0;
        avail = 0;
        if (!WinHttpQueryDataAvailable(r, &avail) || !avail)
            break;
        if (n + avail + 1 > max) {
            LOG("mise a jour : fichier trop gros");
            free(buf);
            buf = NULL;
            n = 0;
            goto done;
        }
        if (n + avail + 1 > cap) {
            unsigned char *nb;
            cap = (n + avail + 1) * 2;
            if (cap > max)
                cap = max;
            nb = (unsigned char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                buf = NULL;
                n = 0;
                goto done;
            }
            buf = nb;
        }
        if (!WinHttpReadData(r, buf + n, avail, &got) || !got)
            break;
        n += got;
    }
    if (buf)
        buf[n] = 0;
done:
    if (r)
        WinHttpCloseHandle(r);
    if (c)
        WinHttpCloseHandle(c);
    if (s)
        WinHttpCloseHandle(s);
    *out_n = n;
    return buf;
}

/* https://host/path -> GET */
static unsigned char *http_get_url(const char *url, size_t max, size_t *n)
{
    wchar_t host[256], path[2048];
    const char *h, *p;
    int hl;
    *n = 0;
    if (strncmp(url, "https://", 8))
        return NULL;
    h = url + 8;
    p = strchr(h, '/');
    if (!p)
        return NULL;
    hl = (int)(p - h);
    if (hl <= 0 || hl >= 255 || strlen(p) >= 2047)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, h, hl, host, 255);
    host[hl] = 0;
    MultiByteToWideChar(CP_UTF8, 0, p, -1, path, 2047);
    return http_get(host, path, max, n);
}

static int sha256_raw(const unsigned char *d, size_t n, unsigned char md[32])
{
    BCRYPT_ALG_HANDLE a = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0)
        return 0;
    if (BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0) == 0 && BCryptHashData(h, (PUCHAR)d, (ULONG)n, 0) == 0 &&
        BCryptFinishHash(h, md, 32, 0) == 0)
        ok = 1;
    if (h)
        BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

/* v1.8.202 : signature (64 octets r||s) du fichier, vérifiée avec la clé publique de l'auteur */
static int sig_ok(const unsigned char *d, size_t n, const unsigned char *sig, size_t sn)
{
    BCRYPT_ALG_HANDLE a = NULL;
    BCRYPT_KEY_HANDLE k = NULL;
    unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64], md[32];
    BCRYPT_ECCKEY_BLOB *hd = (BCRYPT_ECCKEY_BLOB *)blob;
    int ok = 0;
    if (sn != 64 || !sha256_raw(d, n, md))
        return 0;
    hd->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hd->cbKey = 32;
    memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), k_pub_x, 32);
    memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32, k_pub_y, 32);
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) != 0)
        return 0;
    if (BCryptImportKeyPair(a, NULL, BCRYPT_ECCPUBLIC_BLOB, &k, blob, (ULONG)sizeof(blob), 0) == 0 &&
        BCryptVerifySignature(k, NULL, md, 32, (PUCHAR)sig, 64, 0) == 0)
        ok = 1;
    if (k)
        BCryptDestroyKey(k);
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

static int sha256_hex(const unsigned char *d, size_t n, char out[65])
{
    BCRYPT_ALG_HANDLE a = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char md[32];
    int ok = 0, i;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0)
        return 0;
    if (BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0) == 0 && BCryptHashData(h, (PUCHAR)d, (ULONG)n, 0) == 0 &&
        BCryptFinishHash(h, md, 32, 0) == 0)
        ok = 1;
    if (h)
        BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    for (i = 0; ok && i < 32; ++i)
        sprintf(out + 2 * i, "%02x", md[i]);
    return ok;
}

/* valeur texte "key":"..." après p, avant lim */
static int json_str(const char *p, const char *lim, const char *key, char *out, size_t n)
{
    char pat[64];
    const char *q, *e;
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    q = strstr(p, pat);
    if (!q || (lim && q >= lim))
        return 0;
    q += strlen(pat);
    while (*q == ' ')
        ++q;
    if (*q != '"')
        return 0;
    ++q;
    e = strchr(q, '"');
    if (!e || (size_t)(e - q) >= n)
        return 0;
    memcpy(out, q, (size_t)(e - q));
    out[e - q] = 0;
    return 1;
}

/* asset dont le nom commence par prefix et finit par suffix : version (entre les deux), url, sha256 */
static char g_asset_name[160]; /* v1.8.202 : nom du dernier asset trouvé (pour son .sig) */
static int find_asset(const char *js, const char *prefix, const char *suffix, char *ver, size_t vn, char *url,
                      size_t un, char *sha, size_t sn)
{
    const char *p = js;
    char name[160];
    while ((p = strstr(p, "\"name\":")) != NULL) {
        const char *next = strstr(p + 7, "\"browser_download_url\":");
        size_t pl = strlen(prefix), sl = strlen(suffix), nl;
        if (!json_str(p, NULL, "name", name, sizeof(name))) {
            p += 7;
            continue;
        }
        nl = strlen(name);
        if (next && nl > pl + sl - 1 && !strncmp(name, prefix, pl) && !strcmp(name + nl - sl, suffix)) {
            char dig[100];
            const char *lim = strstr(next + 23, "\"name\":");
            if (ver) {
                size_t l = nl - pl - sl;
                if (l >= vn)
                    return 0;
                memcpy(ver, name + pl, l);
                ver[l] = 0;
            }
            if (!json_str(next, lim, "browser_download_url", url, un))
                return 0;
            if (!json_str(p, next, "digest", dig, sizeof(dig)) || strncmp(dig, "sha256:", 7) || strlen(dig + 7) != 64)
                return 0;
            snprintf(sha, sn, "%s", dig + 7);
            snprintf(g_asset_name, sizeof(g_asset_name), "%s", name);
            return 1;
        }
        p += 7;
    }
    return 0;
}

static unsigned char *download_checked(const char *js, const char *url, const char *sha, size_t *n)
{
    char got[65], sname[200], surl[1024], ssha[80];
    size_t sn = 0;
    unsigned char *sig, *d;
    /* v1.8.202 : d'abord la signature de l'auteur (asset « <nom>.sig ») */
    snprintf(sname, sizeof(sname), "%s.sig", g_asset_name);
    if (!find_asset(js, sname, "", NULL, 0, surl, sizeof(surl), ssha, sizeof(ssha)) || strcmp(g_asset_name, sname)) {
        LOG("mise a jour : pas de signature (%s) : fichier refuse", sname);
        return NULL;
    }
    sig = http_get_url(surl, 4096, &sn);
    if (!sig) {
        LOG("mise a jour : signature pas telechargee : fichier refuse");
        return NULL;
    }
    d = http_get_url(url, UPD_MAX_FILE, n);
    if (!d || *n < 1024) {
        LOG("mise a jour : telechargement rate");
        free(d);
        free(sig);
        return NULL;
    }
    if (!sha256_hex(d, *n, got) || _stricmp(got, sha)) {
        LOG("mise a jour : empreinte differente : fichier refuse");
        free(d);
        free(sig);
        return NULL;
    }
    if (d[0] != 'M' || d[1] != 'Z') {
        LOG("mise a jour : ce n'est pas un programme Windows : refuse");
        free(d);
        free(sig);
        return NULL;
    }
    if (!sig_ok(d, *n, sig, sn)) {
        LOG("mise a jour : signature de l'auteur invalide : fichier refuse");
        free(d);
        free(sig);
        return NULL;
    }
    free(sig);
    LOG("mise a jour : signature de l'auteur verifiee");
    return d;
}

static int write_all(const char *path, const unsigned char *d, size_t n)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w = 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(h, d, (DWORD)n, &w, NULL) && w == n;
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok)
        DeleteFileA(path);
    return ok;
}

static DWORD WINAPI update_thread(LPVOID unused)
{
    char me[MAX_PATH], tmp[MAX_PATH + 8], old[MAX_PATH + 8], ver[32], url[1024], sha[80];
    unsigned char *js, *d;
    size_t n;
    int vc[3], vn[3];
    HMODULE mod = NULL;
    (void)unused;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void *)update_thread, &mod) ||
        !GetModuleFileNameA(mod, me, sizeof(me)))
        return 0;
    snprintf(old, sizeof(old), "%s.old", me);
    DeleteFileA(old); /* ancien plugin remplacé au lancement d'avant */
    snprintf(tmp, sizeof(tmp), "%s.new", me);
    DeleteFileA(tmp); /* mise à jour pas installée (plus tard) */
    Sleep(8000);      /* le jeu démarre d'abord */
    if (!parse_ver(g_cur, vc))
        return 0;
    /* essai : la liste des releases (pré-releases comprises, la plus récente d'abord) */
    js = http_get(UPD_HOST, g_test ? UPD_PATH_ALL : UPD_PATH, UPD_MAX_JSON, &n);
    if (g_test)
        LOG("mise a jour : mode essai (pre-releases comprises)");
    if (!js) {
        LOG("mise a jour : pas de reponse de GitHub (hors ligne ?)");
        return 0;
    }
    if (!find_asset((const char *)js, "MXRP_Ragdoll_", ".dlo", ver, sizeof(ver), url, sizeof(url), sha, sizeof(sha)) ||
        !parse_ver(ver, vn)) {
        LOG("mise a jour : pas de plugin dans la derniere release");
        free(js);
        return 0;
    }
    if (!newer(vn, vc)) {
        LOG("mise a jour : a jour (%s, derniere release %s)", g_cur, ver);
        free(js);
        return 0;
    }
    LOG("mise a jour : %s disponible (installe : %s), telechargement", ver, g_cur);
    d = download_checked((const char *)js, url, sha, &n);
    if (d && g_ask) {
        /* v1.8.175 : le joueur choisit (Entrée / A : installer, Retour / B : plus tard) */
        snprintf(g_msg_ver, sizeof(g_msg_ver), "%s", ver);
        g_answer_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        InterlockedExchange(&g_pending, 1);
        LOG("mise a jour : %s telechargee, en attente du choix du joueur", ver);
        if (g_answer_ev)
            WaitForSingleObject(g_answer_ev, INFINITE);
        InterlockedExchange(&g_pending, 0);
        if (g_answer != 1) {
            LOG("mise a jour : %s : plus tard (redemande au prochain lancement)", ver);
            InterlockedExchange(&g_msg, 3);
            free(d);
            free(js);
            return 0;
        }
        LOG("mise a jour : %s : installation acceptee", ver);
    }
    if (d) {
        snprintf(tmp, sizeof(tmp), "%s.new", me);
        /* le plugin chargé ne peut pas être écrasé, mais il peut être renommé */
        if (write_all(tmp, d, n) && MoveFileExA(me, old, MOVEFILE_REPLACE_EXISTING)) {
            if (MoveFileExA(tmp, me, MOVEFILE_REPLACE_EXISTING)) {
                LOG("mise a jour : %s installee (au prochain lancement du jeu)", ver);
                snprintf(g_msg_ver, sizeof(g_msg_ver), "%s", ver);
                InterlockedExchange(&g_msg, 1);
                free(d);
                free(js);
                return 0;
            }
            MoveFileExA(old, me, MOVEFILE_REPLACE_EXISTING); /* remis comme avant */
        }
        DeleteFileA(tmp);
        LOG("mise a jour : dossier plugins non modifiable (erreur %lu) : installateur", (unsigned long)GetLastError());
        free(d);
    }
    /* sans droit d'écriture : l'installateur de la release, ouvert en quittant le jeu */
    {
        char iurl[1024], isha[80], dir[MAX_PATH];
        if (find_asset((const char *)js, "MXRP_Ragdoll_Installer", ".exe", NULL, 0, iurl, sizeof(iurl), isha, sizeof(isha)) &&
            (d = download_checked((const char *)js, iurl, isha, &n)) != NULL) {
            if (GetTempPathA(sizeof(dir), dir) &&
                snprintf(g_installer, sizeof(g_installer), "%sMXRP_Ragdoll_Installer_%s.exe", dir, ver) < (int)sizeof(g_installer) &&
                write_all(g_installer, d, n)) {
                InterlockedExchange(&g_run_installer, 1);
                snprintf(g_msg_ver, sizeof(g_msg_ver), "%s", ver);
                InterlockedExchange(&g_msg, 2);
                LOG("mise a jour : installateur %s pret, il s'ouvrira en quittant le jeu", ver);
            }
            free(d);
        }
    }
    free(js);
    return 0;
}

void update_start(const char *version, UpdateLogFn log)
{
    HANDLE t;
    g_log = log;
    snprintf(g_cur, sizeof(g_cur), "%s", version ? version : "");
    t = CreateThread(NULL, 0, update_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}

void update_set_test(int on) { g_test = on; }
void update_set_ask(int on) { g_ask = on; }

int update_pending(char *ver, int n)
{
    if (!g_pending)
        return 0;
    if (ver && n > 0)
        snprintf(ver, (size_t)n, "%s", g_msg_ver);
    return 1;
}

void update_answer(int install)
{
    if (!g_pending)
        return;
    g_answer = install ? 1 : 0;
    if (g_answer_ev)
        SetEvent(g_answer_ev);
}

int update_take_message(char *ver, int n)
{
    LONG m = InterlockedExchange(&g_msg, 0);
    if (m && ver && n > 0)
        snprintf(ver, (size_t)n, "%s", g_msg_ver);
    return (int)m;
}

void update_on_shutdown(void)
{
    if (InterlockedExchange(&g_run_installer, 0) && g_installer[0])
        ShellExecuteA(NULL, "open", g_installer, NULL, NULL, SW_SHOWNORMAL);
}

#else
void update_start(const char *version, UpdateLogFn log)
{
    (void)version;
    (void)log;
}
int update_take_message(char *ver, int n)
{
    (void)ver;
    (void)n;
    return 0;
}
void update_on_shutdown(void) {}
void update_set_test(int on) { (void)on; }
void update_set_ask(int on) { (void)on; }
int update_pending(char *ver, int n)
{
    (void)ver;
    (void)n;
    return 0;
}
void update_answer(int install) { (void)install; }
#endif

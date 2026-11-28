/*
 * terrain.c - relief de la piste, lu dans la mémoire du jeu (heightfield ODE)
 */
#include "terrain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static TerrainField g_fields[TERRAIN_MAX];
static volatile int g_num_fields = 0;
static volatile int g_ready = 0;
static volatile int g_live_ok = 0; /* grille du jeu connue (hauteur actuelle) */
/* objet ODE (geom) du relief : où est son pointeur vers les données et sa boîte englobante */
static volatile uintptr_t g_hf_ptrloc = 0, g_hf_aabb = 0;
int terrain_geom_info(uintptr_t *ptrloc, uintptr_t *aabb)
{
    if (!g_hf_ptrloc)
        return 0;
    *ptrloc = g_hf_ptrloc;
    *aabb = g_hf_aabb;
    return 1;
}


/* les grilles de l'ancienne piste sont libérées au nettoyage suivant (un autre fil
   pouvait encore les lire) */
static void *g_old_data[TERRAIN_MAX];

size_t terrain_data_size(const TerrainField *f)
{
    size_t e = f->mode == 1 ? 1 : f->mode == 2 ? 2 : 4;
    if (f->nx <= 0 || f->nz <= 0)
        return 0;
    return (size_t)f->nx * (size_t)f->nz * e;
}

static int g_nmod = 0; /* v1.8.120 : cases relevées par les rampes (voir plus bas) */
/* v1.8.127 : copies de la grille du jeu (même taille, même contenu) : le jeu y reprend les hauteurs
   autour de la moto (la rampe disparaissait sous la roue entre deux appels du plugin) */
#define COPY_MAX 4
static uintptr_t g_copy[TERRAIN_MAX][COPY_MAX];
static volatile int g_ncopy[TERRAIN_MAX];
#ifdef _WIN32
static volatile LONG g_copy_state = 0;
#else
static volatile long g_copy_state = 0;
#endif /* 0 pas cherché, 1 en cours, 2 fini */
static char g_copy_msg[512];
static char g_ref_msg[12000]; /* v1.8.134 */
/* v1.8.135 : 2e structure de relief du jeu (celle des roues ?) : même grille 16 bits, son échelle, mode 2 */
#define WREF_MAX 4
static uintptr_t g_wref[WREF_MAX], g_wref_old[WREF_MAX];
static int g_wref_field[WREF_MAX], g_nwref = 0, g_wref_on[WREF_MAX];
/* v1.8.138 : cette structure est l'en-tête d'un 2e relief ODE (celui des roues) : sa hauteur max (9,57 m : la moto
   ne touchait rien au-dessus) et l'objet ODE qui s'en sert (boîte englobante à refaire) */
static float g_wref_maxh[WREF_MAX];
static uintptr_t g_wref_geomref[WREF_MAX][4];
static int g_wref_ngeom[WREF_MAX];
static int (*g_dataoff_fn)(uintptr_t) = NULL;
/* v1.8.140 : hauteur totale de la grille dans les variables du jeu (19,13 m sur Big Air MX : les roues n'ont rien
   au-dessus). Pour aller plus haut : toute la grille (et ses copies) réécrite à l'échelle double, et ces hauteurs,
   les échelles des reliefs ODE et la nôtre doublées. Une fois par piste (pas de retour en arrière). */
#define HREF_MAX 8
static uintptr_t g_href[HREF_MAX];
static int g_nhref = 0;
static int g_rescaled = 0;
static int (*g_dirty_fn)(uintptr_t) = NULL;

void terrain_set_geom_fns(int (*dataoff)(uintptr_t), int (*dirty)(uintptr_t))
{
    g_dataoff_fn = dataoff;
    g_dirty_fn = dirty;
}
/* v1.8.129 : grille du jeu en entiers 16 bits : rien au-dessus du point le plus haut de la piste (sur Big Air MX,
   1,1 m au-dessus du départ : la moto passait à travers la rampe). Pendant qu'il y a des rampes trop hautes,
   ODE lit une grille à nous en flottants (sans limite), tenue à jour avec celle du jeu (ornières) */
static float *g_ff[TERRAIN_MAX];
static short *g_fs[TERRAIN_MAX];
static int g_fswap[TERRAIN_MAX], g_fscan[TERRAIN_MAX];
static float g_bike_x = 0.0f, g_bike_z = 0.0f;
static int g_bike_ok = 0;
static int g_copy_logged = 0;
void terrain_clear(void)
{
    g_nmod = 0; /* (grille libérée avec la piste : rien à remettre) */
    {
        int i;
        for (i = 0; i < TERRAIN_MAX; ++i)
            g_ncopy[i] = 0;
        g_copy_state = 0;
        g_copy_logged = 0;
        g_nwref = 0;
        g_nhref = 0;
        g_rescaled = 0;
        for (i = 0; i < TERRAIN_MAX; ++i) {
            /* (piste libérée par le jeu : on ne touche plus à sa structure ; notre grille reste allouée) */
            g_fswap[i] = 0;
            g_ff[i] = NULL;
            free(g_fs[i]);
            g_fs[i] = NULL;
        }
    }
    g_hf_ptrloc = 0;
    g_hf_aabb = 0;
    int i, n = g_num_fields;
    g_ready = 0;
    g_live_ok = 0;
    g_num_fields = 0;
    for (i = 0; i < TERRAIN_MAX; ++i) {
        free(g_old_data[i]);
        g_old_data[i] = NULL;
    }
    for (i = 0; i < n; ++i)
        if (g_fields[i].owned)
            g_old_data[i] = (void *)g_fields[i].data;
}

void terrain_add(const TerrainField *f)
{
    if (g_num_fields >= TERRAIN_MAX)
        return;
    g_fields[g_num_fields] = *f;
    if (f->live)
        g_live_ok = 1;
    g_num_fields = g_num_fields + 1;
    g_ready = 1;
}

int terrain_ready(void)
{
    return g_ready;
}

static int close_rel(float a, float b, float tol)
{
    float m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
    return fabsf(a - b) <= tol * (m > 1e-6f ? m : 1e-6f);
}

/* Disposition de dxHeightfieldData (ODE, simple précision, 64 bits) */
typedef struct {
    float w, d, sw, sd, aspect, isw, isd, hw, hd, minh, maxh, thick, scale, offset;
    int32_t nw, nd, copy, wrap, mode;
    int32_t pad;
    uint64_t data;
} OdeHeightfieldData; /* 0x58 octets */

/* Variante sans le champ "rapport" (anciennes versions d'ODE) */
typedef struct {
    float w, d, sw, sd, isw, isd, hw, hd, minh, maxh, thick, scale, offset;
    int32_t nw, nd, copy, wrap, mode;
    uint64_t data;
} OdeHeightfieldDataOld; /* 0x50 octets */

static int check_fields(float w, float d, float sw, float sd, float isw, float isd, float hw,
                        float hd, float scale, float offset, int nw, int nd, int wrap, int mode,
                        uint64_t data)
{
    if (!(w >= 5.0f && w <= 100000.0f && d >= 5.0f && d <= 100000.0f))
        return 0;
    if (!close_rel(hw * 2.0f, w, 1e-4f) || !close_rel(hd * 2.0f, d, 1e-4f))
        return 0;
    if (!(sw > 0.0f) || !(sd > 0.0f) || !close_rel(sw * isw, 1.0f, 1e-3f) ||
        !close_rel(sd * isd, 1.0f, 1e-3f))
        return 0;
    if (nw < 2 || nw > 65536 || nd < 2 || nd > 65536)
        return 0;
    if (!close_rel(sw * (float)(nw - 1), w, 1e-3f) || !close_rel(sd * (float)(nd - 1), d, 1e-3f))
        return 0;
    if (mode < 1 || mode > 3 || wrap < 0 || wrap > 1)
        return 0;
    if (data < 0x10000ull || data > 0x00007FFFFFFFFFFFull)
        return 0;
    if (!isfinite(scale) || !isfinite(offset) || scale == 0.0f)
        return 0;
    return 1;
}

int terrain_parse(const void *p, TerrainField *out)
{
    OdeHeightfieldData h;
    OdeHeightfieldDataOld o;
    memcpy(&h, p, sizeof(h));
    memset(out, 0, sizeof(*out));
    if (check_fields(h.w, h.d, h.sw, h.sd, h.isw, h.isd, h.hw, h.hd, h.scale, h.offset, h.nw, h.nd,
                     h.wrap, h.mode, h.data)) {
        out->width = h.w; out->depth = h.d; out->sample_w = h.sw; out->sample_d = h.sd;
        out->half_w = h.hw; out->half_d = h.hd; out->min_h = h.minh; out->max_h = h.maxh;
        out->scale = h.scale; out->offset = h.offset; out->nx = h.nw; out->nz = h.nd;
        out->mode = h.mode; out->data = (const void *)(uintptr_t)h.data;
        return 1;
    }
    memcpy(&o, p, sizeof(o));
    if (check_fields(o.w, o.d, o.sw, o.sd, o.isw, o.isd, o.hw, o.hd, o.scale, o.offset, o.nw, o.nd,
                     o.wrap, o.mode, o.data)) {
        out->width = o.w; out->depth = o.d; out->sample_w = o.sw; out->sample_d = o.sd;
        out->half_w = o.hw; out->half_d = o.hd; out->min_h = o.minh; out->max_h = o.maxh;
        out->scale = o.scale; out->offset = o.offset; out->nx = o.nw; out->nz = o.nd;
        out->mode = o.mode; out->data = (const void *)(uintptr_t)o.data;
        return 2;
    }
    return 0;
}

static float sample_raw(const TerrainField *f, int ix, int iz)
{
    size_t i;
    float v;
    if (ix < 0)
        ix = 0;
    if (iz < 0)
        iz = 0;
    if (ix >= f->nx)
        ix = f->nx - 1;
    if (iz >= f->nz)
        iz = f->nz - 1;
    i = (size_t)ix + (size_t)iz * (size_t)f->nx;
    switch (f->mode) {
    case 1: v = (float)((const unsigned char *)f->data)[i]; break;
    case 2: v = (float)((const short *)f->data)[i]; break;
    default: v = ((const float *)f->data)[i]; break;
    }
    return v * f->scale + f->offset + f->center[1];
}

static int field_height(const TerrainField *f, float x, float z, float *h)
{
    float lx = x - f->center[0] + f->half_w;
    float lz = z - f->center[2] + f->half_d;
    float gx, gz, tx, tz, h00, h10, h01, h11;
    int ix, iz;
    if (lx < 0.0f || lz < 0.0f || lx > f->width || lz > f->depth)
        return 0;
    gx = lx / f->sample_w;
    gz = lz / f->sample_d;
    ix = (int)gx;
    iz = (int)gz;
    tx = gx - (float)ix;
    tz = gz - (float)iz;
    h00 = sample_raw(f, ix, iz);
    h10 = sample_raw(f, ix + 1, iz);
    h01 = sample_raw(f, ix, iz + 1);
    h11 = sample_raw(f, ix + 1, iz + 1);
    *h = (h00 * (1 - tx) + h10 * tx) * (1 - tz) + (h01 * (1 - tx) + h11 * tx) * tz;
    return isfinite(*h);
}

int terrain_live_ready(void)
{
    return g_live_ok;
}

static int g_live_base = 0; /* v1.8.125 : 1 : cases relevées par les rampes lues à leur hauteur d'origine */
static int mod_orig(int field, int idx, float *raw);

static int live_raw(const TerrainField *f, int ix, int iz, float *out)
{
    size_t i, e = f->mode == 1 ? 1 : f->mode == 2 ? 2 : 4;
    union { unsigned char b; short s; float f; } u;
    if (ix < 0) ix = 0;
    if (iz < 0) iz = 0;
    if (ix >= f->nx) ix = f->nx - 1;
    if (iz >= f->nz) iz = f->nz - 1;
    i = (size_t)ix + (size_t)iz * (size_t)f->nx;
#ifdef _WIN32
    {
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (const unsigned char *)f->live + i * e, &u, e, &got) || got != e)
            return 0;
    }
#else
    (void)e;
    (void)i;
    return 0;
#endif
    *out = (f->mode == 1 ? (float)u.b : f->mode == 2 ? (float)u.s : u.f) * f->scale + f->offset + f->center[1];
    if (g_live_base) {
        float o;
        if (mod_orig((int)(f - g_fields), (int)i, &o))
            *out = o * f->scale + f->offset + f->center[1];
    }
    return isfinite(*out);
}

int terrain_height_live_base(float x, float z, float *h)
{
    int r;
    g_live_base = 1;
    r = terrain_height_live(x, z, h);
    g_live_base = 0;
    return r;
}

int terrain_height_live(float x, float z, float *h)
{
    int i, n = g_num_fields;
    if (!g_ready || !g_live_ok)
        return 0;
    for (i = 0; i < n; ++i) {
        const TerrainField *f = &g_fields[i];
        float lx = x - f->center[0] + f->half_w, lz = z - f->center[2] + f->half_d, gx, gz, tx, tz, a, b, c, d;
        int ix, iz;
        if (!f->live || lx < 0.0f || lz < 0.0f || lx > f->width || lz > f->depth)
            continue;
        gx = lx / f->sample_w;
        gz = lz / f->sample_d;
        ix = (int)gx;
        iz = (int)gz;
        tx = gx - (float)ix;
        tz = gz - (float)iz;
        if (!live_raw(f, ix, iz, &a) || !live_raw(f, ix + 1, iz, &b) || !live_raw(f, ix, iz + 1, &c) ||
            !live_raw(f, ix + 1, iz + 1, &d))
            return 0;
        *h = (a * (1 - tx) + b * tx) * (1 - tz) + (c * (1 - tx) + d * tx) * tz;
        return 1;
    }
    return 0;
}

int terrain_height(float x, float z, float *h, float normal[3])
{
    int i, n = g_num_fields;
    if (!g_ready)
        return 0;
    for (i = 0; i < n; ++i) {
        const TerrainField *f = &g_fields[i];
        if (!field_height(f, x, z, h))
            continue;
        if (normal) {
            float e = f->sample_w > 0.25f ? f->sample_w : 0.25f;
            float hx1, hx0, hz1, hz0, l;
            if (!field_height(f, x + e, z, &hx1))
                hx1 = *h;
            if (!field_height(f, x - e, z, &hx0))
                hx0 = *h;
            if (!field_height(f, x, z + e, &hz1))
                hz1 = *h;
            if (!field_height(f, x, z - e, &hz0))
                hz0 = *h;
            normal[0] = -(hx1 - hx0) / (2.0f * e);
            normal[1] = 1.0f;
            normal[2] = -(hz1 - hz0) / (2.0f * e);
            l = sqrtf(normal[0] * normal[0] + 1.0f + normal[2] * normal[2]);
            normal[0] /= l;
            normal[1] /= l;
            normal[2] /= l;
        }
        return 1;
    }
    return 0;
}

/* ---------- cache sur le disque (un fichier par piste) ---------- */

#define CACHE_MAGIC 0x52444D58u /* "XMDR" */
#define CACHE_VERSION 1

typedef struct {
    uint32_t magic, version;
    int32_t count;
    float calib;
} CacheHeader;

typedef struct {
    float width, depth, sample_w, sample_d, half_w, half_d, min_h, max_h, scale, offset;
    int32_t nx, nz, mode, placed;
    float center[3];
} CacheField;

int terrain_save(const char *path, float calib)
{
    FILE *fp;
    CacheHeader hd;
    int i, n = g_num_fields, ok = 1;
    if (!g_ready || n <= 0)
        return 0;
    fp = fopen(path, "wb");
    if (!fp)
        return 0;
    hd.magic = CACHE_MAGIC;
    hd.version = CACHE_VERSION;
    hd.count = n;
    hd.calib = calib;
    ok &= fwrite(&hd, sizeof(hd), 1, fp) == 1;
    for (i = 0; i < n && ok; ++i) {
        const TerrainField *f = &g_fields[i];
        CacheField c;
        c.width = f->width; c.depth = f->depth; c.sample_w = f->sample_w; c.sample_d = f->sample_d;
        c.half_w = f->half_w; c.half_d = f->half_d; c.min_h = f->min_h; c.max_h = f->max_h;
        c.scale = f->scale; c.offset = f->offset; c.nx = f->nx; c.nz = f->nz; c.mode = f->mode;
        c.placed = f->placed;
        memcpy(c.center, f->center, sizeof(c.center));
        ok &= fwrite(&c, sizeof(c), 1, fp) == 1;
        ok &= fwrite(f->data, 1, terrain_data_size(f), fp) == terrain_data_size(f);
    }
    ok &= fclose(fp) == 0;
    if (!ok)
        remove(path);
    return ok;
}

int terrain_load(const char *path, float *calib)
{
    FILE *fp = fopen(path, "rb");
    CacheHeader hd;
    int i, used = 0;
    if (!fp)
        return 0;
    terrain_clear();
    if (fread(&hd, sizeof(hd), 1, fp) != 1 || hd.magic != CACHE_MAGIC ||
        hd.version != CACHE_VERSION || hd.count <= 0 || hd.count > TERRAIN_MAX) {
        fclose(fp);
        return 0;
    }
    for (i = 0; i < hd.count; ++i) {
        CacheField c;
        TerrainField f;
        size_t n;
        void *p;
        if (fread(&c, sizeof(c), 1, fp) != 1)
            break;
        memset(&f, 0, sizeof(f));
        f.width = c.width; f.depth = c.depth; f.sample_w = c.sample_w; f.sample_d = c.sample_d;
        f.half_w = c.half_w; f.half_d = c.half_d; f.min_h = c.min_h; f.max_h = c.max_h;
        f.scale = c.scale; f.offset = c.offset; f.nx = c.nx; f.nz = c.nz; f.mode = c.mode;
        f.placed = c.placed;
        memcpy(f.center, c.center, sizeof(f.center));
        if (f.nx < 2 || f.nz < 2 || f.nx > 65536 || f.nz > 65536 || f.mode < 1 || f.mode > 3 ||
            !(f.sample_w > 0.0f) || !(f.sample_d > 0.0f))
            break;
        n = terrain_data_size(&f);
        p = malloc(n);
        if (!p)
            break;
        if (fread(p, 1, n, fp) != n) {
            free(p);
            break;
        }
        f.data = p;
        f.owned = 1;
        terrain_add(&f);
        ++used;
    }
    fclose(fp);
    if (used != hd.count) {
        terrain_clear();
        return 0;
    }
    if (calib)
        *calib = hd.calib;
    return used;
}

/* ---------- parcours de la mémoire (Windows) ---------- */

#ifdef _WIN32

#define CHUNK (1u << 20)
#define OVERLAP 0x60

typedef int (*ChunkFn)(const uint8_t *buf, size_t len, uintptr_t base, void *ctx);

/* Parcourt toute la mémoire privée en lecture/écriture du processus, par morceaux copiés */
static int walk_memory_range(ChunkFn fn, void *ctx, uintptr_t lo, uintptr_t hi)
{
    static uint8_t buf[CHUNK + OVERLAP];
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = lo < 0x10000 ? 0x10000 : lo;
    HANDLE self = GetCurrentProcess();
    while (addr < hi && VirtualQuery((void *)addr, &mbi, sizeof(mbi))) {
        uintptr_t start = (uintptr_t)mbi.BaseAddress, end = start + mbi.RegionSize;
        if (start < addr)
            start = addr;
        if (end > hi)
            end = hi;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD) &&
            buf != (uint8_t *)start) {
            uintptr_t p;
            for (p = start; p < end; p += CHUNK) {
                size_t len = end - p < CHUNK + OVERLAP ? (size_t)(end - p) : CHUNK + OVERLAP;
                SIZE_T got = 0;
                if (!ReadProcessMemory(self, (void *)p, buf, len, &got) || got < 16)
                    continue;
                if (fn(buf, got, p, ctx))
                    return 1;
            }
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return 0;
}

static void walk_memory(ChunkFn fn, void *ctx)
{
    walk_memory_range(fn, ctx, 0x10000, 0x00007FFFFFFF0000ull);
}

/*
 * Chemin rapide : ODE alloue ses structures avec malloc, donc dans les tas
 * (heaps) Windows du jeu. On liste les blocs alloués des tas (rapide, sans lire
 * leur contenu), puis on ne lit que ces blocs, au lieu de toute la mémoire
 * (textures, sons, maillages : des gigaoctets).
 */
typedef struct {
    uintptr_t addr;
    size_t size;
} HeapBlock;

static HeapBlock *g_blocks = NULL;
static size_t g_num_blocks = 0;
static size_t g_block_bytes = 0;
#define MAX_BLOCKS (4u << 20)
#define BLOCK_MIN 0x40
#define BLOCK_MAX (16u << 20)

static void list_heap_blocks(void)
{
    HANDLE heaps[256];
    DWORD nh, i;
    g_num_blocks = 0;
    g_block_bytes = 0;
    if (!g_blocks)
        g_blocks = (HeapBlock *)VirtualAlloc(NULL, MAX_BLOCKS * sizeof(HeapBlock), MEM_RESERVE | MEM_COMMIT,
                                             PAGE_READWRITE);
    if (!g_blocks)
        return;
    nh = GetProcessHeaps(256, heaps);
    if (nh > 256)
        nh = 256;
    for (i = 0; i < nh; ++i) {
        PROCESS_HEAP_ENTRY e;
        if (!HeapLock(heaps[i]))
            continue;
        memset(&e, 0, sizeof(e));
        while (HeapWalk(heaps[i], &e) && g_num_blocks < MAX_BLOCKS) {
            if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY) || e.cbData < BLOCK_MIN || e.cbData > BLOCK_MAX)
                continue;
            g_blocks[g_num_blocks].addr = (uintptr_t)e.lpData;
            g_blocks[g_num_blocks].size = e.cbData;
            g_block_bytes += e.cbData;
            ++g_num_blocks;
        }
        HeapUnlock(heaps[i]);
    }
}

/* Lit les blocs listés (copie sûre) et les passe à fn. Retourne 1 si fn a demandé l'arrêt. */
static int walk_heap_blocks(ChunkFn fn, void *ctx)
{
    static uint8_t buf[CHUNK + OVERLAP];
    HANDLE self = GetCurrentProcess();
    size_t b;
    for (b = 0; b < g_num_blocks; ++b) {
        uintptr_t p = g_blocks[b].addr, end = p + g_blocks[b].size;
        for (; p < end; p += CHUNK) {
            size_t len = end - p < CHUNK + OVERLAP ? (size_t)(end - p) : CHUNK + OVERLAP;
            SIZE_T got = 0;
            if (!ReadProcessMemory(self, (void *)p, buf, len, &got) || got < 16)
                break;
            if (fn(buf, got, p, ctx))
                return 1;
        }
    }
    return 0;
}

/* Copie la grille du jeu dans notre mémoire (lecture sûre, et sauvegarde possible) */
static int copy_data(TerrainField *f)
{
    size_t n = terrain_data_size(f);
    SIZE_T got = 0;
    void *p = n ? malloc(n) : NULL;
    if (!p)
        return 0;
    if (!ReadProcessMemory(GetCurrentProcess(), f->data, p, n, &got) || got != n) {
        free(p);
        return 0;
    }
    f->live = f->data; /* la grille du jeu reste lue pour la hauteur actuelle */
    f->data = p;
    f->owned = 1;
    return 1;
}

typedef struct {
    uintptr_t addr[TERRAIN_MAX * 4];
    TerrainField field[TERRAIN_MAX * 4];
    uintptr_t ptrloc[TERRAIN_MAX * 4], aabb[TERRAIN_MAX * 4]; /* objet ODE du relief (geom) */
    int n;
} Found;

static void set_geom_info(const Found *fd)
{
    int k;
    for (k = 0; k < fd->n; ++k)
        if (fd->field[k].placed && fd->ptrloc[k]) {
            g_hf_aabb = fd->aabb[k];
            g_hf_ptrloc = fd->ptrloc[k];
            return;
        }
}

static int find_fields(const uint8_t *buf, size_t len, uintptr_t base, void *ctx)
{
    Found *fd = (Found *)ctx;
    size_t off;
    for (off = 0; off + sizeof(OdeHeightfieldData) <= len; off += 4) {
        float w, hw;
        TerrainField f;
        int k, dup = 0;
        memcpy(&w, buf + off, 4);
        if (!(w >= 5.0f && w <= 100000.0f))
            continue;
        memcpy(&hw, buf + off + 28, 4);
        if (fabsf(hw * 2.0f - w) > 1e-4f * w) {
            memcpy(&hw, buf + off + 24, 4); /* variante sans "rapport" */
            if (fabsf(hw * 2.0f - w) > 1e-4f * w)
                continue;
        }
        if (!terrain_parse(buf + off, &f))
            continue;
        for (k = 0; k < fd->n; ++k)
            dup |= fd->addr[k] == base + off;
        if (!dup && fd->n < TERRAIN_MAX * 4) {
            fd->addr[fd->n] = base + off;
            fd->field[fd->n] = f;
            ++fd->n;
        }
    }
    return 0;
}

/* Cherche l'objet ODE qui pointe vers chaque structure trouvée, et sa boîte englobante */
static int place_fields(const uint8_t *buf, size_t len, uintptr_t base, void *ctx)
{
    Found *fd = (Found *)ctx;
    size_t off;
    int k;
    for (off = (8 - (base & 7)) & 7; off + 8 <= len; off += 8) {
        uint64_t v;
        memcpy(&v, buf + off, 8);
        for (k = 0; k < fd->n; ++k) {
            TerrainField *f = &fd->field[k];
            long back;
            if (v != fd->addr[k] || f->placed)
                continue;
            /* boîte englobante : minx maxx miny maxy minz maxz, un peu avant le pointeur */
            for (back = 8; back <= 0x180 && (long)off - back >= 0; back += 4) {
                float a[6];
                memcpy(a, buf + off - back, sizeof(a));
                if (!(a[0] < a[1] && a[2] < a[3] && a[4] < a[5]))
                    continue;
                if (!close_rel(a[1] - a[0], f->width, 2e-3f) || !close_rel(a[5] - a[4], f->depth, 2e-3f))
                    continue;
                f->center[0] = 0.5f * (a[0] + a[1]);
                f->center[2] = 0.5f * (a[4] + a[5]);
                f->center[1] = a[3] - f->max_h;
                f->placed = 1;
                fd->ptrloc[k] = base + off;
                fd->aabb[k] = base + off - back;
                break;
            }
        }
    }
    for (k = 0; k < fd->n; ++k)
        if (!fd->field[k].placed)
            return 0;
    return 1; /* tous placés : on arrête le parcours */
}

int terrain_scan(TerrainLogFn log)
{
    static Found fd;
    int k, used = 0;
    memset(&fd, 0, sizeof(fd));
    terrain_clear();
    DWORD t0 = GetTickCount();
    int fast = 1;
    list_heap_blocks();
    if (log)
        log("relief : %u blocs de tas listes (%.0f Mo) en %.2f s", (unsigned)g_num_blocks,
            g_block_bytes / 1048576.0, (GetTickCount() - t0) / 1000.0f);
    walk_heap_blocks(find_fields, &fd);
    if (fd.n == 0) {
        /* pas trouvé dans les tas : on parcourt toute la mémoire (lent) */
        fast = 0;
        if (log)
            log("relief : rien dans les tas (%.2f s), recherche dans toute la memoire",
                (GetTickCount() - t0) / 1000.0f);
        walk_memory(find_fields, &fd);
    }
    if (log)
        log("relief : %d heightfield(s) ODE trouve(s) en %.2f s", fd.n, (GetTickCount() - t0) / 1000.0f);
    if (fd.n == 0)
        return 0;
    /* l'objet ODE (sa boîte englobante) est lui aussi dans les tas */
    if (!fast || !walk_heap_blocks(place_fields, &fd)) {
        int placed0 = 0;
        for (k = 0; k < fd.n; ++k)
            placed0 += fd.field[k].placed;
        if (log)
            log("relief : %d heightfield(s) sur %d places dans les tas, recherche autour des donnees", placed0, fd.n);
        /* sinon : autour des données, puis partout */
        for (k = 0; k < fd.n; ++k) {
            uintptr_t a = fd.addr[k], r = 64u << 20;
            if (fd.field[k].placed)
                continue;
            if (walk_memory_range(place_fields, &fd, a > r ? a - r : 0x10000, a + r))
                break;
        }
        /* v0.41.18 : toute la mémoire seulement si AUCUN n'est placé. Supercross (4 heightfields) :
           un seul introuvable lançait ce parcours de plusieurs minutes ; le relief n'était jamais
           prêt (pilote sur un sol plat, dans les bosses) et la moto pas retrouvée pendant ce temps */
        {
            int placed = 0;
            for (k = 0; k < fd.n; ++k)
                placed += fd.field[k].placed;
            if (!placed)
                walk_memory(place_fields, &fd);
            else if (placed < fd.n && log)
                log("relief : %d heightfield(s) sur %d places, les autres ignores", placed, fd.n);
        }
    }
    set_geom_info(&fd);
    if (log)
        log("relief : recherche de la position finie en %.2f s", (GetTickCount() - t0) / 1000.0f);
    for (k = 0; k < fd.n; ++k) {
        TerrainField *f = &fd.field[k];
        if (log)
            log("  [%d] %.0f x %.0f m, %d x %d points (pas %.2f m), type %d, hauteurs %.1f a %.1f, "
                "centre (%.1f %.1f %.1f)%s",
                k, f->width, f->depth, f->nx, f->nz, f->sample_w, f->mode, f->min_h, f->max_h,
                f->center[0], f->center[1], f->center[2], f->placed ? "" : " (position inconnue)");
        f->hdr = fd.addr[k]; /* v1.8.120 */
        {
            float hw2;
            memcpy(&hw2, (const void *)(fd.addr[k] + 28), 4);
            f->hdr_new = fabsf(hw2 * 2.0f - f->width) <= 1e-4f * f->width;
        }
        if (f->placed && copy_data(f)) {
            terrain_add(f);
            ++used;
        }
    }
    /* sans position retrouvée, on prend quand même le premier, centré sur l'origine */
    if (!used && copy_data(&fd.field[0])) {
        terrain_add(&fd.field[0]);
        used = 1;
    }
    if (log)
        log("relief : pret en %.2f s", (GetTickCount() - t0) / 1000.0f);
    return used;
}

static TerrainLogFn g_async_log = NULL;
static volatile LONG g_scanning = 0;

static DWORD WINAPI scan_thread(LPVOID arg)
{
    (void)arg;
    terrain_scan(g_async_log);
    InterlockedExchange(&g_scanning, 0);
    return 0;
}

int terrain_scanning(void)
{
    return g_scanning != 0;
}

void terrain_scan_async(TerrainLogFn log)
{
    HANDLE t;
    if (InterlockedCompareExchange(&g_scanning, 1, 0) != 0)
        return;
    g_async_log = log;
    t = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange(&g_scanning, 0);
}

/* Relief venu du cache : on retrouve la grille du jeu (même taille) pour lire la hauteur actuelle */
static TerrainLogFn g_live_log = NULL;
static volatile LONG g_live_finding = 0;
/* v0.41.15 : écart (m) entre une grille du jeu candidate et notre copie (cache) : 400 points.
   La déformation ne change le sol que de quelques dizaines de cm ; une autre grille (celle
   d'une session précédente, libérée) donne des mètres d'écart. */
static float live_mismatch(const TerrainField *f, const void *cand)
{
    TerrainField t = *f;
    int sx, sz, n = 0;
    float worst = 0.0f;
    t.live = cand;
    for (sz = 0; sz < 20; ++sz)
        for (sx = 0; sx < 20; ++sx) {
            int ix = (int)((sx + 0.5f) * f->nx / 20.0f), iz = (int)((sz + 0.5f) * f->nz / 20.0f);
            float a, b = sample_raw(f, ix, iz), d;
            if (!live_raw(&t, ix, iz, &a))
                return 1e9f;
            d = fabsf(a - b);
            if (d > worst)
                worst = d;
            ++n;
        }
    return n ? worst : 1e9f;
}

static DWORD WINAPI live_thread(LPVOID arg)
{
    static Found fd;
    int k, i, hits = 0;
    float best_err = 1e9f;
    DWORD t0 = GetTickCount();
    (void)arg;
    memset(&fd, 0, sizeof(fd));
    list_heap_blocks();
    walk_heap_blocks(find_fields, &fd);
    for (i = 0; i < g_num_fields; ++i) {
        TerrainField *f = &g_fields[i];
        const void *best = NULL;
        float be = 1e9f;
        if (f->live)
            continue;
        for (k = 0; k < fd.n; ++k)
            if (f->nx == fd.field[k].nx && f->nz == fd.field[k].nz &&
                close_rel(f->width, fd.field[k].width, 1e-3f) && close_rel(f->depth, fd.field[k].depth, 1e-3f) &&
                f->mode == fd.field[k].mode) {
                float e = live_mismatch(f, fd.field[k].data);
                if (e < be) {
                    be = e;
                    best = fd.field[k].data;
                }
            }
        if (be < best_err)
            best_err = be;
        if (best && be < 1.5f) {
            f->live = best;
            for (k = 0; k < fd.n; ++k)
                if (fd.field[k].data == best) {
                    float hw2;
                    f->hdr = fd.addr[k]; /* v1.8.120 */
                    memcpy(&hw2, (const void *)(fd.addr[k] + 28), 4);
                    f->hdr_new = fabsf(hw2 * 2.0f - f->width) <= 1e-4f * f->width;
                }
            ++hits;
        }
    }
    if (hits) {
        g_live_ok = 1;
        /* objet ODE du relief (pour retrouver les objets de la piste à côté de lui) */
        if (!walk_heap_blocks(place_fields, &fd)) {
        }
        set_geom_info(&fd);
    }
    if (g_live_log) {
        if (hits)
            g_live_log("relief : grille du jeu retrouvee en %.2f s (hauteur actuelle avec la deformation, ecart max %.2f m)",
                       (GetTickCount() - t0) / 1000.0f, best_err);
        else
            g_live_log("relief : grille du jeu pas retrouvee en %.2f s (meilleur ecart %.2f m) : relief du cache seulement",
                       (GetTickCount() - t0) / 1000.0f, best_err);
    }
    InterlockedExchange(&g_live_finding, 0);
    return 0;
}

/* v0.41.15 : chaque session recrée la grille du jeu : l'ancienne adresse n'est plus bonne
   (2e session sur la même piste : sol lu dans une grille libérée, pilote dans le sol) */
void terrain_live_reset(void)
{
    int i;
    g_live_ok = 0;
    for (i = 0; i < g_num_fields; ++i)
        g_fields[i].live = NULL;
}

void terrain_find_live_async(TerrainLogFn log)
{
    HANDLE t;
    if (g_live_ok || InterlockedCompareExchange(&g_live_finding, 1, 0) != 0)
        return;
    g_live_log = log;
    t = CreateThread(NULL, 0, live_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange(&g_live_finding, 0);
}

/* ---------- recherche de valeurs (diagnostic caméra) ---------- */

typedef struct {
    float v[2][3];   /* deux motifs : position, puis 1re colonne de la vue */
    float tol[2];
    int hits[4];
    uintptr_t stack, stack_lo, stack_hi;
    uintptr_t cam_best;
    TerrainLogFn log;
} FindCtx;

static int near_f(float a, float b, float tol)
{
    return fabsf(a - b) <= tol; /* faux pour NaN */
}

static void log_hit(FindCtx *fc, const char *what, uintptr_t at)
{
    float c[40];
    SIZE_T got = 0;
    int i;
    char line[1200];
    size_t n = 0;
    memset(c, 0, sizeof(c));
    ReadProcessMemory(GetCurrentProcess(), (void *)(at - 48), c, sizeof(c), &got);
    for (i = 0; i < 40 && n + 16 < sizeof(line); ++i)
        n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%.3f", i == 12 ? " | " : " ", c[i]);
    fc->log("  %s @%p : [-48]%s", what, (void *)at, line);
}

/* 3x4 par lignes orthonormées, (0 0 0 1), angle de vue plausible en +0x40 */
int camera_matrix_ok(const float *m)
{
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) {
            float d = m[i] * m[j] + m[4 + i] * m[4 + j] + m[8 + i] * m[8 + j];
            if (fabsf(d - (i == j ? 1.0f : 0.0f)) > 0.02f)
                return 0;
        }
    if (m[12] != 0.0f || m[13] != 0.0f || m[14] != 0.0f || m[15] != 1.0f)
        return 0;
    return m[16] >= 20.0f && m[16] <= 150.0f;
}

static int find_floats(const uint8_t *buf, size_t len, uintptr_t base, void *ctx)
{
    FindCtx *fc = (FindCtx *)ctx;
    size_t off;
    for (off = 0; off + 36 <= len; off += 4) {
        float a[9];
        memcpy(a, buf + off, 36);
        /* position contiguë (x y z) */
        if (near_f(a[0], fc->v[0][0], fc->tol[0]) && near_f(a[1], fc->v[0][1], fc->tol[0]) &&
            near_f(a[2], fc->v[0][2], fc->tol[0]) && ++fc->hits[0] <= 25 && fc->log)
            log_hit(fc, "position", base + off);
        /* matrice 3x4 par lignes : x en +0, y en +16, z en +32 */
        if (near_f(a[0], fc->v[0][0], fc->tol[0]) && near_f(a[4], fc->v[0][1], fc->tol[0]) &&
            near_f(a[8], fc->v[0][2], fc->tol[0])) {
            uintptr_t at = base + off;
            if (++fc->hits[2] <= 25 && fc->log)
                log_hit(fc, "position 3x4", at);
            /* caméra candidate : matrice caméra -> monde (lignes), puis (0 0 0 1), puis l'angle de vue */
            if (at >= fc->stack_lo + 12 && at < fc->stack_hi && at > fc->cam_best + 12) {
                float m[17];
                SIZE_T got = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (void *)(at - 12), m, sizeof(m), &got) &&
                    got == sizeof(m) && camera_matrix_ok(m))
                    fc->cam_best = at - 12;
            }
        }
        /* position en double précision */
        if (off + 24 <= len) {
            double d[3];
            memcpy(d, buf + off, 24);
            if (fabs(d[0] - fc->v[0][0]) <= fc->tol[0] && fabs(d[1] - fc->v[0][1]) <= fc->tol[0] &&
                fabs(d[2] - fc->v[0][2]) <= fc->tol[0] && ++fc->hits[3] <= 25 && fc->log)
                log_hit(fc, "position double", base + off);
        }
        /* première colonne de la vue (= 1re ligne de la matrice caméra -> monde) */
        if (near_f(a[0], fc->v[1][0], fc->tol[1]) && near_f(a[1], fc->v[1][1], fc->tol[1]) &&
            near_f(a[2], fc->v[1][2], fc->tol[1]) && ++fc->hits[1] <= 25 && fc->log)
            log_hit(fc, "vue", base + off);
    }
    return 0;
}

static FindCtx g_find;
static volatile uintptr_t g_cam_found = 0;

uintptr_t mem_camera_candidate(void)
{
    return g_cam_found;
}
static volatile LONG g_finding = 0;

static DWORD WINAPI find_thread(LPVOID arg)
{
    DWORD t0 = GetTickCount();
    uint8_t *img = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    (void)arg;
    if (!g_scanning) {
        list_heap_blocks();
        walk_heap_blocks(find_floats, &g_find);
    }
    /* la pile du fil de rendu (la vue y est copiée : la caméra y est peut-être aussi) */
    if (g_find.stack) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)g_find.stack, &mbi, sizeof(mbi))) {
            uintptr_t lo = (uintptr_t)mbi.AllocationBase;
            g_find.stack_lo = lo;
            g_find.stack_hi = lo + (4u << 20);
            if (g_find.log)
                g_find.log("camera : recherche dans la pile %p - %p", (void *)lo, (void *)(lo + (4u << 20)));
            walk_memory_range(find_floats, &g_find, lo, lo + (4u << 20));
        }
    }
    /* et les variables globales du jeu (son image en mémoire) */
    walk_memory_range(find_floats, &g_find, (uintptr_t)img, (uintptr_t)img + nt->OptionalHeader.SizeOfImage);
    if (g_find.log)
        g_find.log("camera : camera du jeu candidate %p", (void *)g_find.cam_best);
    g_cam_found = g_find.cam_best;
    if (g_find.log)
        g_find.log("camera : recherche finie en %.2f s : %d position(s), %d position(s) 3x4, %d double(s), %d vue(s)",
                   (GetTickCount() - t0) / 1000.0f, g_find.hits[0], g_find.hits[2], g_find.hits[3], g_find.hits[1]);
    InterlockedExchange(&g_finding, 0);
    return 0;
}

void mem_find_camera_async(const float pos[3], const float col0[3], const void *stack, TerrainLogFn log)
{
    HANDLE t;
    if (InterlockedCompareExchange(&g_finding, 1, 0) != 0)
        return;
    memset(&g_find, 0, sizeof(g_find));
    memcpy(g_find.v[0], pos, 12);
    memcpy(g_find.v[1], col0, 12);
    g_find.tol[0] = 0.02f;
    g_find.tol[1] = 2e-3f;
    g_find.log = log;
    g_find.stack = (uintptr_t)stack;
    if (log)
        log("camera : recherche de la camera du jeu, position (%.3f %.3f %.3f), vue x (%.4f %.4f %.4f)",
            pos[0], pos[1], pos[2], col0[0], col0[1], col0[2]);
    t = CreateThread(NULL, 0, find_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange(&g_finding, 0);
}

/* ---------- recherche du corps physique de la moto (diagnostic) ---------- */

typedef struct {
    float r[9];      /* rotation de la télémétrie, lignes */
    float pos[3], vel[3];
    int hits;
    uintptr_t body;
    uintptr_t cand[8];
    float cand_d[8]; /* distance à la position de la télémétrie */
    int ncand;
    TerrainLogFn log;
} BodyCtx;

static volatile uintptr_t g_bike_body = 0;
static volatile LONG g_body_finding = 0;
static uintptr_t g_bodies[64];
static volatile int g_num_bodies = 0;

uintptr_t mem_bike_body(void)
{
    return g_body_finding ? 0 : g_bike_body; /* rien pendant une recherche */
}

/* La moto garde les mêmes corps ODE d'une chute à l'autre : on revérifie le dernier châssis
   trouvé (position près de la moto, quaternion unitaire) au lieu de tout rechercher */
static volatile uintptr_t g_last_good_body = 0;

int mem_bike_revalidate(const float pos[3])
{
    float b[20];
    SIZE_T got = 0;
    uintptr_t a = g_last_good_body;
    float dx, dy, dz, qn;
    if (!a || g_body_finding)
        return 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)(a + 0xB8), b, sizeof(b), &got) || got != sizeof(b))
        return 0;
    dx = b[0] - pos[0];
    dy = b[1] - pos[1];
    dz = b[2] - pos[2];
    qn = b[16] * b[16] + b[17] * b[17] + b[18] * b[18] + b[19] * b[19];
    if (dx * dx + dy * dy + dz * dz > 3.0f * 3.0f || fabsf(qn - 1.0f) > 0.02f || b[7] != 0.0f || b[11] != 0.0f)
        return 0;
    g_bike_body = a;
    return 1;
}

void mem_set_bike_body(uintptr_t b)
{
    if (!b || g_body_finding)
        return;
    g_bike_body = b;
    g_last_good_body = b;
}

int mem_bike_searching(void)
{
    return g_body_finding != 0;
}

int mem_world_bodies(uintptr_t *out, int max)
{
    int i, n = g_num_bodies;
    if (n > max)
        n = max;
    for (i = 0; i < n; ++i)
        out[i] = g_bodies[i];
    return n;
}

/* Tous les dxBody du même monde ODE : leur premier champ pointe vers le monde */
typedef struct {
    uintptr_t world;
    uintptr_t found[64];
    int n;
} WorldCtx;

static int find_world_bodies(const uint8_t *buf, size_t len, uintptr_t base, void *ctx)
{
    WorldCtx *wc = (WorldCtx *)ctx;
    size_t off;
    for (off = (8 - (base & 7)) & 7; off + 0x128 <= len; off += 8) {
        uint64_t v;
        float s[24], qn;
        int k, dup = 0;
        memcpy(&v, buf + off, 8);
        if (v != wc->world)
            continue;
        memcpy(s, buf + off + 0xB8, sizeof(s)); /* position, rotation, quaternion, vitesse */
        qn = s[16] * s[16] + s[17] * s[17] + s[18] * s[18] + s[19] * s[19];
        if (s[7] != 0.0f || s[11] != 0.0f || s[15] != 0.0f || fabsf(qn - 1.0f) > 0.01f || !isfinite(s[0]) ||
            fabsf(s[0]) > 1e5f)
            continue;
        if (fabsf(s[4] * s[4] + s[5] * s[5] + s[6] * s[6] - 1.0f) > 0.02f)
            continue;
        for (k = 0; k < wc->n; ++k)
            dup |= wc->found[k] == base + off;
        if (!dup && wc->n < 64)
            wc->found[wc->n++] = base + off;
    }
    return 0;
}

static BodyCtx g_body;

static void log_body_hit(BodyCtx *bc, const char *what, uintptr_t at)
{
    float c[52];
    SIZE_T got = 0;
    char line[1400];
    size_t n = 0;
    int i;
    memset(c, 0, sizeof(c));
    ReadProcessMemory(GetCurrentProcess(), (void *)(at - 64), c, sizeof(c), &got);
    for (i = 0; i < 52 && n + 16 < sizeof(line); ++i)
        n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%.3f", i == 16 ? " | " : " ", c[i]);
    bc->log("  %s @%p : [-64]%s", what, (void *)at, line);
}

static int find_body(const uint8_t *buf, size_t len, uintptr_t base, void *ctx)
{
    BodyCtx *bc = (BodyCtx *)ctx;
    size_t off;
    const float *r = bc->r;
    /* la rotation de la télémétrie et celle d'ODE ne sont pas lues au même instant :
       si la moto bouge encore un peu, il faut de la marge (la position trie ensuite) */
    const float tol = 0.02f;
    for (off = 0; off + 48 <= len; off += 4) {
        float a[12];
        memcpy(a, buf + off, 48);
        /* lignes de la rotation, espacées de 16 octets (dMatrix3 d'ODE) ou 12 */
        if (near_f(a[0], r[0], tol) && near_f(a[1], r[1], tol) && near_f(a[2], r[2], tol)) {
            if (near_f(a[4], r[3], tol) && near_f(a[5], r[4], tol) && near_f(a[6], r[5], tol) &&
                near_f(a[8], r[6], tol)) {
                /* corps ODE : position juste avant, quaternion unitaire et vitesses juste après */
                float b[28];
                SIZE_T got = 0;
                uintptr_t at = base + off;
                if (++bc->hits <= 20)
                    log_body_hit(bc, "rotation (lignes, pas 16)", at);
                if (ReadProcessMemory(GetCurrentProcess(), (void *)(at - 16), b, sizeof(b), &got) && got == sizeof(b)) {
                    float dx = b[0] - bc->pos[0], dy = b[1] - bc->pos[1], dz = b[2] - bc->pos[2];
                    float qn = b[16] * b[16] + b[17] * b[17] + b[18] * b[18] + b[19] * b[19];
                    float v = fabsf(b[20]) + fabsf(b[21]) + fabsf(b[22]);
                    if (dx * dx + dy * dy + dz * dz < 2.0f * 2.0f && fabsf(qn - 1.0f) < 0.01f && v < 3.0f &&
                        b[7] == 0.0f && b[11] == 0.0f && b[15] == 0.0f && bc->ncand < 8) {
                        bc->cand_d[bc->ncand] = sqrtf(dx * dx + dy * dy + dz * dz);
                        bc->cand[bc->ncand++] = at - 0xC8;
                    } /* début du dxBody (dObject + masse + invI + invMass) */
                }
            }
            else if (near_f(a[3], r[3], tol) && near_f(a[4], r[4], tol) && near_f(a[5], r[5], tol) &&
                     near_f(a[6], r[6], tol) && ++bc->hits <= 20)
                log_body_hit(bc, "rotation (lignes, pas 12)", base + off);
        }
        /* transposée (colonnes) */
        if (near_f(a[0], r[0], tol) && near_f(a[1], r[3], tol) && near_f(a[2], r[6], tol)) {
            if (near_f(a[4], r[1], tol) && near_f(a[5], r[4], tol) && near_f(a[6], r[7], tol) &&
                near_f(a[8], r[2], tol) && ++bc->hits <= 20)
                log_body_hit(bc, "rotation (colonnes, pas 16)", base + off);
            else if (near_f(a[3], r[1], tol) && near_f(a[4], r[4], tol) && near_f(a[5], r[7], tol) &&
                     near_f(a[6], r[2], tol) && ++bc->hits <= 20)
                log_body_hit(bc, "rotation (colonnes, pas 12)", base + off);
        }
    }
    return 0;
}

static DWORD WINAPI body_thread(LPVOID arg)
{
    DWORD t0 = GetTickCount();
    (void)arg;
    if (!g_scanning) {
        list_heap_blocks();
        walk_heap_blocks(find_body, &g_body);
    }
    /* plusieurs mondes ODE contiennent un corps qui suit la moto : on garde celui
       du monde qui a le plus de corps (châssis + roues + suspensions + pilote) */
    {
        static WorldCtx wc, best;
        int c;
        float best_d = 1e9f;
        memset(&best, 0, sizeof(best));
        g_body.body = 0;
        for (c = 0; c < g_body.ncand; ++c) {
            uintptr_t world = 0;
            SIZE_T got = 0;
            memset(&wc, 0, sizeof(wc));
            if (!ReadProcessMemory(GetCurrentProcess(), (void *)g_body.cand[c], &world, 8, &got) || !world)
                continue;
            wc.world = world;
            walk_heap_blocks(find_world_bodies, &wc);
            if (g_body.log)
                g_body.log("moto : corps candidat %p, monde %p : %d corps, a %.2f m de la moto", (void *)g_body.cand[c], (void *)world, wc.n, g_body.cand_d[c]);
            /* la vraie moto : châssis + roues + suspensions (+ pilote), 5 corps ou plus. Un monde
               d'un seul corps (183 kg) suit aussi la moto, mais le relever ne la bougeait pas */
            /* (v0.40.8 : un faux « monde » à l'adresse 0x3f800000 — le nombre 1.0 — comptait 64
               corps et passait devant la vraie moto, qui ne se relevait plus) */
            /* v0.41.31 : même monde (châssis et pilote, 80 kg, ont la même rotation quand la
               moto est droite) : le plus proche de la position de la télémétrie, le châssis
               (~0,55 m) et pas le pilote (~1 m). Le pilote pris pour le châssis : moto relevée
               de travers à la descente volontaire, physique cassée, jeu planté (11/11 00:57) */
            if (wc.n >= 4 && wc.n <= 24 && (world & 7) == 0 && world != 0x3f800000u &&
                (wc.n > best.n || (world == best.world && g_body.cand_d[c] < best_d))) {
                best = wc;
                best_d = g_body.cand_d[c];
                g_body.body = g_body.cand[c];
            }
        }
        if (g_body.body) {
            int k;
            for (k = 0; k < best.n; ++k)
                g_bodies[k] = best.found[k];
            g_num_bodies = best.n;
        }
    }
    g_bike_body = g_body.body;
    if (g_body.body)
        g_last_good_body = g_body.body;

    if (g_body.log)
        g_body.log("moto : recherche finie en %.2f s, %d resultat(s), corps du chassis %p", (GetTickCount() - t0) / 1000.0f,
                   g_body.hits, (void *)g_body.body);
    InterlockedExchange(&g_body_finding, 0);
    return 0;
}

void mem_find_bike_body_async(const float rot[9], const float pos[3], const float vel[3], TerrainLogFn log)
{
    HANDLE t;
    if (InterlockedCompareExchange(&g_body_finding, 1, 0) != 0)
        return;
    memset(&g_body, 0, sizeof(g_body));
    memcpy(g_body.r, rot, sizeof(g_body.r));
    memcpy(g_body.pos, pos, sizeof(g_body.pos));
    memcpy(g_body.vel, vel, sizeof(g_body.vel));
    g_body.log = log;
    if (log)
        log("moto : recherche du corps physique, position (%.3f %.3f %.3f), vitesse (%.3f %.3f %.3f), "
            "rotation [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]",
            pos[0], pos[1], pos[2], vel[0], vel[1], vel[2], rot[0], rot[1], rot[2], rot[3], rot[4], rot[5],
            rot[6], rot[7], rot[8]);
    t = CreateThread(NULL, 0, body_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange(&g_body_finding, 0);
}

/* v1.8.127 : copies de la grille du jeu dans la mémoire (blocs de la même taille, mêmes valeurs) */
static int read_raw_at(uintptr_t base, int mode, int idx, float *v)
{
    size_t e = mode == 1 ? 1 : mode == 2 ? 2 : 4;
    union { unsigned char b; short s; float f; } u;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (const void *)(base + (size_t)idx * e), &u, e, &got) || got != e)
        return 0;
    *v = mode == 1 ? (float)u.b : mode == 2 ? (float)u.s : u.f;
    return isfinite(*v);
}

static DWORD WINAPI copy_thread(LPVOID arg)
{
    int fi, found = 0;
    size_t b;
    DWORD t0 = GetTickCount();
    (void)arg;
    if (g_live_finding || g_scanning) {
        InterlockedExchange(&g_copy_state, 0);
        return 0;
    }
    list_heap_blocks();
    g_copy_msg[0] = 0;
    for (fi = 0; fi < g_num_fields; ++fi) {
        const TerrainField *f = &g_fields[fi];
        size_t n = terrain_data_size(f), e = f->mode == 1 ? 1 : f->mode == 2 ? 2 : 4;
        float ref[256];
        int idx[256], k, nref = 0;
        if (!f->live || !n)
            continue;
        /* 256 points répartis (valeurs de la grille du jeu) */
        for (k = 0; k < 256; ++k) {
            int ix = (int)(((k % 16) + 0.5f) * f->nx / 16.0f), iz = (int)(((k / 16) + 0.5f) * f->nz / 16.0f);
            idx[nref] = ix + iz * f->nx;
            if (read_raw_at((uintptr_t)f->live, f->mode, idx[nref], &ref[nref]))
                ++nref;
        }
        if (nref < 200)
            continue;
        {
            /* (grille toute plate : pas assez différente pour reconnaître une copie) */
            float lo = ref[0], hi = ref[0];
            for (k = 1; k < nref; ++k) {
                lo = ref[k] < lo ? ref[k] : lo;
                hi = ref[k] > hi ? ref[k] : hi;
            }
            if (hi - lo < 20.0f * (f->mode == 3 ? 0.01f : 1.0f))
                continue;
        }
        for (b = 0; b < g_num_blocks && g_ncopy[fi] < COPY_MAX; ++b) {
            uintptr_t a0 = g_blocks[b].addr;
            size_t sz = g_blocks[b].size, off;
            if (sz < n || sz > n + 256)
                continue;
            for (off = 0; off + n <= sz && g_ncopy[fi] < COPY_MAX; off += e) {
                uintptr_t a = a0 + off;
                int ok = 0, bad = 0;
                float v, tol = f->mode == 3 ? 0.3f / (f->scale != 0.0f ? fabsf(f->scale) : 1.0f) : 400.0f;
                if (a == (uintptr_t)f->live || a == (uintptr_t)f->data)
                    continue;
                for (k = 0; k < nref && bad < 12; ++k) {
                    if (!read_raw_at(a, f->mode, idx[k], &v) || fabsf(v - ref[k]) > tol)
                        ++bad;
                    else if (v == ref[k])
                        ++ok;
                    else
                        ++ok;
                }
                if (bad < 12 && ok >= nref - 12) {
                    g_copy[fi][g_ncopy[fi]] = a;
                    ++g_ncopy[fi];
                    ++found;
                    snprintf(g_copy_msg + strlen(g_copy_msg), sizeof(g_copy_msg) - strlen(g_copy_msg),
                             " [grille %d : %p (+%u)]", fi, (void *)a, (unsigned)off);
                }
            }
        }
    }
    snprintf(g_copy_msg + strlen(g_copy_msg), sizeof(g_copy_msg) - strlen(g_copy_msg), " : %d copie(s) en %.2f s", found,
             (GetTickCount() - t0) / 1000.0f);
    {
        /* v1.8.134 : qui garde l'adresse de chaque grille (les roues lisent une grille 16 bits à part) :
           blocs du tas qui contiennent le pointeur, avec les flottants autour (échelle, décalage...) */
        /* v1.8.145 : diagnostic seulement (les essais qui s'en servaient sont coupés) : sauté */
        if (0) {
        uintptr_t tg[2 * (1 + COPY_MAX) + 2];
        int nt = 0, t, fi2, c2;
        static uint8_t buf[1 << 16];
        for (fi2 = 0; fi2 < g_num_fields && fi2 < 2; ++fi2) {
            if (g_fields[fi2].live)
                tg[nt++] = (uintptr_t)g_fields[fi2].live;
            for (c2 = 0; c2 < g_ncopy[fi2] && nt < (int)(sizeof(tg) / sizeof(tg[0])); ++c2)
                tg[nt++] = g_copy[fi2][c2];
        }
        g_ref_msg[0] = 0;
        /* v1.8.139 : aussi les zones écrivables de mxbikes.exe (variables globales du jeu) */
        static uintptr_t xr[64][2];
        int nxr = 0, xi;
        {
            uintptr_t mb = (uintptr_t)GetModuleHandleA(NULL), q = mb;
            MEMORY_BASIC_INFORMATION mi;
            while (nxr < 64 && VirtualQuery((void *)q, &mi, sizeof(mi)) == sizeof(mi) && (uintptr_t)mi.AllocationBase == mb) {
                if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) {
                    xr[nxr][0] = (uintptr_t)mi.BaseAddress;
                    xr[nxr][1] = (uintptr_t)mi.BaseAddress + mi.RegionSize;
                    ++nxr;
                }
                q = (uintptr_t)mi.BaseAddress + mi.RegionSize;
            }
            for (t = 0; t < nt; ++t)
                ;
            /* + les en-têtes des reliefs ODE */
            for (fi2 = 0; fi2 < g_num_fields && fi2 < 2 && nt < (int)(sizeof(tg) / sizeof(tg[0])); ++fi2)
                if (g_fields[fi2].hdr)
                    tg[nt++] = g_fields[fi2].hdr;
        }
        for (xi = -nxr; xi < (int)g_num_blocks; ++xi) {
            uintptr_t p0 = xi < 0 ? xr[-xi - 1][0] : g_blocks[xi].addr,
                      end = xi < 0 ? xr[-xi - 1][1] : g_blocks[xi].addr + g_blocks[xi].size;
            b = xi < 0 ? 0 : (size_t)xi;
            for (; p0 < end; p0 += sizeof(buf)) {
                size_t len = end - p0 < sizeof(buf) ? (size_t)(end - p0) : sizeof(buf), o;
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (void *)p0, buf, len, &got) || got < 8)
                    break;
                for (o = 0; o + 8 <= got; o += 8) {
                    uint64_t v;
                    memcpy(&v, buf + o, 8);
                    for (t = 0; t < nt; ++t)
                        if (v == (uint64_t)tg[t] && strlen(g_ref_msg) + 400 < sizeof(g_ref_msg)) {
                            float fl[24];
                            int k2;
                            size_t l2 = strlen(g_ref_msg);
                            uintptr_t at = p0 + o;
                            SIZE_T g2 = 0;
                            memset(fl, 0, sizeof(fl));
                            ReadProcessMemory(GetCurrentProcess(), (void *)(at - 64), fl, sizeof(fl), &g2);
                            l2 += (size_t)snprintf(g_ref_msg + l2, sizeof(g_ref_msg) - l2, "\n  grille %p vue en %p (%s +%llx) :",
                                                   (void *)tg[t], (void *)at, xi < 0 ? "jeu" : "bloc",
                                                   (unsigned long long)(xi < 0 ? at - (uintptr_t)GetModuleHandleA(NULL) : at - g_blocks[b].addr));
                            for (k2 = 0; k2 < 24; ++k2)
                                l2 += (size_t)snprintf(g_ref_msg + l2, sizeof(g_ref_msg) - l2, " %g", fl[k2]);
                            if (xi < 0) {
                                /* v1.8.140 : hauteur totale de la grille (échelle x 65535) juste après le pointeur */
                                float H = g_fields[0].scale * 65535.0f;
                                int dupH, h2;
                                for (k2 = 17; k2 < 24; ++k2)
                                    if (fabsf(fl[k2] - H) < 0.002f) {
                                        uintptr_t ha = at - 64 + (uintptr_t)k2 * 4;
                                        dupH = 0;
                                        for (h2 = 0; h2 < g_nhref; ++h2)
                                            dupH |= g_href[h2] == ha;
                                        if (!dupH && g_nhref < HREF_MAX) {
                                            g_href[g_nhref++] = ha;
                                            l2 += (size_t)snprintf(g_ref_msg + l2, sizeof(g_ref_msg) - l2, " [hauteur totale en jeu+%llx]",
                                                                   (unsigned long long)(ha - (uintptr_t)GetModuleHandleA(NULL)));
                                        }
                                    }
                            }
                            {
                                /* v1.8.135 : structure reconnue : échelle à -32, nx / nz à -24 / -20, mode 2 à -8 */
                                int32_t nx2, nz2, md2, w2, dup = 0;
                                memcpy(&nx2, &fl[10], 4);
                                memcpy(&nz2, &fl[11], 4);
                                memcpy(&md2, &fl[14], 4);
                                for (w2 = 0; w2 < g_nwref; ++w2)
                                    dup |= g_wref[w2] == at;
                                if (!dup && g_nwref < WREF_MAX && md2 == 2 && nx2 == g_fields[0].nx && nz2 == g_fields[0].nz &&
                                    fabsf(fl[8] - g_fields[0].scale) <= 1e-6f * fabsf(g_fields[0].scale)) {
                                    g_wref[g_nwref] = at;
                                    g_wref_old[g_nwref] = tg[t];
                                    g_wref_field[g_nwref] = 0;
                                    g_wref_on[g_nwref] = 0;
                                    ++g_nwref;
                                    l2 += (size_t)snprintf(g_ref_msg + l2, sizeof(g_ref_msg) - l2, " [structure de relief reconnue]");
                                }
                            }
                        }
                }
            }
        }
    }
    {
        /* v1.8.138 : objets ODE qui pointent sur l'en-tête de chaque 2e relief (en-tête = début du bloc, données à +80) */
        int w;
        static uint8_t buf2[1 << 16];
        for (w = 0; w < g_nwref; ++w) {
            uintptr_t hdr2 = g_wref[w] - 80;
            SIZE_T gr = 0;
            g_wref_ngeom[w] = 0;
            ReadProcessMemory(GetCurrentProcess(), (const void *)(hdr2 + 40), &g_wref_maxh[w], 4, &gr);
            for (b = 0; b < g_num_blocks && g_wref_ngeom[w] < 4; ++b) {
                uintptr_t p0 = g_blocks[b].addr, end = p0 + g_blocks[b].size;
                for (; p0 < end && g_wref_ngeom[w] < 4; p0 += sizeof(buf2)) {
                    size_t len = end - p0 < sizeof(buf2) ? (size_t)(end - p0) : sizeof(buf2), o;
                    SIZE_T got = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), (void *)p0, buf2, len, &got) || got < 8)
                        break;
                    for (o = 0; o + 8 <= got && g_wref_ngeom[w] < 4; o += 8) {
                        uint64_t v;
                        memcpy(&v, buf2 + o, 8);
                        if (v == (uint64_t)hdr2)
                            g_wref_geomref[w][g_wref_ngeom[w]++] = p0 + o;
                    }
                }
            }
            snprintf(g_ref_msg + strlen(g_ref_msg), sizeof(g_ref_msg) - strlen(g_ref_msg),
                     "\n  2e relief : en-tete %p, hauteur max %.2f (+ centre), %d pointeur(s) vers l'en-tete", (void *)hdr2,
                     g_wref_maxh[w], g_wref_ngeom[w]);
        }
    }
    }
    InterlockedExchange(&g_copy_state, 2);
    return 0;
}

static void copy_find_async(void)
{
    HANDLE t;
    if (InterlockedCompareExchange(&g_copy_state, 1, 0) != 0)
        return;
    t = CreateThread(NULL, 0, copy_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange(&g_copy_state, 0);
}

#else
static void copy_find_async(void) {}

int terrain_scan(TerrainLogFn log)
{
    (void)log;
    return 0;
}

void terrain_scan_async(TerrainLogFn log)
{
    (void)log;
}

void terrain_live_reset(void) {}
void terrain_find_live_async(TerrainLogFn log)
{
    (void)log;
}

int terrain_scanning(void)
{
    return 0;
}

void mem_find_camera_async(const float pos[3], const float col0[3], const void *stack, TerrainLogFn log)
{
    (void)pos; (void)col0; (void)stack; (void)log;
}

uintptr_t mem_camera_candidate(void)
{
    return 0;
}

void mem_find_bike_body_async(const float rot[9], const float pos[3], const float vel[3], TerrainLogFn log)
{
    (void)rot; (void)pos; (void)vel; (void)log;
}

uintptr_t mem_bike_body(void)
{
    return 0;
}

int mem_bike_searching(void)
{
    return 0;
}

int mem_bike_revalidate(const float pos[3])
{
    (void)pos;
    return 0;
}

int mem_world_bodies(uintptr_t *out, int max)
{
    (void)out; (void)max;
    return 0;
}

void mem_set_bike_body(uintptr_t b)
{
    (void)b;
}

int camera_matrix_ok(const float *m)
{
    (void)m;
    return 0;
}

#endif


/* ---------- v1.8.120 : rampes FMX écrites dans la grille du relief du jeu ----------
   v1.8.121 : liste triée (avant : recherche linéaire, des millions de comparaisons à chaque mise à jour)
   et entretien à chaque appel (le jeu remet parfois la grille à jour autour de la moto) */
#define MOD_MAX 120000
typedef struct {
    int field, idx;
    float orig; /* valeur brute d'origine */
    float want; /* valeur brute voulue (NAN : plus voulue) */
} Mod;
static Mod g_mod[MOD_MAX], g_new[MOD_MAX];
static int g_nnew = 0;
static volatile int g_mod_busy = 0;
static int g_resets = 0, g_reset_orig = 0, g_reset_mid = 0;

static int raw_get(const TerrainField *f, int idx, float *v)
{
#ifdef _WIN32
    size_t e = f->mode == 1 ? 1 : f->mode == 2 ? 2 : 4;
    union { unsigned char b; short s; float fl; } u;
    SIZE_T got = 0;
    if (!f->live || !ReadProcessMemory(GetCurrentProcess(), (const unsigned char *)f->live + (size_t)idx * e, &u, e, &got) || got != e)
        return 0;
    *v = f->mode == 1 ? (float)u.b : f->mode == 2 ? (float)u.s : u.fl;
    return 1;
#else
    (void)f; (void)idx; (void)v;
    return 0;
#endif
}

static int raw_set(const TerrainField *f, int idx, float v)
{
#ifdef _WIN32
    size_t e = f->mode == 1 ? 1 : f->mode == 2 ? 2 : 4;
    union { unsigned char b; short s; float fl; } u;
    SIZE_T w = 0;
    if (f->mode == 1)
        u.b = (unsigned char)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v + 0.5f);
    else if (f->mode == 2)
        u.s = (short)(v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : floorf(v + 0.5f));
    else
        u.fl = v;
    return f->live && WriteProcessMemory(GetCurrentProcess(), (unsigned char *)f->live + (size_t)idx * e, &u, e, &w) && w == e;
#else
    (void)f; (void)idx; (void)v;
    return 0;
#endif
}

void terrain_raise_begin(void)
{
    g_nnew = 0;
}

void terrain_raise(const float c[3], const float ex[3], const float ey[3], float W, float L, const float *prof, int np,
                   float sx, float sy)
{
    int fi;
    if (!g_ready || !g_live_ok || np < 1 || L <= 0.0f)
        return;
    for (fi = 0; fi < g_num_fields; ++fi) {
        const TerrainField *f = &g_fields[fi];
        float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        int k, ix0, ix1, iz0, iz1, ix, iz;
        if (!f->live || f->scale == 0.0f)
            continue;
        for (k = 0; k < 4; ++k) {
            float sx = (k & 1) ? W : -W, sy = (k & 2) ? L : 0.0f;
            float px = c[0] + ex[0] * sx + ey[0] * sy, pz = c[2] + ex[2] * sx + ey[2] * sy;
            x0 = px < x0 ? px : x0; x1 = px > x1 ? px : x1;
            z0 = pz < z0 ? pz : z0; z1 = pz > z1 ? pz : z1;
        }
        ix0 = (int)floorf((x0 - f->center[0] + f->half_w) / f->sample_w) - 1;
        ix1 = (int)ceilf((x1 - f->center[0] + f->half_w) / f->sample_w) + 1;
        iz0 = (int)floorf((z0 - f->center[2] + f->half_d) / f->sample_d) - 1;
        iz1 = (int)ceilf((z1 - f->center[2] + f->half_d) / f->sample_d) + 1;
        if (ix0 < 0) ix0 = 0;
        if (iz0 < 0) iz0 = 0;
        if (ix1 > f->nx - 1) ix1 = f->nx - 1;
        if (iz1 > f->nz - 1) iz1 = f->nz - 1;
        for (iz = iz0; iz <= iz1; ++iz)
            for (ix = ix0; ix <= ix1; ++ix) {
                float px = ix * f->sample_w + f->center[0] - f->half_w, pz = iz * f->sample_d + f->center[2] - f->half_d;
                float dx = px - c[0], dz = pz - c[2], lx = dx * ex[0] + dz * ex[2], ly = dx * ey[0] + dz * ey[2], t, h;
                int j;
                /* v1.8.131 : + un peu plus d'une demi-case de chaque côté : le bord du tablier à sa pleine hauteur
                   (avant : la dernière rangée retombait en pente sur 22 cm, la roue avant glissait du côté) */
                if (fabsf(lx) > W + 0.6f * f->sample_w || ly < 0.0f || ly > L || g_nnew >= MOD_MAX)
                    continue;
                t = ly / L * np;
                j = (int)t;
                if (j >= np)
                    j = np - 1;
                t -= (float)j;
                h = c[1] + prof[j] + (prof[j + 1] - prof[j]) * t + sx * lx + sy * ly;
                g_new[g_nnew].field = fi;
                g_new[g_nnew].idx = ix + iz * f->nx;
                g_new[g_nnew].want = (h - f->center[1] - f->offset) / f->scale;
                g_new[g_nnew].orig = NAN;
                ++g_nnew;
            }
    }
}

/* v1.8.125 : valeur d'origine d'une case relevée (liste triée) */
static int mod_orig(int field, int idx, float *raw)
{
    int lo = 0, hi = g_nmod - 1;
    if (g_mod_busy)
        return 0;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_mod[m].field == field && g_mod[m].idx == idx) {
            if (!(g_mod[m].want > g_mod[m].orig))
                return 0;
            *raw = g_mod[m].orig;
            return 1;
        }
        if (g_mod[m].field < field || (g_mod[m].field == field && g_mod[m].idx < idx))
            lo = m + 1;
        else
            hi = m - 1;
    }
    return 0;
}

static int mod_cmp(const void *a, const void *b)
{
    const Mod *x = (const Mod *)a, *y = (const Mod *)b;
    if (x->field != y->field)
        return x->field < y->field ? -1 : 1;
    if (x->idx != y->idx)
        return x->idx < y->idx ? -1 : 1;
    return x->want > y->want ? -1 : x->want < y->want ? 1 : 0; /* (la plus haute en premier) */
}

/* fusion de la nouvelle liste (rampes changées) avec l'ancienne : origines gardées, cases plus voulues remises */
int terrain_raise_end(void (*log)(const char *fmt, ...))
{
    int i = 0, j = 0, n = 0, back = 0, added = 0, k;
    qsort(g_new, (size_t)g_nnew, sizeof(Mod), mod_cmp);
    /* doublons (rampes qui se chevauchent) : la plus haute */
    for (k = 0; k < g_nnew; ++k)
        if (!n || g_new[k].field != g_new[n - 1].field || g_new[k].idx != g_new[n - 1].idx)
            g_new[n++] = g_new[k];
    g_nnew = n;
    g_mod_busy = 1;
    n = 0;
    {
        static Mod out[MOD_MAX];
        while (i < g_nmod || j < g_nnew) {
            int c = i >= g_nmod ? 1 : j >= g_nnew ? -1
                  : g_mod[i].field != g_new[j].field ? (g_mod[i].field < g_new[j].field ? -1 : 1)
                  : g_mod[i].idx != g_new[j].idx ? (g_mod[i].idx < g_new[j].idx ? -1 : 1) : 0;
            if (c < 0) {
                /* plus de rampe ici : sol d'origine */
                if (g_mod[i].field < g_num_fields) {
                    int c;
                    raw_set(&g_fields[g_mod[i].field], g_mod[i].idx, g_mod[i].orig);
                    if (g_copy_state == 2)
                        for (c = 0; c < g_ncopy[g_mod[i].field]; ++c) {
                            TerrainField t = g_fields[g_mod[i].field];
                            t.live = (const void *)g_copy[g_mod[i].field][c];
                            raw_set(&t, g_mod[i].idx, g_mod[i].orig);
                        }
                }
                ++back;
                ++i;
            } else if (c > 0) {
                Mod m = g_new[j++];
                if (!raw_get(&g_fields[m.field], m.idx, &m.orig))
                    continue;
                out[n++] = m;
                ++added;
            } else {
                Mod m = g_new[j++];
                m.orig = g_mod[i++].orig;
                out[n++] = m;
            }
        }
        memcpy(g_mod, out, (size_t)n * sizeof(Mod));
        g_nmod = n;
    }
    g_mod_busy = 0;
    if (g_nmod > 0)
        copy_find_async();
    if (log)
        log("rampes : relief du jeu : %d points (%d nouveaux, %d remis au sol d'origine)", g_nmod, added, back);
    if (log) {
        /* v1.8.127 : grilles touchées (chacune a sa hauteur max) */
        int fi, k2;
        for (fi = 0; fi < g_num_fields; ++fi) {
            int c = 0;
            for (k2 = 0; k2 < g_nmod; ++k2)
                c += g_mod[k2].field == fi;
            if (c)
                log("rampes : grille %d (%dx%d, pas %.2f m) : %d points, en-tete ODE %s", fi, g_fields[fi].nx, g_fields[fi].nz,
                    g_fields[fi].sample_w, c, g_fields[fi].hdr ? "trouve" : "PAS trouve");
        }
    }
    return terrain_raise_keep(log);
}

void terrain_raise_set_bike(float x, float z, int ok)
{
    g_bike_x = x;
    g_bike_z = z;
    g_bike_ok = ok;
}

#ifdef _WIN32
static void float_grid(int fi, int need, void (*log)(const char *fmt, ...));

/* v1.8.140 : voir g_href */
static int rescale_all(void (*log)(const char *fmt, ...))
{
    TerrainField *f = &g_fields[0];
    size_t n = (size_t)f->nx * (size_t)f->nz, j;
    uintptr_t arr[2 + 2 * COPY_MAX];
    int na = 0, a, k, fi;
    float olds = f->scale;
    /* v1.8.141 : coupé : essai du 23/11 : moto éjectée à 250 km/h (le jeu ne calcule pas les roues comme prévu) */
    if (1 || g_rescaled || f->mode != 2 || !f->live || !g_nhref || g_copy_state != 2)
        return 0;
    /* ODE relit d'abord les grilles du jeu (notre grille en flottants n'est plus utile) */
    for (fi = 0; fi < g_num_fields; ++fi)
        if (g_fswap[fi])
            float_grid(fi, 0, log);
    arr[na++] = (uintptr_t)f->live;
    for (fi = 0; fi < g_num_fields && fi < 2; ++fi) {
        int c;
        if (g_fields[fi].live && g_fields[fi].nx == f->nx && g_fields[fi].nz == f->nz) {
            for (a = 0; a < na && arr[a] != (uintptr_t)g_fields[fi].live; ++a)
                ;
            if (a == na)
                arr[na++] = (uintptr_t)g_fields[fi].live;
        }
        for (c = 0; c < g_ncopy[fi]; ++c) {
            for (a = 0; a < na && arr[a] != g_copy[fi][c]; ++a)
                ;
            if (a == na && na < (int)(sizeof(arr) / sizeof(arr[0])))
                arr[na++] = g_copy[fi][c];
        }
    }
    /* grilles : r' = (r - 32767) / 2 */
    for (a = 0; a < na; ++a) {
        volatile short *g = (volatile short *)arr[a];
        for (j = 0; j < n; ++j)
            g[j] = (short)(((int)g[j] - 32767) >> 1);
    }
    /* hauteurs totales du jeu x 2 */
    for (k = 0; k < g_nhref; ++k)
        *(volatile float *)g_href[k] *= 2.0f;
    /* reliefs ODE : h = r s + o  ->  r' (2 s) + o + 32767 s */
    {
        uintptr_t hd[1 + WREF_MAX];
        int nh = 0, w;
        if (f->hdr)
            hd[nh++] = f->hdr;
        for (w = 0; w < g_nwref; ++w)
            hd[nh++] = g_wref[w] - 80;
        for (k = 0; k < nh; ++k) {
            volatile float *ps = (volatile float *)(hd[k] + (f->hdr_new ? 48 : 44)), *po = ps + 1;
            float s0 = *ps;
            *po += 32767.0f * s0;
            *ps = s0 * 2.0f;
        }
        /* 2e relief : sa hauteur max monte de la hauteur totale (le 1er : à chaque appel, plus bas) */
        for (w = 0; w < g_nwref; ++w) {
            int off = g_dataoff_fn ? g_dataoff_fn(f->hdr) : -1, q;
            *(volatile float *)(g_wref[w] - 80 + 40) = g_wref_maxh[w] + olds * 65535.0f + 1.0f;
            for (q = 0; q < g_wref_ngeom[w] && off > 0; ++q)
                if (g_dirty_fn)
                    g_dirty_fn(g_wref_geomref[w][q] - (uintptr_t)off);
        }
    }
    /* nos grilles (mêmes données) et nos cases */
    for (fi = 0; fi < g_num_fields; ++fi)
        if (g_fields[fi].mode == 2 && fabsf(g_fields[fi].scale - olds) < 1e-9f) {
            g_fields[fi].offset += 32767.0f * g_fields[fi].scale;
            g_fields[fi].scale *= 2.0f;
        }
    for (k = 0; k < g_nmod; ++k) {
        g_mod[k].orig = (g_mod[k].orig - 32767.0f) * 0.5f;
        g_mod[k].want = (g_mod[k].want - 32767.0f) * 0.5f;
    }
    g_rescaled = 1;
    if (log)
        log("rampes : grille du jeu a l'echelle double (%d grille(s), %d hauteur(s) du jeu) : hauteur max %.2f -> %.2f m", na,
            g_nhref, olds * 65535.0f + f->center[1] - 0.5f * olds * 65535.0f, 2.0f * olds * 65535.0f + f->center[1] - 0.5f * olds * 65535.0f);
    return 1;
}

/* v1.8.129 : grille en flottants pour ODE (voir g_ff) */
static void float_grid(int fi, int need, void (*log)(const char *fmt, ...))
{
    TerrainField *f = &g_fields[fi];
    size_t n = (size_t)f->nx * (size_t)f->nz, i;
    uintptr_t pdata = f->hdr + (f->hdr_new ? 80 : 72), pmode = f->hdr + (f->hdr_new ? 72 : 68),
              pcopy = f->hdr + (f->hdr_new ? 64 : 60);
    const short *live = (const short *)f->live;
    if (!f->hdr || !f->live || f->mode != 2)
        return;
    {
        /* (deux grilles du jeu avec la même structure ODE : la 1re suffit) */
        int o;
        for (o = 0; o < fi; ++o)
            if (g_fields[o].hdr == f->hdr)
                return;
    }
    if (need && !g_fswap[fi]) {
        int32_t cp = -1, md = -1;
        uint64_t dp = 0;
        SIZE_T gr = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (const void *)pcopy, &cp, 4, &gr) ||
            !ReadProcessMemory(GetCurrentProcess(), (const void *)pmode, &md, 4, &gr) ||
            !ReadProcessMemory(GetCurrentProcess(), (const void *)pdata, &dp, 8, &gr) || md != 2 ||
            dp != (uint64_t)(uintptr_t)f->live || cp != 0) {
            static int said = 0;
            if (log && said < 4) {
                ++said;
                log("rampes : grille %d : passage en flottants impossible (copie %d, mode %d, donnees %s)", fi, cp, md,
                    dp == (uint64_t)(uintptr_t)f->live ? "ok" : "autres");
            }
            return;
        }
        if (!g_ff[fi])
            g_ff[fi] = (float *)VirtualAlloc(NULL, n * sizeof(float), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_fs[fi])
            g_fs[fi] = (short *)malloc(n * sizeof(short));
        if (!g_ff[fi] || !g_fs[fi])
            return;
        memcpy(g_fs[fi], live, n * sizeof(short));
        for (i = 0; i < n; ++i)
            g_ff[fi][i] = (float)g_fs[fi][i];
        for (i = 0; i < (size_t)g_nmod; ++i)
            if (g_mod[i].field == fi && g_mod[i].want > g_ff[fi][g_mod[i].idx])
                g_ff[fi][g_mod[i].idx] = g_mod[i].want;
        {
            /* pointeur puis mode (lus ensemble par ODE à chaque contact) */
            uint64_t np = (uint64_t)(uintptr_t)g_ff[fi];
            int32_t m3 = 3;
            *(volatile uint64_t *)pdata = np;
            *(volatile int32_t *)pmode = m3;
        }
        g_fswap[fi] = 1;
        g_fscan[fi] = 0;
        if (log)
            log("rampes : grille %d : ODE lit maintenant une grille en flottants (rampes plus hautes que la piste)", fi);
        return;
    }
    if (!need && g_fswap[fi]) {
        int w;
        for (w = 0; w < g_nwref; ++w)
            if (g_wref_on[w] && g_wref_field[w] == fi) {
                int k, off = g_dataoff_fn ? g_dataoff_fn(f->hdr) : -1;
                *(volatile int32_t *)(g_wref[w] - 8) = 2;
                *(volatile uint64_t *)g_wref[w] = (uint64_t)g_wref_old[w];
                *(volatile float *)(g_wref[w] - 80 + 40) = g_wref_maxh[w];
                for (k = 0; k < g_wref_ngeom[w] && off > 0; ++k)
                    if (g_dirty_fn)
                        g_dirty_fn(g_wref_geomref[w][k] - (uintptr_t)off);
                g_wref_on[w] = 0;
                if (log)
                    log("rampes : 2e structure de relief %p : relit la grille du jeu", (void *)g_wref[w]);
            }
        *(volatile int32_t *)pmode = 2;
        *(volatile uint64_t *)pdata = (uint64_t)(uintptr_t)f->live;
        g_fswap[fi] = 0; /* (notre grille reste allouée : réutilisée, et jamais libérée pendant qu'ODE pourrait la lire) */
        if (log)
            log("rampes : grille %d : ODE relit la grille du jeu", fi);
        return;
    }
    if (!g_fswap[fi])
        return;
    {
        /* v1.8.135 : la 2e structure de relief (roues ?) lit aussi notre grille en flottants */
        int w;
        for (w = 0; w < g_nwref && 0; ++w) /* v1.8.141 : essai coupé (sans effet sur les roues) */
            if (!g_wref_on[w] && g_wref_field[w] == fi && g_copy_state == 2) {
                int32_t md = 0;
                uint64_t dp = 0;
                SIZE_T gr = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (const void *)(g_wref[w] - 8), &md, 4, &gr) &&
                    ReadProcessMemory(GetCurrentProcess(), (const void *)g_wref[w], &dp, 8, &gr) && md == 2 &&
                    dp == (uint64_t)g_wref_old[w]) {
                    int k, off = g_dataoff_fn ? g_dataoff_fn(f->hdr) : -1, nd = 0;
                    *(volatile uint64_t *)g_wref[w] = (uint64_t)(uintptr_t)g_ff[fi];
                    *(volatile int32_t *)(g_wref[w] - 8) = 3;
                    /* v1.8.138 : sa hauteur max aussi (avant : 9,57 m, rien au-dessus pour les roues) */
                    *(volatile float *)(g_wref[w] - 80 + 40) = g_wref_maxh[w] + 20.0f;
                    for (k = 0; k < g_wref_ngeom[w] && off > 0; ++k)
                        if (g_dirty_fn && g_dirty_fn(g_wref_geomref[w][k] - (uintptr_t)off))
                            ++nd;
                    g_wref_on[w] = 1;
                    if (log)
                        log("rampes : 2e relief %p : grille en flottants, hauteur max %.2f -> %.2f, %d objet(s) ODE a recalculer "
                            "(decalage %d)", (void *)(g_wref[w] - 80), g_wref_maxh[w], g_wref_maxh[w] + 20.0f, nd, off);
                } else {
                    g_wref_field[w] = -1; /* (plus la même : on n'y touche pas) */
                    if (log)
                        log("rampes : 2e structure de relief %p changee (mode %d), ignoree", (void *)g_wref[w], md);
                }
            }
    }
    {
        /* ornières : autour de la moto à chaque appel, et toute la grille petit à petit */
        float *F = g_ff[fi];
        short *S = g_fs[fi];
        int x0, x1, z0, z1, ix, iz, chunk = (int)(n / 150) + 1;
        size_t k, st;
        if (g_bike_ok) {
            x0 = (int)((g_bike_x - f->center[0] + f->half_w - 25.0f) / f->sample_w);
            x1 = (int)((g_bike_x - f->center[0] + f->half_w + 25.0f) / f->sample_w);
            z0 = (int)((g_bike_z - f->center[2] + f->half_d - 25.0f) / f->sample_d);
            z1 = (int)((g_bike_z - f->center[2] + f->half_d + 25.0f) / f->sample_d);
            x0 = x0 < 0 ? 0 : x0; z0 = z0 < 0 ? 0 : z0;
            x1 = x1 > f->nx - 1 ? f->nx - 1 : x1; z1 = z1 > f->nz - 1 ? f->nz - 1 : z1;
            for (iz = z0; iz <= z1; ++iz) {
                size_t r = (size_t)iz * (size_t)f->nx;
                if (x1 >= x0 && memcmp(S + r + x0, live + r + x0, (size_t)(x1 - x0 + 1) * sizeof(short)))
                    for (ix = x0; ix <= x1; ++ix)
                        if (S[r + ix] != live[r + ix]) {
                            S[r + ix] = live[r + ix];
                            F[r + ix] = (float)live[r + ix];
                        }
            }
        }
        st = (size_t)g_fscan[fi] * (size_t)chunk;
        if (st >= n) {
            st = 0;
            g_fscan[fi] = 0;
        }
        for (k = st; k < st + (size_t)chunk && k < n; k += 64) {
            size_t m = k + 64 <= n ? 64 : n - k, j;
            if (memcmp(S + k, live + k, m * sizeof(short)))
                for (j = k; j < k + m; ++j)
                    if (S[j] != live[j]) {
                        S[j] = live[j];
                        F[j] = (float)live[j];
                    }
        }
        ++g_fscan[fi];
        /* rampes : jamais sous leur hauteur */
        for (k = 0; k < (size_t)g_nmod; ++k)
            if (g_mod[k].field == fi && F[g_mod[k].idx] < g_mod[k].want)
                F[g_mod[k].idx] = g_mod[k].want;
    }
}
#endif

/* à chaque appel : la grille du jeu remise à la hauteur des rampes là où le jeu l'a rabaissée */
int terrain_raise_keep(void (*log)(const char *fmt, ...))
{
    int i, bounds = 0;
    /* v1.8.127 : hauteur max de CHAQUE grille touchée (avant : seulement celle du point le plus haut ;
       sur une piste plate, la moto passait à travers la rampe au-dessus du relief d'origine) */
    static float top[64];
    for (i = 0; i < 64; ++i)
        top[i] = -1e30f;
    if (g_mod_busy || !g_live_ok)
        return 0;
#ifdef _WIN32
    if (g_nmod == 0) {
        /* v1.8.129 : plus de rampes : ODE relit la grille du jeu */
        int fi;
        for (fi = 0; fi < g_num_fields; ++fi)
            if (g_fswap[fi])
                float_grid(fi, 0, log);
        return 0;
    }
    {
        /* grille du jeu toujours là ? (vérifié une fois par seconde) */
        static DWORD chk = 0;
        static int ok = 0;
        if (GetTickCount() - chk > 1000) {
            MEMORY_BASIC_INFORMATION mi;
            chk = GetTickCount();
            ok = g_nmod > 0 && g_mod[0].field < g_num_fields && g_fields[g_mod[0].field].live &&
                 VirtualQuery(g_fields[g_mod[0].field].live, &mi, sizeof(mi)) == sizeof(mi) && mi.State == MEM_COMMIT &&
                 (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
        }
        if (!ok)
            return 0;
    }
    {
        /* v1.8.127 : copies toujours là et toujours pareilles ? (une fois par seconde ; sinon oubliées) */
        static DWORD cchk = 0;
        if (g_copy_state == 2 && GetTickCount() - cchk > 1000) {
            int fi, c, k;
            cchk = GetTickCount();
            for (fi = 0; fi < g_num_fields; ++fi)
                for (c = 0; c < g_ncopy[fi]; ++c) {
                    const TerrainField *f = &g_fields[fi];
                    int bad = 0;
                    for (k = 0; k < 32; ++k) {
                        int ix = (int)(((k % 8) + 0.37f) * f->nx / 8.0f), iz = (int)(((k / 8) + 0.61f) * f->nz / 4.0f);
                        float a2, b2;
                        if (iz >= f->nz)
                            iz = f->nz - 1;
                        if (!read_raw_at((uintptr_t)f->live, f->mode, ix + iz * f->nx, &a2) ||
                            !read_raw_at(g_copy[fi][c], f->mode, ix + iz * f->nx, &b2) || fabsf(a2 - b2) > 400.0f)
                            ++bad;
                    }
                    if (bad > 4) {
                        if (log)
                            log("rampes : copie %p de la grille %d plus valable, oubliee", (void *)g_copy[fi][c], fi);
                        g_copy[fi][c] = g_copy[fi][g_ncopy[fi] - 1];
                        --g_ncopy[fi];
                        --c;
                    }
                }
        }
    }
#endif
    for (i = 0; i < g_nmod; ++i) {
        const TerrainField *f;
        if (g_mod[i].field >= g_num_fields || g_mod[i].want <= g_mod[i].orig)
            continue;
        f = &g_fields[g_mod[i].field];
        if (!f->live)
            continue;
        if (f->mode == 2) {
            /* (accès direct : la grille du jeu existe tant que la piste est chargée) */
            volatile short *p = (volatile short *)f->live + g_mod[i].idx;
            short w = (short)(g_mod[i].want > 32767.0f ? 32767.0f : floorf(g_mod[i].want + 0.5f));
            int c;
            if (*p < w - 1) {
                short o = (short)floorf(g_mod[i].orig + 0.5f);
                if (*p == o)
                    ++g_reset_orig;
                else if (*p > o + 2)
                    ++g_reset_mid; /* (entre le sol et la rampe : creusé par la roue) */
                *p = w;
                ++g_resets;
            }
            if (g_copy_state == 2)
                for (c = 0; c < g_ncopy[g_mod[i].field]; ++c) {
                    volatile short *q = (volatile short *)g_copy[g_mod[i].field][c] + g_mod[i].idx;
                    short o = (short)floorf(g_mod[i].orig + 0.5f);
                    /* (seulement si la case ressemble encore au sol d'ici : bloc pas réutilisé pour autre chose) */
                    if (*q < w - 1 && *q > o - 400)
                        *q = w;
                }
        } else {
            float cur;
            if (raw_get(f, g_mod[i].idx, &cur) && cur < g_mod[i].want - 0.5f) {
                raw_set(f, g_mod[i].idx, g_mod[i].want);
                ++g_resets;
            }
        }
        if (g_mod[i].field < 64 && g_mod[i].want * f->scale + f->offset > top[g_mod[i].field])
            top[g_mod[i].field] = g_mod[i].want * f->scale + f->offset;
    }
#ifdef _WIN32
    {
        /* v1.8.129 : grille en flottants seulement si une rampe dépasse ce que la grille du jeu peut tenir */
        static int need[TERRAIN_MAX];
        static int last_nmod = -1;
        int fi;
        if (last_nmod != g_nmod) {
            last_nmod = g_nmod;
            for (fi = 0; fi < g_num_fields; ++fi)
                need[fi] = 0;
            for (i = 0; i < g_nmod; ++i)
                if (g_mod[i].field < g_num_fields && g_fields[g_mod[i].field].mode == 2 && g_mod[i].want > 32766.0f)
                    need[g_mod[i].field] = 1;
        }
        {
            int any = 0;
            for (fi = 0; fi < g_num_fields; ++fi)
                any |= need[fi];
            if (any && !g_rescaled && rescale_all(log)) {
                last_nmod = -1; /* (besoins recalculés à l'appel suivant, en nouvelles unités) */
                for (fi = 0; fi < g_num_fields; ++fi)
                    need[fi] = 0;
            }
        }
        if (!g_rescaled)
            for (fi = 0; fi < g_num_fields; ++fi)
                float_grid(fi, need[fi], log);
    }
#endif
    for (i = 0; i < g_num_fields && i < 64; ++i) {
        const TerrainField *tf = &g_fields[i];
        if (top[i] < -1e29f || !tf->hdr)
            continue;
        /* hauteur max du relief (boîte englobante d'ODE : au-dessus, plus de contact) */
#ifdef _WIN32
        {
            uintptr_t a = tf->hdr + (tf->hdr_new ? 40 : 36);
            float mx;
            SIZE_T gr = 0;
            if (ReadProcessMemory(GetCurrentProcess(), (const void *)a, &mx, 4, &gr) && gr == 4 && isfinite(mx) &&
                top[i] > mx - 0.2f) {
                float nm = top[i] + 1.0f;
                SIZE_T w = 0;
                if (WriteProcessMemory(GetCurrentProcess(), (void *)a, &nm, 4, &w) && w == 4) {
                    if (log)
                        log("rampes : grille %d : hauteur max du relief %.2f -> %.2f m", i, mx, nm);
                    bounds = 1;
                }
            }
        }
#endif
    }
    return bounds;
}

float terrain_grid_top(void)
{
    /* v1.8.141 : plus haut point que la grille 16 bits du jeu peut tenir (les roues ne voient rien au-dessus) */
    const TerrainField *f = &g_fields[0];
    if (!g_ready || g_num_fields < 1 || f->mode != 2)
        return 1e9f;
    return 32767.0f * f->scale + f->offset + f->center[1];
}

int terrain_raise_resets(void)
{
    int r = g_resets;
    g_resets = 0;
    return r;
}

/* v1.8.128 : sonde : hauteurs des grilles (et de leurs copies) sous (x, z), hauteur max d'ODE */
void terrain_raise_probe(float x, float z, char *out, size_t n)
{
    int fi, c;
    size_t l = 0;
    out[0] = 0;
#ifdef _WIN32
    for (fi = 0; fi < g_num_fields && l + 80 < n; ++fi) {
        const TerrainField *f = &g_fields[fi];
        float lx = x - f->center[0] + f->half_w, lz = z - f->center[2] + f->half_d, v, mx = 0.0f;
        int ix = (int)floorf(lx / f->sample_w + 0.5f), iz = (int)floorf(lz / f->sample_d + 0.5f);
        SIZE_T gr = 0;
        if (!f->live || ix < 0 || iz < 0 || ix >= f->nx || iz >= f->nz)
            continue;
        if (f->hdr)
            ReadProcessMemory(GetCurrentProcess(), (const void *)(f->hdr + (f->hdr_new ? 40 : 36)), &mx, 4, &gr);
        l += (size_t)snprintf(out + l, n - l, " g%d[", fi);
        if (read_raw_at((uintptr_t)f->live, f->mode, ix + iz * f->nx, &v))
            l += (size_t)snprintf(out + l, n - l, "%.2f", v * f->scale + f->offset + f->center[1]);
        if (g_fswap[fi] && g_ff[fi])
            l += (size_t)snprintf(out + l, n - l, " flottants %.2f", g_ff[fi][ix + iz * f->nx] * f->scale + f->offset + f->center[1]);
        for (c = 0; c < g_ncopy[fi] && l + 20 < n; ++c)
            if (read_raw_at(g_copy[fi][c], f->mode, ix + iz * f->nx, &v))
                l += (size_t)snprintf(out + l, n - l, " c%.2f", v * f->scale + f->offset + f->center[1]);
        l += (size_t)snprintf(out + l, n - l, " max %.2f]", mx + f->center[1]);
    }
#else
    (void)fi; (void)c; (void)l; (void)x; (void)z; (void)n;
#endif
}

/* v1.8.127 : détail des remises (au sol d'origine exactement / à mi-hauteur) et copies trouvées */
void terrain_raise_diag(void (*log)(const char *fmt, ...))
{
    if (log && (g_reset_orig || g_reset_mid))
        log("rampes : remises : %d au sol d'origine exact, %d entre le sol et la rampe", g_reset_orig, g_reset_mid);
    g_reset_orig = g_reset_mid = 0;
    if (log && g_copy_state == 2 && !g_copy_logged) {
        g_copy_logged = 1;
        log("rampes : copies de la grille du jeu :%s", g_copy_msg);
        if (g_ref_msg[0])
            log("rampes : adresses des grilles gardees par le jeu (flottants de -64 a +32 octets) :%s", g_ref_msg);
        {
            int fi3;
            for (fi3 = 0; fi3 < g_num_fields && fi3 < 2; ++fi3)
                log("rampes : grille %d : echelle %g decalage %g centre y %g, donnees %p, en-tete %p", fi3, g_fields[fi3].scale,
                    g_fields[fi3].offset, g_fields[fi3].center[1], (const void *)g_fields[fi3].live, (void *)g_fields[fi3].hdr);
        }
    }
}

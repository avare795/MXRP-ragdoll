/*
 * glcam.c - interception des matrices OpenGL du jeu (voir glcam.h)
 */
#include "glcam.h"
#include "objinst.h"

#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701

typedef void(APIENTRY *PFN_MatrixMode)(unsigned int);
typedef void(APIENTRY *PFN_LoadIdentity)(void);
typedef void(APIENTRY *PFN_Matrixf)(const float *);
typedef void(APIENTRY *PFN_Frustum)(double, double, double, double, double, double);
typedef void(APIENTRY *PFN_Viewport)(int, int, int, int);
typedef void(APIENTRY *PFN_DrawElements)(unsigned int, int, unsigned int, const void *);
typedef BOOL(WINAPI *PFN_SwapBuffers)(HDC);

static PFN_MatrixMode o_MatrixMode;
static PFN_LoadIdentity o_LoadIdentity;
static PFN_Matrixf o_LoadMatrixf, o_MultMatrixf;
static PFN_Frustum o_Frustum, o_Ortho;
static PFN_Viewport o_Viewport;
static PFN_DrawElements o_DrawElements;
static PFN_SwapBuffers o_SwapBuffers;
static void scene_end(void);

/* ---------- enregistrement d'une image ---------- */

enum { EV_MODE, EV_IDENT, EV_LOAD, EV_MULT, EV_FRUSTUM, EV_ORTHO, EV_VIEWPORT, EV_DRAW };

typedef struct {
    unsigned char type;
    unsigned int mode; /* matrice courante (modelview/projection) */
    int count;         /* nombre de dessins regroupés (EV_DRAW) */
    float m[16];
} CapEvent;

#define CAP_MAX 600

static CapEvent g_cap[CAP_MAX];
static volatile int g_cap_n = 0;
static volatile int g_cap_state = 0; /* 0 rien, 1 demandé, 2 en cours, 3 prêt */
static int g_cap_total[8];
static char g_cap_why[64];
static unsigned int g_mode = GL_MODELVIEW;

static void cap_add(int type, const float *m, int nm)
{
    CapEvent *e;
    if (g_cap_state != 2)
        return;
    ++g_cap_total[type];
    if (type == EV_DRAW && g_cap_n > 0 && g_cap[g_cap_n - 1].type == EV_DRAW) {
        g_cap[g_cap_n - 1].count++;
        return;
    }
    if (g_cap_n >= CAP_MAX)
        return;
    e = &g_cap[g_cap_n];
    e->type = (unsigned char)type;
    e->mode = g_mode;
    e->count = 1;
    memset(e->m, 0, sizeof(e->m));
    if (m && nm > 0)
        memcpy(e->m, m, (size_t)nm * sizeof(float));
    g_cap_n = g_cap_n + 1;
}

static void APIENTRY hk_MatrixMode(unsigned int mode)
{
    if (mode == GL_PROJECTION)
        scene_end(); /* nouvelle passe : la précédente est finie */
    g_mode = mode;
    if (g_cap_state == 2) {
        float f = (float)mode;
        cap_add(EV_MODE, &f, 1);
    }
    o_MatrixMode(mode);
}

/* ---------- remplacement de la caméra ---------- */

static volatile GlcamViewFn g_view_fn = NULL;
static int g_pass = 0;      /* 0 hors passe, 1 en attente de la vue, 2 vue remplacée */
static float g_corr[16];    /* nouvelle vue x inverse(ancienne vue) */
static float g_game_view[16];
static const void *g_game_view_ptr = NULL;
static volatile int g_game_view_ok = 0;
/* perspective de la passe principale (celle dont on remplace la vue) */
static float g_frustum_pending[6], g_frustum[6];
static volatile int g_frustum_ok = 0;
/* v1.8.48 : objets du plugin dessinés en 3D dans la passe en perspective (avec la profondeur) */
static float g_scene_view[16];
static int g_scene_ok = 0, g_scene_done = 0, g_pass_fbo = -1;
static volatile GlcamSceneFn g_scene_fn = NULL;
static volatile int g_scene_want = 0;
static volatile DWORD g_scene_tick = 0;
/* v1.8.49 : diagnostic (journal) : pourquoi le dessin 3D se fait ou pas */
static struct {
    int passes, draws, d_on, dw_on, cw_on, done_draw, done_end, end_fbo_diff, end_none, no_gl, done_rebind;
} g_st;
/* v1.8.98 : passes en perspective d'une image (pour savoir laquelle est l'image vue) */
#define PI_MAX 8
typedef struct {
    int fbo, vp[4], view, how; /* how : 0 rien, 1 avant un dessin, 2 fin de passe, 3 cible changee, 4 pas d'opengl */
    float fr[6];
} PassInfo;
static PassInfo g_pi[PI_MAX], g_pi_last[PI_MAX];
static int g_pi_n = 0, g_pi_last_n = 0, g_pi_frames = 0;
/* v1.8.198 : cibles (FBO) où nos objets sont déjà dessinés dans l'image */
static int g_drawn_fbo[PI_MAX], g_drawn_n = 0;
static int fbo_drawn(int f)
{
    int i;
    for (i = 0; i < g_drawn_n; ++i)
        if (g_drawn_fbo[i] == f)
            return 1;
    return 0;
}
static void fbo_mark(int f)
{
    if (!fbo_drawn(f) && g_drawn_n < PI_MAX)
        g_drawn_fbo[g_drawn_n++] = f;
}
static double g_draw_us = 0.0;
static int g_draw_n = 0;
static void scene_begin(void);
static void scene_on_draw(void);
static void scene_end(void);

int glcam_frustum(float out[6])
{
    if (!g_frustum_ok)
        return 0;
    memcpy(out, g_frustum, sizeof(g_frustum));
    return 1;
}

int glcam_game_view(float out[16], const void **ptr)
{
    if (!g_game_view_ok)
        return 0;
    memcpy(out, g_game_view, sizeof(g_game_view));
    if (ptr)
        *ptr = g_game_view_ptr;
    return 1;
}

void glcam_set_view_fn(GlcamViewFn fn)
{
    g_view_fn = fn;
}

static volatile GlcamFrameFn g_frame_fn = NULL;
static volatile unsigned long g_render_tid = 0;

unsigned long glcam_render_thread(void)
{
    return g_render_tid;
}
static int g_frame_started = 0;

void glcam_set_frame_fn(GlcamFrameFn fn)
{
    g_frame_fn = fn;
}

static void frame_event(int where)
{
    GlcamFrameFn fn = g_frame_fn;
    if (fn)
        fn(where);
}

/* r = a x b, matrices 4x4 en colonnes (OpenGL) */
static void mat_mul(const float *a, const float *b, float *r)
{
    int i, j, k;
    for (j = 0; j < 4; ++j)
        for (i = 0; i < 4; ++i) {
            float s = 0.0f;
            for (k = 0; k < 4; ++k)
                s += a[k * 4 + i] * b[j * 4 + k];
            r[j * 4 + i] = s;
        }
}

/* inverse d'une matrice rigide (rotation + translation), en colonnes */
static void mat_rigid_inv(const float *m, float *r)
{
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            r[j * 4 + i] = m[i * 4 + j];
    for (i = 0; i < 3; ++i)
        r[12 + i] = -(r[i] * m[12] + r[4 + i] * m[13] + r[8 + i] * m[14]);
    r[3] = r[7] = r[11] = 0.0f;
    r[15] = 1.0f;
}

static void APIENTRY hk_LoadIdentity(void)
{
    cap_add(EV_IDENT, NULL, 0);
    o_LoadIdentity();
}

static void APIENTRY hk_LoadMatrixf(const float *m)
{
    cap_add(EV_LOAD, m, 16);
    if (g_mode == GL_MODELVIEW && g_pass == 1) {
        GlcamViewFn fn = g_view_fn;
        float nv[16], inv[16];
        memcpy(g_game_view, m, sizeof(g_game_view));
        g_game_view_ptr = m;
        g_game_view_ok = 1;
        if (fn && fn(m, nv)) {
            memcpy(g_scene_view, nv, sizeof(g_scene_view));
            scene_begin();
            mat_rigid_inv(m, inv);
            mat_mul(nv, inv, g_corr);
            g_pass = 2;
            if (g_pi_n > 0)
                g_pi[g_pi_n - 1].view = 1;
            memcpy(g_frustum, g_frustum_pending, sizeof(g_frustum));
            g_frustum_ok = 1;
            o_LoadMatrixf(nv);
            return;
        }
        memcpy(g_scene_view, m, sizeof(g_scene_view)); /* vue du jeu (en roulant) */
        scene_begin();
        g_pass = 0;
    } else if (g_mode == GL_MODELVIEW && g_pass == 2) {
        float r[16];
        mat_mul(g_corr, m, r);
        o_LoadMatrixf(r);
        return;
    }
    o_LoadMatrixf(m);
}

static void APIENTRY hk_MultMatrixf(const float *m)
{
    cap_add(EV_MULT, m, 16);
    o_MultMatrixf(m);
}

static volatile float g_fov_want = 0.0f, g_fov_zoom = 1.0f, g_fov_game = 0.0f;
/* v1.8.164 : passes en perspective de l'image : la plus grande (l'image vue) donne le champ de vision du jeu */
static int g_vp_w = 0, g_vp_h = 0;
static float g_fv_best = 0.0f, g_fv_scale = 1.0f;
static int g_fv_area = 0, g_fv_n = 0;
static float g_fv_list[8][4]; /* w, h, champ brut, champ dessiné */
static float g_fv_last[8][4];
static int g_fv_last_n = 0;
static float g_fv_last_scale = 1.0f;
void glcam_set_fov(float hfov_deg, float zoom)
{
    g_fov_want = hfov_deg;
    g_fov_zoom = zoom > 0.05f ? zoom : 1.0f;
}
float glcam_game_hfov(void) { return g_fov_game; }
static volatile float g_ride_fov = 0.0f;
void glcam_note_ride_fov(float f) { g_ride_fov = f; }
static volatile float g_aspect = 16.0f / 9.0f;
float glcam_aspect(void) { return g_aspect; }
float glcam_ride_fov(void) { return g_ride_fov; }
void glcam_fov_log(void (*log)(const char *fmt, ...))
{
    char b[400];
    int i, k = 0;
    b[0] = 0;
    for (i = 0; i < g_fv_last_n && k < (int)sizeof(b) - 60; ++i)
        k += snprintf(b + k, sizeof(b) - (size_t)k, " [%.0fx%.0f : %.1f -> %.1f]", g_fv_last[i][0], g_fv_last[i][1],
                      g_fv_last[i][2], g_fv_last[i][3]);
    if (log)
        log("champ de vision : jeu %.1f deg, voulu %.1f, zoom %.2f ; passes :%s", g_fov_game, g_fov_want, g_fov_zoom, b);
}

static void APIENTRY hk_Frustum(double l, double r, double b, double t, double n, double f)
{
    /* v1.8.162 : champ de vision réglé (1re personne) et zoom à pied */
    if (n > 1e-6 && r > l && t > b) {
        double half = (r - l) * 0.5 / n, s = 1.0;
        float raw = (float)(2.0 * atan(half) * 57.29577951);
        if (g_vp_w * g_vp_h > g_fv_area) {
            g_fv_area = g_vp_w * g_vp_h;
            g_fv_best = raw;
            if (t > b)
                g_aspect = (float)((r - l) / (t - b));
        }
        if (g_fov_want >= 10.0f && g_fov_want <= 170.0f)
            s = tan(g_fov_want * 0.5 / 57.29577951) / half;
        s *= g_fov_zoom;
        if (g_fv_n < 8) {
            g_fv_list[g_fv_n][0] = (float)g_vp_w;
            g_fv_list[g_fv_n][1] = (float)g_vp_h;
            g_fv_list[g_fv_n][2] = raw;
            g_fv_list[g_fv_n][3] = (float)(2.0 * atan(half * s) * 57.29577951);
            ++g_fv_n;
        }
        g_fv_scale = (float)s;
        if (fabs(s - 1.0) > 1e-4 && s > 0.02 && s < 50.0) {
            l *= s;
            r *= s;
            b *= s;
            t *= s;
        }
    }
    if (g_cap_state == 2) {
        float v[6] = { (float)l, (float)r, (float)b, (float)t, (float)n, (float)f };
        cap_add(EV_FRUSTUM, v, 6);
    }
    g_frustum_pending[0] = (float)l;
    g_frustum_pending[1] = (float)r;
    g_frustum_pending[2] = (float)b;
    g_frustum_pending[3] = (float)t;
    g_frustum_pending[4] = (float)n;
    g_frustum_pending[5] = (float)f;
    scene_end();
    g_pass = 1; /* nouvelle passe en perspective : la vue arrive */
    o_Frustum(l, r, b, t, n, f);
}

static void APIENTRY hk_Ortho(double l, double r, double b, double t, double n, double f)
{
    if (g_cap_state == 2) {
        float v[6] = { (float)l, (float)r, (float)b, (float)t, (float)n, (float)f };
        cap_add(EV_ORTHO, v, 6);
    }
    scene_end();
    g_pass = 0; /* ombres, interface : on ne touche à rien */
    o_Ortho(l, r, b, t, n, f);
}

static void APIENTRY hk_Viewport(int x, int y, int w, int h)
{
    g_vp_w = w;
    g_vp_h = h;
    if (!g_frame_started) {
        g_frame_started = 1;
        frame_event(1); /* premier dessin de l'image */
    }
    if (g_cap_state == 2) {
        float v[4] = { (float)x, (float)y, (float)w, (float)h };
        cap_add(EV_VIEWPORT, v, 4);
    }
    o_Viewport(x, y, w, h);
}

static void APIENTRY hk_DrawElements(unsigned int mode, int count, unsigned int type, const void *idx)
{
    cap_add(EV_DRAW, NULL, 0);
    if (g_scene_ok)
        scene_on_draw();
    o_DrawElements(mode, count, type, idx);
}

/* ---------- v1.4 : bouton du plugin dessiné par-dessus les menus ----------
   Pour les joueurs sans les fichiers de menu modifiés : un petit bouton cliquable en haut à droite.
   Dessiné juste avant l'affichage de l'image, en OpenGL « à l'ancienne », état du jeu sauvegardé
   puis remis. Les fonctions sont prises dans opengl32.dll (pas nos interceptions). */
static volatile GlcamOverlayFn g_overlay_fn = NULL;
void glcam_set_overlay_fn(GlcamOverlayFn fn)
{
    g_overlay_fn = fn;
}

typedef void(APIENTRY *PFN_v)(void);
typedef void(APIENTRY *PFN_u)(unsigned int);
typedef void(APIENTRY *PFN_i)(int);
typedef void(APIENTRY *PFN_ff)(float, float);
typedef void(APIENTRY *PFN_ffff)(float, float, float, float);
typedef void(APIENTRY *PFN_ui_i)(unsigned int, int *);
typedef void(APIENTRY *PFN_uu)(unsigned int, unsigned int);
typedef void(APIENTRY *PFN_iiu_p)(int, unsigned int, const void *);
typedef unsigned int(APIENTRY *PFN_gen)(int);
static struct {
    int ok;
    PFN_u PushAttrib, Disable, Enable, Begin, ListBase;
    PFN_v PopAttrib, End, PushMatrix, PopMatrix;
    PFN_ff Vertex2f, RasterPos2f;
    PFN_ffff Color4f;
    PFN_ui_i GetIntegerv;
    PFN_uu BlendFunc;
    PFN_iiu_p CallLists;
    PFN_gen GenLists;
    PFN_u UseProgram, BindFramebufferDraw;
    void(APIENTRY *BindFramebuffer)(unsigned int, unsigned int);
    /* v1.8.48 : dessin 3D */
    void(APIENTRY *Vertex3f)(float, float, float);
    void(APIENTRY *Color4ub)(unsigned char, unsigned char, unsigned char, unsigned char);
    unsigned char(APIENTRY *IsEnabled)(unsigned int);
    void(APIENTRY *GetBooleanv)(unsigned int, unsigned char *);
    void(APIENTRY *DepthMask)(unsigned char);
    void(APIENTRY *DepthFunc)(unsigned int);
    void(APIENTRY *ShadeModel)(unsigned int);
    void(APIENTRY *ActiveTexture)(unsigned int);
    void(APIENTRY *ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void(APIENTRY *TexCoord2f)(float, float);
    void(APIENTRY *GenTextures)(int, unsigned int *);
    void(APIENTRY *BindTexture)(unsigned int, unsigned int);
    void(APIENTRY *TexImage2D)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
    void(APIENTRY *TexParameteri)(unsigned int, unsigned int, int);
    void(APIENTRY *TexEnvi)(unsigned int, unsigned int, int);
    void(APIENTRY *PixelStorei)(unsigned int, int);
    void(APIENTRY *BindBuffer)(unsigned int, unsigned int);
    /* v1.8.91 : tableaux de sommets (un appel par objet au lieu de 4 par sommet) */
    void(APIENTRY *VertexPointer)(int, unsigned int, int, const void *);
    void(APIENTRY *TexCoordPointer)(int, unsigned int, int, const void *);
    void(APIENTRY *ColorPointer)(int, unsigned int, int, const void *);
    void(APIENTRY *DrawArrays)(unsigned int, int, int);
    void(APIENTRY *EnableClientState)(unsigned int);
    void(APIENTRY *PushClientAttrib)(unsigned int);
    void(APIENTRY *PopClientAttrib)(void);
    /* v1.8.97 : listes d'affichage (un modèle envoyé une fois) et éclairage du matériel */
    void(APIENTRY *NewList)(unsigned int, unsigned int);
    void(APIENTRY *EndList)(void);
    void(APIENTRY *CallList)(unsigned int);
    void(APIENTRY *Normal3f)(float, float, float);
    void(APIENTRY *Lightfv)(unsigned int, unsigned int, const float *);
    void(APIENTRY *LightModelfv)(unsigned int, const float *);
    void(APIENTRY *Materialfv)(unsigned int, unsigned int, const float *);
    void(APIENTRY *ColorMaterial)(unsigned int, unsigned int);
    int ok3d;
} gl;
static unsigned int g_font_base = 0;
static HGLRC g_font_ctx = NULL;

static void *glp(const char *n)
{
    void *p = (void *)GetProcAddress(GetModuleHandleA("opengl32.dll"), n);
    if (!p)
        p = (void *)wglGetProcAddress(n);
    return p;
}

static int gl_load(void)
{
    if (gl.ok)
        return gl.ok > 0;
    gl.PushAttrib = (PFN_u)glp("glPushAttrib");
    gl.PopAttrib = (PFN_v)glp("glPopAttrib");
    gl.Disable = (PFN_u)glp("glDisable");
    gl.Enable = (PFN_u)glp("glEnable");
    gl.Begin = (PFN_u)glp("glBegin");
    gl.End = (PFN_v)glp("glEnd");
    gl.PushMatrix = (PFN_v)glp("glPushMatrix");
    gl.PopMatrix = (PFN_v)glp("glPopMatrix");
    gl.Vertex2f = (PFN_ff)glp("glVertex2f");
    gl.RasterPos2f = (PFN_ff)glp("glRasterPos2f");
    gl.Color4f = (PFN_ffff)glp("glColor4f");
    gl.GetIntegerv = (PFN_ui_i)glp("glGetIntegerv");
    gl.BlendFunc = (PFN_uu)glp("glBlendFunc");
    gl.CallLists = (PFN_iiu_p)glp("glCallLists");
    gl.GenLists = (PFN_gen)glp("glGenLists");
    gl.ListBase = (PFN_u)glp("glListBase");
    gl.UseProgram = (PFN_u)wglGetProcAddress("glUseProgram");
    gl.BindFramebuffer = (void(APIENTRY *)(unsigned int, unsigned int))wglGetProcAddress("glBindFramebuffer");
    gl.ok = (gl.PushAttrib && gl.PopAttrib && gl.Disable && gl.Enable && gl.Begin && gl.End && gl.PushMatrix &&
             gl.PopMatrix && gl.Vertex2f && gl.RasterPos2f && gl.Color4f && gl.GetIntegerv && gl.BlendFunc &&
             gl.CallLists && gl.GenLists && gl.ListBase && o_MatrixMode && o_LoadIdentity && o_Ortho && o_Viewport)
                ? 1
                : -1;
    return gl.ok > 0;
}

static void draw_overlay(HDC dc)
{
    GlcamOverlay ov;
    GlcamOverlayFn fn = g_overlay_fn;
    HWND w;
    RECT rc;
    int prog = 0, fbo = 0, vw, vh;
    HGLRC ctx = wglGetCurrentContext();
    if (!fn || !ctx || !fn(&ov) || !ov.visible || !gl_load())
        return;
    w = WindowFromDC(dc);
    if (!w || !GetClientRect(w, &rc) || rc.right <= 0 || rc.bottom <= 0)
        return;
    vw = rc.right;
    vh = rc.bottom;
    if (g_font_ctx != ctx) {
        /* police Windows en listes OpenGL (une fois par contexte) */
        HFONT f = CreateFontA(-(vh / 40 > 12 ? vh / 40 : 12), 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
        HGDIOBJ old = SelectObject(dc, f);
        g_font_base = gl.GenLists(96);
        if (!g_font_base || !wglUseFontBitmapsA(dc, 32, 96, g_font_base))
            g_font_base = 0;
        SelectObject(dc, old);
        DeleteObject(f);
        g_font_ctx = ctx;
    }
    gl.GetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
    gl.GetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &fbo);
    if (gl.UseProgram)
        gl.UseProgram(0);
    if (fbo && gl.BindFramebuffer)
        gl.BindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, 0);
    gl.PushAttrib(0x000FFFFF /* GL_ALL_ATTRIB_BITS */);
    o_Viewport(0, 0, vw, vh);
    o_MatrixMode(GL_PROJECTION);
    gl.PushMatrix();
    o_LoadIdentity();
    o_Ortho(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);
    o_MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    o_LoadIdentity();
    gl.Disable(0x0B71 /* DEPTH_TEST */);
    gl.Disable(0x0DE1 /* TEXTURE_2D */);
    gl.Disable(0x0B50 /* LIGHTING */);
    gl.Disable(0x0B60 /* FOG */);
    gl.Disable(0x0B44 /* CULL_FACE */);
    gl.Disable(0x0BC0 /* ALPHA_TEST */);
    gl.Disable(0x0C11 /* SCISSOR_TEST */);
    gl.Disable(0x0B90 /* STENCIL_TEST */);
    gl.Enable(0x0BE2 /* BLEND */);
    gl.BlendFunc(0x0302, 0x0303);
    /* fond, bord de couleur (vert actif, rouge coupé) */
    gl.Color4f(0.05f, 0.05f, 0.05f, ov.hover ? 0.9f : 0.75f);
    gl.Begin(7 /* QUADS */);
    gl.Vertex2f(ov.rect[0], ov.rect[1]);
    gl.Vertex2f(ov.rect[2], ov.rect[1]);
    gl.Vertex2f(ov.rect[2], ov.rect[3]);
    gl.Vertex2f(ov.rect[0], ov.rect[3]);
    gl.End();
    if (ov.on)
        gl.Color4f(0.2f, 0.85f, 0.3f, 1.0f);
    else
        gl.Color4f(0.9f, 0.25f, 0.2f, 1.0f);
    gl.Begin(7);
    gl.Vertex2f(ov.rect[0], ov.rect[1]);
    gl.Vertex2f(ov.rect[0] + 0.006f, ov.rect[1]);
    gl.Vertex2f(ov.rect[0] + 0.006f, ov.rect[3]);
    gl.Vertex2f(ov.rect[0], ov.rect[3]);
    gl.End();
    if (g_font_base) {
        gl.Color4f(1.0f, 1.0f, 1.0f, 1.0f);
        gl.RasterPos2f(ov.rect[0] + 0.014f, ov.rect[1] + (ov.rect[3] - ov.rect[1]) * 0.72f);
        gl.ListBase(g_font_base - 32);
        gl.CallLists((int)strlen(ov.text), 0x1401 /* UNSIGNED_BYTE */, ov.text);
    }
    o_MatrixMode(GL_MODELVIEW);
    gl.PopMatrix();
    o_MatrixMode(GL_PROJECTION);
    gl.PopMatrix();
    gl.PopAttrib();
    if (fbo && gl.BindFramebuffer)
        gl.BindFramebuffer(0x8CA9, (unsigned int)fbo);
    if (gl.UseProgram)
        gl.UseProgram((unsigned int)prog);
}

/* v1.8.92 : dessin rapide (tableaux de sommets) seulement avec les objets optimisés */
static volatile int g_scene_fast = 0;
void glcam_set_scene_fast(int on)
{
    g_scene_fast = on;
}

/* v1.8.91 : position de la caméra de la passe en cours et direction du regard (vue du jeu ou la
   nôtre) ; 0 si inconnue */
int glcam_scene_eye(float eye[3], float fwd[3])
{
    const float *m = g_scene_view;
    float t0 = m[12], t1 = m[13], t2 = m[14], l;
    eye[0] = -(m[0] * t0 + m[1] * t1 + m[2] * t2);
    eye[1] = -(m[4] * t0 + m[5] * t1 + m[6] * t2);
    eye[2] = -(m[8] * t0 + m[9] * t1 + m[10] * t2);
    fwd[0] = -m[2];
    fwd[1] = -m[6];
    fwd[2] = -m[10];
    l = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (!(l > 0.5f && l < 2.0f) || !isfinite(eye[0]) || !isfinite(eye[1]) || !isfinite(eye[2]))
        return 0;
    fwd[0] /= l;
    fwd[1] /= l;
    fwd[2] /= l;
    return 1;
}

void glcam_set_scene_fn(GlcamSceneFn fn)
{
    g_scene_fn = fn;
}

void glcam_set_scene_active(int on)
{
    g_scene_want = on;
}

int glcam_scene_recent(void)
{
    DWORD t = g_scene_tick;
    return t && GetTickCount() - t < 500;
}

#define SCENE_TEX 24 /* v1.8.123 : textures des rampes du 2e pack */
static const unsigned char *volatile g_tex_rgb[SCENE_TEX];
static volatile int g_tex_w[SCENE_TEX], g_tex_h[SCENE_TEX];
static HGLRC g_tex_ctx[SCENE_TEX];
static unsigned int g_tex_id[SCENE_TEX];

void glcam_set_scene_texture(int slot, const unsigned char *rgb, int w, int h)
{
    if (slot < 0 || slot >= SCENE_TEX)
        return;
    g_tex_w[slot] = w;
    g_tex_h[slot] = h;
    g_tex_rgb[slot] = rgb;
}

static int gl_load3d(void)
{
    if (gl.ok3d)
        return gl.ok3d > 0;
    if (!gl_load()) {
        gl.ok3d = -1;
        return 0;
    }
    gl.Vertex3f = (void(APIENTRY *)(float, float, float))glp("glVertex3f");
    gl.Color4ub = (void(APIENTRY *)(unsigned char, unsigned char, unsigned char, unsigned char))glp("glColor4ub");
    gl.IsEnabled = (unsigned char(APIENTRY *)(unsigned int))glp("glIsEnabled");
    gl.GetBooleanv = (void(APIENTRY *)(unsigned int, unsigned char *))glp("glGetBooleanv");
    gl.DepthMask = (void(APIENTRY *)(unsigned char))glp("glDepthMask");
    gl.DepthFunc = (void(APIENTRY *)(unsigned int))glp("glDepthFunc");
    gl.ShadeModel = (void(APIENTRY *)(unsigned int))glp("glShadeModel");
    gl.ActiveTexture = (void(APIENTRY *)(unsigned int))wglGetProcAddress("glActiveTexture");
    gl.ColorMask = (void(APIENTRY *)(unsigned char, unsigned char, unsigned char, unsigned char))glp("glColorMask");
    gl.TexCoord2f = (void(APIENTRY *)(float, float))glp("glTexCoord2f");
    gl.GenTextures = (void(APIENTRY *)(int, unsigned int *))glp("glGenTextures");
    gl.BindTexture = (void(APIENTRY *)(unsigned int, unsigned int))glp("glBindTexture");
    gl.TexImage2D = (void(APIENTRY *)(unsigned int, int, int, int, int, int, unsigned int, unsigned int,
                                      const void *))glp("glTexImage2D");
    gl.TexParameteri = (void(APIENTRY *)(unsigned int, unsigned int, int))glp("glTexParameteri");
    gl.TexEnvi = (void(APIENTRY *)(unsigned int, unsigned int, int))glp("glTexEnvi");
    gl.PixelStorei = (void(APIENTRY *)(unsigned int, int))glp("glPixelStorei");
    gl.BindBuffer = (void(APIENTRY *)(unsigned int, unsigned int))wglGetProcAddress("glBindBuffer");
    gl.VertexPointer = (void(APIENTRY *)(int, unsigned int, int, const void *))glp("glVertexPointer");
    gl.TexCoordPointer = (void(APIENTRY *)(int, unsigned int, int, const void *))glp("glTexCoordPointer");
    gl.ColorPointer = (void(APIENTRY *)(int, unsigned int, int, const void *))glp("glColorPointer");
    gl.DrawArrays = (void(APIENTRY *)(unsigned int, int, int))glp("glDrawArrays");
    gl.EnableClientState = (void(APIENTRY *)(unsigned int))glp("glEnableClientState");
    gl.PushClientAttrib = (void(APIENTRY *)(unsigned int))glp("glPushClientAttrib");
    gl.PopClientAttrib = (void(APIENTRY *)(void))glp("glPopClientAttrib");
    gl.NewList = (void(APIENTRY *)(unsigned int, unsigned int))glp("glNewList");
    gl.EndList = (void(APIENTRY *)(void))glp("glEndList");
    gl.CallList = (void(APIENTRY *)(unsigned int))glp("glCallList");
    gl.Normal3f = (void(APIENTRY *)(float, float, float))glp("glNormal3f");
    gl.Lightfv = (void(APIENTRY *)(unsigned int, unsigned int, const float *))glp("glLightfv");
    gl.LightModelfv = (void(APIENTRY *)(unsigned int, const float *))glp("glLightModelfv");
    gl.Materialfv = (void(APIENTRY *)(unsigned int, unsigned int, const float *))glp("glMaterialfv");
    gl.ColorMaterial = (void(APIENTRY *)(unsigned int, unsigned int))glp("glColorMaterial");
    gl.ok3d = (gl.Vertex3f && gl.Color4ub && gl.IsEnabled && gl.GetBooleanv && gl.DepthMask && gl.DepthFunc &&
               gl.ShadeModel && gl.ColorMask && gl.TexCoord2f && gl.GenTextures && gl.BindTexture && gl.TexImage2D &&
               gl.TexParameteri && gl.TexEnvi && gl.PixelStorei && o_LoadMatrixf && o_MatrixMode)
                  ? 1
                  : -1;
    return gl.ok3d > 0;
}

static int draw_fbo(void)
{
    int f = 0;
    gl.GetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &f);
    return f;
}

/* début d'une passe en perspective (la vue vient d'être chargée) */
static void scene_begin(void)
{
    PassInfo *pi = NULL;
    g_scene_ok = 1;
    g_scene_done = 0;
    g_pass_fbo = -1;
    if (g_pi_n < PI_MAX) {
        pi = &g_pi[g_pi_n++];
        memset(pi, 0, sizeof(*pi));
        pi->fbo = -1;
        memcpy(pi->fr, g_frustum_pending, sizeof(pi->fr));
    }
    if (!g_scene_want || !g_scene_fn)
        return;
    if (!gl_load3d()) {
        g_st.no_gl++;
        if (pi)
            pi->how = 4;
        return;
    }
    g_st.passes++;
    g_pass_fbo = draw_fbo();
    if (pi) {
        pi->fbo = g_pass_fbo;
        gl.GetIntegerv(0x0BA2 /* VIEWPORT */, pi->vp);
    }
}

#define SCENE_MAX_VERTS 700000
static float g_sv_p[SCENE_MAX_VERTS][3], g_sv_uv[SCENE_MAX_VERTS][2];
static unsigned long g_sv_c[SCENE_MAX_VERTS];
static unsigned char g_sv_t[SCENE_MAX_VERTS];

/* texture envoyée une fois par contexte ; rend son numéro OpenGL (0 : aucune) */
static unsigned int scene_tex(int t, HGLRC ctx)
{
    int old = 0;
    if (t < 0 || t >= SCENE_TEX || !g_tex_rgb[t] || g_tex_w[t] <= 0)
        return 0;
    if (g_tex_ctx[t] == ctx && g_tex_id[t])
        return g_tex_id[t];
    gl.GetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &old);
    gl.GenTextures(1, &g_tex_id[t]);
    gl.BindTexture(0x0DE1, g_tex_id[t]);
    gl.PixelStorei(0x0CF5 /* UNPACK_ALIGNMENT */, 1);
    gl.PixelStorei(0x0CF2 /* UNPACK_ROW_LENGTH */, 0);
    gl.TexParameteri(0x0DE1, 0x8191 /* GENERATE_MIPMAP */, 1);
    gl.TexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2703 /* LINEAR_MIPMAP_LINEAR */);
    gl.TexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2601 /* LINEAR */);
    gl.TexParameteri(0x0DE1, 0x2802 /* WRAP_S */, 0x2901 /* REPEAT */);
    gl.TexParameteri(0x0DE1, 0x2803 /* WRAP_T */, 0x2901);
    gl.TexImage2D(0x0DE1, 0, 0x1907 /* RGB */, g_tex_w[t], g_tex_h[t], 0, 0x1907, 0x1401 /* UNSIGNED_BYTE */,
                  (const void *)g_tex_rgb[t]);
    gl.BindTexture(0x0DE1, (unsigned int)old);
    g_tex_ctx[t] = ctx;
    return g_tex_id[t];
}

/* ---------- v1.8.97 : objets dessinés par modèle (listes d'affichage) ---------- */
#define INST_MAX 2048
static GlcamLogFn g_glog = NULL;
static GlcamInstFn g_inst_fn = NULL;
static ObjInst g_inst[INST_MAX];
static struct {
    const float *v;
    int stride, n;
} g_model[OBJM_COUNT];
static HGLRC g_list_ctx = NULL;
static unsigned int g_list[OBJM_COUNT];

void glcam_set_inst_fn(GlcamInstFn fn)
{
    g_inst_fn = fn;
}

void glcam_set_model(int id, const float *v, int stride, int nverts)
{
    if (id < 0 || id >= OBJM_COUNT)
        return;
    g_model[id].v = v;
    g_model[id].stride = stride;
    g_model[id].n = nverts;
}

static int lists_ok(void)
{
    return gl.GenLists && gl.NewList && gl.EndList && gl.CallList && gl.Normal3f && gl.Lightfv && gl.LightModelfv &&
           gl.Materialfv && gl.ColorMaterial && gl.Begin && gl.End && gl.TexCoord2f && gl.Vertex3f && o_MultMatrixf;
}

/* modèles compilés dans ce contexte OpenGL (une fois) */
static int lists_ready(HGLRC ctx)
{
    int i, k;
    if (g_list_ctx == ctx && g_list[0])
        return 1;
    for (i = 0; i < OBJM_COUNT; ++i) {
        const float *v = g_model[i].v;
        g_list[i] = 0;
        if (!v || g_model[i].n <= 0)
            continue;
        g_list[i] = gl.GenLists(1);
        if (!g_list[i])
            return 0;
        gl.NewList(g_list[i], 0x1300 /* COMPILE */);
        gl.Begin(4 /* TRIANGLES */);
        for (k = 0; k < g_model[i].n; ++k) {
            const float *q = v + (size_t)k * (size_t)g_model[i].stride;
            gl.Normal3f(q[3], q[4], q[5]);
            gl.TexCoord2f(q[6], q[7]);
            gl.Vertex3f(q[0], q[1], q[2]);
        }
        gl.End();
        gl.EndList();
    }
    g_list_ctx = ctx;
    if (g_glog)
        g_glog("objets 3D : modeles envoyes a la carte graphique (listes d'affichage)");
    return g_list[0] != 0;
}

/* objets : une matrice et un appel de liste chacun ; éclairage fait par la carte, même formule que
   le dessin d'avant : 0,50 + 0,55 x max(normale . soleil, 0) */
static int inst_draw(HGLRC ctx)
{
    static const float amb[4] = { 0.50f, 0.50f, 0.50f, 1.0f }, dif[4] = { 0.55f, 0.55f, 0.55f, 1.0f };
    static const float zero[4] = { 0.0f, 0.0f, 0.0f, 1.0f }, sun[4] = { 0.36f, 0.86f, 0.36f, 0.0f };
    int n, i, cur = -1;
    if (!g_inst_fn || !lists_ok())
        return 0;
    n = g_inst_fn(g_inst, INST_MAX);
    if (n <= 0)
        return 1;
    if (!lists_ready(ctx))
        return 0;
    gl.Enable(0x0B50 /* LIGHTING */);
    for (i = 1; i < 8; ++i)
        gl.Disable(0x4000 + i); /* LIGHT1..7 */
    gl.Enable(0x4000 /* LIGHT0 */);
    gl.Enable(0x0BA1 /* NORMALIZE */);
    gl.LightModelfv(0x0B53 /* LIGHT_MODEL_AMBIENT */, zero);
    {
        /* v1.8.180 : faces vues de dos éclairées de leur côté (toile des chaises, pièces fines) */
        static const float two[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        gl.LightModelfv(0x0B52 /* LIGHT_MODEL_TWO_SIDE */, two);
    }
    gl.Lightfv(0x4000, 0x1200 /* AMBIENT */, amb);
    gl.Lightfv(0x4000, 0x1201 /* DIFFUSE */, dif);
    gl.Lightfv(0x4000, 0x1202 /* SPECULAR */, zero);
    gl.Lightfv(0x4000, 0x1203 /* POSITION */, sun); /* (matrice de vue chargée : direction dans le monde) */
    gl.Materialfv(0x0408 /* FRONT_AND_BACK */, 0x1600 /* EMISSION */, zero);
    gl.Materialfv(0x0408, 0x1202 /* SPECULAR */, zero);
    gl.ColorMaterial(0x0408, 0x1602 /* AMBIENT_AND_DIFFUSE */);
    gl.Enable(0x0B57 /* COLOR_MATERIAL */);
    gl.Color4ub(255, 255, 255, 255);
    for (i = 0; i < n; ++i) {
        const ObjInst *o = &g_inst[i];
        if (o->model < 0 || o->model >= OBJM_COUNT || !g_list[o->model])
            continue;
        if (o->tex != cur) {
            unsigned int id = scene_tex(o->tex, ctx);
            if (id) {
                gl.Enable(0x0DE1 /* TEXTURE_2D */);
                gl.BindTexture(0x0DE1, id);
            } else {
                gl.Disable(0x0DE1);
            }
            cur = o->tex;
        }
        gl.PushMatrix();
        o_MultMatrixf(o->m);
        gl.CallList(g_list[o->model]);
        gl.PopMatrix();
    }
    return 1;
}

static void scene_draw_now(void);
/* v1.8.98 : temps passé à dessiner nos objets (processeur, envoi à la carte compris) */
static void scene_draw(void)
{
    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    scene_draw_now();
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    if (f.QuadPart > 0)
        g_draw_us += (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart;
    g_draw_n++;
}

/* nos triangles, avec la profondeur du jeu : ses objets les cachent, ils cachent ce qui est derrière */
static void scene_draw_now(void)
{
    GlcamSceneFn fn = g_scene_fn;
    int prog = 0, n, i, t, tex_unit = 0, abuf = 0;
    unsigned int used = 0;
    HGLRC ctx = wglGetCurrentContext();
    g_scene_done = 1;
    if (!fn && !g_inst_fn)
        return;
    if (g_inst_fn && lists_ok()) {
        /* v1.8.97 : objets par modèle (plus d'envoi des sommets un par un) */
        n = 0;
    } else {
        if (!fn)
            return;
        n = fn(g_sv_p, g_sv_uv, g_sv_c, g_sv_t, SCENE_MAX_VERTS);
        if (n <= 0)
            return;
    }
    for (i = 0; i < n; ++i)
        used |= 1u << (g_sv_t[i] < SCENE_TEX ? g_sv_t[i] : 0);
    gl.GetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
    gl.GetIntegerv(0x84E0 /* ACTIVE_TEXTURE */, &tex_unit);
    gl.GetIntegerv(0x8894 /* ARRAY_BUFFER_BINDING */, &abuf);
    if (gl.UseProgram)
        gl.UseProgram(0);
    if (gl.ActiveTexture)
        gl.ActiveTexture(0x84C0 /* TEXTURE0 */);
    gl.PushAttrib(0x000FFFFF /* ALL_ATTRIB_BITS */);
    if (gl.BindBuffer)
        gl.BindBuffer(0x8892 /* ARRAY_BUFFER */, 0);
    gl.TexEnvi(0x2300 /* TEXTURE_ENV */, 0x2200 /* ENV_MODE */, 0x2100 /* MODULATE */);
    gl.Disable(0x0B50 /* LIGHTING */);
    gl.Disable(0x0B60 /* FOG */);
    gl.Disable(0x0B44 /* CULL_FACE */);
    gl.Disable(0x0BC0 /* ALPHA_TEST */);
    gl.Disable(0x0BE2 /* BLEND */);
    gl.Disable(0x0B90 /* STENCIL_TEST */);
    gl.Disable(0x0B57 /* COLOR_MATERIAL */);
    gl.Disable(0x0C11 /* SCISSOR_TEST */);
    gl.Enable(0x0B71 /* DEPTH_TEST */);
    gl.DepthMask(1);
    gl.ColorMask(1, 1, 1, 1);
    gl.DepthFunc(0x0203 /* LEQUAL */);
    gl.ShadeModel(0x1D01 /* SMOOTH */);
    o_MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    o_LoadMatrixf(g_scene_view);
    if (g_inst_fn && lists_ok()) {
        if (!inst_draw(ctx))
            g_inst_fn = NULL; /* (listes refusées : dessin d'avant à l'image suivante) */
    } else if (g_scene_fast && gl.VertexPointer && gl.TexCoordPointer && gl.ColorPointer && gl.DrawArrays &&
        gl.EnableClientState && gl.PushClientAttrib && gl.PopClientAttrib) {
        /* v1.8.91 : tableaux de sommets : un appel par suite de sommets de même texture (un objet) */
        int cur = -1;
        (void)used;
        for (i = 0; i < n; ++i)
            g_sv_c[i] |= 0xFF000000UL; /* opaque (comme avant : Color4ub(..., 255)) */
        gl.PushClientAttrib(0x00000002 /* CLIENT_VERTEX_ARRAY_BIT */);
        gl.EnableClientState(0x8074 /* VERTEX_ARRAY */);
        gl.EnableClientState(0x8078 /* TEXTURE_COORD_ARRAY */);
        gl.EnableClientState(0x8076 /* COLOR_ARRAY */);
        gl.VertexPointer(3, 0x1406 /* FLOAT */, 0, g_sv_p);
        gl.TexCoordPointer(2, 0x1406, 0, g_sv_uv);
        gl.ColorPointer(4, 0x1401 /* UNSIGNED_BYTE */, sizeof(unsigned long), g_sv_c);
        for (i = 0; i < n;) {
            int j = i, tt = g_sv_t[i] < SCENE_TEX ? g_sv_t[i] : 0;
            while (j < n && (g_sv_t[j] < SCENE_TEX ? g_sv_t[j] : 0) == tt)
                ++j;
            if (tt != cur) {
                unsigned int id = scene_tex(tt, ctx);
                if (id) {
                    gl.Enable(0x0DE1 /* TEXTURE_2D */);
                    gl.BindTexture(0x0DE1, id);
                } else {
                    gl.Disable(0x0DE1);
                }
                cur = tt;
            }
            gl.DrawArrays(4 /* TRIANGLES */, i, j - i);
            i = j;
        }
        gl.PopClientAttrib();
    } else
    for (t = 0; t < SCENE_TEX; ++t) {
        unsigned int id;
        if (!(used & (1u << t)))
            continue;
        id = scene_tex(t, ctx);
        if (id) {
            gl.Enable(0x0DE1 /* TEXTURE_2D */);
            gl.BindTexture(0x0DE1, id);
        } else {
            gl.Disable(0x0DE1);
        }
        gl.Begin(4 /* TRIANGLES */);
        for (i = 0; i < n; ++i) {
            unsigned long c;
            if ((g_sv_t[i] < SCENE_TEX ? g_sv_t[i] : 0) != t)
                continue;
            c = g_sv_c[i];
            gl.Color4ub((unsigned char)(c & 0xFF), (unsigned char)((c >> 8) & 0xFF),
                        (unsigned char)((c >> 16) & 0xFF), 255);
            gl.TexCoord2f(g_sv_uv[i][0], g_sv_uv[i][1]);
            gl.Vertex3f(g_sv_p[i][0], g_sv_p[i][1], g_sv_p[i][2]);
        }
        gl.End();
    }
    gl.PopMatrix();
    o_MatrixMode(g_mode);
    gl.PopAttrib();
    if (gl.BindBuffer)
        gl.BindBuffer(0x8892, (unsigned int)abuf);
    if (gl.ActiveTexture)
        gl.ActiveTexture((unsigned int)tex_unit);
    if (gl.UseProgram)
        gl.UseProgram((unsigned int)prog);
    g_scene_tick = GetTickCount();
}

/* un dessin du jeu dans la passe : au premier qui teste la profondeur, on dessine avant lui
   (v1.8.48 demandait aussi l'écriture de la profondeur et de la couleur : jamais vu en jeu) */
static void scene_on_draw(void)
{
    unsigned char dw = 0, cw[4] = { 0, 0, 0, 0 };
    int d;
    if (g_scene_done || g_pass_fbo < 0)
        return;
    g_st.draws++;
    d = gl.IsEnabled(0x0B71 /* DEPTH_TEST */);
    gl.GetBooleanv(0x0B72 /* DEPTH_WRITEMASK */, &dw);
    gl.GetBooleanv(0x0C23 /* COLOR_WRITEMASK */, cw);
    g_st.d_on += d != 0;
    g_st.dw_on += dw != 0;
    g_st.cw_on += cw[0] && cw[1] && cw[2];
    if (!d)
        return;
    g_st.done_draw++;
    if (g_pi_n > 0)
        g_pi[g_pi_n - 1].how = 1;
    fbo_mark(g_pass_fbo);
    scene_draw();
}

/* fin de la passe sans dessin : si sa cible (FBO) est toujours en place, on dessine maintenant */
static void scene_end(void)
{
    if (!g_scene_ok)
        return;
    g_scene_ok = 0;
    if (g_scene_done || g_pass_fbo < 0 || !g_scene_want)
        return;
    if (draw_fbo() != g_pass_fbo) {
        /* v1.8.198 : par temps de pluie / nuages, le jeu change de cible avant la fin de la passe de l'image
           vue (cible 55) : on remet sa cible et sa taille le temps de dessiner nos objets, puis on rend tout.
           Seulement une grande passe dont la cible n'a pas encore nos objets (pas les reflets 256x128). */
        PassInfo *pi = g_pi_n > 0 ? &g_pi[g_pi_n - 1] : NULL;
        int cur = draw_fbo(), vp[4];
        g_st.end_fbo_diff++;
        if (pi)
            pi->how = 3;
        if (pi && gl.BindFramebuffer && !fbo_drawn(g_pass_fbo) && pi->vp[2] >= 1024 && pi->vp[3] >= 512) {
            gl.GetIntegerv(0x0BA2 /* VIEWPORT */, vp);
            gl.BindFramebuffer(0x8CA9 /* DRAW_FRAMEBUFFER */, (unsigned int)g_pass_fbo);
            o_Viewport(pi->vp[0], pi->vp[1], pi->vp[2], pi->vp[3]);
            pi->how = 5;
            g_st.done_rebind++;
            fbo_mark(g_pass_fbo);
            scene_draw();
            gl.BindFramebuffer(0x8CA9, (unsigned int)cur);
            o_Viewport(vp[0], vp[1], vp[2], vp[3]);
        }
        return;
    }
    g_st.done_end++;
    if (g_pi_n > 0)
        g_pi[g_pi_n - 1].how = 2;
    fbo_mark(g_pass_fbo);
    scene_draw();
}

int glcam_scene_stats(char *buf, int n)
{
    int r = snprintf(buf, (size_t)n,
                     "passes %d, dessins du jeu %d (profondeur testee %d, ecrite %d, couleur %d), dessine au debut %d, "
                     "en fin de passe %d, cible changee %d (dont remise %d), opengl absent %d",
                     g_st.passes, g_st.draws, g_st.d_on, g_st.dw_on, g_st.cw_on, g_st.done_draw, g_st.done_end,
                     g_st.end_fbo_diff, g_st.done_rebind, g_st.no_gl);
    memset(&g_st, 0, sizeof(g_st));
    return r;
}

/* v1.8.98 : les passes de la dernière image (taille, cible, perspective, dessin ou pas) + temps */
int glcam_pass_info(char *buf, int n)
{
    int i, r, k = g_pi_last_n;
    static const char *how[] = { "rien", "avant un dessin", "fin de passe", "cible changee", "sans opengl",
                                 "cible remise" };
    r = snprintf(buf, (size_t)n, "%d passe(s) par image (%d images) ; nos objets : %d dessin(s), %.0f us en tout",
                 k, g_pi_frames, g_draw_n, g_draw_us);
    for (i = 0; i < k && r < n; ++i) {
        const PassInfo *p = &g_pi_last[i];
        float w = p->fr[1] - p->fr[0], nr = p->fr[4];
        r += snprintf(buf + r, (size_t)(n - r), " | #%d cible %d, %dx%d en (%d %d), proche %.3f loin %.0f, champ %.3f%s, %s",
                      i, p->fbo, p->vp[2], p->vp[3], p->vp[0], p->vp[1], nr, p->fr[5], nr > 0 ? w / nr : 0.0f,
                      p->view ? ", vue remplacee" : "", how[(p->how >= 0 && p->how < 6) ? p->how : 0]);
    }
    g_pi_frames = 0;
    g_draw_us = 0.0;
    g_draw_n = 0;
    return r;
}

static BOOL WINAPI hk_SwapBuffers(HDC dc)
{
    BOOL r;
    scene_end();
    draw_overlay(dc);
    r = o_SwapBuffers(dc);
    /* v1.8.164 : champ de vision du jeu : celui de la plus grande passe en perspective de l'image */
    if (g_fv_area > 0)
        g_fov_game = g_fv_best;
    memcpy(g_fv_last, g_fv_list, sizeof(g_fv_last));
    g_fv_last_n = g_fv_n;
    g_fv_last_scale = g_fv_scale;
    g_fv_area = 0;
    g_fv_n = 0;
    if (g_pi_n > 0) {
        memcpy(g_pi_last, g_pi, sizeof(g_pi));
        g_pi_last_n = g_pi_n;
    }
    g_pi_n = 0;
    g_drawn_n = 0;
    g_pi_frames++;
    g_render_tid = GetCurrentThreadId();
    g_pass = 0;
    g_frame_started = 0;
    frame_event(0); /* fin de l'image */
    if (g_cap_state == 2) {
        g_cap_state = 3;
    } else if (g_cap_state == 1) {
        g_cap_n = 0;
        memset(g_cap_total, 0, sizeof(g_cap_total));
        g_cap_state = 2;
    }
    return r;
}

/* ---------- table d'imports de mxbikes.exe ---------- */

typedef struct {
    const char *dll, *name;
    void *hook;
    void **orig;
    void **slot;
} ImportHook;

static ImportHook g_hooks[] = {
    { "opengl32.dll", "glMatrixMode", (void *)hk_MatrixMode, (void **)&o_MatrixMode, NULL },
    { "opengl32.dll", "glLoadIdentity", (void *)hk_LoadIdentity, (void **)&o_LoadIdentity, NULL },
    { "opengl32.dll", "glLoadMatrixf", (void *)hk_LoadMatrixf, (void **)&o_LoadMatrixf, NULL },
    { "opengl32.dll", "glMultMatrixf", (void *)hk_MultMatrixf, (void **)&o_MultMatrixf, NULL },
    { "opengl32.dll", "glFrustum", (void *)hk_Frustum, (void **)&o_Frustum, NULL },
    { "opengl32.dll", "glOrtho", (void *)hk_Ortho, (void **)&o_Ortho, NULL },
    { "opengl32.dll", "glViewport", (void *)hk_Viewport, (void **)&o_Viewport, NULL },
    { "opengl32.dll", "glDrawElements", (void *)hk_DrawElements, (void **)&o_DrawElements, NULL },
    { "gdi32.dll", "SwapBuffers", (void *)hk_SwapBuffers, (void **)&o_SwapBuffers, NULL },
};
#define NUM_HOOKS (int)(sizeof(g_hooks) / sizeof(g_hooks[0]))

static int write_slot(void **slot, void *value)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return 0;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void *), old, &old);
    return 1;
}

int glcam_install(GlcamLogFn log)
{
    g_glog = log;
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int done = 0, i;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        const char *dll = (const char *)(base + imp->Name);
        IMAGE_THUNK_DATA64 *names, *slots;
        if (!imp->OriginalFirstThunk || !imp->FirstThunk)
            continue;
        names = (IMAGE_THUNK_DATA64 *)(base + imp->OriginalFirstThunk);
        slots = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            const char *fn;
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            for (i = 0; i < NUM_HOOKS; ++i) {
                ImportHook *h = &g_hooks[i];
                void **slot = (void **)&slots->u1.Function;
                if (h->slot || _stricmp(dll, h->dll) != 0 || strcmp(fn, h->name) != 0)
                    continue;
                *h->orig = *slot; /* fonction d'origine, ou celle d'un autre mod */
                if (*h->orig && write_slot(slot, h->hook)) {
                    h->slot = slot;
                    ++done;
                }
            }
        }
    }
    if (log) {
        for (i = 0; i < NUM_HOOKS; ++i)
            if (!g_hooks[i].slot)
                log("camera : %s introuvable dans les imports du jeu", g_hooks[i].name);
        log("camera : %d/%d fonctions OpenGL interceptees", done, NUM_HOOKS);
    }
    return done;
}

void glcam_uninstall(void)
{
    int i;
    for (i = 0; i < NUM_HOOKS; ++i) {
        ImportHook *h = &g_hooks[i];
        /* on ne remet le pointeur que si personne ne nous a intercepté entre-temps */
        if (h->slot && *h->slot == h->hook)
            write_slot(h->slot, *h->orig);
        h->slot = NULL;
    }
}

void glcam_request_capture(const char *why)
{
    if (g_cap_state != 0 || !g_hooks[NUM_HOOKS - 1].slot)
        return;
    strncpy(g_cap_why, why ? why : "", sizeof(g_cap_why) - 1);
    g_cap_why[sizeof(g_cap_why) - 1] = '\0';
    g_cap_state = 1;
}

static const char *ev_name(int t)
{
    static const char *n[] = { "mode", "identite", "load", "mult", "frustum", "ortho", "viewport", "dessins" };
    return t >= 0 && t < 8 ? n[t] : "?";
}

void glcam_flush(GlcamLogFn log)
{
    int i, n;
    if (g_cap_state != 3 || !log)
        return;
    n = g_cap_n;
    log("camera : image enregistree (%s) : %d appels, load %d, mult %d, identite %d, mode %d, "
        "frustum %d, ortho %d, dessins %d", g_cap_why, n, g_cap_total[EV_LOAD], g_cap_total[EV_MULT],
        g_cap_total[EV_IDENT], g_cap_total[EV_MODE], g_cap_total[EV_FRUSTUM], g_cap_total[EV_ORTHO],
        g_cap_total[EV_DRAW]);
    for (i = 0; i < n; ++i) {
        const CapEvent *e = &g_cap[i];
        const float *m = e->m;
        const char *md = e->mode == GL_MODELVIEW ? "MV" : e->mode == GL_PROJECTION ? "PR" : "??";
        switch (e->type) {
        case EV_LOAD:
        case EV_MULT:
            /* OpenGL : colonnes. On écrit les 3 axes puis la translation (m[12..14]). */
            log("  #%d %s %s  x(%.3f %.3f %.3f) y(%.3f %.3f %.3f) z(%.3f %.3f %.3f) t(%.3f %.3f %.3f) w(%.3f %.3f %.3f %.3f)",
                i, ev_name(e->type), md, m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12],
                m[13], m[14], m[3], m[7], m[11], m[15]);
            break;
        case EV_FRUSTUM:
        case EV_ORTHO:
            log("  #%d %s %s  %.4f %.4f %.4f %.4f %.4f %.1f", i, ev_name(e->type), md, m[0], m[1], m[2],
                m[3], m[4], m[5]);
            break;
        case EV_VIEWPORT:
            log("  #%d viewport %.0f %.0f %.0f %.0f", i, m[0], m[1], m[2], m[3]);
            break;
        case EV_MODE:
            log("  #%d mode %s", i, (unsigned)m[0] == GL_MODELVIEW ? "MODELVIEW" : (unsigned)m[0] == GL_PROJECTION ? "PROJECTION" : "autre");
            break;
        case EV_DRAW:
            log("  #%d %d dessin(s)", i, e->count);
            break;
        default:
            log("  #%d %s %s", i, ev_name(e->type), md);
        }
    }
    if (n >= CAP_MAX)
        log("  (liste coupee a %d appels)", CAP_MAX);
    g_cap_state = 0;
}

#else

int glcam_frustum(float out[6]) { (void)out; return 0; }

int glcam_install(GlcamLogFn log) { (void)log; return 0; }
int glcam_game_view(float out[16], const void **ptr) { (void)out; (void)ptr; return 0; }
void glcam_set_view_fn(GlcamViewFn fn) { (void)fn; }
void glcam_set_fov(float hfov_deg, float zoom) { (void)hfov_deg; (void)zoom; }
float glcam_game_hfov(void) { return 0.0f; }
void glcam_note_ride_fov(float f) { (void)f; }
float glcam_aspect(void) { return 16.0f / 9.0f; }
float glcam_ride_fov(void) { return 0.0f; }
void glcam_fov_log(void (*log)(const char *fmt, ...)) { (void)log; }
void glcam_set_frame_fn(GlcamFrameFn fn) { (void)fn; }
void glcam_set_overlay_fn(GlcamOverlayFn fn) { (void)fn; }
void glcam_set_scene_fn(GlcamSceneFn fn) { (void)fn; }
int glcam_scene_eye(float eye[3], float fwd[3]) { (void)eye; (void)fwd; return 0; }
void glcam_set_scene_fast(int on) { (void)on; }
void glcam_set_model(int id, const float *v, int stride, int nverts) { (void)id; (void)v; (void)stride; (void)nverts; }
void glcam_set_inst_fn(GlcamInstFn fn) { (void)fn; }
int glcam_scene_recent(void) { return 0; }
void glcam_set_scene_active(int on) { (void)on; }
void glcam_set_scene_texture(int slot, const unsigned char *rgb, int w, int h) { (void)slot; (void)rgb; (void)w; (void)h; }
int glcam_scene_stats(char *buf, int n) { if (n > 0) buf[0] = 0; return 0; }
int glcam_pass_info(char *buf, int n) { if (n > 0) buf[0] = 0; return 0; }
unsigned long glcam_render_thread(void) { return 0; }
void glcam_uninstall(void) {}
void glcam_request_capture(const char *why) { (void)why; }
void glcam_flush(GlcamLogFn log) { (void)log; }

#endif

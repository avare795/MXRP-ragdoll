/*
 * padicon.c - v1.8.109 : symboles PlayStation (croix, rond, carré, triangle) dessinés avec des
 * quadrilatères, et texte mêlé de symboles.
 *
 * La police du jeu n'a pas ces symboles. Le jeu dessine pour le plugin des quadrilatères de couleur
 * (4 coins libres) : un triangle = quadrilatère avec deux coins confondus ; contour = bandes fines ;
 * rond = anneau de 16 morceaux. Pour placer un symbole au milieu d'une phrase, il faut la largeur du
 * texte : elle est lue dans la police du joueur (plugins\MXRP_Ragdoll.fnt, copiée de son misc.pkz) :
 * un enregistrement de 40 octets par caractère à partir de 0x604 (avance = avant + largeur + après,
 * en pixels pour une hauteur de ligne de 38).
 */
#include "padicon.h"
#include "padstyle.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static float g_adv[256]; /* avance de chaque caractère, en hauteurs de ligne */
static int g_font_ok = 0;

int padicon_load_font(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[4];
    int c, ok = 0;
    if (!f)
        return 0;
    if (fread(hdr, 1, 4, f) == 4 && !memcmp(hdr, "FNT", 3)) {
        for (c = 0; c < 256; ++c)
            g_adv[c] = 0.5f;
        for (c = 32; c < 256; ++c) {
            int r[10];
            if (fseek(f, 0x604 + 40 * (c - 32), SEEK_SET) || fread(r, 4, 10, f) != 10)
                break;
            if (r[2] != 1 || r[4] < 0 || r[4] > 200) /* (pas un caractère dessiné) */
                continue;
            g_adv[c] = (float)(r[3] + r[4] + r[5]) / 38.0f;
            if (c == 'A')
                ok = g_adv[c] > 0.2f && g_adv[c] < 1.5f;
        }
    }
    fclose(f);
    g_font_ok = ok;
    return ok;
}

float padicon_text_width(const char *t, float size, float aspect)
{
    float w = 0.0f;
    if (!t)
        return 0.0f;
    for (; *t; ++t)
        w += g_font_ok ? g_adv[(unsigned char)*t] : 0.5f;
    return w * size / (aspect > 0.1f ? aspect : 1.0f);
}

/* ---- formes ---- */

static void wind(SPluginQuad_t *q)
{
    /* même sens que les rectangles du plugin (haut gauche, bas gauche, bas droite) : aire négative */
    float a = 0.0f, t[2];
    int k;
    for (k = 0; k < 4; ++k) {
        int n = (k + 1) & 3;
        a += q->m_aafPos[k][0] * q->m_aafPos[n][1] - q->m_aafPos[n][0] * q->m_aafPos[k][1];
    }
    if (a > 0.0f) {
        memcpy(t, q->m_aafPos[1], sizeof(t));
        memcpy(q->m_aafPos[1], q->m_aafPos[3], sizeof(t));
        memcpy(q->m_aafPos[3], t, sizeof(t));
    }
}

/* bande de A à B ; coordonnées « carrées » (x déjà multiplié par l'aspect), rendues à l'écran */
static void band(SPluginQuad_t *q, float ax, float ay, float bx, float by, float th, float asp, unsigned long c)
{
    float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy), nx, ny;
    if (l < 1e-6f)
        l = 1e-6f;
    nx = -dy / l * th * 0.5f;
    ny = dx / l * th * 0.5f;
    q->m_aafPos[0][0] = (ax + nx) / asp; q->m_aafPos[0][1] = ay + ny;
    q->m_aafPos[1][0] = (ax - nx) / asp; q->m_aafPos[1][1] = ay - ny;
    q->m_aafPos[2][0] = (bx - nx) / asp; q->m_aafPos[2][1] = by - ny;
    q->m_aafPos[3][0] = (bx + nx) / asp; q->m_aafPos[3][1] = by + ny;
    q->m_iSprite = 0;
    q->m_ulColor = c;
    wind(q);
}

/* couleurs (ABGR) : croix bleue, rond rouge, carré rose, triangle vert */
static const unsigned long k_col[4] = { 0xFFF0B87CUL, 0xFF6A6AFFUL, 0xFFD69CF0UL, 0xFF9AE040UL };

int padicon_draw(SPluginQuad_t *q, int max_q, int kind, float cx, float cy, float h, float asp)
{
    float X = cx * asp, r = h * 0.42f, th = h * 0.13f;
    unsigned long c;
    int n = 0, k;
    if (kind < 0 || kind > 3 || asp < 0.1f)
        return 0;
    c = k_col[kind];
    /* pastille sombre derrière (lisible sur tous les fonds) : octogone plein en 4 quadrilatères */
    if (n + 4 <= max_q) {
        float R = h * 0.62f;
        for (k = 0; k < 4; ++k) {
            float a0 = (float)k * 0.7853982f * 2.0f, a1 = a0 + 0.7853982f, a2 = a1 + 0.7853982f;
            SPluginQuad_t *p = &q[n++];
            p->m_aafPos[0][0] = cx; p->m_aafPos[0][1] = cy;
            p->m_aafPos[1][0] = (X + R * cosf(a0)) / asp; p->m_aafPos[1][1] = cy + R * sinf(a0);
            p->m_aafPos[2][0] = (X + R * cosf(a1)) / asp; p->m_aafPos[2][1] = cy + R * sinf(a1);
            p->m_aafPos[3][0] = (X + R * cosf(a2)) / asp; p->m_aafPos[3][1] = cy + R * sinf(a2);
            p->m_iSprite = 0;
            p->m_ulColor = 0xD0201814UL;
            wind(p);
        }
    }
    switch (kind) {
    case PADICON_CROSS:
        if (n + 2 <= max_q) {
            float d = r * 0.85f;
            band(&q[n++], X - d, cy - d, X + d, cy + d, th, asp, c);
            band(&q[n++], X - d, cy + d, X + d, cy - d, th, asp, c);
        }
        break;
    case PADICON_CIRCLE:
        for (k = 0; k < 16 && n < max_q; ++k) {
            float a0 = (float)k * 0.3926991f, a1 = a0 + 0.3926991f + 0.02f;
            band(&q[n++], X + r * cosf(a0), cy + r * sinf(a0), X + r * cosf(a1), cy + r * sinf(a1), th, asp, c);
        }
        break;
    case PADICON_SQUARE:
        if (n + 4 <= max_q) {
            float d = r * 0.8f, e = th * 0.5f;
            band(&q[n++], X - d - e, cy - d, X + d + e, cy - d, th, asp, c);
            band(&q[n++], X - d - e, cy + d, X + d + e, cy + d, th, asp, c);
            band(&q[n++], X - d, cy - d, X - d, cy + d, th, asp, c);
            band(&q[n++], X + d, cy - d, X + d, cy + d, th, asp, c);
        }
        break;
    case PADICON_TRIANGLE:
        if (n + 3 <= max_q) {
            float tx = X, ty = cy - r * 0.95f, lx = X - r * 0.95f, ly = cy + r * 0.7f, rx = X + r * 0.95f, ry = ly;
            band(&q[n++], tx, ty, lx, ly, th, asp, c);
            band(&q[n++], lx, ly, rx, ry, th, asp, c);
            band(&q[n++], rx, ry, tx, ty, th, asp, c);
        }
        break;
    }
    return n;
}

/* ---- texte avec symboles ---- */

static int is_word(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static int icon_of(const char *w, size_t l)
{
    if (l != 1)
        return -1;
    return w[0] == 'A' ? PADICON_CROSS : w[0] == 'B' ? PADICON_CIRCLE : w[0] == 'X' ? PADICON_SQUARE
         : w[0] == 'Y' ? PADICON_TRIANGLE : -1;
}

static void put_str(SPluginString_t *s, const char *t, float x, float y, float size, unsigned long c)
{
    snprintf(s->m_szString, sizeof(s->m_szString), "%s", t);
    s->m_afPos[0] = x;
    s->m_afPos[1] = y;
    s->m_iFont = 1;
    s->m_fSize = size;
    s->m_iJustify = 0;
    s->m_ulColor = c;
}

float padicon_text_width_rich(const char *txt, float size, float asp)
{
    float w = 0.0f, iw = size * 1.15f / (asp > 0.1f ? asp : 1.0f);
    const char *p = txt;
    if (!txt)
        return 0.0f;
    if (!padstyle_is_ps() || !g_font_ok)
        return padicon_text_width(padstyle_text(txt), size, asp);
    while (*p) {
        if (is_word(*p) && (p == txt || !is_word(p[-1]))) {
            const char *e = p;
            char wd[40];
            size_t wl;
            while (is_word(*e))
                ++e;
            if (icon_of(p, (size_t)(e - p)) >= 0)
                w += iw;
            else {
                wl = (size_t)(e - p) < sizeof(wd) - 1 ? (size_t)(e - p) : sizeof(wd) - 1;
                memcpy(wd, p, wl);
                wd[wl] = 0;
                w += padicon_text_width(padstyle_text(wd), size, asp);
            }
            p = e;
            continue;
        }
        {
            char one[2] = { *p, 0 };
            w += padicon_text_width(one, size, asp);
        }
        ++p;
    }
    return w;
}

int padicon_text(SPluginQuad_t *q, int max_q, int *nq, SPluginString_t *s, int max_s, int *ns, const char *txt, float x,
                 float y, float size, unsigned long color, int justify, float asp)
{
    /* morceaux : texte (noms PlayStation déjà mis : L1, R1, Options...) ou symbole */
    struct { int icon; char t[100]; } seg[16];
    int nseg = 0, i;
    float total = 0.0f, iw = size * 1.15f / (asp > 0.1f ? asp : 1.0f), cx;
    const char *p = txt;
    char cur[100];
    size_t cl = 0;
    if (!txt)
        return 0;
    if (!padstyle_is_ps() || !g_font_ok) {
        /* Xbox (ou largeur du texte inconnue) : texte seul */
        if (*ns >= max_s)
            return 0;
        put_str(&s[*ns], padstyle_text(txt), x, y, size, color);
        s[(*ns)++].m_iJustify = justify;
        return 1;
    }
    cur[0] = 0;
    while (*p && nseg < 15) {
        if (is_word(*p) && (p == txt || !is_word(p[-1]))) {
            const char *e = p;
            int ic;
            while (is_word(*e))
                ++e;
            ic = icon_of(p, (size_t)(e - p));
            if (ic >= 0) {
                if (cl) {
                    seg[nseg].icon = -1;
                    snprintf(seg[nseg++].t, sizeof(seg[0].t), "%s", cur);
                    cl = 0;
                    cur[0] = 0;
                }
                seg[nseg].icon = ic;
                seg[nseg++].t[0] = 0;
                p = e;
                continue;
            }
            {
                char w[40];
                size_t wl = (size_t)(e - p) < sizeof(w) - 1 ? (size_t)(e - p) : sizeof(w) - 1;
                const char *r;
                memcpy(w, p, wl);
                w[wl] = 0;
                r = padstyle_text(w);
                if (cl + strlen(r) + 1 < sizeof(cur)) {
                    memcpy(cur + cl, r, strlen(r) + 1);
                    cl += strlen(r);
                }
                p = e;
                continue;
            }
        }
        if (cl + 2 < sizeof(cur)) {
            cur[cl++] = *p;
            cur[cl] = 0;
        }
        ++p;
    }
    if (cl && nseg < 16) {
        seg[nseg].icon = -1;
        snprintf(seg[nseg++].t, sizeof(seg[0].t), "%s", cur);
    }
    for (i = 0; i < nseg; ++i)
        total += seg[i].icon >= 0 ? iw : padicon_text_width(seg[i].t, size, asp);
    cx = justify == 1 ? x - total * 0.5f : justify == 2 ? x - total : x;
    for (i = 0; i < nseg; ++i) {
        if (seg[i].icon >= 0) {
            if (*nq < max_q)
                *nq += padicon_draw(&q[*nq], max_q - *nq, seg[i].icon, cx + iw * 0.5f, y + size * 0.55f, size, asp);
            cx += iw;
        } else {
            if (*ns < max_s)
                put_str(&s[(*ns)++], seg[i].t, cx, y, size, color);
            cx += padicon_text_width(seg[i].t, size, asp);
        }
    }
    return nseg;
}

/* padicon.h - v1.8.109 : symboles PlayStation dessinés (quadrilatères) et texte mêlé de symboles */
#ifndef MXB_PADICON_H
#define MXB_PADICON_H

#include "mxb_api.h"

enum { PADICON_CROSS = 0, PADICON_CIRCLE, PADICON_SQUARE, PADICON_TRIANGLE };

/* largeurs des caractères lues dans la police du jeu (plugins\MXRP_Ragdoll.fnt) ; 1 si lue */
int padicon_load_font(const char *path);
/* largeur d'un texte à l'écran (0..1 de la largeur), taille de police size, aspect = largeur / hauteur */
float padicon_text_width(const char *t, float size, float aspect);
/* un symbole centré en (cx, cy), hauteur h ; rend le nombre de quadrilatères écrits */
int padicon_draw(SPluginQuad_t *q, int max_q, int kind, float cx, float cy, float h, float asp);
/* texte avec les boutons de la manette : en mode PlayStation, A / B / X / Y deviennent les symboles,
   LB, RB... leurs noms (L1, R1...) ; sinon texte seul. justify : 0 gauche, 1 centre, 2 droite. */
/* largeur qu'aurait padicon_text (symboles compris) */
float padicon_text_width_rich(const char *txt, float size, float asp);
int padicon_text(SPluginQuad_t *q, int max_q, int *nq, SPluginString_t *s, int max_s, int *ns, const char *txt, float x,
                 float y, float size, unsigned long color, int justify, float asp);

#endif

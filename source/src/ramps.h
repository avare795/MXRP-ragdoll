/*
 * ramps.h - v1.8.119 : rampes FMX à poser sur la piste (menu roue : « Rampes FMX »)
 *
 * Modèles du pack PROCworks FMX Ramp (3 formes, 4 couleurs). Fixes une fois posées (pas de physique
 * propre) : la moto roule dessus (boîtes inclinées données à la physique du jeu, comme les bottes),
 * le pilote marche et tombe dessus (hauteur du tablier). Vues par les autres joueurs du plugin.
 */
#ifndef MXB_RAMPS_H
#define MXB_RAMPS_H

#include <stdint.h>
#include "bales.h" /* BaleBox */
#include "objinst.h"

#define RAMPS_MAX 24
#define RAMP_BOXES 18 /* boîtes de physique par rampe (v1.8.143 : tablier en 16 morceaux, dos) */

typedef void (*RampsLogFn)(const char *fmt, ...);

void ramps_init(RampsLogFn log);
int ramps_count(void);
int ramps_shapes(void);
int ramps_shapes_menu(void); /* v1.8.150 : formes proposées dans le menu */
int ramps_colors(void);
const char *ramps_shape_name(int s);
const char *ramps_color_name(int c);
unsigned long ramps_color_rgb(int c); /* 0xBBGGRR (icônes) */
void ramps_set_shape(int s);
void ramps_set_color(int c);
int ramps_shape(void);
int ramps_color(void);
/* longueur de la forme s (m) */
#define RAMP_SHAPES_MAX 16
float ramps_length(int s);
/* v1.8.122 : icônes : hauteur du tablier (0..1 de la plus haute des formes) à u = 0..1 de la longueur */
float ramps_icon_prof(int s, float u);
/* v1.8.122 : forme à texture fixe (la couleur ne s'y applique pas) */
int ramps_shape_fixed(int s);
/* v1.8.122 : hauteur de la forme (m) */
float ramps_shape_height(int s);

/* pose : bas de la rampe (milieu) en (x, y, z), le saut vers la direction yaw (sin yaw, 0, cos yaw) ;
   rend son numéro ou -1 */
/* v1.8.124 : ground (peut être NULL) : hauteur du sol ; la rampe suit la pente du sol sous elle (sans flotter) */
int ramps_add(float x, float y, float z, float yaw, int (*ground)(float x, float z, float *h));
/* v1.8.141 : point le plus haut de la rampe i (coordonnées du plugin) */
float ramps_top(int i);
/* retire la plus proche (bord de la rampe à moins de maxd m) ; 1 si retirée */
int ramps_remove_nearest(float x, float z, float maxd);
void ramps_clear(void);

/* dessus de rampe le plus haut sous (x, z) qui ne dépasse pas ymax ; 1 si trouvé (nos rampes et celles des autres) */
int ramps_height(float x, float z, float ymax, float *h);
/* v1.8.131 : rampe (à nous) la plus proche : position dans son repère (lx largeur, ly longueur), demi-largeur, longueur ;
   rend sa distance à l'emprise (-1 : aucune) */
float ramps_local(float x, float z, float *lx, float *ly, float *W, float *L);
/* v1.8.125 : pilote à pied : une rampe plus haute que ses pieds + marche (côté, cadre, quarter pipe) le bloque */
int ramps_blocked(float x, float z, float feet, float r, float step);
/* v1.8.152 : sphère (p, r) dans une rampe : déplacement pour l'en sortir ; 1 si touchée */
int ramps_push(const float p[3], float r, float push[3]);
/* v1.8.155 : notre moto (point p, rayon r) sous le tablier d'une rampe : entrée, sortie (out) et normale (à plat) */
float ramps_wall_block(const float p[3], float r, float out[2], float n[2]);

/* boîtes pour la physique du jeu ; slot : numéro stable (nos rampes : base + RAMP_BOXES x rampe + k) */
int ramps_boxes(BaleBox *out, int *slot, int max, int base);

/* v1.8.120 : rampes écrites dans le relief du jeu (les roues roulent dessus) ; yoff : hauteur du monde du
   plugin moins celle du jeu. Entre terrain_raise_begin et terrain_raise_end. */
void ramps_terrain(float yoff);
/* v1.8.146 : rampes en maillage de triangles (les roues roulent dessus comme sur les passerelles des pistes,
   sans limite de hauteur). ramps_mesh_count : rampes à construire (les nôtres et celles des autres) ;
   ramps_mesh : rampe k en triangles, coordonnées du jeu (y - yoff) ; rend le nombre de sommets (0 : rien) */
#define RAMP_MESH_V 160
#define RAMP_MESH_T 320
int ramps_mesh_count(void);
int ramps_mesh(int k, float yoff, float *v, int *ix, int *nt);
/* v1.8.149 : dessus seul (pour les roues : rayon vertical) */
int ramps_mesh_top(int k, float yoff, float *v, int *ix, int *nt);
/* v1.8.152 : côtés, avant et dos seuls (murs) */
int ramps_mesh_walls(int k, float yoff, float *v, int *ix, int *nt);
/* v1.8.121 : change quand une rampe (la nôtre ou celle d'un autre) est posée, bougée ou retirée */
uint32_t ramps_signature(void);

/* dessin : modèle OBJM_RAMP0 + forme, sommets (9 nombres comme les bottes) ; textures RGB deflate brut */
const float *ramps_model_data(int shape, int *nverts);
int ramps_textures(void);
const unsigned char *ramps_texture_z(int k, int *len, int *w, int *h);
/* v1.8.159 : rediffusion (comme les cônes et les bottes) */
void ramps_hist_clear(void);
void ramps_hist_record(double t);
int ramps_hist_count(void);
void ramps_show_live(void);
int ramps_show_replay(double t);
int ramps_instances(ObjInst *out, int max, int tex0);

/* en ligne : nos rampes envoyées (un paquet), celles des autres reçues */
int ramps_net_pack(unsigned char *buf, int max);
void ramps_net_recv(uint32_t id, const unsigned char *buf, int len, double now);
void ramps_net_clock(double now);
int ramps_net_remote_count(void);

#endif

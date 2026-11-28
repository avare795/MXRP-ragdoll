/*
 * bales.h - v1.8.82 : bottes de foin à poser sur la piste (menu roue : « Bottes de foin »)
 *
 * Objets lourds (modèles hayBales.fbx et Bale.fbx du joueur) :
 *   - le pilote à pied bute dedans (il ne les pousse pas), il peut monter dessus ;
 *   - le pilote au sol est repoussé (il ne passe pas au travers) ;
 *   - une moto qui les percute les déplace un peu (elles glissent et s'arrêtent vite) ;
 *   - on peut les empiler ; vues en rediffusion et par les autres joueurs du plugin.
 */
#ifndef MXB_BALES_H
#define MXB_BALES_H

#include <stdint.h>

#define BALES_MAX 200 /* v1.8.89 : limite levée (24) ; numéros sur un octet en ligne */

typedef void (*BalesLogFn)(const char *fmt, ...);
/* hauteur du sol en (x, z) et sa normale (peut être NULL) ; 0 si inconnue */
typedef int (*BalesGroundFn)(float x, float z, float *h);

void bales_init(BalesLogFn log);
void bales_clear(void);
int bales_count(void);
/* v1.8.179 : catégorie de la page de la roue (0 bottes de foin, 1 mobilier) : variante choisie, pose,
   retirer, porter, remettre au départ et compte ne touchent qu'elle */
void bales_set_cat(int c);
void bales_cat_range(int c, int *first, int *n);
int bales_count_cat(void);
void bales_clear_cat(void);
/* v1.8.181 : s'asseoir : chaise la plus proche (rang ou -1), son assise (monde) et son avant ;
   la chaise où il est assis ne le pousse pas (-1 : aucune) */
int bales_nearest_chair(float x, float z, float maxd);
int bales_chair_seat(int i, float seat[3], float front[3]);
void bales_set_ignore(int i);
/* v1.8.182 : sol du mobilier (relief seul) */
void bales_set_furn_ground(BalesGroundFn fn);
int bales_chair_front(int v, float *fx, float *fy);
int bales_variants(void);
void bales_set_variant(int v);
int bales_variant(void);
unsigned long bales_variant_rgb(int v); /* 0xBBGGRR (icônes) */
const char *bales_variant_name(int v);
/* demi-largeur, demi-longueur, hauteur de la variante v */
void bales_variant_size(int v, float *hx, float *hy, float *h);

/* pose une botte (variante choisie) : centre du dessous en (x, y, z), cap yaw (axe long le long de
   (cos yaw, -sin yaw) : en travers de la direction yaw) ; rend son numéro ou -1 */
int bales_add(float x, float y, float z, float yaw, BalesGroundFn ground);
int bales_remove_nearest(float x, float z, float maxd);
void bales_reset_spawn(void);
int bales_moved_count(void);
/* v1.8.84 : la petite se porte : la plus proche (à moins de maxd m), ou -1 ; tenue (sans physique) en
   (x, y, z) (centre du dessous), lâchée (elle tombe), nouveau point de départ, retirée, variante */
int bales_carry_nearest(float x, float z, float maxd);
void bales_hold(int i, float x, float y, float z, float yaw);
void bales_hold_end(int i);
void bales_set_spawn(int i, float x, float y, float z, float yaw);
void bales_remove(int i);
int bales_get_var(int i);
/* centre du dessous (pos) et cap ; 0 si pas de botte i */
int bales_get(int i, float pos[3], float *yaw);

/* une image : glisse, suit le sol, se pose sur une autre botte */
void bales_update(float dt, BalesGroundFn ground);
/* v1.8.101 : bottes pas encore endormies (recalculées à chaque appel) */
int bales_awake(void);
/* une moto (pos, vitesse, rayon) : la pousse un peu ; rend le nombre de bottes touchées */
int bales_bike_pass(const float pos[3], const float vel[3], float r);

/* dessus de botte le plus haut sous (x, z) qui ne dépasse pas ymax (marcher dessus) ; 1 si trouvé */
int bales_height(float x, float z, float ymax, float *h);
/* pilote debout en (x, z), pieds à feet, rayon r : bute-t-il dans une botte ? */
int bales_blocked(float x, float z, float feet, float r);
/* pilote debout : déplacement (x, z) pour sortir d'une botte (s'il y est entré) ; 1 si dedans */
int bales_push_walker(float x, float z, float feet, float r, float out[2]);
/* sphère (p, r) du corps au sol dans une botte : déplacement pour l'en sortir ; 1 si dedans */
int bales_push(const float p[3], float r, float push[3]);

/* dessin : sommets 3D (3 par triangle), comme cones_mesh ; texture = tex0 + numéro de la texture */
int bales_mesh(float (*pos)[3], float (*uv)[2], unsigned long *col, unsigned char *tex, int max_verts,
               const float eye[3], int tex0);
/* textures (RGB deflate brut) : nombre, puis texture k */
int bales_textures(void);
const unsigned char *bales_texture_z(int k, int *len, int *w, int *h);

/* rediffusion */
void bales_hist_record(double t);
void bales_hist_clear(void);
int bales_hist_count(void);
void bales_show_live(void);
int bales_show_replay(double t);
int bales_shown_count(void);

/* en ligne : nos bottes envoyées, celles des autres reçues (dessinées, solides) */
int bales_net_moving(void);
int bales_net_pack(unsigned char *buf, int max, int part, int *nparts);
void bales_net_recv(uint32_t id, const unsigned char *buf, int len, double now);
void bales_net_clock(double now);
int bales_net_remote_count(void);
int bales_mesh_remote(float (*pos)[3], float (*uv)[2], unsigned long *col, unsigned char *tex, int max_verts,
                      int tex0);

/* v1.8.87 : boîtes à donner à la physique du jeu (la ronde en fait deux, en octogone) : centre (monde
   du plugin), rotation (lignes ; colonnes = axes de la boîte : largeur, haut, longueur), demi-côtés ;
   slot : numéro stable de chaque boîte (nos bottes : 2 x botte + k). Rend le nombre de boîtes. */
typedef struct {
    float c[3], R[9], h[3];
} BaleBox;
int bales_boxes(BaleBox *out, int *slot, int max);
/* v1.8.89 : notre moto (position gardée par l'appelant, vitesse) : la petite botte n'est pas un mur
   pour elle quand elle arrive vite */
void bales_set_bike(const float pos[3], float speed);

/* v1.8.91 : caméra (NULL : pas de tri) : bottes derrière elle ou à plus de maxd m pas dessinées */
void bales_set_view(const float eye[3], const float fwd[3], float maxd);

/* v1.8.92 : 1 = botte ronde simplifiée (objets optimisés), 0 = haute qualité */
void bales_set_quality(int optimized);

/* v1.8.94 : notre moto dans une petite botte d'un autre joueur (pour la freiner) ; clé de la botte */
int bales_net_bike_touch(const float pos[3], float r, uint32_t *key);

/* v1.8.96 : poussées à pied des petites bottes des autres joueurs (à leur envoyer), et reçues */
int bales_net_push_take(uint32_t *id, int *slot, float *dx, float *dz);
void bales_net_push_apply(int slot, float dx, float dz);
/* v1.8.96 : ronde = mur pour notre moto : sortie (x, z), normale ; profondeur (0 : pas dedans) */
float bales_round_block(const float p[3], float r, float out[2], float n[2]);

/* v1.8.97 : dessin par modèle : sommets d'un modèle OBJM_BALE_... (9 nombres : x, y, haut, normale,
   u, v, texture), objets (les nôtres ou la rediffusion, puis ceux des autres) */
#include "objinst.h"
const float *bales_model_data(int model, int *nverts);
int bales_instances(ObjInst *out, int max, int tex0);
int bales_instances_remote(ObjInst *out, int max, int tex0);

#endif

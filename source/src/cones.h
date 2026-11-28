/*
 * cones.h - v1.8.45 : cônes à poser sur la piste (menu : flèche Bas ou bouton B, à pied)
 *
 * Les cônes ne sont que chez nous (ni le jeu ni les autres joueurs ne les voient) :
 *   - dessinés par-dessus l'image (projetés avec la caméra), cachés derrière le relief ;
 *   - le pilote à pied bute dedans, le corps au sol est repoussé ;
 *   - une moto qui passe dessus (la nôtre ou une autre) les renverse.
 */
#ifndef MXB_CONES_H
#define MXB_CONES_H

#define CONES_MAX 250 /* v1.8.89 : limite levée (48) ; numéros sur un octet en ligne */

typedef void (*ConesLogFn)(const char *fmt, ...);
/* hauteur du sol en (x, z) ; 0 si inconnue */
typedef int (*ConesGroundFn)(float x, float z, float *h);
/* point du monde -> écran (0..1) ; 0 si derrière la caméra */
typedef int (*ConesProjectFn)(const float p[3], float *sx, float *sy);
/* quadrilatère numéro idx à l'écran (4 points 0..1, couleur 0xAABBGGRR) */
typedef void (*ConesQuadFn)(int idx, const float x[4], const float y[4], unsigned long color);

void cones_init(ConesLogFn log);
void cones_clear(void);
/* v1.8.51 : hauteur du cône au-dessus du sol (m) */
void cones_set_lift(float m);
int cones_count(void);
/* pose un cône debout en (x, y, z) ; rend son numéro ou -1 (plein) */
int cones_add(float x, float y, float z);
/* retire le cône le plus proche de (x, z) à moins de maxd m ; 1 si retiré */
int cones_remove_nearest(float x, float z, float maxd);
/* relève tous les cônes renversés, là où ils sont */
void cones_reset_upright(void);
/* v1.8.57 : remet tous les cônes debout là où ils ont été posés ; nombre de cônes déplacés ou tombés */
void cones_reset_spawn(void);
/* v1.8.62 : cône tenu par le pilote (animation de pose) : centre du socle, cap ; puis lâché */
void cones_hold(int i, float x, float y, float z, float yaw);
void cones_hold_end(int i);
/* v1.8.63 : nouveau point de départ (reposé ailleurs) ; retirer un cône */
void cones_set_spawn(int i, float x, float y, float z, float yaw);
void cones_remove(int i);
int cones_moved_count(void);
int cones_down_count(void);

/* renverse le cône i, poussé à la vitesse vel (m/s, monde) */
void cones_knock(int i, const float vel[3]);
/* position et état du cône i (pos : centre du socle) */
int cones_get(int i, float pos[3], int *down);

/* une image : cônes renversés qui glissent et se couchent, posés sur le sol */
void cones_update(float dt, ConesGroundFn ground);

/* une moto (pos, vitesse) : renverse les cônes qu'elle touche (rayon r autour de pos) */
int cones_bike_pass(const float pos[3], const float vel[3], float r);

/* pilote debout en (x, z), pieds à feet, rayon r : déplacement pour sortir des cônes debout.
   vx, vz : sa vitesse ; en courant, il renverse le cône */
int cones_push_walker(float x, float z, float feet, float r, float vx, float vz, float out[2]);
/* point (p, r) du corps au sol (ragdoll) qui touche un cône debout : le cône tombe (il est léger,
   le corps n'est pas repoussé). Rend le nombre de cônes renversés */
int cones_body_touch(const float p[3], float r);

/* dessin : eye = position de la caméra ; rend le nombre de quadrilatères écrits.
   ground (peut être NULL) sert à cacher les cônes derrière le relief */
int cones_draw(int max, const float eye[3], ConesProjectFn proj, ConesQuadFn put, ConesGroundFn ground);

/* v1.8.49 : sommets 3D des cônes affichés (modèle du joueur) : 3 par triangle, position monde,
   coordonnées de texture, éclairage gris 0x00BBGGRR, numéro de texture (variante).
   eye (peut être NULL) : les cônes trop loin sont laissés. Rend le nombre de sommets */
int cones_mesh(float (*pos)[3], float (*uv)[2], unsigned long *col, unsigned char *tex, int max_verts,
               const float eye[3]);
/* texture de la variante v : RGB compressé (deflate brut), len octets, w x h une fois décompressée */
const unsigned char *cones_texture_z(int v, int *len, int *w, int *h);

/* v1.8.52 : variantes (textures du modèle) ; celle des prochains cônes posés */
int cones_variants(void);
void cones_set_variant(int v);
int cones_variant(void);
unsigned long cones_variant_rgb(int v); /* couleur du corps 0xBBGGRR (icônes) */

/* v1.8.52 : rediffusion : états retenus au temps de la rediffusion pendant qu'on joue */
void cones_hist_record(double t);
void cones_hist_clear(void);
int cones_hist_count(void);
/* ce que cones_mesh dessine : les cônes du moment, ou ceux de la rediffusion au temps t */
void cones_show_live(void);
int cones_show_replay(double t);
int cones_shown_count(void);

/* v1.8.65 : en ligne. Nos cônes : bougent-ils (envoi plus souvent) ; paquet de la partie part
   (24 cônes par partie) ; ceux des autres : reçus (fil du réseau), comptés, dessinés */
#include <stdint.h>
int cones_net_moving(void);
int cones_net_pack(unsigned char *buf, int max, int part, int *nparts);
void cones_net_recv(uint32_t id, const unsigned char *buf, int len, double now);
void cones_net_clock(double now);
int cones_net_remote_count(void);
/* v1.8.66 : cônes des autres joueurs : on se les fait donner pour les toucher, ramasser... */
int cones_net_give_size(void);
int cones_net_give(int slot, unsigned char *out);
int cones_net_adopt(uint32_t from, int from_slot, const unsigned char *data, int len);
int cones_net_nearest(float x, float z, float maxd, uint32_t *id, int *slot, float pos[3]);
int cones_net_touching(const float p[3], float r, uint32_t *id, int *slot);
void cones_net_forget(uint32_t id, int slot);
int cones_mesh_remote(float (*pos)[3], float (*uv)[2], unsigned long *col, unsigned char *tex, int max_verts);

/* v1.8.91 : caméra (NULL : pas de tri) : cônes derrière elle ou à plus de maxd m pas dessinés */
void cones_set_view(const float eye[3], const float fwd[3], float maxd);

/* v1.8.97 : dessin par modèle : sommets du modèle (x, y, haut, normale, u, v : 8 nombres), objets
   (les nôtres ou ceux de la rediffusion, puis ceux des autres joueurs) ; textures à partir de tex0 */
#include "objinst.h"
const float *cones_model_data(int *nverts);
int cones_instances(ObjInst *out, int max, int tex0);
int cones_instances_remote(ObjInst *out, int max, int tex0);

#endif

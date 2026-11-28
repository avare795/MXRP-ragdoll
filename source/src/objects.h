/*
 * objects.h - objets de la piste (tremplins, bottes de paille, ponts...) pour marcher dessus
 *
 * Les objets sur lesquels la moto roule sont des objets ODE (geoms) du même espace que le relief :
 * boîtes et maillages de triangles. On les retrouve à partir de l'objet ODE du relief (liste
 * chaînée de l'espace ODE), on copie leurs surfaces une fois (ils ne bougent pas), puis on
 * donne la hauteur de la surface sous un point.
 */
#ifndef MXB_RAGDOLL_OBJECTS_H
#define MXB_RAGDOLL_OBJECTS_H

typedef void (*ObjLogFn)(const char *fmt, ...);

/* À appeler souvent : lance la recherche (en arrière-plan) quand le relief est trouvé, et la
   relance si la piste change. */
void objects_forget(void);
void objects_poll(ObjLogFn log);
/* Surface d'objet la plus haute sous (x, z) qui ne dépasse pas ymax. 1 si trouvée. */
int objects_height(float x, float z, float ymax, float *h);
int objects_ready(void);
/* Un pilote debout en (x, z), pieds à la hauteur feet, rayon r, bute-t-il dans un objet
   (mur, barrière, bord trop haut) ? */
int objects_blocked(float x, float z, float feet, float r);
/* Sphère (p, r) dans un objet (boîte, mur) ? 1 et push = déplacement pour l'en sortir */
int objects_push(const float p[3], float r, float push[3]);

/* Collisions des objets ODE attachés à ces corps : 0 = coupées, 1 = remises. -1 si les objets
   de la piste ne sont pas encore connus. */
#include <stdint.h>
int objects_body_collide(const uintptr_t *bodies, int n, int on);

/* v1.8.87 : obstacle i (boîte ODE ajoutée à la physique du jeu : la moto et le pilote du jeu la
   touchent). on = 0 : désactivée. c : centre (coordonnées ODE), R : rotation (lignes), h : demi-côtés.
   1 si fait. */
int objects_obstacle(int i, int on, const float c[3], const float R[9], const float h[3]);
int objects_obstacle_ready(void);
/* v1.8.95 : nouvel événement : nos boîtes oubliées (le jeu recrée sa physique) */
void objects_obstacles_reset(void);
void objects_obstacle_log(ObjLogFn log);

/* v1.8.120 : relief du jeu changé (rampes) : boîte englobante à recalculer */
void objects_relief_moved(void);
/* v1.8.138 : décalage du pointeur vers les données du relief dans l'objet ODE du relief (-1 : pas trouvé) */
int objects_hf_data_off(uintptr_t hdr);
/* v1.8.138 : boîte englobante d'un autre objet ODE à recalculer (1 : fait) */
int objects_geom_dirty(uintptr_t g);

/* v1.8.146 : rampes en maillage de triangles dans la physique du jeu, à côté du relief (même espace, mêmes filtres :
   les roues roulent dessus). clear : retire les nôtres ; add : un maillage (sommets en coordonnées ODE, copiés) ;
   stale : à refaire (autre piste, ou pas encore pu être fait) ; 1 si fait */
void objects_ramp_meshes_clear(void);
int objects_ramp_mesh_add(const float *v, int nv, const int *ix, int nt);
/* v1.8.152 : murs (côtés, dos) : la moto (roues comprises) et le pilote butent dedans */
int objects_ramp_wall_add(const float *v, int nv, const int *ix, int nt);
/* v1.8.153 : nos espaces (diagnostic) */
void objects_ramp_spaces(uintptr_t *top, uintptr_t *wall);
int objects_ramp_meshes_stale(void);
int objects_ramp_meshes_count(void);
void objects_ramp_meshes_log(ObjLogFn log);

#endif

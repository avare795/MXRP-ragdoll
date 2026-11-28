/*
 * bike.h - corps physiques ODE de la moto (MX Bikes beta21e) : liste et redressement
 */
#ifndef MXB_RAGDOLL_BIKE_H
#define MXB_RAGDOLL_BIKE_H

#include <stdint.h>

typedef void (*BikeLogFn)(const char *fmt, ...);
/* hauteur du sol en (x, z) : 1 si connue */
typedef int (*BikeGroundFn)(float x, float z, float *y);
void bike_set_ground_fn(BikeGroundFn fn);
/* Moto arrêtée droite sur ses roues : mesure la hauteur du centre des roues au-dessus du sol
   (retourne la mesure, 0 si impossible) */
float bike_measure_wheels(uintptr_t chassis);

/* Écrit dans le journal tous les corps du monde ODE du châssis */
void bike_log_bodies(uintptr_t chassis, BikeLogFn log);

/*
 * Redresse la moto : toutes les pièces à moins de 2,5 m du châssis (sauf le
 * corps proche de exclude, le pilote) tournent ensemble pour remettre le
 * châssis debout (même cap), centre de gravité du châssis à cg_height, vitesses
 * à zéro. Retourne le nombre de corps déplacés.
 */
int bike_upright(uintptr_t chassis, float cg_height, const float exclude[3], BikeLogFn log);

/*
 * Relever la moto en douceur : bike_lift_begin mémorise les pièces et le
 * mouvement, bike_lift_apply(t) place la moto entre couchée (0) et debout (1)
 * (à appeler à chaque pas tant qu'on la tient), bike_lift_end la lâche.
 */
int bike_lift_begin(uintptr_t chassis, float cg_height, float pitch, const float exclude[3], BikeLogFn log);
void bike_lift_apply(float t);
/* v1.8.20 : même position que le dernier bike_lift_apply (avant chaque image) */
void bike_lift_reapply(void);
void bike_lift_end(void);
void bike_lift_forget(void); /* v0.41.26 : sortie de piste / nouvelle piste : pièces oubliées sans écriture */
/* 1 : la moto relevée est gelée (immobile pour le jeu, qui refuse le respawn si elle bouge) */
void bike_lift_freeze(int on);
int bike_lift_active(void);
/* moto tenue sans gravité (pièces de la moto relevée) */
int bike_lift_frozen(void);
int bike_lift_bodies(uintptr_t *out, int max);
/* 1 : la moto tenue est remontée (10 cm au plus) si une roue touche le sol */
void bike_lift_keep_wheels(int on);
/* Moto couchée : roues arrêtées (moteur en prise, elles creusaient la piste) ; nombre de pièces corrigées */
int bike_calm_wheels(uintptr_t chassis);
/* Châssis de la moto tenue (0 si aucune) */
uintptr_t bike_lift_chassis(void);

/*
 * Remonter : bike_capture_target mémorise la pose du châssis tenu debout ;
 * après le respawn du jeu, bike_move_to_target replace d'un bloc tous les corps
 * proches du châssis (moto et pilote) à cette pose, vitesses à zéro.
 */
int bike_capture_target(uintptr_t chassis);
int bike_move_to_target(uintptr_t chassis, BikeLogFn log);
void bike_clear_target(void);
int bike_has_target(void);
/* Fait basculer la moto (vitesse de rotation autour de axis, pivot pivot_drop m sous le châssis) */
int bike_tip(uintptr_t chassis, const float axis[3], float rate, float pivot_drop);

void bike_set_ground_trusted(int on); /* v1.7.16 */
/* v1.8.89 : vitesse horizontale des pièces de la moto (et du pilote) multipliée par k ; nombre de pièces */
int bike_slow(uintptr_t chassis, float k);

/* v1.8.96 : moto sortie d'un obstacle de (dx, dz), vitesse contre la normale (nx, nz) annulée */
int bike_block(uintptr_t chassis, float dx, float dz, float nx, float nz);

#endif

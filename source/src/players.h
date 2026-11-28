/*
 * players.h - v1.8 : collisions avec les autres joueurs
 *
 * Les motos des autres joueurs (positions données par le jeu : RaceTrackPosition) et les pilotes
 * à pied des autres joueurs (poses reçues de leur plugin) deviennent des obstacles :
 *   - à pied, on ne traverse plus une moto ni un autre pilote à pied ;
 *   - une moto qui roule et nous percute à pied nous fait tomber (ragdoll) ;
 *   - au sol (ragdoll), le corps est repoussé par les motos au lieu de les traverser.
 * Seulement notre pilote est touché : chacun gère le sien (même règle chez les autres).
 */
#ifndef MXB_PLAYERS_H
#define MXB_PLAYERS_H

#include "ragdoll.h"

typedef void (*PlayersLogFn)(const char *fmt, ...);

void players_init(PlayersLogFn log);
void players_reset(void);
/* positions des motos, telles que le jeu les donne (tableau d'éléments de taille elem) */
void players_track(int n, const void *arr, int elem, double now);
/* notre numéro de course (notre moto n'est pas un obstacle) */
void players_set_self(int num);
/* position de notre moto (télémétrie) : la moto du jeu à cet endroit est la nôtre */
void players_set_self_pos(const float p[3]);
/* pilote à pied d'un autre joueur (points du corps), à remettre à chaque image */
void players_walkers_clear(void);
void players_walker_add(const float pts[RD_NUM_POINTS][3]);

/* à pied : un pilote en (x, z), pieds à feet, rayon r, bute-t-il dans une moto ou un pilote ? */
int players_blocked(float x, float z, float feet, float r);
/* sphère (p, r) dans une moto ou un pilote : déplacement pour l'en sortir */
int players_push(const float p[3], float r, float push[3]);
/* une moto qui roule (> min_speed m/s) touche le point p (rayon r) ? vel = sa vitesse, num = son n° */
int players_hit(const float p[3], float r, float min_speed, float vel[3], int *num);

/* diagnostic : moto la plus proche (distance au bord), sa vitesse, son n° */
int players_nearest(const float p[3], float *dist, float *speed, int *num);
int players_walker_count(void);
/* hauteur du point au-dessus de la position de la moto la plus proche (dernier players_nearest) */
float players_near_dy(void);
/* v1.8.2 : horloge des collisions (pour prévoir la position des motos) */
void players_set_now(double now);

/* v1.8.4 : motos vues par leur modèle 3D (à remettre à chaque image). own_collide : notre moto compte */
void players_models_clear(int own_collide);
void players_model_add(const float M[12], int own);
int players_model_count(void);

/* v1.8.5 : pilote debout dans un obstacle : déplacement horizontal pour l'en sortir */
int players_push_walker(float x, float z, float feet, float r, float out[2]);

#endif

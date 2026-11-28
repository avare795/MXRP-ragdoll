/*
 * ridepose.h - v1.7 : la place du pilote sur SA moto, apprise en regardant le jeu quand il roule
 *
 * Quand le joueur roule droit, le jeu pose lui-même le pilote sur la moto d'après le gfx.cfg de
 * la moto (mains sur les poignées, fesses sur la selle, pieds sur les repose-pieds). On note ces
 * points dans le repère du guidon (modèle steer de la moto), pour chaque moto, et on s'en sert
 * pour les gestes à pied : mains sur le guidon, montée sur la selle, pieds sur les repose-pieds.
 */
#ifndef MXB_RIDEPOSE_H
#define MXB_RIDEPOSE_H

typedef void (*RidePoseLogFn)(const char *fmt, ...);

void ridepose_init(const char *save_dir, RidePoseLogFn log);
/* à chaque mise à jour d'un squelette pendant qu'on roule droit (le jeu pilote le pilote) */
void ridepose_sample(void *skeleton, const float bike_pos[3], void *own_skel);
/* points appris pour la moto près de center, en monde : 0 = pas encore appris pour cette moto.
   grip : 0 gauche, 1 droite (paume) ; pelvis : bassin assis ; ankle : chevilles sur les repose-pieds */
int ridepose_world(const float center[3], float grip[2][3], float pelvis[3], float ankle[2][3]);
/* v1.7.8 : aussi le milieu des épaules (buste penché comme quand il roule) */
int ridepose_world2(const float center[3], float grip[2][3], float pelvis[3], float ankle[2][3], float shoulder[3]);
void ridepose_save(void);
/* journal : pourquoi des moments ne servent pas */
void ridepose_report(void);
/* v1.7.3 : hauteur du point de référence de la moto (télémétrie) au-dessus du sol, apprise sur les roues */
int ridepose_refh(const float center[3], float *h);
void ridepose_set_refh(const float center[3], float h);

/* v1.7.27 : relire le gfx.cfg de la moto à la prochaine utilisation */
void ridepose_reload_gfx(void);
/* v1.7.32 : ID de la moto du joueur (= nom de son dossier) : son guidon passe avant celui des autres */
void ridepose_set_bike(const char *id);
#include <stddef.h>
int ridepose_find_steer(const float center[3], float M[12], char *path, size_t pl);

/* v1.8.4 : fichier du modèle du cadre d'une moto (lu dans son gfx.cfg, gardé en mémoire) */
const char *ridepose_chassis_of(const char *dir);

#endif

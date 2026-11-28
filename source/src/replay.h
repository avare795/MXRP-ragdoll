/*
 * replay.h - revoir les chutes et la marche du plugin dans la rediffusion du jeu
 *
 * Le jeu ne rejoue que sa propre physique. Pendant qu'on roule (mode Essai), chaque passage du
 * pilote sous notre contrôle (chute, marche, descente) est enregistré : position du modèle au
 * départ, puis à chaque image les points du pilote et l'état du pilotage (tête, pieds, mains).
 * En rediffusion, quand le pilote du jeu repasse exactement au point et dans l'orientation d'un
 * départ enregistré, on rejoue l'enregistrement sur son squelette.
 * Fichiers : <dossier de sauvegarde>\MXRP_Ragdoll_replay\<piste>.rdr (un segment ajouté par chute).
 */
#ifndef MXB_RAGDOLL_REPLAY_H
#define MXB_RAGDOLL_REPLAY_H

#include "drive.h"
#include "ragdoll.h"

typedef void (*ReplayLogFn)(const char *fmt, ...);

void replay_init(const char *save_dir, ReplayLogFn log);
/* Piste en cours (nom de fichier sûr) */
void replay_set_track(const char *track_id);
/* nouvelle session de jeu (EventInit) : son horloge repart de 0 */
void replay_new_session(void);

/* Enregistrement (pendant qu'on roule). ch : 0 = le joueur, 1 à REPLAY_CH-1 = pilotes des autres
   joueurs reçus par le réseau du plugin */
#define REPLAY_CH 8
/* rc_live : horloge de la rediffusion lue pendant la session (-1 : inconnue) */
void replay_rec_begin(int ch, const float model_world[12], double rc_live);
/* model_world : modèle du pilote (corps de chute du jeu) à cette image, pour reconnaître le pilote */
void replay_rec_frame(int ch, double t, const Ragdoll *rd, const DriveSnap *snap, const float model_world[12]);
void replay_rec_end(int ch);
void replay_rec_end_all(void);
int replay_rec_active(int ch);
/* Pendant la rediffusion : l'enregistrement en cours du canal ch est aussi lu (en direct),
   à partir du temps de la rediffusion rc_start */
void replay_rec_share(int ch, double rc_start);

/* Rediffusion : charge les enregistrements de la piste (ou de toutes si inconnue) */
void replay_load(void);
/* À chaque mise à jour d'un modèle de pilote en rediffusion (key = son squelette) : 1 si un
   enregistrement est joué sur lui (rd et snap remplis pour l'instant t). Au plus REPLAY_PLAYERS
   pilotes rejoués en même temps (une place de pilotage chacun, gérées par l'appelant). */
/* have_rc / rc : temps de la rediffusion (s), s'il est connu (retours en arrière, avance rapide) */
#define REPLAY_PLAYERS 8
int replay_play(void *key, const float model_world[12], const float pelvis[3], double now, int have_rc, double rc,
                Ragdoll *rd, DriveSnap *snap, int *starting, int *player);
/* key NULL : tous */
void replay_stop(void *key);

#endif

/*
 * drive.h - pilote le squelette 3D du pilote (rider.hrc) avec le ragdoll
 *
 * Principe (validé en jeu, beta21e) : avant que le jeu mette à jour le
 * modèle rider.hrc, on écrit les matrices LOCALES des os (+0x68). Le jeu
 * calcule ensuite les matrices monde et la peau du pilote à partir d'elles.
 *
 * On calcule, pour chaque os piloté, la rotation monde voulue à partir des
 * 16 points du ragdoll, puis local = inverse(monde du parent) x monde voulu.
 * Les autres os (doigts, clavicules, torsions...) gardent leur matrice locale
 * et suivent leur parent.
 */
#ifndef MXB_RAGDOLL_DRIVE_H
#define MXB_RAGDOLL_DRIVE_H

#include "ragdoll.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DriveLogFn)(const char *fmt, ...);

void drive_init(DriveLogFn log);

/* Places de pilotage : 0 = le joueur, 1 à DRIVE_SLOTS-1 = pilotes des autres joueurs (réseau).
   Toutes les fonctions ci-dessous agissent sur la place choisie par le fil appelant. */
#define DRIVE_SLOTS 8
void drive_select(int slot);

/*
 * Lit la pose actuelle du squelette, prépare les LOD et lance le ragdoll
 * depuis cette pose. vel : vitesse initiale (m/s). Retourne 1 si réussi.
 */
int drive_spawn(void *skeleton, Ragdoll *rd, const float vel[3], float ground_y);

/*
 * Accroche le ragdoll au corps de chute du jeu (à appeler avant rd_step) :
 * bassin tiré vers lui (strength, 1/s) et sol placé body_ground_offset sous lui.
 */
void drive_anchor(void *skeleton, Ragdoll *rd, float dt, float strength, float body_ground_offset);

/* Écrit la pose du ragdoll dans les matrices locales (avant la mise à jour du jeu) */
int drive_apply(void *skeleton, const Ragdoll *rd);

/* Remet les matrices locales d'origine (fin de chute) */
void drive_restore(void);

/* Squelette piloté actuellement (NULL si aucun) */
void *drive_skeleton(void);

/* Pieds pilotés : direction talon -> pointe (monde) de chaque pied ; NULL = pieds libres */
void drive_set_feet(const float left[3], const float right[3]);

/* Repère de la marche (droite, avant du pilote) : tête de niveau et mains détendues ; NULL = libre */
void drive_set_walk_frame(const float right[3], const float fwd[3]);
/* Mains au guidon : axe du guidon (main gauche -> main droite), NULL pour arrêter */
void drive_set_grip(const float bar[3]);
/* Poings : 0 = mains de la pose du jeu, 1 = poings fermés (marche, course) */
void drive_set_fist(float amount);
/* Diagnostic : os pilotés que le jeu réécrit pendant sa mise à jour (après l'appel du jeu) */
int drive_check_overwritten(void *skeleton, DriveLogFn log);
/* Journal : os du LOD 0 (nom, parent, rôle, translation locale de départ) */
void drive_log_nodes(DriveLogFn log);
/* Après la mise à jour du jeu : hold = 0 retient la rotation des poignets, 1 la remet (course) */
void drive_wrists_post(void *skeleton, int hold);
/* Modèle à piloter : 1 = mod de pilote ; up = hauteur ajoutée à l'affichage (m) */
void drive_set_model(int is_mod, float up);
/* Matrice monde attendue (3x4 par lignes) d'un os du pilote, par son nom */
int drive_expected_world(const char *name, float out[12]);
/* Recopie la matrice monde (+0x100) dans la matrice de rendu (+0x1C0) de chaque os ; log = journal des écarts */
int drive_sync_render(void *skeleton, DriveLogFn log);
/* Tête stabilisée en marchant : direction cou -> tête et axe droite (monde) ; NULL = libre */
void drive_set_head(const float dir[3], const float right[3]);
/* État du pilotage en plus des points du ragdoll (pour la rediffusion) */
typedef struct {
    int head_on, feet_on, frame_on;
    float head_dir[3], head_right[3];
    float feet[2][3];
    float frame_right[3], frame_fwd[3];
    float fist, root_up;
} DriveSnap;
void drive_get_snap(DriveSnap *s);
void drive_set_snap(const DriveSnap *s);

/* Hauteur de la matrice du modèle (corps de chute du jeu) à la dernière image */
float drive_model_y(void);

/* Écart (somme des valeurs absolues) entre monde(Point01) prédit et lu, mesuré au départ */
float drive_root_error(void);

/* v1.8.79 : matrices monde de l'os de la tête (au départ, maintenant) ; rend le numéro du départ ou 0 */
int drive_head_frames(float spawn[12], float now[12]);

#ifdef __cplusplus
}
#endif

#endif /* MXB_RAGDOLL_DRIVE_H */

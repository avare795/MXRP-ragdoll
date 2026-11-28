/*
 * skelscan.h - recherche des os du pilote dans la mémoire du jeu (diagnostic phase 2)
 *
 * Le hook transmet à chaque image le pointeur du squelette mis à jour par le
 * jeu. Pendant une chute, on parcourt ces structures (et celles qu'elles
 * pointent) à la recherche de positions 3D proches de la moto : ce sont
 * probablement les os du pilote. Les résultats vont dans le journal.
 */
#ifndef MXB_RAGDOLL_SKELSCAN_H
#define MXB_RAGDOLL_SKELSCAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ScanLogFn)(const char *fmt, ...);

void skelscan_init(ScanLogFn log);

/* À brancher sur le hook : enregistre un pointeur de squelette */
int skelscan_record(void *skeleton);   /* retourne l'index du modèle, ou -1 */

/* RIDER_EDF : modèle de pilote chargé sans rider.hrc (mods : gfx.cfg -> rider.edf) */
/* RIDER_CEDF : c_rider.edf, le modèle que le jeu affiche pendant une chute (mods sans rider.hrc) */
enum { SKEL_KIND_OTHER = 0, SKEL_KIND_RIDER_HRC = 1, SKEL_KIND_RIDER_MESH = 2, SKEL_KIND_RIDER_EDF = 3, SKEL_KIND_RIDER_CEDF = 4 };
/* 1 si un modèle de ce type est à moins de radius m de center */
int skelscan_kind_near(int kind, const float center[3], float radius);

/* Type du modèle d'index donné (SKEL_KIND_*) */
int skelscan_kind(int index);
/* Chemin du fichier du modèle ("" si inconnu) */
const char *skelscan_name(int index);

/* Test d'écriture : monte tous les os du modèle de dy mètres. Retourne le nombre d'os modifiés. */
int skelscan_lift(void *skeleton, float dy);
/* Idem ; with_second = 1 décale aussi la seconde matrice de chaque nœud (+0x140) */
int skelscan_lift_ex(void *skeleton, float dy, int with_second);

/* Lit / écrit la hauteur (ty) de la matrice monde du modèle. Retourne 1 si ok. */
int skelscan_model_ty(void *skeleton, float *ty);
int skelscan_set_model_ty(void *skeleton, float ty);
/* Position monde du modèle (translation de la matrice +0x11C). Retourne 1 si ok. */
int skelscan_model_pos(void *skeleton, float out[3]);
/* Matrice monde du modèle (3x4 par lignes) */
int skelscan_model_world(void *skeleton, float out[12]);
/* v1.8.80 : matrice monde (3x4 par lignes) d'un nœud du LOD 0, par son nom exact */
int skelscan_node_world(void *skeleton, const char *node, float out[12]);
/* Position monde d'un nœud du LOD 0, par son nom exact */
int skelscan_node_pos(void *skeleton, const char *node, float out[3]);

/* Journal : vérifie monde = monde(parent) x locale pour chaque os (LOD 0) */
void skelscan_check_locals(void *skeleton);

/* Tourne la matrice locale (+0x68) de l'os d'index bone autour de son axe Z, sur tous les LOD */
int skelscan_rotate_local(void *skeleton, int bone, float angle);
/* Oublie les matrices mémorisées par skelscan_rotate_local (nouvelle chute) */
void skelscan_rotate_reset(void);
/* Nombre de fois où le jeu n'avait pas réécrit une matrice locale entre deux images */
int skelscan_rotate_reused(void);

/* Écrit la liste des squelettes vus et le nombre de mises à jour de chacun */
void skelscan_log_table(void);

/* Remet les compteurs à zéro (nouvelle sortie en piste) */
void skelscan_reset(void);

/*
 * Cherche les positions proches de center (rayon en mètres) dans chaque
 * squelette, et un niveau de pointeurs plus loin.
 * dump = 1 : écrit aussi les 256 premiers octets de chaque squelette.
 */
void skelscan_scan(const float center[3], float radius, int dump, const char *label);

/* Journal : modèles (hors pilote et piste) à moins de radius de center, avec leurs nœuds */
void skelscan_log_near(const float center[3], float radius);
/* Modèles du pilote vus (nom, type, distance) */
void skelscan_log_riders(const float center[3], ScanLogFn log);
/* Position du premier nœud dont le nom contient word (minuscules), dans ces modèles */
int skelscan_find_node(const float center[3], float radius, const char *word, float out[3]);
int skelscan_find_nodes(const float center[3], float radius, const char *word, float out[][3], int max);
/* Modèle dont le chemin finit par suffix (minuscules) : matrice monde 3x4 et chemin */
/* v1.8.4 */
typedef void (*SkelBikeFn)(const char *lower_name, const float world[12], void *user);
int skelscan_each_bike_model(const float center[3], float radius, SkelBikeFn fn, void *user);
/* v1.8.19 : chaque modèle dont le chemin (minuscules, « \\ ») contient sub, à moins de radius */
int skelscan_each_model_sub(const float center[3], float radius, const char *sub, SkelBikeFn fn, void *user);
int skelscan_find_model_sub(const float center[3], float radius, const char *sub, float world[12], char *path,
                            size_t path_size);
int skelscan_find_model(const float center[3], float radius, const char *suffix, float world[12], char *path,
                        size_t path_size);
void skelscan_log_model_nodes(const float center[3], float radius, const char *word, ScanLogFn log);
void skelscan_log_wrists(const float center[3], float radius, ScanLogFn log);
uint8_t *skelscan_node_addr(const float center[3], float radius, const char *model_word, const char *node_word);
void skelscan_log_model_all(const float center[3], float radius, const char *model_word, ScanLogFn log);
void skelscan_find_points(const float center[3], float radius, const float pts[][3], int npts, float tol, ScanLogFn log);

/* Vide toute la hiérarchie du squelette du pilote (rider\\riders\\...\\*.hrc) */
void skelscan_dump_rider(void);

#ifdef __cplusplus
}
#endif

#endif /* MXB_RAGDOLL_SKELSCAN_H */

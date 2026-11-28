/*
 * equip.h - réglage de la position du casque et des bottes (menu sur la touche P)
 *
 * Le casque (helmets\<nom>\helmet.hrc) et les bottes (boots\<nom>\left_boot.hrc,
 * right_boot.hrc) sont des modèles 3D à part, placés par le jeu sur les os du pilote. Juste
 * après leur mise à jour, on décale leurs nœuds (hauteur, avant / arrière). Les réglages sont
 * enregistrés par casque et par bottes (nom du dossier du mod).
 */
#ifndef MXB_RAGDOLL_EQUIP_H
#define MXB_RAGDOLL_EQUIP_H

#include "mxb_api.h"

typedef void (*EquipLogFn)(const char *fmt, ...);

/* Fichier des réglages (chargé tout de suite) */
void equip_init(const char *save_path, EquipLogFn log);
/* Nouvelle sortie en piste : on oublie les modèles vus (les adresses changent) */
void equip_reset(void);
/* v1.8.21 : chaque casque / botte vu (chemin en minuscules, modèle du jeu) */
typedef void (*EquipGearFn)(const char *path, void *model, void *user);
int equip_each_gear(EquipGearFn fn, void *user);
/* Appelé après la mise à jour de chaque modèle 3D (fil de rendu) ; allowed = modifications permises */
void equip_after_update(void *model, int allowed);
/* Position du joueur (pour reconnaître son casque et ses bottes) */
void equip_set_player_pos(const float pos[3]);
/* Modèle du pilote du joueur (NULL : inconnu) : son équipement y reste accroché */
void equip_set_player_model_pos(const float pos[3]);
/* Direction où regarde le pilote (« avant » des réglages) ; up est ignoré */
void equip_set_rider_frame(const float up[3], const float fwd[3]);

/* Menu : touches lues 100 fois par seconde (fil de la télémétrie) */
void equip_menu_keys(int focused, int vk1, int vk2);
/* Page « Touches » du menu : config du plugin (PluginConfig *) et dossier de sauvegarde ;
   relit MXRP_Ragdoll_touches.ini (touches réglées en jeu) */
void equip_set_config(void *cfg, const char *save_path);
int equip_menu_open(void);
/* v1.8.110 : enregistre les touches et options (MXRP_Ragdoll_touches.ini) */
void equip_save_options(void);
/* v1.8.111 : 1 une fois si « Afficher le tutoriel » vient d'être mis sur OUI */
int equip_tutorial_requested(void);
/* Vue 1re personne : casque du joueur caché */
void equip_hide_helmet(int on);
/* tête du pilote du joueur (NULL : inconnue) : son casque est celui dessiné là */
/* 1 : pilote à pied (debout, marche) : les réglages du menu P s'appliquent */
void equip_set_on_foot(int on);
void equip_set_player_head(const float head[3]);
/* Caméra du plugin à pied : casque de la vue embarquée (c_helmet) et caméra de casque cachés */
void equip_hide_cockpit(int on);
/* Modèle de pilote utilisé (chemin du fichier) et sa hauteur réglée dans le menu P, m */
void equip_set_rider_model(const char *path);
/* v1.1.4 : piste en cours (hauteur du pilote réglée par pilote et par piste) */
void equip_set_track(const char *track);
float equip_rider_up(void);
/* Moto du joueur affichée à une autre place (pendant la bascule de la descente, elle reste
   droite à l'écran) : chaque nœud p devient to + M (p - from). on = 0 : affichage normal */
void equip_set_bike_fix(int on, const float M[9], const float from[3], const float to[3]);
/* Dessin du menu ; retourne le nombre de rectangles ajoutés, *ns = textes ajoutés */
int equip_menu_draw(SPluginQuad_t *q, int max_q, SPluginString_t *s, int max_s, int *ns, int font, float aspect);

/* v1.8.79 : os de la tête du joueur (au départ, maintenant ; matrices 3x4 du jeu), n = numéro du départ (0 : aucun) */
void equip_set_head_frames(const float spawn[12], const float now[12], int n);

/* v1.8.80 : os de la tête de notre pilote sur la moto (3x4 du jeu), NULL : inconnu */
void equip_set_ride_head(const float w[12]);

#endif

/*
 * config.h - lecture du fichier MXRP_Ragdoll.ini
 */
#ifndef MXB_RAGDOLL_CONFIG_H
#define MXB_RAGDOLL_CONFIG_H

#include "ragdoll.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    RdConfig physics;

    int   enabled;          /* 0 = plugin inactif */
    int   debug_window;     /* 1 = fenêtre de debug à l'écran pendant la chute */
    float window_x;         /* coin haut gauche, 0..1 */
    float window_y;
    float window_w;         /* taille, 0..1 */
    float window_h;
    float window_scale;     /* mètres visibles en largeur dans la fenêtre */
    float screen_aspect;    /* largeur / hauteur de l'écran (16/9 = 1.778) */
    float hide_delay;       /* secondes d'affichage après le respawn */
    int   log;              /* 1 = écrire MXRP_Ragdoll.log */

    int   skeleton_hook;    /* 1 = intercepte la mise à jour du squelette (phase 2, beta21e) */
    int   skeleton_scan;    /* 1 = pendant une chute, cherche les os du pilote en mémoire */
    float scan_radius;      /* distance max (m) autour de la moto pour retenir une position */
    int   write_test;       /* 1 = tests d'écriture des versions 0.3.x */
    float write_lift;       /* (ancien réglage, gardé pour compatibilité) */
    int   ragdoll_3d;       /* 1 = le ragdoll pilote le pilote 3D du jeu (demande skeleton_hook = 1) */
    float anchor_strength;  /* force qui tire le bassin vers le corps de chute du jeu, 1/s (0 = libre) */
    float body_ground;      /* hauteur du corps de chute du jeu au-dessus du sol, m */
    int   walking;          /* 1 = se relever et marcher après une chute (ZQSD / WASD, Maj, X) */
    float getup_delay;      /* on peut se relever après ce délai même si le corps bouge encore, s */
    float foot_height;      /* cheville au-dessus du sol en marchant, m (plus grand = pieds plus hauts) */
    float rider_lift;        /* pilote affiché plus haut que son squelette, m (pieds du modèle sous la cheville) */
    float head_down;         /* à pied : tête baissée de cet angle, rad (casque orienté comme sur la moto) */
    float stand_lean;        /* à pied : buste penché (< 0 = vers l'arrière), m par m de buste */
    int   replay_record;     /* 1 = enregistre les passages du pilote pour la rediffusion */
    int   replay_play;       /* 1 = les rejoue dans la rediffusion */
    int   walk_camera;      /* 1 = la caméra suit le pilote quand il marche */
    int   bike_lift;        /* 1 = E relève la moto, puis E pour remonter */
    int   pad_respawn;      /* 1 = le plugin appuie sur la croix (haut) de la manette pour remonter */
    int   online_full;      /* 1 = sur un serveur avec « ragdoll » dans le nom : tout comme en solo (moto relevée, descente, remontée) */
    int   online_visual;    /* en ligne : 0 = plugin coupé, 1 = seulement sur les serveurs avec « ragdoll » dans le nom, 2 = toujours (affichage chez soi, moto non touchée) */
    int   net_on;           /* 1 = en ligne, échange la marche / les chutes avec les joueurs qui ont le plugin */
    int   net_host;         /* 1 = ce plugin est l'hôte (les autres mettent son adresse dans reseau_ip) */
    char  net_ip[64];       /* adresse de l'hôte (IP Radmin, ou IP publique si port ouvert) */
    int   net_port;         /* port UDP */
    int   testing_only;     /* 1 = ne modifie le jeu qu'en mode Essai (toujours hors ligne) */
    float camera_distance;  /* distance derrière le pilote, m */
    float camera_height;    /* hauteur de la caméra au-dessus du bassin, m */
    float mouse_sensitivity; /* caméra libre : radians par pixel de souris */
    float fov_fp;            /* v1.8.162 : champ de vision horizontal à pied en 1re personne, degrés ; 0 = celui de la moto en 1re personne */
    int   auto_update;
    int   update_ask;        /* v1.8.175 : 1 = demander avant d'installer une mise à jour */
    int   auto_kick;         /* v1.8.200 : 1 = expulser les joueurs sans plugin de son serveur RAGDOLL (0 de base) */
    int   online_ok;         /* v1.8.204 : 1 = multi activé par le joueur (0 de base : rien n'est partagé en ligne) */
    int   online_asked;      /* v1.8.204 : 1 = question « activer le multi ? » déjà posée */
    int   auto_kick_info;    /* v1.8.200 : messages déjà montrés : 1 premier lancement, 2 premier serveur hébergé */
    int   update_test;       /* v1.8.173 : essai des mises à jour avec les pré-releases (MXRP_Ragdoll.ini seulement) */       /* v1.8.172 : 1 = mises à jour automatiques (release GitHub) */
    int   hold_msgs;         /* v1.8.111 : 1 = messages « garde appuye » avec barre de progression ; 0 = caches */
    int   bike_marker;       /* 1 = flèche au-dessus de la moto pendant qu'on marche */
    float cones_lift;        /* v1.8.51 : cônes au-dessus du sol (m) */
    int   tutorial;          /* v1.8.110 : 1 = fenetre du tutoriel a afficher (premier lancement, ou demandee dans le menu P) */
    int   pad_style;         /* v1.8.107 : boutons affiches : 0 auto (manette detectee), 1 Xbox, 2 PlayStation */
    int   objects_opti;      /* v1.8.92 : 1 = objets (cônes, bottes) optimisés, moins beaux ; 0 = haute qualité */
    int   cones_3d;          /* v1.8.48 : 1 = cônes dessinés en 3D dans la scène (0 = par-dessus l'image) */
    int   test_keys;         /* v1.8.38 : 1 = touches d'essai Ctrl+Haut/Bas/Page (pilotage sans manette) */
    int   fists;             /* 1 = poings fermés quand il marche ou court */
    int   language;          /* langue des textes à l'écran : 0 fr, 1 en, 2 es, 3 de, 4 it */

    /* touches (codes Windows, 2 au plus par action, 0 = aucune) et boutons de manette (XInput) */
    int   key_forward[2], key_back[2], key_left[2], key_right[2];
    int   key_run[2], key_getup[2], key_bike[2], key_dismount[2], key_menu[2], key_view[2], key_jump[2];
    int   key_prio_kbd[2], key_prio_pad[2]; /* bascule : clavier / manette prioritaires pour le plugin à pied */
    int   pad_prio_pad;
    int   kbd_prio, pad_prio;
    int   force_walk;           /* 1 = à pied, le respawn du jeu est bloqué (appui long = respawn sur la piste) */
    float respawn_hold;
    float getup_hold;           /* garder la touche / le stick pour se relever du sol, s */         /* durée de l'appui long, s */               /* au départ : 1 = les touches du plugin ne vont pas au jeu à pied */
    int   pad_getup, pad_bike, pad_dismount, pad_run, pad_view, pad_jump;
    int   key_crouch[2], pad_crouch; /* v1.8.170 : s'accroupir (bascule) */
    int   crouch_seen;               /* v1.8.170 : key_crouch lu dans un fichier (sinon : Maj et RS libérés pour lui) */
    float dismount_hold;     /* garder le bouton de manette pour descendre, s */
    int   crider_arms;       /* 1 = recopie les bras dans c_rider.edf quand on est descendu sans chute */
    int   dismount_fade;     /* 1 = fondu au noir pendant la bascule de la descente */
    int   dismount_crash;    /* 1 = descente : le jeu passe en chute (mains libres) mais le pilote reste debout et la moto est tenue droite */
    int   dismount_fall;     /* 1 = descendre en faisant tomber doucement la moto (bras corrects, respawn pour remonter) */
} PluginConfig;

/* Boutons de manette en plus de XInput : gâchettes */
#define PAD_RT 0x10000
#define PAD_LT 0x20000

/* Nom lisible d'une touche / d'un bouton (pour le journal) */
const char *cfg_key_name(int vk);
const char *cfg_pad_name(int mask);

void cfg_default(PluginConfig *cfg);

/* Applique une ligne "cle = valeur". Retourne 1 si la clé est connue. */
int cfg_apply_line(PluginConfig *cfg, const char *line);

/* Lit un fichier ini. Retourne 1 si le fichier a été ouvert. */
/* max_level : CFG_USER = touches et réseau seulement (MXRP_Ragdoll.ini), CFG_MENU = + options du
   menu P (MXRP_Ragdoll_touches.ini) ; le reste est fixé dans le plugin */
#define CFG_USER 1
#define CFG_MENU 2
int cfg_load(PluginConfig *cfg, const char *path, int max_level);

#ifdef __cplusplus
}
#endif

#endif /* MXB_RAGDOLL_CONFIG_H */

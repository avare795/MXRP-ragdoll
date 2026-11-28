/*
 * net.h - marche et chutes partagées entre les joueurs qui ont le plugin (en ligne)
 *
 * Le réseau du jeu ne transporte pas nos données : le plugin a son propre petit réseau UDP.
 * Par défaut (ni hôte ni adresse) : automatique, chaque plugin se signale sur les réseaux du PC
 * (Radmin VPN, réseau local) et envoie directement aux autres plugins trouvés.
 * Sinon, un des plugins est l'hôte (reseau_hote = 1) ; les autres envoient à son adresse
 * (reseau_ip : IP Radmin VPN, ou IP publique avec le port UDP ouvert). L'hôte renvoie à
 * chacun ce que les autres envoient. Rien ne passe par le serveur du jeu.
 *
 * Ce qui est envoyé, 30 fois par seconde tant qu'un pilote est sous le contrôle du plugin :
 * la position du corps de chute du jeu (pour retrouver son pilote chez les autres), les points
 * du pilote et l'état du pilotage (tête, pieds, mains).
 */
#ifndef MXB_RAGDOLL_NET_H
#define MXB_RAGDOLL_NET_H

#include <stdint.h>
#include "drive.h"
#include "ragdoll.h"

typedef void (*NetLogFn)(const char *fmt, ...);

#define NET_MAX_PEERS 7

/* host = 1 : écoute sur port ; sinon envoie à ip:port. Retourne 1 si démarré */
int net_start(int host, const char *ip, int port, NetLogFn log);
void net_stop(void);
int net_running(void);
/* v1.7.36 : canal du jeu en marche (sinon pas d'expulsion automatique) */
int net_game_ok(void);
/* nom du joueur (journal des autres) */
void net_set_name(const char *name);
/* v1.2 : 1 si le plugin du joueur « name » s'est signalé depuis moins de max_age s */
int net_name_seen(const char *name, double max_age);
/* v1.3 : canal du jeu (paquets du plugin par la connexion du serveur MX Bikes, port 54210) :
   interception à installer au lancement ; activé seulement sur un serveur RAGDOLL */
int net_game_hooks_install(NetLogFn log);
/* v1.7.22 : paquets du plugin par Steam (ISteamNetworking), en ligne sans Radmin */
int net_steam_install(NetLogFn log);
void net_game_channel(int on);

/* Pilote du joueur sous contrôle du plugin (à chaque image ; limité à 30 envois par seconde) */
void net_send_pose(const float model_world[12], const Ragdoll *rd, const DriveSnap *snap);

/* v1.8.65 : petites données du plugin (cônes...) envoyées aux autres plugins (1300 octets au plus) ;
   fn reçoit celles des autres (fil du réseau) */
typedef void (*NetBlobFn)(uint32_t id, int type, const unsigned char *data, int len);
void net_set_blob_fn(NetBlobFn fn);
int net_send_blob(int type, const void *data, int len);
/* v1.8.196 : dernière version d'un paquet d'objets (partie part sur nparts), renvoyée par le fil du réseau
   quand le jeu n'appelle plus le plugin en piste (joueur au stand) ; vidée au changement de piste */
void net_sticky(int type, int part, int nparts, const void *data, int len);
void net_sticky_clear(void);
/* notre numéro sur le réseau du plugin (0 : pas démarré) */
uint32_t net_my_id(void);

/* Pilote d'un autre joueur, interpolé pour l'instant présent. i : 0..NET_MAX_PEERS-1.
   Retourne 1 s'il est sous le contrôle de son plugin en ce moment. */
int net_peer_pose(int i, uint32_t *id, char name[32], float model_world[12], float pos[RD_NUM_POINTS][3],
                  DriveSnap *snap);

#endif

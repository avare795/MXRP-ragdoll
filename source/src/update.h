/*
 * update.h - v1.8.172 : mises à jour automatiques
 *
 * Au lancement du jeu, un fil regarde la dernière release publique
 * (github.com/avare795/MXRP-ragdoll). Si elle contient un plugin plus récent
 * (asset MXRP_Ragdoll_<version>.dlo, empreinte SHA-256 vérifiée), il est
 * téléchargé et remplace celui du dossier plugins : il marche au prochain
 * lancement. Sans droit d'écriture, l'installateur de la release
 * (MXRP_Ragdoll_Installer.exe) est téléchargé et s'ouvre en quittant le jeu.
 */
#ifndef MXB_RAGDOLL_UPDATE_H
#define MXB_RAGDOLL_UPDATE_H

typedef void (*UpdateLogFn)(const char *fmt, ...);

/* lance la vérification (fil à part) ; version = PLUGIN_VERSION */
void update_start(const char *version, UpdateLogFn log);
/* v1.8.173 : essai : aussi les pré-releases (MXRP_Ragdoll.ini : maj_test = 1) */
void update_set_test(int on);
/* v1.8.175 : 1 = demander avant d'installer (la mise à jour est téléchargée puis attend la réponse) */
void update_set_ask(int on);
/* mise à jour téléchargée qui attend la réponse du joueur : 1 et sa version */
int update_pending(char *ver, int n);
/* réponse : 1 installer, 0 plus tard (redemandé au prochain lancement) */
void update_answer(int install);
/* à afficher en jeu (une fois) : 0 rien, 1 installée (prochain lancement),
   2 l'installateur s'ouvrira en quittant le jeu, 3 plus tard ; ver = version de la mise à jour */
int update_take_message(char *ver, int n);
/* à l'arrêt du jeu : ouvre l'installateur téléchargé s'il y en a un */
void update_on_shutdown(void);

#endif

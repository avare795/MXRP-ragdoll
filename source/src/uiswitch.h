#ifndef MXB_RAGDOLL_UISWITCH_H
#define MXB_RAGDOLL_UISWITCH_H
typedef void (*UiLogFn)(const char *fmt, ...);
/* enabled : réglage « plugin actif » (lu dans MXRP_Ragdoll_menu.ini, mis à jour quand on clique) */
/* save_dir : dossier du jeu (global.ini, profils, lus) ; own_dir : nos fichiers (v1.8.106) */
void uiswitch_start(UiLogFn log, volatile int *enabled, const char *save_dir, const char *own_dir);
void uiswitch_stop(void);
/* 1 entre RunInit et RunDeinit (en piste : pas de recherche des menus) */
void uiswitch_on_track(int on);
/* 1 entre EventInit et EventDeinit (la case n'est plus lue : le jeu la remet à zéro) */
void uiswitch_event(int on);
/* v1.7.25 : sur un serveur RAGDOLL le plugin marche même si le bouton a été coupé en solo */
void uiswitch_force(int on);
int uiswitch_button_on(void);
#endif

/* profsave.h - v1.8.105 : copie des profils MX Bikes au lancement (MXRP_Ragdoll_sauvegarde_profils) */
#ifndef MXB_PROFSAVE_H
#define MXB_PROFSAVE_H

typedef void (*ProfsaveLogFn)(const char *fmt, ...);
/* dossier de sauvegarde du jeu (Documents\PiBoSo\MX Bikes) ; travaille dans un fil à part */
void profsave_start(const char *save_path, const char *own_dir, ProfsaveLogFn log);

#endif

/* padstyle.h - v1.8.107 : boutons affichés façon Xbox ou PlayStation (détection de la manette) */
#ifndef MXB_PADSTYLE_H
#define MXB_PADSTYLE_H

typedef void (*PadStyleLogFn)(const char *fmt, ...);

/* détection en fond (toutes les 3 s : manette branchée en cours de partie) */
void padstyle_start(PadStyleLogFn log);
/* réglage du menu P : 0 auto, 1 Xbox, 2 PlayStation */
void padstyle_set_mode(int mode);
int padstyle_is_ps(void);
const char *padstyle_mode_name(int mode);
/* texte à afficher : noms Xbox (A, B, X, Y, LB, RB, LT, RT, LS, RS, BACK, START) remplacés par ceux de
   PlayStation si besoin (mots entiers seulement) ; tampon tournant de 4 */
const char *padstyle_text(const char *in);

#endif

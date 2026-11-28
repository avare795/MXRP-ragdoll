/*
 * uipatch.h - v1.5 : le bouton « Plugin Ragdoll » ajouté aux menus du jeu par le plugin lui-même
 */
#ifndef MXB_UIPATCH_H
#define MXB_UIPATCH_H

typedef void (*UiPatchLogFn)(const char *fmt, ...);

/* Prépare les copies des menus et textes avec le bouton (Documents\PiBoSo\MX Bikes\MXRP_Ragdoll_menus)
   et les donne au jeu quand il ouvre ses menus ; aucun fichier du dossier du jeu n'est modifié.
   Retour : nombre de copies prêtes (0 = menus déjà avec le bouton), -1 = impossible. */
int uipatch_install(const char *save_path, UiPatchLogFn log);
/* journal : combien de menus le jeu a ouvert et combien ont été remplacés */
void uipatch_report(void);
/* police du plugin copiée depuis misc.pkz du jeu (misc/misc.fnt) vers dest ; 1 = faite */
int uipatch_font(const char *dest);
/* v1.7.31 : <dir>.pkz -> fichier temporaire (chemin dans out) ; 1 = trouvé */
#include <stddef.h>
int uipatch_pkz_to_temp(const char *dir, const char *file, char *out, size_t outsz);
/* v1.8.19 : n premiers octets d'un fichier d'une moto (dossier, sinon .pkz) ; 1 = lus */
int uipatch_bike_file_head(const char *dir, const char *file, unsigned char *buf, size_t n);

#endif

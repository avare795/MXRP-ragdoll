/*
 * camhook.h - interception du calcul de la caméra du jeu (MX Bikes beta21e)
 */
#ifndef MXB_RAGDOLL_CAMHOOK_H
#define MXB_RAGDOLL_CAMHOOK_H

#include <stdint.h>

/* CALL qui calcule la matrice caméra -> monde (retour en +0xCFE2) */
#define CAMHOOK_SITE_RVA   0xCFDDu
/* la fonction appelée (produit de matrices 4x4) contient jeu+0x26659F */
#define CAMHOOK_FN_MIN_RVA 0x265E00u
#define CAMHOOK_FN_MAX_RVA 0x26659Fu

typedef void (*CamHookLogFn)(const char *fmt, ...);
/* Appelée juste après le calcul : a et b les deux matrices, out le résultat (4x4 par lignes) */
typedef void (*CamHookFn)(const float *a, const float *b, float *out);

int camhook_install(CamHookFn cb, CamHookLogFn log);
void camhook_uninstall(void);
long long camhook_calls(void);

#endif

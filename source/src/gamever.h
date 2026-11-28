/*
 * gamever.h - v1.8.137 : endroits du jeu utilisés par le plugin, selon la version de mxbikes.exe
 * (table des versions connues, sinon recherche par signature dans le code du jeu)
 */
#ifndef MXB_RAGDOLL_GAMEVER_H
#define MXB_RAGDOLL_GAMEVER_H
#include <stdint.h>

typedef struct {
    uint32_t stamp;                  /* date de compilation de mxbikes.exe */
    uint32_t skel_site, skel_target; /* CALL de la fonction du squelette */
    uint32_t cam_site, cam_min, cam_max; /* CALL du calcul de la caméra */
    uint32_t ak_call, ak_lea, ak_fn, ak_tab; /* bouton Kick (0 : inconnu) */
    int ak_c350, ak_c370, ak_c357;
    int by_sig;                      /* 1 : trouvé par signature (version inconnue) */
} GameAddrs;

/* v1.8.146 : fonctions ODE du jeu pour les maillages de triangles (adresses absolues, 0 : pas trouvées) */
typedef struct {
    uintptr_t data_create;  /* dGeomTriMeshDataCreate() */
    uintptr_t build_single; /* dGeomTriMeshDataBuildSingle(data, sommets, 12, nb, indices, nb, 12) */
    uintptr_t create;       /* dCreateTriMesh(espace, data, 0, 0, 0) */
    uintptr_t destroy;      /* dGeomDestroy (par dSpaceDestroy) */
    uintptr_t data_destroy; /* dGeomTriMeshDataDestroy(data) */
    uintptr_t space_create; /* dSimpleSpaceCreate(parent) */
} GameOde;
const GameOde *gamever_ode(void (*log)(const char *fmt, ...));

/* une fois (résultat gardé) ; log peut être NULL */
const GameAddrs *gamever_get(void (*log)(const char *fmt, ...));

#endif

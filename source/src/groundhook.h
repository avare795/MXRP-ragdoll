/*
 * groundhook.h - v1.8.148 : rampes vues par les roues
 *
 * Les roues du jeu ne touchent pas les objets ODE : elles demandent au jeu la liste des surfaces sous elles
 * (rayon vertical : maillages de la piste rangés dans un arbre, puis le relief). On intercepte l'appel
 * du relief dans cette fonction (trouvé par signature) et on ajoute à la liste le dessus de nos rampes.
 */
#ifndef MXB_GROUNDHOOK_H
#define MXB_GROUNDHOOK_H
#include <stdint.h>

/* fn : dessus de rampe sous (x, z) en coordonnées du jeu ; 1 si une rampe, h et n (normale) remplis */
typedef int (*GroundRampFn)(float x, float z, float *h, float n[3]);
/* une fois ; 1 si installé */
int groundhook_install(GroundRampFn fn, void (*log)(const char *fmt, ...));
/* nombre de surfaces de rampe données aux roues depuis le début */
unsigned long groundhook_hits(void);
unsigned long groundhook_calls(void);

/* v1.8.149 : rampes ajoutées aux maillages de la piste (comme les passerelles) : sommets, triangles et arbre de
   recherche des roues (données de la piste +0x5e0 à +0x640). begin, add (une rampe : dessus seul, coordonnées du
   jeu), commit (remplace les nôtres d'avant ; aucune rampe : piste d'origine remise). */
void groundhook_ramps_begin(void);
int groundhook_ramps_add(const float *v, int nv, const int *ix, int nt);
int groundhook_ramps_commit(void (*log)(const char *fmt, ...));
/* à appeler souvent : mémoire rendue au jeu après quelques secondes */
void groundhook_poll(void);
/* données de la piste vues par les roues (0 : pas encore) */
int groundhook_ready(void);
/* nos rampes plus en place (autre piste chargée) */
int groundhook_stale(void);

/* v1.8.153 : diagnostic : contacts ODE avec nos espaces (rampes, murs) : n contacts vus, rendus par le jeu
   (-1 : refusés, 0 ou 1 : gardés) */
void groundhook_contact_watch(uintptr_t sp_top, uintptr_t sp_wall);
void groundhook_contact_stats(unsigned long out[6]);
/* détail de quelques contacts (puis oubliés) */
void groundhook_contact_log(void (*log)(const char *fmt, ...));

#endif

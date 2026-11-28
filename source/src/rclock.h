/*
 * rclock.h - temps de la rediffusion (s'arrête en pause), trouvé dans la mémoire du jeu
 */
#ifndef MXB_RAGDOLL_RCLOCK_H
#define MXB_RAGDOLL_RCLOCK_H

typedef void (*RclockLogFn)(const char *fmt, ...);

void rclock_init(RclockLogFn log);
/* nouvelle rediffusion : on recommence la recherche */
void rclock_reset(void);
/* à appeler souvent pendant la rediffusion ; 1 quand l'horloge de la rediffusion est connue */
int rclock_tick(double now);
/* temps de la rediffusion, s (0 si pas encore trouvée) */
int rclock_time(double *t);
/* L'horloge trouvée est retenue (fichier) : relue dès la rediffusion suivante, et pendant la
   session (temps enregistré au départ de chaque passage) */
void rclock_set_file(const char *path);
int rclock_live_time(double *t);
/* 1 : vérifié que cette horloge donne le même temps en session et en rediffusion */
int rclock_verified(void);
void rclock_set_verified(void);

#endif

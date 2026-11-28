/*
 * watch.h - diagnostic : point d'arrêt matériel en écriture sur une adresse
 * (registres de débogage du processeur) pour trouver le code du jeu qui écrit
 * sa caméra. Lecture seule pour le jeu : aucun octet de son code n'est modifié.
 */
#ifndef MXB_RAGDOLL_WATCH_H
#define MXB_RAGDOLL_WATCH_H

#include <stdint.h>

typedef void (*WatchLogFn)(const char *fmt, ...);

/* Surveille les écritures de 4 octets à addr dans le fil thread_id */
int watch_start(uintptr_t addr, unsigned long thread_id);
void watch_stop(void);
int watch_hits(void);
/* Écrit dans le journal les endroits du code trouvés (RVA, pile, octets du code) */
void watch_report(WatchLogFn log);

#endif

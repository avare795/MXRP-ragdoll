/*
 * hook.h - interception de la mise à jour du squelette du pilote (Windows)
 *
 * Principe : dans mxbikes.exe, un CALL appelle la fonction qui met à jour
 * le squelette des pilotes. On remplace la cible de ce CALL par un petit
 * trampoline qui saute vers notre fonction ; notre fonction appelle la
 * fonction d'origine puis nous transmet le pointeur du squelette.
 *
 * Adresses valables pour MX Bikes beta21e uniquement (relatives au module).
 */
#ifndef MXB_RAGDOLL_HOOK_H
#define MXB_RAGDOLL_HOOK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* beta21e : CALL de mise à jour du squelette et sa cible attendue */
#define HOOK_SKEL_SITE_RVA   0x221BC3u
#define HOOK_SKEL_TARGET_RVA 0x243220u

/* Appelé après chaque mise à jour de squelette par le jeu */
typedef void (*SkeletonCallback)(void *skeleton);

typedef enum {
    HOOK_OK = 0,
    HOOK_ERR_NOT_CALL,       /* l'octet à l'adresse n'est pas un CALL */
    HOOK_ERR_WRONG_TARGET,   /* le CALL vise autre chose : autre version du jeu ou autre mod */
    HOOK_ERR_READ,           /* mémoire illisible */
    HOOK_ERR_ALLOC,          /* impossible d'allouer le trampoline */
    HOOK_ERR_PROTECT,        /* impossible de modifier le code */
    HOOK_ERR_ALREADY
} HookResult;

/*
 * Vérifie l'appel sans rien modifier. Remplit info avec un résumé lisible
 * (octets lus, cible trouvée).
 */
HookResult hook_check(char *info, size_t info_size);

/* Installe l'interception. */
HookResult hook_install(SkeletonCallback cb, char *info, size_t info_size);

/* Remet le code d'origine. */
void hook_uninstall(void);

/* Fonction appelée AVANT la mise à jour du modèle par le jeu (NULL = aucune) */
void hook_set_pre_callback(SkeletonCallback cb);

int hook_installed(void);

/* Nombre total de mises à jour de squelette interceptées */
uint64_t hook_call_count(void);

/* Écriture mémoire qui ne plante pas sur une adresse invalide. Retourne 1 si tout a été écrit. */
int safe_write(void *addr, const void *data, size_t size);

/* Lecture mémoire qui ne plante pas sur une adresse invalide. Retourne 1 si tout a été lu. */
int safe_read(const void *addr, void *out, size_t size);

/* Adresse de base de mxbikes.exe en mémoire */
uintptr_t hook_module_base(void);

const char *hook_result_text(HookResult r);

#ifdef __cplusplus
}
#endif

#endif /* MXB_RAGDOLL_HOOK_H */

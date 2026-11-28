/*
 * kbhook.h - clavier prioritaire : à pied, les touches du plugin n'arrivent plus au jeu
 */
#ifndef MXB_RAGDOLL_KBHOOK_H
#define MXB_RAGDOLL_KBHOOK_H

typedef void (*KbLogFn)(const char *fmt, ...);

int kbhook_install(KbLogFn log);
void kbhook_uninstall(void);
/* touches du plugin (codes VK) à garder pour lui quand le blocage est actif (jamais Échap) */
void kbhook_set_keys(const int *vks, int n);
/* 1 : ces touches ne vont plus au jeu (pilote à pied) */
void kbhook_block(int on);
/* une touche de plus à garder (0 : aucune), même sans le blocage ci-dessus */
void kbhook_block_extra(int vk);
/* état d'une touche vu par le crochet (aussi pour les touches retenues) */
int kbhook_down(int vk);

#endif

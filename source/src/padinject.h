/*
 * padinject.h - appuyer sur la croix de la manette (haut) à la place du joueur
 */
#ifndef MXB_RAGDOLL_PADINJECT_H
#define MXB_RAGDOLL_PADINJECT_H

typedef void (*PadLogFn)(const char *fmt, ...);

int padinject_install(PadLogFn log);
void padinject_uninstall(void);
/* La manette XInput passe par le plugin d'entrée xinput64.dli : à réessayer s'il n'était pas encore chargé */
int padinject_install_xinput(PadLogFn log);
/* 1 : la croix de la manette est lue « vers le haut » jusqu'à padinject_pov_up(0) */
void padinject_pov_up(int on);
/* v1.8.24 : appuie / relâche un autre bouton du jeu (même codes que padinject_set_reset) */
void padinject_press_extra(int kind, int code, int on);
/* v1.8.33 : gâchette droite forcée à v (0..255, 0 = rien) */
void padinject_force_rt(int v);
/* v1.8.34 : boutons XInput forcés (masque, 0 = rien) */
void padinject_force_buttons(unsigned short m);
/* Touche de respawn à presser (CTRL_RESET du profil) : croix (code 0 haut, 1 droite, 2 bas,
   3 gauche), bouton (numéro DirectInput 0..9) ou touche du clavier (code DirectInput) */
enum { PAD_RESET_POV = 0, PAD_RESET_BUTTON = 1, PAD_RESET_KEY = 2,
       /* v1.8.203 : respawn sur une manette DirectInput (profil « BUTTON » / « POV », pas XInput) */
       PAD_RESET_DIBUTTON = 3, PAD_RESET_DIPOV = 4 };
void padinject_set_reset(int kind, int code);
/* 1 : le jeu ne voit plus la manette (on lit toujours le vrai état avec padinject_read) */
void padinject_block(int on);
/* 1 : le bouton de respawn du joueur (manette) ne va plus au jeu ; le plugin peut toujours l'appuyer */
void padinject_block_reset(int on);
/* ces boutons XInput ne vont plus au jeu (0 : aucun) */
void padinject_block_mask(unsigned short mask);
/* bouton (numéro 0..9 du profil) ou croix (0 haut, 1 droite, 2 bas, 3 gauche) -> masque XInput */
unsigned short padinject_game_mask(int kind, int code);
/* masque XInput du bouton de respawn du profil (0 : pas sur la manette) */
unsigned short padinject_reset_mask(void);
/* le joueur tient son bouton de respawn de manette */
int padinject_reset_held(void);
/* touche de respawn au clavier : code VK (0 si ce n'est pas une touche du clavier) */
int padinject_reset_vk(void);
/* État réel de la manette (lu au passage par le plugin manette du jeu) */
typedef struct {
    unsigned short buttons;   /* XINPUT_GAMEPAD_* */
    float lx, ly;             /* stick gauche, -1..1 */
    float rx, ry;             /* stick droit, -1..1 */
    float lt, rt;             /* gâchettes, 0..1 */
} PadState;
int padinject_read(PadState *out);

/* 1 si le jeu a déjà lu une manette */
int padinject_pad_seen(void);
/* v1.3.3 : lectures de la manette où le bouton de respawn a été ajouté depuis le dernier appel
   (DirectInput, XInput, manette simulée) */
void padinject_inject_counts(long *di, long *x, long *fake);

#endif

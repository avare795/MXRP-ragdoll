/*
 * walk.h - se relever et marcher après une chute
 *
 * Le marcheur ne simule pas de physique : il calcule, à chaque image, la
 * position des 16 points du ragdoll (pose debout, cycle de marche, bras qui
 * se balancent). drive.c transforme ensuite ces points en rotations d'os,
 * exactement comme pour la chute.
 *
 * Les longueurs (cuisse, tibia, bras...) sont mesurées sur le ragdoll au
 * moment où le pilote se relève : ce sont celles du vrai squelette.
 */
#ifndef MXB_RAGDOLL_WALK_H
#define MXB_RAGDOLL_WALK_H

#include "ragdoll.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int forward, back, left, right; /* touches de déplacement */
    int run;                        /* trottiner */
    int steer;                      /* 1 : aller vers steer_heading (déplacement relatif à la caméra) */
    float steer_heading;            /* cap voulu, radians (même convention que Walker.heading) */
    int steer_back;                 /* 1 : il recule vers steer_heading en regardant à l'opposé */
    int jump;                       /* touche de saut tenue */
    int crouch;                     /* v1.8.170 : accroupi (bascule gérée par le plugin) */
} WalkInput;

typedef enum {
    WALK_OFF = 0,
    WALK_GETTING_UP,   /* transition du corps au sol vers la pose debout */
    WALK_ON            /* debout, contrôlé au clavier */
} WalkState;

typedef struct {
    WalkState state;
    float t;                       /* temps dans l'état courant, s */

    /* dimensions mesurées */
    float thigh, shin, upper_arm, forearm;
    float spine, neck, head;       /* bassin-torse, torse-cou, cou-tête */
    float hip_half, shoulder_half; /* demi-largeurs */
    float shoulder_drop;           /* épaules sous le cou */
    float hip_height;              /* hauteur des hanches debout au-dessus des chevilles */

    /* position et allure */
    float pos[2];                  /* x, z du bassin */
    float heading;                 /* direction regardée, radians (0 = +z) */
    float speed;                   /* m/s, signé (négatif = recule) */
    float phase;                   /* cycle de marche, radians */

    float from[RD_NUM_POINTS][3];  /* pose au sol au début de la relève */
    float ankle_height;            /* cheville au-dessus du sol, m (bottes comprises) */
    int mirror;                    /* 1 si la droite du pilote est à gauche de notre repère */

    /* geste avec la moto : mains au guidon, accroupi, jambe levée pour l'enjamber */
    float grab;                    /* 0 = bras libres, 1 = mains sur grab_hand */
    float carry;                   /* v1.8.63 : mains sur grab_hand sans se pencher (il porte un cône) */
    float grab_hand[2][3];         /* gauche, droite (monde) */
    float crouch;                  /* descente du bassin, m */
    float leg_lift;                /* 0..1 : la jambe leg_side passe par-dessus la selle */
    int leg_side;                  /* 0 = gauche, 1 = droite */
    float leg_target[3];           /* où va ce pied quand leg_lift = 1 */
    float leg_clear;               /* hauteur à franchir par le pied (dessus de la selle), m monde */
    float sit;                     /* 0..1 : assis sur la selle (bassin à sit_y, pieds écartés) */
    float sit_y;                   /* hauteur du bassin assis, m monde */
    float rise;                    /* bassin plus haut (sur la pointe du pied d'appui), m */
    float support;                 /* hauteur du sol sous le pilote à l'image d'avant (objets compris) */
    int support_ok;
    int air;                       /* en l'air (saut ou chute d'une hauteur) */
    float air_y, vy;               /* hauteur des pieds en l'air, vitesse verticale (m/s) */
    float dip;                     /* bassin plus bas : jambes pliées (en l'air, à la réception) */
    int jump_prev;
    float airw, land, land_hold, prejump, air_t;
    /* v1.8.15 : relevé naturel : sur le dos, il se retourne d'abord ; puis à quatre pattes, à genou, debout */
    int getup_roll;
    float roll_axis[3], roll_c[3], getup_T;
    int air_jump;
    float air_tgt, land_tgt, dip_tgt, airw_v, land_v, dip_v, risew, rise_v, push_dip; /* cibles et vitesses (lissage) */ /* poids des poses en l'air / réception, élan */
    float lean;                    /* buste penché en plus vers l'avant (sur le réservoir) */
    float stand_lean;              /* buste debout : < 0 = vers l'arrière */
    float pegs;                    /* 0..1 : les pieds montent sur les repose-pieds */
    float peg[2][3];               /* repose-pieds gauche, droit (monde, position de la cheville) */
    int calibrated;
    float squat, squat_v;          /* v1.8.170 : 0..1 accroupi (bassin bas, genoux pliés) */
} Walker;

/* Inclinaison des pieds de la dernière pose (+ = pointe vers le bas), pour les chevilles */
void walk_foot_pitch(float out[2]);

/* Réglages */
#define WALK_SPEED   1.4f   /* m/s */
#define RUN_SPEED    4.2f
#define BACK_SPEED   0.8f
#define CROUCH_SPEED 0.65f  /* v1.8.170 : en marchant accroupi */
#define TURN_RATE    2.4f   /* rad/s */
#define GETUP_TIME   1.5f   /* s : au sol -> à genou -> debout (avant la v1.8.15) */
#define ANKLE_HEIGHT 0.14f  /* cheville au-dessus du sol par défaut, m (bottes de cross) */

/*
 * Sur la moto, les genoux pointent vers l'avant : à appeler au début de la chute
 * pour savoir de quel côté est la droite du pilote dans le repère du jeu.
 */
void walk_calibrate(Walker *w, const Ragdoll *rd);

/* Commence à se relever à partir de la pose actuelle du ragdoll */
void walk_begin(Walker *w, const Ragdoll *rd);

/*
 * Avance d'un pas de temps : lit les touches, déplace le pilote et écrit la
 * pose dans rd->pos (rd->prev suit, vitesse nulle). Le sol est celui du ragdoll.
 */
void walk_update(Walker *w, Ragdoll *rd, const WalkInput *in, float dt);

/* Hauteur du sol à la position (x, z) : le relief si disponible, sinon le sol du ragdoll */
float walk_ground_height(const Ragdoll *rd, float x, float z);

/* Fonction de relief : retourne 1 et la hauteur si (x, z) est sur le relief connu */
typedef int (*WalkGroundFn)(float x, float z, float *h);
void walk_set_ground_fn(WalkGroundFn fn);
/* objets de la piste : surface la plus haute sous (x, z) qui ne dépasse pas ymax */
typedef int (*WalkObjectFn)(float x, float z, float ymax, float *h);
void walk_set_object_fn(WalkObjectFn fn);
/* objets : un pilote en (x, z), pieds à feet, rayon r, bute-t-il ? */
typedef int (*WalkBlockFn)(float x, float z, float feet, float r);
void walk_set_block_fn(WalkBlockFn fn);
/* repère de hauteur pour le premier sol (descente de moto : la moto, posée sur le ponton) */
void walk_set_start_ref(float y);
float walk_support_y(const Walker *w);

#ifdef __cplusplus
}
#endif

#endif /* MXB_RAGDOLL_WALK_H */

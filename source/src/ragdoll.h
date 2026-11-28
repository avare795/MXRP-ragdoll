/*
 * ragdoll.h - moteur physique du ragdoll (portable, sans dépendance au jeu)
 *
 * Le corps est un ensemble de points (articulations) reliés par des
 * contraintes de distance (os). Intégration de Verlet à pas fixe,
 * résolution itérative des contraintes, collision avec un sol plan.
 *
 * Repère : Y vers le haut, comme dans MX Bikes.
 */
#ifndef MXB_RAGDOLL_H
#define MXB_RAGDOLL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Articulations du pilote */
enum {
    RD_HEAD = 0,
    RD_NECK,
    RD_CHEST,
    RD_PELVIS,
    RD_SHOULDER_L, RD_ELBOW_L, RD_HAND_L,
    RD_SHOULDER_R, RD_ELBOW_R, RD_HAND_R,
    RD_HIP_L, RD_KNEE_L, RD_FOOT_L,
    RD_HIP_R, RD_KNEE_R, RD_FOOT_R,
    RD_NUM_POINTS
};

#define RD_MAX_CONSTRAINTS 64

typedef enum {
    RD_CONSTRAINT_FIXED = 0, /* longueur fixe (os) */
    RD_CONSTRAINT_MIN        /* distance minimale (limite d'articulation) */
} RdConstraintType;

typedef struct {
    int a, b;
    float length;
    RdConstraintType type;
    int visible;             /* dessiné dans la fenêtre de debug */
} RdConstraint;

typedef struct {
    float gravity;           /* m/s², positif vers le bas */
    float damping;           /* 0..1, perte de vitesse par seconde */
    float ground_friction;   /* 0..1, freinage horizontal au contact du sol */
    float ground_bounce;     /* 0..1, rebond vertical */
    float point_radius;      /* rayon des articulations, m */
    float ground_offset;     /* hauteur du sol par rapport au point de référence du châssis, m */
    float substep;           /* pas de simulation, s */
    int   iterations;        /* itérations de résolution des contraintes */
    float rest_speed;        /* vitesse max (m/s) sous laquelle le corps est considéré immobile */
    float rest_time;         /* durée (s) d'immobilité avant l'état "au repos" */
    int   rot_transpose;     /* 1 = utiliser la transposée de la matrice de rotation du jeu */
} RdConfig;

typedef struct {
    RdConfig cfg;

    float pos[RD_NUM_POINTS][3];
    float prev[RD_NUM_POINTS][3];
    float inv_mass[RD_NUM_POINTS];

    RdConstraint constraints[RD_MAX_CONSTRAINTS];
    int num_constraints;

    float ground_y;          /* sol : dot(ground_n, p) = ground_y (hauteur si sol horizontal) */
    float ground_n[3];       /* normale du sol, unitaire ; (0, 1, 0) = horizontal */
    float accumulator;
    float time;              /* temps écoulé depuis le début de la chute, s */
    float still_time;        /* temps passé immobile, s */
    int   active;
    int   resting;
    float max_impact;        /* plus grosse vitesse d'impact au sol, m/s */
    float head_impact;       /* plus grosse vitesse d'impact de la tête, m/s */
    float hinge_sign;        /* sens des genoux selon le repère du squelette (+1 / -1), pris au départ */
    float h0;                /* vitesse horizontale au départ de la chute (-1 : pas encore lue) */
    float guard_speed;       /* vitesse du centre au pas précédent (garde-fou d'énergie) */
    float hip_sign;
    float arm_sign;
    /* relief réel (facultatif) : hauteur et normale du sol sous (x, z). Chaque point touche
       alors le sol sous lui, au lieu d'un seul plan pris sous le bassin. */
    int (*ground_fn)(float x, float z, float *h, float n[3]);
    /* objets de la piste (facultatif) : déplacement pour sortir la sphère (p, r) d'un objet */
    int (*obstacle_fn)(const float p[3], float r, float push[3]);
    float pt_n[RD_NUM_POINTS][3];
    float pt_d[RD_NUM_POINTS];
    /* v1.8.23 : épaisseur en plus contre le sol, par point (casque, bottes : réglée par le plugin) */
    float gextra[RD_NUM_POINTS];
    /* v1.8.23 : sol sous le milieu des membres (relief réel seulement) */
    float seg_n[10][3];
    float seg_d[10];
    int seg_on;          /* sens de « l'avant » du torse (mains vers l'avant au départ) */          /* sens de « l'avant » du bassin (cuisses vers l'avant au départ) */
} Ragdoll;

/* Valeurs par défaut de la configuration */
void rd_config_default(RdConfig *cfg);
/* v1.8.23 : rayon du point i contre le sol (sans gextra) */
float rd_ground_radius(const Ragdoll *rd, int i);

/* Prépare le ragdoll (os, masses). N'active pas la simulation. */
void rd_init(Ragdoll *rd, const RdConfig *cfg);

/*
 * Lance la chute : place le pilote en position assise sur la moto.
 *  bike_pos : position du point de référence du châssis (monde)
 *  bike_rot : matrice de rotation du châssis (m_aafRot du jeu)
 *  bike_vel : vitesse du centre de gravité (monde), m/s
 */
void rd_spawn(Ragdoll *rd, const float bike_pos[3], const float bike_rot[3][3],
              const float bike_vel[3]);

/*
 * Lance la chute à partir de positions réelles (par exemple les os du pilote
 * du jeu). Les longueurs des os sont mesurées sur ces positions.
 */
void rd_spawn_points(Ragdoll *rd, const float points[RD_NUM_POINTS][3], const float vel[3],
                     float ground_y);

/* Avance la simulation de dt secondes (découpé en sous-pas fixes). */
void rd_step(Ragdoll *rd, float dt);

/* Arrête la simulation (respawn du pilote). */
void rd_stop(Ragdoll *rd);

/* Vitesse la plus élevée parmi les articulations, m/s */
float rd_max_speed(const Ragdoll *rd);

/* Plus grand écart relatif entre longueur actuelle et longueur des os (0.01 = 1 %) */
float rd_max_bone_error(const Ragdoll *rd);

/* Centre de masse */
void rd_center(const Ragdoll *rd, float out[3]);

/* Nom lisible d'une articulation */
const char *rd_point_name(int index);

/* hauteur du point dont le sol est demandé (pendant ground_fn) : pour les objets de la piste */
extern float ragdoll_query_y;

#ifdef __cplusplus
}
#endif

#endif /* MXB_RAGDOLL_H */

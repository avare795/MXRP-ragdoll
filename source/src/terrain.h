/*
 * terrain.h - relief de la piste, lu dans la mémoire du jeu
 *
 * MX Bikes utilise le moteur physique ODE (open source, simple précision).
 * Le relief est un "heightfield" ODE : une grille de hauteurs. Sa structure
 * de données (dxHeightfieldData) est publique et très reconnaissable :
 *
 *   float largeur, profondeur, pas_x, pas_z, rapport, 1/pas_x, 1/pas_z,
 *         demi_largeur, demi_profondeur, h_min, h_max, épaisseur, échelle, décalage
 *   int   nb_x, nb_z, copie, bouclage, type (1 = octets, 2 = short, 3 = float)
 *   void *données
 *
 * On la retrouve en parcourant la mémoire, puis on retrouve l'objet ODE qui la
 * référence pour connaître sa position (sa boîte englobante, AABB).
 * Aucune adresse codée en dur : ça devrait survivre aux mises à jour du jeu.
 */
#ifndef MXB_RAGDOLL_TERRAIN_H
#define MXB_RAGDOLL_TERRAIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERRAIN_MAX 8

typedef struct {
    float width, depth;        /* taille, m */
    float sample_w, sample_d;  /* pas de la grille, m */
    float half_w, half_d;
    float min_h, max_h;
    float scale, offset;       /* hauteur = valeur x scale + offset */
    int   nx, nz;              /* nombre de points */
    int   mode;                /* 1 = octet, 2 = short, 3 = float */
    const void *data;
    float center[3];           /* position du heightfield dans le monde */
    int   placed;              /* 1 si la position a été retrouvée (sinon centre = 0) */
    int   owned;               /* 1 si data est une copie à nous (malloc) */
    const void *live;          /* grille du jeu (modifiée par la déformation du terrain) ; NULL : inconnue */
    uintptr_t hdr;             /* v1.8.120 : structure ODE du relief dans le jeu (0 : inconnue) */
    int hdr_new;               /* v1.8.120 : 1 = disposition avec « rapport » (hauteur max à +40), 0 : +36 */
} TerrainField;

typedef void (*TerrainLogFn)(const char *fmt, ...);

/* Parcourt la mémoire du jeu (lent : à lancer dans un fil séparé). Retourne le nombre trouvé. */
int terrain_scan(TerrainLogFn log);

/* Lance terrain_scan dans un fil séparé (Windows). */
void terrain_scan_async(TerrainLogFn log);

/* 1 quand un relief est disponible */
int terrain_ready(void);

/*
 * Hauteur du relief en (x, z). Retourne 1 si le point est sur un heightfield.
 * Si normal n'est pas NULL, y met la normale (unitaire).
 */
int terrain_height(float x, float z, float *h, float normal[3]);

/* Hauteur ACTUELLE du relief, lue dans la grille du jeu (ornières et bosses de la déformation
   comprises). 0 si la grille du jeu n'est pas connue. */
int terrain_height_live(float x, float z, float *h);
/* v1.8.125 : pareil, sans les rampes écrites dans la grille (sol d'origine dessous) */
int terrain_height_live_base(float x, float z, float *h);
int terrain_live_ready(void);
/* v1.8.120 : rampes FMX dans le relief du jeu (les roues roulent dessus comme sur la terre).
   begin : rien de voulu ; raise : rectangle (bas au milieu c, axes ex largeur et ey longueur, demi-largeur W,
   longueur L) relevé à c.y + prof(ly) (np + 1 points sur L), hauteurs du monde du JEU ; end : écrit dans la
   grille du jeu (et remet le sol d'origine là où plus rien n'est voulu). Rend 1 si la hauteur max du relief
   a été augmentée (l'objet ODE du relief doit être marqué déplacé). */
void terrain_raise_begin(void);
void terrain_raise(const float c[3], const float ex[3], const float ey[3], float W, float L, const float *prof, int np,
                   float sx, float sy); /* v1.8.124 : pente (m par m) en largeur, en longueur */
int terrain_raise_end(void (*log)(const char *fmt, ...));
/* v1.8.121 : à chaque appel : remet la hauteur des rampes là où le jeu l'a rabaissée (1 : hauteur max changée) */
int terrain_raise_keep(void (*log)(const char *fmt, ...));
/* points remis depuis le dernier appel */
int terrain_raise_resets(void);
/* v1.8.141 : hauteur max (coordonnées du jeu) que les roues peuvent voir (1e9 : pas de limite) */
float terrain_grid_top(void);
void terrain_raise_diag(void (*log)(const char *fmt, ...));
void terrain_raise_probe(float x, float z, char *out, size_t n);
/* v1.8.129 : position de notre moto (ornières recopiées autour d'elle à chaque appel) */
void terrain_raise_set_bike(float x, float z, int ok);
/* v1.8.138 : fonctions du module objets (objet ODE du 2e relief) */
void terrain_set_geom_fns(int (*dataoff)(uintptr_t), int (*dirty)(uintptr_t));
/* Relief chargé depuis le cache : retrouve en arrière-plan la grille du jeu (pour la hauteur actuelle) */
void terrain_find_live_async(TerrainLogFn log);
/* oublie la grille du jeu (à refaire à chaque session) */
void terrain_live_reset(void);

/* Objet ODE (geom) du relief retrouvé : adresse du champ qui pointe vers ses données et de sa
   boîte englobante (pour retrouver les objets de la piste). 0 si inconnu. */
int terrain_geom_info(uintptr_t *ptrloc, uintptr_t *aabb);

/* Ajoute un heightfield à la main (tests) */
void terrain_add(const TerrainField *f);
void terrain_clear(void);

/* Taille de la grille en octets */
size_t terrain_data_size(const TerrainField *f);

/*
 * Cache sur le disque : sauvegarde le relief actuel avec son recalage, et le
 * recharge instantanément au prochain passage sur la même piste.
 */
int terrain_save(const char *path, float calib);
int terrain_load(const char *path, float *calib);

/* 1 pendant qu'une recherche en mémoire tourne */
int terrain_scanning(void);

/* Diagnostic caméra : cherche en mémoire la position et la vue de la caméra du jeu */
void mem_find_camera_async(const float pos[3], const float col0[3], const void *stack, TerrainLogFn log);

/* Diagnostic : cherche le corps physique ODE de la moto (rotation de la télémétrie) */
void mem_find_bike_body_async(const float rot[9], const float pos[3], const float vel[3], TerrainLogFn log);

/* Début du dxBody ODE du châssis de la moto trouvé (0 si aucun) */
uintptr_t mem_bike_body(void);
int mem_bike_searching(void);
/* Revérifie le dernier châssis trouvé près de pos ; 1 si encore bon (instantané) */
int mem_bike_revalidate(const float pos[3]);
/* châssis retrouvé autrement (parmi les pièces déjà connues) */
void mem_set_bike_body(uintptr_t b);

/* Tous les corps du même monde ODE que le châssis (trouvés avec lui) */
int mem_world_bodies(uintptr_t *out, int max);

/* Adresse de la matrice caméra -> monde du jeu trouvée (0 si aucune) */
uintptr_t mem_camera_candidate(void);

/* 1 si m (17 flottants) ressemble à la caméra du jeu : 3x4 orthonormée, 0 0 0 1, angle de vue */
int camera_matrix_ok(const float *m);

/* Pour les tests : reconnaît une structure dxHeightfieldData à cette adresse */
int terrain_parse(const void *p, TerrainField *out);

#ifdef __cplusplus
}
#endif

#endif /* MXB_RAGDOLL_TERRAIN_H */

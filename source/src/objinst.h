/*
 * objinst.h - v1.8.97 : objets dessinés par modèle (cônes, bottes de foin)
 *
 * Chaque modèle est envoyé une seule fois à la carte graphique (liste d'affichage) ; un objet n'est
 * plus qu'une matrice (position, rotation, taille) et une texture.
 */
#ifndef MXB_OBJINST_H
#define MXB_OBJINST_H

/* modèles */
enum { OBJM_CONE = 0, OBJM_BALE_ROUND, OBJM_BALE_ROUND_LOW, OBJM_BALE_SMALL, OBJM_RAMP0, OBJM_RAMP1, OBJM_RAMP2, OBJM_RAMP3,
       OBJM_RAMP4, OBJM_RAMP5, OBJM_RAMP6, OBJM_RAMP7, OBJM_RAMP8, OBJM_RAMP9,
       OBJM_FURN0, OBJM_FURN1, OBJM_FURN2, OBJM_COUNT }; /* v1.8.178 : mobilier */ /* v1.8.123 : 9 formes */ /* v1.8.119 : rampes */

typedef struct {
    int model;   /* OBJM_... */
    int tex;     /* numéro de texture de la scène (glcam) */
    float m[16]; /* matrice OpenGL (colonnes) : modèle -> monde */
} ObjInst;

/* matrice colonnes (axes x, y, z du modèle, déjà mis à l'échelle) + origine */
static inline void objinst_matrix(float m[16], const float x[3], const float y[3], const float z[3], const float o[3])
{
    int k;
    for (k = 0; k < 3; ++k) {
        m[k] = x[k];
        m[4 + k] = y[k];
        m[8 + k] = z[k];
        m[12 + k] = o[k];
    }
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
}

#endif

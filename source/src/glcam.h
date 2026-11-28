/*
 * glcam.h - caméra : interception des appels OpenGL de matrices du jeu
 *
 * MX Bikes dessine avec l'OpenGL "classique" (glMatrixMode, glLoadMatrixf,
 * glMultMatrixf, glFrustum...). Ces fonctions sont dans la table d'imports de
 * mxbikes.exe (non chiffrée par la DRM Steam) : on remplace les pointeurs de
 * cette table par les nôtres, et on appelle ensuite la fonction d'origine (ou
 * celle d'un autre mod qui l'a interceptée avant nous).
 *
 * v0.8.0 : diagnostic seulement. glcam_request_capture() enregistre tous les
 * appels de matrices d'une image complète, pour savoir où est la caméra.
 */
#ifndef MXB_RAGDOLL_GLCAM_H
#define MXB_RAGDOLL_GLCAM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GlcamLogFn)(const char *fmt, ...);

/* Installe les interceptions. Retourne le nombre de fonctions interceptées. */
int glcam_install(GlcamLogFn log);
void glcam_uninstall(void);

/*
 * Remplacement de la caméra. Au début de chaque passe de rendu en perspective,
 * le jeu charge sa matrice de vue ; fn la reçoit (old, colonnes OpenGL) et
 * écrit la nôtre dans out. Si fn retourne 1, la vue est remplacée et tous les
 * objets de la passe sont corrigés (out x inverse(old) x matrice du jeu).
 * fn est appelée dans le fil de rendu. NULL = caméra du jeu.
 */
typedef int (*GlcamViewFn)(const float old_view[16], float out_view[16]);
void glcam_set_view_fn(GlcamViewFn fn);
/* v1.8.162 : champ de vision : hfov en degrés (0 = celui du jeu) puis zoom (1 = aucun ; < 1 rapproche) ;
   appliqué aux perspectives du jeu tant qu'il est demandé */
void glcam_set_fov(float hfov_deg, float zoom);
/* champ de vision horizontal du jeu (degrés, 0 : pas encore vu) */
float glcam_game_hfov(void);
/* v1.8.165 : champ de vision du jeu en roulant en 1re personne (relevé par le plugin ; 0 : pas encore) */
void glcam_note_ride_fov(float f);
float glcam_ride_fov(void);
/* v1.8.166 : largeur / hauteur de l'image vue */
float glcam_aspect(void);
/* v1.8.164 : diagnostic : passes de la dernière image */
void glcam_fov_log(void (*log)(const char *fmt, ...));

/* Appelée à la fin de chaque image (0, après SwapBuffers) et au premier dessin (1) */
typedef void (*GlcamFrameFn)(int where);
void glcam_set_frame_fn(GlcamFrameFn fn);
/* v1.4 : bouton dessiné par-dessus les menus du jeu. La fonction remplit ov et rend 1 pour le
   dessiner (appelée à chaque image, depuis le fil de dessin du jeu) */
typedef struct {
    int visible, on, hover;
    float rect[4]; /* x0 y0 x1 y1, 0..1 dans la fenêtre du jeu */
    char text[64];
} GlcamOverlay;
typedef int (*GlcamOverlayFn)(GlcamOverlay *ov);
void glcam_set_overlay_fn(GlcamOverlayFn fn);

/* v1.8.48 : triangles du plugin dessinés en 3D dans la passe en perspective, cachés par les objets
   du jeu. fn remplit n sommets (3 par triangle) : position monde, coordonnées de texture,
   couleur 0x00BBGGRR (multipliée par la texture s'il y en a une). Rend n (<= max_verts). */
typedef int (*GlcamSceneFn)(float (*pos)[3], float (*uv)[2], unsigned long *col, unsigned char *tex, int max_verts);
void glcam_set_scene_fn(GlcamSceneFn fn);
/* v1.8.91 : caméra de la passe en cours (position, regard) ; 0 si inconnue */
int glcam_scene_eye(float eye[3], float fwd[3]);
/* v1.8.97 : objets par modèle : modèle id (sommets : x, y, z, normale, u, v ; stride nombres par
   sommet, données gardées), et fonction qui donne les objets à dessiner (remplace la fonction de
   sommets quand la carte accepte les listes d'affichage) */
#include "objinst.h"
typedef int (*GlcamInstFn)(ObjInst *out, int max);
void glcam_set_model(int id, const float *v, int stride, int nverts);
void glcam_set_inst_fn(GlcamInstFn fn);
/* v1.8.92 : 1 = dessin rapide (objets optimisés) */
void glcam_set_scene_fast(int on);
/* 1 : il y a quelque chose à dessiner (sinon rien n'est vérifié pendant le rendu) */
void glcam_set_scene_active(int on);
/* texture RGB numéro slot (0..15) (w x h, la mémoire doit rester), envoyée une fois à OpenGL ;
   tex[] de chaque sommet donne le numéro */
void glcam_set_scene_texture(int slot, const unsigned char *rgb, int w, int h);
/* 1 si des triangles ont été dessinés il y a moins de 0,5 s */
int glcam_scene_recent(void);
/* diagnostic depuis l'appel précédent (remis à zéro) */
int glcam_scene_stats(char *buf, int n);
/* v1.8.98 : passes en perspective de la dernière image et temps de dessin de nos objets (remis à zéro) */
int glcam_pass_info(char *buf, int n);

/* Identifiant du fil de rendu (0 tant qu'aucune image n'est finie) */
unsigned long glcam_render_thread(void);

/* Dernière vue chargée par le jeu (avant remplacement) et l'adresse d'où il l'a lue */
int glcam_game_view(float out[16], const void **ptr);
/* glFrustum (l, r, b, t, n, f) de la passe principale ; 0 si pas encore vue */
int glcam_frustum(float out[6]);

/* Demande l'enregistrement de la prochaine image complète */
void glcam_request_capture(const char *why);

/* Si une image a été enregistrée, l'écrit dans le journal (à appeler hors rendu) */
void glcam_flush(GlcamLogFn log);

#ifdef __cplusplus
}
#endif

#endif

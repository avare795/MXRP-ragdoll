/*
 * test_ragdoll.c - tests du moteur physique (se lancent sur PC, sans le jeu)
 *
 *   make test
 */
#include "../src/config.h"
#include "../src/ragdoll.h"
#include "../src/lang.h"

#include <math.h>
#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (cond) {                                          \
            printf("  ok    ");                              \
        } else {                                             \
            printf("  ECHEC ");                              \
            ++g_failures;                                    \
        }                                                    \
        printf(__VA_ARGS__);                                 \
        printf("\n");                                        \
    } while (0)

static const float k_identity[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

static int all_finite(const Ragdoll *rd)
{
    int i, k;
    for (i = 0; i < RD_NUM_POINTS; ++i)
        for (k = 0; k < 3; ++k)
            if (!isfinite(rd->pos[i][k]))
                return 0;
    return 1;
}

static float lowest_point(const Ragdoll *rd)
{
    int i;
    float low = rd->pos[0][1];
    for (i = 1; i < RD_NUM_POINTS; ++i)
        if (rd->pos[i][1] < low)
            low = rd->pos[i][1];
    return low;
}

/* Simule jusqu'au repos (ou timeout) avec des pas de 10 ms comme le jeu */
static float run_until_rest(Ragdoll *rd, float timeout, float *worst_bone)
{
    float t = 0.0f;
    *worst_bone = 0.0f;
    while (t < timeout && !rd->resting) {
        float err;
        rd_step(rd, 0.01f);
        t += 0.01f;
        err = rd_max_bone_error(rd);
        if (err > *worst_bone)
            *worst_bone = err;
    }
    return t;
}

static void test_chute_a_l_arret(void)
{
    RdConfig cfg;
    Ragdoll rd;
    const float pos[3] = { 0.0f, 0.55f, 0.0f };
    const float vel[3] = { 0.0f, 0.0f, 0.0f };
    float worst, t;

    printf("Chute a l'arret (moto immobile)\n");
    rd_config_default(&cfg);
    rd_init(&rd, &cfg);
    rd_spawn(&rd, pos, k_identity, vel);
    CHECK(fabsf(rd.ground_y) < 1e-4f, "sol a y=0 (%.3f)", rd.ground_y);

    t = run_until_rest(&rd, 10.0f, &worst);
    CHECK(all_finite(&rd), "aucune valeur NaN / infinie");
    CHECK(rd.resting, "le pilote s'immobilise (%.2f s)", t);
    /* v0.41.20 : épaisseur par partie, la plus fine (botte) = 6/8 de point_radius */
    CHECK(lowest_point(&rd) >= rd.ground_y + cfg.point_radius * 0.75f - 1e-3f,
          "aucun point sous le sol (min %.3f)", lowest_point(&rd));
    CHECK(worst < 0.05f, "les os gardent leur longueur (ecart max %.1f %%)", worst * 100.0f);
}

static void test_chute_a_haute_vitesse(void)
{
    RdConfig cfg;
    Ragdoll rd;
    const float pos[3] = { 10.0f, 2.55f, -5.0f }; /* 2 m au-dessus du sol */
    const float vel[3] = { 0.0f, 3.0f, 20.0f };   /* 72 km/h vers l'avant, en l'air */
    float start[3], end[3], worst, t, dx, dz, distance;

    printf("Chute a 72 km/h en l'air\n");
    rd_config_default(&cfg);
    cfg.ground_offset = -2.55f;
    rd_init(&rd, &cfg);
    rd_spawn(&rd, pos, k_identity, vel);
    rd_center(&rd, start);

    t = run_until_rest(&rd, 20.0f, &worst);
    rd_center(&rd, end);
    dx = end[0] - start[0];
    dz = end[2] - start[2];
    distance = sqrtf(dx * dx + dz * dz);

    CHECK(all_finite(&rd), "aucune valeur NaN / infinie");
    CHECK(rd.resting, "le pilote s'immobilise (%.2f s)", t);
    CHECK(distance > 10.0f && distance < 80.0f,
          "glisse sur une distance realiste (%.1f m)", distance);
    CHECK(dz > 0.0f, "part dans le sens de la moto");
    CHECK(rd.max_impact > 3.0f, "un impact au sol est detecte (%.1f m/s)", rd.max_impact);
    CHECK(worst < 0.15f, "les os tiennent pendant l'impact (ecart max %.1f %%)", worst * 100.0f);
    CHECK(rd_max_bone_error(&rd) < 0.03f, "os corrects une fois au repos (%.1f %%)",
          rd_max_bone_error(&rd) * 100.0f);
}

static void test_rotation(void)
{
    RdConfig cfg;
    Ragdoll rd;
    /* moto couchée sur le côté droit (rotation de 90 degres autour de Z) */
    const float rot[3][3] = { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
    const float pos[3] = { 0.0f, 0.55f, 0.0f };
    const float vel[3] = { 0.0f, 0.0f, 0.0f };

    printf("Moto couchee au moment de la chute\n");
    rd_config_default(&cfg);
    rd_init(&rd, &cfg);
    rd_spawn(&rd, pos, rot, vel);
    CHECK(fabsf(rd.pos[RD_HEAD][1] - rd.pos[RD_PELVIS][1]) < 0.2f,
          "la tete est a la hauteur du bassin (pilote couche)");
    CHECK(rd_max_bone_error(&rd) < 1e-4f, "la rotation ne deforme pas le corps");
}

static void test_config(void)
{
    PluginConfig cfg;
    printf("Lecture de la configuration\n");
    cfg_default(&cfg);
    CHECK(cfg_apply_line(&cfg, "ground_friction = 0.4 ; commentaire"), "cle connue acceptee");
    CHECK(fabsf(cfg.physics.ground_friction - 0.4f) < 1e-6f, "valeur flottante lue");
    CHECK(cfg_apply_line(&cfg, "  iterations=12"), "espaces ignores");
    CHECK(cfg.physics.iterations == 12, "valeur entiere lue");
    CHECK(!cfg_apply_line(&cfg, "; iterations = 99"), "commentaire ignore");
    CHECK(!cfg_apply_line(&cfg, "[physique]"), "section ignoree");
    CHECK(!cfg_apply_line(&cfg, "inconnu = 3"), "cle inconnue refusee");
    CHECK(cfg_load(&cfg, "config/MXRP_Ragdoll.ini", CFG_USER), "le fichier ini du depot se lit");
    CHECK(cfg.skeleton_hook == 1 && cfg.skeleton_scan == 0, "interception du squelette fixee dans le plugin");
    {
        /* v0.41.22 : seules les touches et le reseau se lisent dans MXRP_Ragdoll.ini */
        PluginConfig c2;
        FILE *f = fopen("build/test_lock.ini", "w");
        cfg_default(&c2);
        if (f) {
            fputs("ground_friction = 0.1\nskeleton_hook = 0\nlanguage = 2\nkey_run = CTRL\nreseau_port = 5000\n", f);
            fclose(f);
        }
        CHECK(cfg_load(&c2, "build/test_lock.ini", CFG_USER) && fabsf(c2.physics.ground_friction - 1.2f) < 1e-6f &&
                  c2.skeleton_hook == 1 && c2.language == LANG_AUTO, /* v1.8.118 : langue de Windows par defaut */
              "reglages fixes ignores dans le .ini");
        CHECK(c2.key_run[0] == 0x11 && c2.net_port == 5000, "touches et reseau lus dans le .ini");
        CHECK(cfg_load(&c2, "build/test_lock.ini", CFG_MENU) && c2.language == 2 &&
                  fabsf(c2.physics.ground_friction - 1.2f) < 1e-6f,
              "fichier du menu P : langue lue, physique toujours fixe");
    }
    CHECK(cfg.key_forward[0] == 'Z' && cfg.key_bike[0] == 'E' && cfg.pad_bike == 0x8000, "touches par defaut");
    CHECK(cfg_apply_line(&cfg, "key_forward = W up") && cfg.key_forward[0] == 'W' && cfg.key_forward[1] == 0x26,
          "deux touches pour une action (W et fleche haut)");
    CHECK(cfg_apply_line(&cfg, "key_run = ctrl") && cfg.key_run[0] == 0x11 && cfg.key_run[1] == 0, "touche nommee (CTRL)");
    CHECK(cfg_apply_line(&cfg, "key_menu = F10") && cfg.key_menu[0] == 0x79, "touche de fonction F10");
    CHECK(cfg_apply_line(&cfg, "pad_dismount = LB+RB") && cfg.pad_dismount == 0x0300, "boutons combines LB+RB");
    CHECK(cfg_apply_line(&cfg, "pad_run = lt") && cfg.pad_run == PAD_LT, "gachette gauche");
    CHECK(!cfg_apply_line(&cfg, "pad_bike = Z9") && cfg.pad_bike == 0x8000, "bouton inconnu refuse, valeur gardee");
}

/* tibia vers l'arrière de la cuisse (charnière du genou, comme dans ragdoll.c) : > 0 si correct */
static float knee_bend(const Ragdoll *rd, int hip, int knee, int foot)
{
    float a[3], th[3], sh[3], n[3], tl, sl;
    int k;
    for (k = 0; k < 3; ++k) {
        a[k] = rd->pos[RD_HIP_R][k] - rd->pos[RD_HIP_L][k];
        th[k] = rd->pos[knee][k] - rd->pos[hip][k];
        sh[k] = rd->pos[foot][k] - rd->pos[knee][k];
    }
    n[0] = th[1] * sh[2] - th[2] * sh[1];
    n[1] = th[2] * sh[0] - th[0] * sh[2];
    n[2] = th[0] * sh[1] - th[1] * sh[0];
    tl = sqrtf(th[0] * th[0] + th[1] * th[1] + th[2] * th[2]);
    sl = sqrtf(sh[0] * sh[0] + sh[1] * sh[1] + sh[2] * sh[2]);
    return (n[0] * a[0] + n[1] * a[1] + n[2] * a[2]) / (tl * sl * sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) + 1e-6f);
}

/* torsion bassin / buste autour de la colonne, en degrés */
static float torsion(const Ragdoll *rd)
{
    float s[3], h[3], u[3], ul, ds, dh, c;
    int k;
    for (k = 0; k < 3; ++k) {
        s[k] = rd->pos[RD_SHOULDER_R][k] - rd->pos[RD_SHOULDER_L][k];
        h[k] = rd->pos[RD_HIP_R][k] - rd->pos[RD_HIP_L][k];
        u[k] = rd->pos[RD_NECK][k] - rd->pos[RD_PELVIS][k];
    }
    ul = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (k = 0; k < 3; ++k)
        u[k] /= ul;
    ds = s[0] * u[0] + s[1] * u[1] + s[2] * u[2];
    dh = h[0] * u[0] + h[1] * u[1] + h[2] * u[2];
    for (k = 0; k < 3; ++k) {
        s[k] -= u[k] * ds;
        h[k] -= u[k] * dh;
    }
    c = (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]) /
        sqrtf((s[0] * s[0] + s[1] * s[1] + s[2] * s[2]) * (h[0] * h[0] + h[1] * h[1] + h[2] * h[2]));
    return acosf(c > 1.0f ? 1.0f : c) * 57.2958f;
}

/* v0.41.27 : mur à z = 3 m (devant la chute) : le pilote ne le traverse pas */
static int wall_fn(const float p[3], float r, float push[3])
{
    push[0] = push[1] = push[2] = 0.0f;
    if (p[2] + r <= -2.0f || p[2] > -1.0f + 1.0f)
        return 0;
    push[2] = -2.0f - r - p[2];
    return 1;
}

static void test_mur(void)
{
    RdConfig cfg;
    Ragdoll rd;
    const float pos[3] = { 10.0f, 2.55f, -5.0f };
    const float vel[3] = { 0.0f, 3.0f, 20.0f };
    float worst;
    int i, through = 0;
    printf("Chute a 72 km/h contre un mur\n");
    rd_config_default(&cfg);
    cfg.ground_offset = -2.55f;
    rd_init(&rd, &cfg);
    rd.obstacle_fn = wall_fn;
    rd_spawn(&rd, pos, k_identity, vel);
    run_until_rest(&rd, 20.0f, &worst);
    for (i = 0; i < RD_NUM_POINTS; ++i)
        if (rd.pos[i][2] > -2.0f)
            ++through;
    CHECK(all_finite(&rd), "aucune valeur NaN / infinie");
    CHECK(through == 0, "aucun point de l'autre cote du mur (%d)", through);
    CHECK(rd_max_bone_error(&rd) < 0.05f, "os corrects contre le mur (%.1f %%)", rd_max_bone_error(&rd) * 100.0f);
}

static void test_genoux(void)
{
    RdConfig cfg;
    Ragdoll rd;
    const float pos[3] = { 0.0f, 3.0f, 0.0f };
    const float vel[3] = { 6.0f, 4.0f, 25.0f };
    float worst = 1.0f, t, worst_bone = 0.0f, worst_tw = 0.0f;
    int i;
    printf("Genoux pendant une chute violente (tonneaux)\n");
    rd_config_default(&cfg);
    cfg.ground_offset = -3.0f;
    rd_init(&rd, &cfg);
    rd_spawn(&rd, pos, k_identity, vel);
    /* rotations violentes : on fouette les pieds et les genoux */
    for (i = 0; i < 3; ++i) {
        rd.prev[RD_FOOT_L][i] -= (i == 0 ? 0.05f : -0.03f);
        rd.prev[RD_KNEE_R][i] += (i == 2 ? 0.04f : -0.02f);
        rd.prev[RD_FOOT_R][i] += 0.035f;
    }
    for (t = 0.0f; t < 6.0f; t += 0.01f) {
        float kl, kr, e;
        rd_step(&rd, 0.01f);
        if (t < 0.05f)
            continue;
        kl = knee_bend(&rd, RD_HIP_L, RD_KNEE_L, RD_FOOT_L);
        kr = knee_bend(&rd, RD_HIP_R, RD_KNEE_R, RD_FOOT_R);
        if (kl < worst) worst = kl;
        if (kr < worst) worst = kr;
        e = rd_max_bone_error(&rd);
        if (e > worst_bone) worst_bone = e;
        if (torsion(&rd) > worst_tw) worst_tw = torsion(&rd);
    }
    CHECK(all_finite(&rd), "aucune valeur NaN / infinie");
    CHECK(worst > -0.12f, "les genoux ne plient jamais a l'envers (pire %.3f)", worst);
    CHECK(worst_tw < 45.0f, "le bassin ne se retourne pas par rapport au buste (torsion max %.0f deg)", worst_tw);
    CHECK(worst_bone < 0.2f, "les os tiennent (ecart max %.1f %%)", worst_bone * 100.0f);
}

int main(void)
{
    test_genoux();
    test_chute_a_l_arret();
    test_chute_a_haute_vitesse();
    test_mur();
    test_rotation();
    test_config();
    if (g_failures) {
        printf("\n%d test(s) en echec\n", g_failures);
        return 1;
    }
    printf("\nTous les tests passent\n");
    return 0;
}

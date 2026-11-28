/*
 * test_drive.c - simule le jeu pour tester le pilotage du squelette 3D
 *
 * Reconstruit en mémoire un modèle rider.hrc avec le vrai squelette de
 * MX Bikes beta21e (73 os, parents et longueurs relevés en jeu), émule la
 * mise à jour du jeu (monde = monde(parent) x locale) et vérifie que le
 * squelette suit le ragdoll sans se déformer.
 *
 *   make test
 */
#include "../src/drive.h"
#include "../src/hook.h"
#include "../src/ragdoll.h"
#include "../src/walk.h"
#include "../src/terrain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* lecture / écriture directes : ici toute la mémoire est à nous */
int safe_read(const void *addr, void *out, size_t size)
{
    if (!addr)
        return 0;
    memcpy(out, addr, size);
    return 1;
}

int safe_write(void *addr, const void *data, size_t size)
{
    if (!addr)
        return 0;
    memcpy(addr, data, size);
    return 1;
}

typedef struct {
    int index;
    const char *name;
    int parent;
    float t[3];
} BoneDef;

static const BoneDef k_bones[] = {
#include "../src/rider_skeleton_beta21e.inc"
};
#define NB ((int)(sizeof(k_bones) / sizeof(k_bones[0])))

#define NODE_SIZE 0x3F8

static uint8_t g_model[0x400];
static uint8_t g_lod[0x40];
static uint8_t *g_nodes;
static int g_failures = 0;

#define CHECK(cond, ...)                         \
    do {                                         \
        printf((cond) ? "  ok    " : "  ECHEC ");\
        if (!(cond))                             \
            ++g_failures;                        \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
    } while (0)

static void mat_rot(float m[16], int axis, float deg)
{
    float a = deg * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
    memset(m, 0, 16 * sizeof(float));
    m[15] = 1.0f;
    if (axis == 2) { m[0] = c; m[1] = -s; m[4] = s; m[5] = c; m[10] = 1; }
    if (axis == 1) { m[0] = c; m[2] = s; m[8] = -s; m[10] = c; m[5] = 1; }
    if (axis == 0) { m[5] = c; m[6] = -s; m[9] = s; m[10] = c; m[0] = 1; }
}

static void mul(const float a[16], const float b[16], float o[16])
{
    float t[16];
    int r, c, k;
    for (r = 0; r < 4; ++r)
        for (c = 0; c < 4; ++c) {
            float v = 0;
            for (k = 0; k < 4; ++k)
                v += a[r * 4 + k] * b[k * 4 + c];
            t[r * 4 + c] = v;
        }
    memcpy(o, t, sizeof(t));
}

static float *node_local(int k) { return (float *)(g_nodes + (size_t)k * NODE_SIZE + 0x68); }
static float *node_world(int k) { return (float *)(g_nodes + (size_t)k * NODE_SIZE + 0x100); }

static void set_model(float x, float y, float z, float yaw_deg)
{
    float m[16];
    mat_rot(m, 1, yaw_deg);
    m[3] = x; m[7] = y; m[11] = z;
    memcpy(g_model + 0x11C, m, 12 * sizeof(float));
}

/* ce que fait le jeu : monde = modèle x locale(racine), puis monde(parent) x locale */
static void game_update(void)
{
    float model[16], w[16];
    int k;
    memcpy(model, g_model + 0x11C, 12 * sizeof(float));
    model[12] = model[13] = model[14] = 0; model[15] = 1;
    for (k = 0; k < NB; ++k) {
        int p = k_bones[k].parent;
        float pw[16];
        if (p < 0) {
            memcpy(pw, model, sizeof(pw));
        } else {
            memcpy(pw, node_world(p), 12 * sizeof(float));
            pw[12] = pw[13] = pw[14] = 0; pw[15] = 1;
        }
        mul(pw, node_local(k), w);
        memcpy(node_world(k), w, 12 * sizeof(float));
        node_world(k)[12] = 0; node_world(k)[13] = 0; node_world(k)[14] = 0; node_world(k)[15] = 1;
    }
}

static int find(const char *name)
{
    int k;
    for (k = 0; k < NB; ++k)
        if (!strcmp(k_bones[k].name, name))
            return k;
    return -1;
}

/* pose de départ : pilote assis sur la moto, à peu près */
static void build_rider(void)
{
    int k;
    g_nodes = calloc((size_t)NB, NODE_SIZE);
    for (k = 0; k < NB; ++k) {
        float *l = node_local(k);
        strcpy((char *)(g_nodes + (size_t)k * NODE_SIZE), k_bones[k].name);
        mat_rot(l, 2, 0);
        l[3] = k_bones[k].t[0]; l[7] = k_bones[k].t[1]; l[11] = k_bones[k].t[2];
    }
#define ROT(name, axis, deg)                                         \
    do {                                                             \
        float r[16], *l = node_local(find(name));                    \
        float t3 = l[3], t7 = l[7], t11 = l[11];                     \
        mat_rot(r, axis, deg);                                       \
        memcpy(l, r, sizeof(r));                                     \
        l[3] = t3; l[7] = t7; l[11] = t11;                           \
    } while (0)
    ROT("riderRIG_Root", 2, 80);          /* colonne vers le haut, un peu penchée */
    ROT("riderRIG_LeftHip", 2, 120);      /* cuisses vers l'avant et le bas */
    ROT("riderRIG_RightHip", 2, 120);
    ROT("riderRIG_LeftKnee", 2, -100);    /* tibias vers le bas */
    ROT("riderRIG_RightKnee", 2, -100);
    ROT("riderRIG_LeftCollar", 1, 80);    /* épaules sur les côtés */
    ROT("riderRIG_RightCollar", 1, -80);
    ROT("riderRIG_LeftShoulder", 2, -70); /* bras vers l'avant et le bas */
    ROT("riderRIG_RightShoulder", 2, -70);
    ROT("riderRIG_LeftElbow", 2, 40);
    ROT("riderRIG_RightElbow", 2, 40);
#undef ROT
    memset(g_model, 0, sizeof(g_model));
    strcpy((char *)g_model, "rider\\riders\\default_mx\\rider.hrc");
    {
        int32_t count = NB;
        void *arr = g_nodes, *lod = g_lod;
        memcpy(g_lod + 0x10, &count, 4);
        memcpy(g_lod + 0x18, &arr, 8);
        memcpy(g_model + 0x1F0, &lod, 8);
    }
}

static int terrain_ground_for_test(float x, float z, float *h)
{
    return terrain_height(x, z, h, NULL);
}

static float dist(const float *a, const float *b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void wpos(int k, float out[3])
{
    out[0] = node_world(k)[3];
    out[1] = node_world(k)[7];
    out[2] = node_world(k)[11];
}

int main(void)
{
    RdConfig cfg;
    Ragdoll rd;
    const float vel[3] = { 0.0f, 1.5f, 12.0f };
    static float base_locals[NB][16];
    float worst_root = 0, worst_knee = 0, worst_elbow = 0, worst_len = 0, lowest = 1e9f;
    int frame, k, nan = 0;
    int i_root = find("riderRIG_Root"), i_knee = find("riderRIG_LeftKnee"),
        i_ankle = find("riderRIG_LeftAnkle"), i_elbow = find("riderRIG_RightElbow");

    printf("Pilotage du squelette 3D (%d os du vrai rider.hrc)\n", NB);
    build_rider();
    set_model(10.0f, 0.0f, 20.0f, 30.0f);
    game_update();
    for (k = 0; k < NB; ++k)
        memcpy(base_locals[k], node_local(k), sizeof(base_locals[k]));

    rd_config_default(&cfg);
    rd_init(&rd, &cfg);
    drive_init(NULL);
    CHECK(drive_spawn(g_model, &rd, vel, 0.0f), "depart depuis la pose reelle");
    CHECK(drive_root_error() < 1e-3f, "racine = matrice du modele x locale (erreur %.5f)",
          drive_root_error());
    {
        float p[3];
        wpos(i_root, p);
        CHECK(dist(p, rd.pos[RD_PELVIS]) < 1e-4f, "le bassin du ragdoll part du bassin du pilote");
    }

    for (frame = 0; frame < 600; ++frame) {
        float p[3], q[3];
        /* le jeu déplace aussi son propre corps de chute : la matrice du modèle bouge */
        /* le corps de chute du jeu glisse et descend une pente, puis s'arrête */
        {
            float f = frame < 300 ? (float)frame : 300.0f;
            set_model(10.0f + f * 0.01f, 0.6f - f * 0.002f, 20.0f + f * 0.02f, 30.0f + f * 0.2f);
        }
        drive_anchor(g_model, &rd, 0.01f, 3.0f, 0.35f);
        rd_step(&rd, 0.01f);
        drive_apply(g_model, &rd);
        game_update();

        wpos(i_root, p);
        if (dist(p, rd.pos[RD_PELVIS]) > worst_root)
            worst_root = dist(p, rd.pos[RD_PELVIS]);
        wpos(i_knee, p);
        if (dist(p, rd.pos[RD_KNEE_L]) > worst_knee)
            worst_knee = dist(p, rd.pos[RD_KNEE_L]);
        wpos(i_elbow, p);
        if (dist(p, rd.pos[RD_ELBOW_R]) > worst_elbow)
            worst_elbow = dist(p, rd.pos[RD_ELBOW_R]);
        for (k = 1; k < NB; ++k) {
            int par = k_bones[k].parent;
            float len = sqrtf(k_bones[k].t[0] * k_bones[k].t[0] + k_bones[k].t[1] * k_bones[k].t[1] +
                              k_bones[k].t[2] * k_bones[k].t[2]);
            if (par < 1)
                continue;
            wpos(k, p);
            wpos(par, q);
            if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
                ++nan;
            if (fabsf(dist(p, q) - len) > worst_len)
                worst_len = fabsf(dist(p, q) - len);
            if (frame > 400) {
                float h = rd.ground_n[0] * p[0] + rd.ground_n[1] * p[1] + rd.ground_n[2] * p[2] -
                          rd.ground_y;
                if (h < lowest)
                    lowest = h;
            }
        }
    }
    {
        float a[3];
        wpos(i_ankle, a);
        printf("  (au repos : bassin y=%.2f, cheville gauche y=%.2f, ragdoll %s)\n",
               rd.pos[RD_PELVIS][1], a[1], rd.resting ? "immobile" : "en mouvement");
    }
    CHECK(nan == 0, "aucune valeur NaN dans les os");
    CHECK(worst_root < 1e-3f, "le bassin 3D suit le ragdoll (ecart max %.4f m)", worst_root);
    CHECK(worst_knee < 0.06f, "le genou 3D suit le ragdoll (ecart max %.3f m)", worst_knee);
    CHECK(worst_elbow < 0.10f, "le coude 3D suit le ragdoll (ecart max %.3f m)", worst_elbow);
    CHECK(worst_len < 1e-3f, "aucun os ne s'etire (ecart max %.5f m)", worst_len);
    CHECK(lowest > -0.15f, "le pilote 3D reste au-dessus du sol (point le plus bas a %.2f m du sol)",
          lowest);
    {
        float dx = rd.pos[RD_PELVIS][0] - 13.0f, dz = rd.pos[RD_PELVIS][2] - 26.0f;
        CHECK(rd.ground_n[1] > 0.9f, "pose sur un sol a peu pres plat a la fin (normale y %.2f)",
              rd.ground_n[1]);
        printf("  (bassin a %.2f m du corps de chute du jeu : le pilote est libre)\n", sqrtf(dx * dx + dz * dz));
    }

    /* poings fermés : le bout des doigts se rapproche de la paume, sans NaN */
    {
        int tip = find("riderRIG_RightIndex3"), mid = find("riderRIG_RightMiddle3"), wr = find("riderRIG_RightWrist");
        int th = find("riderRIG_RightThumb3"), th1 = find("riderRIG_RightThumb1");
        float a[3], b[3], w[3], d0, d1, m0, m1, t0, t1, mw[3], c[3], k0, k1;
        printf("Poings fermes\n");
        drive_set_fist(0.0f);
        drive_apply(g_model, &rd);
        game_update();
        wpos(tip, a); wpos(wr, w); d0 = dist(a, w);
        wpos(mid, a); m0 = dist(a, w);
        wpos(th, a); t0 = dist(a, w);
        wpos(tip, a); wpos(th1, c); k0 = dist(a, c);
        drive_set_fist(1.0f);
        drive_apply(g_model, &rd);
        game_update();
        wpos(tip, b); wpos(wr, mw); d1 = dist(b, mw);
        wpos(mid, b); m1 = dist(b, mw);
        wpos(th, b); t1 = dist(b, mw);
        wpos(tip, b); wpos(th1, c); k1 = dist(b, c);
        CHECK(k1 < k0, "cote paume : le bout de l'index va vers la base du pouce (%.3f -> %.3f m)", k0, k1);
        {
            int lt = find("riderRIG_LeftIndex3"), lt1 = find("riderRIG_LeftThumb1");
            float l0, l1;
            drive_set_fist(0.0f); drive_apply(g_model, &rd); game_update();
            wpos(lt, a); wpos(lt1, c); l0 = dist(a, c);
            drive_set_fist(1.0f); drive_apply(g_model, &rd); game_update();
            wpos(lt, a); wpos(lt1, c); l1 = dist(a, c);
            CHECK(l1 < l0, "main gauche aussi vers la paume (%.3f -> %.3f m)", l0, l1);
        }
        CHECK(isfinite(b[0]) && isfinite(b[1]) && isfinite(b[2]), "aucune valeur NaN dans les doigts");
        CHECK(d1 < d0 - 0.01f && m1 < m0 - 0.01f,
              "les doigts se replient vers la paume (index %.3f -> %.3f m, majeur %.3f -> %.3f m du poignet)",
              d0, d1, m0, m1);
        CHECK(t1 <= t0 + 0.005f, "le pouce se replie aussi (%.3f -> %.3f m)", t0, t1);
        drive_set_fist(0.0f);
        drive_apply(g_model, &rd);
        game_update();
        wpos(tip, b);
        wpos(wr, mw);
        CHECK(fabsf(dist(b, mw) - d0) < 1e-3f, "main rouverte = pose du jeu");
    }

    /* tête stabilisée en marchant : au-dessus du cou, sans NaN, et elle suit la direction donnée */
    {
        int hd = find("riderRIG_Head"), nk = find("riderRIG_Neck1");
        const float up[3] = { 0.0f, 1.0f, 0.15f }, rt[3] = { 1.0f, 0.0f, 0.0f };
        float h0[3], n0[3], hx;
        printf("Tete en marchant\n");
        drive_set_head(up, rt);
        drive_apply(g_model, &rd);
        game_update();
        wpos(hd, h0);
        wpos(nk, n0);
        hx = node_world(hd)[0] * 0.0f + node_world(hd)[4] * 1.0f + node_world(hd)[8] * 0.15f; /* axe X de l'os . dir */
        CHECK(isfinite(h0[0]) && isfinite(h0[1]) && isfinite(h0[2]), "aucune valeur NaN dans la tete");
        CHECK(h0[1] > n0[1], "la tete est au-dessus du cou (%.2f > %.2f)", h0[1], n0[1]);
        CHECK(hx / sqrtf(1.0f + 0.15f * 0.15f) > 0.9f, "l'axe de la tete suit la direction voulue (%.2f)",
              hx / sqrtf(1.0f + 0.15f * 0.15f));
        drive_set_head(NULL, NULL);
    }

    /* deuxième chute : le corps du jeu glisse en continu sur une pente de 30 degrés */
    printf("Chute sur une pente\n");
    set_model(10.0f, 0.0f, 20.0f, 0.0f);
    drive_restore();
    game_update();
    rd_init(&rd, &cfg);
    CHECK(drive_spawn(g_model, &rd, vel, -1.0f), "depart");
    lowest = 1e9f;
    for (frame = 0; frame < 500; ++frame) {
        float x = 10.0f + frame * 0.01f * 1.0f; /* 1 m/s vers +x */
        float y = -frame * 0.01f * 0.577f;      /* tan(30) */
        set_model(x, y, 20.0f, 0.0f);
        drive_anchor(g_model, &rd, 0.01f, 3.0f, 0.2f);
        rd_step(&rd, 0.01f);
        drive_apply(g_model, &rd);
        game_update();
        if (frame > 250) /* la pente est reconnue en ~1,5 s */
            for (k = 1; k < NB; ++k) {
                float p[3], h;
                wpos(k, p);
                /* hauteur au-dessus de la vraie pente, mesurée selon sa normale */
                h = (p[1] - (-(p[0] - 10.0f) * 0.577f) + 0.2f) * 0.866f;
                if (h < lowest)
                    lowest = h;
            }
    }
    CHECK(fabsf(rd.ground_n[0] - 0.5f) < 0.06f && fabsf(rd.ground_n[1] - 0.866f) < 0.06f,
          "le sol penche comme la pente (normale %.2f %.2f %.2f, attendu 0.50 0.87 0)",
          rd.ground_n[0], rd.ground_n[1], rd.ground_n[2]);
    /* v0.41.19 : -0,25 (avant -0,15). Plus de frottement au sol : le pilote reste un peu en
       arrière du corps du jeu qui glisse, et ce test n'a qu'un plan estimé (en jeu, le relief
       est lu point par point et actif dès le départ) */
    CHECK(lowest > -0.25f, "le pilote ne traverse pas la pente (point le plus bas a %.2f m)", lowest);

    /* troisième chute (vue en jeu, v0.4.4) : le corps du jeu tombe de 4,4 m en avançant,
     * puis s'arrête sur un sol plat. Ce n'est pas une pente. */
    printf("Chute de haut sur un sol plat\n");
    set_model(10.0f, 4.7f, 20.0f, 0.0f);
    drive_restore();
    game_update();
    rd_init(&rd, &cfg);
    CHECK(drive_spawn(g_model, &rd, vel, 3.0f), "depart");
    {
        float y = 4.7f, vy = 0.0f, x = 10.0f, worst_n = 1.0f;
        for (frame = 0; frame < 700; ++frame) {
            if (y > 0.3f) {
                vy -= 9.81f * 0.01f;
                y += vy * 0.01f;
                x += 0.08f;
                if (y < 0.3f)
                    y = 0.3f;
            } else if (frame < 300) {
                x += 0.02f; /* glisse un peu sur le plat */
            }
            set_model(x, y, 20.0f, 0.0f);
            drive_anchor(g_model, &rd, 0.01f, 3.0f, 0.08f);
            rd_step(&rd, 0.01f);
            drive_apply(g_model, &rd);
            game_update();
            if (rd.ground_n[1] < worst_n)
                worst_n = rd.ground_n[1];
        }
        CHECK(worst_n > 0.99f, "le sol reste plat (normale y min %.3f)", worst_n);
        CHECK(rd.pos[RD_PELVIS][1] > 0.2f, "le bassin finit sur la piste (y %.2f, corps du jeu 0.30)",
              rd.pos[RD_PELVIS][1]);
    }

    /* quatrième chute : au repos, le pilote se relève puis marche */
    printf("Se relever et marcher\n");
    set_model(10.0f, 0.3f, 20.0f, 0.0f);
    drive_restore();
    game_update();
    rd_init(&rd, &cfg);
    CHECK(drive_spawn(g_model, &rd, vel, 0.0f), "depart");
    {
        Walker w;
        WalkInput in;
        float start[2], worst_len2 = 0.0f, foot_low = 1e9f, foot_high = -1e9f, pel_min = 1e9f,
              pel_max = -1e9f, moved;
        int ia = find("riderRIG_LeftAnkle"), ir = find("riderRIG_Root"), bad = 0;
        memset(&w, 0, sizeof(w));
        walk_calibrate(&w, &rd);
        for (frame = 0; frame < 400; ++frame) {
            drive_anchor(g_model, &rd, 0.01f, 3.0f, 0.08f);
            rd_step(&rd, 0.01f);
            drive_apply(g_model, &rd);
            game_update();
        }
        walk_begin(&w, &rd);
        start[0] = w.pos[0];
        start[1] = w.pos[1];
        memset(&in, 0, sizeof(in));
        /* v1.8.15 : le relevé dure jusqu'à 2,7 s (quatre pattes, genou) */
        for (frame = 0; frame < 830; ++frame) {
            float p[3];
            in.forward = frame >= 280;
            in.right = frame >= 580 && frame < 680;
            in.run = frame >= 680;
            walk_update(&w, &rd, &in, 0.01f);
            drive_apply(g_model, &rd);
            game_update();
            if (frame < 280)
                continue; /* relève */
            for (k = 1; k < NB; ++k) {
                int par = k_bones[k].parent;
                float q[3], len;
                if (par < 1)
                    continue;
                wpos(k, p);
                wpos(par, q);
                if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
                    ++bad;
                len = sqrtf(k_bones[k].t[0] * k_bones[k].t[0] + k_bones[k].t[1] * k_bones[k].t[1] +
                            k_bones[k].t[2] * k_bones[k].t[2]);
                if (fabsf(dist(p, q) - len) > worst_len2)
                    worst_len2 = fabsf(dist(p, q) - len);
            }
            wpos(ia, p);
            if (p[1] < foot_low)
                foot_low = p[1];
            if (p[1] > foot_high)
                foot_high = p[1];
            wpos(ir, p);
            if (p[1] < pel_min)
                pel_min = p[1];
            if (p[1] > pel_max)
                pel_max = p[1];
        }
        moved = sqrtf((w.pos[0] - start[0]) * (w.pos[0] - start[0]) +
                      (w.pos[1] - start[1]) * (w.pos[1] - start[1]));
        printf("  (bassin entre %.2f et %.2f m, cheville entre %.2f et %.2f m, sol %.2f)\n", pel_min,
               pel_max, foot_low, foot_high, walk_ground_height(&rd, w.pos[0], w.pos[1]));
        CHECK(w.state == WALK_ON, "le pilote est debout apres la releve");

        {
            /* geste : mains sur le guidon, accroupi, puis jambe qui passe par-dessus */
            float fwd[3] = { sinf(w.heading), 0.0f, cosf(w.heading) }, hl[3], hr[3], g;
            int i2, nan2 = 0;
            memset(&in, 0, sizeof(in));
            for (k = 0; k < 3; ++k) {
                hl[k] = rd.pos[RD_PELVIS][k] + fwd[k] * 0.55f;
                hr[k] = hl[k];
            }
            hl[1] += 0.10f;
            hr[1] += 0.10f;
            hl[0] -= 0.3f * fwd[2];
            hl[2] += 0.3f * fwd[0];
            hr[0] += 0.3f * fwd[2];
            hr[2] -= 0.3f * fwd[0];
            memcpy(w.grab_hand[0], hl, sizeof(hl));
            memcpy(w.grab_hand[1], hr, sizeof(hr));
            w.grab = 1.0f;
            w.crouch = 0.3f;
            walk_update(&w, &rd, &in, 0.01f);
            CHECK(dist(rd.pos[RD_HAND_L], hl) < 0.01f || dist(rd.pos[RD_HAND_R], hl) < 0.01f,
                  "une main est sur la poignee (%.3f / %.3f m)", dist(rd.pos[RD_HAND_L], hl), dist(rd.pos[RD_HAND_R], hl));
            CHECK(fabsf(dist(rd.pos[RD_SHOULDER_L], rd.pos[RD_ELBOW_L]) - w.upper_arm) < 1e-3f,
                  "le bras garde sa longueur en tenant le guidon");
            g = walk_ground_height(&rd, w.pos[0], w.pos[1]);
            w.leg_side = 1;
            w.leg_target[0] = w.pos[0] + 0.6f * fwd[2];
            w.leg_target[1] = g + w.ankle_height;
            w.leg_target[2] = w.pos[1] - 0.6f * fwd[0];
            w.leg_lift = 0.5f;
            walk_update(&w, &rd, &in, 0.01f);
            for (i2 = 0; i2 < RD_NUM_POINTS; ++i2)
                nan2 += !isfinite(rd.pos[i2][0]) || !isfinite(rd.pos[i2][1]) || !isfinite(rd.pos[i2][2]);
            CHECK(nan2 == 0 && rd.pos[RD_FOOT_R][1] > rd.pos[RD_FOOT_L][1] + 0.2f,
                  "en enjambant, le pied monte au-dessus de la selle (%.2f m de plus que l'autre)",
                  rd.pos[RD_FOOT_R][1] - rd.pos[RD_FOOT_L][1]);
            w.grab = 0.0f;
            w.crouch = 0.0f;
            w.leg_lift = 0.0f;
        }
        {
            /* montée complète sur la moto (même déroulé que plugin.c) : le pied passe au-dessus
               de la selle, le bassin finit assis au milieu, un pied de chaque côté au sol */
            float fwd[3] = { sinf(w.heading), 0.0f, cosf(w.heading) }, rgt[3] = { fwd[2], 0.0f, -fwd[0] };
            float c0[2] = { w.pos[0], w.pos[1] }, g0 = walk_ground_height(&rd, w.pos[0], w.pos[1]);
            float pivot_y = g0 + 0.62f, peak = -1e9f, t2, worst_l = 0.0f, flex = 0.0f;
            Walker saved = w;
            int nan3 = 0, i3;
            const float side = 1.0f; /* le pilote est à droite de la moto : la moto est à sa gauche */
            float bike_c[2] = { c0[0] - rgt[0] * 0.42f, c0[1] - rgt[2] * 0.42f };
            memset(&in, 0, sizeof(in));
            w.grab = 1.0f;
            for (t2 = 0.0f; t2 < 1.8f; t2 += 0.01f) {
                float lat = 0.42f * (t2 < 0.85f ? 0.0f : t2 > 1.3f ? 1.0f : (t2 - 0.85f) / 0.45f), l1;
                float u = (t2 - 0.5f) / 0.65f;
                w.leg_side = 0; /* jambe gauche = côté moto */
                w.leg_lift = u < 0 ? 0 : u > 1 ? 1 : u * u * (3 - 2 * u);
                w.leg_clear = pivot_y + 0.45f;
                w.leg_target[0] = bike_c[0] - rgt[0] * side * 0.40f;
                w.leg_target[1] = g0 + w.ankle_height;
                w.leg_target[2] = bike_c[1] - rgt[2] * side * 0.40f;
                w.pos[0] = c0[0] - rgt[0] * side * lat;
                w.pos[1] = c0[1] - rgt[2] * side * lat;
                w.sit = t2 < 0.95f ? 0.0f : t2 > 1.4f ? 1.0f : (t2 - 0.95f) / 0.45f;
                w.sit_y = pivot_y + 0.36f;
                if (w.sit_y > g0 + w.ankle_height + w.hip_height - 0.04f)
                    w.sit_y = g0 + w.ankle_height + w.hip_height - 0.04f;
                w.lean = 0.22f;
                for (k = 0; k < 3; ++k)
                    w.grab_hand[0][k] = w.grab_hand[1][k] = 0.0f;
                w.grab_hand[0][0] = bike_c[0] + fwd[0] * 0.55f - rgt[0] * 0.38f;
                w.grab_hand[0][1] = pivot_y + 0.45f;
                w.grab_hand[0][2] = bike_c[1] + fwd[2] * 0.55f - rgt[2] * 0.38f;
                w.grab_hand[1][0] = bike_c[0] + fwd[0] * 0.55f + rgt[0] * 0.38f;
                w.grab_hand[1][1] = pivot_y + 0.45f;
                w.grab_hand[1][2] = bike_c[1] + fwd[2] * 0.55f + rgt[2] * 0.38f;
                walk_update(&w, &rd, &in, 0.01f);
                for (i3 = 0; i3 < RD_NUM_POINTS; ++i3)
                    nan3 += !isfinite(rd.pos[i3][0]) || !isfinite(rd.pos[i3][1]) || !isfinite(rd.pos[i3][2]);
                if (w.leg_lift > 0.3f && w.leg_lift < 0.7f && rd.pos[RD_FOOT_L][1] > peak)
                    peak = rd.pos[RD_FOOT_L][1];
                if (w.leg_lift > 0.3f && w.leg_lift < 0.7f) {
                    float a3[3], b3[3], c3;
                    for (k = 0; k < 3; ++k) {
                        a3[k] = rd.pos[RD_KNEE_L][k] - rd.pos[RD_HIP_L][k];
                        b3[k] = rd.pos[RD_FOOT_L][k] - rd.pos[RD_KNEE_L][k];
                    }
                    c3 = (a3[0] * b3[0] + a3[1] * b3[1] + a3[2] * b3[2]) /
                         sqrtf((a3[0] * a3[0] + a3[1] * a3[1] + a3[2] * a3[2]) * (b3[0] * b3[0] + b3[1] * b3[1] + b3[2] * b3[2]));
                    if (acosf(c3 > 1 ? 1 : c3) * 57.3f > flex)
                        flex = acosf(c3 > 1 ? 1 : c3) * 57.3f;
                }
                l1 = fabsf(dist(rd.pos[RD_HIP_L], rd.pos[RD_KNEE_L]) - w.thigh) +
                     fabsf(dist(rd.pos[RD_KNEE_L], rd.pos[RD_FOOT_L]) - w.shin);
                if (l1 > worst_l)
                    worst_l = l1;
            }
            {
                float pc = (rd.pos[RD_PELVIS][0] - bike_c[0]) * rgt[0] + (rd.pos[RD_PELVIS][2] - bike_c[1]) * rgt[2];
                float fl = (rd.pos[RD_FOOT_L][0] - bike_c[0]) * rgt[0] + (rd.pos[RD_FOOT_L][2] - bike_c[1]) * rgt[2];
                float fr = (rd.pos[RD_FOOT_R][0] - bike_c[0]) * rgt[0] + (rd.pos[RD_FOOT_R][2] - bike_c[1]) * rgt[2];
                printf("Montee sur la moto\n");
                CHECK(nan3 == 0, "aucune valeur NaN pendant la montee");
                CHECK(peak > pivot_y + 0.40f, "le pied passe au-dessus de la selle (%.2f m, selle a %.2f m)", peak - g0,
                      pivot_y + 0.40f - g0);
                CHECK(flex < 45.0f, "la jambe reste presque tendue quand elle passe (pli max %.0f deg)", flex);
                CHECK(fabsf(pc) < 0.06f, "assis au milieu de la moto (ecart %.2f m)", pc);
                CHECK(fl < -0.25f && fr > 0.25f, "un pied de chaque cote de la moto (%.2f / %.2f m)", fl, fr);
                CHECK(rd.pos[RD_FOOT_L][1] < g0 + w.ankle_height + 0.03f && rd.pos[RD_FOOT_R][1] < g0 + w.ankle_height + 0.03f,
                      "les deux pieds au sol");
                CHECK(worst_l < 0.01f, "la jambe garde sa longueur (ecart %.3f m)", worst_l);
            }
            w = saved;
        }
        CHECK(bad == 0, "aucune valeur NaN en marchant");
        CHECK(worst_len2 < 1e-3f, "aucun os ne s'etire en marchant (ecart max %.5f m)", worst_len2);
        CHECK(moved > 3.0f && w.speed > 3.0f,
              "il avance et trottine (%.1f m du depart apres un virage, vitesse finale %.1f m/s)", moved,
              w.speed);
        {
            /* marche régulière : le pied d'appui ne glisse pas sur le sol */
            float last[3] = { 0, 0, 0 }, slide = 0.0f, gnd;
            int planted = 0, fr2;
            memset(&in, 0, sizeof(in));
            in.forward = 1;
            for (fr2 = 0; fr2 < 300; ++fr2) {
                walk_update(&w, &rd, &in, 0.01f);
                gnd = walk_ground_height(&rd, rd.pos[RD_FOOT_L][0], rd.pos[RD_FOOT_L][2]) + w.ankle_height;
                if (fr2 > 150 && rd.pos[RD_FOOT_L][1] < gnd + 0.002f) {
                    /* la cheville avance encore un peu quand le pied se pose (il roule sur le
                       talon) : on mesure une fois le pied à plat */
                    if (planted >= 6) {
                        float dx2 = rd.pos[RD_FOOT_L][0] - last[0], dz2 = rd.pos[RD_FOOT_L][2] - last[2];
                        float d2 = sqrtf(dx2 * dx2 + dz2 * dz2) / 0.01f;
                        if (d2 > slide)
                            slide = d2;
                    }
                    memcpy(last, rd.pos[RD_FOOT_L], sizeof(last));
                    ++planted;
                } else {
                    planted = 0;
                }
            }
            CHECK(slide < 0.25f, "le pied d'appui ne glisse pas (vitesse max au sol %.2f m/s, marche %.1f m/s)", slide, w.speed);
            in.forward = 0;
            for (fr2 = 0; fr2 < 150; ++fr2)
                walk_update(&w, &rd, &in, 0.01f);
        }
        {
            float g = walk_ground_height(&rd, w.pos[0], w.pos[1]);
            CHECK(foot_low - g > 0.0f && foot_high - g < 0.60f,
                  "les chevilles restent entre le sol et 60 cm (talon vers les fesses en courant) (%.2f a %.2f m au-dessus du sol)",
                  foot_low - g, foot_high - g);
            CHECK(pel_min - g > 0.65f && pel_max - g < 0.9f,
                  "bassin a hauteur d'un pilote debout (%.2f a %.2f m au-dessus du sol)", pel_min - g,
                  pel_max - g);
        }
    }

    /* relief : une structure dxHeightfieldData d'ODE en mémoire, avec une bosse */
    printf("Relief de la piste (heightfield ODE)\n");
    {
        static short hm[101 * 101];
        static uint8_t ode[0x58];
        float fl[14];
        int32_t in4[6] = { 101, 101, 0, 0, 2, 0 };
        void *ptr = hm;
        TerrainField tf;
        float h, nrm[3];
        int i, j;
        for (j = 0; j < 101; ++j)
            for (i = 0; i < 101; ++i) {
                /* bosse de 2 m de haut centrée en (25, 25) monde, rayon 6 m */
                float x = -25.0f + i * 0.5f + 15.0f, z = -25.0f + j * 0.5f + 20.0f;
                float r = sqrtf((x - 25.0f) * (x - 25.0f) + (z - 25.0f) * (z - 25.0f));
                hm[i + j * 101] = (short)(r < 6.0f ? 1000.0f * (1.0f + cosf(r / 6.0f * 3.14159f)) : 0);
            }
        fl[0] = 50.0f; fl[1] = 50.0f; fl[2] = 0.5f; fl[3] = 0.5f; fl[4] = 1.0f; fl[5] = 2.0f;
        fl[6] = 2.0f; fl[7] = 25.0f; fl[8] = 25.0f; fl[9] = 0.0f; fl[10] = 2.0f; fl[11] = 1.0f;
        fl[12] = 0.001f; fl[13] = 0.0f;
        memcpy(ode, fl, sizeof(fl));
        memcpy(ode + 56, in4, sizeof(in4));
        memcpy(ode + 80, &ptr, 8);
        CHECK(terrain_parse(ode, &tf) == 1, "structure dxHeightfieldData reconnue (%d x %d)", tf.nx, tf.nz);
        CHECK(terrain_parse(hm, &tf) == 0, "des donnees quelconques ne sont pas reconnues");
        terrain_parse(ode, &tf);
        tf.center[0] = 15.0f; tf.center[1] = 0.0f; tf.center[2] = 20.0f; tf.placed = 1;
        terrain_clear();
        terrain_add(&tf);
        CHECK(terrain_height(25.0f, 25.0f, &h, nrm) && fabsf(h - 2.0f) < 0.05f,
              "hauteur au sommet de la bosse (%.2f m, attendu 2)", h);
        CHECK(terrain_height(28.0f, 25.0f, &h, nrm) && nrm[0] > 0.3f,
              "normale penchee sur le flanc (%.2f %.2f %.2f)", nrm[0], nrm[1], nrm[2]);
        CHECK(!terrain_height(100.0f, 100.0f, &h, NULL), "hors du relief : pas de hauteur");
        {
            /* cache sur le disque : sauvegarde puis rechargement instantané */
            const char *cp = "build/test_cache.relief";
            float c = 0.0f, h2 = 0.0f;
            CHECK(terrain_save(cp, -0.09f), "relief sauvegarde dans le cache");
            terrain_clear();
            CHECK(!terrain_ready(), "relief vide avant rechargement");
            CHECK(terrain_load(cp, &c) == 1 && fabsf(c + 0.09f) < 1e-6f, "relief recharge avec son recalage (%.2f)", c);
            CHECK(terrain_height(25.0f, 25.0f, &h2, NULL) && fabsf(h2 - 2.0f) < 0.05f,
                  "meme hauteur apres rechargement (%.2f m)", h2);
            CHECK(!terrain_load("build/absent.relief", &c), "pas de cache : rien de charge");
            terrain_load(cp, &c);
            remove(cp);
        }
    }
    {
        /* marcher par-dessus la bosse : les chevilles suivent le relief */
        Walker w;
        WalkInput in;
        float worst = 1e9f, top = 0.0f;
        memset(&w, 0, sizeof(w));
        rd_init(&rd, &cfg);
        set_model(25.0f, 0.3f, 15.0f, 0.0f);
        drive_restore();
        game_update();
        drive_spawn(g_model, &rd, vel, 0.0f);
        walk_calibrate(&w, &rd);
        walk_set_ground_fn(terrain_ground_for_test);
        w.ankle_height = 0.17f;
        walk_begin(&w, &rd);
        w.heading = 0.0f; /* vers +z, droit sur la bosse */
        memset(&in, 0, sizeof(in));
        for (frame = 0; frame < 1140; ++frame) {
            float pa[3], g;
            int s2;
            in.forward = frame >= 280;
            walk_update(&w, &rd, &in, 0.01f);
            if (frame < 280)
                continue;
            for (s2 = 0; s2 < 2; ++s2) {
                int ft = s2 ? RD_FOOT_R : RD_FOOT_L;
                memcpy(pa, rd.pos[ft], sizeof(pa));
                terrain_height(pa[0], pa[2], &g, NULL);
                if (pa[1] - g < worst)
                    worst = pa[1] - g;
            }
            if (rd.pos[RD_PELVIS][1] > top)
                top = rd.pos[RD_PELVIS][1];
        }
        walk_set_ground_fn(NULL);
        CHECK(top > 2.5f, "le pilote monte sur la bosse (bassin jusqu'a %.2f m)", top);
        CHECK(worst > 0.1f, "les chevilles restent au-dessus du relief (min %.2f m)", worst);
    }

    drive_restore();
    {
        int same = 1;
        for (k = 0; k < NB; ++k)
            if (memcmp(base_locals[k], node_local(k), sizeof(base_locals[k])))
                same = 0;
        CHECK(same, "fin de chute : matrices locales d'origine remises");
    }

    if (g_failures) {
        printf("\n%d test(s) en echec\n", g_failures);
        return 1;
    }
    printf("\nTous les tests de pilotage passent\n");
    return 0;
}

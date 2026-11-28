/*
 * config.c - lecture du fichier MXRP_Ragdoll.ini
 *
 * Format : une ligne "cle = valeur" par réglage.
 * Les lignes qui commencent par ';' ou '#' et les sections [xxx] sont ignorées.
 */
#include "config.h"
#include "lang.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cfg_default(PluginConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    rd_config_default(&cfg->physics);
    cfg->enabled = 1;
    cfg->debug_window = 0;
    cfg->window_x = 0.70f;
    cfg->window_y = 0.55f;
    cfg->window_w = 0.28f;
    cfg->window_h = 0.40f;
    cfg->window_scale = 6.0f;
    cfg->screen_aspect = 16.0f / 9.0f;
    cfg->hide_delay = 2.0f;
    cfg->log = 1;
    cfg->skeleton_hook = 1;
    cfg->skeleton_scan = 0;
    cfg->scan_radius = 6.0f;
    cfg->write_test = 0;
    cfg->write_lift = 1.0f;
    cfg->ragdoll_3d = 1;
    cfg->anchor_strength = 3.0f;
    cfg->body_ground = 0.08f;
    cfg->walking = 1;
    cfg->getup_delay = 0.5f;
    cfg->foot_height = 0.20f;
    cfg->rider_lift = 0.10f;
    cfg->head_down = 0.25f;
    cfg->stand_lean = -0.08f;
    cfg->replay_record = 1;
    cfg->replay_play = 1;
    cfg->walk_camera = 1;
    cfg->bike_lift = 1;
    cfg->pad_respawn = 1;
    cfg->testing_only = 1;
    cfg->online_visual = 1;
    cfg->online_full = 1;
    cfg->net_on = 1;
    cfg->net_host = 0;
    cfg->net_ip[0] = '\0';
    cfg->net_port = 47731;
    cfg->camera_distance = 3.0f;
    cfg->camera_height = 0.9f;
    cfg->mouse_sensitivity = 0.004f;
    cfg->fov_fp = 0.0f;
    cfg->hold_msgs = 1; /* v1.8.111 */
    cfg->auto_update = 1; /* v1.8.172 */
    cfg->update_ask = 1;  /* v1.8.175 */
    cfg->bike_marker = 1;
    cfg->test_keys = 0;
    cfg->cones_3d = 1;
    cfg->tutorial = 1; /* v1.8.110 : montre au premier lancement */
    cfg->pad_style = 0; /* v1.8.107 : auto */
    cfg->objects_opti = 0; /* v1.8.92 : haute qualité par défaut */
    cfg->cones_lift = 0.045f;
    cfg->fists = 1;
    cfg->language = LANG_AUTO; /* v1.8.118 : langue de Windows par defaut (avant : anglais) ; menu P pour changer */
    cfg->key_forward[0] = 'Z'; cfg->key_forward[1] = 'W';
    cfg->key_back[0] = 'S';
    cfg->key_left[0] = 'Q'; cfg->key_left[1] = 'A';
    cfg->key_right[0] = 'D';
    cfg->key_run[0] = 0x11;   /* v1.8.170 : Ctrl (Maj : s'accroupir) */
    cfg->key_crouch[0] = 0x10; /* v1.8.170 : Maj */
    cfg->pad_crouch = 0x0080;  /* v1.8.170 : clic du stick droit */
    cfg->key_getup[0] = 'E';
    cfg->key_bike[0] = 'E';
    cfg->key_dismount[0] = 'F';
    cfg->key_menu[0] = 'P';
    cfg->key_view[0] = 'V';
    cfg->key_jump[0] = 0x20; /* espace */
    cfg->key_prio_kbd[0] = 0x76; /* F7 */
    cfg->key_prio_pad[0] = 0x77; /* F8 */
    cfg->kbd_prio = 1;
    cfg->force_walk = 1;
    cfg->respawn_hold = 20.0f;
    cfg->getup_hold = 2.0f;
    cfg->pad_prio = 1;
    cfg->pad_view = 0;      /* v1.8.170 : plus de bouton de vue du plugin (clic du stick droit : s'accroupir) */
    cfg->pad_jump = 0x1000; /* A */
    cfg->pad_getup = 0x8000;  /* Y */
    cfg->pad_bike = 0x8000;   /* Y */
    cfg->pad_dismount = 0x8000; /* Y gardé */
    cfg->pad_run = PAD_RT;
    cfg->dismount_hold = 0.8f;
    cfg->dismount_fall = 0;
    cfg->dismount_crash = 1;
    cfg->dismount_fade = 0;
    cfg->crider_arms = 0;
}

/* ---------- noms des touches ---------- */

typedef struct { const char *name; int code; } NamedCode;

/* découpe s aux séparateurs (comme strtok_r, absent de MinGW en C99) */
static char *next_tok(char **s, const char *delims)
{
    char *b = *s, *e;
    if (!b)
        return NULL;
    b += strspn(b, delims);
    if (!*b) {
        *s = NULL;
        return NULL;
    }
    e = b + strcspn(b, delims);
    if (*e) {
        *e = 0;
        *s = e + 1;
    } else {
        *s = NULL;
    }
    return b;
}

static const NamedCode k_keys[] = {
    { "SHIFT", 0x10 }, { "MAJ", 0x10 }, { "CTRL", 0x11 }, { "ALT", 0x12 }, { "SPACE", 0x20 }, { "ESPACE", 0x20 },
    { "ENTER", 0x0D }, { "ENTREE", 0x0D }, { "TAB", 0x09 }, { "BACKSPACE", 0x08 }, { "UP", 0x26 },
    { "HAUT", 0x26 }, { "DOWN", 0x28 }, { "BAS", 0x28 }, { "LEFT", 0x25 }, { "GAUCHE", 0x25 }, { "RIGHT", 0x27 },
    { "DROITE", 0x27 }, { "DELETE", 0x2E }, { "SUPPR", 0x2E }, { "INSERT", 0x2D }, { "HOME", 0x24 }, { "END", 0x23 },
    { "PAGEUP", 0x21 }, { "PAGEDOWN", 0x22 }, { "MOUSE4", 0x05 }, { "MOUSE5", 0x06 }, { "MOUSEMID", 0x04 },
    { "NONE", 0 }, { "AUCUNE", 0 },
};

static const NamedCode k_pads[] = {
    { "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 }, { "LB", 0x0100 }, { "RB", 0x0200 },
    { "BACK", 0x0020 }, { "START", 0x0010 }, { "LS", 0x0040 }, { "RS", 0x0080 }, { "DUP", 0x0001 },
    { "DDOWN", 0x0002 }, { "DLEFT", 0x0004 }, { "DRIGHT", 0x0008 }, { "RT", PAD_RT }, { "LT", PAD_LT },
    { "NONE", 0 }, { "AUCUN", 0 },
};

static int parse_key(const char *s)
{
    size_t i;
    char up[32];
    for (i = 0; s[i] && i + 1 < sizeof(up); ++i)
        up[i] = (char)toupper((unsigned char)s[i]);
    up[i] = 0;
    if (strlen(up) == 1 && ((up[0] >= 'A' && up[0] <= 'Z') || (up[0] >= '0' && up[0] <= '9')))
        return up[0];
    if (up[0] == 'F' && up[1] >= '1' && up[1] <= '9' && atoi(up + 1) >= 1 && atoi(up + 1) <= 12)
        return 0x70 + atoi(up + 1) - 1;
    if (!strncmp(up, "NUM", 3) && up[3] >= '0' && up[3] <= '9' && !up[4])
        return 0x60 + (up[3] - '0');
    for (i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); ++i)
        if (!strcmp(up, k_keys[i].name))
            return k_keys[i].code;
    if (up[0] == '0' && up[1] == 'X')
        return (int)strtol(up, NULL, 16);
    return -1;
}

static int parse_pad(const char *s)
{
    size_t i;
    char up[32];
    int m = 0;
    char *tok, *save;
    for (i = 0; s[i] && i + 1 < sizeof(up); ++i)
        up[i] = (char)toupper((unsigned char)s[i]);
    up[i] = 0;
    save = up;
    /* plusieurs boutons possibles : "LB+RB" (tous à la fois) */
    while ((tok = next_tok(&save, "+ ,")) != NULL) {
        int found = -1;
        for (i = 0; i < sizeof(k_pads) / sizeof(k_pads[0]); ++i)
            if (!strcmp(tok, k_pads[i].name))
                found = k_pads[i].code;
        if (found < 0)
            return -1;
        m |= found;
    }
    return m;
}

const char *cfg_key_name(int vk)
{
    static char buf[8][16];
    static int n = 0;
    size_t i;
    char *b = buf[n++ & 7];
    if (vk <= 0)
        return "aucune";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        b[0] = (char)vk;
        b[1] = 0;
        return b;
    }
    if (vk >= 0x70 && vk <= 0x7B) {
        snprintf(b, 16, "F%d", vk - 0x70 + 1);
        return b;
    }
    for (i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); ++i)
        if (k_keys[i].code == vk)
            return k_keys[i].name;
    snprintf(b, 16, "0x%02X", vk);
    return b;
}

const char *cfg_pad_name(int mask)
{
    static char buf[4][48];
    static int n = 0;
    size_t i;
    char *b = buf[n++ & 3];
    b[0] = 0;
    if (!mask)
        return "aucun";
    for (i = 0; i < sizeof(k_pads) / sizeof(k_pads[0]); ++i)
        if (k_pads[i].code && (mask & k_pads[i].code) == k_pads[i].code) {
            if (b[0])
                strncat(b, "+", 47 - strlen(b));
            strncat(b, k_pads[i].name, 47 - strlen(b));
        }
    return b;
}

typedef enum { T_FLOAT, T_INT, T_KEY, T_PAD, T_STR } FieldType;

typedef struct {
    const char *key;
    FieldType type;
    size_t offset;
    int level; /* 0 = fixé dans le plugin, 1 = touches / réseau (MXRP_Ragdoll.ini), 2 = menu P */
} Field;

#define F(name, member, lv) { name, T_FLOAT, offsetof(PluginConfig, member), lv }
#define I(name, member, lv) { name, T_INT, offsetof(PluginConfig, member), lv }
#define K(name, member, lv) { name, T_KEY, offsetof(PluginConfig, member), lv }
#define P(name, member, lv) { name, T_PAD, offsetof(PluginConfig, member), lv }
#define STR(name, member, lv) { name, T_STR, offsetof(PluginConfig, member), lv }

static const Field k_fields[] = {
    I("enabled", enabled, 0),
    I("debug_window", debug_window, 0),
    F("window_x", window_x, 0),
    F("window_y", window_y, 0),
    F("window_w", window_w, 0),
    F("window_h", window_h, 0),
    F("window_scale", window_scale, 0),
    F("screen_aspect", screen_aspect, 0),
    F("hide_delay", hide_delay, 0),
    I("log", log, 0),
    I("skeleton_hook", skeleton_hook, 0),
    I("skeleton_scan", skeleton_scan, 0),
    F("scan_radius", scan_radius, 0),
    I("write_test", write_test, 0),
    F("write_lift", write_lift, 0),
    I("ragdoll_3d", ragdoll_3d, 0),
    F("anchor_strength", anchor_strength, 0),
    F("body_ground", body_ground, 0),
    I("walking", walking, 0),
    F("getup_delay", getup_delay, 0),
    F("foot_height", foot_height, 0),
    F("rider_lift", rider_lift, 0),
    F("head_down", head_down, 0),
    F("stand_lean", stand_lean, 0),
    I("replay_record", replay_record, 0),
    I("replay_play", replay_play, 0),
    I("walk_camera", walk_camera, 0),
    I("bike_lift", bike_lift, 0),
    I("pad_respawn", pad_respawn, 0),
    I("testing_only", testing_only, 0),
    I("online_visual", online_visual, 0),
    I("online_full", online_full, 0),
    I("reseau", net_on, 1),
    I("reseau_hote", net_host, 1),
    STR("reseau_ip", net_ip, 1),
    I("reseau_port", net_port, 1),
    F("camera_distance", camera_distance, 0),
    F("camera_height", camera_height, 0),
    F("mouse_sensitivity", mouse_sensitivity, 2),
    F("fov_1re_personne", fov_fp, 2), /* v1.8.162 */
    I("bike_marker", bike_marker, 2),
    I("barres_progression", hold_msgs, 2), /* v1.8.111 */
    I("mises_a_jour_auto", auto_update, 2), /* v1.8.172 */
    I("maj_test", update_test, 1), /* v1.8.173 */
    I("maj_demander", update_ask, 2), /* v1.8.175 */
    I("expulsion_auto", auto_kick, 2), /* v1.8.200 */
    I("expulsion_info", auto_kick_info, 2), /* v1.8.200 */
    I("multi", online_ok, 2),            /* v1.8.204 */
    I("multi_demande", online_asked, 2), /* v1.8.204 */
    I("touches_essai", test_keys, 2),
    I("cones_3d", cones_3d, 2),
    I("objets_optimises", objects_opti, 2),
    I("boutons_manette", pad_style, 2), /* v1.8.107 */
    I("tutoriel", tutorial, 2), /* v1.8.110 */
    F("cones_hauteur", cones_lift, 2),
    I("fists", fists, 0),
    I("language", language, 2),
    K("key_forward", key_forward, 1),
    K("key_back", key_back, 1),
    K("key_left", key_left, 1),
    K("key_right", key_right, 1),
    K("key_run", key_run, 1),
    K("key_getup", key_getup, 1),
    K("key_bike", key_bike, 1),
    K("key_dismount", key_dismount, 1),
    K("key_menu", key_menu, 1),
    K("key_view", key_view, 1),
    K("key_jump", key_jump, 1),
    K("key_crouch", key_crouch, 1),
    P("pad_crouch", pad_crouch, 1),
    K("key_prio_kbd", key_prio_kbd, 1),
    K("key_prio_pad", key_prio_pad, 1),
    P("pad_prio_pad", pad_prio_pad, 1),
    I("clavier_prio", kbd_prio, 1),
    I("manette_prio", pad_prio, 1),
    I("marche_forcee", force_walk, 1),
    F("respawn_appui_long", respawn_hold, 1),
    F("releve_appui", getup_hold, 1),
    P("pad_view", pad_view, 1),
    P("pad_jump", pad_jump, 1),
    P("pad_getup", pad_getup, 1),
    P("pad_bike", pad_bike, 1),
    P("pad_dismount", pad_dismount, 1),
    P("pad_run", pad_run, 1),
    F("dismount_hold", dismount_hold, 1),
    I("dismount_fall", dismount_fall, 1),
    I("dismount_crash", dismount_crash, 1),
    I("dismount_fade", dismount_fade, 1),
    I("crider_arms", crider_arms, 0),
    F("gravity", physics.gravity, 0),
    F("damping", physics.damping, 0),
    F("ground_friction", physics.ground_friction, 0),
    F("ground_bounce", physics.ground_bounce, 0),
    F("point_radius", physics.point_radius, 0),
    F("ground_offset", physics.ground_offset, 0),
    F("substep", physics.substep, 0),
    I("iterations", physics.iterations, 0),
    F("rest_speed", physics.rest_speed, 0),
    F("rest_time", physics.rest_time, 0),
    I("rot_transpose", physics.rot_transpose, 0),
};

static char *trim(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s))
        ++s;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return s;
}

static int apply_line(PluginConfig *cfg, const char *line, int max_level);

int cfg_apply_line(PluginConfig *cfg, const char *line)
{
    return apply_line(cfg, line, 99);
}

/* v0.41.22 : seuls les réglages de niveau <= max_level sont lus ; les autres restent ceux du
   plugin, même si quelqu'un les ajoute à la main dans le fichier */
static int apply_line(PluginConfig *cfg, const char *line, int max_level)
{
    char buf[256];
    char *eq, *key, *value, *comment;
    size_t i;

    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    key = trim(buf);
    if (*key == '\0' || *key == ';' || *key == '#' || *key == '[')
        return 0;

    eq = strchr(key, '=');
    if (!eq)
        return 0;
    *eq = '\0';
    value = eq + 1;
    comment = strpbrk(value, ";#");
    if (comment)
        *comment = '\0';
    key = trim(key);
    value = trim(value);

    for (i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); ++i) {
        const Field *f = &k_fields[i];
        char *p = (char *)cfg + f->offset;
        if (strcmp(key, f->key) != 0)
            continue;
        if (f->level > max_level || (f->level == 0 && max_level < 99))
            return 0;
        if (f->type == T_FLOAT) {
            *(float *)p = (float)atof(value);
        } else if (f->type == T_INT) {
            *(int *)p = atoi(value);
        } else if (f->type == T_STR) {
            snprintf(p, 64, "%s", value);
        } else if (f->type == T_KEY) {
            /* une ou deux touches : "Z W" */
            char tmp[64], *tok, *save = tmp;
            int k = 0, codes[2] = { 0, 0 };
            strncpy(tmp, value, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            while (k < 2 && (tok = next_tok(&save, " ,")) != NULL) {
                int c = parse_key(tok);
                if (c < 0)
                    return 0;
                codes[k++] = c;
            }
            memcpy(p, codes, sizeof(codes));
        } else {
            int m = parse_pad(value);
            if (m < 0)
                return 0;
            *(int *)p = m;
        }
        return 1;
    }
    return 0;
}

int cfg_load(PluginConfig *cfg, const char *path, int max_level)
{
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!strncmp(p, "key_crouch", 10))
            cfg->crouch_seen = 1;
        apply_line(cfg, line, max_level);
    }
    fclose(f);
    /* v1.8.170 : réglages d'avant l'accroupi : Maj courait, le clic du stick droit changeait la vue :
       courir passe sur Ctrl, la vue du plugin sur la manette est enlevée (celle du jeu et V restent) */
    if (!cfg->crouch_seen) {
        int i;
        for (i = 0; i < 2; ++i)
            if (cfg->key_run[i] == cfg->key_crouch[0])
                cfg->key_run[i] = 0x11;
        if (cfg->pad_view == cfg->pad_crouch)
            cfg->pad_view = 0;
    }
    return 1;
}

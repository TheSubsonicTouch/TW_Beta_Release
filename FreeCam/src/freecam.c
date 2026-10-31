#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freecam.h"

#define FC_PI 3.14159265358979323846f
#define FC_DEG (FC_PI / 180.0f)

/* ---- small helpers ---------------------------------------------------------- */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int finitef_(float v)
{
    /* NaN != v, and +-inf is larger than any finite float */
    return v == v && v < 3.0e38f && v > -3.0e38f;
}

static void copy3(float dst[3], const float src[3]) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float len3(const float a[3]) { return sqrtf(dot3(a, a)); }

static void norm3(float a[3])
{
    float l = len3(a);
    if (l > 1e-6f) { a[0] /= l; a[1] /= l; a[2] /= l; }
}

/* ---- configuration ---------------------------------------------------------- */
void fc_config_defaults(fc_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = FC_MODE_CAMERA;
    strcpy(cfg->module, "TriadWars.exe");
    cfg->chain_len = 0;
    cfg->address = 0;
    cfg->view = 0;
    cfg->speed = 14.0f;          /* metres per second: a car is ~20, a run ~6 */
    cfg->sprint = 5.0f;
    cfg->slow = 0.2f;
    cfg->sensitivity = 0.0028f;  /* ~0.16 degrees per pixel */
    cfg->invert_y = 0;
    cfg->max_distance = 4000.0f;
    cfg->eye_height = 2.0f;
    cfg->restore = 1;
    cfg->toggle_vk = 0x14;   /* CapsLock */
    cfg->panic_vk = 0x79;    /* F10 */
}

/* strchr without dragging in the CRT's private alias */
static char *find_chr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return NULL;
}

/* strtok_r is not in C99 and MSVC calls it strtok_s, so roll a tiny one. */
static char *next_token(char **cursor, const char *delims)
{
    char *start = *cursor;
    char *p;
    if (!start) return NULL;
    start += strspn(start, delims);
    if (!*start) { *cursor = NULL; return NULL; }
    p = start + strcspn(start, delims);
    if (*p) { *p = 0; *cursor = p + 1; } else { *cursor = NULL; }
    return start;
}

static char *trim(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

static int parse_bool(const char *v, int *out)
{
    if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on")) { *out = 1; return 1; }
    if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "off")) { *out = 0; return 1; }
    return 0;
}

static int parse_float(const char *v, float *out)
{
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v || !end || *trim(end)) return 0;
    if (!finitef_((float)d)) return 0;
    *out = (float)d;
    return 1;
}

int fc_chain_parse(fc_config *cfg, const char *text)
{
    char buf[256];
    char *cursor, *part;
    long parsed[FC_MAX_CHAIN];
    int n = 0;

    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    cursor = buf;
    /* split on every comma, empty parts included: "0x10,,0x20" is a typo, not two hops */
    while (cursor && *cursor) {
        char *comma;
        part = cursor;
        comma = find_chr(part, ',');
        if (comma) { *comma = 0; cursor = comma + 1; } else { cursor = NULL; }
        {
            char *s = trim(part);
            char *end = NULL;
            long v;
            if (*s == '+') s++;                   /* "+0x58" and "0x58" mean the same */
            if (!*s) return 0;
            v = strtol(s, &end, 0);
            if (end == s || *trim(end) || v < 0) return 0;
            if (n >= FC_MAX_CHAIN) return 0;
            parsed[n++] = v;
        }
    }
    if (!n) return 0;
    memcpy(cfg->chain, parsed, (size_t)n * sizeof(parsed[0]));   /* commit only on success */
    cfg->chain_len = n;
    return 1;
}

typedef struct { const char *name; int vk; } fc_key_name_t;

static const fc_key_name_t fc_keys[] = {
    {"capslock", 0x14}, {"caps_lock", 0x14}, {"tab", 0x09}, {"space", 0x20},
    {"insert", 0x2d}, {"ins", 0x2d}, {"delete", 0x2e}, {"del", 0x2e},
    {"home", 0x24}, {"end", 0x23}, {"pageup", 0x21}, {"pagedown", 0x22},
    {"pause", 0x13}, {"scrolllock", 0x91}, {"numlock", 0x90},
    {"backspace", 0x08}, {"enter", 0x0d}, {"escape", 0x1b}, {"esc", 0x1b},
    {"shift", 0x10}, {"ctrl", 0x11}, {"control", 0x11}, {"alt", 0x12},
    {"mouse1", 0x01}, {"mouse2", 0x02}, {"mouse3", 0x04}, {"mouse4", 0x05},
    {"mouse5", 0x06}, {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28},
    {"f1", 0x70}, {"f2", 0x71}, {"f3", 0x72}, {"f4", 0x73}, {"f5", 0x74}, {"f6", 0x75},
    {"f7", 0x76}, {"f8", 0x77}, {"f9", 0x78}, {"f10", 0x79}, {"f11", 0x7a}, {"f12", 0x7b}
};

int fc_key_parse(const char *name, int *vk)
{
    char lower[32];
    size_t i, n;

    if (!name || !vk) return 0;
    n = strlen(name);
    if (!n || n >= sizeof(lower)) return 0;
    for (i = 0; i < n; i++) {
        char ch = name[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        lower[i] = ch;
    }
    lower[n] = 0;

    if (n > 2 && lower[0] == '0' && lower[1] == 'x') {          /* 0x2d */
        char *end = NULL;
        long v = strtol(lower, &end, 0);
        if (end && !*end && v > 0 && v < 0x100) { *vk = (int)v; return 1; }
        return 0;
    }
    if (n == 1) {                                               /* a, 1 */
        char ch = lower[0];
        if (ch >= 'a' && ch <= 'z') { *vk = 'A' + (ch - 'a'); return 1; }
        if (ch >= '0' && ch <= '9') { *vk = '0' + (ch - '0'); return 1; }
        return 0;
    }
    for (i = 0; i < sizeof(fc_keys) / sizeof(fc_keys[0]); i++) {
        if (!strcmp(lower, fc_keys[i].name)) { *vk = fc_keys[i].vk; return 1; }
    }
    return 0;
}

const char *fc_key_name(int vk)
{
    size_t i;
    for (i = 0; i < sizeof(fc_keys) / sizeof(fc_keys[0]); i++) {
        if (fc_keys[i].vk == vk) return fc_keys[i].name;
    }
    return "that key";
}

int fc_address_parse(fc_config *cfg, const char *text)
{
    char *end = NULL;
    const char *s = text ? text : "";
    unsigned long long v;
    if (*s == '+') s++;
    if (!*s) return 0;
    v = strtoull(s, &end, 0);
    if (end == s || *trim((char *)end) || v < 0x10000ull) return 0;
    cfg->address = v;
    return 1;
}

int fc_config_parse(fc_config *cfg, const char *text,
                    void (*warn)(void *ctx, int line, const char *msg), void *ctx)
{
    char buf[4096];
    char *cursor, *line, *section = (char *)"";
    int applied = 0, lineno = 0, bad = 0;

    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    cursor = buf;
    for (line = next_token(&cursor, "\n"); line; line = next_token(&cursor, "\n")) {
        char *s = trim(line), *eq, *key, *val;
        lineno++;
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) { if (warn) warn(ctx, lineno, "unterminated section");
                bad = 1;
                continue; }
            *close = 0;
            section = s + 1;
            continue;
        }
        eq = strchr(s, '=');
        if (!eq) { if (warn) warn(ctx, lineno, "expected key=value");
                bad = 1;
                continue; }
        *eq = 0;
        key = trim(s);
        val = trim(eq + 1);

        if (!strcmp(section, "freecam") && !strcmp(key, "mode")) {
            if (!strcmp(val, "camera")) cfg->mode = FC_MODE_CAMERA;
            else if (!strcmp(val, "player")) cfg->mode = FC_MODE_PLAYER;
            else { if (warn) warn(ctx, lineno, "mode is camera or player");
                bad = 1;
                continue; }
        } else if (!strcmp(section, "freecam") && !strcmp(key, "chain")) {
            if (*val && !fc_chain_parse(cfg, val)) {
                if (warn) warn(ctx, lineno, "chain looks like 0x23f25f8,0x58,0x80");
                bad = 1;
                continue;
            }
            if (!*val) { cfg->chain_len = 0; }
        } else if (!strcmp(section, "freecam") && !strcmp(key, "toggle")) {
            int v;
            if (!fc_key_parse(val, &v)) {
                if (warn) warn(ctx, lineno, "toggle is a key name, e.g. capslock or f6");
                bad = 1;
                continue;
            }
            cfg->toggle_vk = v;
        } else if (!strcmp(section, "freecam") && !strcmp(key, "panic")) {
            int v;
            if (!fc_key_parse(val, &v)) {
                if (warn) warn(ctx, lineno, "panic is a key name, e.g. f10");
                bad = 1;
                continue;
            }
            cfg->panic_vk = v;
        } else if (!strcmp(section, "freecam") && !strcmp(key, "address")) {
            if (!*val) { cfg->address = 0; }
            else if (!fc_address_parse(cfg, val)) {
                if (warn) warn(ctx, lineno, "address looks like 0x7ff6a1b2c3d0");
                bad = 1;
                continue;
            }
        } else if (!strcmp(section, "freecam") && !strcmp(key, "matrix")) {
            if (!strcmp(val, "world")) cfg->view = 0;
            else if (!strcmp(val, "view")) cfg->view = 1;
            else { if (warn) warn(ctx, lineno, "matrix is world or view");
                bad = 1;
                continue; }
        } else if (!strcmp(section, "game") && !strcmp(key, "module")) {
            snprintf(cfg->module, sizeof(cfg->module), "%s", val);
            if (!cfg->module[0]) { if (warn) warn(ctx, lineno, "empty module");
                bad = 1;
                continue; }
        } else if (!strcmp(section, "freecam") && !strcmp(key, "speed")) {
            float v;
            if (!parse_float(val, &v) || v <= 0.0f) {
                if (warn) warn(ctx, lineno, "speed must be greater than zero");
                bad = 1;
                continue;
            }
            cfg->speed = v;
        } else if (!strcmp(section, "freecam") && !strcmp(key, "sprint")) {
            float v;
            if (!parse_float(val, &v) || v < 1.0f) {
                if (warn) warn(ctx, lineno, "sprint is a multiplier of 1 or more");
                bad = 1;
                continue;
            }
            cfg->sprint = v;
        } else if (!strcmp(section, "freecam") && !strcmp(key, "slow")) {
            float v;
            if (!parse_float(val, &v) || v <= 0.0f || v > 1.0f) {
                if (warn) warn(ctx, lineno, "slow is a multiplier between 0 and 1");
                bad = 1;
                continue;
            }
            cfg->slow = v;
        } else if (!strcmp(section, "freecam") && !strcmp(key, "sensitivity")) {
            float v;
            if (!parse_float(val, &v) || v <= 0.0f) {
                if (warn) warn(ctx, lineno, "sensitivity must be greater than zero");
                bad = 1;
                continue;
            }
            cfg->sensitivity = v;
        } else if (!strcmp(section, "freecam") && !strcmp(key, "invert_y")) {
            int v;
            if (!parse_bool(val, &v)) {
                if (warn) warn(ctx, lineno, "invert_y is true or false");
                bad = 1;
                continue;
            }
            cfg->invert_y = v;
        } else if (!strcmp(section, "freecam") && !strcmp(key, "max_distance")) {
            float v;
            if (!parse_float(val, &v) || v <= 0.0f) {
                if (warn) warn(ctx, lineno, "max_distance must be greater than zero");
                bad = 1;
                continue;
            }
            cfg->max_distance = v;
        } else if (!strcmp(section, "freecam") && !strcmp(key, "eye_height")) {
            float v;
            if (!parse_float(val, &v)) {
                if (warn) warn(ctx, lineno, "eye_height is a number");
                bad = 1;
                continue;
            }
            cfg->eye_height = v;
        } else if (!strcmp(section, "freecam") && !strcmp(key, "restore")) {
            int v;
            if (!parse_bool(val, &v)) {
                if (warn) warn(ctx, lineno, "restore is true or false");
                bad = 1;
                continue;
            }
            cfg->restore = v;
        } else {
            if (warn) warn(ctx, lineno, "unknown setting, ignored");
            continue;
        }
        applied++;
    }
    return bad ? -1 : applied;
}

void *fc_chain_resolve(void *base, const fc_config *cfg, fc_read_fn read, void *ctx)
{
    unsigned char *p = (unsigned char *)base;
    int i;

    if (!p || !read || cfg->chain_len <= 0 || cfg->chain_len > FC_MAX_CHAIN) return NULL;
    for (i = 0; i + 1 < cfg->chain_len; i++) {
        void *next = NULL;
        if (!read(ctx, p + cfg->chain[i], &next, (unsigned)sizeof(next))) return NULL;
        if (!next) return NULL;
        p = (unsigned char *)next;
    }
    return p + cfg->chain[cfg->chain_len - 1];
}

/* ---- matrix maths ----------------------------------------------------------- */

void fc_mat_identity(fc_mat *m)
{
    int i;
    for (i = 0; i < 16; i++) m->m[i] = 0.0f;
    m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1.0f;
}

/* The world is Z-up, which is what UFG's engine uses (it is why this file said
 * "Y up" until build 6: with the wrong up axis every camera came out rolled 180
 * degrees, and an upside-down picture makes every control feel inverted).
 *
 * The camera frame is right-handed: right = forward x world up, up = right x forward,
 * and a camera looks down its own -Z, so the third axis of the frame is backwards,
 * w = -forward. TW_MenuLog.txt proves it: the world matrix at camera +0x80 for the
 * 14:10:09 camera reads
 *     row 0  -0.996  0.087 -0.000     (right)
 *     row 1  -0.014 -0.155  0.988     (up)
 *     row 2   0.086  0.984  0.156     (backwards)
 *     row 3  1411.382 -819.282 3.070  (eye)
 * and the view matrix at +0x40 is exactly its inverse: rows (r_x,u_x,w_x) ...
 *
 * yaw 0, pitch 0 looks along +Y; positive yaw turns left (towards -X); positive
 * pitch looks up (towards +Z). */
void fc_basis(float yaw, float pitch, float forward[3], float right[3], float up[3])
{
    float cy = cosf(yaw), sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);

    forward[0] = -sy * cp; forward[1] = cy * cp; forward[2] = sp;
    right[0] = cy;         right[1] = sy;        right[2] = 0.0f;

    /* up = right x forward, which is what a right-handed basis wants */
    up[0] = right[1] * forward[2] - right[2] * forward[1];
    up[1] = right[2] * forward[0] - right[0] * forward[2];
    up[2] = right[0] * forward[1] - right[1] * forward[0];
    norm3(forward); norm3(right); norm3(up);
}

void fc_mat_from_pose(fc_mat *m, const float pos[3], float yaw, float pitch, int view)
{
    float f[3], r[3], u[3], w[3];
    const float *rows[3];
    int i;

    fc_basis(yaw, pitch, f, r, u);
    /* the third axis points backwards; view 2 is for engines that keep +forward */
    for (i = 0; i < 3; i++) w[i] = (view == 2) ? f[i] : -f[i];

    if (view) {
        /* A view matrix is the inverse of the camera's world matrix: each row holds
         * the components of one basis vector, and the translation of a row is minus
         * that basis vector's dot with the eye. */
        rows[0] = r; rows[1] = u; rows[2] = w;
        for (i = 0; i < 3; i++) {
            m->m[0 + i] = rows[i][0];
            m->m[4 + i] = rows[i][1];
            m->m[8 + i] = rows[i][2];
            m->m[12 + i] = -(rows[i][0] * pos[0] + rows[i][1] * pos[1] + rows[i][2] * pos[2]);
        }
    } else {
        /* A world matrix keeps the basis as rows and the eye in the translation. */
        for (i = 0; i < 3; i++) {
            m->m[0 + i] = r[i];
            m->m[4 + i] = u[i];
            m->m[8 + i] = w[i];
        }
        m->m[12] = pos[0];
        m->m[13] = pos[1];
        m->m[14] = pos[2];
    }
    m->m[3] = m->m[7] = m->m[11] = 0.0f;
    m->m[15] = 1.0f;
}

int fc_mat_to_pose(const fc_mat *m, float pos[3], float *yaw, float *pitch, int view)
{
    float f[3];

    if (!fc_mat_valid(m)) return 0;
    if (view) {
        /* third block of a view matrix: the backwards axis, so forward = -it (view 2
         * is the engine that keeps +forward there) */
        f[0] = -m->m[2]; f[1] = -m->m[6]; f[2] = -m->m[10];
        if (view == 2) { f[0] = -f[0]; f[1] = -f[1]; f[2] = -f[2]; }
        /* each row of the view is a basis vector, so its eye component is the
         * negated dot of that row with the translation */
        pos[0] = -(m->m[0] * m->m[12] + m->m[1] * m->m[13] + m->m[2] * m->m[14]);
        pos[1] = -(m->m[4] * m->m[12] + m->m[5] * m->m[13] + m->m[6] * m->m[14]);
        pos[2] = -(m->m[8] * m->m[12] + m->m[9] * m->m[13] + m->m[10] * m->m[14]);
    } else {
        /* row 2 of a world matrix is the backwards axis, so forward = -it */
        f[0] = -m->m[8]; f[1] = -m->m[9]; f[2] = -m->m[10];
        pos[0] = m->m[12]; pos[1] = m->m[13]; pos[2] = m->m[14];
    }
    norm3(f);
    /* forward = (-sin(yaw)cos(pitch), cos(yaw)cos(pitch), sin(pitch)), Z up */
    if (pitch) *pitch = asinf(clampf(f[2], -1.0f, 1.0f));
    if (yaw) {
        float flat = sqrtf(f[0] * f[0] + f[1] * f[1]);
        *yaw = flat > 1e-5f ? atan2f(-f[0], f[1]) : 0.0f;
    }
    return 1;
}

int fc_spawn_matrix(fc_mat *out, const fc_mat *player, float metres)
{
    float pos[3], yaw = 0.0f, pitch = 0.0f, f[3], r[3], u[3];

    if (!out || !player) return 0;

    /* The player's own transform: where they are and which way they face. fc_mat_to_pose
     * is the same reader the camera uses on the game's matrices, so this works on the
     * layout the game actually writes, not on an assumed one. */
    if (!fc_mat_to_pose(player, pos, &yaw, &pitch, 0)) return 0;

    /* Forward, flat: pitch belongs to the camera, not to the thing being placed. A pawn
     * set down while the player looks at their feet would otherwise end up half in the
     * road, and one placed while they look at the sky would be over their head. */
    fc_basis(yaw, 0.0f, f, r, u);
    (void)r; (void)u;

    pos[0] += f[0] * metres;
    pos[1] += f[1] * metres;
    pos[2] += f[2] * metres;

    /* A world transform: basis as rows, position in the translation. Upright, facing
     * where the player faces. */
    fc_mat_from_pose(out, pos, yaw, 0.0f, 0);
    return 1;
}

int fc_pose_from_view(const fc_mat *view, float pos[3], float *yaw, float *pitch, int *sign)
{
    float f[3], eye[3], y, pi;
    int s;

    if (!view || !fc_mat_valid(view)) return 0;

    /* The third block (m[2], m[6], m[10]) holds the camera's backwards axis, so
     * forward = -it for sign 1 and +it for sign 2. The eye does not depend on the
     * sign: each row of the view is a basis vector, so its eye component is the
     * negated dot of that row with the translation. */
    eye[0] = -(view->m[0] * view->m[12] + view->m[1] * view->m[13] + view->m[2] * view->m[14]);
    eye[1] = -(view->m[4] * view->m[12] + view->m[5] * view->m[13] + view->m[6] * view->m[14]);
    eye[2] = -(view->m[8] * view->m[12] + view->m[9] * view->m[13] + view->m[10] * view->m[14]);

    /* An exact match first: the pose rebuilds the matrix it came from. */
    for (s = 1; s <= 2; s++) {
        fc_mat want;
        float worst = 0.0f;
        int k;

        for (k = 0; k < 3; k++) f[k] = (s == 1) ? -view->m[2 + 4 * k] : view->m[2 + 4 * k];
        norm3(f);
        pi = asinf(clampf(f[2], -1.0f, 1.0f));
        y = (f[0] * f[0] + f[1] * f[1]) > 1e-10f ? atan2f(-f[0], f[1]) : 0.0f;

        fc_mat_from_pose(&want, eye, y, pi, s);
        for (k = 0; k < 16; k++) {
            float d = want.m[k] - view->m[k];
            if (d < 0.0f) d = -d;
            if (d > worst) worst = d;
        }
        if (worst > 0.02f) continue;

        if (pos) copy3(pos, eye);
        if (yaw) *yaw = y;
        if (pitch) *pitch = pi;
        if (sign) *sign = s;
        return 1;
    }

    /* Not exact: a camera with roll cannot be written as yaw and pitch, but its
     * forward row still says which way it looks, which is what the flight needs. */
    for (s = 1; s <= 2; s++) {
        fc_mat want;
        float worst = 0.0f;
        int k;

        for (k = 0; k < 3; k++) f[k] = (s == 1) ? -view->m[2 + 4 * k] : view->m[2 + 4 * k];
        norm3(f);
        pi = asinf(clampf(f[2], -1.0f, 1.0f));
        y = (f[0] * f[0] + f[1] * f[1]) > 1e-10f ? atan2f(-f[0], f[1]) : 0.0f;

        fc_mat_from_pose(&want, eye, y, pi, s);
        for (k = 0; k < 3; k++) {           /* the forward (third) block only */
            float d = want.m[2 + 4 * k] - view->m[2 + 4 * k];
            if (d < 0.0f) d = -d;
            if (d > worst) worst = d;
        }
        if (worst > 0.25f) continue;

        if (pos) copy3(pos, eye);
        if (yaw) *yaw = y;
        if (pitch) *pitch = pi;
        if (sign) *sign = s;
        return 1;
    }
    return 0;
}

int fc_mat_valid(const fc_mat *m)
{
    float r[3], u[3], f[3];
    int i;

    for (i = 0; i < 16; i++) if (!finitef_(m->m[i])) return 0;
    if (fabsf(m->m[15] - 1.0f) > 0.01f) return 0;
    if (fabsf(m->m[3]) > 0.01f || fabsf(m->m[7]) > 0.01f || fabsf(m->m[11]) > 0.01f) return 0;
    for (i = 0; i < 3; i++) { r[i] = m->m[0 + i]; u[i] = m->m[4 + i]; f[i] = m->m[8 + i]; }
    if (fabsf(len3(r) - 1.0f) > 0.02f) return 0;
    if (fabsf(len3(u) - 1.0f) > 0.02f) return 0;
    if (fabsf(len3(f) - 1.0f) > 0.02f) return 0;
    if (fabsf(dot3(r, u)) > 0.02f) return 0;
    if (fabsf(dot3(r, f)) > 0.02f) return 0;
    if (fabsf(dot3(u, f)) > 0.02f) return 0;
    return 1;
}

void fc_mat_transpose(const fc_mat *m, fc_mat *out)
{
    fc_mat t;
    int r, c;
    if (!m || !out) return;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) t.m[r * 4 + c] = m->m[c * 4 + r];
    *out = t;
}

void fc_mat_mul(const fc_mat *a, const fc_mat *b, fc_mat *out)
{
    fc_mat r;
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r.m[i * 4 + j] = a->m[i * 4 + 0] * b->m[0 * 4 + j] +
                             a->m[i * 4 + 1] * b->m[1 * 4 + j] +
                             a->m[i * 4 + 2] * b->m[2 * 4 + j] +
                             a->m[i * 4 + 3] * b->m[3 * 4 + j];
        }
    }
    *out = r;
}

int fc_mat_near_identity(const fc_mat *m, float tol)
{
    int i, j;
    if (!m) return 0;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            float want = (i == j) ? 1.0f : 0.0f;
            float d = m->m[i * 4 + j] - want;
            if (d < 0.0f) d = -d;
            if (!(d <= tol)) return 0;
        }
    }
    return 1;
}

static float fc_dist3(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return (float)sqrt((double)(dx * dx + dy * dy + dz * dz));
}

/* A projection matrix is the odd one out in a camera's matrix block: perspective
 * projections end in a zero (m[15] == 0) and carry a +-1 in m[11], orthographic ones
 * keep a 1 there but have no orthonormal basis. Either way it is not a transform. */
int fc_mat_is_projection(const fc_mat *m)
{
    int i;
    if (!m) return 0;
    for (i = 0; i < 16; i++) if (!finitef_(m->m[i])) return 0;
    if (fc_mat_valid(m)) return 0;                       /* it is a transform */
    if (fabsf(m->m[15]) < 0.001f) return 1;              /* perspective */
    if (fabsf(fabsf(m->m[11]) - 1.0f) < 0.001f) return 1;/* perspective, other layout */
    if (fabsf(m->m[15] - 1.0f) < 0.001f &&
        fabsf(m->m[3]) < 0.001f && fabsf(m->m[7]) < 0.001f && fabsf(m->m[11]) < 0.001f)
        return 1;                                        /* orthographic */
    return 0;
}

int fc_view_sign(const fc_mat *world, const fc_mat *view)
{
    float pos[3], yaw, pitch;
    int sign;

    if (!world || !view) return 0;
    if (!fc_mat_valid(world) || !fc_mat_valid(view)) return 0;
    if (!fc_mat_to_pose(world, pos, &yaw, &pitch, 0)) return 0;
    for (sign = 1; sign <= 2; sign++) {
        fc_mat want;
        float worst = 0.0f, worstT = 0.0f;
        int k;
        fc_mat_from_pose(&want, pos, yaw, pitch, sign);
        for (k = 0; k < 12; k++) {
            float d = want.m[k] - view->m[k];
            if (d < 0.0f) d = -d;
            if (d > worst) worst = d;
        }
        /* the translation is a dot with an eye that can be thousands of units out, so
         * it gets its own tolerance: a wrong layout misses by hundreds, not by one */
        for (k = 12; k < 15; k++) {
            float d = want.m[k] - view->m[k];
            if (d < 0.0f) d = -d;
            if (d > worstT) worstT = d;
        }
        if (worst < 0.01f && worstT < 1.0f) return sign;
    }
    return 0;
}

int fc_pick_world_view(const fc_mat *cands, int count, const float near[3], float radius,
                       int *world_idx, int *view_idx)
{
    int i, j;
    float best = 0.0f;
    int found = 0;

    if (!cands || count < 2) return 0;
    /* The view matrix is the world one rebuilt from the same pose, so test exactly that
     * rather than a product. The world matrix also has to be standing next to the player,
     * which rules out every other transform in the scene. */
    for (i = 0; i < count; i++) {
        float pos[3], yaw, pitch, d;
        if (!fc_mat_valid(&cands[i])) continue;
        if (!fc_mat_to_pose(&cands[i], pos, &yaw, &pitch, 0)) continue;
        d = fc_dist3(pos, near);
        if (d > radius) continue;
        for (j = 0; j < count; j++) {
            if (i == j) continue;
            if (!fc_view_sign(&cands[i], &cands[j])) continue;
            if (found && d >= best) continue;
            best = d;
            *world_idx = i;
            *view_idx = j;
            found = 1;
        }
    }
    return found;
}

/* ---- state machine ---------------------------------------------------------- */
void fc_init(fc_state *st, const fc_config *cfg)
{
    fc_config copy = *cfg;   /* cfg may point into st; read it before the memset */

    memset(st, 0, sizeof(*st));
    st->cfg = copy;
    st->pitch = 0.0f;
    st->play_speed = 1.0f;
    fc_mat_identity(&st->saved);
}

void fc_capture(fc_state *st, const fc_mat *target)
{
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float yaw = st->yaw, pitch = 0.0f;

    if (target && fc_mat_to_pose(target, pos, &yaw, &pitch, st->cfg.view)) {
        st->saved = *target;
        copy3(st->pos, pos);
        copy3(st->anchor, pos);
        st->yaw = yaw;
        st->pitch = pitch;
    } else {
        copy3(st->pos, st->anchor);
    }
    st->have_pose = 1;
    st->moved = 0.0f;
}

int fc_set_active(fc_state *st, int on, const fc_mat *target)
{
    if (on == st->active) return 0;
    if (on) {
        /* Only fly if there is a sane camera to fly from; otherwise leave the game alone. */
        if (!target || !fc_mat_valid(target)) return 0;
        fc_capture(st, target);
        if (!st->have_pose) return 0;
        st->active = 1;
        st->frozen = 0;          /* a fresh flight is never frozen */

        st->keys[FC_FORWARD] = st->keys[FC_BACK] = st->keys[FC_LEFT] = st->keys[FC_RIGHT] = 0;
        st->keys[FC_UP] = st->keys[FC_DOWN] = 0;
    } else {
        st->active = 0;
        st->frozen = 0;
        st->playing = 0;
    }
    return 1;
}

int fc_toggle(fc_state *st, const fc_mat *target)
{
    return fc_set_active(st, !st->active, target);
}

int fc_step(fc_state *st, float dt, float mouse_dx, float mouse_dy, fc_mat *out)
{
    float f[3], r[3], u[3], step;
    float mult = 1.0f;
    int i;

    if (!st->active) return 0;

    /* a hitch must not teleport the camera across the map */
    if (!(dt > 0.0f)) return 0;
    if (dt > 0.1f) dt = 0.1f;

    /* frozen: the pose stays exactly as it is, whatever is being pressed */
    if (st->frozen) {
        fc_mat_from_pose(out, st->pos, st->yaw, st->pitch, st->cfg.view);
        return fc_mat_valid(out);
    }

    if (mouse_dx || mouse_dy) {
        /* Natural: the mouse moving down looks down. Windows reports screen y growing
         * downwards, so the pitch comes off the movement (it went on before build 5,
         * which read as an inverted mouse). invert_y flips it back for anyone who
         * prefers the old behaviour. */
        float s = st->cfg.sensitivity;
        st->yaw -= mouse_dx * s;
        st->pitch += (st->cfg.invert_y ? 1.0f : -1.0f) * mouse_dy * s;
        st->pitch = clampf(st->pitch, -1.5533f, 1.5533f);      /* ~+-89 degrees */
        while (st->yaw > FC_PI) st->yaw -= 2.0f * FC_PI;
        while (st->yaw < -FC_PI) st->yaw += 2.0f * FC_PI;
    }

    if (st->keys[FC_SPRINT]) mult = st->cfg.sprint;
    else if (st->keys[FC_SLOW]) mult = st->cfg.slow;
    step = st->cfg.speed * mult * dt;

    fc_basis(st->yaw, st->pitch, f, r, u);
    if (st->keys[FC_FORWARD]) for (i = 0; i < 3; i++) st->pos[i] += f[i] * step;
    if (st->keys[FC_BACK])    for (i = 0; i < 3; i++) st->pos[i] -= f[i] * step;
    if (st->keys[FC_RIGHT])   for (i = 0; i < 3; i++) st->pos[i] += r[i] * step;
    if (st->keys[FC_LEFT])    for (i = 0; i < 3; i++) st->pos[i] -= r[i] * step;
    if (st->keys[FC_UP])      st->pos[2] += step;   /* Z is up */
    if (st->keys[FC_DOWN])    st->pos[2] -= step;

    /* Safety: a flying camera that drifts off the map is worse than one that stops. */
    {
        float d[3] = { st->pos[0] - st->anchor[0], st->pos[1] - st->anchor[1], st->pos[2] - st->anchor[2] };
        if (len3(d) > st->cfg.max_distance) {
            float k = st->cfg.max_distance / len3(d);
            st->pos[0] = st->anchor[0] + d[0] * k;
            st->pos[1] = st->anchor[1] + d[1] * k;
            st->pos[2] = st->anchor[2] + d[2] * k;
        }
    }

    st->moved += step * ((st->keys[FC_FORWARD] || st->keys[FC_BACK] || st->keys[FC_LEFT] ||
                          st->keys[FC_RIGHT] || st->keys[FC_UP] || st->keys[FC_DOWN]) ? 1.0f : 0.0f);

    fc_mat_from_pose(out, st->pos, st->yaw, st->pitch, st->cfg.view);
    if (!fc_mat_valid(out)) return 0;
    return 1;
}

/* ---- following the player --------------------------------------------------- */

void fc_follow(fc_state *st, const float player[3], int on)
{
    if (!st) return;
    if (!on) {
        st->follow = 0;
        st->have_last_player = 0;
        st->have_last_angles = 0;
        return;
    }
    if (!player) return;
    st->follow = 1;
    copy3(st->last_player, player);
    st->have_last_player = 1;
    st->have_last_angles = 0;      /* the first angle seen is the one to count from */
}

int fc_following(const fc_state *st)
{
    return st && st->follow;
}

void fc_follow_update(fc_state *st, const float player[3])
{
    float d[3];
    int i;

    if (!st || !st->follow || !player) return;
    if (!st->have_last_player) {
        copy3(st->last_player, player);
        st->have_last_player = 1;
        return;
    }

    /* Move by the player's step rather than snapping to the offset: a camera that is
     * being flown by hand keeps that movement, and a frame in which the player did not
     * move does not disturb anything. */
    for (i = 0; i < 3; i++) d[i] = player[i] - st->last_player[i];
    copy3(st->last_player, player);
    for (i = 0; i < 3; i++) st->pos[i] += d[i];
}

void fc_follow_update_angles(fc_state *st, float yaw, float pitch)
{
    float dy, dp;

    if (!st || !st->follow) return;
    if (!st->have_last_angles) {
        st->last_player_yaw = yaw;
        st->last_player_pitch = pitch;
        st->have_last_angles = 1;
        return;
    }

    /* the short way round, so a player looking past +-180 degrees does not spin the
     * camera on the spot */
    dy = yaw - st->last_player_yaw;
    while (dy > FC_PI) dy -= 2.0f * FC_PI;
    while (dy < -FC_PI) dy += 2.0f * FC_PI;
    dp = pitch - st->last_player_pitch;
    st->last_player_yaw = yaw;
    st->last_player_pitch = pitch;

    st->yaw += dy;
    while (st->yaw > FC_PI) st->yaw -= 2.0f * FC_PI;
    while (st->yaw < -FC_PI) st->yaw += 2.0f * FC_PI;
    st->pitch = clampf(st->pitch + dp, -1.5533f, 1.5533f);
}

/* ---- looking at the player without moving ------------------------------------ */

void fc_angles_to(const float pos[3], const float target[3], float *yaw, float *pitch)
{
    float d[3];
    float flat;

    d[0] = target[0] - pos[0];
    d[1] = target[1] - pos[1];
    d[2] = target[2] - pos[2];
    norm3(d);
    flat = sqrtf(d[0] * d[0] + d[1] * d[1]);

    if (yaw) *yaw = flat > 1e-5f ? atan2f(-d[0], d[1]) : 0.0f;
    if (pitch) *pitch = asinf(clampf(d[2], -1.0f, 1.0f));
}

void fc_look_at(fc_state *st, int on)
{
    if (!st) return;
    st->looking = on ? 1 : 0;
}

int fc_looking_at(const fc_state *st)
{
    return st && st->looking;
}

int fc_aim_at(fc_state *st, const float target[3], float dt)
{
    /* How quickly the camera comes round. 0.14 s is about a third of a second to settle:
     * fast enough to keep up with a walking player, slow enough to read as a pan rather
     * than a cut. */
    const float tau = 0.14f;
    float want_yaw, want_pitch, dy, dp, k;

    if (!st || !st->looking || !target || !st->active) return 0;
    if (!(dt > 0.0f)) dt = 0.008f;
    if (dt > 0.1f) dt = 0.1f;

    fc_angles_to(st->pos, target, &want_yaw, &want_pitch);

    /* the short way round, so a player walking past +-180 degrees does not spin it */
    dy = want_yaw - st->yaw;
    while (dy > FC_PI) dy -= 2.0f * FC_PI;
    while (dy < -FC_PI) dy += 2.0f * FC_PI;
    dp = want_pitch - st->pitch;

    if (fabsf(dy) < 1e-5f && fabsf(dp) < 1e-5f) return 0;

    k = 1.0f - expf(-dt / tau);
    if (k > 1.0f) k = 1.0f;
    st->yaw += dy * k;
    st->pitch = clampf(st->pitch + dp * k, -1.5533f, 1.5533f);

    /* keep the yaw in range, so a long pan does not run away */
    while (st->yaw > FC_PI) st->yaw -= 2.0f * FC_PI;
    while (st->yaw < -FC_PI) st->yaw += 2.0f * FC_PI;
    return 1;
}

/* ---- freezing ---------------------------------------------------------------- */

void fc_freeze(fc_state *st, int on)
{
    if (!st) return;
    st->frozen = on ? 1 : 0;
    if (st->frozen) {
        /* a frozen camera does not move, so nothing it was flying to matters. A follow
         * that carried the camera along is dropped too - but the aiming kind (P while
         * frozen) is allowed, and is how a frozen camera watches the player. */
        st->playing = 0;
        st->play_time = 0.0f;
        st->have_last_player = 0;
        st->have_last_angles = 0;
        st->follow = 0;
    } else {
        /* thawed: the camera is free again, and aiming would keep pulling it round */
        st->looking = 0;
    }
}

int fc_frozen(const fc_state *st)
{
    return st && st->frozen;
}

/* ---- keyframes -------------------------------------------------------------- */

int fc_key_add(fc_state *st, float now)
{
    fc_keyframe *k;
    float t;

    if (!st || !st->active) return 0;
    if (st->path_count >= FC_MAX_KEYS) return 0;

    t = st->path_count == 0 ? 0.0f : now - st->path_clock;
    if (t < 0.0f) t = 0.0f;
    /* never let two keys share a moment: the interpolation divides by the gap */
    if (st->path_count > 0 && t <= st->path[st->path_count - 1].time + 0.02f)
        t = st->path[st->path_count - 1].time + 0.02f;

    if (st->path_count == 0) st->path_clock = now;

    k = &st->path[st->path_count++];
    copy3(k->pos, st->pos);
    k->yaw = st->yaw;
    k->pitch = st->pitch;
    k->time = t;
    return st->path_count;
}

void fc_path_resume(fc_state *st, float now)
{
    if (!st || st->path_count <= 0) return;
    /* The clock that fc_key_add() subtracts is the moment the first key of this path was
     * taken. A path restored from an earlier flight has no such moment any more, so it is
     * rebuilt from the last key: the next key added then follows it by however long the
     * camera really took, instead of 0.02 s later - which used to squeeze a whole scene
     * into a few milliseconds. */
    st->path_clock = now - st->path[st->path_count - 1].time;
}

void fc_path_clear(fc_state *st)
{
    if (!st) return;
    st->path_count = 0;
    st->playing = 0;
    st->play_time = 0.0f;
    st->path_clock = 0.0f;
}

int fc_playing(const fc_state *st)
{
    return st && st->playing;
}

int fc_play_start(fc_state *st)
{
    if (!st || !st->active) return 0;
    if (st->path_count < 2) return 0;
    st->playing = 1;
    st->play_time = 0.0f;
    return 1;
}

void fc_play_stop(fc_state *st)
{
    if (!st) return;
    st->playing = 0;
    st->play_time = 0.0f;
}

/* The ladder the arrow keys walk up and down. Small steps near 1x, bigger at the ends,
 * so a press is always a visible change. */
static const float kPlaySpeeds[] = {
    0.1f, 0.2f, 0.25f, 0.33f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f
};
#define FC_PLAY_SPEED_STEPS ((int)(sizeof(kPlaySpeeds) / sizeof(kPlaySpeeds[0])))

void fc_play_set_speed(fc_state *st, float speed)
{
    if (!st) return;
    if (!(speed > FC_PLAY_SPEED_MIN)) speed = FC_PLAY_SPEED_MIN;
    if (speed > FC_PLAY_SPEED_MAX) speed = FC_PLAY_SPEED_MAX;
    st->play_speed = speed;
}

float fc_play_speed(const fc_state *st)
{
    if (!st || !(st->play_speed > 0.0f)) return 1.0f;
    return st->play_speed;
}

float fc_play_speed_shift(fc_state *st, int faster)
{
    float now;
    int i, at = -1;

    if (!st) return 1.0f;
    now = fc_play_speed(st);

    /* find the rung we are on: the closest one, so a speed set by hand still steps */
    for (i = 0; i < FC_PLAY_SPEED_STEPS; i++) {
        if (at < 0 || fabsf(now - kPlaySpeeds[i]) < fabsf(now - kPlaySpeeds[at])) at = i;
    }
    if (faster) { if (at < FC_PLAY_SPEED_STEPS - 1) at++; }
    else        { if (at > 0) at--; }

    st->play_speed = kPlaySpeeds[at];
    return st->play_speed;
}

/* ---- playback ---------------------------------------------------------------- */

/* A path whose recorded times make no sense - two keys in the same instant, or times
 * that do not increase, which a path restored from an earlier flight can end up with -
 * would play in a few milliseconds and look like a playback that does nothing at all.
 * Such a path is played by spacing its keys evenly instead, so it always plays. */
#define FC_PLAY_FALLBACK_GAP 0.6f

/* The times playback uses, one per key, and the length of the whole path. */
static float play_times(const fc_state *st, float *t)
{
    const int n = st->path_count;
    int i, usable = n >= 2;

    for (i = 0; i < n; i++) t[i] = st->path[i].time;
    if (usable) {
        for (i = 1; i < n; i++) {
            if (!(t[i] > t[i - 1] + 0.001f)) { usable = 0; break; }
        }
    }
    if (usable && !(t[n - 1] > 0.05f)) usable = 0;

    if (!usable) {
        for (i = 0; i < n; i++) t[i] = (float)i * FC_PLAY_FALLBACK_GAP;
    }
    return n >= 2 ? t[n - 1] : 0.0f;
}

int fc_play_pose(const fc_state *st, float clock, float pos[3], float *yaw, float *pitch)
{
    float t[FC_MAX_KEYS], yawu[FC_MAX_KEYS];
    const fc_keyframe *path;
    float total;
    int n, i;

    if (!st || !pos) return 0;
    n = st->path_count;
    if (n < 2) return 0;
    path = st->path;
    total = play_times(st, t);
    if (!(total > 0.0f)) return 0;

    /* the recorded angles, with the yaw unwound along the path: a keyframe pair that
     * crosses +-180 degrees then turns the short way instead of all the way round */
    yawu[0] = path[0].yaw;
    for (i = 1; i < n; i++) {
        float d = path[i].yaw - path[i - 1].yaw;
        while (d > FC_PI) d -= 2.0f * FC_PI;
        while (d < -FC_PI) d += 2.0f * FC_PI;
        yawu[i] = yawu[i - 1] + d;
    }

    /* the ends are the keys themselves, so a playback always begins and finishes on a
     * pose that was recorded */
    if (!(clock > 0.0f)) {
        copy3(pos, path[0].pos);
        if (yaw) *yaw = path[0].yaw;
        if (pitch) *pitch = path[0].pitch;
        return 1;
    }
    if (clock >= total) {
        copy3(pos, path[n - 1].pos);
        if (yaw) *yaw = path[n - 1].yaw;
        if (pitch) *pitch = path[n - 1].pitch;
        return 1;
    }

    /* Between two keys: straight from one pose to the next, along the clock, which is
     * what the playback has always done - the camera travels exactly as fast as it was
     * flown and is already moving on the first frame. */
    for (i = 0; i + 1 < n; i++) {
        float span, u;

        if (clock < t[i] || clock > t[i + 1]) continue;
        span = t[i + 1] - t[i];
        u = span > 1e-6f ? (clock - t[i]) / span : 0.0f;

        pos[0] = path[i].pos[0] + (path[i + 1].pos[0] - path[i].pos[0]) * u;
        pos[1] = path[i].pos[1] + (path[i + 1].pos[1] - path[i].pos[1]) * u;
        pos[2] = path[i].pos[2] + (path[i + 1].pos[2] - path[i].pos[2]) * u;

        if (yaw) {
            float y = yawu[i] + (yawu[i + 1] - yawu[i]) * u;
            while (y > FC_PI) y -= 2.0f * FC_PI;
            while (y < -FC_PI) y += 2.0f * FC_PI;
            *yaw = y;
        }
        if (pitch) {
            *pitch = clampf(path[i].pitch + (path[i + 1].pitch - path[i].pitch) * u,
                            -1.5533f, 1.5533f);
        }
        return 1;
    }

    /* only reachable if a clock lands outside every segment; the last key is safe */
    copy3(pos, path[n - 1].pos);
    if (yaw) *yaw = path[n - 1].yaw;
    if (pitch) *pitch = path[n - 1].pitch;
    return 1;
}

int fc_play_step(fc_state *st, float dt, fc_mat *out)
{
    float t[FC_MAX_KEYS];
    float total;

    if (!st || !st->playing) return 0;
    if (st->path_count < 2) { st->playing = 0; return 0; }
    if (!(dt > 0.0f)) dt = 0.0f;

    st->play_time += dt * (st->play_speed > 0.0f ? st->play_speed : 1.0f);
    total = play_times(st, t);

    if (total <= 0.0f || st->play_time >= total) {
        /* land exactly on the last key and hand control back */
        fc_keyframe *last = &st->path[st->path_count - 1];
        copy3(st->pos, last->pos);
        st->yaw = last->yaw;
        st->pitch = last->pitch;
        st->playing = 0;
        st->play_time = 0.0f;
    } else {
        fc_play_pose(st, st->play_time, st->pos, &st->yaw, &st->pitch);
    }

    /* the pose being played is the camera's pose now, so turning it off, following the
     * player or taking another keyframe all continue from here */
    copy3(st->anchor, st->pos);
    st->have_pose = 1;
    fc_mat_from_pose(out, st->pos, st->yaw, st->pitch, st->cfg.view);
    return fc_mat_valid(out);
}

int fc_restore(fc_state *st, fc_mat *out)
{
    if (!st->cfg.restore || !st->have_pose) return 0;
    if (!fc_mat_valid(&st->saved)) return 0;
    *out = st->saved;
    return 1;
}

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

void fc_basis(float yaw, float pitch, float forward[3], float right[3], float up[3])
{
    float cy = cosf(yaw), sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);

    /* yaw 0, pitch 0 looks along -Z; positive yaw turns left */
    forward[0] = -sy * cp; forward[1] = sp; forward[2] = -cy * cp;
    right[0] = cy;         right[1] = 0.0f; right[2] = -sy;

    /* up = right x forward, which is what a right-handed basis wants */
    up[0] = right[1] * forward[2] - right[2] * forward[1];
    up[1] = right[2] * forward[0] - right[0] * forward[2];
    up[2] = right[0] * forward[1] - right[1] * forward[0];
    norm3(forward); norm3(right); norm3(up);
}

void fc_mat_from_pose(fc_mat *m, const float pos[3], float yaw, float pitch, int view)
{
    float f[3], r[3], u[3];
    int i;

    fc_basis(yaw, pitch, f, r, u);
    for (i = 0; i < 3; i++) {
        m->m[0 + i] = r[i];
        m->m[4 + i] = u[i];
        /* view 1 keeps -forward in row 2, view 2 keeps +forward: engines differ */
        m->m[8 + i] = (view == 0) ? f[i] : (view == 1 ? -f[i] : f[i]);
    }
    if (view) {
        /* a view matrix puts the camera at the origin: translation = -dot(basis, pos) */
        for (i = 0; i < 3; i++) {
            m->m[12 + i] = -(m->m[0 + i] * pos[0] + m->m[4 + i] * pos[1] + m->m[8 + i] * pos[2]);
        }
    } else {
        m->m[12] = pos[0];
        m->m[13] = pos[1];
        m->m[14] = pos[2];
    }
    m->m[3] = m->m[7] = m->m[11] = 0.0f;
    m->m[15] = 1.0f;
}

int fc_mat_to_pose(const fc_mat *m, float pos[3], float *yaw, float *pitch, int view)
{
    float r[3], u[3], f[3];
    int i;

    if (!fc_mat_valid(m)) return 0;
    for (i = 0; i < 3; i++) { r[i] = m->m[0 + i]; u[i] = m->m[4 + i]; f[i] = m->m[8 + i]; }
    if (view) { f[0] = -f[0]; f[1] = -f[1]; f[2] = -f[2]; }

    if (view) {
        /* t = -dot(row_i, eye) for each of the three rows, and eye = -(sum row_i * t_i) */
        float t[3] = { m->m[12], m->m[13], m->m[14] };
        float fr[3] = { m->m[8], m->m[9], m->m[10] };       /* the view row, not the flipped one */
        pos[0] = -(r[0] * t[0] + u[0] * t[1] + fr[0] * t[2]);
        pos[1] = -(r[1] * t[0] + u[1] * t[1] + fr[1] * t[2]);
        pos[2] = -(r[2] * t[0] + u[2] * t[1] + fr[2] * t[2]);
    } else {
        pos[0] = m->m[12]; pos[1] = m->m[13]; pos[2] = m->m[14];
    }
    norm3(f);
    if (pitch) *pitch = asinf(clampf(f[1], -1.0f, 1.0f));
    if (yaw) {
        /* forward = (-sin(yaw)cos(pitch), sin(pitch), -cos(yaw)cos(pitch)) */
        float flat = sqrtf(f[0] * f[0] + f[2] * f[2]);
        *yaw = flat > 1e-5f ? atan2f(-f[0], -f[2]) : 0.0f;
    }
    return 1;
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

int fc_view_sign(const fc_mat *world, const fc_mat *view)
{
    float pos[3], yaw, pitch;
    int sign;

    if (!world || !view) return 0;
    if (!fc_mat_valid(world) || !fc_mat_valid(view)) return 0;
    if (!fc_mat_to_pose(world, pos, &yaw, &pitch, 0)) return 0;
    for (sign = 1; sign <= 2; sign++) {
        fc_mat want;
        float worst = 0.0f;
        int k;
        fc_mat_from_pose(&want, pos, yaw, pitch, sign);
        for (k = 0; k < 16; k++) {
            float d = want.m[k] - view->m[k];
            if (d < 0.0f) d = -d;
            if (d > worst) worst = d;
        }
        if (worst < 0.01f) return sign;
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
    memset(st, 0, sizeof(*st));
    st->cfg = *cfg;
    st->pitch = 0.0f;
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
        st->keys[FC_FORWARD] = st->keys[FC_BACK] = st->keys[FC_LEFT] = st->keys[FC_RIGHT] = 0;
        st->keys[FC_UP] = st->keys[FC_DOWN] = 0;
    } else {
        st->active = 0;
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

    if (mouse_dx || mouse_dy) {
        float s = st->cfg.sensitivity;
        st->yaw -= mouse_dx * s;
        st->pitch += (st->cfg.invert_y ? -1.0f : 1.0f) * mouse_dy * s;
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
    if (st->keys[FC_UP])      st->pos[1] += step;
    if (st->keys[FC_DOWN])    st->pos[1] -= step;

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

int fc_restore(fc_state *st, fc_mat *out)
{
    if (!st->cfg.restore || !st->have_pose) return 0;
    if (!fc_mat_valid(&st->saved)) return 0;
    *out = st->saved;
    return 1;
}

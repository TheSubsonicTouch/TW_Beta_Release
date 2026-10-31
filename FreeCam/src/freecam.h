/* FreeCam core - portable, no Windows calls, so it can be unit tested on any machine.
 *
 * The Windows glue (win32.c) owns the clock, the keyboard, the process memory and the
 * DLL exports; everything with a rule in it lives here.
 *
 * Matrix convention: 16 floats, row major, rows 0..2 are the basis and row 3 is the
 * position. Two layouts can be written to memory:
 *
 *   world  rows = right, up, forward          (an object's transform)
 *   view   rows = right, up, -forward, and the position row is the negated dot product
 *          (a camera's view matrix)
 *
 * Which one the game's camera keeps is a property of the build, so it is a setting.
 */
#ifndef FREECAM_H
#define FREECAM_H

#define FC_KEY_COUNT 9
enum {
    FC_FORWARD = 0, FC_BACK, FC_LEFT, FC_RIGHT, FC_UP, FC_DOWN, FC_SPRINT, FC_SLOW, FC_LOOK
};

#define FC_MODE_CAMERA 0   /* move the camera; the player never moves */
#define FC_MODE_PLAYER 1   /* move the player; the game's camera follows (a fly mode) */

#define FC_MAX_CHAIN 6
#define FC_MAX_KEYS  64     /* keyframes in one recorded path */

typedef struct {
    float m[16];
} fc_mat;

/* One recorded pose. `time` is seconds since the first keyframe of the path, so a
 * playback can run at the speed it was recorded at. */
typedef struct {
    float pos[3];
    float yaw, pitch;
    float time;
} fc_keyframe;

typedef struct {
    int    mode;                 /* FC_MODE_* */
    char   module[64];           /* the executable the offsets are relative to */
    int    chain_len;            /* number of offsets in chain[] */
    long   chain[FC_MAX_CHAIN];  /* the last one is the matrix offset, the others are derefs */
    unsigned long long address;  /* a plain address for the matrix, if chain is empty */
    int    view;                 /* 1 = write a view matrix, 0 = a world matrix */
    float  speed;                /* units per second */
    float  sprint;               /* multiplier while sprinting */
    float  slow;                 /* multiplier while slow */
    float  sensitivity;          /* radians of look per pixel of mouse movement */
    int    invert_y;
    float  max_distance;         /* refuse to fly further than this from the anchor */
    float  eye_height;           /* the pose the camera starts at, above the anchor */
    int    restore;              /* put the camera back when free cam is switched off */
    int    toggle_vk;            /* virtual key that switches free cam on and off */
    int    panic_vk;             /* hold this to put the camera back at once */
} fc_config;

typedef struct {
    fc_config cfg;

    int    active;               /* free cam on */
    int    have_pose;
    int    caps;                 /* last Caps Lock state (edge detection) */
    float  yaw, pitch;           /* radians; yaw 0 looks along -Z */
    float  pos[3];               /* camera (or player) position we are driving */
    float  anchor[3];            /* where the target was when free cam was switched on */
    fc_mat saved;                /* the matrix as it was, for restore */
    int    keys[FC_KEY_COUNT];
    float  moved;                /* metres travelled since switching on (for the log) */

    /* follow the player: the camera keeps its offset and goes where the player goes */
    int    follow;
    float  last_player[3];       /* where the player was on the previous tick */
    int    have_last_player;

    /* the camera is held exactly where it is: keys and mouse are ignored and the
     * player gets to move, fight and interact while the view stays put */
    int    frozen;

    /* following also turns the camera with the player's own camera, so the framing
     * survives the player looking around */
    int    have_last_angles;

    /* aiming at the player without moving: the frozen camera's P */
    int    looking;
    float  last_player_yaw, last_player_pitch;

    /* how fast a recorded path plays back; 1.0 is the speed it was flown at */
    float  play_speed;

    /* a recorded path of poses, and the playback of it */
    int         path_count;
    fc_keyframe path[FC_MAX_KEYS];
    float       path_clock;      /* seconds since the first keyframe was taken */
    int         playing;
    float       play_time;
} fc_state;

/* ---- configuration ---------------------------------------------------------- */
void fc_config_defaults(fc_config *cfg);

/* Parse an ini body. Unknown keys are reported through `warn` (may be NULL) and
 * ignored; returns the number of settings applied, or -1 if a value is unusable. */
int fc_config_parse(fc_config *cfg, const char *text,
                    void (*warn)(void *ctx, int line, const char *msg), void *ctx);

/* "0x23f25f8,0x58,0x80" -> chain_len 3. Returns 1 on success, 0 if malformed. */
int fc_chain_parse(fc_config *cfg, const char *text);

/* "0x7ff6a1b2c3d0" -> cfg->address. Returns 1 on success. */
int fc_address_parse(fc_config *cfg, const char *text);

/* "capslock", "f10", "0x2d" -> a Windows virtual key code. 1 on success. */
int fc_key_parse(const char *name, int *vk);

/* The name of a virtual key, for the log ("CapsLock"). Never NULL. */
const char *fc_key_name(int vk);

/* Reads `size` bytes at `addr`. Returns 1 on success; this is how the chain walk
 * stays testable without touching real memory. */
typedef int (*fc_read_fn)(void *ctx, const void *addr, void *out, unsigned size);

/* base + chain[0], dereference, + chain[1], dereference, ... , + chain[n-1].
 * Returns the final address of the matrix, or NULL if a hop could not be read. */
void *fc_chain_resolve(void *base, const fc_config *cfg, fc_read_fn read, void *ctx);

/* ---- matrix maths ----------------------------------------------------------- */
void fc_mat_identity(fc_mat *m);
int  fc_mat_valid(const fc_mat *m);                    /* finite, orthogonal basis, sane size */
int  fc_mat_is_projection(const fc_mat *m);            /* a projection, not a transform */
/* `view` is 0 for a world matrix, 1 or 2 for the two view-matrix conventions. */
void fc_mat_from_pose(fc_mat *m, const float pos[3], float yaw, float pitch, int view);
int  fc_mat_to_pose(const fc_mat *m, float pos[3], float *yaw, float *pitch, int view);
/* The pose a view matrix was built from. A view matrix is all a renderer hands out,
 * and its third row may be -forward (sign 1) or +forward (sign 2), so both are tried
 * and the one that rebuilds the same matrix exactly wins. */
int  fc_pose_from_view(const fc_mat *view, float pos[3], float *yaw, float *pitch, int *sign);
void fc_basis(float yaw, float pitch, float forward[3], float right[3], float up[3]);
void fc_mat_mul(const fc_mat *a, const fc_mat *b, fc_mat *out);   /* out = a * b */
void fc_mat_transpose(const fc_mat *m, fc_mat *out);
int  fc_mat_near_identity(const fc_mat *m, float tol);

/* Which way round the engine stores a view matrix. A camera's view matrix is the
 * world matrix inverted, but the third row is either -forward (1) or +forward (2)
 * depending on the engine's handedness, and only the game's own pair can say which.
 * Returns that code, or 0 when `view` is not the inverse of `world` at all. */
int  fc_view_sign(const fc_mat *world, const fc_mat *view);

/* The game keeps a camera's world transform and its view matrix side by side, and a
 * view matrix is the inverse of the world one, so the pair multiplies to the identity.
 * `near` is where the player is: the world matrix is the one whose position row sits
 * closest to it. Returns 1 and fills the two indices when a pair is found. */
int  fc_pick_world_view(const fc_mat *cands, int count, const float near[3], float radius,
                        int *world_idx, int *view_idx);

/* ---- state machine ---------------------------------------------------------- */
void fc_init(fc_state *st, const fc_config *cfg);
int  fc_toggle(fc_state *st, const fc_mat *target);    /* Caps Lock edge; 1 = handled */
int  fc_set_active(fc_state *st, int on, const fc_mat *target);
void fc_capture(fc_state *st, const fc_mat *target);   /* remember what we found */

/* One tick: `dt` seconds, `mouse` pixel movement since the last tick, key states
 * already in st->keys. Fills `out` with the matrix to write and returns 1 when it
 * should be written, 0 when the camera is not ours or the move was refused. */
int  fc_step(fc_state *st, float dt, float mouse_dx, float mouse_dy, fc_mat *out);

/* The matrix to put back when free cam is switched off (1 = restore, 0 = nothing). */
int  fc_restore(fc_state *st, fc_mat *out);

/* ---- following the player --------------------------------------------------- */

/* Turn following on (the camera starts moving with the player from here) or off (it
 * stays where it is). The offset comes from the two positions at the moment it is
 * switched on, so the camera keeps whatever framing it had. */
void fc_follow(fc_state *st, const float player[3], int on);
int  fc_following(const fc_state *st);

/* Move the camera by however far the player moved since the previous call. Call this
 * after fc_step, every tick, while following. */
void fc_follow_update(fc_state *st, const float player[3]);

/* Turn with the player. `yaw` and `pitch` are the player's own camera angles this tick;
 * the camera turns by however much they changed since the previous call, so it keeps
 * its framing while the player looks around. Call it next to fc_follow_update. */
void fc_follow_update_angles(fc_state *st, float yaw, float pitch);

/* ---- looking at the player without moving ------------------------------------ */

/* Aim at the player: the camera keeps its position and turns (yaws and pitches) so the
 * player is in the middle of the picture. This is the frozen camera's follow (P while
 * the camera is held by O): it pans, it does not travel.
 *
 * fc_aim_at() turns toward `target` by a fraction of the way, so the pan is smooth
 * instead of a snap. Call it every tick with the same dt the camera is stepped with.
 * Returns 1 when it moved the camera's angles. */
void fc_look_at(fc_state *st, int on);
int  fc_looking_at(const fc_state *st);
int  fc_aim_at(fc_state *st, const float target[3], float dt);

/* The yaw and pitch that point from `pos` at `target`, in this file's convention:
 * forward = (-sin(yaw)cos(pitch), cos(yaw)cos(pitch), sin(pitch)). */
void fc_angles_to(const float pos[3], const float target[3], float *yaw, float *pitch);

/* ---- spawning things in front of the player ------------------------------------ */

/* Where a new object should appear: `metres` in front of the transform `player`, facing
 * the same way. The result is a world transform (basis as rows, position in m[12..14]),
 * laid out like the one the game keeps for the player, which is what the game's own
 * spawn call wants. Pitch is dropped, so the new object stands upright and stays at the
 * player's height instead of being pushed into the ground or into the air when the
 * player looks up or down.
 *
 * Returns 1 when `out` was written, 0 when `player` is not a usable transform (then
 * `out` is left alone, so a caller that ignores the return value cannot act on
 * garbage). */
int fc_spawn_matrix(fc_mat *out, const fc_mat *player, float metres);

/* ---- freezing ---------------------------------------------------------------- */

/* Hold the camera where it is. While frozen, fc_step ignores every key and the mouse
 * and hands back the same pose, so the player is free to do anything. */
void fc_freeze(fc_state *st, int on);
int  fc_frozen(const fc_state *st);

/* ---- keyframes -------------------------------------------------------------- */

/* Add the current pose as a keyframe. `now` is any monotonically increasing number of
 * seconds (the first call starts the clock). Returns the number of keyframes, or 0 if
 * the path is full or the camera is not flying. */
int  fc_key_add(fc_state *st, float now);

/* Forget the whole path (and stop any playback). */
void fc_path_clear(fc_state *st);

/* The path was restored from an earlier flight: carry on from where it stopped, so keys
 * added now follow the last one by the real time taken (see fc_key_add's clock). `now` is
 * the same clock the next fc_key_add() will be given. */
void fc_path_resume(fc_state *st, float now);

/* Start playing the path back. Needs at least two keyframes. Returns 1 if it started. */
int  fc_play_start(fc_state *st);
void fc_play_stop(fc_state *st);
int  fc_playing(const fc_state *st);

/* Advance the playback by dt and hand out the pose to draw. Returns 1 while it wrote
 * `out`. The last keyframe is played and then playback stops, leaving the camera there,
 * so pressing the key again simply plays it again from the start. */
int  fc_play_step(fc_state *st, float dt, fc_mat *out);

/* The pose the recorded path has `clock` seconds after its first keyframe: straight from
 * key to key along the clock, so the camera travels exactly as fast as it was flown, and
 * the yaw unwound along the path so a pair that crosses +-180 degrees turns the short way.
 * `clock` >= the path's duration gives the last keyframe. A path whose recorded times are
 * unusable (two keys in one instant, or out of order) is played with its keys spaced
 * evenly, so it always plays rather than finishing in a millisecond. Returns 1 when it
 * wrote the pose, 0 when there is no path to play. */
int  fc_play_pose(const fc_state *st, float clock, float pos[3], float *yaw, float *pitch);

/* How fast the path plays back: 1.0 is the speed it was flown at, 0.5 takes twice as
 * long, 2.0 twice as short. Clamped to FC_PLAY_SPEED_MIN..FC_PLAY_SPEED_MAX. */
#define FC_PLAY_SPEED_MIN 0.1f
#define FC_PLAY_SPEED_MAX 4.0f
void  fc_play_set_speed(fc_state *st, float speed);
float fc_play_speed(const fc_state *st);

/* One notch up (faster != 0) or down the ladder of speeds, from 0.1x to 4x. Returns the
 * speed it landed on. Slowing down and speeding up again comes back to the same place. */
float fc_play_speed_shift(fc_state *st, int faster);

#endif /* FREECAM_H */

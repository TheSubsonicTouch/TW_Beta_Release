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

typedef struct {
    float m[16];
} fc_mat;

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
void fc_mat_from_pose(fc_mat *m, const float pos[3], float yaw, float pitch, int view);
int  fc_mat_to_pose(const fc_mat *m, float pos[3], float *yaw, float *pitch, int view);
void fc_basis(float yaw, float pitch, float forward[3], float right[3], float up[3]);

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

#endif /* FREECAM_H */

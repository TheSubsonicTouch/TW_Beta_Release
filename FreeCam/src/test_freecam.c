/* Unit tests for the free camera core. Builds and runs anywhere with a C99 compiler:

       python FreeCam/tests/run_tests.py

   or, by hand:  cc -std=c99 -O2 -o t FreeCam/src/freecam.c FreeCam/src/test_freecam.c -lm && ./t
*/
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../src/freecam.h"

#define TEST_PI 3.14159265358979323846

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("FAIL %s:%d  ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (eps))

static void warn_collect(void *ctx, int line, const char *msg)
{
    int *count = (int *)ctx;
    (void)line;
    (void)msg;
    (*count)++;
}

static void test_defaults(void)
{
    fc_config cfg;
    fc_state st;

    fc_config_defaults(&cfg);
    CHECK(cfg.mode == FC_MODE_CAMERA, "default mode is camera");
    CHECK(cfg.chain_len == 0, "no chain by default");
    CHECK(cfg.speed > 1.0f && cfg.sprint > 1.0f && cfg.slow < 1.0f, "speed defaults");
    CHECK(cfg.sensitivity > 0.0f && cfg.sensitivity < 0.05f, "mouse sensitivity is small");
    CHECK(cfg.restore == 1, "the camera is put back by default");

    fc_init(&st, &cfg);
    CHECK(st.active == 0 && st.have_pose == 0, "starts idle");
    CHECK(st.keys[FC_FORWARD] == 0, "no keys held");
}

static void test_chain(void)
{
    fc_config cfg;
    fc_config_defaults(&cfg);

    CHECK(fc_chain_parse(&cfg, "0x23f25f8,0x58,0x80") == 1, "the debug menu's player chain parses");
    CHECK(cfg.chain_len == 3, "three hops");
    CHECK(cfg.chain[0] == 0x23f25f8 && cfg.chain[1] == 0x58 && cfg.chain[2] == 0x80, "values");

    CHECK(fc_chain_parse(&cfg, " +0x10 , +0x20 ") == 1, "plus signs and spaces");
    CHECK(cfg.chain_len == 2 && cfg.chain[0] == 0x10 && cfg.chain[1] == 0x20, "two hops");

    CHECK(fc_chain_parse(&cfg, "16,32,64") == 1, "decimal offsets are fine");
    CHECK(cfg.chain[0] == 16 && cfg.chain[2] == 64, "decimal values");

    CHECK(fc_chain_parse(&cfg, "") == 0, "empty chain rejected");
    CHECK(fc_chain_parse(&cfg, "0x10,,0x20") == 0, "empty hop rejected");
    CHECK(fc_chain_parse(&cfg, "0x10,zzz") == 0, "junk rejected");
    CHECK(fc_chain_parse(&cfg, "-0x10") == 0, "negative offset rejected");
    CHECK(fc_chain_parse(&cfg, "1,2,3,4,5,6,7") == 0, "overlong chain rejected");
    CHECK(cfg.chain_len == 3, "a rejected chain leaves the old one alone");
}

static void test_config(void)
{
    fc_config cfg;
    int warns = 0;
    const char *ini =
        "; a comment\n"
        "# another\n"
        "[game]\n"
        "module=TriadWars.exe\n"
        "[freecam]\n"
        "mode=camera\n"
        "chain=0x23f25f8,0x58,0x80\n"
        "matrix=view\n"
        "speed=22.5\n"
        "sprint=3\n"
        "slow=0.25\n"
        "sensitivity=0.003\n"
        "invert_y=true\n"
        "max_distance=1500\n"
        "eye_height=1.8\n"
        "restore=false\n";

    fc_config_defaults(&cfg);
    CHECK(fc_config_parse(&cfg, ini, warn_collect, &warns) >= 10, "every setting applied");
    CHECK(warns == 0, "a clean ini warns about nothing (%d warnings)", warns);
    CHECK(strcmp(cfg.module, "TriadWars.exe") == 0, "module");
    CHECK(cfg.mode == FC_MODE_CAMERA && cfg.view == 1, "camera mode, view matrix");
    CHECK(cfg.chain_len == 3, "chain from the ini");
    CHECK(NEAR(cfg.speed, 22.5, 1e-3) && NEAR(cfg.sprint, 3.0, 1e-3), "numbers");
    CHECK(NEAR(cfg.slow, 0.25, 1e-6), "slow factor");
    CHECK(cfg.invert_y == 1, "invert_y");
    CHECK(NEAR(cfg.max_distance, 1500.0, 1e-3), "max_distance");
    CHECK(cfg.restore == 0, "restore off");

    /* a player-mode ini, as shipped in FreeCam.player.ini */
    {
        fc_config p;
        int w = 0;
        fc_config_defaults(&p);
        CHECK(fc_config_parse(&p, "[freecam]\nmode=player\nchain=0x23f25f8,0x58,0x80\nmatrix=world\n",
                              warn_collect, &w) >= 2, "player mode parses");
        CHECK(p.mode == FC_MODE_PLAYER && p.view == 0, "player mode is a world matrix");
        CHECK(w == 0, "no warnings");
    }

    /* key names, so the toggle is not hard coded */
    {
        int vk = 0;
        fc_config k;
        int w = 0;
        CHECK(fc_key_parse("CapsLock", &vk) && vk == 0x14, "CapsLock");
        CHECK(fc_key_parse("f10", &vk) && vk == 0x79, "f10");
        CHECK(fc_key_parse("0x2d", &vk) && vk == 0x2d, "a raw code");
        CHECK(fc_key_parse("k", &vk) && vk == 'K', "a letter");
        CHECK(fc_key_parse("7", &vk) && vk == '7', "a digit");
        CHECK(!fc_key_parse("nope", &vk), "a made up key is refused");
        CHECK(!fc_key_parse("", &vk), "an empty key is refused");
        CHECK(!strcmp(fc_key_name(0x14), "capslock"), "the log can name the key");
        fc_config_defaults(&k);
        CHECK(k.toggle_vk == 0x14 && k.panic_vk == 0x79, "default toggle and panic");
        CHECK(fc_config_parse(&k, "[freecam]\ntoggle=f6\npanic=delete\n", warn_collect, &w) == 2,
              "both keys parse");
        CHECK(k.toggle_vk == 0x75 && k.panic_vk == 0x2e, "key values");
        CHECK(fc_config_parse(&k, "[freecam]\ntoggle=bogus\n", warn_collect, &w) == -1,
              "a bad key name is refused");
        CHECK(k.toggle_vk == 0x75, "and leaves the old key alone");
    }

    /* a raw address, for a camera found at run time */
    {
        fc_config a;
        int w = 0;
        fc_config_defaults(&a);
        CHECK(a.address == 0, "no address by default");
        CHECK(fc_config_parse(&a, "[freecam]\naddress=0x7ff6a1b2c3d0\n", warn_collect, &w) == 1,
              "an address parses");
        CHECK(a.address == 0x7ff6a1b2c3d0ull, "address value");
        CHECK(fc_config_parse(&a, "[freecam]\naddress=\n", warn_collect, &w) == 1 && a.address == 0,
              "an empty address clears it");
        CHECK(fc_config_parse(&a, "[freecam]\naddress=12\n", warn_collect, &w) == -1,
              "a nonsense address is refused");
        CHECK(a.address == 0, "and leaves the old one alone");
    }

    /* bad values are reported and change nothing */
    {
        fc_config b;
        int w = 0, rc;
        fc_config_defaults(&b);
        rc = fc_config_parse(&b, "[freecam]\nspeed=0\nmode=flying\nmatrix=sideways\ninvert_y=maybe\n",
                             warn_collect, &w);
        CHECK(rc == -1, "bad values make the parse fail");
        CHECK(w == 4, "one warning per bad value (%d)", w);
        CHECK(NEAR(b.speed, 14.0, 1e-3) && b.mode == FC_MODE_CAMERA && b.view == 0 && b.invert_y == 0,
              "bad values leave the defaults");
    }

    /* unknown keys and sections are ignored with a warning, not an error */
    {
        fc_config u;
        int w = 0, rc;
        fc_config_defaults(&u);
        rc = fc_config_parse(&u, "[freecam]\nturbo=yes\n[nonsense]\nx=1\n", warn_collect, &w);
        CHECK(rc == 0 && w == 2, "unknown settings warn but do not fail (%d warnings)", w);
    }

    /* a lone \r (an ini saved on Windows) must not break values */
    {
        fc_config c;
        int w = 0;
        fc_config_defaults(&c);
        CHECK(fc_config_parse(&c, "[freecam]\r\nspeed=30\r\n", warn_collect, &w) == 1,
              "CRLF line endings");
        CHECK(NEAR(c.speed, 30.0, 1e-3), "value after a CR is right");
    }
}

static void test_matrices(void)
{
    fc_mat m;
    float pos[3] = { 10.0f, 20.0f, -30.0f };
    float back[3], yaw = 0.0f, pitch = 0.0f;

    fc_mat_identity(&m);
    CHECK(fc_mat_valid(&m), "identity is a valid transform");

    /* world matrix, looking along -Z */
    fc_mat_from_pose(&m, pos, 0.0f, 0.0f, 0);
    CHECK(NEAR(m.m[12], 10.0f, 1e-5) && NEAR(m.m[13], 20.0f, 1e-5) && NEAR(m.m[14], -30.0f, 1e-5),
          "position lands in row 3");
    CHECK(NEAR(m.m[8], 0.0f, 1e-5) && NEAR(m.m[9], 0.0f, 1e-5) && NEAR(m.m[10], -1.0f, 1e-5),
          "forward row is -Z at yaw 0 (got %f %f %f)", m.m[8], m.m[9], m.m[10]);
    CHECK(NEAR(m.m[0], 1.0f, 1e-5) && NEAR(m.m[5], 1.0f, 1e-5), "right is +X, up is +Y");
    CHECK(fc_mat_valid(&m), "a built world matrix is valid");

    /* and back out again */
    CHECK(fc_mat_to_pose(&m, back, &yaw, &pitch, 0) == 1, "decompose");
    CHECK(NEAR(back[0], 10.0f, 1e-4) && NEAR(back[2], -30.0f, 1e-4), "position round trips");
    CHECK(NEAR(yaw, 0.0f, 1e-4) && NEAR(pitch, 0.0f, 1e-4), "angles round trip");

    /* quarter turn to the left: yaw = +90 degrees looks along -X */
    fc_mat_from_pose(&m, pos, (float)(TEST_PI / 2.0), 0.0f, 0);
    CHECK(NEAR(m.m[8], -1.0f, 1e-4) && NEAR(m.m[10], 0.0f, 1e-4), "yaw +90 looks along -X");
    CHECK(fc_mat_to_pose(&m, back, &yaw, &pitch, 0) == 1 && NEAR(yaw, (float)(TEST_PI / 2.0), 1e-4),
          "yaw round trips");

    /* pitch up */
    fc_mat_from_pose(&m, pos, 0.0f, 0.5f, 0);
    CHECK(NEAR(m.m[9], sinf(0.5f), 1e-4), "pitch tilts the forward row up");
    CHECK(fc_mat_to_pose(&m, back, &yaw, &pitch, 0) == 1 && NEAR(pitch, 0.5f, 1e-4),
          "pitch round trips");

    /* view matrix: a camera at (0,0,10) looking down -Z sees the origin 10 ahead */
    {
        float eye[3] = { 0.0f, 0.0f, 10.0f };
        fc_mat v;
        fc_mat_from_pose(&v, eye, 0.0f, 0.0f, 1);
        CHECK(fc_mat_valid(&v), "a view matrix is valid");
        CHECK(NEAR(v.m[14], -10.0f, 1e-4), "view translation is -dot(basis, eye)");
        CHECK(fc_mat_to_pose(&v, back, &yaw, &pitch, 1) == 1, "decompose a view matrix");
        CHECK(NEAR(back[2], 10.0f, 1e-4), "view position round trips");
    }

    /* nonsense is rejected before it can be written into the game */
    {
        fc_mat bad;
        fc_mat_identity(&bad);
        bad.m[1] = 5.0f;
        CHECK(!fc_mat_valid(&bad), "a skewed basis is rejected");
        fc_mat_identity(&bad);
        bad.m[12] = 0.0f / 0.0f;
        CHECK(!fc_mat_valid(&bad), "NaN is rejected");
        fc_mat_identity(&bad);
        bad.m[15] = 0.0f;
        CHECK(!fc_mat_valid(&bad), "a projective row is rejected");
        fc_mat_identity(&bad);
        bad.m[14] = 9.0f;
        CHECK(fc_mat_valid(&bad), "a translated identity is still valid");
    }

    /* a matrix that is not finite cannot become a pose */
    {
        fc_mat bad;
        float p[3];
        fc_mat_identity(&bad);
        bad.m[0] = 1.0f / 0.0f;
        CHECK(fc_mat_to_pose(&bad, p, NULL, NULL, 0) == 0, "decomposing garbage fails");
    }
}

static void test_toggle(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam;
    float pos[3] = { 100.0f, 5.0f, 200.0f };

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.7f, 0.1f, 0);

    CHECK(st.active == 0, "off to start with");
    CHECK(fc_toggle(&st, &cam) == 1, "Caps Lock switches it on");
    CHECK(st.active == 1 && st.have_pose == 1, "on, with the camera captured");
    CHECK(NEAR(st.pos[0], 100.0f, 1e-3) && NEAR(st.pos[2], 200.0f, 1e-3), "starts where the camera was");
    CHECK(NEAR(st.yaw, 0.7f, 1e-3) && NEAR(st.pitch, 0.1f, 1e-3), "and looking the same way");
    CHECK(NEAR(st.saved.m[12], 100.0f, 1e-3), "the original matrix is kept");

    CHECK(fc_toggle(&st, &cam) == 1, "and off again");
    CHECK(st.active == 0, "off");

    /* a garbage camera is never taken over */
    {
        fc_mat bad;
        fc_state st2;
        fc_mat_identity(&bad);
        bad.m[4] = 0.5f;                       /* up no longer unit length */
        fc_init(&st2, &cfg);
        CHECK(fc_toggle(&st2, &bad) == 0, "an invalid camera is refused");
        CHECK(st2.active == 0, "and free cam stays off");
    }

    /* no camera at all: nothing happens, nothing crashes */
    {
        fc_state st3;
        fc_init(&st3, &cfg);
        CHECK(fc_toggle(&st3, NULL) == 0, "no target, no free cam");
        CHECK(st3.active == 0, "still off");
    }
}

static void test_movement(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 0.0f, 0.0f, 0.0f };

    fc_config_defaults(&cfg);
    cfg.speed = 10.0f;
    cfg.sprint = 4.0f;
    cfg.max_distance = 100.0f;
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.0f, 0.0f, 0);
    fc_toggle(&st, &cam);

    /* W for a tenth of a second at 10 m/s: 1 m along -Z */
    st.keys[FC_FORWARD] = 1;
    CHECK(fc_step(&st, 0.1f, 0.0f, 0.0f, &out) == 1, "a step is produced");
    CHECK(NEAR(st.pos[2], -1.0f, 1e-3), "forward is -Z at yaw 0 (got %f)", st.pos[2]);
    CHECK(NEAR(st.pos[0], 0.0f, 1e-3) && NEAR(st.pos[1], 0.0f, 1e-3), "no sideways drift");
    CHECK(NEAR(out.m[14], -1.0f, 1e-3), "the written matrix carries the new position");

    /* D strafes along +X */
    st.keys[FC_FORWARD] = 0;
    st.keys[FC_RIGHT] = 1;
    fc_step(&st, 0.1f, 0.0f, 0.0f, &out);
    CHECK(NEAR(st.pos[0], 1.0f, 1e-3), "strafe right moves +X (got %f)", st.pos[0]);
    st.keys[FC_RIGHT] = 0;

    /* Space climbs */
    st.keys[FC_UP] = 1;
    fc_step(&st, 0.1f, 0.0f, 0.0f, &out);
    CHECK(NEAR(st.pos[1], 1.0f, 1e-3), "up climbs (got %f)", st.pos[1]);
    st.keys[FC_UP] = 0;

    /* shift multiplies the step */
    {
        float before = st.pos[2];
        st.keys[FC_FORWARD] = 1;
        st.keys[FC_SPRINT] = 1;
        fc_step(&st, 0.1f, 0.0f, 0.0f, &out);
        CHECK(NEAR(st.pos[2] - before, -4.0f, 1e-3), "sprint is 4x (moved %f)", st.pos[2] - before);
        st.keys[FC_SPRINT] = 0;
        st.keys[FC_FORWARD] = 0;
    }

    /* a long stall must not teleport the camera */
    {
        float before = st.pos[2];
        st.keys[FC_FORWARD] = 1;
        fc_step(&st, 5.0f, 0.0f, 0.0f, &out);   /* five seconds of hitch */
        CHECK(NEAR(st.pos[2] - before, -1.0f, 1e-3), "dt is clamped to 0.1 s (moved %f)", st.pos[2] - before);
        st.keys[FC_FORWARD] = 0;
    }

    /* the safety radius stops a runaway camera */
    {
        st.pos[2] = -1000.0f;
        st.keys[FC_FORWARD] = 0;
        fc_step(&st, 0.016f, 0.0f, 0.0f, &out);
        float d = sqrtf(st.pos[0] * st.pos[0] + st.pos[1] * st.pos[1] + st.pos[2] * st.pos[2]);
        CHECK(d <= cfg.max_distance + 0.01f, "the camera is held inside max_distance (%f)", d);
    }

    /* nothing moves while free cam is off */
    {
        fc_mat before;
        fc_toggle(&st, &out);
        CHECK(st.active == 0, "switched off");
        st.keys[FC_FORWARD] = 1;
        CHECK(fc_step(&st, 0.1f, 0.0f, 0.0f, &before) == 0, "no step while off");
    }
}

static void test_look(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 0.0f, 0.0f, 0.0f };

    fc_config_defaults(&cfg);
    cfg.sensitivity = 0.01f;           /* 10 px per tenth of a radian */
    cfg.invert_y = 0;
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.0f, 0.0f, 0);
    fc_toggle(&st, &cam);

    /* mouse right turns the view to the right: yaw goes negative */
    fc_step(&st, 0.016f, 10.0f, 0.0f, &out);
    CHECK(NEAR(st.yaw, -0.1f, 1e-4), "mouse right turns right (yaw %f)", st.yaw);

    /* mouse up looks up: pitch goes positive */
    fc_step(&st, 0.016f, 0.0f, 10.0f, &out);
    CHECK(NEAR(st.pitch, 0.1f, 1e-4), "mouse up looks up (pitch %f)", st.pitch);

    /* inverted Y flips the vertical only */
    cfg.invert_y = 1;
    fc_init(&st, &cfg);
    fc_toggle(&st, &cam);
    fc_step(&st, 0.016f, 0.0f, 10.0f, &out);
    CHECK(NEAR(st.pitch, -0.1f, 1e-4), "invert_y flips the vertical");
    fc_step(&st, 0.016f, 10.0f, 0.0f, &out);
    CHECK(NEAR(st.yaw, -0.1f, 1e-4), "invert_y leaves the horizontal alone");

    /* looking straight up or down is clamped, not allowed to go over the pole */
    cfg.invert_y = 0;
    fc_init(&st, &cfg);
    fc_toggle(&st, &cam);
    fc_step(&st, 0.016f, 0.0f, 1000.0f, &out);
    CHECK(st.pitch <= 1.5534f && st.pitch > 1.5f, "pitch is clamped near the pole (%f)", st.pitch);
    CHECK(fc_mat_valid(&out), "the clamped matrix is still valid");
}

static void test_restore(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 7.0f, 8.0f, 9.0f };

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.3f, -0.2f, 0);
    fc_toggle(&st, &cam);
    st.keys[FC_FORWARD] = 1;
    fc_step(&st, 0.2f, 0.0f, 0.0f, &out);
    st.keys[FC_FORWARD] = 0;
    fc_toggle(&st, &out);                       /* off */

    CHECK(fc_restore(&st, &out) == 1, "the saved camera comes back");
    CHECK(NEAR(out.m[12], 7.0f, 1e-4) && NEAR(out.m[13], 8.0f, 1e-4) && NEAR(out.m[14], 9.0f, 1e-4),
          "at the position it was switched on at");
    CHECK(NEAR(out.m[8], cam.m[8], 1e-5), "and the same angle");

    /* with restore turned off nothing is written back */
    cfg.restore = 0;
    fc_init(&st, &cfg);
    fc_toggle(&st, &cam);
    fc_toggle(&st, &out);
    CHECK(fc_restore(&st, &out) == 0, "restore=false writes nothing");
}

static void test_safety(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 0.0f, 0.0f, 0.0f };

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.0f, 0.0f, 0);
    fc_toggle(&st, &cam);

    CHECK(fc_step(&st, 0.0f, 0.0f, 0.0f, &out) == 0, "a zero-length tick writes nothing");
    CHECK(fc_step(&st, -1.0f, 0.0f, 0.0f, &out) == 0, "a negative tick writes nothing");
    CHECK(fc_step(&st, 0.016f, 0.0f, 0.0f, &out) == 1, "a normal tick writes");

    /* ten thousand random ticks: the matrix is always something the game can use */
    {
        unsigned seed = 12345u;
        int i;
        for (i = 0; i < 10000; i++) {
            seed = seed * 1103515245u + 12345u;
            st.keys[FC_FORWARD] = (seed >> 3) & 1;
            st.keys[FC_BACK] = (seed >> 4) & 1;
            st.keys[FC_LEFT] = (seed >> 5) & 1;
            st.keys[FC_RIGHT] = (seed >> 6) & 1;
            st.keys[FC_UP] = (seed >> 7) & 1;
            st.keys[FC_SPRINT] = (seed >> 8) & 1;
            if (fc_step(&st, 0.016f, (float)((int)(seed >> 9) % 40) - 20.0f,
                        (float)((int)(seed >> 17) % 40) - 20.0f, &out)) {
                if (!fc_mat_valid(&out)) {
                    CHECK(0, "a random tick produced an invalid matrix at %d", i);
                    break;
                }
            }
        }
        CHECK(1, "random ticks stay valid");
    }
}

/* ---- chain walking --------------------------------------------------------- */

/* A pretend process: one mock region, so the chain rules can be checked off Windows. */
typedef struct {
    unsigned char *base;
    unsigned size;
} fake_mem;

static int fake_read(void *ctx, const void *addr, void *out, unsigned size)
{
    fake_mem *m = (fake_mem *)ctx;
    size_t off;
    if ((const unsigned char *)addr < m->base) return 0;
    off = (size_t)((const unsigned char *)addr - m->base);
    if (off + size > m->size) return 0;
    memcpy(out, addr, size);
    return 1;
}

static void test_chain_walk(void)
{
    fc_config cfg;
    fake_mem mem;
    unsigned char *b;
    void *p1, *p2;

    mem.size = 0x400;
    b = (unsigned char *)calloc(1, mem.size);
    CHECK(b != NULL, "mock memory");
    if (!b) return;
    mem.base = b;
    p1 = b + 0x100;
    p2 = b + 0x200;
    *(void **)(b + 0x10) = p1;
    *(void **)(p1 + 0x58) = p2;

    /* exe + 0x10 -> object, object + 0x58 -> body, matrix at body + 0x80.
     * The last hop is an offset, it is never dereferenced. */
    fc_config_defaults(&cfg);
    CHECK(fc_chain_parse(&cfg, "0x10,0x58,0x80") == 1, "a three hop chain");
    CHECK(fc_chain_resolve(b, &cfg, fake_read, &mem) == (void *)((unsigned char *)p2 + 0x80),
          "the walk ends at body + the last offset");

    /* one hop: an address inside the base, no dereference at all */
    fc_config_defaults(&cfg);
    CHECK(fc_chain_parse(&cfg, "0x2f0") == 1, "a one hop chain");
    CHECK(fc_chain_resolve(b, &cfg, fake_read, &mem) == (void *)(b + 0x2f0), "base + offset");

    /* a null pointer stops the walk rather than following it */
    *(void **)(b + 0x20) = NULL;
    fc_config_defaults(&cfg);
    CHECK(fc_chain_parse(&cfg, "0x20,0x58,0x80") == 1, "chain over a null pointer");
    CHECK(fc_chain_resolve(b, &cfg, fake_read, &mem) == NULL, "a null hop ends the walk");

    /* a hop outside the readable region stops it too */
    fc_config_defaults(&cfg);
    CHECK(fc_chain_parse(&cfg, "0x3ff,0x58,0x80") == 1, "chain over the edge");
    CHECK(fc_chain_resolve(b, &cfg, fake_read, &mem) == NULL, "an unreadable hop ends the walk");

    /* nothing configured is not a chain */
    fc_config_defaults(&cfg);
    CHECK(fc_chain_resolve(b, &cfg, fake_read, &mem) == NULL, "no chain, no address");

    free(b);
}

int main(void)
{
    test_defaults();
    test_chain();
    test_chain_walk();
    test_config();
    test_matrices();
    test_toggle();
    test_movement();
    test_look();
    test_restore();
    test_safety();

    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}

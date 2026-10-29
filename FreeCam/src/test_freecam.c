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

    /* world matrix, looking along +Y in a Z-up world */
    fc_mat_from_pose(&m, pos, 0.0f, 0.0f, 0);
    CHECK(NEAR(m.m[12], 10.0f, 1e-5) && NEAR(m.m[13], 20.0f, 1e-5) && NEAR(m.m[14], -30.0f, 1e-5),
          "position lands in row 3");
    CHECK(NEAR(m.m[8], 0.0f, 1e-5) && NEAR(m.m[9], -1.0f, 1e-5) && NEAR(m.m[10], 0.0f, 1e-5),
          "the third row is backwards: -Y at yaw 0 (got %f %f %f)", m.m[8], m.m[9], m.m[10]);
    CHECK(NEAR(m.m[0], 1.0f, 1e-5) && NEAR(m.m[6], 1.0f, 1e-5), "right is +X, up is +Z");
    CHECK(fc_mat_valid(&m), "a built world matrix is valid");

    /* and back out again */
    CHECK(fc_mat_to_pose(&m, back, &yaw, &pitch, 0) == 1, "decompose");
    CHECK(NEAR(back[0], 10.0f, 1e-4) && NEAR(back[2], -30.0f, 1e-4), "position round trips");
    CHECK(NEAR(yaw, 0.0f, 1e-4) && NEAR(pitch, 0.0f, 1e-4), "angles round trip");

    /* quarter turn to the left: yaw = +90 degrees looks along -X */
    fc_mat_from_pose(&m, pos, (float)(TEST_PI / 2.0), 0.0f, 0);
    CHECK(NEAR(m.m[8], 1.0f, 1e-4) && NEAR(m.m[9], 0.0f, 1e-4), "yaw +90 looks along -X");
    CHECK(fc_mat_to_pose(&m, back, &yaw, &pitch, 0) == 1 && NEAR(yaw, (float)(TEST_PI / 2.0), 1e-4),
          "yaw round trips");

    /* pitch up */
    fc_mat_from_pose(&m, pos, 0.0f, 0.5f, 0);
    CHECK(NEAR(m.m[10], -sinf(0.5f), 1e-4), "pitch tilts the backwards row down");
    CHECK(fc_mat_to_pose(&m, back, &yaw, &pitch, 0) == 1 && NEAR(pitch, 0.5f, 1e-4),
          "pitch round trips");

    /* view matrix: a camera at (0,-10,0) looking along +Y sees the origin 10 ahead */
    {
        float eye[3] = { 0.0f, -10.0f, 0.0f };
        fc_mat v;
        fc_mat_from_pose(&v, eye, 0.0f, 0.0f, 1);
        CHECK(fc_mat_valid(&v), "a view matrix is valid");
        CHECK(NEAR(v.m[13], 0.0f, 1e-4) && NEAR(v.m[14], -10.0f, 1e-4),
              "view translation is -dot(row, eye) (got %f %f %f)", v.m[12], v.m[13], v.m[14]);
        CHECK(fc_mat_to_pose(&v, back, &yaw, &pitch, 1) == 1, "decompose a view matrix");
        CHECK(NEAR(back[1], -10.0f, 1e-4), "view position round trips");
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

static void test_spawn_matrix(void)
{
    fc_mat player, spot;
    float pos[3] = { 100.0f, 200.0f, 5.0f };
    float spos[3], yaw = 0.0f, pitch = 0.0f;

    /* Facing +Y at yaw 0: three metres in front is three metres up the Y axis, at the
     * same height. This is the whole point of the feature, so it is checked exactly. */
    fc_mat_from_pose(&player, pos, 0.0f, 0.0f, 0);
    CHECK(fc_spawn_matrix(&spot, &player, 3.0f) == 1, "a good transform makes a spot");
    CHECK(NEAR(spot.m[12], 100.0f, 1e-4) && NEAR(spot.m[13], 203.0f, 1e-4) &&
          NEAR(spot.m[14], 5.0f, 1e-4),
          "3 m in front is 3 m along +Y (got %f %f %f)", spot.m[12], spot.m[13], spot.m[14]);

    /* The new transform faces the way the player faces, and is a world transform. */
    CHECK(fc_mat_to_pose(&spot, spos, &yaw, &pitch, 0) == 1, "the spot decomposes");
    CHECK(NEAR(yaw, 0.0f, 1e-4) && NEAR(pitch, 0.0f, 1e-4), "the spot faces where the player does");
    CHECK(NEAR(spot.m[15], 1.0f, 1e-4), "the last row is completed");

    /* Turned left: yaw +90 degrees looks along -X, so the spot moves to -X. */
    fc_mat_from_pose(&player, pos, (float)(TEST_PI / 2.0), 0.0f, 0);
    CHECK(fc_spawn_matrix(&spot, &player, 3.0f) == 1, "a turned transform makes a spot");
    CHECK(NEAR(spot.m[12], 97.0f, 1e-3) && NEAR(spot.m[13], 200.0f, 1e-3),
          "yaw +90 puts the spot along -X (got %f %f)", spot.m[12], spot.m[13]);

    /* Looking at the sky must not lift the spot off the ground: pitch is dropped. */
    fc_mat_from_pose(&player, pos, 0.0f, 1.2f, 0);
    CHECK(fc_spawn_matrix(&spot, &player, 3.0f) == 1, "a pitched transform makes a spot");
    CHECK(NEAR(spot.m[14], 5.0f, 1e-4), "pitch does not lift the spot (got %f)", spot.m[14]);
    CHECK(NEAR(spot.m[13], 203.0f, 1e-3), "pitch does not shorten the distance");

    /* Straight down is a special case worth pinning down: the yaw is undefined, but the
     * spot must still be horizontal from the player rather than at their feet. */
    fc_mat_from_pose(&player, pos, 0.0f, (float)(TEST_PI / 2.0), 0);
    CHECK(fc_spawn_matrix(&spot, &player, 3.0f) == 1, "looking straight up still works");
    CHECK(NEAR(spot.m[14], 5.0f, 1e-4), "looking straight up does not stack the spot overhead");

    /* Garbage in, nothing out: the caller's matrix is left alone. */
    {
        fc_mat bad, untouched;
        fc_mat_identity(&bad);
        bad.m[0] = 1.0f / 0.0f;
        untouched = bad;
        CHECK(fc_spawn_matrix(&spot, &bad, 3.0f) == 0, "a broken transform is refused");
        CHECK(fc_spawn_matrix(NULL, &player, 3.0f) == 0, "no output means no work");
        CHECK(fc_spawn_matrix(&spot, NULL, 3.0f) == 0, "no input means no work");
        CHECK(memcmp(&bad, &untouched, sizeof bad) == 0, "the input is not touched");
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

    /* W for a tenth of a second at 10 m/s: 1 m along +Y */
    st.keys[FC_FORWARD] = 1;
    CHECK(fc_step(&st, 0.1f, 0.0f, 0.0f, &out) == 1, "a step is produced");
    CHECK(NEAR(st.pos[1], 1.0f, 1e-3), "forward is +Y at yaw 0 (got %f)", st.pos[1]);
    CHECK(NEAR(st.pos[0], 0.0f, 1e-3) && NEAR(st.pos[2], 0.0f, 1e-3), "no sideways drift");
    CHECK(NEAR(out.m[13], 1.0f, 1e-3), "the written matrix carries the new position");

    /* D strafes along +X */
    st.keys[FC_FORWARD] = 0;
    st.keys[FC_RIGHT] = 1;
    fc_step(&st, 0.1f, 0.0f, 0.0f, &out);
    CHECK(NEAR(st.pos[0], 1.0f, 1e-3), "strafe right moves +X (got %f)", st.pos[0]);
    st.keys[FC_RIGHT] = 0;

    /* Space climbs */
    st.keys[FC_UP] = 1;
    fc_step(&st, 0.1f, 0.0f, 0.0f, &out);
    CHECK(NEAR(st.pos[2], 1.0f, 1e-3), "up climbs (got %f)", st.pos[2]);
    st.keys[FC_UP] = 0;

    /* shift multiplies the step */
    {
        float before = st.pos[1];
        st.keys[FC_FORWARD] = 1;
        st.keys[FC_SPRINT] = 1;
        fc_step(&st, 0.1f, 0.0f, 0.0f, &out);
        CHECK(NEAR(st.pos[1] - before, 4.0f, 1e-3), "sprint is 4x (moved %f)", st.pos[1] - before);
        st.keys[FC_SPRINT] = 0;
        st.keys[FC_FORWARD] = 0;
    }

    /* a long stall must not teleport the camera */
    {
        float before = st.pos[1];
        st.keys[FC_FORWARD] = 1;
        fc_step(&st, 5.0f, 0.0f, 0.0f, &out);   /* five seconds of hitch */
        CHECK(NEAR(st.pos[1] - before, 1.0f, 1e-3), "dt is clamped to 0.1 s (moved %f)", st.pos[1] - before);
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

    /* Windows reports screen y growing downwards, so a positive dy is the mouse moving
     * down - and moving the mouse down has to look down (natural, not inverted). */
    fc_step(&st, 0.016f, 0.0f, 10.0f, &out);
    CHECK(NEAR(st.pitch, -0.1f, 1e-4), "mouse down looks down (pitch %f)", st.pitch);
    fc_step(&st, 0.016f, 0.0f, -10.0f, &out);
    CHECK(NEAR(st.pitch, 0.0f, 1e-4), "and mouse up looks up again");

    /* invert_y flips the vertical only, for anyone who wants the old behaviour */
    cfg.invert_y = 1;
    fc_init(&st, &cfg);
    fc_toggle(&st, &cam);
    fc_step(&st, 0.016f, 0.0f, 10.0f, &out);
    CHECK(NEAR(st.pitch, 0.1f, 1e-4), "invert_y flips the vertical");
    fc_step(&st, 0.016f, 10.0f, 0.0f, &out);
    CHECK(NEAR(st.yaw, -0.1f, 1e-4), "invert_y leaves the horizontal alone");

    /* looking straight up or down is clamped, not allowed to go over the pole */
    cfg.invert_y = 0;
    fc_init(&st, &cfg);
    fc_toggle(&st, &cam);
    fc_step(&st, 0.016f, 0.0f, 1000.0f, &out);
    CHECK(st.pitch >= -1.5534f && st.pitch < -1.5f, "pitch is clamped near the pole (%f)", st.pitch);
    CHECK(fc_mat_valid(&out), "the clamped matrix is still valid");
}

/* The pose of the game's own camera, straight out of TW_MenuLog.txt. This is the test
 * that would have caught the Y-up/Z-up mistake: with the wrong up axis the extracted
 * pose reproduced a camera rolled 180 degrees. */
static void test_game_camera_matrix(void)
{
    fc_mat v = { { -0.996f, -0.014f, 0.086f, 0.000f,
                    0.087f, -0.155f, 0.984f, 0.000f,
                   -0.000f,  0.988f, 0.156f, 0.000f,
                 1477.117f, -111.211f, 684.829f, 1.000f } };
    fc_mat rebuilt;
    float pos[3] = { 0, 0, 0 }, yaw = 0.0f, pitch = 0.0f;
    int sign = 0, k;

    CHECK(fc_mat_valid(&v), "the game's camera matrix is a view matrix");
    CHECK(fc_pose_from_view(&v, pos, &yaw, &pitch, &sign) == 1, "its pose comes out");
    CHECK(sign == 1, "with row 2 holding -forward");

    CHECK(NEAR(pos[0], 1410.8f, 0.1f) && NEAR(pos[1], -819.6f, 0.1f) && NEAR(pos[2], 3.0f, 0.1f),
          "eye at %.1f %.1f %.1f", pos[0], pos[1], pos[2]);
    CHECK(NEAR(pitch, -0.157f, 0.01f), "looking 9 degrees down (pitch %.1f deg)",
          pitch * 57.29578f);
    CHECK(fabsf(yaw - 3.0543f) < 0.02f, "yaw 175 degrees (%.1f deg)", yaw * 57.29578f);

    /* and the rebuild reproduces it, which is what says the basis is the right one.
     * The log prints three decimals, and a translation component is the dot of a
     * basis row with an eye 1400 units out, so those can be half a unit off. */
    fc_mat_from_pose(&rebuilt, pos, yaw, pitch, sign);
    {
        float worst = 0.0f, worstT = 0.0f;
        for (k = 0; k < 12; k++) {
            float d = rebuilt.m[k] - v.m[k];
            if (d < 0.0f) d = -d;
            if (d > worst) worst = d;
        }
        for (k = 12; k < 15; k++) {
            float d = rebuilt.m[k] - v.m[k];
            if (d < 0.0f) d = -d;
            if (d > worstT) worstT = d;
        }
        CHECK(worst < 0.01f, "the rebuilt rotation matches the game's (worst %f)", worst);
        CHECK(worstT < 1.0f, "and so does its translation (worst %f)", worstT);
    }

    /* the camera's world matrix, logged at the same moment from +0x80 */
    {
        fc_mat world = { { -0.996f, 0.087f, -0.000f, 0.000f,
                           -0.014f, -0.155f, 0.988f, 0.000f,
                            0.086f, 0.984f, 0.156f, 0.000f,
                         1411.382f, -819.282f, 3.070f, 1.000f } };
        float wp[3] = { 0, 0, 0 }, wyaw = 0.0f, wpitch = 0.0f;

        CHECK(fc_mat_valid(&world), "the game's world matrix is a rigid transform");
        CHECK(fc_mat_to_pose(&world, wp, &wyaw, &wpitch, 0) == 1, "and gives the same pose");
        CHECK(NEAR(wp[0], 1411.4f, 0.1f) && NEAR(wp[1], -819.3f, 0.1f) && NEAR(wp[2], 3.1f, 0.1f),
              "camera at %.1f %.1f %.1f", wp[0], wp[1], wp[2]);
        CHECK(NEAR(wyaw, yaw, 0.02f) && NEAR(wpitch, pitch, 0.02f),
              "looking the same way as the view says (%.1f %.1f deg)",
              wyaw * 57.29578f, wpitch * 57.29578f);
        CHECK(fc_view_sign(&world, &v) == 1, "and the two are a world/view pair");

        /* the world matrix has no such rounding problem: it rebuilds to the digit */
        {
            fc_mat wreb;
            float worstW = 0.0f;
            fc_mat_from_pose(&wreb, wp, wyaw, wpitch, 0);
            for (k = 0; k < 16; k++) {
                float d = wreb.m[k] - world.m[k];
                if (d < 0.0f) d = -d;
                if (d > worstW) worstW = d;
            }
            CHECK(worstW < 0.01f, "the world matrix rebuilds exactly (worst %f)", worstW);
        }
    }
}

/* Freezing holds the pose while the player gets on with it. */
static void test_freeze(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 1.0f, 2.0f, 3.0f };

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.2f, -0.1f, 0);
    fc_toggle(&st, &cam);

    CHECK(!fc_frozen(&st), "not frozen to begin with");
    fc_freeze(&st, 1);
    CHECK(fc_frozen(&st), "frozen after O");

    /* every key pressed, mouse moving: the pose must not change */
    st.keys[FC_FORWARD] = 1;
    st.keys[FC_RIGHT] = 1;
    fc_step(&st, 0.5f, 40.0f, -25.0f, &out);
    CHECK(NEAR(st.pos[0], 1.0f, 1e-5) && NEAR(st.pos[1], 2.0f, 1e-5),
          "a frozen camera ignores the keys (%.3f %.3f)", st.pos[0], st.pos[1]);
    CHECK(NEAR(st.yaw, 0.2f, 1e-5) && NEAR(st.pitch, -0.1f, 1e-5), "and the mouse");
    CHECK(fc_mat_valid(&out), "but it still hands out a pose to draw");

    /* switching free cam off and on again must never come back frozen */
    fc_freeze(&st, 1);
    fc_set_active(&st, 0, NULL);
    CHECK(!fc_frozen(&st), "switching free cam off thaws it");
    fc_mat_from_pose(&cam, pos, 0.2f, -0.1f, 0);
    fc_set_active(&st, 1, &cam);
    CHECK(!fc_frozen(&st), "and switching it on starts unfrozen");

    fc_freeze(&st, 0);
    st.keys[FC_FORWARD] = 0;
    st.keys[FC_RIGHT] = 0;
    {
        float before[3], f[3], r[3], u[3];
        before[0] = st.pos[0]; before[1] = st.pos[1]; before[2] = st.pos[2];
        fc_basis(0.2f, -0.1f, f, r, u);
        st.keys[FC_FORWARD] = 1;
        fc_step(&st, 0.1f, 0.0f, 0.0f, &out);
        st.keys[FC_FORWARD] = 0;
        CHECK(NEAR(st.pos[0] - before[0], f[0] * cfg.speed * 0.1f, 1e-3) &&
              NEAR(st.pos[1] - before[1], f[1] * cfg.speed * 0.1f, 1e-3) &&
              NEAR(st.pos[2] - before[2], f[2] * cfg.speed * 0.1f, 1e-3),
              "flying again after unfreezing");
    }
}

/* Following: the camera goes where the player goes, keeping whatever offset it had, and
 * manual movement still works on top of it. */
static void test_follow(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 10.0f, 20.0f, 30.0f };
    float player[3] = { 8.0f, 20.0f, 28.0f };

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.3f, 0.1f, 0);
    fc_toggle(&st, &cam);

    CHECK(!fc_following(&st), "not following to begin with");
    fc_follow(&st, player, 1);
    CHECK(fc_following(&st), "following after switching it on");

    /* the player walks two metres; the camera has to cover the same two metres */
    player[0] += 2.0f;
    fc_follow_update(&st, player);
    CHECK(NEAR(st.pos[0], 12.0f, 1e-4), "the camera keeps its offset and moves with the player");
    CHECK(NEAR(st.pos[2], 30.0f, 1e-4), "and does not move on the other axes");

    /* flying by hand while following adds to the follow, it does not fight it */
    st.cfg.speed = 10.0f;                 /* 0.1 s of flight is one unit */
    st.keys[FC_UP] = 1;
    fc_step(&st, 0.1f, 0.0f, 0.0f, &out);      /* one unit up (+Z) */
    st.keys[FC_UP] = 0;
    player[1] += 1.0f;                          /* and the player walks one unit (+Y) */
    fc_follow_update(&st, player);
    CHECK(NEAR(st.pos[1], 21.0f, 1e-3) && NEAR(st.pos[2], 31.0f, 1e-3),
          "manual movement and following add up (%f %f)", st.pos[1], st.pos[2]);

    fc_follow(&st, player, 0);
    player[0] += 100.0f;
    fc_follow_update(&st, player);
    CHECK(NEAR(st.pos[0], 12.0f, 1e-4), "switching it off leaves the camera where it is");
}

/* Keyframes: record, play back at the speed it was recorded, land on the last one. */
/* The camera turns with the player's own camera, so the framing survives the player
 * looking around. */
static void test_follow_pan(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam;
    float pos[3] = { 10.0f, 20.0f, 30.0f };
    float player[3] = { 8.0f, 20.0f, 28.0f };

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.5f, 0.0f, 0);
    fc_toggle(&st, &cam);
    fc_follow(&st, player, 1);

    /* the first angles seen are the baseline, not a turn */
    fc_follow_update_angles(&st, 1.0f, 0.0f);
    CHECK(NEAR(st.yaw, 0.5f, 1e-5), "the first angle only sets the baseline (%f)", st.yaw);

    /* the player's camera turns 0.2 rad to the left: so does the free camera */
    fc_follow_update_angles(&st, 1.2f, 0.0f);
    CHECK(NEAR(st.yaw, 0.7f, 1e-5), "the camera pans with the player (%f)", st.yaw);

    /* and looking down tilts it down by the same amount */
    fc_follow_update_angles(&st, 1.2f, -0.3f);
    CHECK(NEAR(st.pitch, -0.3f, 1e-5), "and tilts with it (%f)", st.pitch);

    /* the short way round: 3.10 to -3.10 is a 0.083 rad turn, not a full circle */
    st.yaw = 0.0f;
    st.last_player_yaw = 3.10f;
    st.have_last_angles = 1;
    fc_follow_update_angles(&st, -3.10f, 0.0f);
    CHECK(NEAR(st.yaw, 0.083f, 1e-3), "a wrap past 180 degrees takes the short way (%f)", st.yaw);

    /* no follow, no pan */
    fc_follow(&st, player, 0);
    st.yaw = 0.25f;
    fc_follow_update_angles(&st, 2.0f, 1.0f);
    CHECK(NEAR(st.yaw, 0.25f, 1e-6), "with following off the camera does not turn");

    /* switching following back on starts a fresh baseline */
    fc_follow(&st, player, 1);
    fc_follow_update_angles(&st, 2.0f, 1.0f);
    CHECK(NEAR(st.yaw, 0.25f, 1e-6), "and the first angle after switching on is a baseline again");
}

/* The playback travels from keyframe to keyframe exactly as fast as the camera flew, is
 * moving on the first frame, lands on the last key, and plays at all even when the
 * recorded times are unusable. */
static void test_playback_path(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float a[3], yaw = 0.0f, pitch = 0.0f;

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.0f, 0.0f, 0);
    fc_toggle(&st, &cam);

    /* a right-angle corner: along +X for a second, then along +Y for a second */
    st.pos[0] = 0.0f;  st.pos[1] = 0.0f;  st.pos[2] = 0.0f;
    fc_key_add(&st, 0.0f);
    st.pos[0] = 10.0f; st.pos[1] = 0.0f;
    fc_key_add(&st, 1.0f);
    st.pos[0] = 10.0f; st.pos[1] = 10.0f;
    fc_key_add(&st, 2.0f);
    CHECK(st.path_count == 3, "three keys recorded");

    /* every key is hit exactly, at the moment it was recorded */
    CHECK(fc_play_pose(&st, 0.0f, a, &yaw, &pitch) == 1, "the path can be sampled");
    CHECK(NEAR(a[0], 0.0f, 1e-4) && NEAR(a[1], 0.0f, 1e-4), "it starts on the first key");
    fc_play_pose(&st, 1.0f, a, &yaw, &pitch);
    CHECK(NEAR(a[0], 10.0f, 1e-3) && NEAR(a[1], 0.0f, 1e-3), "it reaches the middle key (%f %f)",
          a[0], a[1]);
    fc_play_pose(&st, 2.0f, a, NULL, NULL);
    CHECK(NEAR(a[0], 10.0f, 1e-3) && NEAR(a[1], 10.0f, 1e-3), "and finishes on the last one");

    /* halfway through the first leg is halfway along it, and already moving */
    fc_play_pose(&st, 0.01f, a, NULL, NULL);
    CHECK(a[0] > 0.05f, "the camera is moving on the first frame (%f)", a[0]);
    fc_play_pose(&st, 0.5f, a, NULL, NULL);
    CHECK(NEAR(a[0], 5.0f, 1e-3) && NEAR(a[1], 0.0f, 1e-3), "half a second in is halfway (%f)", a[0]);

    /* a pair that crosses 180 degrees turns the short way, not all the way round */
    {
        fc_state w;
        fc_init(&w, &cfg);
        fc_mat_from_pose(&cam, pos, 0.0f, 0.0f, 0);
        fc_toggle(&w, &cam);
        w.pos[0] = 0.0f;  w.yaw = 3.10f;  fc_key_add(&w, 0.0f);
        w.pos[0] = 10.0f; w.yaw = -3.10f; fc_key_add(&w, 1.0f);
        fc_play_pose(&w, 0.5f, a, &yaw, NULL);
        CHECK(fabsf(fabsf(yaw) - 3.14159f) < 0.06f,
              "the turn through 180 degrees is short, not a full spin (%f)", yaw);
    }

    /* And the case that made the playback look broken: a path whose keys share one
     * moment. It used to play in a millisecond - the camera simply jumped to the last
     * key. It now spaces the keys out and plays properly. */
    {
        fc_state d;
        fc_init(&d, &cfg);
        fc_mat_from_pose(&cam, pos, 0.0f, 0.0f, 0);
        fc_toggle(&d, &cam);
        d.pos[0] = 0.0f;
        CHECK(fc_key_add(&d, 5.0f) == 1, "first key");
        d.pos[0] = 10.0f;
        CHECK(fc_key_add(&d, 5.0f) == 2, "second key at the same moment");
        d.pos[0] = 20.0f;
        CHECK(fc_key_add(&d, 5.0f) == 3, "third key too");
        CHECK(NEAR(d.path[2].time, 0.04f, 1e-4), "the recorded times are 0.02 s apart");

        CHECK(fc_play_start(&d) == 1, "the playback starts");
        fc_play_step(&d, 0.1f, &out);        /* well inside the first of three 0.6 s legs */
        CHECK(d.pos[0] > 1.0f && d.pos[0] < 9.0f,
              "and it is travelling rather than jumping to the end (%f)", d.pos[0]);
        CHECK(fc_playing(&d), "still playing");
        for (int i = 0; i < 200 && fc_playing(&d); i++) fc_play_step(&d, 0.05f, &out);
        CHECK(NEAR(d.pos[0], 20.0f, 1e-3), "and it reaches the last key in the end (%f)", d.pos[0]);
    }

    /* the same for a path whose times run backwards (a restored path, then new keys) */
    {
        fc_state r;
        fc_init(&r, &cfg);
        fc_mat_from_pose(&cam, pos, 0.0f, 0.0f, 0);
        fc_toggle(&r, &cam);
        r.pos[0] = 0.0f;  fc_key_add(&r, 0.0f);
        r.pos[0] = 10.0f; fc_key_add(&r, 4.0f);
        fc_path_resume(&r, 0.0f);                    /* restored, flight clock back at 0 */
        r.pos[0] = 20.0f; fc_key_add(&r, 1.0f);      /* a second into the new flight */
        CHECK(NEAR(r.path[2].time, 5.0f, 1e-3),
              "a key added after a restore follows the last one by the real time (%f)",
              r.path[2].time);

        fc_play_start(&r);
        fc_play_step(&r, 1.0f, &out);
        CHECK(r.pos[0] > 2.0f && r.pos[0] < 15.0f, "and the playback is under way (%f)", r.pos[0]);
    }
}

/* A frozen camera keeps its position and only pans: P while the camera is held still
 * aims it at the player and leaves it where it is. */
static void test_frozen_pan(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 10.0f, 0.0f, 5.0f };
    float player[3] = { 10.0f, 30.0f, 1.0f };    /* straight ahead in +Y, below */
    float yaw = 0.0f, pitch = 0.0f;
    int i;

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, (float)TEST_PI, 0.0f, 0);   /* facing away */
    fc_toggle(&st, &cam);
    fc_freeze(&st, 1);

    CHECK(fc_frozen(&st), "frozen after O");
    CHECK(!fc_looking_at(&st), "and not aiming at anything yet");

    /* the angles that point at the player, worked out the long way */
    fc_angles_to(st.pos, player, &yaw, &pitch);
    CHECK(NEAR(yaw, 0.0f, 1e-4), "the player is straight along +Y, so yaw 0 (%f)", yaw);
    CHECK(pitch < -0.1f, "and below the camera, so pitch is down (%f)", pitch);

    fc_look_at(&st, 1);
    CHECK(fc_looking_at(&st), "P turns the aiming on");

    /* a few ticks, and it comes round without moving */
    for (i = 0; i < 200; i++) fc_aim_at(&st, player, 0.005f);
    CHECK(NEAR(st.yaw, yaw, 0.02f), "the camera has panned round to the player (%f)", st.yaw);
    CHECK(NEAR(st.pitch, pitch, 0.02f), "and tilted down to it (%f)", st.pitch);
    CHECK(NEAR(st.pos[0], 10.0f, 1e-5) && NEAR(st.pos[1], 0.0f, 1e-5) && NEAR(st.pos[2], 5.0f, 1e-5),
          "without travelling: it is still frozen at %.1f %.1f %.1f", st.pos[0], st.pos[1], st.pos[2]);

    /* the player walks away to the side: the camera pans, and still does not move */
    player[0] = -20.0f;                 /* 30 to the left, 30 ahead: yaw +pi/4 */
    for (i = 0; i < 200; i++) fc_aim_at(&st, player, 0.005f);
    CHECK(NEAR(st.yaw, (float)(TEST_PI / 4.0), 0.02f), "it pans after the player (%f)", st.yaw);
    CHECK(NEAR(st.pos[0], 10.0f, 1e-5) && NEAR(st.pos[1], 0.0f, 1e-5),
          "and stays exactly where it was frozen");

    /* a frozen camera still ignores the mouse and the keys, so the aiming is not fought */
    st.keys[FC_FORWARD] = 1;
    fc_step(&st, 0.5f, 60.0f, 40.0f, &out);
    CHECK(NEAR(st.pos[1], 0.0f, 1e-5) && NEAR(st.pos[0], 10.0f, 1e-5),
          "the keys and the mouse do nothing while frozen");

    /* unfreezing ends the aiming: the camera is free again */
    fc_freeze(&st, 0);
    CHECK(!fc_looking_at(&st), "thawing stops the aiming");
    CHECK(fc_aim_at(&st, player, 0.01f) == 0, "and it does not pull the camera round any more");
}

/* The arrow keys walk the playback speed up and down a ladder. */
static void test_play_speed(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 1.0f, 2.0f, 3.0f };

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.0f, 0.0f, 0);
    fc_toggle(&st, &cam);

    CHECK(NEAR(fc_play_speed(&st), 1.0f, 1e-6), "the path plays at the speed it was flown");

    CHECK(NEAR(fc_play_speed_shift(&st, 1), 1.25f, 1e-6), "one step faster is 1.25x");
    CHECK(NEAR(fc_play_speed_shift(&st, 1), 1.5f, 1e-6), "then 1.5x");
    fc_play_speed_shift(&st, 0);
    fc_play_speed_shift(&st, 0);
    CHECK(NEAR(fc_play_speed(&st), 1.0f, 1e-6), "and back down to 1x");

    /* the ends are the ends */
    for (int i = 0; i < 20; i++) fc_play_speed_shift(&st, 0);
    CHECK(NEAR(fc_play_speed(&st), FC_PLAY_SPEED_MIN, 1e-6), "slowest is 0.1x (%f)",
          fc_play_speed(&st));
    for (int i = 0; i < 20; i++) fc_play_speed_shift(&st, 1);
    CHECK(NEAR(fc_play_speed(&st), FC_PLAY_SPEED_MAX, 1e-6), "fastest is 4x (%f)",
          fc_play_speed(&st));

    /* and the speed really changes the playback: 2 s of path at 2x takes 1 s */
    {
        /* a keyframe records the pose the camera is in, so move it between takes */
        st.path_count = 0;
        st.pos[0] = 0.0f;
        fc_key_add(&st, 0.0f);
        st.pos[0] = 10.0f;
        fc_key_add(&st, 2.0f);
        CHECK(st.path_count == 2, "two keys recorded");

        fc_play_set_speed(&st, 2.0f);
        CHECK(fc_play_start(&st) == 1, "playback starts");
        fc_play_step(&st, 0.5f, &out);          /* 1.0 s of path */
        CHECK(NEAR(st.pos[0], 5.0f, 1e-3), "at 2x, half a second is a quarter of the path (%f)",
              st.pos[0]);
        CHECK(fc_playing(&st), "still playing");
        fc_play_step(&st, 0.5f, &out);          /* 2.0 s of path: the end */
        CHECK(NEAR(st.pos[0], 10.0f, 1e-3) && !fc_playing(&st), "and it lands on the last key");
    }

    /* clearing the path with M leaves nothing to play */
    fc_path_clear(&st);
    CHECK(st.path_count == 0 && !fc_playing(&st), "M clears the recorded path");
    CHECK(fc_play_start(&st) == 0, "and there is nothing left to play");
}

static void test_keyframes(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 0.0f, 0.0f, 0.0f };

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.0f, 0.0f, 0);
    CHECK(fc_key_add(&st, 0.0f) == 0, "a keyframe needs free cam to be flying");
    fc_toggle(&st, &cam);
    CHECK(st.active, "free cam is flying for the rest of this test");

    /* three poses two seconds apart, going 10 units along x each time */
    st.pos[0] = 0.0f;  st.yaw = 0.0f;
    CHECK(fc_key_add(&st, 100.0f) == 1, "first keyframe");
    st.pos[0] = 10.0f; st.yaw = 0.2f;
    CHECK(fc_key_add(&st, 102.0f) == 2, "second keyframe");
    st.pos[0] = 20.0f; st.yaw = 0.4f;
    CHECK(fc_key_add(&st, 104.0f) == 3, "third keyframe");
    CHECK(NEAR(st.path[1].time, 2.0f, 1e-4) && NEAR(st.path[2].time, 4.0f, 1e-4),
          "the times are relative to the first keyframe");

    CHECK(!fc_playing(&st), "nothing is playing yet");
    st.pos[0] = -50.0f;                       /* somewhere else entirely */
    CHECK(fc_play_start(&st), "playback starts");
    CHECK(fc_playing(&st), "and says so");

    CHECK(fc_play_step(&st, 0.5f, &out), "playback produces a pose");
    CHECK(NEAR(st.pos[0], 2.5f, 1e-3), "a quarter of the way through the first leg (%f)", st.pos[0]);
    CHECK(fc_play_step(&st, 0.5f, &out), "still playing");
    CHECK(NEAR(st.pos[0], 5.0f, 1e-3), "halfway through the first leg (%f)", st.pos[0]);
    CHECK(NEAR(st.yaw, 0.1f, 1e-3), "and the angle is interpolated too (%f)", st.yaw);

    /* run to the end */
    {
        int i, guard = 0;
        while (fc_playing(&st) && guard++ < 1000) fc_play_step(&st, 0.1f, &out);
        CHECK(!fc_playing(&st), "the playback stops at the end");
        CHECK(NEAR(st.pos[0], 20.0f, 1e-3) && NEAR(st.yaw, 0.4f, 1e-3),
              "and leaves the camera on the last keyframe");
        (void)i;
    }

    /* pressing the key again replays from the start */
    CHECK(fc_play_start(&st), "it can be played again");
    fc_play_step(&st, 0.1f, &out);
    CHECK(st.pos[0] < 2.0f, "from the beginning (%f)", st.pos[0]);

    fc_play_stop(&st);
    CHECK(!fc_playing(&st), "and stopped on demand");
    fc_path_clear(&st);
    CHECK(st.path_count == 0 && !fc_play_start(&st), "an empty path cannot be played");
}

/* A path that crosses +-180 degrees has to turn the short way, not spin all the way. */
static void test_keyframe_yaw_wrap(void)
{
    fc_config cfg;
    fc_state st;
    fc_mat cam, out;
    float pos[3] = { 0.0f, 0.0f, 0.0f };

    fc_config_defaults(&cfg);
    fc_init(&st, &cfg);
    fc_mat_from_pose(&cam, pos, 0.0f, 0.0f, 0);
    fc_toggle(&st, &cam);

    st.yaw = 3.10f;  fc_key_add(&st, 0.0f);
    st.yaw = -3.10f; fc_key_add(&st, 1.0f);

    fc_play_start(&st);
    fc_play_step(&st, 0.5f, &out);
    CHECK(fabsf(fabsf(st.yaw) - 3.14159f) < 0.02f,
          "halfway round a 180 degree turn is 180 degrees, not zero (%f)", st.yaw);
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

/* ---- pairing a camera's world and view matrices ------------------------------ */

static void test_world_view_pair(void)
{
    fc_mat cands[3], id;
    int wi = -1, vi = -1;
    float pos[3] = { 100.0f, 5.0f, -40.0f };
    float nearp[3] = { 104.0f, 6.5f, -36.0f };   /* the player, a step from the camera */

    fc_mat_identity(&id);
    CHECK(fc_mat_near_identity(&id, 1e-5f), "the identity is the identity");

    /* world = where the camera stands, view = its inverse */
    fc_mat_from_pose(&cands[0], pos, 0.7f, -0.2f, 1);          /* the view matrix */
    fc_mat_from_pose(&cands[1], pos, 0.7f, -0.2f, 0);          /* the world matrix */
    fc_mat_from_pose(&cands[2], (const float[3]){ 0.0f, 0.0f, 0.0f }, 0.0f, 1.2f, 1); /* a projection-ish spread */

    CHECK(fc_pick_world_view(cands, 3, nearp, 30.0f, &wi, &vi) == 1, "the pair is found");
    CHECK(wi == 1, "the world matrix is the one standing near the player");
    CHECK(vi == 0, "the view matrix is its inverse");

    /* with the player far away the world matrix is rejected */
    {
        float far[3] = { 5000.0f, 0.0f, 5000.0f };
        CHECK(fc_pick_world_view(cands, 3, far, 30.0f, &wi, &vi) == 0,
              "a pair away from the player is refused");
    }

    /* two matrices that are not inverses are not a pair */
    {
        fc_mat two[2];
        fc_mat_from_pose(&two[0], pos, 0.1f, 0.1f, 0);
        fc_mat_from_pose(&two[1], pos, 1.9f, -0.4f, 0);
        CHECK(fc_pick_world_view(two, 2, pos, 30.0f, &wi, &vi) == 0, "not a pair");
    }

    /* the engine's own pair says which way round its view matrix goes */
    {
        fc_mat w, v;
        fc_mat_from_pose(&w, pos, -2.1f, 0.3f, 0);
        fc_mat_from_pose(&v, pos, -2.1f, 0.3f, 1);
        CHECK(fc_view_sign(&w, &v) == 1, "row 2 is -forward");
        fc_mat_from_pose(&v, pos, -2.1f, 0.3f, 2);
        CHECK(fc_view_sign(&w, &v) == 2, "row 2 is +forward");
        fc_mat_from_pose(&v, pos, -2.1f, 0.3f, 0);
        CHECK(fc_view_sign(&w, &v) == 0, "a world matrix is not a view matrix");
    }
}

/* ---- projection detection --------------------------------------------------- */

static void test_projection(void)
{
    fc_mat persp = { { 1.3f, 0.0f, 0.0f, 0.0f,
                       0.0f, 1.7f, 0.0f, 0.0f,
                       0.0f, 0.0f, 1.0005f, 1.0f,
                       0.0f, 0.0f, -0.2f, 0.0f } };
    fc_mat world, view, id;
    float pos[3] = { 10.0f, 2.0f, -3.0f };

    fc_mat_from_pose(&world, pos, 0.4f, 0.1f, 0);
    fc_mat_from_pose(&view, pos, 0.4f, 0.1f, 1);
    fc_mat_identity(&id);

    CHECK(fc_mat_is_projection(&persp), "a perspective matrix is a projection");
    CHECK(!fc_mat_is_projection(&world), "a world matrix is not");
    CHECK(!fc_mat_is_projection(&view), "a view matrix is not");
    CHECK(!fc_mat_is_projection(&id), "nor is the identity");
}

/* The pose a view matrix was built from. Note that a world matrix also round trips
 * as *some* view matrix - a view matrix is the same basis with a different row 3 - so
 * the only real test is that the answer rebuilds the matrix it came from. */
/* A camera with roll cannot be written as yaw and pitch, but the flight still has to
 * start from the right place and look the right way. */
static void test_pose_from_view_roll(void)
{
    fc_mat v;
    float pos[3] = { 5.0f, 6.0f, 7.0f }, got[3] = { 0, 0, 0 };
    float yaw = 0.0f, pitch = 0.0f, r[3], u[3], th = 0.35f, c = cosf(th), sn = sinf(th);
    int sign = 0, i;

    fc_mat_from_pose(&v, pos, 0.7f, 0.2f, 1);
    /* a view keeps the components of the basis in its rows, so rolling the camera about
     * its backwards axis mixes columns 0 and 1 of every row */
    for (i = 0; i < 3; i++) {
        r[i] = v.m[4 * i]; u[i] = v.m[4 * i + 1];
        v.m[4 * i] = r[i] * c + u[i] * sn;
        v.m[4 * i + 1] = -r[i] * sn + u[i] * c;
    }
    /* the translation has to follow the rolled basis, exactly as fc_mat_from_pose
     * writes it, or the matrix is not the view of any camera */
    for (i = 0; i < 3; i++) {
        v.m[12 + i] = -(v.m[i] * pos[0] + v.m[4 + i] * pos[1] + v.m[8 + i] * pos[2]);
    }

    CHECK(fc_mat_valid(&v), "a rolled view matrix is still rigid");
    CHECK(fc_pose_from_view(&v, got, &yaw, &pitch, &sign) == 1, "a rolled camera still gives a pose");
    CHECK(fabsf(got[0] - pos[0]) < 0.05f && fabsf(got[1] - pos[1]) < 0.05f &&
          fabsf(got[2] - pos[2]) < 0.05f, "from the right place (%.3f %.3f %.3f)",
          got[0], got[1], got[2]);
    CHECK(fabsf(yaw - 0.7f) < 0.02f && fabsf(pitch - 0.2f) < 0.02f, "looking the right way");
    CHECK(sign == 1, "with the row sign it uses");
}

/* An engine that keeps matrices column-major stores the same camera transposed. The
 * transposed copy is not a valid rigid transform, which is exactly how it is detected. */
static void test_transpose(void)
{
    fc_mat v, t, back;
    float pos[3] = { 3.0f, 4.0f, 5.0f };

    fc_mat_from_pose(&v, pos, 0.9f, 0.3f, 1);
    fc_mat_transpose(&v, &t);
    /* transposing a view puts the translation into the fourth column, which no rigid
     * transform has, and that is exactly how a column-major engine is spotted */
    CHECK(!fc_mat_valid(&t), "the transposed view matrix is not a rigid transform");
    CHECK(fc_pose_from_view(&t, NULL, NULL, NULL, NULL) == 0, "so it is not read as one");
    fc_mat_transpose(&t, &back);
    CHECK(back.m[0] == v.m[0] && back.m[5] == v.m[5] && back.m[12] == v.m[12],
          "transposing twice gives the matrix back");
    CHECK(fc_pose_from_view(&back, NULL, NULL, NULL, NULL) == 1, "and the original decomposes");
}

static void test_init_aliasing(void)
{
    fc_state st;

    memset(&st, 0, sizeof(st));
    fc_config_defaults(&st.cfg);
    st.cfg.speed = 42.0f;
    st.cfg.sensitivity = 0.5f;
    fc_init(&st, &st.cfg);          /* the config lives inside the state */
    CHECK(st.cfg.speed == 42.0f, "fc_init keeps a config that lives in the state");
    CHECK(st.cfg.sensitivity == 0.5f, "all of it");
    CHECK(st.active == 0, "and clears the state itself");
}

static void test_pose_from_view(void)
{
    fc_mat bad;
    float pos[3] = { -12.5f, 3.25f, 47.0f }, got[3] = { 0, 0, 0 };
    float yaw = 0.0f, pitch = 0.0f;
    int s;

    for (s = 1; s <= 2; s++) {
        fc_mat v;
        int sign = 0;

        fc_mat_from_pose(&v, pos, 1.1f, -0.22f, s);
        yaw = 0.0f; pitch = 0.0f;
        CHECK(fc_pose_from_view(&v, got, &yaw, &pitch, &sign) == 1,
              s == 1 ? "a view matrix gives its pose back" : "and so does the other row sign");
        CHECK(sign == s, "the row sign is reported");
        CHECK(fabsf(got[0] - pos[0]) < 0.01f && fabsf(got[1] - pos[1]) < 0.01f &&
              fabsf(got[2] - pos[2]) < 0.01f, "the eye comes back, not just on an axis");
        CHECK(fabsf(yaw - 1.1f) < 0.01f, "the yaw comes back");
        CHECK(fabsf(pitch + 0.22f) < 0.01f, "the pitch comes back");
    }

    fc_mat_identity(&bad);
    bad.m[0] = 2.0f;
    CHECK(fc_pose_from_view(&bad, got, &yaw, &pitch, NULL) == 0,
          "something that is not a rigid transform is refused");
}

int main(void)
{
    test_defaults();
    test_chain();
    test_chain_walk();
    test_world_view_pair();
    test_projection();
    test_pose_from_view();
    test_init_aliasing();
    test_transpose();
    test_pose_from_view_roll();
    test_config();
    test_matrices();
    test_spawn_matrix();
    test_toggle();
    test_movement();
    test_look();
    test_game_camera_matrix();
    test_freeze();
    test_follow();
    test_follow_pan();
    test_frozen_pan();
    test_playback_path();
    test_play_speed();
    test_keyframes();
    test_keyframe_yaw_wrap();
    test_restore();
    test_safety();

    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}

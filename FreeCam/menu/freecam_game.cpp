// freecam_game.cpp - free camera for the Triad Wars debug menu.
//
// How it works, in order:
//
//   1. Find the camera. UFG::BaseCameraComponent and its eight subclasses are
//      plain heap objects that start with a vtable pointer, and the nine vtables
//      are at fixed RVAs, so a heap scan for those pointers finds every camera in
//      the process - usually one or two objects, not thousands.
//   2. Work out the layout. Every camera component keeps three 4x4 matrices 0x40
//      apart at +0x40 (or +0x110 when the byte at +0x100 is set): the world
//      transform, its view matrix, and the projection. A view matrix is the world
//      matrix rebuilt from the same pose, so rebuild it both ways round and see
//      which one the game actually stored (fc_view_sign). The world matrix has to
//      be standing next to the player, which is the check that rejects everything
//      else in the heap.
//   3. Fly. The camera rewrites those matrices every frame, so one write per frame
//      would lose the race. A thread of our own re-writes the pair continuously
//      (world + view, both from our pose), so whatever order the game's update and
//      its render run in, it renders our pose.
//
// Everything is guarded with VirtualQuery: a stale pointer is a log line, not a
// crash. Set off with the menu entry, moved with WASD, back with the menu entry or
// by pressing the panic key.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>   // timeBeginPeriod, for a 1 ms sleep while flying

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "../src/freecam.h"
}

#include "freecam_game.h"
#include "log.h"

namespace tw {
namespace game {
namespace {

// --- what we know about this build of the game -------------------------------

constexpr uintptr_t kPlayerGlobal     = 0x23f25f8;  // void* local player
constexpr uintptr_t kPlayerNodeOffset = 0x58;       // player -> transform node
constexpr uintptr_t kPlayerMatrix     = 0x80;       // node -> 4x4 transform

// UFG::*CameraComponent vtables, relative to the image base.
const uintptr_t kCamVtables[] = {
    0x0182a800,  // BaseCameraComponent
    0x0182a9a0,  // DebugCameraComponent
    0x01936140,  // GameCameraComponent
    0x01936430,  // ChaseCameraComponent
    0x01936590,  // FollowCameraComponent
    0x01936ed0,  // AnimatedCameraComponent
    0x01937030,  // ScriptCameraComponent
    0x01937300,  // ProgressionCameraComponent
    0x01937da0,  // TurfCameraComponent
};

constexpr int    kMaxCameras   = 64;
constexpr float  kNearRadius   = 80.0f;   // camera must be this close to the player
constexpr int    kLookKey      = VK_RBUTTON;

struct Camera {
    uint8_t *obj = nullptr;
    int worldOffset = 0x40;
    int viewOffset  = 0x80;
    int viewSign    = 1;
    float distance  = 0.0f;
};

Camera   g_cam;
fc_state g_state;
HANDLE   g_thread = nullptr;
volatile LONG g_quit = 0;
volatile LONG g_live = 0;          // the menu's toggle state
POINT    g_lookOrigin = {};
bool     g_looking = false;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

bool Readable(const void *p, size_t n) {
    if (!p) return false;
    auto *at = static_cast<const uint8_t *>(p);
    MEMORY_BASIC_INFORMATION mbi;
    while (n) {
        if (!VirtualQuery(at, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        size_t left = static_cast<size_t>(static_cast<uint8_t *>(mbi.BaseAddress) +
                                          mbi.RegionSize - at);
        if (!left) return false;
        size_t step = left < n ? left : n;
        at += step;
        n -= step;
    }
    return true;
}

bool Writable(const void *p, size_t n) {
    if (!p) return false;
    auto *at = static_cast<const uint8_t *>(p);
    MEMORY_BASIC_INFORMATION mbi;
    while (n) {
        if (!VirtualQuery(at, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD prot = mbi.Protect & 0xff;
        if (!(prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
              prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY))
            return false;
        size_t left = static_cast<size_t>(static_cast<uint8_t *>(mbi.BaseAddress) +
                                          mbi.RegionSize - at);
        if (!left) return false;
        size_t step = left < n ? left : n;
        at += step;
        n -= step;
    }
    return true;
}

// Where the player is standing, from the same chain the menu already reads.
bool PlayerPosition(float out[3]) {
    auto *base = reinterpret_cast<uint8_t *>(Base());
    void *player = *reinterpret_cast<void **>(base + kPlayerGlobal);
    if (!Readable(player, 0x60)) return false;
    void *node = *reinterpret_cast<void **>(static_cast<uint8_t *>(player) + kPlayerNodeOffset);
    if (!Readable(node, 0xC0)) return false;
    const float *m = reinterpret_cast<const float *>(static_cast<uint8_t *>(node) + kPlayerMatrix);
    if (!std::isfinite(m[12]) || !std::isfinite(m[13]) || !std::isfinite(m[14])) return false;
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
    return true;
}

int ScanCameras(uint8_t *out[], int max) {
    const uintptr_t lo = Base();
    const uintptr_t hi = lo + 0x04000000;      // the exe image is 40 MB
    uintptr_t vt[sizeof(kCamVtables) / sizeof(kCamVtables[0])];
    for (size_t i = 0; i < sizeof(vt) / sizeof(vt[0]); i++) vt[i] = Base() + kCamVtables[i];

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = 0x10000;
    uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    int found = 0;

    while (addr < end && found < max) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void *>(addr), &mbi, sizeof mbi)) break;
        auto *region = static_cast<uint8_t *>(mbi.BaseAddress);
        size_t size = static_cast<size_t>(mbi.RegionSize);
        if (mbi.State == MEM_COMMIT) {
            DWORD prot = mbi.Protect & 0xff;
            if ((prot == PAGE_READWRITE || prot == PAGE_WRITECOPY) && !(mbi.Protect & PAGE_GUARD)) {
                for (size_t off = 0; off + 8 <= size && found < max; off += 8) {
                    uintptr_t v = *reinterpret_cast<uintptr_t *>(region + off);
                    if (v < lo || v >= hi) continue;
                    for (size_t k = 0; k < sizeof(vt) / sizeof(vt[0]); k++) {
                        if (v == vt[k]) {
                            out[found++] = region + off;
                            break;
                        }
                    }
                }
            }
        }
        if (!size) break;
        addr = reinterpret_cast<uintptr_t>(region) + size;
    }
    return found;
}

float Distance(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void LogMatrix(const char *what, const fc_mat &m) {
    TW_Log("%s [ %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f ]",
           what, m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5], m.m[6], m.m[7],
           m.m[8], m.m[9], m.m[10], m.m[11], m.m[12], m.m[13], m.m[14], m.m[15]);
}

// Look for the camera: every component whose three matrices contain a world/view
// pair standing next to the player. Closest one wins.
bool FindCamera() {
    float player[3];
    if (!PlayerPosition(player)) {
        TW_Log("FreeCam: the player transform is not readable yet, so the camera cannot be told apart");
        return false;
    }
    TW_Log("FreeCam: player at %.1f %.1f %.1f, scanning for camera components", player[0], player[1],
           player[2]);

    uint8_t *objs[kMaxCameras];
    int count = ScanCameras(objs, kMaxCameras);
    TW_Log("FreeCam: %d camera component(s) found", count);

    Camera best;
    best.distance = 1e9f;
    for (int i = 0; i < count; i++) {
        uint8_t *obj = objs[i];
        if (!Readable(obj, 0x200)) continue;
        const int block = (*(obj + 0x100) != 0) ? 0x110 : 0x40;
        fc_mat cands[3];
        std::memcpy(&cands[0], obj + block + 0x00, 64);
        std::memcpy(&cands[1], obj + block + 0x40, 64);
        std::memcpy(&cands[2], obj + block + 0x80, 64);

        int wi = -1, vi = -1;
        if (!fc_pick_world_view(cands, 3, player, kNearRadius, &wi, &vi)) continue;
        float pos[3], yaw, pitch;
        if (!fc_mat_to_pose(&cands[wi], pos, &yaw, &pitch, 0)) continue;
        float d = Distance(pos, player);
        if (d >= best.distance) continue;
        best.obj = obj;
        best.worldOffset = block + wi * 0x40;
        best.viewOffset = block + vi * 0x40;
        best.viewSign = fc_view_sign(&cands[wi], &cands[vi]);
        best.distance = d;
    }

    if (!best.obj) {
        TW_Log("FreeCam: no camera component is holding a world/view pair near the player "
               "(%d object(s) checked)", count);
        return false;
    }
    g_cam = best;
    TW_Log("FreeCam: camera at %p, world +0x%x, view +0x%x, view row sign %d, %.1f units from the player",
           g_cam.obj, g_cam.worldOffset, g_cam.viewOffset, g_cam.viewSign, g_cam.distance);
    {
        fc_mat m;
        std::memcpy(&m, g_cam.obj + g_cam.worldOffset, 64);
        LogMatrix("FreeCam: world ", m);
        std::memcpy(&m, g_cam.obj + g_cam.viewOffset, 64);
        LogMatrix("FreeCam: view  ", m);
    }
    return true;
}

bool CameraStillThere() {
    if (!g_cam.obj) return false;
    if (!Readable(g_cam.obj, 0x200)) return false;
    uintptr_t v = *reinterpret_cast<uintptr_t *>(g_cam.obj);
    uintptr_t lo = Base();
    for (size_t k = 0; k < sizeof(kCamVtables) / sizeof(kCamVtables[0]); k++) {
        if (v == lo + kCamVtables[k]) return true;
    }
    return false;
}

void WriteMatrices(const fc_mat &world, const fc_mat &view) {
    if (!g_cam.obj) return;
    if (Writable(g_cam.obj + g_cam.worldOffset, 64)) std::memcpy(g_cam.obj + g_cam.worldOffset, &world, 64);
    if (Writable(g_cam.obj + g_cam.viewOffset, 64)) std::memcpy(g_cam.obj + g_cam.viewOffset, &view, 64);
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void PollInput(float *mouseX, float *mouseY) {
    static const int map[FC_KEY_COUNT] = { 'W', 'S', 'A', 'D', VK_SPACE, 'C', VK_SHIFT, VK_CONTROL, 0 };
    for (int i = 0; i < FC_KEY_COUNT; ++i) g_state.keys[i] = map[i] ? (KeyDown(map[i]) ? 1 : 0) : 0;

    *mouseX = *mouseY = 0.0f;
    if (KeyDown(kLookKey)) {
        if (!g_looking) {
            GetCursorPos(&g_lookOrigin);
            g_looking = true;
        }
        POINT cur;
        GetCursorPos(&cur);
        *mouseX = static_cast<float>(cur.x - g_lookOrigin.x);
        *mouseY = static_cast<float>(cur.y - g_lookOrigin.y);
        if (*mouseX != 0.0f || *mouseY != 0.0f) SetCursorPos(g_lookOrigin.x, g_lookOrigin.y);
        g_state.keys[FC_LOOK] = 1;
    } else {
        g_looking = false;
    }
}

DWORD WINAPI Loop(LPVOID) {
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    int lost = 0;
    int held = 0, overwritten = 0;
    ULONGLONG lastReport = GetTickCount64();

    /* 1 ms instead of the default 15 ms, so the writes are spread across the frame
     * instead of arriving in one burst. */
    timeBeginPeriod(1);

    while (!g_quit) {
        float dt, mx, my;
        Sleep(1);

        QueryPerformanceCounter(&now);
        dt = static_cast<float>(static_cast<double>(now.QuadPart - prev.QuadPart) /
                                static_cast<double>(freq.QuadPart));
        prev = now;
        if (dt <= 0.0f) dt = 0.001f;
        if (dt > 0.1f) dt = 0.1f;

        if (!g_live) {
            lost = 0;
            continue;
        }

        /* F10 is the panic key: the camera goes straight back, whatever else is going on. */
        if (GetAsyncKeyState(VK_F10) & 0x8000) {
            TW_Log("FreeCam: panic key, putting the camera back");
            FreeCamSetActive(false);
            continue;
        }

        PollInput(&mx, &my);

        if (!CameraStillThere()) {
            if (++lost < 60) continue;                 // one bad frame is not a lost camera
            TW_Log("FreeCam: the camera object moved, looking for it again");
            g_cam.obj = nullptr;
            if (!FindCamera()) {
                TW_Log("FreeCam: switching off, no camera to fly");
                g_live = 0;
                g_state.active = 0;
                continue;
            }
            lost = 0;
        }

        fc_mat view;
        if (fc_step(&g_state, dt, mx, my, &view)) {
            fc_mat world;
            fc_mat_from_pose(&world, g_state.pos, g_state.yaw, g_state.pitch, 0);
            WriteMatrices(world, view);

            /* Did the game take it back? Once every other second is enough to say
             * whether the camera is really ours or being rewritten under us. */
            fc_mat after;
            if (Readable(g_cam.obj + g_cam.viewOffset, 64)) {
                std::memcpy(&after, g_cam.obj + g_cam.viewOffset, 64);
                int same = 1;
                for (int i = 0; i < 16; i++) {
                    float d = after.m[i] - view.m[i];
                    if (d < 0.0f) d = -d;
                    if (d > 0.001f) { same = 0; break; }
                }
                if (same) held++; else overwritten++;
            }
            ULONGLONG nowTick = GetTickCount64();
            if (nowTick - lastReport > 2000) {
                lastReport = nowTick;
                TW_Log("FreeCam: camera at %.1f %.1f %.1f, writes held %d, taken back %d",
                       g_state.pos[0], g_state.pos[1], g_state.pos[2], held, overwritten);
                held = overwritten = 0;
            }
        }
    }
    timeEndPeriod(1);
    return 0;
}

}  // namespace

bool FreeCamAvailable() {
    return Base() != 0 && Readable(reinterpret_cast<void *>(Base() + 0x1000), 16);
}

bool FreeCamActive() { return g_live != 0; }

void FreeCamSetSpeed(float unitsPerSecond) {
    if (unitsPerSecond < 1.0f) unitsPerSecond = 1.0f;
    if (unitsPerSecond > 200.0f) unitsPerSecond = 200.0f;
    g_state.cfg.speed = unitsPerSecond;
}

float FreeCamSpeed() { return g_state.cfg.speed; }

std::string FreeCamSetActive(bool on) {
    if (!on) {
        if (!g_live) return "Free camera: already off";
        fc_mat back;
        int restored = fc_restore(&g_state, &back);
        if (restored && g_cam.obj) {
            /* The saved matrix is the world one; rebuild the matching view from it, not
             * from where we flew to, so the game carries on from its own pose. */
            float pos[3], yaw = 0.0f, pitch = 0.0f;
            if (fc_mat_to_pose(&back, pos, &yaw, &pitch, 0)) {
                fc_mat view;
                fc_mat_from_pose(&view, pos, yaw, pitch, (g_cam.viewSign == 2) ? 2 : 1);
                WriteMatrices(back, view);
            }
        }
        g_live = 0;
        fc_set_active(&g_state, 0, nullptr);
        TW_Log("FreeCam: off after %.0f units", g_state.moved);
        return "Free camera: off";
    }

    if (g_live) return "Free camera: already on";
    if (!FreeCamAvailable()) return "Free camera: game module not found";

    if (!CameraStillThere()) {
        g_cam.obj = nullptr;
        if (!FindCamera()) {
            return "Free camera: no camera found (see TW_MenuLog.txt)";
        }
    }

    if (!g_thread) {
        fc_config_defaults(&g_state.cfg);
        fc_init(&g_state, &g_state.cfg);
        g_thread = CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr);
        if (!g_thread) return "Free camera: could not start the fly thread";
    }

    fc_mat world;
    std::memcpy(&world, g_cam.obj + g_cam.worldOffset, 64);
    if (!fc_mat_valid(&world)) {
        TW_Log("FreeCam: the world matrix at +0x%x is not a transform", g_cam.worldOffset);
        return "Free camera: camera transform looks wrong";
    }

    // Capture the pose from the world matrix, then write the view convention the game uses.
    g_state.cfg.view = 0;
    if (!fc_set_active(&g_state, 1, &world)) return "Free camera: could not take the camera over";
    g_state.cfg.view = (g_cam.viewSign == 2) ? 2 : 1;

    g_live = 1;
    g_looking = false;
    TW_Log("FreeCam: on at %.1f %.1f %.1f, yaw %.1f pitch %.1f", g_state.pos[0], g_state.pos[1],
           g_state.pos[2], g_state.yaw * 57.2957795f, g_state.pitch * 57.2957795f);
    return "Free camera: on - WASD, Space/C, hold right mouse";
}

void FreeCamTick() {
    // The fly thread does the work; this is only here so the render loop has a hook
    // if the camera ever needs to be nudged from it.
    if (!g_live) return;
}

}  // namespace game
}  // namespace tw

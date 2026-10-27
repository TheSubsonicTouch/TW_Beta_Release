// freecam_game.cpp - free camera for the Triad Wars debug menu.
//
// Three steps, in order:
//
//   1. Find the camera. UFG::BaseCameraComponent and its eight subclasses are plain
//      heap objects that begin with a vtable pointer, and the nine camera vtables
//      sit at fixed RVAs in TriadWars.exe, so a heap walk for those pointers finds
//      the live camera. The walk is time-budgeted per tick and runs on our own
//      thread: doing it on the render thread freezes the game for as long as it
//      takes to walk a multi-gigabyte heap, which is a hang, not a stutter.
//   2. Work out the layout. Every component keeps three 4x4 matrices 0x40 apart at
//      +0x40 (or +0x110 when the byte at +0x100 is set): world transform, view
//      matrix, projection. A view matrix is the world matrix rebuilt from the same
//      pose, so rebuild both ways round and keep whichever the game stored
//      (fc_view_sign). The world matrix also has to stand next to the player, which
//      is what rejects every other transform in the process.
//   3. Fly. The game rewrites the camera's matrices every frame, so a single write
//      per frame would lose the race; our thread rewrites the pair continuously.
//
// Safety: every pointer is checked with VirtualQuery before it is touched, the scan
// reads under SEH so a page released mid-walk costs a skipped chunk rather than the
// process, and nothing in here blocks the caller.

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
constexpr size_t kCamVtableCount = sizeof(kCamVtables) / sizeof(kCamVtables[0]);

constexpr float  kNearRadius   = 120.0f;    // the camera must be this close to the player
constexpr size_t kObjectBytes  = 0x200;     // enough for all three matrices
constexpr int    kLookKey      = VK_RBUTTON;
constexpr float  kScanBudgetMs = 2.0f;      // heap walked per tick, never more
constexpr size_t kScanChunk    = 64 * 1024; // bytes per fault-guarded read, per clock check

struct Camera {
    uint8_t *obj = nullptr;
    int worldOffset = 0x40;
    int viewOffset = 0x80;
    int viewSign = 1;
    float distance = 0.0f;
};

struct ScanCtx {
    const float *player;
    uintptr_t lo, hi;
    uintptr_t vt[kCamVtableCount];
    Camera *out;
};

Camera   g_cam;
fc_state g_state;
HANDLE   g_thread = nullptr;
volatile LONG g_quit = 0;          // the DLL is going away
volatile LONG g_want = 0;          // the menu toggle: the player wants free cam
volatile LONG g_live = 0;          // the camera is actually being flown
POINT    g_lookOrigin = {};
bool     g_looking = false;

uintptr_t g_scanAddr = 0x10000;    // search cursor, kept between ticks
int       g_scanPasses = 0;
int       g_scanRegions = 0;
bool      g_searchLogged = false;
bool      g_noPlayerLogged = false;

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

double MsSince(const LARGE_INTEGER &a, const LARGE_INTEGER &now, const LARGE_INTEGER &freq) {
    return static_cast<double>(now.QuadPart - a.QuadPart) * 1000.0 /
           static_cast<double>(freq.QuadPart);
}

// Where the player is standing, from the same chain the menu already reads.
bool PlayerPosition(float out[3]) {
    auto *base = reinterpret_cast<uint8_t *>(Base());
    if (!base) return false;
    void *player = *reinterpret_cast<void **>(base + kPlayerGlobal);
    if (!Readable(player, 0x60)) return false;
    void *node = *reinterpret_cast<void **>(static_cast<uint8_t *>(player) + kPlayerNodeOffset);
    if (!Readable(node, 0xC0)) return false;
    const float *m = reinterpret_cast<const float *>(static_cast<uint8_t *>(node) + kPlayerMatrix);
    if (!std::isfinite(m[12]) || !std::isfinite(m[13]) || !std::isfinite(m[14])) return false;
    if (std::fabs(m[12]) > 100000.0f || std::fabs(m[14]) > 100000.0f) return false;
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
    return true;
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

// --- finding the camera ------------------------------------------------------

// Is this the start of a camera component holding a world/view pair standing next
// to the player? Fills `out` when it is.
bool TestObject(uint8_t *obj, const float player[3], Camera *out) {
    if (!obj || (reinterpret_cast<uintptr_t>(obj) & 7)) return false;
    if (!Readable(obj, kObjectBytes)) return false;

    const int block = (*(obj + 0x100) != 0) ? 0x110 : 0x40;
    fc_mat cands[3];
    std::memcpy(&cands[0], obj + block + 0x00, 64);
    std::memcpy(&cands[1], obj + block + 0x40, 64);
    std::memcpy(&cands[2], obj + block + 0x80, 64);

    int wi = -1, vi = -1;
    if (!fc_pick_world_view(cands, 3, player, kNearRadius, &wi, &vi)) return false;

    float pos[3], yaw = 0.0f, pitch = 0.0f;
    if (!fc_mat_to_pose(&cands[wi], pos, &yaw, &pitch, 0)) return false;
    int sign = fc_view_sign(&cands[wi], &cands[vi]);
    if (!sign) return false;

    out->obj = obj;
    out->worldOffset = block + wi * 0x40;
    out->viewOffset = block + vi * 0x40;
    out->viewSign = sign;
    out->distance = Distance(pos, player);
    return true;
}

// The inner loop, kept free of anything that needs unwinding so it can sit under SEH.
bool ScanChunkInner(const ScanCtx &ctx, uintptr_t from, uintptr_t to, bool *found) {
    const uintptr_t span = ctx.hi - ctx.lo;
    uintptr_t p = (from + 7) & ~static_cast<uintptr_t>(7);

    for (; p + 8 <= to; p += 8) {
        uintptr_t v = *reinterpret_cast<uintptr_t *>(p);
        if (v - ctx.lo > span) continue;              // one compare, wrapping: the fast path
        for (size_t k = 0; k < kCamVtableCount; k++) {
            if (v != ctx.vt[k]) continue;
            if (TestObject(reinterpret_cast<uint8_t *>(p), ctx.player, ctx.out)) {
                *found = true;
                return true;
            }
            break;
        }
    }
    return true;
}

// The game can release a page while we are walking it. One fault should cost a
// skipped chunk, not the process.
bool ScanChunk(const ScanCtx &ctx, uintptr_t from, uintptr_t to, bool *found) {
    *found = false;
    __try {
        ScanChunkInner(ctx, from, to, found);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *found = false;
        return false;
    }
    return true;
}

// Walk a region in fault-guarded chunks. `next` is where to carry on, so a budgeted
// stop does not skip the rest of the region.
bool ScanRegion(uintptr_t from, uintptr_t end, const ScanCtx &ctx, const LARGE_INTEGER &t0,
                const LARGE_INTEGER &freq, uintptr_t *next) {
    uintptr_t p = from;
    LARGE_INTEGER now;

    while (p + 8 <= end) {
        uintptr_t stop = p + kScanChunk;
        if (stop > end) stop = end;

        bool found = false;
        ScanChunk(ctx, p, stop, &found);
        if (found) {
            *next = stop;
            return true;
        }
        p = stop;

        QueryPerformanceCounter(&now);
        if (MsSince(t0, now, freq) > kScanBudgetMs) {
            *next = p;                                // resume here next tick
            return false;
        }
    }
    *next = end;
    return false;
}

// One budgeted slice of the search. Returns true when the camera has been found.
bool SearchStep() {
    float player[3];
    if (!PlayerPosition(player)) {
        if (!g_noPlayerLogged) {
            g_noPlayerLogged = true;
            TW_Log("FreeCam: waiting for the player transform - the camera can only be told apart "
                   "once you are in the world");
        }
        return false;
    }
    g_noPlayerLogged = false;

    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t0, now;
    QueryPerformanceCounter(&t0);

    ScanCtx ctx;
    ctx.player = player;
    ctx.lo = Base() + kCamVtables[0];
    ctx.hi = Base() + kCamVtables[kCamVtableCount - 1];
    ctx.out = &g_cam;
    for (size_t i = 0; i < kCamVtableCount; i++) ctx.vt[i] = Base() + kCamVtables[i];

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    if (g_scanAddr < 0x10000 || g_scanAddr >= end) g_scanAddr = 0x10000;

    while (g_scanAddr < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void *>(g_scanAddr), &mbi, sizeof mbi)) {
            g_scanAddr += 0x10000;
            continue;
        }
        uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = regionBase + mbi.RegionSize;
        if (regionEnd <= g_scanAddr) {                // defensive: always move forward
            g_scanAddr += 0x1000;
            continue;
        }

        const DWORD prot = mbi.Protect & 0xff;
        const bool scannable =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY);

        uintptr_t next = regionEnd;
        if (scannable) {
            g_scanRegions++;
            if (ScanRegion(g_scanAddr, regionEnd, ctx, t0, freq, &next)) {
                TW_Log("FreeCam: camera at %p, world +0x%x, view +0x%x, view row sign %d, "
                       "%.1f units from the player (after %d region(s))",
                       g_cam.obj, g_cam.worldOffset, g_cam.viewOffset, g_cam.viewSign,
                       g_cam.distance, g_scanRegions);
                fc_mat m;
                std::memcpy(&m, g_cam.obj + g_cam.worldOffset, 64);
                LogMatrix("FreeCam: world ", m);
                std::memcpy(&m, g_cam.obj + g_cam.viewOffset, 64);
                LogMatrix("FreeCam: view  ", m);
                return true;
            }
        }
        g_scanAddr = next > g_scanAddr ? next : regionEnd;

        QueryPerformanceCounter(&now);
        if (MsSince(t0, now, freq) > kScanBudgetMs) return false;
    }

    g_scanAddr = 0x10000;
    if (++g_scanPasses % 10 == 1) {
        TW_Log("FreeCam: pass %d over the heap found no camera near the player "
               "(%.0f %.0f %.0f); still looking", g_scanPasses, player[0], player[1], player[2]);
    }
    return false;
}

bool IsOurVtable(uint8_t *obj) {
    if (!obj || !Readable(obj, 8)) return false;
    uintptr_t v = *reinterpret_cast<uintptr_t *>(obj);
    uintptr_t lo = Base();
    for (size_t k = 0; k < kCamVtableCount; k++) {
        if (v == lo + kCamVtables[k]) return true;
    }
    return false;
}

// A fault here would be a page released between the check and the store, so the
// pair is written under the same guard the scan uses.
bool WriteMatrices(const fc_mat &world, const fc_mat &view) {
    if (!g_cam.obj) return false;
    bool ok = false;
    __try {
        if (Writable(g_cam.obj + g_cam.worldOffset, 64) &&
            Writable(g_cam.obj + g_cam.viewOffset, 64)) {
            std::memcpy(g_cam.obj + g_cam.worldOffset, &world, 64);
            std::memcpy(g_cam.obj + g_cam.viewOffset, &view, 64);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// --- flying ------------------------------------------------------------------

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

bool StartFlying() {
    fc_mat world;
    std::memcpy(&world, g_cam.obj + g_cam.worldOffset, 64);
    if (!fc_mat_valid(&world)) {
        TW_Log("FreeCam: the matrix at +0x%x is not a transform; searching again", g_cam.worldOffset);
        return false;
    }

    fc_config_defaults(&g_state.cfg);
    g_state.cfg.view = 0;            // fc_step hands back a world matrix; the view is built here
    fc_init(&g_state, &g_state.cfg);
    if (!fc_set_active(&g_state, 1, &world)) return false;

    g_cam.distance = 0.0f;
    g_looking = false;
    g_live = 1;
    TW_Log("FreeCam: on at %.1f %.1f %.1f (yaw %.0f, pitch %.0f, view row sign %d)",
           g_state.pos[0], g_state.pos[1], g_state.pos[2], g_state.yaw * 57.2957795f,
           g_state.pitch * 57.2957795f, g_cam.viewSign);
    return true;
}

void StopFlying() {
    fc_mat back;
    if (fc_restore(&g_state, &back) && g_cam.obj) {
        float pos[3], yaw = 0.0f, pitch = 0.0f;
        if (fc_mat_to_pose(&back, pos, &yaw, &pitch, 0)) {
            fc_mat view;
            fc_mat_from_pose(&view, pos, yaw, pitch, g_cam.viewSign);
            WriteMatrices(back, view);
        }
    }
    fc_set_active(&g_state, 0, nullptr);
    g_live = 0;
    g_looking = false;
    TW_Log("FreeCam: off after %.0f units", g_state.moved);
}

DWORD WINAPI Loop(LPVOID) {
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    int lost = 0, held = 0, taken = 0, refused = 0;
    ULONGLONG lastReport = GetTickCount64();

    timeBeginPeriod(1);
    /* A build stamp, so the log says which DLL is installed. */
    TW_Log("FreeCam: camera thread up (build 2, search on this thread, %d camera vtables)",
           static_cast<int>(kCamVtableCount));

    for (;;) {
        if (g_quit) break;

        if (!g_want) {
            if (g_live) StopFlying();
            Sleep(5);
            prev.QuadPart = 0;
            continue;
        }

        if (!g_cam.obj) {
            if (SearchStep()) {
                if (StartFlying()) prev.QuadPart = 0;
                else g_cam.obj = nullptr;
            }
            Sleep(2);
            continue;
        }

        if (!IsOurVtable(g_cam.obj)) {
            if (++lost < 30) { Sleep(1); continue; }   // never spin: a lost object is not urgent
            TW_Log("FreeCam: the camera object is gone from %p, looking again", g_cam.obj);
            g_cam.obj = nullptr;
            lost = 0;
            continue;
        }
        lost = 0;

        if (!g_live) {
            if (!StartFlying()) { g_cam.obj = nullptr; continue; }
            prev.QuadPart = 0;
        }

        QueryPerformanceCounter(&now);
        float dt = prev.QuadPart ? static_cast<float>(MsSince(prev, now, freq) / 1000.0) : 0.016f;
        prev = now;
        if (dt <= 0.0f) dt = 0.001f;
        if (dt > 0.1f) dt = 0.1f;

        /* F10 is the panic key: put the camera back, whatever else is going on. */
        if (KeyDown(VK_F10)) {
            TW_Log("FreeCam: panic key, putting the camera back");
            g_want = 0;
            StopFlying();
            continue;
        }

        float mx, my;
        PollInput(&mx, &my);

        fc_mat world;
        if (fc_step(&g_state, dt, mx, my, &world)) {
            fc_mat view;
            fc_mat_from_pose(&view, g_state.pos, g_state.yaw, g_state.pitch, g_cam.viewSign);

            if (WriteMatrices(world, view)) {
                refused = 0;
                /* Did the game take it straight back? Once every other second is
                 * enough to say whether the camera is really ours. */
                fc_mat after;
                if (Readable(g_cam.obj + g_cam.viewOffset, 64)) {
                    std::memcpy(&after, g_cam.obj + g_cam.viewOffset, 64);
                    int same = 1;
                    for (int i = 0; i < 16; i++) {
                        float d = after.m[i] - view.m[i];
                        if (d < 0.0f) d = -d;
                        if (d > 0.001f) { same = 0; break; }
                    }
                    if (same) held++; else taken++;
                }
            } else if (++refused > 100) {
                TW_Log("FreeCam: the camera memory will not take a write; switching off");
                g_want = 0;
                g_cam.obj = nullptr;
                StopFlying();
                continue;
            }

            ULONGLONG nowTick = GetTickCount64();
            if (nowTick - lastReport > 2000) {
                lastReport = nowTick;
                TW_Log("FreeCam: camera at %.1f %.1f %.1f, writes held %d, taken back %d",
                       g_state.pos[0], g_state.pos[1], g_state.pos[2], held, taken);
                held = taken = 0;
            }
        }
        Sleep(1);
    }

    if (g_live) StopFlying();
    g_live = 0;
    timeEndPeriod(1);
    return 0;
}

void EnsureThread() {
    if (g_thread) return;
    g_thread = CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr);
    if (!g_thread) TW_Log("FreeCam: could not start the camera thread");
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

// Never blocks: the menu calls this from the render thread, so all it does is set a
// flag and let the camera thread get on with it.
std::string FreeCamSetActive(bool on) {
    if (!on) {
        if (!g_want && !g_live) return "Free camera: already off";
        g_want = 0;
        return "Free camera: off";
    }
    if (g_want) return "Free camera: already on";
    if (!FreeCamAvailable()) return "Free camera: game module not found";

    EnsureThread();
    if (!g_thread) return "Free camera: could not start the camera thread";

    g_want = 1;
    if (g_live) return "Free camera: on";
    if (!g_searchLogged) {
        g_searchLogged = true;
        TW_Log("FreeCam: switched on, looking for the camera");
    }
    return "Free camera: on - looking for it (see TW_MenuLog.txt)";
}

void FreeCamTick() {
    // The camera runs on its own thread, so there is nothing to do per frame.
}

}  // namespace game
}  // namespace tw

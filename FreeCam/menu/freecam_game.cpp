// freecam_game.cpp - free camera for the Triad Wars debug menu.
//
// HOW THE GAME DRAWS ITS CAMERA (from TriadWars.exe, build in TestingSkybox)
// -------------------------------------------------------------------------
// UFG::BaseCameraComponent keeps three 4x4 matrices, 0x40 apart, at +0x40 (or at
// +0x110 when the byte at +0x100 is set):
//
//     +0x40   view matrix      accessor 0xcfc30   (gets 0x110 when +0x100 is set)
//     +0x80   world transform  accessor 0xcfc10   (gets 0x150)
//     +0xc0   projection       accessor 0xcfbf0   (gets 0x190)
//
// The render path (0x641f0) asks the camera manager at 0x23e0150 for the active
// camera (manager+0x50), calls the view and projection accessors on it, copies both
// out and multiplies them (0x2d520 calls 0x1400049b0 with view and projection). So
// the view accessor is the one thing whose answer decides what is drawn.
//
// The accessors are plain functions called directly (`call 0x1400cfc30`), not vtable
// slots - there are 17 call sites for the view one - so patching the function body
// catches every one of them.
//
// WHY THE FIRST ATTEMPT DID NOTHING
// ---------------------------------
// It found *a* camera by scanning the heap for camera vtables and took the one
// nearest the player. That is not necessarily the camera being drawn - the engine
// keeps several camera components, and the renderer uses the one the manager points
// at. Writing matrices into a camera that is not drawn is invisible.
//
// WHAT THIS DOES NOW
// ------------------
//   1. Resolve the camera the engine draws: manager (0x23e0150) -> +0x50, exactly
//      as the renderer does it, validated against the nine known camera vtables and
//      against its own matrices (one of the three has to be a projection, one a
//      transform, and the third has to be that transform's exact inverse). If the
//      engine path fails, fall back to the old heap scan, which also asks for the
//      camera to be near the player.
//   2. Detour the view and world accessors of *that object only*. While free cam is
//      on, a call to them returns a matrix rebuilt from our pose, refreshed on the
//      spot. The engine rewrites the matrices every frame, so simply storing new
//      values loses the race; answering the accessor cannot lose, because the
//      answer is recomputed at the moment it is asked for.
//   3. Still write the same matrices into the object, so code that reads the memory
//      directly (rather than through an accessor) sees our pose too.
//
// Everything is guarded: VirtualQuery before every read, SEH around the scan and
// the writes, byte-verified detours that are skipped if the build differs, and the
// accessors forward to the untouched originals whenever free cam is off or the call
// is for a different camera.

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
constexpr uintptr_t kCameraManager    = 0x23e0150;  // void* camera manager singleton
constexpr int       kManagerActive    = 0x50;       // manager -> the camera in use
constexpr int       kCameraInWrapper  = 0x50;       // wrapper -> its component

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
const char *const kCamNames[] = {
    "BaseCameraComponent", "DebugCameraComponent", "GameCameraComponent",
    "ChaseCameraComponent", "FollowCameraComponent", "AnimatedCameraComponent",
    "ScriptCameraComponent", "ProgressionCameraComponent", "TurfCameraComponent",
};
constexpr size_t kCamVtableCount = sizeof(kCamVtables) / sizeof(kCamVtables[0]);

// The three accessors, with their exact opening bytes. They are tiny leaf functions
// that return a pointer into the object; the detour copies the whole function into
// executable memory first, so the original behaviour is always available.
struct Accessor {
    uintptr_t rva;
    const char *name;
    std::string bytes;          // the original function, byte for byte
    void *trampoline = nullptr; // our copy of it, callable
    bool installed = false;
    void *(*fn)(void *) = nullptr;   // our detour
};

Accessor g_viewAcc  = { 0xcfc30, "view",
    std::string("\x80\xb9\x00\x01\x00\x00\x00\x48\x8d\x81\x10\x01\x00\x00\x75\x04"
                "\x48\x8d\x41\x40\xf3\xc3", 22) };
Accessor g_worldAcc = { 0xcfc10, "world",
    std::string("\x80\xb9\x00\x01\x00\x00\x00\x48\x8d\x81\x50\x01\x00\x00\x75\x07"
                "\x48\x8d\x81\x80\x00\x00\x00\xf3\xc3", 25) };

constexpr float  kNearRadius   = 150.0f;    // world transform must be this close to the player
constexpr size_t kObjectBytes  = 0x200;     // enough for all three matrices
constexpr int    kLookKey      = VK_RBUTTON;
constexpr float  kScanBudgetMs = 2.0f;
constexpr size_t kScanChunk    = 64 * 1024;

struct Camera {
    uint8_t *obj = nullptr;      // the component (what the accessors are called on)
    int viewOff = 0x40;
    int worldOff = 0x80;
    int projOff = 0xc0;
    int viewSign = 1;
    int vtableIndex = -1;
    const char *how = "none";
    bool live = false;           // whether the engine path found it
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
volatile LONG g_quit = 0;
volatile LONG g_want = 0;
volatile LONG g_live = 0;
POINT    g_lookOrigin = {};
bool     g_looking = false;

// fresh answers for the accessor detours
fc_mat g_viewOut;
fc_mat g_worldOut;

uintptr_t g_scanAddr = 0x10000;
int       g_scanPasses = 0;
int       g_scanRegions = 0;
bool      g_searchLogged = false;
bool      g_noPlayerLogged = false;
bool      g_engineLogged = false;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// --- memory helpers ----------------------------------------------------------

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

int CameraVtableIndex(uint8_t *obj) {
    if (!obj || !Readable(obj, 8)) return -1;
    uintptr_t v = *reinterpret_cast<uintptr_t *>(obj);
    for (size_t k = 0; k < kCamVtableCount; k++) {
        if (v == Base() + kCamVtables[k]) return static_cast<int>(k);
    }
    return -1;
}

bool PlayerPosition(float out[3]) {
    auto *base = reinterpret_cast<uint8_t *>(Base());
    if (!base) return false;
    if (!Readable(base + kPlayerGlobal, 8)) return false;
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

// --- adopting a camera -------------------------------------------------------

// Read the three matrices and decide whether this really is a camera component we
// can drive: one projection, one transform, and a view that is exactly that
// transform rebuilt the other way round. Nothing is written unless all three agree.
bool AdoptCamera(uint8_t *obj, const char *how, Camera *out, bool requireNear) {
    int vt = CameraVtableIndex(obj);
    if (vt < 0) return false;
    if (!Readable(obj, kObjectBytes)) return false;

    const int block = (*(obj + 0x100) != 0) ? 0x110 : 0x40;
    fc_mat view, world, proj;
    std::memcpy(&view, obj + block + 0x00, 64);
    std::memcpy(&world, obj + block + 0x40, 64);
    std::memcpy(&proj, obj + block + 0x80, 64);

    if (!fc_mat_is_projection(&proj)) return false;
    if (!fc_mat_valid(&view) || !fc_mat_valid(&world)) return false;

    int sign = fc_view_sign(&world, &view);
    if (!sign) return false;

    float pos[3], yaw = 0.0f, pitch = 0.0f;
    if (!fc_mat_to_pose(&world, pos, &yaw, &pitch, 0)) return false;

    float player[3];
    float dist = -1.0f;
    if (PlayerPosition(player)) {
        dist = Distance(pos, player);
        if (requireNear && dist > kNearRadius) return false;
    }

    out->obj = obj;
    out->viewOff = block + 0x00;
    out->worldOff = block + 0x40;
    out->projOff = block + 0x80;
    out->viewSign = sign;
    out->vtableIndex = vt;
    out->how = how;
    out->live = true;

    TW_Log("FreeCam: using %s at %p (%s), view +0x%x, world +0x%x, view row sign %d, %.1f units from the player",
           how, obj, kCamNames[vt], out->viewOff, out->worldOff, sign, dist);
    LogMatrix("FreeCam: its world", world);
    LogMatrix("FreeCam: its view ", view);
    return true;
}

// The pointer the renderer would use right now, without validating it.
uint8_t *EngineActiveCamera() {
    auto *base = reinterpret_cast<uint8_t *>(Base());
    if (!base || !Readable(base + kCameraManager, sizeof(void *))) return nullptr;
    void *manager = *reinterpret_cast<void **>(base + kCameraManager);
    if (!manager || !Readable(manager, kManagerActive + sizeof(void *))) return nullptr;
    return static_cast<uint8_t *>(
        *reinterpret_cast<void **>(static_cast<uint8_t *>(manager) + kManagerActive));
}

// The camera the renderer uses. The render code does exactly this:
//
//     0x64220  call 0xcf9d0          ; rax = [0x23e0150], the camera manager
//     0x64225  mov  rdi, [rax+0x50]  ; rdi = the camera in use
//     0x64318  mov  rcx, rdi
//     0x64318  call 0xcfc30          ; the view accessor, on rdi
//
// so one hop out of the manager *is* the component - there is no second level.
// (An earlier version added another +0x50 and ended up writing into a neighbour.)
// The wrapper form is still tried as a second candidate so a build with a level of
// indirection extra would work too.
bool ResolveByEngine(Camera *out) {
    uint8_t *active = EngineActiveCamera();
    if (!active) return false;

    uint8_t *candidates[2];
    int count = 0;
    if (Readable(active, kObjectBytes)) candidates[count++] = static_cast<uint8_t *>(active);
    if (Readable(active, kCameraInWrapper + sizeof(void *))) {
        void *inner = *reinterpret_cast<void **>(static_cast<uint8_t *>(active) + kCameraInWrapper);
        if (inner && inner != active && Readable(inner, kObjectBytes))
            candidates[count++] = static_cast<uint8_t *>(inner);
    }

    for (int i = 0; i < count; i++) {
        if (AdoptCamera(candidates[i], "the engine's active camera", out, false)) return true;
    }

    if (!g_engineLogged) {
        g_engineLogged = true;
        TW_Log("FreeCam: the engine's camera at %p did not look like a camera component; "
               "falling back to the heap scan", active);
        if (count) {
            fc_mat m;
            std::memcpy(&m, candidates[0] + 0x40, 64);
            LogMatrix("FreeCam: its first matrix", m);
        }
    }
    return false;
}

// --- fallback: scan the heap ------------------------------------------------

bool TestObject(uint8_t *obj, const float player[3], Camera *out) {
    if (!AdoptCamera(obj, "a heap scan", out, true)) return false;
    fc_mat world;
    float pos[3], yaw = 0.0f, pitch = 0.0f;
    std::memcpy(&world, obj + out->worldOff, 64);
    if (!fc_mat_to_pose(&world, pos, &yaw, &pitch, 0)) return false;
    return Distance(pos, player) <= kNearRadius;
}

bool ScanChunkInner(const ScanCtx &ctx, uintptr_t from, uintptr_t to, bool *found) {
    const uintptr_t span = ctx.hi - ctx.lo;
    uintptr_t p = (from + 7) & ~static_cast<uintptr_t>(7);
    for (; p + 8 <= to; p += 8) {
        uintptr_t v = *reinterpret_cast<uintptr_t *>(p);
        if (v - ctx.lo > span) continue;
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

bool ScanRegion(uintptr_t from, uintptr_t end, const ScanCtx &ctx, const LARGE_INTEGER &t0,
                const LARGE_INTEGER &freq, uintptr_t *next) {
    uintptr_t p = from;
    LARGE_INTEGER now;
    while (p + 8 <= end) {
        uintptr_t stop = p + kScanChunk;
        if (stop > end) stop = end;
        bool found = false;
        ScanChunk(ctx, p, stop, &found);
        if (found) { *next = stop; return true; }
        p = stop;
        QueryPerformanceCounter(&now);
        if (MsSince(t0, now, freq) > kScanBudgetMs) { *next = p; return false; }
    }
    *next = end;
    return false;
}

bool SearchStep() {
    float player[3] = { 0.0f, 0.0f, 0.0f };
    if (!PlayerPosition(player)) {
        if (!g_noPlayerLogged) {
            g_noPlayerLogged = true;
            TW_Log("FreeCam: waiting for the player transform - needed to tell a camera from the rest "
                   "of the world");
        }
    } else {
        g_noPlayerLogged = false;
    }

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
        if (regionEnd <= g_scanAddr) { g_scanAddr += 0x1000; continue; }

        const DWORD prot = mbi.Protect & 0xff;
        const bool scannable =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY);

        uintptr_t next = regionEnd;
        if (scannable) {
            g_scanRegions++;
            if (ScanRegion(g_scanAddr, regionEnd, ctx, t0, freq, &next)) {
                TW_Log("FreeCam: camera found after %d region(s)", g_scanRegions);
                return true;
            }
        }
        g_scanAddr = next > g_scanAddr ? next : regionEnd;

        QueryPerformanceCounter(&now);
        if (MsSince(t0, now, freq) > kScanBudgetMs) return false;
    }

    g_scanAddr = 0x10000;
    if (++g_scanPasses % 10 == 1) {
        TW_Log("FreeCam: pass %d over the heap found no camera; still looking", g_scanPasses);
    }
    return false;
}

// --- the accessor detours ----------------------------------------------------

void *CallOriginal(Accessor &acc, void *self) {
    if (!acc.trampoline) return nullptr;
    return reinterpret_cast<void *(*)(void *)>(acc.trampoline)(self);
}

// While free cam is on, anyone asking this camera for its view gets our pose,
// rebuilt at the moment of the question. Nothing else is affected: other cameras,
// and every call while free cam is off, go to the untouched original.
extern "C" void *FreeCamViewAccessor(void *self) {
    if (g_live && self && self == g_cam.obj) {
        fc_mat view;
        fc_mat_from_pose(&view, g_state.pos, g_state.yaw, g_state.pitch, g_cam.viewSign);
        g_viewOut = view;
        return &g_viewOut;
    }
    return CallOriginal(g_viewAcc, self);
}

extern "C" void *FreeCamWorldAccessor(void *self) {
    if (g_live && self && self == g_cam.obj) {
        fc_mat world;
        fc_mat_from_pose(&world, g_state.pos, g_state.yaw, g_state.pitch, 0);
        g_worldOut = world;
        return &g_worldOut;
    }
    return CallOriginal(g_worldAcc, self);
}

bool InstallDetour(Accessor &acc, void *detour) {
    if (acc.installed) return true;
    auto *target = reinterpret_cast<uint8_t *>(Base() + acc.rva);
    const size_t len = acc.bytes.size();

    if (!Readable(target, len) || std::memcmp(target, acc.bytes.data(), len) != 0) {
        TW_Log("FreeCam: the %s accessor does not match this game build; leaving it alone "
               "(free cam will still write the matrices, but the engine may redraw over it)",
               acc.name);
        return false;
    }

    // Our own copy of the original, so the detour always has a working fallback.
    void *copy = VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!copy) return false;
    std::memcpy(copy, acc.bytes.data(), len);
    acc.trampoline = copy;

    DWORD old = 0;
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old)) {
        TW_Log("FreeCam: could not unlock the %s accessor", acc.name);
        return false;
    }
    // jmp rel32 to our function, then int3 over the bytes we replaced
    auto *p = reinterpret_cast<uint8_t *>(target);
    const intptr_t rel = reinterpret_cast<intptr_t>(detour) - (reinterpret_cast<intptr_t>(p) + 5);
    p[0] = 0xE9;
    std::memcpy(p + 1, &rel, 4);
    std::memset(p + 5, 0xCC, len - 5);
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);

    acc.installed = true;
    acc.fn = reinterpret_cast<void *(*)(void *)>(detour);
    TW_Log("FreeCam: hooked the %s accessor at %p (original kept at %p)", acc.name, target, copy);
    return true;
}

bool EnsureDetours() {
    bool view = InstallDetour(g_viewAcc, reinterpret_cast<void *>(&FreeCamViewAccessor));
    // The world accessor only has three callers, none of them the renderer, so it is
    // a nicety rather than a requirement: the view answer alone decides the picture.
    if (!InstallDetour(g_worldAcc, reinterpret_cast<void *>(&FreeCamWorldAccessor)) && view) {
        TW_Log("FreeCam: the world accessor was left alone; the view answer is still ours");
    }
    return view;
}

// --- writing the matrices ----------------------------------------------------

bool WriteMatrices(const fc_mat &world, const fc_mat &view) {
    if (!g_cam.obj) return false;
    bool ok = false;
    __try {
        if (Writable(g_cam.obj + g_cam.worldOff, 64) && Writable(g_cam.obj + g_cam.viewOff, 64)) {
            std::memcpy(g_cam.obj + g_cam.worldOff, &world, 64);
            std::memcpy(g_cam.obj + g_cam.viewOff, &view, 64);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// The game swaps the camera object when the player changes state (on foot, in a
// car, a cutscene). Our pose lives in g_state, so moving to the new object keeps the
// flight going instead of silently stopping.
void FollowEngineCamera() {
    uint8_t *active = EngineActiveCamera();
    if (!active || active == g_cam.obj) return;
    Camera next;
    if (!AdoptCamera(active, "the engine's new active camera", &next, false)) return;
    g_cam = next;
    TW_Log("FreeCam: the game switched cameras; carrying on from %.1f %.1f %.1f",
           g_state.pos[0], g_state.pos[1], g_state.pos[2]);
}

bool CameraStillThere() {
    if (!g_cam.obj) return false;
    return CameraVtableIndex(g_cam.obj) >= 0;
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
    std::memcpy(&world, g_cam.obj + g_cam.worldOff, 64);
    if (!fc_mat_valid(&world)) {
        TW_Log("FreeCam: the matrix at +0x%x is not a transform; searching again", g_cam.worldOff);
        return false;
    }

    fc_config_defaults(&g_state.cfg);
    g_state.cfg.view = 0;
    fc_init(&g_state, &g_state.cfg);
    if (!fc_set_active(&g_state, 1, &world)) return false;

    g_looking = false;
    g_live = 1;
    TW_Log("FreeCam: on at %.1f %.1f %.1f (yaw %.0f, pitch %.0f, view row sign %d, %s)",
           g_state.pos[0], g_state.pos[1], g_state.pos[2], g_state.yaw * 57.2957795f,
           g_state.pitch * 57.2957795f, g_cam.viewSign, g_cam.how);
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
    int lost = 0, held = 0, taken = 0, refused = 0, sinceSwitch = 0;
    ULONGLONG lastReport = GetTickCount64();

    timeBeginPeriod(1);
    TW_Log("FreeCam: camera thread up (build 3: engine camera + accessor detours)");

    for (;;) {
        if (g_quit) break;

        if (!g_want) {
            if (g_live) StopFlying();
            Sleep(5);
            prev.QuadPart = 0;
            continue;
        }

        if (!g_cam.obj) {
            Camera found;
            if (ResolveByEngine(&found)) {
                g_cam = found;
                EnsureDetours();
                if (!StartFlying()) g_cam.obj = nullptr;
                else prev.QuadPart = 0;
                continue;
            }
            if (SearchStep()) {
                EnsureDetours();
                if (!StartFlying()) g_cam.obj = nullptr;
                else prev.QuadPart = 0;
            }
            Sleep(2);
            continue;
        }

        if (!CameraStillThere()) {
            if (++lost < 30) { Sleep(1); continue; }
            TW_Log("FreeCam: the camera object is gone from %p, resolving again", g_cam.obj);
            g_cam.obj = nullptr;
            g_cam.live = false;
            lost = 0;
            continue;
        }
        lost = 0;

        if (g_live && ++sinceSwitch >= 500) {
            sinceSwitch = 0;
            FollowEngineCamera();
        }

        if (!g_live) {
            if (!StartFlying()) { g_cam.obj = nullptr; continue; }
            prev.QuadPart = 0;
        }

        QueryPerformanceCounter(&now);
        float dt = prev.QuadPart ? static_cast<float>(MsSince(prev, now, freq) / 1000.0) : 0.016f;
        prev = now;
        if (dt <= 0.0f) dt = 0.001f;
        if (dt > 0.1f) dt = 0.1f;

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
                fc_mat after;
                if (Readable(g_cam.obj + g_cam.viewOff, 64)) {
                    std::memcpy(&after, g_cam.obj + g_cam.viewOff, 64);
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
                TW_Log("FreeCam: camera at %.1f %.1f %.1f, view accessor %s, writes held %d, taken back %d",
                       g_state.pos[0], g_state.pos[1], g_state.pos[2],
                       g_viewAcc.installed ? "hooked" : "not hooked", held, taken);
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
        TW_Log("FreeCam: switched on, resolving the camera the engine draws");
    }
    return "Free camera: on - finding the camera (see TW_MenuLog.txt)";
}

void FreeCamTick() {
    // The camera runs on its own thread, so there is nothing to do per frame.
}

}  // namespace game
}  // namespace tw

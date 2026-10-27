// freecam_game.cpp - free camera for the Triad Wars debug menu.
//
// BUILD 4: ASK THE RENDERER WHICH CAMERA IT IS DRAWING
// ---------------------------------------------------
// Build 3 tried to find the camera through the manager at 0x23e0150:
//
//     0x64220  call 0xcf9d0          rax = [0x23e0150]
//     0x64225  mov  rdi, [rax+0x50]  rdi = "the camera"
//     0x64318  call 0xcfc30          the view accessor, called on rdi
//
// On the retail game that pointer does not lead to a camera component: the log from
// the shipped build says
//
//     FreeCam: the engine's camera at 00007FF436CD96A0 did not look like a camera
//     FreeCam: its first matrix [ 0.000 0.000 0.000 0.000 | ... ]
//
// so the walk was writing into something that is not a camera at all.
//
// The renderer, however, tells us the answer every frame: it calls the view accessor
// with the component in rcx. So this build stops guessing and hooks the accessor
// first, then simply records which object is passed to it - and how often. The object
// the renderer asks about most often, that stands near the player and whose matrix
// really is a view matrix, is the camera. Its offset inside the object comes from the
// pointer the accessor returned (view = ptr - object), so nothing about the layout is
// assumed either.
//
// The accessors are plain functions called directly - 17 call sites for the view one,
// none through a vtable:
//
//     0xcfc30  view         80 b9 00 01 00 00 00 48 8d 81 10 01 00 00 75 04 48 8d 41 40 f3 c3
//     0xcfc10  world        80 b9 00 01 00 00 00 48 8d 81 50 01 00 00 75 07 48 8d 81 80 00 00 00 f3 c3
//
// Each is byte-verified before it is patched; if the bytes are not where the table says
// they are, the whole .text section is searched for them instead, and if that finds no
// single match the hook is skipped rather than forced.
//
// How the flight works, once the camera is known:
//
//   * while free cam is on, a call to the view accessor for *that* object returns a
//     matrix built from our pose at that instant - an answer cannot be overwritten by
//     the frame that follows, which a stored matrix can;
//   * the matrices are written into the object as well, for code that reads memory;
//   * every other object, and every call while free cam is off, runs the original
//     function from a copied trampoline;
//   * if the game switches cameras (car, cutscene) the proxy table notices a new object
//     being asked about and the flight carries on with it.
//
// Everything is guarded: VirtualQuery before every read, SEH around the scan and the
// writes, and a hand-verified byte check before any code is patched.

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

// Fallback only: the manager build 3 used. Kept because it costs nothing and may be
// right on another build of the game.
constexpr uintptr_t kCameraManager    = 0x23e0150;
constexpr int       kManagerActive    = 0x50;

// UFG::*CameraComponent vtables, relative to the image base. Used for the last-resort
// heap scan and for naming a camera in the log; the primary path does not need them.
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

// --- the accessors ------------------------------------------------------------

struct Accessor {
    uintptr_t rva;
    const char *name;
    std::string bytes;               // the original function, byte for byte
    void *trampoline = nullptr;      // our copy of it, callable
    uintptr_t at = 0;                // where it was found (0 = not found)
    bool installed = false;
    void *(*fn)(void *) = nullptr;   // our detour
};

Accessor g_viewAcc = { 0xcfc30, "view",
    std::string("\x80\xb9\x00\x01\x00\x00\x00\x48\x8d\x81\x10\x01\x00\x00\x75\x04"
                "\x48\x8d\x41\x40\xf3\xc3", 22) };
Accessor g_worldAcc = { 0xcfc10, "world",
    std::string("\x80\xb9\x00\x01\x00\x00\x00\x48\x8d\x81\x50\x01\x00\x00\x75\x07"
                "\x48\x8d\x81\x80\x00\x00\x00\xf3\xc3", 25) };

constexpr float  kNearRadius   = 200.0f;    // a camera further than this from the player is not the game camera
constexpr size_t kObjectBytes  = 0x200;     // enough for all three matrices
constexpr int    kLookKey      = VK_RBUTTON;
constexpr float  kScanBudgetMs = 2.0f;
constexpr size_t kScanChunk    = 64 * 1024;
constexpr int    kProbeSlots   = 8;
constexpr LONG   kProbeEnough  = 5;         // calls in half a second: this is the drawn camera

struct Camera {
    uint8_t *obj = nullptr;      // the component the accessor is called on
    int viewOff = 0x40;          // where the view matrix sits inside it
    int worldOff = 0x80;
    int projOff = 0xc0;
    int viewSign = 1;            // row 2 is -forward (1) or +forward (2)
    int vtableIndex = -1;
    uintptr_t vtable = 0;        // as it was when we took it: a different one means reused memory
    const char *how = "none";
    fc_mat savedView {};
    fc_mat savedWorld {};
    float startPos[3] = { 0.0f, 0.0f, 0.0f };
    float startYaw = 0.0f, startPitch = 0.0f;
};

// One entry per object the renderer has asked about. Written on the render thread by
// the detour, read on the camera thread. Plain pointers and an interlocked counter:
// no allocation, no locks, nothing that can block a frame.
struct Probe {
    void *obj;
    void *viewPtr;          // what the accessor returned for it: view = viewPtr - obj
    volatile LONG calls;
};

struct ScanCtx {
    const float *player;
    uintptr_t lo, hi;
    uintptr_t vt[kCamVtableCount];
    Camera *out;
};

Camera   g_cam;
void    *g_banned = nullptr;   // a camera the renderer never asked about after all
fc_state g_state;
HANDLE   g_thread = nullptr;
volatile LONG g_quit = 0;
volatile LONG g_want = 0;
volatile LONG g_live = 0;
volatile LONG g_accessorCalls = 0;   // every call the game makes to the view accessor
volatile LONG g_accessorAnswers = 0; // of those, the ones answered with our pose
POINT    g_lookOrigin = {};
bool     g_looking = false;
float    g_speed = 30.0f;

// fresh answers for the accessor detours
fc_mat g_viewOut;
fc_mat g_worldOut;

Probe     g_probes[kProbeSlots] = {};
uintptr_t g_scanAddr = 0x10000;
int       g_scanPasses = 0;
int       g_scanRegions = 0;
bool      g_searchLogged = false;
bool      g_noPlayerLogged = false;
bool      g_engineLogged = false;
bool      g_detourTried = false;
bool      g_detourOk = false;
bool      g_writeRefusedLogged = false;

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

// --- the probe table: which object does the renderer ask about? ---------------

void RecordProbe(void *self, void *viewPtr) {
    Probe *free_slot = nullptr;
    for (int i = 0; i < kProbeSlots; i++) {
        if (g_probes[i].obj == self) {
            g_probes[i].viewPtr = viewPtr;
            InterlockedIncrement(&g_probes[i].calls);
            return;
        }
        if (!g_probes[i].obj && !free_slot) free_slot = &g_probes[i];
    }
    if (free_slot) {
        free_slot->viewPtr = viewPtr;
        InterlockedExchange(&free_slot->calls, 1);
        free_slot->obj = self;   // published last: the reader only trusts a slot with an obj
    }
}

// Keep the entry for a camera we are flying alive. The live branch of the detour
// answers before the original runs, so without this the camera we are using would be
// forgotten by the ageing pass - and then a switch could not be told from a miss.
void TouchProbe(void *self, void *viewPtr) {
    for (int i = 0; i < kProbeSlots; i++) {
        if (g_probes[i].obj == self) {
            InterlockedIncrement(&g_probes[i].calls);
            return;
        }
    }
    RecordProbe(self, viewPtr);
}

// Halve every count twice a second and forget the ones that stopped being asked for,
// so a camera the game stopped drawing does not stay in the table for ever.
void AgeProbes() {
    for (int i = 0; i < kProbeSlots; i++) {
        if (!g_probes[i].obj) continue;
        LONG now = InterlockedExchange(&g_probes[i].calls, g_probes[i].calls / 2);
        if (now <= 0) {
            g_probes[i].obj = nullptr;
            g_probes[i].viewPtr = nullptr;
        }
    }
}

void ClearProbes() {
    for (int i = 0; i < kProbeSlots; i++) {
        g_probes[i].obj = nullptr;
        g_probes[i].viewPtr = nullptr;
        InterlockedExchange(&g_probes[i].calls, 0);
    }
}

// --- adopting a camera -------------------------------------------------------

// Log a rejected candidate once per object, so a wrong guess does not fill the log at
// 60 frames a second.
void *g_lastRejectObj = nullptr;
void RejectOnce(void *obj, const char *why) {
    if (g_lastRejectObj == obj) return;
    g_lastRejectObj = obj;
    TW_Log("FreeCam: %p %s", obj, why);
}

// Validate one candidate and fill in everything the flight needs. The view offset is
// not guessed: it is where the accessor's answer pointed.
bool TakeCandidate(void *objPtr, void *viewPtr, LONG calls, const float player[3],
                   bool havePlayer, Camera *out, const char *how, bool enforceNear) {
    auto *obj = static_cast<uint8_t *>(objPtr);
    if (!obj || !viewPtr) return false;
    if (!Readable(obj, kObjectBytes) || !Readable(viewPtr, 64)) return false;

    ptrdiff_t off = static_cast<uint8_t *>(viewPtr) - obj;
    if (off < 0x10 || off > 0x300 || (off & 3)) {
        RejectOnce(obj, "answered the view accessor with a pointer that is not a matrix inside it");
        return false;
    }

    fc_mat view;
    std::memcpy(&view, viewPtr, 64);

    float pos[3], yaw = 0.0f, pitch = 0.0f;
    int sign = 0;
    if (!fc_pose_from_view(&view, pos, &yaw, &pitch, &sign)) {
        RejectOnce(obj, "has a matrix where its view should be, but it is not a camera's");
        return false;
    }

    float dist = havePlayer ? Distance(pos, player) : -1.0f;
    int vt = CameraVtableIndex(obj);

    if (havePlayer && dist > kNearRadius) {
        if (enforceNear) {
            TW_Log("FreeCam: ignoring %p (%s): %.0f units from the player, %d calls",
                   obj, vt >= 0 ? kCamNames[vt] : "vtable not in the table", dist, calls);
            return false;
        }
        TW_Log("FreeCam: taking a camera %.0f units from the player - further than expected, but the "
               "renderer asks about it %d times", dist, calls);
    }

    out->obj = obj;
    out->viewOff = static_cast<int>(off);
    out->worldOff = out->viewOff + 0x40;
    out->projOff = out->viewOff + 0x80;
    out->viewSign = sign;
    out->vtableIndex = vt;
    out->vtable = *reinterpret_cast<uintptr_t *>(obj);
    out->how = how;
    out->startPos[0] = pos[0];
    out->startPos[1] = pos[1];
    out->startPos[2] = pos[2];
    out->startYaw = yaw;
    out->startPitch = pitch;
    std::memcpy(&out->savedView, &view, 64);
    if (Readable(obj + out->worldOff, 64)) std::memcpy(&out->savedWorld, obj + out->worldOff, 64);
    else out->savedWorld = view;

    TW_Log("FreeCam: using %s at %p (%s), view +0x%x, world +0x%x, view row sign %d, "
           "%d calls, %.1f units from the player",
           how, obj, vt >= 0 ? kCamNames[vt] : "vtable not in the table", out->viewOff,
           out->worldOff, sign, calls, dist);
    LogMatrix("FreeCam: its view ", view);
    return true;
}

// One line per object the renderer has been asking about - what the choice was made
// from, so a wrong choice can be seen in the log instead of guessed at.
void LogProbeTable() {
    float player[3] = { 0.0f, 0.0f, 0.0f };
    const bool havePlayer = PlayerPosition(player);
    for (int i = 0; i < kProbeSlots; i++) {
        Probe &p = g_probes[i];
        if (!p.obj) continue;
        int vt = CameraVtableIndex(static_cast<uint8_t *>(p.obj));
        float dist = -1.0f;
        if (havePlayer && p.viewPtr && Readable(p.viewPtr, 64)) {
            fc_mat view;
            std::memcpy(&view, p.viewPtr, 64);
            float pos[3], yaw = 0.0f, pitch = 0.0f;
            int sign = 0;
            if (fc_pose_from_view(&view, pos, &yaw, &pitch, &sign)) dist = Distance(pos, player);
        }
        TW_Log("FreeCam: candidate %p (%s), %d calls, %.1f units from the player",
               p.obj, vt >= 0 ? kCamNames[vt] : "vtable not in the table", p.calls, dist);
    }
}

// The camera the renderer draws: the object it asks the view accessor about most.
bool AdoptFromProbes(Camera *out) {
    float player[3] = { 0.0f, 0.0f, 0.0f };
    const bool havePlayer = PlayerPosition(player);

    int order[kProbeSlots];
    int count = 0;
    for (int i = 0; i < kProbeSlots; i++) {
        if (g_probes[i].obj) order[count++] = i;
    }
    if (!count) return false;

    // highest call count first
    for (int i = 1; i < count; i++) {
        int slot = order[i];
        int j = i - 1;
        while (j >= 0 && g_probes[order[j]].calls < g_probes[slot].calls) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = slot;
    }

    // First the cameras that stand next to the player, then - because the player
    // transform may simply be at an offset this build does not have - the one the
    // renderer asks about most, whatever the distance says.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            Probe &p = g_probes[order[i]];
            LONG calls = p.calls;
            if (calls <= 0) continue;
            if (p.obj == g_banned) continue;
            if (TakeCandidate(p.obj, p.viewPtr, calls, player, havePlayer, out,
                              "the camera the renderer asks for", pass == 0)) {
                LogProbeTable();
                return true;
            }
        }
    }
    return false;
}

// The manager path from build 3, kept as a fallback.
bool ResolveByEngine(Camera *out) {
    auto *base = reinterpret_cast<uint8_t *>(Base());
    if (!base) return false;
    if (!Readable(base + kCameraManager, sizeof(void *))) return false;
    void *manager = *reinterpret_cast<void **>(base + kCameraManager);
    if (!Readable(manager, kManagerActive + 8)) return false;
    void *active = *reinterpret_cast<void **>(static_cast<uint8_t *>(manager) + kManagerActive);
    if (!active) return false;

    if (TakeCandidate(active, static_cast<uint8_t *>(active) + 0x40, 1,
                      nullptr, false, out, "the camera in the manager", false)) {
        return true;
    }

    if (!g_engineLogged) {
        g_engineLogged = true;
        TW_Log("FreeCam: the manager's camera at %p did not look like one either", active);
    }
    return false;
}

// --- last resort: scan the heap ----------------------------------------------

bool AdoptScanned(uint8_t *obj, const float player[3], Camera *out) {
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
    if (!fc_view_sign(&world, &view)) return false;

    float pos[3], yaw = 0.0f, pitch = 0.0f;
    if (!fc_mat_to_pose(&world, pos, &yaw, &pitch, 0)) return false;
    if (Distance(pos, player) > kNearRadius) return false;

    int sign = fc_view_sign(&world, &view);
    out->obj = obj;
    out->viewOff = block;
    out->worldOff = block + 0x40;
    out->projOff = block + 0x80;
    out->viewSign = sign;
    out->vtableIndex = vt;
    out->vtable = *reinterpret_cast<uintptr_t *>(obj);
    out->how = "a camera found by scanning the heap";
    out->startPos[0] = pos[0];
    out->startPos[1] = pos[1];
    out->startPos[2] = pos[2];
    out->startYaw = yaw;
    out->startPitch = pitch;
    out->savedView = view;
    out->savedWorld = world;

    TW_Log("FreeCam: using a heap-scanned camera at %p (%s), view +0x%x, view row sign %d",
           obj, kCamNames[vt], block, sign);
    return true;
}

bool ScanChunkInner(const ScanCtx &ctx, uintptr_t from, uintptr_t to, bool *found) {
    const uintptr_t span = ctx.hi - ctx.lo;
    uintptr_t p = (from + 7) & ~static_cast<uintptr_t>(7);
    for (; p + 8 <= to; p += 8) {
        uintptr_t v = *reinterpret_cast<uintptr_t *>(p);
        if (v - ctx.lo > span) continue;
        for (size_t k = 0; k < kCamVtableCount; k++) {
            if (v != ctx.vt[k]) continue;
            if (AdoptScanned(reinterpret_cast<uint8_t *>(p), ctx.player, ctx.out)) {
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

// While free cam is on, anyone asking *this* camera for its view gets our pose,
// rebuilt at the moment of the question. Nothing else is affected: other cameras, and
// every call while free cam is off, go to the untouched original.
extern "C" void *FreeCamViewAccessor(void *self) {
    InterlockedIncrement(&g_accessorCalls);
    if (g_live && self && self == g_cam.obj) {
        fc_mat view;
        fc_mat_from_pose(&view, g_state.pos, g_state.yaw, g_state.pitch, g_cam.viewSign);
        g_viewOut = view;
        InterlockedIncrement(&g_accessorAnswers);
        TouchProbe(self, g_cam.obj + g_cam.viewOff);
        return &g_viewOut;
    }
    void *answer = CallOriginal(g_viewAcc, self);
    // The renderer asking about a camera is the one reliable statement about which
    // camera it draws - remember it, and how often.
    if (answer && self) RecordProbe(self, answer);
    return answer;
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

// Where the accessor actually lives: the recorded RVA if the bytes there match, else
// the single place in .text that holds them. Anything else is "not found" - and then
// nothing is patched.
uintptr_t FindAccessor(const Accessor &acc) {
    auto *base = reinterpret_cast<uint8_t *>(Base());
    if (!base) return 0;

    const size_t len = acc.bytes.size();
    auto *expected = base + acc.rva;
    if (Readable(expected, len) && std::memcmp(expected, acc.bytes.data(), len) == 0)
        return reinterpret_cast<uintptr_t>(expected);

    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    uint8_t *text = nullptr;
    size_t size = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            text = base + sec[i].VirtualAddress;
            size = sec[i].Misc.VirtualSize;
            break;
        }
    }
    if (!text || !size) return 0;

    const uint8_t *needle = reinterpret_cast<const uint8_t *>(acc.bytes.data());
    uint8_t *found = nullptr;
    for (size_t i = 0; i + len <= size; i++) {
        if (text[i] != needle[0]) continue;
        if (std::memcmp(text + i, needle, len) != 0) continue;
        if (found) return 0;          // more than one: not safe to pick
        found = text + i;
    }
    if (found) {
        TW_Log("FreeCam: the %s accessor was not at %p; found its bytes at %p",
               acc.name, static_cast<void *>(expected), static_cast<void *>(found));
    }
    return reinterpret_cast<uintptr_t>(found);
}

bool InstallDetour(Accessor &acc, void *detour) {
    if (acc.installed) return true;
    uintptr_t at = FindAccessor(acc);
    if (!at) {
        TW_Log("FreeCam: the %s accessor does not match this game build; leaving it alone "
               "(free cam will still write the matrices, but the engine may redraw over it)",
               acc.name);
        return false;
    }

    auto *target = reinterpret_cast<uint8_t *>(at);
    const size_t len = acc.bytes.size();

    // Our own copy of the original, so the detour always has a working fallback.
    void *copy = VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!copy) return false;
    std::memcpy(copy, target, len);
    acc.trampoline = copy;
    acc.at = at;

    DWORD old = 0;
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old)) {
        TW_Log("FreeCam: could not unlock the %s accessor", acc.name);
        return false;
    }
    // jmp rel32 to our function, then int3 over the bytes we replaced
    const intptr_t rel = reinterpret_cast<intptr_t>(detour) - (reinterpret_cast<intptr_t>(target) + 5);
    target[0] = 0xE9;
    std::memcpy(target + 1, &rel, 4);
    std::memset(target + 5, 0xCC, len - 5);
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);

    acc.installed = true;
    acc.fn = reinterpret_cast<void *(*)(void *)>(detour);
    TW_Log("FreeCam: hooked the %s accessor at %p (original kept at %p)", acc.name,
           static_cast<void *>(target), copy);
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

void RestoreMatrices() {
    if (!g_cam.obj) return;
    WriteMatrices(g_cam.savedWorld, g_cam.savedView);
}

// The game swaps cameras when the player gets into a car or a cutscene starts. The
// proxy table shows it at once: a different object starts being asked about often.
bool FollowCameraSwitch() {
    if (!g_live) return false;
    float player[3] = { 0.0f, 0.0f, 0.0f };
    const bool havePlayer = PlayerPosition(player);

    for (int i = 0; i < kProbeSlots; i++) {
        Probe &p = g_probes[i];
        if (!p.obj || p.obj == g_cam.obj) continue;
        if (p.calls < kProbeEnough) continue;
        Camera next;
        if (!TakeCandidate(p.obj, p.viewPtr, p.calls, player, havePlayer, &next,
                           "the camera the renderer switched to", false))
            continue;
        g_cam = next;      // the pose in g_state carries straight over
        TW_Log("FreeCam: the game switched cameras; carrying on from %.1f %.1f %.1f",
               g_state.pos[0], g_state.pos[1], g_state.pos[2]);
        return true;
    }
    return false;
}

bool CameraStillThere() {
    if (!g_cam.obj) return false;
    if (!Readable(g_cam.obj, 8)) return false;
    if (!Readable(g_cam.obj + g_cam.viewOff, 64)) return false;
    // The object starts with its vtable. If that is no longer what it was, the memory
    // has been handed to something else and writing into it would corrupt that.
    if (g_cam.vtable && *reinterpret_cast<uintptr_t *>(g_cam.obj) != g_cam.vtable) return false;
    return true;
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

// Start flying from wherever the camera is right now: read its view matrix, pull the
// pose out of it, and hand that to the state machine.
bool StartFlying() {
    if (!g_cam.obj) return false;

    fc_mat view = g_cam.savedView;
    if (Readable(g_cam.obj + g_cam.viewOff, 64))
        std::memcpy(&view, g_cam.obj + g_cam.viewOff, 64);

    float pos[3] = { g_cam.startPos[0], g_cam.startPos[1], g_cam.startPos[2] };
    float yaw = g_cam.startYaw, pitch = g_cam.startPitch;
    int sign = g_cam.viewSign;
    if (fc_mat_valid(&view) && fc_pose_from_view(&view, pos, &yaw, &pitch, &sign)) {
        g_cam.viewSign = sign;
        g_cam.startPos[0] = pos[0];
        g_cam.startPos[1] = pos[1];
        g_cam.startPos[2] = pos[2];
        g_cam.startYaw = yaw;
        g_cam.startPitch = pitch;
        g_cam.savedView = view;
        if (Readable(g_cam.obj + g_cam.worldOff, 64))
            std::memcpy(&g_cam.savedWorld, g_cam.obj + g_cam.worldOff, 64);
    }

    // fc_init() memsets the whole state, cfg included, before copying cfg in - so the
    // config has to be a separate object or it is zeroed on the way in. (Builds 2 and 3
    // passed &g_state.cfg here, which left speed 0 and sensitivity 0: the camera was on
    // and could never move.)
    fc_config cfg;
    fc_config_defaults(&cfg);
    cfg.view = 0;
    cfg.speed = g_speed;
    fc_init(&g_state, &cfg);

    fc_mat world;
    fc_mat_from_pose(&world, pos, yaw, pitch, 0);
    if (!fc_set_active(&g_state, 1, &world)) return false;

    g_looking = false;
    g_live = 1;
    TW_Log("FreeCam: on at %.1f %.1f %.1f (yaw %.0f, pitch %.0f, view row sign %d, %s)",
           g_state.pos[0], g_state.pos[1], g_state.pos[2], g_state.yaw * 57.2957795f,
           g_state.pitch * 57.2957795f, g_cam.viewSign, g_cam.how);
    return true;
}

void StopFlying() {
    fc_set_active(&g_state, 0, nullptr);
    g_live = 0;
    g_looking = false;
    RestoreMatrices();   // the matrices as they were when the flight started
    TW_Log("FreeCam: off after %.0f units", g_state.moved);
}

DWORD WINAPI Loop(LPVOID) {
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    int lost = 0, held = 0, taken = 0, refused = 0, sinceAge = 0, waited = 0, looks = 0;
    ULONGLONG lastReport = GetTickCount64();

    timeBeginPeriod(1);
    TW_Log("FreeCam: camera thread up (build 4: the renderer is asked which camera it draws)");

    for (;;) {
        if (g_quit) break;

        if (!g_want) {
            if (g_live) StopFlying();
            if (g_cam.obj) {
                g_cam.obj = nullptr;
                ClearProbes();     // whatever was learned belongs to the last flight
            }
            Sleep(5);
            prev.QuadPart = 0;
            continue;
        }

        if (!g_detourTried) {
            g_detourTried = true;
            g_detourOk = EnsureDetours();
            if (!g_detourOk) {
                TW_Log("FreeCam: no accessor hook on this build; falling back to writing matrices only");
            }
        }

        if (++sinceAge >= 500) {
            sinceAge = 0;
            AgeProbes();
            FollowCameraSwitch();
        }

        if (!g_cam.obj) {
            Camera found;
            if (AdoptFromProbes(&found)) {
                g_cam = found;
                if (StartFlying()) { prev.QuadPart = 0; continue; }
                g_cam.obj = nullptr;
            } else if (++waited >= 1000) {
                // A second of probing found nothing: from now on also try the older
                // routes once a second, but the probes still get first refusal above.
                waited = 0;
                if (++looks >= 3) {
                    if (ResolveByEngine(&found)) {
                        g_cam = found;
                        if (StartFlying()) { prev.QuadPart = 0; continue; }
                        g_cam.obj = nullptr;
                    } else if (SearchStep()) {
                        if (StartFlying()) { prev.QuadPart = 0; continue; }
                        g_cam.obj = nullptr;
                    }
                }
            }
            Sleep(1);
            continue;
        }

        if (!CameraStillThere()) {
            if (++lost < 30) { Sleep(1); continue; }
            TW_Log("FreeCam: the camera object is gone from %p, resolving again", g_cam.obj);
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
                if (!g_writeRefusedLogged) {
                    g_writeRefusedLogged = true;
                    TW_Log("FreeCam: the camera memory will not take a write; the accessor answer is "
                           "still ours, so the picture should still move");
                }
                refused = 0;
            }

            ULONGLONG nowTick = GetTickCount64();
            if (nowTick - lastReport > 2000) {
                lastReport = nowTick;
                LONG calls = InterlockedExchange(&g_accessorCalls, 0);
                LONG answers = InterlockedExchange(&g_accessorAnswers, 0);
                if (answers == 0 && calls > 0) {
                    // The renderer is asking about cameras, just not this one. Whatever
                    // this object is, it is not what is being drawn: pick again.
                    TW_Log("FreeCam: the renderer never asked about %p in two seconds; picking another "
                           "camera", g_cam.obj);
                    g_banned = g_cam.obj;
                    g_cam.obj = nullptr;
                    held = taken = 0;
                    Sleep(1);
                    continue;
                }
                TW_Log("FreeCam: camera at %.1f %.1f %.1f, view accessor %s, answered %d of %d calls, "
                       "writes held %d, taken back %d",
                       g_state.pos[0], g_state.pos[1], g_state.pos[2],
                       g_viewAcc.installed ? "hooked" : "not hooked", answers, calls, held, taken);
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
    g_speed = unitsPerSecond;
    g_state.cfg.speed = unitsPerSecond;
}

float FreeCamSpeed() { return g_speed; }

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
        TW_Log("FreeCam: switched on, asking the renderer which camera it draws");
    }
    return "Free camera: on - finding the camera (see TW_MenuLog.txt)";
}

void FreeCamTick() {
    // The camera runs on its own thread, so there is nothing to do per frame.
}

}  // namespace game
}  // namespace tw

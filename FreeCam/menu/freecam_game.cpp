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
#include <tlhelp32.h>   // listing the other threads while code is patched
#include "input_block.h"
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
    int loOffset;                    // what it returns when the byte at +0x100 is 0
    int hiOffset;                    // and when it is not
    void *trampoline = nullptr;      // our copy of it, callable
    uintptr_t at = 0;                // where it was found (0 = not found)
    bool installed = false;
    void *(*fn)(void *) = nullptr;   // our detour
};

//         cmp byte ptr [rcx+0x100], 0 / lea rax,[rcx+0x110] / jne +4 / lea rax,[rcx+0x40] / ret
Accessor g_viewAcc = { 0xcfc30, "view",
    std::string("\x80\xb9\x00\x01\x00\x00\x00\x48\x8d\x81\x10\x01\x00\x00\x75\x04"
                "\x48\x8d\x41\x40\xf3\xc3", 22),
    0x40, 0x110 };

constexpr float  kNearRadius   = 200.0f;    // a camera further than this from the player is not the game camera
constexpr size_t kObjectBytes  = 0x200;     // enough for all three matrices
constexpr int    kLookKey      = VK_RBUTTON;
// The player transform sits at the character's feet, so a camera aiming at it points at
// the pavement. This much above it is the chest, which is what should be in the picture.
constexpr float  kAimHeight    = 1.0f;
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
    int transposed = 0;          // the engine keeps matrices column-major
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
float    g_flightClock = 0.0f;     // seconds of flight, the keyframe clock

// the path is kept across switching free cam off and on again
fc_keyframe g_savedPath[FC_MAX_KEYS];
int         g_savedPathCount = 0;
float       g_playSpeed = 1.0f;         // playback speed, kept across on/off like the path

// the matrices as they were the last time we wrote them, so a later read can tell our
// pose from one the game has just put there itself
fc_mat g_wroteWorld;
fc_mat g_wroteView;
int    g_haveWrote = 0;
int    g_panSamples = 0;   // game camera poses seen while following (the turns copied)


// the fresh answer the view detour hands out
fc_mat g_viewOut;

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
bool      g_hookQuietLogged = false;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// The end of the game's own image, so a pointer can be told apart from a heap one.
uintptr_t ImageEnd() {
    auto *base = reinterpret_cast<uint8_t *>(Base());
    if (!base) return 0;
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return Base() + nt->OptionalHeader.SizeOfImage;
}

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

void *g_newProbe = nullptr;   // an object seen for the first time, for the camera thread to log

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
        g_newProbe = self;       // the camera thread writes the log line
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
    // Only what is actually read is required to be readable: a component that ends near
    // the end of a heap block must not be thrown away for the bytes after it.
    if (!Readable(obj, 0x40) || !Readable(viewPtr, 64)) return false;

    ptrdiff_t off = static_cast<uint8_t *>(viewPtr) - obj;
    if (off < 0x10 || off > 0x300 || (off & 3)) {
        RejectOnce(obj, "answered the view accessor with a pointer that is not a matrix inside it");
        return false;
    }

    fc_mat view, raw;
    std::memcpy(&raw, viewPtr, 64);
    view = raw;

    float pos[3] = { 0.0f, 0.0f, 0.0f }, yaw = 0.0f, pitch = 0.0f;
    int sign = 0;
    int transposed = 0;
    if (!fc_pose_from_view(&view, pos, &yaw, &pitch, &sign)) {
        // Not a view matrix as stored. A column-major engine keeps the same camera
        // transposed, so try that before giving up on this object.
        fc_mat flipped;
        fc_mat_transpose(&raw, &flipped);
        if (!fc_pose_from_view(&flipped, pos, &yaw, &pitch, &sign)) {
            RejectOnce(obj, "has a matrix where its view should be, but it is not a camera's");
            return false;
        }
        view = flipped;
        transposed = 1;
        TW_Log("FreeCam: %p keeps its matrices transposed (column-major)", obj);
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
    out->transposed = transposed;
    // Only watch the first qword if it really is a code pointer: this is how memory
    // that has been reused is caught, and a counter there would change every frame.
    {
        uintptr_t first = *reinterpret_cast<uintptr_t *>(obj);
        out->vtable = (first >= Base() && first < ImageEnd()) ? first : 0;
    }
    out->how = how;
    out->startPos[0] = pos[0];
    out->startPos[1] = pos[1];
    out->startPos[2] = pos[2];
    out->startYaw = yaw;
    out->startPitch = pitch;
    out->savedView = raw;   // the engine's own bytes, for putting back
    if (Readable(obj + out->worldOff, 64)) std::memcpy(&out->savedWorld, obj + out->worldOff, 64);
    else out->savedWorld = raw;

    TW_Log("FreeCam: using %s at %p (%s), view +0x%x, world +0x%x, view row sign %d, "
           "%d calls, %.1f units from the player",
           how, obj, vt >= 0 ? kCamNames[vt] : "vtable not in the table", out->viewOff,
           out->worldOff, sign, calls, dist);
    LogMatrix("FreeCam: its view ", view);
    return true;
}

// Everything worth knowing about the first object the renderer asks about, written by
// the camera thread (never by the hook itself).
void DumpProbe(void *objPtr) {
    auto *obj = static_cast<uint8_t *>(objPtr);
    TW_Log("FreeCam: the renderer asked about %p for the first time", objPtr);
    if (!Readable(obj, 0x100)) {
        TW_Log("FreeCam:   it is not readable");
        return;
    }
    uintptr_t vt = 0;
    std::memcpy(&vt, obj, sizeof vt);
    int known = CameraVtableIndex(obj);
    TW_Log("FreeCam:   vtable %p (%s), base %p, so +0x%llx", reinterpret_cast<void *>(vt),
           known >= 0 ? kCamNames[known] : "not one of the camera vtables",
           reinterpret_cast<void *>(Base()),
           static_cast<unsigned long long>(vt - Base()));
    if (Readable(obj + 0x40, 64)) {
        fc_mat m;
        std::memcpy(&m, obj + 0x40, 64);
        LogMatrix("FreeCam:   +0x40", m);
        TW_Log("FreeCam:   +0x40 is %s", fc_mat_valid(&m) ? "a rigid transform" : "not a rigid transform");
    }
    if (Readable(obj + 0x80, 64)) {
        fc_mat m;
        std::memcpy(&m, obj + 0x80, 64);
        LogMatrix("FreeCam:   +0x80", m);
    }
    unsigned char flag = 0;
    if (Readable(obj + 0x100, 1)) {
        std::memcpy(&flag, obj + 0x100, 1);
        TW_Log("FreeCam:   byte +0x100 is %d, so the accessor returns +0x%x", flag,
               flag ? g_viewAcc.hiOffset : g_viewAcc.loOffset);
    }
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
    out->transposed = 0;
    {
        uintptr_t first = *reinterpret_cast<uintptr_t *>(obj);
        out->vtable = (first >= Base() && first < ImageEnd()) ? first : 0;
    }
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
        if (g_cam.transposed) fc_mat_transpose(&view, &g_viewOut);
        else g_viewOut = view;
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

// --- patching code, carefully ------------------------------------------------
//
// Two things went wrong in build 4, and both of them hang a running game:
//
//   * the jump was `E9 rel32`. The game sits at 0x7FF7..., our DLL does not, and the
//     distance does not fit in 32 bits - so the "jump" landed somewhere arbitrary. A
//     `FF 25 [rip+0]` + 8-byte absolute address cannot overflow.
//   * the bytes were written while the game was executing the function. A thread that
//     was three bytes into it when the patch landed then ran int3 filler. Every other
//     thread is now suspended for the write, and the write is skipped (and retried) if
//     any of them is actually inside the function.

struct ThreadSet {
    HANDLE handles[512];
    int count;
};

void SuspendOthers(ThreadSet &set) {
    set.count = 0;
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();

    // The ids first, with the snapshot closed before anything is suspended: walking a
    // snapshot while threads are frozen invites a lock that is never released.
    DWORD ids[512];
    int idCount = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                if (idCount >= 512) break;
                ids[idCount++] = te.th32ThreadID;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }

    for (int i = 0; i < idCount && set.count < 512; i++) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                              FALSE, ids[i]);
        if (!h) continue;
        if (SuspendThread(h) == static_cast<DWORD>(-1)) { CloseHandle(h); continue; }
        set.handles[set.count++] = h;
    }
}

void ResumeAll(ThreadSet &set) {
    for (int i = 0; i < set.count; i++) {
        ResumeThread(set.handles[i]);
        CloseHandle(set.handles[i]);
    }
    set.count = 0;
}

bool AnyThreadInside(const ThreadSet &set, uintptr_t lo, uintptr_t hi) {
    for (int i = 0; i < set.count; i++) {
        CONTEXT ctx;
        std::memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(set.handles[i], &ctx)) continue;
        const uintptr_t rip = static_cast<uintptr_t>(ctx.Rip);
        if (rip >= lo && rip < hi) return true;
    }
    return false;
}

// The copied function is only usable at another address if nothing in it is relative.
// These two accessors are pure register arithmetic with one short conditional jump
// inside themselves, so this check is exact for them.
bool CopyIsRelocatable(const std::string &bytes) {
    for (size_t i = 0; i < bytes.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0xE8 || c == 0xE9 || c == 0xEB) return false;   // relative call/jmp
        if (c == 0x0F && i + 1 < bytes.size() &&
            (static_cast<unsigned char>(bytes[i + 1]) & 0xF0) == 0x80) return false;  // jcc rel32
    }
    return true;
}

void WriteAbsoluteJump(uint8_t *target, void *dest) {
    // jmp qword ptr [rip+0] ; then the 64-bit destination
    target[0] = 0xFF;
    target[1] = 0x25;
    const uint32_t zero = 0;
    std::memcpy(target + 2, &zero, 4);
    std::memcpy(target + 6, &dest, 8);
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
    constexpr size_t kJumpBytes = 14;   // ff 25 00 00 00 00 + 8-byte address

    if (len < kJumpBytes) {
        TW_Log("FreeCam: the %s accessor is too short to redirect", acc.name);
        return false;
    }
    if (!CopyIsRelocatable(acc.bytes)) {
        TW_Log("FreeCam: the %s accessor is not position independent; not hooking it", acc.name);
        return false;
    }

    // Our own copy of the original, so the detour always has a working fallback.
    void *copy = VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!copy) {
        TW_Log("FreeCam: could not allocate a trampoline for the %s accessor", acc.name);
        return false;
    }
    std::memcpy(copy, target, len);
    FlushInstructionCache(GetCurrentProcess(), copy, len);
    acc.trampoline = copy;
    acc.at = at;

    const intptr_t distance = reinterpret_cast<intptr_t>(detour) - reinterpret_cast<intptr_t>(target);
    TW_Log("FreeCam: the %s accessor is %lld bytes from this DLL, so the %s jump is used",
           acc.name, static_cast<long long>(distance),
           (distance > -2000000000ll && distance < 2000000000ll) ? "near" : "absolute");

    bool patched = false;
    for (int attempt = 0; attempt < 100 && !patched; attempt++) {
        ThreadSet others;
        SuspendOthers(others);
        if (AnyThreadInside(others, at, at + len)) {
            ResumeAll(others);
            Sleep(1);
            continue;
        }

        DWORD old = 0;
        if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old)) {
            ResumeAll(others);
            TW_Log("FreeCam: could not unlock the %s accessor", acc.name);
            return false;
        }
        WriteAbsoluteJump(target, detour);
        std::memset(target + kJumpBytes, 0xCC, len - kJumpBytes);

        // Read the patch back while everything is still stopped: if it is not exactly
        // what was written, put the original back before a thread can run it.
        bool written = std::memcmp(target, "\xFF\x25\x00\x00\x00\x00", 6) == 0;
        void *there = nullptr;
        if (written) std::memcpy(&there, target + 6, 8);
        if (!written || there != detour) {
            std::memcpy(target, acc.bytes.data(), len);
            written = false;
        }

        VirtualProtect(target, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, len);
        ResumeAll(others);

        if (!written) {
            TW_Log("FreeCam: the %s accessor did not take the patch; original put back, not hooking it",
                   acc.name);
            return false;
        }
        patched = true;
    }

    if (!patched) {
        TW_Log("FreeCam: the %s accessor could not be redirected; leaving it alone", acc.name);
        return false;
    }

    acc.installed = true;
    acc.fn = reinterpret_cast<void *(*)(void *)>(detour);
    TW_Log("FreeCam: hooked the %s accessor at %p with an absolute jump (original kept at %p)",
           acc.name, static_cast<void *>(target), copy);
    return true;
}

// Put the original bytes back. This is the way out if the picture or the frame rate
// goes wrong: F10 does it, and so does switching free cam off from the menu.
bool RemoveDetour(Accessor &acc) {
    if (!acc.installed || !acc.at) return true;
    auto *target = reinterpret_cast<uint8_t *>(acc.at);
    const size_t len = acc.bytes.size();

    ThreadSet others;
    SuspendOthers(others);
    DWORD old = 0;
    bool ok = false;
    if (VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old)) {
        std::memcpy(target, acc.bytes.data(), len);
        VirtualProtect(target, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, len);
        ok = true;
    }
    ResumeAll(others);

    acc.installed = false;
    TW_Log(ok ? "FreeCam: the %s accessor is back to the game's own code" :
                "FreeCam: could not put the %s accessor back",
           acc.name);
    return ok;
}

bool EnsureDetours() {
    // Only the view accessor is patched. It is the one the renderer draws with, and one
    // patch is one thing that can go wrong; the world matrix is written in memory.
    return InstallDetour(g_viewAcc, reinterpret_cast<void *>(&FreeCamViewAccessor));
}

// --- following the player's camera -------------------------------------------

// While following, the camera turns with the player's own camera. The game keeps writing
// its camera pose into the component every frame (from the player's look input) and this
// writes over it; a read that is not what we last wrote is the game's own, and the turn
// it has made since the last one is the pan to copy.
void PeekGameCameraAngles() {
    float pos[3], yaw = 0.0f, pitch = 0.0f;
    fc_mat world, flipped;

    if (!fc_following(&g_state) || !g_cam.obj) return;
    if (!Readable(g_cam.obj + g_cam.worldOff, 64)) return;
    std::memcpy(&world, g_cam.obj + g_cam.worldOff, 64);
    if (g_cam.transposed) {
        fc_mat_transpose(&world, &flipped);
        world = flipped;
    }

    if (g_haveWrote) {
        int same = 1;
        for (int i = 0; i < 16; ++i) {
            float d = world.m[i] - g_wroteWorld.m[i];
            if (d < 0.0f) d = -d;
            if (d > 0.01f) { same = 0; break; }
        }
        if (same) return;      // still our own pose; the game has not written since
    }

    if (!fc_mat_to_pose(&world, pos, &yaw, &pitch, 0)) return;
    ++g_panSamples;      // the game rewrote its camera: this is a turn to copy
    fc_follow_update_angles(&g_state, yaw, pitch);
}

// --- writing the matrices ----------------------------------------------------

bool CameraStillThere();   // defined below; the restore has to ask first

bool WriteRaw(const fc_mat &world, const fc_mat &view) {
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

// Our own matrices, written in whatever order the engine keeps them.
bool WriteMatrices(const fc_mat &world, const fc_mat &view) {
    g_wroteWorld = world;
    g_wroteView = view;
    g_haveWrote = 1;
    if (g_cam.transposed) {
        fc_mat w, v;
        fc_mat_transpose(&world, &w);
        fc_mat_transpose(&view, &v);
        return WriteRaw(w, v);
    }
    return WriteRaw(world, view);
}

void RestoreMatrices() {
    if (!g_cam.obj) return;
    if (!CameraStillThere()) return;

    // Only put the old bytes back while the camera still holds ours. If the game has
    // written its own pose since, that one is newer than anything saved when the flight
    // started: writing the old bytes over it is what made switching back and forth
    // jump the picture, and would drag the camera back to where it was minutes ago.
    if (g_haveWrote && Readable(g_cam.obj + g_cam.viewOff, 64)) {
        fc_mat now, expect = g_wroteView;
        std::memcpy(&now, g_cam.obj + g_cam.viewOff, 64);
        if (g_cam.transposed) {
            fc_mat t;
            fc_mat_transpose(&expect, &t);
            expect = t;
        }
        for (int i = 0; i < 16; ++i) {
            float d = now.m[i] - expect.m[i];
            if (d < 0.0f) d = -d;
            if (d > 0.01f) return;      // the game's own pose is there: leave it alone
        }
    }
    WriteRaw(g_cam.savedWorld, g_cam.savedView);
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

// A key that fires once per press, not once per frame it is held down.
//
// Pressed() latches its answer for the rest of the frame: asking twice returns the same
// answer, not "yes" and then "no". It used to consume the edge, and one hotkey asking
// twice - K, once to see whether the camera was frozen and again to play - meant the
// second branch never ran. That is exactly how K stopped playing anything.
struct Hotkey {
    int vk;
    bool was = false;
    bool answered = false;      // already asked this frame
    bool answer = false;
    bool Peek() const { return KeyDown(vk); }
    bool Pressed() {
        if (answered) return answer;
        const bool down = KeyDown(vk);
        answer = down && !was;
        was = down;
        answered = true;
        return answer;
    }
    void NewFrame() { answered = false; answer = false; }
};

Hotkey g_followKey{ 'P' };
Hotkey g_keyKey{ 'L' };
Hotkey g_playKey{ 'J' };   // J - play the captured keyframes back
Hotkey g_freezeKey{ 'O' };
Hotkey g_clearKey{ 'M' };
Hotkey g_slowerKey{ VK_LEFT };      // playback speed down
Hotkey g_fasterKey{ VK_RIGHT };     // playback speed up

// Start a new frame for every hotkey. One call per tick, before anything reads a key.
void BeginHotkeyFrame() {
    g_followKey.NewFrame();
    g_keyKey.NewFrame();
    g_playKey.NewFrame();
    g_freezeKey.NewFrame();
    g_clearKey.NewFrame();
    g_slowerKey.NewFrame();
    g_fasterKey.NewFrame();
}

// While the camera is flying, the player is not in control: the game's own input is
// held back so the character stands still and does not swing or interact. O and P hand
// control back to the player, which is the whole point of those two.
bool PlayerHasControl() {
    return fc_frozen(&g_state) || fc_following(&g_state) || fc_looking_at(&g_state);
}

// --- the corner display ------------------------------------------------------
//
// Every hotkey that does something leaves a line here. The line is fully readable
// for a moment and then fades out over the next second, so the display is empty
// unless something was just pressed: a HUD that never goes away is clutter.

struct HudNote {
    char key[8];
    char text[44];
    DWORD at;       // GetTickCount when it was pressed
};
const int kHudSlots = 8;
HudNote g_hud[kHudSlots];
int g_hudNext = 0;

void Hud(const char *key, const char *text) {
    HudNote &n = g_hud[g_hudNext];
    g_hudNext = (g_hudNext + 1) % (sizeof(g_hud) / sizeof(g_hud[0]));
    std::snprintf(n.key, sizeof(n.key), "%s", key);
    std::snprintf(n.text, sizeof(n.text), "%s", text);
    n.at = GetTickCount();
}

// Public wrapper, defined outside the anonymous namespace below.
int HudCollect(FreeCamHudLine *out, int max) {
    if (!out || max <= 0) return 0;
    const DWORD now = GetTickCount();
    const DWORD hold = 900;     // fully visible for this long
    const DWORD fade = 700;     // then gone within this
    int n = 0;
    // newest first, so a fresh press pushes the older ones down
    for (int age = 0; age < kHudSlots && n < max; ++age) {
        const HudNote &c = g_hud[(g_hudNext - 1 - age + kHudSlots * 2) % kHudSlots];
        if (!c.key[0]) continue;
        const DWORD dt = now - c.at;
        if (dt > hold + fade) continue;
        float a = 1.0f;
        if (dt > hold) a = 1.0f - static_cast<float>(dt - hold) / static_cast<float>(fade);
        if (a <= 0.01f) continue;
        std::snprintf(out[n].key, sizeof(out[n].key), "%s", c.key);
        std::snprintf(out[n].text, sizeof(out[n].text), "%s", c.text);
        out[n].alpha = a;
        ++n;
    }
    return n;
}

void PollInput(float *mouseX, float *mouseY) {
    static const int map[FC_KEY_COUNT] = { 'W', 'S', 'A', 'D', VK_SPACE, 'C', VK_SHIFT, VK_CONTROL, 0 };
    for (int i = 0; i < FC_KEY_COUNT; ++i) g_state.keys[i] = map[i] ? (KeyDown(map[i]) ? 1 : 0) : 0;

    *mouseX = *mouseY = 0.0f;

    // Frozen (O) or following (P): the player is driving, not the camera. The keys belong
    // to the character and the mouse belongs to the game's own look - capturing the
    // pointer here would fight it, and flying by hand would add to the follow.
    if (PlayerHasControl()) {
        for (int i = 0; i < FC_KEY_COUNT; ++i) g_state.keys[i] = 0;
        g_looking = false;
        return;
    }

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

// "0.1x", "1.25x", "2x": the playback speed as it is shown in the corner
void SpeedText(float speed, char *out, size_t n) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(speed));
    size_t len = std::strlen(buf);
    while (len > 3 && buf[len - 1] == '0') buf[--len] = '\0';
    std::snprintf(out, n, "%sx", buf);
}

// Left and right arrows change how fast a recorded path plays back.
void HandlePlaybackSpeed() {
    const bool faster = g_fasterKey.Pressed();
    const bool slower = g_slowerKey.Pressed();
    if (!faster && !slower) return;

    const float speed = fc_play_speed_shift(&g_state, faster ? 1 : 0);
    g_playSpeed = speed;

    char text[44], shown[16];
    SpeedText(speed, shown, sizeof(shown));
    std::snprintf(text, sizeof(text), "playback %s", shown);
    Hud(faster ? "RIGHT" : "LEFT", text);
    TW_Log("FreeCam: playback speed %s (%s arrow)", shown, faster ? "right" : "left");
}

// P - follow the player, L - keyframe, J - play the path back, M - forget the path,
// O - freeze, left/right - how fast the path plays.
void HandleHotkeys(const float player[3], bool havePlayer) {
    // Pressed() latches its answer for the frame, so reading a key more than once in a
    // tick is safe - but each key is still asked once here, for clarity.
    if (g_followKey.Pressed()) {
        if (fc_frozen(&g_state)) {
            // Frozen: the camera stays exactly where it is and only pans, so the player
            // is kept in the middle of the picture as they walk, drive and fight.
            if (fc_looking_at(&g_state)) {
                fc_look_at(&g_state, 0);
                Hud("P", "look at player off");
                TW_Log("FreeCam: not aiming at the player any more; the frozen camera holds still");
            } else if (!havePlayer) {
                TW_Log("FreeCam: cannot aim at the player - its transform is not readable yet");
            } else {
                fc_look_at(&g_state, 1);
                Hud("P", "camera looks at player");
                TW_Log("FreeCam: frozen, so the camera only pans: it stays at %.1f %.1f %.1f and "
                       "turns to keep the player (%.1f %.1f %.1f) in the picture; P again to hold "
                       "still", g_state.pos[0], g_state.pos[1], g_state.pos[2],
                       player[0], player[1], player[2]);
            }
        } else if (fc_playing(&g_state)) {
            TW_Log("FreeCam: following cannot be changed while the path is playing (J to stop)");
        } else if (fc_following(&g_state)) {
            fc_follow(&g_state, player, 0);
            TW_Log("FreeCam: no longer following the player; the camera stays here");
            Hud("P", "follow off");
        } else if (!havePlayer) {
            TW_Log("FreeCam: cannot follow the player - its transform is not readable yet");
        } else {
            fc_follow(&g_state, player, 1);
            Hud("P", "follow on");
            TW_Log("FreeCam: following the player (%.1f %.1f %.1f) - the camera keeps its framing "
                   "and pans with them; P again to stay put", player[0], player[1], player[2]);
        }
    }

    if (g_freezeKey.Pressed()) {
        if (fc_frozen(&g_state)) {
            fc_freeze(&g_state, 0);      // also stops the aiming
            TW_Log("FreeCam: camera unfrozen; the player's controls are held again and the camera "
                   "flies from %.1f %.1f %.1f", g_state.pos[0], g_state.pos[1], g_state.pos[2]);
            Hud("O", "camera flying");
        } else {
            // following would keep dragging the frozen view along with the player
            if (fc_following(&g_state)) {
                fc_follow(&g_state, player, 0);
                TW_Log("FreeCam: no longer following the player (the camera is being frozen)");
            }
            fc_freeze(&g_state, 1);
            TW_Log("FreeCam: camera frozen at %.1f %.1f %.1f - the player can move, fight and "
                   "interact, P makes the camera pan to follow them, O to fly again",
                   g_state.pos[0], g_state.pos[1], g_state.pos[2]);
            Hud("O", "camera frozen");
        }
    }

    HandlePlaybackSpeed();

    if (g_clearKey.Pressed()) {
        // M: forget every keyframe, so the next ones are a new scene rather than an
        // addition to the old one
        const int had = g_state.path_count;
        fc_path_clear(&g_state);
        g_savedPathCount = 0;
        if (had > 0) {
            char text[44];
            std::snprintf(text, sizeof(text), "cleared %d keyframe%s", had, had == 1 ? "" : "s");
            Hud("M", text);
            TW_Log("FreeCam: %d keyframe(s) cleared - the next L starts a new path", had);
        } else {
            Hud("M", "no keyframes to clear");
            TW_Log("FreeCam: M pressed but the path was already empty");
        }
    }

    if (g_keyKey.Pressed()) {
        if (fc_playing(&g_state)) {
            TW_Log("FreeCam: recording is off while the path is playing (J to stop)");
        } else {
            const int n = fc_key_add(&g_state, g_flightClock);
            if (n == 0) {
                TW_Log("FreeCam: the path is full (%d keyframes) - switch free cam off and on to clear it",
                       FC_MAX_KEYS);
            } else {
                TW_Log("FreeCam: keyframe %d recorded (up to %d per path) at %.1f %.1f %.1f "
                       "(yaw %.0f, pitch %.0f)%s",
                       n, FC_MAX_KEYS, g_state.pos[0], g_state.pos[1], g_state.pos[2],
                       g_state.yaw * 57.2957795f, g_state.pitch * 57.2957795f,
                       n == 1 ? " - move the camera, then press L again" : "");
                if (n == 2) TW_Log("FreeCam: press J to play the path back, in the time it was flown");
                {
                    char line[44];
                    std::snprintf(line, sizeof(line), "keyframe %d", n);
                    Hud("L", line);
                }
            }
        }
    }

    if (g_playKey.Pressed()) {
        if (fc_frozen(&g_state)) {
            // A playback is the camera moving, so it cannot happen behind a freeze. Thaw
            // and play rather than refusing: J means "show me the shot".
            fc_freeze(&g_state, 0);
            TW_Log("FreeCam: J on a frozen camera - thawing it first, so the playback can be seen");
        }
        if (fc_playing(&g_state)) {
            fc_play_stop(&g_state);
            TW_Log("FreeCam: playback stopped; the camera stays where the path reached");
            Hud("J", "playback stopped");
        } else if (fc_play_start(&g_state)) {
            TW_Log("FreeCam: playing %d keyframes over %.1f seconds at %.2fx - through every key, "
                   "smooth, no corners (J stops it)",
                   g_state.path_count, g_state.path[g_state.path_count - 1].time,
                   static_cast<double>(fc_play_speed(&g_state)));
            {
                char line[44];
                std::snprintf(line, sizeof(line), "playing %d keyframes", g_state.path_count);
                Hud("J", line);
            }
        } else {
            TW_Log("FreeCam: nothing to play yet - press L to record a keyframe "
                   "(at least two, with the camera in different places)");
        }
    }
}

// Start flying from wherever the camera is right now: read its view matrix, pull the
// pose out of it, and hand that to the state machine.
bool StartFlying() {
    if (!g_cam.obj) return false;

    fc_mat raw = g_cam.savedView;
    if (Readable(g_cam.obj + g_cam.viewOff, 64))
        std::memcpy(&raw, g_cam.obj + g_cam.viewOff, 64);
    fc_mat view = raw;
    if (g_cam.transposed) fc_mat_transpose(&raw, &view);

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
        g_cam.savedView = raw;
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

    // fc_init() clears the state, path included; keep what was recorded before
    if (g_state.path_count > 0) {
        g_savedPathCount = g_state.path_count;
        std::memcpy(g_savedPath, g_state.path, sizeof(fc_keyframe) * (size_t)g_savedPathCount);
    }
    fc_init(&g_state, &cfg);
    if (g_savedPathCount > 0) {
        g_state.path_count = g_savedPathCount;
        std::memcpy(g_state.path, g_savedPath, sizeof(fc_keyframe) * (size_t)g_savedPathCount);
        TW_Log("FreeCam: %d recorded keyframes kept from the last flight", g_savedPathCount);
    }
    g_flightClock = 0.0f;
    fc_play_set_speed(&g_state, g_playSpeed);

    fc_mat world;
    fc_mat_from_pose(&world, pos, yaw, pitch, 0);
    if (!fc_set_active(&g_state, 1, &world)) return false;

    g_looking = false;
    g_live = 1;

    // Once, when the camera is first found: stand in front of the game's own input, so
    // the character stops moving and fighting under the camera.
    if (!tw::game::InputBlockReady()) {
        tw::game::InputBlockInstall(reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr)));
        TW_Log("FreeCam: %s", tw::game::InputBlockStatus());
    }

    TW_Log("FreeCam: on at %.1f %.1f %.1f (yaw %.0f, pitch %.0f, view row sign %d, %s)",
           g_state.pos[0], g_state.pos[1], g_state.pos[2], g_state.yaw * 57.2957795f,
           g_state.pitch * 57.2957795f, g_cam.viewSign, g_cam.how);
    return true;
}

void StopFlying() {
    InputBlockSetActive(false);      // the controls are the player's again
    fc_play_stop(&g_state);
    fc_follow(&g_state, nullptr, 0);
    fc_set_active(&g_state, 0, nullptr);
    g_live = 0;
    g_looking = false;
    g_haveWrote = 0;
    RestoreMatrices();   // the matrices as they were when the flight started
    TW_Log("FreeCam: off after %.0f units", g_state.moved);
}

DWORD WINAPI Loop(LPVOID) {
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    int lost = 0, held = 0, taken = 0, refused = 0, sinceAge = 0, waited = 0, looks = 0;
    int quietTicks = 0;   // loop turns (1 ms each) without a single accessor call
    ULONGLONG lastReport = GetTickCount64();

    timeBeginPeriod(1);
    TW_Log("FreeCam: camera thread up (build 11: J plays the path, M clears it, O+O freezes, frozen camera pans)");

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

        if (g_newProbe) {
            void *obj = g_newProbe;
            g_newProbe = nullptr;
            DumpProbe(obj);
        }

        if (KeyDown(VK_F10)) {
            TW_Log("FreeCam: panic key, putting the camera back and the accessor back");
            Hud("F10", "free cam off");
            fc_freeze(&g_state, 0);
            InputBlockSetActive(false);
            g_want = 0;
            if (g_live) StopFlying();
            RemoveDetour(g_viewAcc);
            g_detourTried = false;
            g_detourOk = false;
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
            if (!g_hookQuietLogged && g_viewAcc.installed && ++quietTicks >= 3000) {
                g_hookQuietLogged = true;
                TW_Log("FreeCam: the view accessor is hooked but the game has not called it in three "
                       "seconds - this build may not use that function");
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

        float mx, my;
        PollInput(&mx, &my);

        float player[3] = { 0.0f, 0.0f, 0.0f };
        bool havePlayer = false;
        if (fc_following(&g_state) || g_followKey.Peek()) havePlayer = PlayerPosition(player);

        g_flightClock += dt;
        BeginHotkeyFrame();
        HandleHotkeys(player, havePlayer);

        // The player only keeps control while the camera is frozen (O) or following (P).
        const bool blockNow = g_live && !PlayerHasControl();
        if (blockNow != (tw::game::InputBlockActive() ? true : false)) {
            tw::game::InputBlockSetActive(blockNow);
            TW_Log("FreeCam: player controls %s (%s)", blockNow ? "held back" : "given back",
                   PlayerHasControl() ? (fc_frozen(&g_state) ? "the camera is frozen" :
                                                               "following the player")
                                      : "the camera is flying");
        }

        if (fc_following(&g_state) && !fc_frozen(&g_state)) PeekGameCameraAngles();

        fc_mat world;
        bool got = false;
        if (fc_playing(&g_state)) {
            // the path drives the camera; keys and mouse are ignored until it ends
            got = fc_play_step(&g_state, dt, &world) != 0;
        } else if (fc_step(&g_state, dt, mx, my, &world)) {
            if (fc_looking_at(&g_state)) {
                // frozen camera aiming at the player: the position never changes, only the
                // angles, so this is a pan and not a follow
                float target[3];
                if (PlayerPosition(target)) {
                    target[2] += kAimHeight;
                    if (fc_aim_at(&g_state, target, dt)) {
                        fc_mat_from_pose(&world, g_state.pos, g_state.yaw, g_state.pitch, 0);
                    }
                }
            } else if (fc_following(&g_state)) {
                if (PlayerPosition(player)) {
                    fc_follow_update(&g_state, player);
                    /* the matrix written to the camera's world slot is always a world
                     * matrix, whatever fc_step was asked to produce */
                    fc_mat_from_pose(&world, g_state.pos, g_state.yaw, g_state.pitch, 0);
                }
            }
            got = fc_mat_valid(&world) != 0;
        }

        if (got) {
            fc_mat view;
            fc_mat_from_pose(&view, g_state.pos, g_state.yaw, g_state.pitch, g_cam.viewSign);

            if (WriteMatrices(world, view)) {
                refused = 0;
                fc_mat after;
                if (Readable(g_cam.obj + g_cam.viewOff, 64)) {
                    std::memcpy(&after, g_cam.obj + g_cam.viewOff, 64);
                    fc_mat expect = view;
                    if (g_cam.transposed) fc_mat_transpose(&view, &expect);
                    int same = 1;
                    for (int i = 0; i < 16; i++) {
                        float d = after.m[i] - expect.m[i];
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
                       "writes held %d, taken back %d%s%s",
                       g_state.pos[0], g_state.pos[1], g_state.pos[2],
                       g_viewAcc.installed ? "hooked" : "not hooked", answers, calls, held, taken,
                       fc_looking_at(&g_state) ? ", frozen and aiming at the player"
                       : fc_following(&g_state) ? ", following the player" : "",
                       g_state.path_count ? ", path has keyframes" : "");
                if (fc_following(&g_state) || g_panSamples) {
                    TW_Log("FreeCam: %d game camera pose(s) seen while following - those are the "
                           "turns copied; playback %s", g_panSamples,
                           fc_playing(&g_state) ? "on" : "off");
                    if (!fc_following(&g_state)) g_panSamples = 0;
                }
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

int FreeCamHudLines(FreeCamHudLine *out, int max) { return HudCollect(out, max); }

bool FreeCamBlockingInput() { return tw::game::InputBlockActive(); }

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
        InputBlockSetActive(false);      // the controls are the player's before the thread wakes
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

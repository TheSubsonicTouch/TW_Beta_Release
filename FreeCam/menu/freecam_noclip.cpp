// freecam_noclip.cpp - fly the player themselves.
//
// The free camera writes a camera; this writes the character's own world transform. It is
// the same two hops the camera already uses to follow the player
// (0x23f25f8 -> +0x58 -> +0x80), read and written through the same validation, with only
// the translation touched.
//
// Two things have to be true for that to be noclip rather than a very fast elevator.
//
//   1. The position written has to be the one the game reads. It is, for the camera that
//      follows the player and for the renderer - but the character controller may keep its
//      own copy and copy it back over the node each tick, in which case a write is undone
//      before it is ever drawn. So the first tick looks for other pointers to the player's
//      position: any pointer in the player object or the node that lands on three readable,
//      writable floats matching the position exactly is a copy, and every tick writes the
//      anchor into those as well. Nothing is assumed about where they are or what owns
//      them; a pointer that stops matching is dropped.
//   2. The game must not win the race. The write happens on the camera thread every 1 ms,
//      which is several times per frame, so the last write before the frame is ours.
//
// The log line reports how far the game moved the character back between two writes. A
// steady 0.00 m means the write is holding and the only thing in the way of walking into a
// wall was the direction the anchor was moved in; a metre or two per tick means the
// controller is rewriting the node and the copies above are what carries the flight.
//
// Why an anchor rather than "add to the matrix every tick": the game's physics keeps
// running. If the position were only nudged while a key was down, the character would fall
// between presses and the whole thing would be a very fast elevator. So the first tick
// keeps the position it found, every key moves that kept position, and every tick writes
// it back. The character therefore hovers where you put it, which is what noclip means.
// If the game moves the character somewhere else on its own - a cutscene, a loading
// screen, a respawn - the anchor notices the distance and follows it instead of dragging
// the player back.
#include "freecam_noclip.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "../src/freecam.h"
}

#include "freecam_game.h"
#include "log.h"

namespace tw {
namespace game {
namespace {

constexpr uintptr_t kPlayerGlobal = 0x23f25f8;  // void *local player
constexpr uintptr_t kPlayerNode   = 0x58;       // player -> node
constexpr uintptr_t kNodeMatrix   = 0x80;       // node -> 4x4 world transform

// Metres per second. A character walks about 1.8; this is meant to be a way to get from
// one end of the map to the other, so the slowest setting is already brisk.
constexpr float kBaseSpeed = 8.0f;
constexpr float kFastSpeed = 3.0f;    // Shift
constexpr float kSlowSpeed = 0.3f;    // Ctrl
constexpr float kMaxStep = 0.25f;     // a hitch in the loop must not teleport anybody

// How far the game may move the character before the anchor decides that it was not us
// who moved it - a respawn, a cutscene, a loading screen - and follows along.
constexpr float kReanchorMetres = 12.0f;

const char *const kSpeedLabels[] = { "1x", "2x", "3x", "4x", "6x", "8x" };
const float kSpeedValues[] = { 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f };
const int kSpeedCount = 6;

bool g_active = false;
volatile int g_speedIndex = 0;   // changed by the menu thread as well
ULONGLONG g_lastLog = 0;

// The position this feature owns. Kept between ticks, because it is written back every
// tick; see the note at the top of the file.
struct Anchor {
    bool have = false;
    void *player = nullptr;
    float p[3] = { 0.0f, 0.0f, 0.0f };
};
Anchor g_anchor;

// A second copy of the player's position, found at runtime: a pointer in the player object
// or its node that lands on three floats matching the position. The controller writing one
// of these back over the node is exactly what "collision pushes me out of the wall" looks
// like from the outside, so the anchor is written into them too.
struct Mirror {
    float *address;
    float last[3];      // what was written last, so a copy someone else owns is noticed
};
constexpr int kMaxMirrors = 12;
Mirror g_mirrors[kMaxMirrors];
int g_mirrorCount = 0;
ULONGLONG g_lastScan = 0;
bool g_scanned = false;

// How far the game moved the character since the last write, kept for the log line.
float g_driftMax = 0.0f;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

bool Readable(const void *p, size_t n) {
    if (!p) return false;
    const uint8_t *b = static_cast<const uint8_t *>(p);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(b, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const uint8_t *last = b + (n ? n - 1 : 0);
    if ((reinterpret_cast<uintptr_t>(b) & ~uintptr_t(0xFFF)) !=
        (reinterpret_cast<uintptr_t>(last) & ~uintptr_t(0xFFF))) {
        MEMORY_BASIC_INFORMATION m2{};
        if (VirtualQuery(last, &m2, sizeof m2) != sizeof m2) return false;
        if (m2.State != MEM_COMMIT || (m2.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    }
    // A matrix we are about to write has to be writable, not merely readable.
    if (mbi.Protect & (PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE)) return false;
    return true;
}

void *PlayerObject() {
    const uintptr_t base = Base();
    if (!base || !Readable(reinterpret_cast<void *>(base + kPlayerGlobal), 8)) return nullptr;
    void *player = *reinterpret_cast<void **>(base + kPlayerGlobal);
    if (!Readable(player, 0x60)) return nullptr;
    return player;
}

// The live player transform, as the game keeps it. Null when the player is not in the
// world (title screen, loading, between maps).
float *PlayerMatrix(void **nodeOut) {
    if (nodeOut) *nodeOut = nullptr;
    void *player = PlayerObject();
    if (!player) return nullptr;
    void *node = *reinterpret_cast<void **>(static_cast<uint8_t *>(player) + kPlayerNode);
    if (!Readable(node, 0xC0)) return nullptr;
    float *m = reinterpret_cast<float *>(static_cast<uint8_t *>(node) + kNodeMatrix);
    if (!Readable(m, 64)) return nullptr;
    if (nodeOut) *nodeOut = node;
    return m;
}

float *PlayerMatrix() {
    void *node = nullptr;
    return PlayerMatrix(&node);
}

float Distance(const float a[3], const float b[3]);

bool Writable(const void *p, size_t n) {
    if (!Readable(p, n)) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;
    return !(mbi.Protect & (PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_GUARD));
}

bool Same(const float *f, const float p[3], float tolerance) {
    return std::fabs(f[0] - p[0]) <= tolerance && std::fabs(f[1] - p[1]) <= tolerance &&
           std::fabs(f[2] - p[2]) <= tolerance;
}

// Every pointer in the player object and the node that lands on a copy of the player's
// position. Pointers back into the two objects themselves are skipped: those are the
// transform being written already.
void ScanMirrors(void *player, void *node, const float pos[3]) {
    g_mirrorCount = 0;
    g_scanned = true;
    const struct { void *object; uintptr_t span; } objects[] = {
        { player, 0x800 }, { node, 0x400 },
    };
    for (const auto &object : objects) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(object.object);
        if (!base) continue;
        for (uintptr_t off = 0; off + sizeof(uintptr_t) <= object.span; off += sizeof(uintptr_t)) {
            if (!Readable(reinterpret_cast<void *>(base + off), sizeof(uintptr_t))) continue;
            const uintptr_t target = *reinterpret_cast<uintptr_t *>(base + off);
            if (target < 0x10000) continue;
            // Its own objects are the transform, not a copy of it.
            if (target >= base && target < base + object.span) continue;
            if (target >= reinterpret_cast<uintptr_t>(player) &&
                target < reinterpret_cast<uintptr_t>(player) + 0x800) {
                continue;
            }
            if (target >= reinterpret_cast<uintptr_t>(node) &&
                target < reinterpret_cast<uintptr_t>(node) + 0x400) {
                continue;
            }
            if (!Writable(reinterpret_cast<void *>(target), sizeof(float) * 3)) continue;
            const float *f = reinterpret_cast<const float *>(target);
            if (!Same(f, pos, 0.02f)) continue;
            bool known = false;
            for (int i = 0; i < g_mirrorCount; ++i) {
                if (g_mirrors[i].address == f) known = true;
            }
            if (known) continue;
            Mirror &mirror = g_mirrors[g_mirrorCount++];
            mirror.address = const_cast<float *>(f);
            mirror.last[0] = f[0];
            mirror.last[1] = f[1];
            mirror.last[2] = f[2];
            if (g_mirrorCount >= kMaxMirrors) return;
        }
    }
}

void WriteMirrors(const float pos[3], const float current[3]) {
    for (int i = 0; i < g_mirrorCount; ++i) {
        Mirror &mirror = g_mirrors[i];
        float *f = mirror.address;
        // Out of range, out of the process, or no longer a position-shaped block: drop it
        // rather than write into whatever owns it now.
        bool keep = Writable(f, sizeof(float) * 3) &&
                    std::fabs(f[0]) < 50000.0f && std::fabs(f[1]) < 50000.0f &&
                    std::fabs(f[2]) < 50000.0f;
        // A copy the game is still writing itself is worth keeping even though our value is
        // gone; a copy that belongs to something else entirely - never what was written and
        // never the player - is not.
        if (keep && Distance(f, mirror.last) > 3.0f && Distance(f, current) > 3.0f) keep = false;
        if (!keep) {
            g_mirrors[i] = g_mirrors[--g_mirrorCount];
            --i;
            continue;
        }
        f[0] = pos[0];
        f[1] = pos[1];
        f[2] = pos[2];
        mirror.last[0] = pos[0];
        mirror.last[1] = pos[1];
        mirror.last[2] = pos[2];
    }
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

float Distance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool NoclipStep() {
    if (!g_active) return false;

    // Its own clock, because the camera loop's one is reset whenever the camera is idle and
    // noclip has to keep working then.
    static LARGE_INTEGER freq = {0}, last = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = 0.0f;
    if (last.QuadPart != 0 && freq.QuadPart != 0)
        dt = static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart);
    last.QuadPart = now.QuadPart;
    if (dt > kMaxStep) dt = kMaxStep;

    void *node = nullptr;
    float *m = PlayerMatrix(&node);
    if (!m) {
        g_anchor.have = false;   // not in the world; start again from wherever we land
        g_mirrorCount = 0;
        g_scanned = false;
        return false;
    }
    void *player = PlayerObject();

    const float cur[3] = { m[12], m[13], m[14] };
    if (!g_anchor.have || g_anchor.player != player) {
        std::memcpy(g_anchor.p, cur, sizeof g_anchor.p);
        g_anchor.player = player;
        g_anchor.have = true;
        TW_Log("Noclip: anchored at %.1f %.1f %.1f", g_anchor.p[0], g_anchor.p[1], g_anchor.p[2]);
    } else if (Distance(cur, g_anchor.p) > kReanchorMetres) {
        // The game put the character somewhere else: follow it rather than fight it.
        std::memcpy(g_anchor.p, cur, sizeof g_anchor.p);
        TW_Log("Noclip: the game moved the player, re-anchored at %.1f %.1f %.1f",
               g_anchor.p[0], g_anchor.p[1], g_anchor.p[2]);
    }

    // Which way the character is facing, flat. The camera's reader is used so this follows
    // whatever the game wrote rather than an assumed layout.
    fc_mat world;
    std::memcpy(world.m, m, sizeof world.m);
    fc_mat check = world;
    float pos[3] = { 0.0f, 0.0f, 0.0f }, yaw = 0.0f, pitch = 0.0f;
    static float lastYaw = 0.0f;
    if (fc_mat_to_pose(&check, pos, &yaw, &pitch, 0)) lastYaw = yaw;
    else yaw = lastYaw;   // keep flying in the last known direction rather than stopping

    float f[3], r[3], u[3];
    fc_basis(yaw, 0.0f, f, r, u);

    // The menu has the keyboard while it is open; this reads the keys directly, so a name
    // being typed must not also fly the character around. The position is still written
    // below, so switching the menu on does not drop the player out of the air.
    float wish[3] = { 0.0f, 0.0f, 0.0f };
    const bool typing = FreeCamMenuOpen();
    if (!typing && KeyDown('W')) { wish[0] += f[0]; wish[1] += f[1]; wish[2] += f[2]; }
    if (!typing && KeyDown('S')) { wish[0] -= f[0]; wish[1] -= f[1]; wish[2] -= f[2]; }
    if (!typing && KeyDown('D')) { wish[0] += r[0]; wish[1] += r[1]; wish[2] += r[2]; }
    if (!typing && KeyDown('A')) { wish[0] -= r[0]; wish[1] -= r[1]; wish[2] -= r[2]; }
    if (!typing && KeyDown(VK_SPACE)) { wish[0] += u[0]; wish[1] += u[1]; wish[2] += u[2]; }
    if (!typing && KeyDown('C')) { wish[0] -= u[0]; wish[1] -= u[1]; wish[2] -= u[2]; }

    const float len = std::sqrt(wish[0] * wish[0] + wish[1] * wish[1] + wish[2] * wish[2]);
    if (len > 1e-4f) {
        float speed = kBaseSpeed * NoclipSpeed();
        if (KeyDown(VK_SHIFT)) speed *= kFastSpeed;
        if (KeyDown(VK_CONTROL)) speed *= kSlowSpeed;
        const float step = (speed * dt) / len;
        for (int i = 0; i < 3; ++i) g_anchor.p[i] += wish[i] * step;
    }

    // How far the game moved the character since the last write. Zero means the write is
    // what the game is reading; a steady metre means it keeps a copy of its own, which is
    // what the scan below is for. Measured before the write, so it is the game's doing.
    const float drift = Distance(cur, g_anchor.p);
    if (drift > g_driftMax) g_driftMax = drift;

    // The other copies of the position, found once the player exists. Rescanning every two
    // seconds until something turns up covers a player object that is rebuilt on a respawn
    // or a load; once found, they are tracked from then on.
    const ULONGLONG scanTick = GetTickCount64();
    if (!g_scanned || (g_mirrorCount == 0 && scanTick - g_lastScan > 2000)) {
        g_lastScan = scanTick;
        ScanMirrors(player, node, cur);
        if (g_mirrorCount > 0) {
            TW_Log("Noclip: %d other copy/copies of the player position tracked",
                   g_mirrorCount);
        }
    }

    // Written every tick, moving or not: that is what holds the character still instead of
    // letting the game's gravity take it back down. Only the translation.
    m[12] = g_anchor.p[0];
    m[13] = g_anchor.p[1];
    m[14] = g_anchor.p[2];
    WriteMirrors(g_anchor.p, cur);

    const ULONGLONG tick = GetTickCount64();
    if (tick - g_lastLog > 5000) {
        g_lastLog = tick;
        TW_Log("Noclip: player at %.1f %.1f %.1f, %.0fx, the game pulled it back %.2f m,"
               " %d position copy/copies held",
               g_anchor.p[0], g_anchor.p[1], g_anchor.p[2],
               static_cast<double>(NoclipSpeed()), static_cast<double>(g_driftMax),
               g_mirrorCount);
        g_driftMax = 0.0f;
    }
    return true;
}

void NoclipSet(bool on) {
    if (on == g_active) return;
    if (on) {
        // Noclip and the freed player would both answer to WASD; the character has one
        // mover at a time, so turning this on takes the controls off the player again.
        tw::game::FreeCamSetPlayerFree(false);
        // Noclip runs on the camera thread, and that thread is created when the free
        // camera is switched on. Someone who turns noclip on from the menu without ever
        // flying would otherwise get nothing at all.
        const bool thread = FreeCamEnsureThread();
        if (!thread) TW_Log("Noclip: could not start the thread that moves the player");
    }
    g_active = on;
    g_anchor.have = false;    // take the position it finds when it starts
    g_lastLog = 0;
    g_mirrorCount = 0;        // the copies belong to whoever was in the world last
    g_scanned = false;
    g_driftMax = 0.0f;
    TW_Log("Noclip: %s", on ? "on - the player is flown directly, collision is not consulted"
                            : "off - the game's own movement takes the character back");
}

bool NoclipActive() { return g_active; }

bool NoclipPlayerReady() { return PlayerMatrix() != nullptr; }

float NoclipSpeed() { return kSpeedValues[g_speedIndex]; }

void NoclipSetSpeedIndex(int index) {
    if (index < 0) index = 0;
    if (index >= kSpeedCount) index = kSpeedCount - 1;
    g_speedIndex = index;
}

int NoclipSpeedIndex() { return g_speedIndex; }

void NoclipSpeedStep(int direction) {
    NoclipSetSpeedIndex(g_speedIndex + (direction > 0 ? 1 : -1));
}

const char *const *NoclipSpeedLabels() { return kSpeedLabels; }

int NoclipSpeedCount() { return kSpeedCount; }

}  // namespace game
}  // namespace tw

// freecam_sky.cpp - hold the skybox on the variant and the colour the menu chose.
//
// The one thing this file must never do is write somewhere it has not looked at first.
// The sky's colours live behind three pointers, and a mod that writes a float through a
// pointer it guessed is a mod that crashes somebody's game once and is never trusted
// again. So every hop is checked (readable, committed, writable), the block that comes out
// the other end has to look like a colour block before it is believed, and the values it
// held are kept so they can be put back.
#include "freecam_sky.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "freecam_weather.h"
#include "log.h"
#include "menu_core.h"      // tw::SkySettings, the object the menu and the keys share

namespace tw {
namespace game {
namespace {

constexpr uintptr_t kSkyMarshall = 0x23a73e0;   // see the table in freecam_sky.h
constexpr uintptr_t kSlots       = 0x0d8;       // void*[4], stride 0x20
constexpr int       kSlotCount   = 4;
constexpr uintptr_t kSlotStride  = 0x20;
constexpr uintptr_t kSlotArray   = 0x078;       // int64: offset from the slot to its entries
constexpr uintptr_t kEntryStride = 0x0c0;
constexpr uintptr_t kEntryParams = 0x050;       // entry -> the block the renderer reads
constexpr uintptr_t kColourA     = 0x05c;       // float3
constexpr uintptr_t kColourB     = 0x068;       // float3
constexpr uintptr_t kColourC     = 0x074;       // float3
constexpr uintptr_t kColourSpan  = kColourC + 3 * sizeof(float);

// How far into a slot's entry array to look. The renderer draws the skies a slot is
// showing, and never more than a handful at once; four covers that with room to spare and
// keeps the walk short enough to run every frame.
constexpr int kLookEntries = 4;

// How many distinct parameter blocks to tint at once. Two skies blending into each other
// is the normal case; anything above this is a list that changed under us, and the oldest
// blocks are dropped rather than followed.
constexpr int kMaxBlocks = 12;

// The two durations of a sky change, in seconds, on the SkyMarshall itself. The game
// starts every change with thirty; the menu's switch is meant to be seen, so while the
// override is on a change fades over kShortFade instead. They are the divisors the sky's
// own state machine uses for its blend, so a smaller number is a faster fade.
constexpr uintptr_t kFadeIn  = 0x9fec;
constexpr uintptr_t kFadeOut = 0x9ff0;
constexpr float     kShortFade = 0.20f;

// A block is believed only if every float in it is a plausible colour: finite, and inside
// a range wide enough for the sky's own (possibly unclamped) units and narrow enough to
// reject a pointer that landed on a vtable or a string instead. Nothing outside this range
// is ever written through.
constexpr float kColourMin = -2.0f;
constexpr float kColourMax = 16.0f;

struct Block {
    uint8_t* base = nullptr;
    float original[9] = { 0.0f };   // what was there before this module touched it
    float written[9] = { 0.0f };    // what this module last put there
    bool tinted = false;            // true while the game's memory holds our colour
};

Block g_blocks[kMaxBlocks];
int g_blockCount = 0;
ULONGLONG g_lastLook = 0;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

bool Readable(const void* p, size_t n, bool forWrite) {
    if (!p) return false;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(b, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    if (forWrite) {
        if (prot != PAGE_READWRITE && prot != PAGE_WRITECOPY &&
            prot != PAGE_EXECUTE_READWRITE && prot != PAGE_EXECUTE_WRITECOPY)
            return false;
    } else {
        const bool ok = prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                        prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
                        prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
        if (!ok) return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(b);
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start + n <= end;
}

uint8_t* SkyMarshall() {
    const uintptr_t base = Base();
    if (!base) return nullptr;
    void* obj = reinterpret_cast<void*>(base + kSkyMarshall);
    // The slots are the furthest thing this file touches on the object itself.
    if (!Readable(obj, kSlots + kSlotCount * kSlotStride, false)) return nullptr;
    return static_cast<uint8_t*>(obj);
}

float* FloatAt(uint8_t* p, uintptr_t off) { return reinterpret_cast<float*>(p + off); }

bool Finite(float v) { return v == v && v > -1e6f && v < 1e6f; }

// A block is only believed if all nine floats are colours: finite, not negative, and
// under kColourMax. Anything else is not the sky's colours and is left alone.
bool LooksLikeColours(const uint8_t* p) {
    if (!Readable(p, kColourSpan, true)) return false;
    const float* f = reinterpret_cast<const float*>(p + kColourA);
    bool anyLit = false;
    for (int i = 0; i < 9; ++i) {
        if (!Finite(f[i])) return false;
        if (f[i] < kColourMin || f[i] > kColourMax) return false;
        if (f[i] > 0.001f) anyLit = true;
    }
    // Nine zeroes is a block that has not been filled in yet; tinting it would make a sky
    // out of nothing, and there is no telling what it belongs to before it is filled.
    return anyLit;
}

bool Known(const uint8_t* base) {
    for (int i = 0; i < g_blockCount; ++i)
        if (g_blocks[i].base == base) return true;
    return false;
}

void Remember(uint8_t* base) {
    if (g_blockCount >= kMaxBlocks) return;
    Block& b = g_blocks[g_blockCount++];
    b.base = base;
    b.tinted = false;
    std::memset(b.written, 0, sizeof b.written);
    std::memcpy(b.original, base + kColourA, sizeof b.original);
}

// Walk the object down to the parameter blocks the renderer reads. Returns how many are
// known afterwards, which is also the answer to "can this build tint the sky at all".
int Discover() {
    uint8_t* sky = SkyMarshall();
    if (!sky) return 0;

    // Drop the blocks whose memory has gone - a reloaded world builds new ones - so the
    // list does not fill up with addresses that can never be written again.
    for (int i = 0; i < g_blockCount;) {
        if (g_blocks[i].base && Readable(g_blocks[i].base, kColourSpan, true)) { ++i; continue; }
        g_blocks[i] = g_blocks[--g_blockCount];
    }
    for (int slot = 0; slot < kSlotCount; ++slot) {
        uint8_t** holderSlot = reinterpret_cast<uint8_t**>(sky + kSlots + slot * kSlotStride);
        if (!Readable(holderSlot, sizeof(void*), false)) continue;
        uint8_t* holder = *holderSlot;
        if (!Readable(holder, kSlotArray + sizeof(int64_t), false)) continue;
        const int64_t rel = *reinterpret_cast<int64_t*>(holder + kSlotArray);
        if (rel == 0) continue;                       // this slot is not showing a sky
        // The offset is relative to the field itself, the way the game computes it.
        uint8_t* entries = holder + kSlotArray + rel;
        for (int i = 0; i < kLookEntries; ++i) {
            uint8_t* entry = entries + static_cast<size_t>(i) * kEntryStride;
            if (!Readable(entry, kEntryParams + sizeof(void*), false)) break;
            uint8_t* params = *reinterpret_cast<uint8_t**>(entry + kEntryParams);
            if (!params || Known(params) || !LooksLikeColours(params)) continue;
            Remember(params);
        }
    }
    return g_blockCount;
}

// Put every borrowed value back. Called when the override is switched off, so the game
// gets the sky it had rather than the one that was painted over it.
void Restore() {
    for (int i = 0; i < g_blockCount; ++i) {
        Block& b = g_blocks[i];
        if (!b.base) continue;
        // Only what this module actually changed is put back: the originals are kept, so
        // switching the override on and off again cannot drift the sky's own colours.
        if (!b.tinted) continue;
        if (!Readable(b.base, kColourSpan, true)) { b.tinted = false; continue; }
        std::memcpy(b.base + kColourA, b.original, sizeof b.original);
        b.tinted = false;
    }
}

// One tick of "the override is off": put back anything that is still out there, and keep a
// slow eye on whether the colours can be reached at all, so the menu can say so.
void TickIdle() {
    Restore();
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastLook < 500) return;
    g_lastLook = now;
    Discover();
}

// The colour: every one of the three triples is scaled toward the tint, so the sky keeps
// its shape - a sunset still reads as a sunset - and changes hue. Strength 0 leaves the
// sky exactly as the game drew it; strength 1 is the tint at full voice.
void Paint(const tw::SkySettings& s) {
    float r = 1.0f, g = 1.0f, b = 1.0f;
    s.tint(&r, &g, &b);
    const float strength = s.strength() <= 0.0f ? 0.0f : s.strength();
    if (strength <= 0.0f) { Restore(); return; }

    for (int i = 0; i < g_blockCount; ++i) {
        Block& block = g_blocks[i];
        if (!block.base || !Readable(block.base, kColourSpan, true)) continue;
        float* live = reinterpret_cast<float*>(block.base + kColourA);

        // If the block no longer holds what this module put there, the game has written
        // it since - a different skydome, a reloaded world. Ours is not the value to
        // restore any more, so it goes back to being the game's block.
        if (block.tinted && std::memcmp(live, block.written, sizeof block.written) != 0)
            block.tinted = false;

        // A block that is not ours is read as it stands: the original is whatever is in
        // it right now, so the tint is always relative to the game's own colour and a
        // value captured minutes ago is never multiplied up out of nowhere.
        if (!block.tinted) std::memcpy(block.original, live, sizeof block.original);

        for (int c = 0; c < 9; ++c) {
            // 0.5 is "no change": a tint of 1.0 doubles the channel, 0 removes it, and the
            // strength decides how far along that the sky actually goes.
            const float wanted = (c % 3 == 0) ? r : (c % 3 == 1) ? g : b;
            float factor = 1.0f + (2.0f * wanted - 1.0f) * strength;
            if (factor < 0.0f) factor = 0.0f;
            if (factor > 2.0f) factor = 2.0f;
            float out = block.original[c] * factor;
            if (out < 0.0f) out = 0.0f;
            if (out > kColourMax) out = kColourMax;
            live[c] = out;
        }
        std::memcpy(block.written, live, sizeof block.written);
        block.tinted = true;
    }
}

// Hold the chosen skydome. Only when the game has actually moved off it: calling
// set_skydome every frame would restart the blend every frame and the sky would sit
// permanently half-way between two of them.
void HoldVariant(const tw::SkySettings& s) {
    if (s.variant() <= 0) return;                    // "Automatic": the game decides
    uint8_t* sky = SkyMarshall();
    if (!sky) return;
    const int want = s.variant();
    if (want > static_cast<int>(SkydomeNames().size()) - 1) return;
    if (SkydomeCurrentMenuIndex() == want) return;
    SkydomeSetIndex(want);
}

}  // namespace

tw::SkySettings& Sky() {
    // Deliberately a function-local static: the menu is loaded into a running game, and
    // nothing here may depend on the order static initialisers run in.
    static tw::SkySettings settings;
    return settings;
}

std::vector<std::string> SkyVariantNames() { return SkydomeNames(); }

std::string SkyStateLine() {
    const tw::SkySettings& s = Sky();
    const std::vector<std::string> names = SkydomeNames();
    if (names.size() <= 1) return "waiting for the world to load";
    if (!s.enabled()) return "off - the game's own sky";
    if (g_blockCount <= 0) return "variant held; this build cannot reach the sky colours";
    const std::size_t index = static_cast<std::size_t>(s.variant());
    const std::string name = index < names.size() ? names[index] : std::string("Automatic");
    return name + ", " + s.describe();
}

void SkyTick() {
    tw::SkySettings& s = Sky();
    const std::vector<std::string> names = SkydomeNames();
    s.setVariantCount(static_cast<int>(names.size()));

    if (!SkyMarshall()) { Restore(); return; }
    if (!s.enabled()) { TickIdle(); return; }

    Discover();
    HoldVariant(s);

    // The game starts every sky change with a thirty second fade. While the override is
    // on, a change is the menu's doing and should be visible now, so the fade is cut down.
    {
        uint8_t* sky = SkyMarshall();
        if (sky && Readable(sky + kFadeIn, 2 * sizeof(float), true)) {
            float* fadeIn = FloatAt(sky, kFadeIn);
            float* fadeOut = FloatAt(sky, kFadeOut);
            if (*fadeIn > kShortFade) *fadeIn = kShortFade;
            if (*fadeOut > kShortFade) *fadeOut = kShortFade;
        }
    }

    Paint(s);
}

}  // namespace game
}  // namespace tw

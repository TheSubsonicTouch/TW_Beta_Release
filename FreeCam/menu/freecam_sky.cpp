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

// A block is believed only if its nine floats look like a sky's colours. The sky is lit in
// linear space, so a sun or a cloud value can be far above 1 - the range has to be wide
// enough for that and narrow enough to reject a pointer that landed on a vtable or a
// string, whose bytes read as floats are either denormal or astronomically large.
constexpr float kColourMin = -16.0f;
constexpr float kColourMax = 4096.0f;
// How many of the nine have to be in a sane, non-negative range, and how bright the block
// as a whole has to be. A block of genuine zeroes is a block that has not been filled in
// yet: tinting it would paint a sky out of nothing.
constexpr int   kSaneFloats = 7;
constexpr float kLitAt = 0.01f;

struct Block {
    uint8_t* base = nullptr;
    float original[9] = { 0.0f };   // what was there before this module touched it
    float written[9] = { 0.0f };    // what this module last put there
    bool tinted = false;            // true while the game's memory holds our colour
    DWORD oldProtect = 0;           // set when the page had to be opened to be written
};

Block g_blocks[kMaxBlocks];
int g_blockCount = 0;
ULONGLONG g_lastLook = 0;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// Reads only: whether the pages can be read for the span asked about. Writing goes
// through OpenForWrite below, which is a separate question with a separate answer.
bool Readable(const void* p, size_t n) {
    if (!p) return false;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(b, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    const bool ok = prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                    prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
                    prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    if (!ok) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(b);
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start + n <= end;
}

bool Writable(const void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

// The skydome data is loaded out of the game's own bundles, and those are mapped rather
// than copied: the colour block can be readable and not writable. That is not a reason to
// give up on it - the game is the only writer of those pages, and asking for write access
// on the couple of pages that hold the sky's colours is what a texture mod does anyway.
// The previous protection is remembered per block and handed back when the override goes
// off, so nothing is left writable that was not writable before.
bool OpenForWrite(void* p, size_t n, DWORD* old) {
    *old = 0;
    if (Writable(p)) return true;
    DWORD previous = 0;
    if (!VirtualProtect(p, n, PAGE_READWRITE, &previous)) return false;
    *old = previous;
    return true;
}

void CloseWrite(void* p, size_t n, DWORD old) {
    if (old == 0) return;               // we never changed it
    DWORD ignored = 0;
    VirtualProtect(p, n, old, &ignored);
}

uint8_t* SkyMarshall() {
    const uintptr_t base = Base();
    if (!base) return nullptr;
    void* obj = reinterpret_cast<void*>(base + kSkyMarshall);
    // The slots are the furthest thing this file touches on the object itself.
    if (!Readable(obj, kSlots + kSlotCount * kSlotStride)) return nullptr;
    return static_cast<uint8_t*>(obj);
}

float* FloatAt(uint8_t* p, uintptr_t off) { return reinterpret_cast<float*>(p + off); }

bool Finite(float v) { return v == v && v > -1e6f && v < 1e6f; }

// A block is only believed if all nine floats are colours: finite, not negative, and
// under kColourMax. Anything else is not the sky's colours and is left alone.
bool LooksLikeColours(const uint8_t* p) {
    if (!Readable(p, kColourSpan)) return false;
    const float* f = reinterpret_cast<const float*>(p + kColourA);
    int sane = 0;
    bool anyLit = false;
    for (int i = 0; i < 9; ++i) {
        if (!Finite(f[i])) return false;
        if (f[i] >= 0.0f && f[i] <= kColourMax) ++sane;
        else if (f[i] < kColourMin || f[i] > kColourMax) return false;
        if (f[i] > kLitAt) anyLit = true;
    }
    return anyLit && sane >= kSaneFloats;
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
    b.oldProtect = 0;
    std::memset(b.written, 0, sizeof b.written);
    std::memcpy(b.original, base + kColourA, sizeof b.original);
}

// Walk the object down to the parameter blocks the renderer reads. Returns how many are
// known afterwards, which is also the answer to "can this build tint the sky at all".
int Discover() {
    static bool logged = false;
    uint8_t* sky = SkyMarshall();
    if (!sky) return 0;

    // Drop the blocks whose memory has gone - a reloaded world builds new ones - so the
    // list does not fill up with addresses that can never be written again.
    for (int i = 0; i < g_blockCount;) {
        if (g_blocks[i].base && Readable(g_blocks[i].base, kColourSpan)) { ++i; continue; }
        CloseWrite(g_blocks[i].base, kColourSpan, g_blocks[i].oldProtect);
        g_blocks[i] = g_blocks[--g_blockCount];
    }
    for (int slot = 0; slot < kSlotCount; ++slot) {
        uint8_t** holderSlot = reinterpret_cast<uint8_t**>(sky + kSlots + slot * kSlotStride);
        if (!Readable(holderSlot, sizeof(void*))) continue;
        uint8_t* holder = *holderSlot;
        if (!Readable(holder, kSlotArray + sizeof(int64_t))) continue;
        const int64_t rel = *reinterpret_cast<int64_t*>(holder + kSlotArray);
        if (rel == 0) continue;                       // this slot is not showing a sky
        // The offset is relative to the field itself, the way the game computes it.
        uint8_t* entries = holder + kSlotArray + rel;
        for (int i = 0; i < kLookEntries; ++i) {
            uint8_t* entry = entries + static_cast<size_t>(i) * kEntryStride;
            if (!Readable(entry, kEntryParams + sizeof(void*))) break;
            uint8_t* params = *reinterpret_cast<uint8_t**>(entry + kEntryParams);
            if (!params || Known(params) || !LooksLikeColours(params)) continue;
            Remember(params);
        }
    }

    if (g_blockCount > 0 && !logged) {
        logged = true;
        const Block& b = g_blocks[0];
        TW_Log("sky: tinting %d colour block(s); first = %.3f %.3f %.3f / %.3f %.3f %.3f / "
               "%.3f %.3f %.3f",
               g_blockCount,
               static_cast<double>(b.original[0]), static_cast<double>(b.original[1]),
               static_cast<double>(b.original[2]), static_cast<double>(b.original[3]),
               static_cast<double>(b.original[4]), static_cast<double>(b.original[5]),
               static_cast<double>(b.original[6]), static_cast<double>(b.original[7]),
               static_cast<double>(b.original[8]));
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
        if (!Readable(b.base, kColourSpan)) { b.tinted = false; continue; }
        DWORD opened = 0;
        if (!Writable(b.base + kColourA)) {
            if (!OpenForWrite(b.base + kColourA, kColourSpan - kColourA, &opened)) {
                b.tinted = false;
                continue;
            }
            b.oldProtect = opened;
        }
        std::memcpy(b.base + kColourA, b.original, sizeof b.original);
        // Whatever this block borrowed is given back now: the protection first, so the game
        // sees the page exactly as it left it, then the colour.
        if (b.oldProtect) CloseWrite(b.base + kColourA, kColourSpan - kColourA, b.oldProtect);
        b.oldProtect = 0;
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
        if (!block.base || !Readable(block.base, kColourSpan)) continue;
        uint8_t* colours = block.base + kColourA;
        if (!Writable(colours) && !OpenForWrite(colours, kColourSpan - kColourA, &block.oldProtect))
            continue;                       // the page will not take a write; leave it alone
        float* live = reinterpret_cast<float*>(colours);

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
            // A dark sky multiplied by a colour is still dark, and at night nothing would
            // appear to happen. The tint also adds a little of itself, so a colour with
            // red in it shows on a near-black sky as well as on a bright one. It is added
            // rather than mixed in, so the sky keeps its own gradation.
            const float lift = (wanted > 0.5f ? (wanted - 0.5f) * 0.60f : 0.0f) * strength;
            float out = block.original[c] * factor + lift;
            if (out < 0.0f) out = 0.0f;
            if (out > kColourMax) out = kColourMax;
            live[c] = out;
        }
        std::memcpy(block.written, live, sizeof block.written);
        block.tinted = true;
        // The page is handed back as it was found the moment the write is done. The write
        // is repeated next frame from the original kept above, so nothing depends on the
        // page staying open - and the game's memory is never left writable by this module.
        if (block.oldProtect) {
            CloseWrite(colours, kColourSpan - kColourA, block.oldProtect);
            block.oldProtect = 0;
        }
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
    if (g_blockCount <= 0)
        return "variant held; no sky colours found - see TW_MenuLog.txt";
    const std::size_t index = static_cast<std::size_t>(s.variant());
    const std::string name = index < names.size() ? names[index] : std::string("Automatic");
    return name + ", " + s.describe();
}

// Ask the game for the sky it is already showing. That costs one sky change - short, since
// the fade is cut down - and in return the game's own setup runs again and builds its copy
// of the colours from the parameters this module has just tinted. "Automatic" gets the
// same nudge and is then handed straight back, so the weather still picks what comes next.
bool g_handBackAutomatic = false;

void NudgeSky(const tw::SkySettings& s) {
    const int showing = SkydomeCurrentMenuIndex();
    if (showing <= 0) return;                       // nothing showing: nothing to rebuild
    if (s.variant() > 0) {
        if (showing == s.variant()) SkydomeSetIndex(showing);
        return;                                     // otherwise HoldVariant will set it
    }
    SkydomeSetIndex(showing);
    g_handBackAutomatic = true;
}

void SkyTick() {
    tw::SkySettings& s = Sky();
    // The renderer takes its copy of the colours when the game sets a sky up, and draws
    // from that copy. A colour written into the parameters alone therefore does nothing
    // until the sky is changed again - which is the report this build answers. So when the
    // colour itself moves, the game is asked for the same sky once more: its own setup runs,
    // and it copies back the colours that were just written.
    float wr = 1.0f, wg = 1.0f, wb = 1.0f;
    s.tint(&wr, &wg, &wb);
    const float ws = s.strength();
    static float lastR = -1.0f, lastG = -1.0f, lastB = -1.0f, lastS = -1.0f;
    const bool newColour = wr != lastR || wg != lastG || wb != lastB || ws != lastS;
    lastR = wr; lastG = wg; lastB = wb; lastS = ws;
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
        if (sky && Readable(sky + kFadeIn, 2 * sizeof(float))) {
            float* fadeIn = FloatAt(sky, kFadeIn);
            float* fadeOut = FloatAt(sky, kFadeOut);
            if (*fadeIn > kShortFade) *fadeIn = kShortFade;
            if (*fadeOut > kShortFade) *fadeOut = kShortFade;
        }
    }

    Paint(s);

    // The colours have to be in the parameters before the game is asked to build its copy
    // of them, so the nudge comes after the paint - and for a few ticks, in case the world
    // was mid-change when the first one landed.
    static int nudgeLeft = 0;
    if (newColour) nudgeLeft = 3;
    if (nudgeLeft > 0) {
        --nudgeLeft;
        NudgeSky(s);
    } else if (g_handBackAutomatic) {
        // The nudge pinned the sky for a few ticks so it could be rebuilt; this is where
        // "Automatic" gets the weather back. Not if a variant has been picked in the
        // meantime - that is a choice, and the weather does not get to undo it.
        g_handBackAutomatic = false;
        if (s.variant() == 0) SkydomeSetIndex(0);
    }
}

}  // namespace game
}  // namespace tw

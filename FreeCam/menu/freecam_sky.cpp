// freecam_sky.cpp - hold one of the game's skydomes, and paint the sky the menu's colour.
//
// The variant half talks to the game: it reads the skydome list out of the SkyMarshall and
// calls the game's own set_skydome with an id taken from that list, which is the same call
// its weather makes. Nothing is written into the game's memory here at all - the only
// thing this module changes is which entry the game is asked to show.
//
// The colour half draws. The sky's colours live inside a .perm.bin resource that the game's
// resource system loads and hands to its renderer; there is no field outside it to write,
// and an earlier build of this file spent a while proving that by writing to the places
// they were believed to be and producing no change on screen. What is left is the part
// that works: a gradient in the colour the menu chose, over the top of the frame, drawn by
// the menu's renderer (patched/win_menu.cpp). It is a paint, not a shader, and it says so.
#include "freecam_sky.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

#include "freecam_weather.h"   // SkydomeNames, SkydomeSetIndex, SkydomeCurrentMenuIndex
#include "log.h"
#include "menu_core.h"         // tw::SkySettings, the object the menu's rows write

namespace tw {
namespace game {
namespace {

constexpr uintptr_t kSkyMarshall = 0x23a73e0;   // the object weather/skydome already use
constexpr uintptr_t kFadeIn      = 0x9fec;      // float, seconds the change fades in over
constexpr uintptr_t kFadeOut     = 0x9ff0;      // float, and out over

// The game starts every sky change with a thirty second fade. While the override is on, a
// change is the menu's doing and should be visible now, so the fade is cut down to this.
constexpr float kShortFade = 0.20f;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

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

uint8_t* SkyMarshall() {
    const uintptr_t base = Base();
    if (!base) return nullptr;
    uint8_t* obj = reinterpret_cast<uint8_t*>(base + kSkyMarshall);
    if (!Readable(obj, kFadeOut + sizeof(float))) return nullptr;
    return obj;
}

float* FloatAt(uint8_t* p, uintptr_t off) { return reinterpret_cast<float*>(p + off); }

// Hold the chosen skydome. Only when the game has actually moved off it: asking every frame
// would restart the blend every frame, and the sky would sit permanently half way between
// two of them.
void HoldVariant(const tw::SkySettings& s) {
    if (s.variant() <= 0) return;                    // "Automatic": the game decides
    if (!SkyMarshall()) return;
    const int want = s.variant();
    if (want > static_cast<int>(SkydomeNames().size()) - 1) return;
    if (SkydomeCurrentMenuIndex() == want) return;
    SkydomeSetIndex(want);
}

void ShortenFade() {
    uint8_t* sky = SkyMarshall();
    if (!sky || !Readable(sky + kFadeIn, 2 * sizeof(float))) return;
    float* fadeIn = FloatAt(sky, kFadeIn);
    float* fadeOut = FloatAt(sky, kFadeOut);
    if (*fadeIn > kShortFade) *fadeIn = kShortFade;
    if (*fadeOut > kShortFade) *fadeOut = kShortFade;
}

// Once, so the log says which skies the installation actually has - the entries carry their
// id and the name the game gave them, and a list that is empty or has one entry is the
// answer to a report that the picker does nothing.
void LogListOnce() {
    static bool logged = false;
    static int ticks = 0;
    if (logged) return;
    const std::vector<std::string> names = SkydomeNames();
    if (names.size() <= 1) {
        // Five seconds of an override that is on and a list that is empty is not "still
        // loading" any more, and it is the answer to a picker that appears to do nothing.
        if (++ticks == 300) TW_Log("sky: the skydome list is empty - the game has no skies to hold");
        return;
    }
    logged = true;
    TW_Log("sky: %d skydome(s) in the game's list: %s%s%s", static_cast<int>(names.size()) - 1,
           names.size() > 1 ? names[1].c_str() : "",
           names.size() > 2 ? ", " : "", names.size() > 2 ? names[2].c_str() : "");
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
    const std::size_t index = static_cast<std::size_t>(s.variant());
    const std::string name = index < names.size() ? names[index] : std::string("Automatic");
    if (s.strength() <= 0.0f) return name + ", no colour";
    return name + ", " + s.describe() + " painted over the sky";
}

bool SkyPaint(float* r, float* g, float* b, float* alpha) {
    const tw::SkySettings& s = Sky();
    if (!s.enabled() || s.strength() <= 0.0f) return false;
    float tr = 1.0f, tg = 1.0f, tb = 1.0f;
    s.tint(&tr, &tg, &tb);
    // "Natural" is 1,1,1 - a tint that changes nothing. Painting white over the top of the
    // frame would just wash it out, so there is nothing to draw.
    if (tr >= 0.999f && tg >= 0.999f && tb >= 0.999f) return false;
    *r = tr;
    *g = tg;
    *b = tb;
    // 0.80 at full strength: enough to read as a different sky, not so much that the
    // buildings under it lose their shading. The gradient in the renderer takes it down
    // from there.
    *alpha = 0.80f * s.strength();
    return true;
}

bool SkyPaintActive() {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    return SkyPaint(&r, &g, &b, &a);
}

void SkyTick() {
    tw::SkySettings& s = Sky();
    const std::vector<std::string> names = SkydomeNames();
    s.setVariantCount(static_cast<int>(names.size()));

    if (!SkyMarshall()) return;
    if (!s.enabled()) return;

    LogListOnce();
    HoldVariant(s);
    ShortenFade();
}

}  // namespace game
}  // namespace tw

// freecam_weather.cpp - weather and skybox, by direct field writes.
//
// Both systems are plain static objects in the game's .data, reached from the module base
// without any pattern scanning:
//
//   weather      0x23d1af0   the object 0x71780 returns
//   SkyMarshall  0x23a73e0
//
// The weather object's fields are written in the same order and with the same clamps the
// game's own script bindings use (see the table in freecam_weather.h), because those
// bindings are what the game itself calls when a mission changes the weather.
//
// The skydome list is a qArray header at SkyMarshall+0x9fd8: count, capacity, data. Entries
// are 0x60 bytes each, with the id set_skydome matches on at +0, and the entry's own
// "Data\Global\<name>.perm.bin" path at +0x38. The menu reads that name back so it offers
// the skydomes this installation really has - the list is built from the game's own
// skydome configuration, so it is different on a different install.
#include "freecam_weather.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace tw {
namespace game {
namespace {

constexpr uintptr_t kWeatherGlobal = 0x23d1af0;  // the weather object itself
constexpr uintptr_t kWeatherGetter = 0x71780;    // -> that object (the game's accessor)

// Weather fields, as read and written by the bindings listed in freecam_weather.h.
constexpr uintptr_t kWeatherAmount   = 0x34;     // float, current, 0..2
constexpr uintptr_t kWeatherTarget   = 0x38;     // float, target,  0..2
constexpr uintptr_t kWeatherTarget2  = 0x3c;     // float, second target
constexpr uintptr_t kWeatherInterval = 0x40;     // int, seconds; 0 = locked
constexpr uintptr_t kWeatherSaved    = 0x44;     // int, the interval kept while locked
constexpr uintptr_t kWeatherChance   = 0x4c;     // float, 0..1
constexpr uintptr_t kWeatherWetness  = 0x74;     // float, surface wetness override

constexpr float kAmountMax   = 2.0f;             // 0x1405766d1 / 0x14057678e
constexpr float kChanceMax   = 1.0f;             // 0x140576606, the chance clamp
constexpr float kRainAt      = 1.0f;             // weather_is_raining: amount > 1.0

// The SkyMarshall and its skydome list.
constexpr uintptr_t kSkyMarshall   = 0x23a73e0;
constexpr uintptr_t kSkyCount      = 0x9fd8;     // int, qArray count
constexpr uintptr_t kSkyData       = 0x9fe0;     // entry*, qArray data
constexpr uintptr_t kSkyCurrent    = 0x9f94;     // int, current index, -1 = automatic
constexpr uintptr_t kSkyStride     = 0x60;       // bytes per entry
constexpr uintptr_t kSkyId         = 0x00;       // int, the id set_skydome matches
constexpr uintptr_t kSkyName       = 0x38;       // char[40], "Data\Global\NAME.perm.bin"
constexpr int       kSkyNameMax    = 40;
constexpr uintptr_t kSetSkydome    = 0x6d150;    // (this, const int* id)
constexpr int       kSkyMaxEntries = 64;         // sanity cap on the game's count

using WeatherGet = void* (*)();
using SetSkydomeFn = void (*)(void*, const int*);

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

// The weather object. The game's own accessor is used rather than reading the global, so a
// build that moves the object still works as long as it keeps the accessor.
uint8_t* Weather() {
    const uintptr_t base = Base();
    if (!base) return nullptr;
    void* obj = reinterpret_cast<WeatherGet>(base + kWeatherGetter)();
    if (!Readable(obj, kWeatherWetness + sizeof(float))) return nullptr;
    return static_cast<uint8_t*>(obj);
}

uint8_t* SkyMarshall() {
    const uintptr_t base = Base();
    if (!base) return nullptr;
    void* obj = reinterpret_cast<void*>(base + kSkyMarshall);
    if (!Readable(obj, kSkyCurrent + sizeof(int))) return nullptr;
    return static_cast<uint8_t*>(obj);
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float ReadFloat(uint8_t* p, uintptr_t off) { return *reinterpret_cast<float*>(p + off); }
void WriteFloat(uint8_t* p, uintptr_t off, float v) { *reinterpret_cast<float*>(p + off) = v; }
int ReadInt(uint8_t* p, uintptr_t off) { return *reinterpret_cast<int*>(p + off); }
void WriteInt(uint8_t* p, uintptr_t off, int v) { *reinterpret_cast<int*>(p + off) = v; }

// The presets are the interesting points of the 0..2 range. weather_is_raining flips at
// 1.0, so the first three are dry and the last four are visibly wet.
struct Mode {
    const char* name;
    float amount;
};
const Mode kModes[] = {
    { "Clear",      0.0f },
    { "Overcast",   0.4f },
    { "Drizzle",    0.8f },
    { "Light rain", 1.2f },
    { "Rain",       1.5f },
    { "Downpour",   1.8f },
    { "Monsoon",    kAmountMax },
};

char* NameBuffer(uint8_t* entry) { return reinterpret_cast<char*>(entry + kSkyName); }

// "Data\Global\Cloudy.perm.bin" -> "Cloudy". Empty when the entry does not carry a
// recognisable path, which is the caller's cue to fall back to a numbered label.
std::string EntryName(uint8_t* entry) {
    if (!Readable(NameBuffer(entry), kSkyNameMax)) return std::string();
    const char* s = NameBuffer(entry);
    size_t len = 0;
    while (len < kSkyNameMax && s[len]) ++len;
    if (len < 5) return std::string();
    std::string path(s, len);
    const std::string suffix = ".perm.bin";
    if (path.size() <= suffix.size() ||
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::string();
    }
    std::string stem = path.substr(0, path.size() - suffix.size());
    const size_t slash = stem.find_last_of("\\/");
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    return stem;
}

int SkyCount(uint8_t* sky) {
    const int n = ReadInt(sky, kSkyCount);
    if (n < 0 || n > kSkyMaxEntries) return 0;
    return n;
}

uint8_t* SkyEntry(uint8_t* sky, int index) {
    uint8_t** data = reinterpret_cast<uint8_t**>(sky + kSkyData);
    if (!Readable(data, sizeof(uint8_t*))) return nullptr;
    if (!Readable(*data, (index + 1) * kSkyStride)) return nullptr;
    return *data + index * kSkyStride;
}

}  // namespace

// ---- weather ---------------------------------------------------------------------

bool WeatherAvailable() { return Weather() != nullptr; }

std::vector<std::string> WeatherModePresets() {
    std::vector<std::string> out;
    for (const Mode& m : kModes) out.push_back(m.name);
    return out;
}

std::string WeatherApplyMode(const std::string& preset) {
    for (const Mode& m : kModes) {
        if (preset == m.name) return WeatherApplyAmount(m.amount);
    }
    return "Weather: unknown preset " + preset;
}

std::string WeatherApplyAmount(float amount) {
    uint8_t* w = Weather();
    if (!w) return "Weather: not ready";
    const float v = Clamp(amount, 0.0f, kAmountMax);
    // weather_set_amount writes all three, so the change is immediate and there is no
    // transition to sit through.
    WriteFloat(w, kWeatherAmount, v);
    WriteFloat(w, kWeatherTarget, v);
    WriteFloat(w, kWeatherTarget2, v);
    char buf[96];
    std::snprintf(buf, sizeof buf, "Weather: %.2f of %.0f (%s)", v, kAmountMax,
                  v > kRainAt ? "raining" : "dry");
    TW_Log("weather amount=%.2f", v);
    return buf;
}

float WeatherAmount() {
    uint8_t* w = Weather();
    return w ? ReadFloat(w, kWeatherAmount) : 0.0f;
}

std::string WeatherSetChance(float chance) {
    uint8_t* w = Weather();
    if (!w) return "Rain chance: not ready";
    const float v = Clamp(chance, 0.0f, kChanceMax);
    WriteFloat(w, kWeatherChance, v);
    char buf[96];
    std::snprintf(buf, sizeof buf, "Rain chance: %d%%", static_cast<int>(v * 100.0f + 0.5f));
    return buf;
}

float WeatherChance() {
    uint8_t* w = Weather();
    return w ? ReadFloat(w, kWeatherChance) : 0.0f;
}

std::string WeatherSetWetness(float wetness) {
    uint8_t* w = Weather();
    if (!w) return "Wetness: not ready";
    const float v = Clamp(wetness, 0.0f, 1.0f);
    WriteFloat(w, kWeatherWetness, v);
    char buf[96];
    std::snprintf(buf, sizeof buf, "Wetness override: %d%%", static_cast<int>(v * 100.0f + 0.5f));
    TW_Log("weather wetness=%.2f", v);
    return buf;
}

float WeatherWetness() {
    uint8_t* w = Weather();
    return w ? ReadFloat(w, kWeatherWetness) : 0.0f;
}

// Exactly what weather_lock does: the game zeroes the interval and keeps it in the saved
// slot while locked, and puts it back when unlocked.
bool WeatherLocked() {
    uint8_t* w = Weather();
    return w ? ReadInt(w, kWeatherInterval) == 0 : false;
}

std::string WeatherSetLocked(bool locked) {
    uint8_t* w = Weather();
    if (!w) return "Weather lock: not ready";
    if (locked) {
        const int live = ReadInt(w, kWeatherInterval);
        if (live != 0) {
            WriteInt(w, kWeatherSaved, live);
            WriteInt(w, kWeatherInterval, 0);
        }
    } else if (ReadInt(w, kWeatherInterval) == 0) {
        const int saved = ReadInt(w, kWeatherSaved);
        if (saved != 0) WriteInt(w, kWeatherInterval, saved);
    }
    return locked ? "Weather locked: it will not change on its own"
                  : "Weather unlocked: the game changes it again";
}

int WeatherInterval() {
    uint8_t* w = Weather();
    if (!w) return 0;
    // While locked the live field is zero and the real value sits in the saved slot.
    const int live = ReadInt(w, kWeatherInterval);
    return live != 0 ? live : ReadInt(w, kWeatherSaved);
}

std::string WeatherSetInterval(int seconds) {
    uint8_t* w = Weather();
    if (!w) return "Weather interval: not ready";
    if (seconds < 0) seconds = 0;
    if (seconds > 3600) seconds = 3600;
    // The saved slot is the one to write while locked, or unlocking would throw the value
    // away; the live slot is the one the game reads otherwise. weather_randomize_interval
    // writes the live slot unconditionally, which would unlock the weather by accident.
    if (ReadInt(w, kWeatherInterval) == 0) WriteInt(w, kWeatherSaved, seconds);
    else WriteInt(w, kWeatherInterval, seconds);
    char buf[96];
    if (seconds == 0) std::snprintf(buf, sizeof buf, "Weather interval: 0 s (no automatic change)");
    else std::snprintf(buf, sizeof buf, "Weather interval: every %d s", seconds);
    return buf;
}

// ---- skybox ----------------------------------------------------------------------

bool SkydomeAvailable() {
    uint8_t* sky = SkyMarshall();
    return sky && SkyCount(sky) > 0;
}

std::vector<std::string> SkydomeNames() {
    // The menu asks for this every frame, so the names are cached. The list is built once,
    // when the world loads, and does not change afterwards; a different count means the
    // world was reloaded and it is rebuilt.
    static std::vector<std::string> cached;
    static int cachedCount = -1;
    std::vector<std::string> out;
    out.push_back("Automatic");
    uint8_t* sky = SkyMarshall();
    if (!sky) { cached.clear(); cachedCount = -1; return out; }
    const int count = SkyCount(sky);
    if (count == cachedCount && !cached.empty()) {
        out.insert(out.end(), cached.begin(), cached.end());
        return out;
    }
    cached.clear();
    cachedCount = count;
    for (int i = 0; i < count; ++i) {
        uint8_t* entry = SkyEntry(sky, i);
        std::string name = entry ? EntryName(entry) : std::string();
        if (name.empty()) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "Skydome %d", i + 1);
            name = buf;
        }
        // Two entries may share a name; the selector only carries the label, so make it
        // unique rather than pick the wrong one.
        std::string unique = name;
        for (int n = 2; std::find(out.begin() + 1, out.end(), unique) != out.end(); ++n) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%s (%d)", name.c_str(), n);
            unique = buf;
        }
        out.push_back(unique);
    }
    cached.assign(out.begin() + 1, out.end());
    return out;
}

std::string SkydomeSetIndex(int index) {
    uint8_t* sky = SkyMarshall();
    if (!sky) return "Skybox: not ready";
    const int count = SkyCount(sky);
    if (index <= 0) {
        int automatic = -1;
        reinterpret_cast<SetSkydomeFn>(Base() + kSetSkydome)(sky, &automatic);
        TW_Log("skydome automatic");
        return "Skybox: automatic (the weather picks)";
    }
    if (index > count) return "Skybox: that entry is gone - reopen the menu";
    uint8_t* entry = SkyEntry(sky, index - 1);
    if (!entry) return "Skybox: the skydome list moved - reopen the menu";
    const int id = ReadInt(entry, kSkyId);
    reinterpret_cast<SetSkydomeFn>(Base() + kSetSkydome)(sky, &id);
    const std::string name = EntryName(entry);
    TW_Log("skydome id=%d (%s)", id, name.empty() ? "?" : name.c_str());
    return "Skybox: " + (name.empty() ? ("skydome " + std::to_string(index)) : name);
}

int SkydomeCurrentMenuIndex() {
    uint8_t* sky = SkyMarshall();
    if (!sky) return 0;
    const int current = ReadInt(sky, kSkyCurrent);
    if (current < 0 || current >= SkyCount(sky)) return 0;   // -1, or stale after a reload
    return current + 1;
}

}  // namespace game
}  // namespace tw

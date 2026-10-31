// freecam_spawn.cpp - spawn an NPC three metres in front of the player.
//
// Everything here was read out of TriadWars.exe (the 2016 build the menu targets) rather
// than guessed at, because the game is 40 MB of stripped code and a wrong offset is a
// crash. The addresses are all RVAs from the image base:
//
//   0x1b82a0  hash(name) -> uint32      - table-driven CRC, the engine's name hash
//   0x23f2510 the table object          - its address, not its contents
//   0x63deb0  SpawnAtXform(id, hash, matrix, flags)
//   0x3960e0  GetFaction(entity) -> int - reads [resolved + 0xC8]
//   0x5ebbc0  SetFaction(object, id)    - writes [object + 0xC8] and [object + 0xCC]
//
// The call chain is the same one the game's own encounters use at 0x431d9d when they put
// an on-foot unit in the world: hash the property-set name, make an id, call the spawn
// with a transform and flags = 0.
//
// The faction offsets are the interesting part: the game's own script binding
// `get_faction` resolves the entity and reads a 32-bit field at +0xC8, and its
// `set_faction` ends in a call to 0x5ebbc0, which writes exactly that field (and the name
// hash at +0xCC next to it). So "hostile or friendly" is that field: the same value the
// game's AI reads when it decides who is on whose side.
#include "freecam_spawn.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../src/freecam.h"
}
#include "freecam_game.h"  // FreeCamEnsureThread: the spawn runs on that thread
#include "game_hooks.h"   // EnsureThreadAllocator
#include "win/spawn_guard.h"   // the menu's crash-guarded trampoline + its VEH backstop

// The build's stand-in for spawn_guard.S (menu/build_menu.sh) also offers the same guarded
// call with its return value kept: the spawn's whole result is the object it creates.
extern "C" void *tw_spawn_call(void *target, void *a1, void *a2, const void *a3, int flags);
#include "log.h"

namespace tw {
namespace game {
namespace {

// --- addresses -------------------------------------------------------------

constexpr uintptr_t kHashFn        = 0x1b82a0;  // (uint32_t *out, const char *name)
constexpr uintptr_t kIdTable       = 0x23f2510; // the table object itself
constexpr uintptr_t kSpawnAtXform  = 0x63deb0;  // (id, second, matrix, flags)
constexpr uintptr_t kResolveName   = 0x5c7610;  // (idOut, hash) - the encounter path's
constexpr uintptr_t kGetFaction    = 0x3960e0;  // (entity) -> int, 0 when it cannot
constexpr uintptr_t kSetFaction    = 0x5ebbc0;  // (object, int id)
constexpr uintptr_t kPlayerGlobal  = 0x23f25f8; // void *local player
constexpr uintptr_t kPlayerNode    = 0x58;      // player -> node
constexpr uintptr_t kNodeMatrix    = 0x80;      // node -> 4x4 world transform

// The three ways the game turns an entity handle into the object behind it, chosen by
// the tag bits in word[entity+0x4c] (that is the whole of GetFaction before it reads the
// field). The hash handed to the last two is a constant the game keeps at 0x22ee3a4.
constexpr uintptr_t kResolveSimple = 0x1c8ae0;  // (entity, hash)
constexpr uintptr_t kResolveTagged = 0x5c9330;  // (entity, hash)
constexpr uintptr_t kResolveOther  = 0x5b09b0;  // (entity)
constexpr uintptr_t kAnyAspectHash = 0x22ee3a4;

// The faction ids the game's own name table has entries for. It indexes that table with
// the id and does not range-check it, so nothing outside this range is ever passed in.
constexpr int kFactionIdMax = 38;

// --- small helpers ---------------------------------------------------------

uintptr_t Base() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

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
    return true;
}

template <typename Fn>
Fn At(uintptr_t rva) {
    return reinterpret_cast<Fn>(Base() + rva);
}

// The player's own 4x4 world transform, the one the camera follows in follow mode.
bool PlayerTransform(fc_mat *out) {
    const uintptr_t base = Base();
    if (!base || !Readable(reinterpret_cast<void *>(base + kPlayerGlobal), 8)) return false;
    void *player = *reinterpret_cast<void **>(base + kPlayerGlobal);
    if (!Readable(player, 0x60)) return false;
    void *node = *reinterpret_cast<void **>(static_cast<uint8_t *>(player) + kPlayerNode);
    if (!Readable(node, 0xC0)) return false;
    std::memcpy(out->m, static_cast<uint8_t *>(node) + kNodeMatrix, 64);
    return true;
}

// --- the game's own calls --------------------------------------------------

uint32_t HashName(const char *name) {
    uint32_t out = 0;
    using Fn = void (*)(uint32_t *, const char *);
    At<Fn>(kHashFn)(&out, name);
    return out;
}

// How the game's own encounter spawn gets an id from a name: hash -> resolve -> spawn
// (0x431d6f, 0x431d7b, 0x431da0). The resolve starts at 0xffffffff and only replaces it
// when the name is known, so anything but a real id means "that name is not in the game".
bool ResolveName(uint32_t hash, uint32_t *out) {
    uint32_t id = 0;
    using Fn = void *(*)(uint32_t *, const uint32_t *);
    At<Fn>(kResolveName)(&id, &hash);
    if (id == 0 || id == 0xFFFFFFFFu) return false;
    *out = id;
    return true;
}

// The calls that allocate. Going through the guarded trampoline arms tw_in_spawn, so the
// VEH handler in win_menu.cpp recognises a fault inside the spawn tree and lands back here
// (g_spawnFaulted set) instead of taking the game down with it.
void *GuardedCall(uintptr_t target, void *a1, void *a2, const void *a3, int flags) {
    g_spawnFaulted = 0;
    void *result = tw_spawn_call(reinterpret_cast<void *>(Base() + target), a1, a2, a3, flags);
    return g_spawnFaulted ? nullptr : result;
}


void *SpawnAtXform(uint32_t id, uint32_t second, const fc_mat &where, bool *faulted) {
    if (faulted) *faulted = false;
    // The last two arguments are the ones the game leaves empty for an ordinary spawn,
    // and flags 0 is what its own encounter spawn passes at 0x431d9a.
    void *npc = GuardedCall(kSpawnAtXform, &id, &second, where.m, 0);
    if (g_spawnFaulted && faulted) *faulted = true;
    return npc;
}

// Not every pointer the game hands back is an entity handle. The four tag bits in
// word[+0x4c] are what the game itself looks at before it resolves one, so a pointer
// without any of them is left alone: the faction is then the prefab's own, and the log
// says so. A no-op is much better than a read through the wrong object.
bool LooksLikeEntity(const void *p) {
    if (!Readable(p, 0x50)) return false;
    const unsigned tag = *reinterpret_cast<const unsigned *>(
        static_cast<const uint8_t *>(p) + 0x4c) & 0xFFFFu;
    return (tag & 0xF000u) != 0;
}

// entity handle -> the object the game keeps the faction on. This is GetFaction's own
// prologue, minus the read, so that a write lands on exactly the object it would read.
void *ResolveEntity(void *entity) {
    if (!LooksLikeEntity(entity)) return nullptr;
    const unsigned tag = *reinterpret_cast<unsigned *>(
        static_cast<uint8_t *>(entity) + 0x4c) & 0xFFFFu;
    const uint32_t any = *reinterpret_cast<const uint32_t *>(Base() + kAnyAspectHash);
    using Simple = void *(*)(void *, uint32_t);
    using Other = void *(*)(void *);
    if (tag & 0x4000) return At<Other>(kResolveOther)(entity);
    if (tag & 0x8000) return At<Other>(kResolveOther)(entity);
    if (tag & 0x2000) return At<Simple>(kResolveTagged)(entity, any);
    if (tag & 0x1000) return At<Simple>(kResolveTagged)(entity, any);
    return At<Simple>(kResolveSimple)(entity, any);
}

// The entity arrives in the *second* argument: the game's own binding calls it as
// GetFaction(world, entity), and the function reads rdx. The first argument is untouched,
// so null is fine there.
int GetFaction(void *entity) {
    if (!LooksLikeEntity(entity)) return 0;
    using Fn = int (*)(void *, void *);
    return At<Fn>(kGetFaction)(nullptr, entity);
}

// Writes the faction the way the game's set_faction binding does. The object has to be
// the resolved one, and the id has to be inside the name table, so both are checked
// before anything is touched: the game's lookup at 0x3a47d0 indexes a table with the id
// and does not bound it.
bool SetFaction(void *object, int id) {
    if (!object || !Readable(static_cast<uint8_t *>(object) + 0xC8, 8)) return false;
    if (id < 0 || id > kFactionIdMax) return false;
    using Fn = void (*)(void *, int);
    At<Fn>(kSetFaction)(object, id);
    return *reinterpret_cast<int *>(static_cast<uint8_t *>(object) + 0xC8) == id;
}

// --- the model list -------------------------------------------------------

// The character property sets the executable names by itself. The rest of the game's
// prefabs live in data bundles and cannot be listed from here; FreeCam.npc.txt is how to
// add the ones found by hand.
const char *const kBuiltInModels[] = {
    "object-physical-character-police",
    "object-physical-character-player",
    "object-physical-character-taxi_driver_faceaction",
    "object-physical-character-ThumbnailMan",
};

std::string ExeFolder() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    wchar_t *slash = wcsrchr(path, L'\\');   // the last backslash in the exe's path
    if (!slash) return std::string();
    *slash = 0;
    char narrow[MAX_PATH * 3];
    const int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof narrow, nullptr, nullptr);
    return len > 0 ? std::string(narrow) : std::string();
}

std::vector<std::string> LoadModels() {
    std::vector<std::string> out;
    const std::string folder = ExeFolder();
    if (!folder.empty()) {
        FILE *f = std::fopen((folder + "\\FreeCam.npc.txt").c_str(), "r");
        if (f) {
            char line[256];
            while (std::fgets(line, sizeof line, f)) {
                char *s = line;
                while (*s == ' ' || *s == '\t') s++;
                char *end = s + std::strlen(s);
                while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
                if (!*s || *s == '#') continue;
                if (std::strncmp(s, "object-", 7) != 0) continue;   // a property set, nothing else
                out.push_back(s);
            }
            std::fclose(f);
            if (!out.empty()) {
                TW_Log("Spawn: %d model(s) read from FreeCam.npc.txt", static_cast<int>(out.size()));
                return out;
            }
            TW_Log("Spawn: FreeCam.npc.txt held no property-set names, using the built-in list");
        }
    }
    for (const char *name : kBuiltInModels) out.push_back(name);
    return out;
}

// Loaded once, replaced when the game files are scanned. The revision lets the menu know
// it has to rebuild its list.
std::vector<std::string> g_models;
int g_modelsRev = 0;

const std::vector<std::string> &Models() {
    if (g_models.empty()) g_models = LoadModels();
    return g_models;
}

// --- state ---------------------------------------------------------------

struct Request {
    std::string model;
    bool hostile = false;
    ULONGLONG firstTry = 0;    // when it was asked for, for the retry deadline
    ULONGLONG lastTry = 0;     // when it was last attempted, for the gap between attempts
    int tries = 0;
};

// A spawn asked for before the game is in the world cannot work yet: there is no player
// transform to stand in front of, and the allocator may not be up. Rather than fail at
// once and make the player press the key again, the request is retried for this long.
constexpr ULONGLONG kRetryForMs = 8000;
constexpr ULONGLONG kRetryGapMs = 250;

// The menu and the camera thread both touch the request, so it needs a lock. It is a
// static object rather than a lazily created one so there is no window at all in which
// two threads could both create it.
struct LockBox {
    CRITICAL_SECTION cs;
    LockBox() { InitializeCriticalSection(&cs); }
    ~LockBox() { DeleteCriticalSection(&cs); }
};

LockBox g_box;
bool g_pending = false;
Request g_request;
std::string g_status;
int g_model = 0;
bool g_hostile = false;

void Lock() { EnterCriticalSection(&g_box.cs); }
void Unlock() { LeaveCriticalSection(&g_box.cs); }

// --- the spawn itself ----------------------------------------------------

bool PlayerFaction(int *out) {
    const uintptr_t base = Base();
    if (!base || !Readable(reinterpret_cast<void *>(base + kPlayerGlobal), 8)) return false;
    void *player = *reinterpret_cast<void **>(base + kPlayerGlobal);
    if (!Readable(player, 0x60)) return false;
    if (!LooksLikeEntity(player)) return false;
    const int f = GetFaction(player);
    if (f <= 0 || f > kFactionIdMax) return false;
    *out = f;
    return true;
}

std::string Describe(bool hostile, const std::string &model, int npcFaction, int playerFaction,
                     bool factionSet, const char *note) {
    const std::string shortModel = SpawnShortName(model);
    char buf[160];
    (void)playerFaction;
    (void)factionSet;
    std::snprintf(buf, sizeof buf, "%s %s %s (faction %d)", hostile ? "Hostile" : "Friendly",
                  shortModel.c_str(), note, npcFaction);
    return buf;
}

// One attempt at the game's own spawn, with no C++ objects inside the __try: a fault is
// turned into an outcome the caller can report, and nothing with a destructor is alive
// while the handler runs.
struct Outcome {
    bool faulted = false;
    bool resolved = false;
    bool created = false;
    uint32_t hash = 0;
    uint32_t id = 0;
    uint32_t code = 0;
    void *npc = nullptr;
};

// Returns true when nothing faulted. The whole chain - hash, name lookup, spawn - runs
// under one __try, because a bad address anywhere in it would otherwise take the game
// down, and this is the code that has done that twice.
static bool CallSpawnChain(const char *name, const fc_mat &spot, Outcome *out) {
    __try {
        out->hash = HashName(name);
        out->resolved = ResolveName(out->hash, &out->id);
        if (out->resolved) {
            out->npc = SpawnAtXform(out->id, out->hash, spot, &out->faulted);
            out->created = out->npc != nullptr && !out->faulted;
        }
        return !out->faulted;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->code = GetExceptionCode();
        out->faulted = true;
        out->created = false;
        return false;
    }
}

// Spawning is disabled by this after a fault, and only "try again" turns it back on. A
// mod that keeps calling a function that has already crashed the game once is not being
// useful, it is being stubborn.
bool g_blocked = false;
uint32_t g_blockedCode = 0;

std::string DoSpawn(const std::string &model, bool hostile, bool *retry) {
    if (retry) *retry = false;
    if (model.empty()) return "Spawn failed: no model selected";
    if (g_blocked) {
        char buf[120];
        std::snprintf(buf, sizeof buf,
                      "Spawn is off after a fault (code %08X) - press R to try once more",
                      g_blockedCode);
        return buf;
    }

    fc_mat player, spot;
    if (!PlayerTransform(&player)) {
        if (retry) *retry = true;
        return "Spawn: waiting for you to be in the world";
    }
    if (!fc_spawn_matrix(&spot, &player, 3.0f)) {
        if (retry) *retry = true;
        return "Spawn: waiting for a usable transform";
    }

    if (!EnsureThreadAllocator()) {
        if (retry) *retry = true;
        return "Spawn: waiting for the game's allocator";
    }

    Outcome out;
    CallSpawnChain(model.c_str(), spot, &out);

    TW_Log("Spawn: '%s' hash=%08X resolved=%d id=%08X -> %p faulted=%d code=%08X at %.2f %.2f %.2f",
           model.c_str(), out.hash, out.resolved ? 1 : 0, out.id, out.npc,
           out.faulted ? 1 : 0, out.code, spot.m[12], spot.m[13], spot.m[14]);

    if (out.faulted) {
        g_blocked = true;
        g_blockedCode = out.code;
        char buf[140];
        std::snprintf(buf, sizeof buf,
                      "Spawn crashed (code %08X) - spawner switched off, see the log", out.code);
        return buf;
    }
    if (!out.resolved) {
        return "Spawn failed: this build does not know '" + SpawnShortName(model) + "'";
    }

    // Nothing appeared three metres ahead. That spot can be refused - a doorway, a wall,
    // the middle of the road - so the same call is tried once more at the player's own
    // position, which is always somewhere the game already agreed a character can be. It
    // is one extra attempt and only when the first made nothing, so a working spawn is
    // still exactly one NPC.
    const char *where = "3 m ahead";
    if (!out.created) {
        TW_Log("Spawn: nothing placed 3 m ahead, trying the player's own position");
        Outcome again;
        CallSpawnChain(model.c_str(), player, &again);
        TW_Log("Spawn: '%s' at the player -> %p faulted=%d code=%08X at %.2f %.2f %.2f",
               model.c_str(), again.npc, again.faulted ? 1 : 0, again.code, player.m[12],
               player.m[13], player.m[14]);
        if (again.faulted) {
            g_blocked = true;
            g_blockedCode = again.code;
            char buf[140];
            std::snprintf(buf, sizeof buf,
                          "Spawn crashed (code %08X) - spawner switched off, see the log",
                          again.code);
            return buf;
        }
        if (again.created) {
            out = again;
            where = "on you";
        }
    }
    if (!out.created) {
        return "Spawn failed: the game made no object for '" + SpawnShortName(model) +
               "' (3 m ahead or on you)";
    }

    void *npc = out.npc;

    // Faction: the same field the game's get_faction reads and set_faction writes.
    int npcFaction = GetFaction(npc);
    int playerFaction = 0;
    const bool havePlayer = PlayerFaction(&playerFaction);
    bool factionSet = false;
    // "3 m ahead" or "on you", with the reason when the side could not be written.
    std::string note = where;

    if (npcFaction > 0 && npcFaction <= kFactionIdMax) {
        int want = npcFaction;
        if (hostile) {
            if (havePlayer && npcFaction == playerFaction) {
                want = (playerFaction % kFactionIdMax) + 1;
                if (want == playerFaction) want = (want % kFactionIdMax) + 1;
            }
        } else if (havePlayer && npcFaction != playerFaction) {
            want = playerFaction;
        }

        if (want != npcFaction) {
            const int wasFaction = npcFaction;
            void *object = ResolveEntity(npc);
            if (object && Readable(static_cast<uint8_t *>(object) + 0xC8, 8) &&
                *reinterpret_cast<int *>(static_cast<uint8_t *>(object) + 0xC8) == npcFaction) {
                if (SetFaction(object, want)) {
                    factionSet = true;
                    npcFaction = want;
                } else {
                    note = std::string(where) + ", own side";
                }
            } else {
                note = std::string(where) + ", own side";
            }
            TW_Log("Spawn: faction %d -> %d (yours %d), resolved object %p, wrote %s",
                   wasFaction, want, playerFaction, object, factionSet ? "yes" : "no");
        } else {
            TW_Log("Spawn: faction %d already %s (yours %d)", npcFaction,
                   hostile ? "hostile" : "friendly", playerFaction);
        }
    } else {
        TW_Log("Spawn: the NPC's faction could not be read (%d); its side is the game's default",
               npcFaction);
    }

    return Describe(hostile, model, npcFaction, playerFaction, factionSet, note.c_str());
}


// --- finding NPC models in the game's own files ----------------------------

// The executable names only a handful of character property sets; the rest live inside the
// game's data files, which are opaque bundles. They are not encrypted, though: the names
// are in there as plain strings, the same "object-physical-character-<name>" form the
// executable uses. So instead of guessing names, this walks the game folder once and
// collects every one it can see. It is a read-only scan, bounded in time and bytes, run
// only when the menu asks for it.
const char kCharacterPrefix[] = "object-physical-character-";

bool PlausibleNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

// Collects the names in one buffer. A name has to be long enough to be a real model and
// must not run into the end of the buffer.
void ScanBuffer(const std::vector<unsigned char> &data, std::vector<std::string> *found) {
    const size_t prefix = sizeof kCharacterPrefix - 1;
    if (data.size() <= prefix) return;
    for (size_t i = 0; i + prefix < data.size(); ++i) {
        if (std::memcmp(&data[i], kCharacterPrefix, prefix) != 0) continue;
        size_t j = i + prefix;
        while (j < data.size() && PlausibleNameChar(static_cast<char>(data[j]))) ++j;
        const size_t len = j - (i + prefix);
        if (len >= 3 && len <= 64) {
            std::string name(reinterpret_cast<const char *>(&data[i]), j - i);
            if (std::find(found->begin(), found->end(), name) == found->end()) found->push_back(name);
        }
        i = j ? j - 1 : i;
    }
}

bool ReadWholeFile(const std::wstring &path, std::vector<unsigned char> *out, size_t cap) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > cap) {
        CloseHandle(h);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0, total = 0;
    while (total < out->size() &&
           ReadFile(h, out->data() + total, static_cast<DWORD>(out->size() - total), &got, nullptr) &&
           got) {
        total += got;
    }
    CloseHandle(h);
    out->resize(total);
    return total > 0;
}

// Walks the game folder. Returns how many names it found; writes the list next to the exe
// so the next launch starts with it, and so the player can edit it.
int ScanForModels(std::vector<std::string> *found, unsigned long long *bytesRead, int *filesRead) {
    *bytesRead = 0;
    *filesRead = 0;
    const std::string folder = ExeFolder();
    if (folder.empty()) return 0;

    std::wstring root(folder.begin(), folder.end());   // the folder is ASCII in practice
    if (!root.empty() && root.back() == L'\\') root.pop_back();

    // Bounded: a game data folder is a few gigabytes and this runs a menu action.
    const unsigned long long kByteBudget = 768ull << 20;   // 768 MB in total
    const size_t kFileCap = 96ull << 20;                   // 96 MB per file
    const int kMaxFiles = 4000;

    std::vector<std::wstring> pending{ root };
    std::vector<unsigned char> data;
    std::vector<std::string> names;

    while (!pending.empty() && *filesRead < kMaxFiles && *bytesRead < kByteBudget) {
        std::wstring dir = pending.back();
        pending.pop_back();

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.cFileName[0] == L'.') continue;
                if (fd.cFileName[0] == L'_' || fd.cFileName[0] == L'.') continue;
                pending.push_back(dir + L"\\" + fd.cFileName);
                continue;
            }
            if (*filesRead >= kMaxFiles || *bytesRead >= kByteBudget) break;
            if (fd.nFileSizeHigh != 0 || fd.nFileSizeLow < 32 || fd.nFileSizeLow > kFileCap) continue;

            data.clear();
            if (!ReadWholeFile(dir + L"\\" + fd.cFileName, &data, kFileCap)) continue;
            ++*filesRead;
            *bytesRead += data.size();
            ScanBuffer(data, &names);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::sort(names.begin(), names.end());
    *found = names;
    return static_cast<int>(names.size());
}

}  // namespace

// --- public interface ----------------------------------------------------

std::string SpawnShortName(const std::string &model) {
    static const char kPrefix[] = "object-physical-character-";
    if (model.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0) return model.substr(sizeof(kPrefix) - 1);
    return model;
}

const std::vector<std::string> &SpawnModels() { return Models(); }

int SpawnModelsRevision() { return g_modelsRev; }

std::string SpawnScanGameFiles() {
    std::vector<std::string> found;
    unsigned long long bytes = 0;
    int files = 0;
    const int count = ScanForModels(&found, &bytes, &files);
    TW_Log("Spawn scan: %d file(s), %.1f MB, %d character model(s)", files, bytes / (1024.0 * 1024.0),
           count);
    if (count == 0) {
        return "Scan: no character models found in the game files";
    }

    // Keep the built-ins in front - they are the ones the executable itself vouches for -
    // then everything the scan found, without duplicates.
    std::vector<std::string> merged;
    for (const char *name : kBuiltInModels) merged.push_back(name);
    for (const std::string &name : found) {
        if (std::find(merged.begin(), merged.end(), name) == merged.end()) merged.push_back(name);
    }

    // Written where the next launch reads it, so the list survives a restart and can be
    // trimmed by hand.
    const std::string folder = ExeFolder();
    if (!folder.empty()) {
        FILE *f = std::fopen((folder + "\\FreeCam.npc.txt").c_str(), "w");
        if (f) {
            std::fprintf(f, "# Character models found in the game files by the debug menu.\r\n"
                            "# One property set per line. Lines starting with # are ignored.\r\n");
            for (const std::string &name : merged) std::fprintf(f, "%s\r\n", name.c_str());
            std::fclose(f);
        }
    }

    Lock();
    g_models = merged;
    ++g_modelsRev;
    if (g_model >= static_cast<int>(g_models.size())) g_model = 0;
    const int total = static_cast<int>(g_models.size());
    Unlock();

    char buf[120];
    std::snprintf(buf, sizeof buf, "Scan: %d model(s) (%d new) - N cycles them", total,
                  count);
    TW_Log("Spawn scan: model list is now %d long", total);
    return buf;
}

void SpawnClearFault() {
    g_blocked = false;
    g_blockedCode = 0;
    TW_Log("Spawn: fault cleared, one more attempt allowed");
}

bool SpawnBlocked() { return g_blocked; }

void SpawnSelectByName(const std::string &model) {
    const auto &models = Models();
    for (int i = 0; i < static_cast<int>(models.size()); ++i) {
        if (models[i] == model) {
            g_model = i;
            return;
        }
    }
}

void SpawnSetModel(int index) {
    const int count = static_cast<int>(Models().size());
    if (count <= 0) return;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    g_model = index;
}

int SpawnModelIndex() { return g_model; }

void SpawnSetHostile(bool hostile) { g_hostile = hostile; }
bool SpawnHostile() { return g_hostile; }

void SpawnRequest(const std::string &model, bool hostile) {
    // The spawn is carried out by the camera thread, and that thread is created when the
    // free camera is switched on. Anyone spawning from the menu without ever flying would
    // otherwise queue a request that nothing ever runs - start it here instead.
    const bool thread = FreeCamEnsureThread();
    if (!thread) TW_Log("Spawn: could not start the thread that carries out spawns");

    Lock();
    g_request.model = model;
    g_request.hostile = hostile;
    g_request.firstTry = GetTickCount64();
    g_request.lastTry = 0;
    g_request.tries = 0;
    g_pending = true;
    Unlock();
    TW_Log("Spawn: asked for '%s' (%s), the worker thread %s", model.c_str(),
           hostile ? "hostile" : "friendly", thread ? "is up" : "is NOT up");
}

bool SpawnPump() {
    Lock();
    if (!g_pending) {
        Unlock();
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    // A retry waits a moment between attempts; otherwise the loop would hammer the game
    // with a spawn every millisecond while it is still loading.
    if (g_request.tries > 0 && now - g_request.lastTry < kRetryGapMs) {
        Unlock();
        return false;
    }
    Request request = g_request;
    request.tries = g_request.tries + 1;
    request.lastTry = now;
    g_request = request;
    Unlock();

    bool retry = false;
    const std::string status = DoSpawn(request.model, request.hostile, &retry);

    const bool waiting = retry && (now - request.firstTry) < kRetryForMs;
    Lock();
    if (waiting) {
        // Not ready yet: keep the request queued and say nothing final. The line still
        // reaches the log once, on the transition, so the log does not fill with the same
        // sentence five times a second.
        const bool first = request.tries == 1;
        if (first) TW_Log("Spawn: %s - will keep trying for %llu ms", status.c_str(),
                          static_cast<unsigned long long>(kRetryForMs));
        g_status = status;
        Unlock();
        return first;
    }
    g_pending = false;
    g_status = status;
    Unlock();

    TW_Log("Spawn: %s", status.c_str());
    return true;
}

const std::string &SpawnStatus() { return g_status; }


}  // namespace game
}  // namespace tw

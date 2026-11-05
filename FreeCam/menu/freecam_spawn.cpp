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
#include "freecam_spawn_models.h"   // the built-in model list, rivals included
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

// The names the list starts with live in freecam_spawn_models.h, next to the flag that
// says which of them are rival crews. The rest of the game's prefabs live in data
// bundles and cannot be listed from here; FreeCam.npc.txt is how to add the ones found
// by hand.

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

// The models named in FreeCam.npc.txt, in file order. Empty when the file is not there -
// which is the normal case on a first run, before the scan has written one.
std::vector<std::string> ReadNpcFile() {
    std::vector<std::string> out;
    const std::string folder = ExeFolder();
    if (folder.empty()) return out;
    FILE *f = std::fopen((folder + "\\FreeCam.npc.txt").c_str(), "r");
    if (!f) return out;
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
    return out;
}

// Built-ins first - they are the ones the executable itself vouches for - then the file,
// then whatever the scan found. Duplicates are dropped, so the three sources can overlap
// freely and the list stays one model per line.
std::vector<std::string> MergeModels(const std::vector<std::string> &scanned) {
    std::vector<std::string> out;
    const auto add = [&out](const std::string &name) {
        if (name.size() < 8) return;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    };
    for (const SpawnModelName &model : kSpawnModels) add(model.name);
    for (const std::string &name : ReadNpcFile()) add(name);
    for (const std::string &name : scanned) add(name);
    return out;
}

// --- the prop list -------------------------------------------------------
// A prop is spawned exactly like a character: the game hashes the property set's name, looks
// the hash up, and builds the object at a matrix. What changes is which names are worth
// offering. The executable names a handful of vehicles and weapons (below); the rest are in
// the game's data files, and the same scan that finds the character sets finds these.
const char *const kBuiltInProps[] = {
    "object-physical-vehicle-etalon01",
    "object-physical-vehicle-klienodcop",
    "object-physical-vehicle-klienodcop02",
    "object-physical-vehicle-1100",
    "object-physical-vehicle-270",
    "object-physical-vehicle-425",
    "object-physical-vehicle-600",
    "object-physical-vehicle-625",
    "object-physical-vehicle-860",
    "object-physical-weapon-melee-tireiron",
    "object-physical-weapon-melee-umbrella",
    "object-physical-weapon-melee-cleaver_",
    "object-physical-weapon-firearm-pistol-9mm",
    "object-physical-weapon-firearm-rifle-assault_dlc",
    "object-physical-weapon-firearm-smg-45cal_gold",
};

// FreeCam.props.txt, in file order: the list the scan wrote, which the player can trim.
std::vector<std::string> ReadPropFile() {
    std::vector<std::string> out;
    const std::string folder = ExeFolder();
    if (folder.empty()) return out;
    FILE *f = std::fopen((folder + "\\FreeCam.props.txt").c_str(), "r");
    if (!f) return out;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *end = s + std::strlen(s);
        while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        if (!*s || *s == '#') continue;
        if (std::strncmp(s, "object-", 7) != 0) continue;
        out.push_back(s);
    }
    std::fclose(f);
    return out;
}

std::vector<std::string> MergeProps(const std::vector<std::string> &scanned) {
    std::vector<std::string> out;
    const auto add = [&out](const std::string &name) {
        if (name.size() < 10) return;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    };
    for (const char *name : kBuiltInProps) add(name);
    for (const std::string &name : ReadPropFile()) add(name);
    for (const std::string &name : scanned) add(name);
    return out;
}

std::vector<std::string> LoadProps() { return MergeProps({}); }

std::vector<std::string> LoadModels() {
    const std::vector<std::string> models = MergeModels({});
    if (models.size() > static_cast<std::size_t>(kSpawnModelCount))
        TW_Log("Spawn: %d model(s) from the built-ins and FreeCam.npc.txt",
               static_cast<int>(models.size()));
    return models;
}

// Loaded once, replaced when the game files are scanned. The revision lets the menu know
// it has to rebuild its list.
std::vector<std::string> g_models;
int g_modelsRev = 0;
int g_scannedCount = 0;      // how many of the list came out of the game's data files
std::vector<std::string> g_props;   // the prop list, replaced by a scan
int g_propRev = 0;
int g_scannedProps = 0;
int g_prop = 0;                     // which one G spawns
volatile LONG g_propBusy = 0;       // a prop scan is running
volatile LONG g_scanBusy = 0;    // a background scan is reading the game folder

// The list is replaced by a scan (on the game thread) while the camera thread and the
// menu both read it, so the same lock covers it. A CRITICAL_SECTION is recursive, so a
// reader that already holds it cannot deadlock itself.
void Lock();
void Unlock();

// A copy, never a reference: a background scan can replace the list between the moment it
// is handed out and the moment it is read, and a reference would then be read after the
// vector behind it had been destroyed.
std::vector<std::string> Models() {
    Lock();
    if (g_models.empty()) g_models = LoadModels();
    const std::vector<std::string> copy = g_models;
    Unlock();
    return copy;
}

// The prop list, copied under the same lock as the NPC one and for the same reason.
std::vector<std::string> Props() {
    Lock();
    if (g_props.empty()) g_props = LoadProps();
    const std::vector<std::string> copy = g_props;
    Unlock();
    return copy;
}

// --- state ---------------------------------------------------------------

struct Request {
    std::string model;
    bool hostile = false;
    float metres = 3.0f;       // how far in front; 0 puts the NPC where the player stands
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

std::string DoSpawn(const std::string &model, bool hostile, float metres, bool *retry) {
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
    if (metres > 0.05f && !fc_spawn_matrix(&spot, &player, metres)) {
        if (retry) *retry = true;
        return "Spawn: waiting for a usable transform";
    }
    if (metres <= 0.05f) spot = player;  // on the player: no offset at all

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

    // Nothing appeared where it was asked for. That spot can be refused - a doorway, a wall,
    // the middle of the road - so the same call is tried once more at the player's own
    // position, which is always somewhere the game already agreed a character can be. It
    // is one extra attempt and only when the first made nothing, so a working spawn is
    // still exactly one NPC.
    char whereBuf[32];
    std::snprintf(whereBuf, sizeof whereBuf, "%.0f m ahead", static_cast<double>(metres));
    const char *where = metres > 0.05f ? whereBuf : "on you";
    if (!out.created && metres > 0.05f) {
        TW_Log("Spawn: nothing placed %s, trying the player's own position", where);
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
               "' (" + where + ")";
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


// --- spawning a prop -------------------------------------------------------
// The same three calls as a character, without the faction half: a crate has no side, and
// writing one into a weapon or a vehicle would be writing into whatever field happens to be
// at that offset. The fault switch is this module's own, so a prop that misbehaves leaves
// the NPC spawner working.
bool g_propBlocked = false;
uint32_t g_propBlockedCode = 0;
bool g_propPending = false;
Request g_propRequest;
std::string g_propStatus;

std::string ShortPropName(const std::string &model) {
    static const char *const kPrefixes[] = {
        "object-physical-prop-", "object-physical-vehicle-", "object-physical-weapon-",
        "object-logical-collectibles-", "object-physical-", "object-",
    };
    for (const char *prefix : kPrefixes) {
        const size_t len = std::strlen(prefix);
        if (model.compare(0, len, prefix) == 0) return model.substr(len);
    }
    return model;
}

std::string DoSpawnProp(const std::string &model, float metres, bool *retry) {
    if (retry) *retry = false;
    if (model.empty()) return "Prop: no model selected";
    if (g_propBlocked) {
        char buf[120];
        std::snprintf(buf, sizeof buf,
                      "Prop spawn is off after a fault (code %08X) - press R to try once more",
                      g_propBlockedCode);
        return buf;
    }

    fc_mat player, spot;
    if (!PlayerTransform(&player)) {
        if (retry) *retry = true;
        return "Prop: waiting for you to be in the world";
    }
    if (metres > 0.05f && !fc_spawn_matrix(&spot, &player, metres)) {
        if (retry) *retry = true;
        return "Prop: waiting for a usable transform";
    }
    if (metres <= 0.05f) spot = player;

    if (!EnsureThreadAllocator()) {
        if (retry) *retry = true;
        return "Prop: waiting for the game's allocator";
    }

    Outcome out;
    CallSpawnChain(model.c_str(), spot, &out);
    TW_Log("Prop: '%s' hash=%08X resolved=%d id=%08X -> %p faulted=%d code=%08X at %.2f %.2f %.2f",
           model.c_str(), out.hash, out.resolved ? 1 : 0, out.id, out.npc, out.faulted ? 1 : 0,
           out.code, spot.m[12], spot.m[13], spot.m[14]);

    if (out.faulted) {
        g_propBlocked = true;
        g_propBlockedCode = out.code;
        char buf[140];
        std::snprintf(buf, sizeof buf,
                      "Prop spawn crashed (code %08X) - prop spawner off, see the log", out.code);
        return buf;
    }
    if (!out.resolved) {
        return "Prop failed: this build does not know '" + ShortPropName(model) + "'";
    }

    char whereBuf[32];
    std::snprintf(whereBuf, sizeof whereBuf, "%.0f m ahead", static_cast<double>(metres));
    const char *where = metres > 0.05f ? whereBuf : "on you";
    if (!out.created && metres > 0.05f) {
        TW_Log("Prop: nothing placed %s, trying the player's own position", where);
        Outcome again;
        CallSpawnChain(model.c_str(), player, &again);
        if (again.faulted) {
            g_propBlocked = true;
            g_propBlockedCode = again.code;
            char buf[140];
            std::snprintf(buf, sizeof buf,
                          "Prop spawn crashed (code %08X) - prop spawner off, see the log",
                          again.code);
            return buf;
        }
        if (again.created) {
            out = again;
            where = "on you";
        }
    }
    if (!out.created) {
        return "Prop failed: the game made no object for '" + ShortPropName(model) + "' (" +
               where + ")";
    }
    return "Spawned " + ShortPropName(model) + " " + where;
}

// --- finding NPC models in the game's own files ----------------------------

// The executable names only a handful of character property sets; the rest live inside the
// game's data files, which are opaque bundles. They are not encrypted, though: the names
// are in there as plain strings, the same "object-physical-character-<name>" form the
// executable uses. So instead of guessing names, this walks the game folder once and
// collects every one it can see. It is a read-only scan, bounded in time and bytes, run
// only when the menu asks for it.
const char kCharacterPrefix[] = "object-physical-character-";
// Everything the game can put in the world is an object-* property set; the character ones
// are the NPC spawner's list and the rest are this file's props. Scanning for the shorter
// "object-" prefix once and splitting afterwards costs one walk instead of two.
const char kObjectPrefix[] = "object-";

bool PlausibleNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

// Collects the names in one buffer. A name has to be long enough to be a real model and
// must not run into the end of the buffer.
// The big archives are named files in a table that sits at one end of them, and they are far
// too large to read whole. So a big file is read from both ends instead: a table at the front
// is found by the head, one at the back by the tail, and the several gigabytes in between -
// the models and textures themselves - never have to be touched.
bool ReadChunk(const std::wstring &path, unsigned long long offset, size_t len,
               std::vector<unsigned char> *out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return false;
    }
    out->resize(len);
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

// The same names again as UTF-16: some of the game's tables are wide, and a name stored
// that way is invisible to a byte scan because of the zero after every letter.
void ScanWide(const std::vector<unsigned char> &data, const char *prefixText,
              std::vector<std::string> *found) {
    const size_t prefix = std::strlen(prefixText);
    for (int align = 0; align < 2; ++align) {
        for (size_t i = static_cast<size_t>(align); i + 2 * prefix < data.size(); i += 2) {
            bool hit = true;
            for (size_t k = 0; k < prefix && hit; ++k) {
                if (data[i + 2 * k] != static_cast<unsigned char>(prefixText[k]) ||
                    data[i + 2 * k + 1] != 0)
                    hit = false;
            }
            if (!hit) continue;
            std::string name(prefixText, prefix);
            for (size_t j = i + 2 * prefix; j + 1 < data.size() && data[j + 1] == 0; j += 2) {
                if (!PlausibleNameChar(static_cast<char>(data[j]))) break;
                name.push_back(static_cast<char>(data[j]));
            }
            if (name.size() - prefix >= 3 && name.size() <= 96 &&
                std::find(found->begin(), found->end(), name) == found->end())
                found->push_back(name);
        }
    }
}

void ScanBuffer(const std::vector<unsigned char> &data, const char *prefixText,
                std::vector<std::string> *found) {
    const size_t prefix = std::strlen(prefixText);
    if (data.size() <= prefix) return;
    for (size_t i = 0; i + prefix < data.size(); ++i) {
        if (std::memcmp(&data[i], prefixText, prefix) != 0) continue;
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
// A prop, a vehicle or a weapon: the groups the game spawns as objects rather than as
// characters. The executable names a few and the game files name the rest.
bool IsPropName(const std::string &n) {
    if (n.size() < 12 || n.size() > 96) return false;
    if (n.find("-stats-") != std::string::npos) return false;
    if (n.find("Parameters") != std::string::npos) return false;
    const size_t characters = sizeof kCharacterPrefix - 1;
    if (n.compare(0, characters, kCharacterPrefix) == 0) return false;
    static const char *const kGroups[] = {
        "object-physical-prop-", "object-physical-vehicle-", "object-physical-weapon-",
        "object-logical-collectibles-",
    };
    for (const char *group : kGroups) {
        const size_t len = std::strlen(group);
        if (n.compare(0, len, group) == 0) return n.size() > len;
    }
    return false;
}

int ScanForModels(std::vector<std::string> *found, std::vector<std::string> *props,
                  unsigned long long *bytesRead, int *filesRead) {
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
    const auto scanBoth = [&names](const std::vector<unsigned char> &data) {
        ScanBuffer(data, kObjectPrefix, &names);
        ScanWide(data, kObjectPrefix, &names);
    };

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
            if (fd.nFileSizeLow < 32) continue;

            const unsigned long long size =
                (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            const std::wstring path = dir + L"\\" + fd.cFileName;
            data.clear();
            if (size <= kFileCap) {
                if (!ReadWholeFile(path, &data, kFileCap)) continue;
                ++*filesRead;
                *bytesRead += data.size();
                scanBoth(data);
                continue;
            }

            // Too big to read whole: both ends, which is where a name table lives.
            const size_t kPeek = 32ull << 20;              // 32 MB from each end
            if (ReadChunk(path, 0, kPeek, &data)) {
                ++*filesRead;
                *bytesRead += data.size();
                scanBoth(data);
            }
            if (*bytesRead >= kByteBudget) break;
            data.clear();
            if (ReadChunk(path, size - kPeek, kPeek, &data)) {
                *bytesRead += data.size();
                scanBoth(data);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // A property set is not a ped: the executable's own table names things like
    // "...-stats-BaseAttackParameters" that hash to a stat block, not to a character.
    // Anything with a stats/parameters suffix is dropped rather than offered in a list
    // where picking it would spawn nothing.
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::string &n) {
                                   if (n.size() > 96) return true;
                                   if (n.find("-stats-") != std::string::npos) return true;
                                   if (n.find("Parameters") != std::string::npos) return true;
                                   return false;
                               }),
                names.end());
    std::sort(names.begin(), names.end());
    if (props) props->clear();
    found->clear();
    for (const std::string &name : names) {
        if (name.compare(0, sizeof kCharacterPrefix - 1, kCharacterPrefix) == 0) {
            found->push_back(name);
        } else if (props && IsPropName(name)) {
            props->push_back(name);
        }
    }
    return static_cast<int>(found->size());
}

}  // namespace

// --- public interface ----------------------------------------------------

std::string SpawnShortName(const std::string &model) {
    static const char kPrefix[] = "object-physical-character-";
    if (model.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0) return model.substr(sizeof(kPrefix) - 1);
    return model;
}

// Which of the built-in names are rival crews. The menu asks this about the name it is
// about to spawn: a triad that arrives friendly is a bystander with a nice jacket.
bool SpawnModelIsEnemy(const std::string &model) {
    for (const SpawnModelName &known : kSpawnModels)
        if (model == known.name) return known.enemy;
    return false;
}

// How many rival crews the built-in table holds, for the menu's own line. Counted rather
// than written down, so the row cannot drift away from the list above it.
int SpawnEnemyModelCount() {
    int count = 0;
    for (const SpawnModelName &known : kSpawnModels)
        if (known.enemy) ++count;
    return count;
}

std::vector<std::string> SpawnModels() { return Models(); }

int SpawnModelsRevision() { return g_modelsRev; }

int SpawnModelCount() { return static_cast<int>(Models().size()); }

int SpawnScannedModelCount() {
    Lock();
    const int n = g_scannedCount;
    Unlock();
    return n;
}

bool SpawnScanRunning() { return InterlockedCompareExchange(&g_scanBusy, 1, 1) == 1; }

// Defined further down, next to the scanner it drives.
std::string SpawnScanGameFiles();

// The scan reads a gigabyte of game data, so it runs on its own thread rather than inside
// a menu frame. It touches no game memory - only files and this module's own list - which
// is why it is safe to start it before the world has loaded.
struct ScanThreadArg {
    bool report;
};

DWORD WINAPI ScanThread(LPVOID arg) {
    const bool report = arg && static_cast<ScanThreadArg *>(arg)->report;
    delete static_cast<ScanThreadArg *>(arg);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const std::string line = SpawnScanGameFiles();
    if (report) {
        Lock();
        g_status = line;
        Unlock();
    }
    InterlockedExchange(&g_scanBusy, 0);
    return 0;
}

bool SpawnScanAsync() {
    // Claim the flag first: two callers in the same frame cannot both start a scan.
    if (InterlockedCompareExchange(&g_scanBusy, 1, 0) != 0) return false;
    ScanThreadArg *arg = new ScanThreadArg{ false };
    HANDLE h = CreateThread(nullptr, 0, ScanThread, arg, 0, nullptr);
    if (!h) {
        delete arg;
        InterlockedExchange(&g_scanBusy, 0);
        return false;
    }
    CloseHandle(h);
    TW_Log("Spawn scan: started in the background");
    return true;
}

std::string SpawnScanGameFiles() {
    std::vector<std::string> found, foundProps;
    unsigned long long bytes = 0;
    int files = 0;
    const int count = ScanForModels(&found, &foundProps, &bytes, &files);
    TW_Log("Spawn scan: %d file(s), %.1f MB, %d character model(s)", files, bytes / (1024.0 * 1024.0),
           count);
    if (count == 0) {
        return "Scan: no character models found in the game files";
    }

    // Built-ins first, then the file, then everything the scan found, without duplicates.
    const std::vector<std::string> merged = MergeModels(found);
    {
        Lock();
        g_scannedCount = count;
        Unlock();
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

    // The same walk found the props and the vehicles. They are a separate list, written
    // next to the NPC one so it can be trimmed by hand in the same way.
    const std::vector<std::string> propList = MergeProps(foundProps);
    if (!folder.empty()) {
        FILE *pf = std::fopen((folder + "\\FreeCam.props.txt").c_str(), "w");
        if (pf) {
            std::fprintf(pf, "# Props and objects found in the game files by the debug menu.\r\n"
                             "# One property set per line. Lines starting with # are ignored.\r\n");
            for (const std::string &name : propList) std::fprintf(pf, "%s\r\n", name.c_str());
            std::fclose(pf);
        }
    }
    Lock();
    g_props = propList;
    ++g_propRev;
    if (g_prop >= static_cast<int>(g_props.size())) g_prop = 0;
    const int propTotal = static_cast<int>(g_props.size());
    g_scannedProps = static_cast<int>(foundProps.size());
    Unlock();

    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "NPC models: %d (%d scanned), props: %d", total, count, propTotal);
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
    const auto models = Models();
    for (int i = 0; i < static_cast<int>(models.size()); ++i) {
        if (models[i] == model) {
            g_model = i;
            return;
        }
    }
}

void SpawnSetModel(int index) {
    const auto models = Models();
    const int count = static_cast<int>(models.size());
    if (count <= 0) return;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    g_model = index;
    // N walks the same list the menu offers, so the same rule applies to it: landing on a
    // rival crew means the next spawn is hostile. One direction only - H is still the
    // switch, and walking on to a civilian does not undo a side chosen on purpose.
    if (SpawnModelIsEnemy(models[index])) g_hostile = true;
}

int SpawnModelIndex() { return g_model; }

void SpawnSetHostile(bool hostile) { g_hostile = hostile; }
bool SpawnHostile() { return g_hostile; }

void SpawnRequest(const std::string &model, bool hostile) { SpawnRequestAt(model, hostile, 3.0f); }

void SpawnRequestAt(const std::string &model, bool hostile, float metres) {
    // The spawn is carried out by the camera thread, and that thread is created when the
    // free camera is switched on. Anyone spawning from the menu without ever flying would
    // otherwise queue a request that nothing ever runs - start it here instead.
    const bool thread = FreeCamEnsureThread();
    if (!thread) TW_Log("Spawn: could not start the thread that carries out spawns");

    Lock();
    g_request.model = model;
    g_request.hostile = hostile;
    g_request.metres = metres;
    g_request.firstTry = GetTickCount64();
    g_request.lastTry = 0;
    g_request.tries = 0;
    g_pending = true;
    Unlock();
    TW_Log("Spawn: asked for '%s' (%s, %.0f m), the worker thread %s", model.c_str(),
           hostile ? "hostile" : "friendly", static_cast<double>(metres),
           thread ? "is up" : "is NOT up");
}

bool SpawnSpot(float metres, float out[16]) {
    if (!out) return false;
    fc_mat player, spot;
    if (!PlayerTransform(&player)) return false;
    if (metres > 0.05f) {
        if (!fc_spawn_matrix(&spot, &player, metres)) return false;
    } else {
        spot = player;
    }
    std::memcpy(out, spot.m, sizeof(float) * 16);
    return true;
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
    const std::string status = DoSpawn(request.model, request.hostile, request.metres, &retry);

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

std::string SpawnStatusCopy() {
    Lock();
    const std::string copy = g_status;
    Unlock();
    return copy;
}

bool SpawnPlayerReady() {
    fc_mat player;
    return PlayerTransform(&player);
}

bool SpawnPending() {
    Lock();
    const bool pending = g_pending;
    Unlock();
    return pending;
}

// --- props: the public half ------------------------------------------------

std::string PropShortName(const std::string &model) { return ShortPropName(model); }

std::vector<std::string> PropModels() { return Props(); }

int PropModelsRevision() { return g_propRev; }

int PropModelCount() { return static_cast<int>(Props().size()); }

int PropScannedPropCount() {
    Lock();
    const int n = g_scannedProps;
    Unlock();
    return n;
}

void PropSetModel(int index) {
    const int count = static_cast<int>(Props().size());
    if (count <= 0) return;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    g_prop = index;
}

int PropModelIndex() { return g_prop; }

void PropSelectByName(const std::string &model) {
    const auto props = Props();
    for (int i = 0; i < static_cast<int>(props.size()); ++i) {
        if (props[i] == model) {
            g_prop = i;
            return;
        }
    }
}

void PropClearFault() {
    g_propBlocked = false;
    g_propBlockedCode = 0;
    TW_Log("Prop: fault cleared, one more attempt allowed");
}

bool PropBlocked() { return g_propBlocked; }

void PropRequestAt(const std::string &model, float metres) {
    const bool thread = FreeCamEnsureThread();
    if (!thread) TW_Log("Prop: could not start the thread that carries out spawns");

    Lock();
    g_propRequest.model = model;
    g_propRequest.metres = metres;
    g_propRequest.firstTry = GetTickCount64();
    g_propRequest.lastTry = 0;
    g_propRequest.tries = 0;
    g_propPending = true;
    Unlock();
    TW_Log("Prop: asked for '%s' (%.0f m), the worker thread %s", model.c_str(),
           static_cast<double>(metres), thread ? "is up" : "is NOT up");
}

void PropRequest(const std::string &model) { PropRequestAt(model, 3.0f); }

bool PropPump() {
    Lock();
    if (!g_propPending) {
        Unlock();
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_propRequest.tries > 0 && now - g_propRequest.lastTry < kRetryGapMs) {
        Unlock();
        return false;
    }
    Request request = g_propRequest;
    request.tries = g_propRequest.tries + 1;
    request.lastTry = now;
    g_propRequest = request;
    Unlock();

    bool retry = false;
    const std::string status = DoSpawnProp(request.model, request.metres, &retry);
    const bool waiting = retry && (now - request.firstTry) < kRetryForMs;

    Lock();
    if (waiting) {
        const bool first = request.tries == 1;
        if (first) TW_Log("Prop: %s - will keep trying for %llu ms", status.c_str(),
                          static_cast<unsigned long long>(kRetryForMs));
        g_propStatus = status;
        Unlock();
        return first;
    }
    g_propPending = false;
    g_propStatus = status;
    Unlock();

    TW_Log("Prop: %s", status.c_str());
    return true;
}

std::string PropStatusCopy() {
    Lock();
    const std::string copy = g_propStatus;
    Unlock();
    return copy;
}

bool PropPending() {
    Lock();
    const bool pending = g_propPending;
    Unlock();
    return pending;
}

bool PropScanRunning() { return InterlockedCompareExchange(&g_propBusy, 1, 1) == 1; }

std::string PropScanGameFiles();   // the shared walk, defined above

// The prop half of the scan: the same walk, so it is the same cost as the NPC one and the
// two lists can never disagree about what is in the folder.
struct PropScanArg {
    bool report;
};

DWORD WINAPI PropScanThread(LPVOID arg) {
    const bool report = arg && static_cast<PropScanArg *>(arg)->report;
    delete static_cast<PropScanArg *>(arg);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const std::string line = PropScanGameFiles();
    if (report) {
        Lock();
        g_propStatus = line;
        Unlock();
    }
    InterlockedExchange(&g_propBusy, 0);
    return 0;
}

bool PropScanAsync() {
    if (InterlockedCompareExchange(&g_propBusy, 1, 0) != 0) return false;
    PropScanArg *arg = new PropScanArg{ false };
    HANDLE h = CreateThread(nullptr, 0, PropScanThread, arg, 0, nullptr);
    if (!h) {
        delete arg;
        InterlockedExchange(&g_propBusy, 0);
        return false;
    }
    CloseHandle(h);
    TW_Log("Prop scan: started in the background");
    return true;
}

std::string PropScanGameFiles() {
    std::vector<std::string> found, foundProps;
    unsigned long long bytes = 0;
    int files = 0;
    const int characters = ScanForModels(&found, &foundProps, &bytes, &files);
    TW_Log("Prop scan: %d file(s), %.1f MB, %d character model(s), %d prop(s)", files,
           bytes / (1024.0 * 1024.0), characters, static_cast<int>(foundProps.size()));

    // The NPC list is rebuilt from the same walk, so a prop scan never halves the NPC list.
    const std::vector<std::string> mergedChars = MergeModels(found);
    const std::vector<std::string> mergedProps = MergeProps(foundProps);
    const std::string folder = ExeFolder();
    if (!folder.empty()) {
        FILE *f = std::fopen((folder + "\\FreeCam.props.txt").c_str(), "w");
        if (f) {
            std::fprintf(f, "# Props and objects found in the game files by the debug menu.\r\n"
                            "# One property set per line. Lines starting with # are ignored.\r\n");
            for (const std::string &name : mergedProps) std::fprintf(f, "%s\r\n", name.c_str());
            std::fclose(f);
        }
    }

    Lock();
    g_props = mergedProps;
    ++g_propRev;
    if (g_prop >= static_cast<int>(g_props.size())) g_prop = 0;
    const int propTotal = static_cast<int>(g_props.size());
    g_scannedProps = static_cast<int>(foundProps.size());
    if (mergedChars.size() > g_models.size()) {
        g_models = mergedChars;
        ++g_modelsRev;
    }
    Unlock();

    char buf[160];
    std::snprintf(buf, sizeof buf, "Props: %d in the list, %d read from the game files",
                  propTotal, static_cast<int>(foundProps.size()));
    return buf;
}


}  // namespace game
}  // namespace tw

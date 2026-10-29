// freecam_spawn.cpp - spawn an NPC three metres in front of the player.
//
// Everything here was read out of TriadWars.exe (the 2016 build the menu targets) rather
// than guessed at, because the game is 40 MB of stripped code and a wrong offset is a
// crash. The addresses are all RVAs from the image base:
//
//   0x1b82a0  hash(name) -> uint32      - table-driven CRC, the engine's name hash
//   0x1c8a60  MakeId(table, out, idStr) - the id the spawn call wants
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
constexpr uintptr_t kMakeId        = 0x1c8a60;  // (table, uint32_t *out, const char *id)
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

bool MakeId(uint32_t *out) {
    // The other way the game builds an id: hash the instance name through the id table.
    // Kept as a second shape to try, because the encounter path above and this one have
    // both been seen in this executable.
    static uint32_t counter = 0;
    char id[48];
    std::snprintf(id, sizeof id, "FreeCamNPCSpawn%u", ++counter);
    if (!GuardedCall(kMakeId, reinterpret_cast<void *>(Base() + kIdTable), out, id, 0)) {
        TW_Log("Spawn: the game faulted making an id (see the VEH line above)");
        return false;
    }
    return *out != 0 && *out != 0xFFFFFFFFu;
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

const std::vector<std::string> &Models() {
    static const std::vector<std::string> models = LoadModels();
    return models;
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

// Runs on the camera thread. Everything it touches is the game's own code, in the order
// the game itself uses.
//
// `retry` comes back true for the failures that only mean "not yet" - no player in the
// world, allocator not up - so the caller can try again instead of reporting a failure the
// player would have to fix by pressing the key at a better moment.
std::string DoSpawn(const std::string &model, bool hostile, bool *retry) {
    if (retry) *retry = false;
    if (model.empty()) return "Spawn failed: no model selected";

    fc_mat player, spot;
    if (!PlayerTransform(&player)) {
        if (retry) *retry = true;
        return "Spawn: waiting for you to be in the world";
    }
    if (!fc_spawn_matrix(&spot, &player, 3.0f)) {
        if (retry) *retry = true;
        return "Spawn: waiting for a usable transform";
    }

    // The spawn path allocates through the game's per-thread allocator, which lives in
    // TLS and is only there if the game's own init has run on this thread.
    if (!EnsureThreadAllocator()) {
        if (retry) *retry = true;
        return "Spawn: waiting for the game's allocator";
    }

    const uint32_t hash = HashName(model.c_str());

    // Try the shapes the game itself uses, in order, and stop at the first one that comes
    // back with an object. A spawn that returns nothing created nothing, so a second
    // attempt cannot leave two NPCs behind.
    void *npc = nullptr;
    const char *how = "";
    uint32_t id = 0;
    bool faulted = false;

    if (ResolveName(hash, &id)) {
        npc = SpawnAtXform(id, hash, spot, &faulted);
        how = "resolved name";
    } else {
        TW_Log("Spawn: the game does not know '%s' (hash %08X); no object came back from the "
               "name table", model.c_str(), hash);
    }

    // A fault inside the spawn tree means the game was not ready after all (or that this
    // shape is wrong for this build). Trying the next shape straight away would only fault
    // again, so report it as "not yet" and let the retry loop come back to it.
    if (!npc && !faulted && MakeId(&id)) {
        npc = SpawnAtXform(id, hash, spot, &faulted);
        how = "id from the table";
    }

    if (!npc && !faulted) {
        // Last shape: both arguments the same id, which is what the encounter path hands
        // over when it has no separate type id of its own to give.
        uint32_t again = 0;
        if (ResolveName(hash, &again)) {
            npc = SpawnAtXform(again, again, spot, &faulted);
            how = "resolved name twice";
        }
    }

    if (faulted) {
        if (retry) *retry = true;
        return "Spawn: the game was not ready to spawn yet";
    }

    TW_Log("Spawn: '%s' hash=%08X id=%08X at %.2f %.2f %.2f -> %p (%s)", model.c_str(), hash, id,
           spot.m[12], spot.m[13], spot.m[14], npc, npc ? how : "nothing");

    if (!npc) {
        return "Spawn failed: the game did not create '" + SpawnShortName(model) +
               "' (see TW_MenuLog.txt)";
    }

    // Faction: the same field the game's get_faction reads and set_faction writes.
    int npcFaction = GetFaction(npc);
    int playerFaction = 0;
    const bool havePlayer = PlayerFaction(&playerFaction);
    bool factionSet = false;
    const char *note = "3 m ahead";

    if (npcFaction > 0 && npcFaction <= kFactionIdMax) {
        int want = npcFaction;
        if (hostile) {
            // Different from the player is what "hostile" means here: the game's AI sorts
            // sides out from this id. If the prefab already belongs to another faction,
            // leave it alone - that is its own faction and the right one.
            if (havePlayer && npcFaction == playerFaction) {
                want = (playerFaction % kFactionIdMax) + 1;
                if (want == playerFaction) want = (want % kFactionIdMax) + 1;
            }
        } else if (havePlayer && npcFaction != playerFaction) {
            want = playerFaction;   // same side as the player, so it will not fight you
        }

        if (want != npcFaction) {
            const int wasFaction = npcFaction;
            void *object = ResolveEntity(npc);
            // Only write when the object really is the one the game reads the faction
            // from; if the resolve is not the same object, doing nothing is better than
            // writing into something else.
            if (object && Readable(static_cast<uint8_t *>(object) + 0xC8, 8) &&
                *reinterpret_cast<int *>(static_cast<uint8_t *>(object) + 0xC8) == npcFaction) {
                if (SetFaction(object, want)) {
                    factionSet = true;
                    npcFaction = want;
                } else {
                    note = "3 m ahead, own side";
                }
            } else {
                note = "3 m ahead, own side";
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

    return Describe(hostile, model, npcFaction, playerFaction, factionSet, note);
}

}  // namespace

// --- public interface ----------------------------------------------------

std::string SpawnShortName(const std::string &model) {
    static const char kPrefix[] = "object-physical-character-";
    if (model.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0) return model.substr(sizeof(kPrefix) - 1);
    return model;
}

const std::vector<std::string> &SpawnModels() { return Models(); }

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

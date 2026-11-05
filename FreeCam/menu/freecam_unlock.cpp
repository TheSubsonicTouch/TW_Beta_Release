// freecam_unlock.cpp - make the game's jobs, raids and events reachable.
//
// The menu's "Unlock all" gives you the content: vehicles, weapons, clothing, upgrades and
// top levels. It does not touch the three things that decide whether the world offers you
// anything to *do*:
//
//   1. Every rival's turf is on a post-raid cooldown (state.py, RIVAL_RAID_COOLDOWN). With
//      all seven counting down the world map has nothing to raid.
//
//   2. Jobs are chained: state.py refuses an errand whose links have not been completed
//      ("Raid 2 Rivals" links to "Raid A Rival"), gates the rest behind XP and safehouse
//      levels, and only falls back to the 29 jobs the game data switches off once the
//      enabled ones run out. A fresh profile therefore shows the same three tutorial jobs
//      for a long time.
//
//   3. Nobody has opted into a challenge, so the events screen has nothing running.
//
// The profile gets an UnlockBag.All flag (state.py unlocked()), which opens the whole job
// catalogue in one read, and the bags, levels, rivals, job board and challenge are written
// the way the server keeps them. handlers.py is then touched, which is how the running
// server re-reads both state.json and its own modules.
//
// The decisions live in freecam_unlock_rules.cpp, which has no Windows in it and is unit
// tested; this file is only the file handling.
#include <windows.h>

#include <ctime>
#include <string>
#include <vector>

#include "freecam_unlock.h"
#include "freecam_unlock_rules.h"
#include "json_util.h"
#include "log.h"

namespace tw {
namespace game {
namespace {

using unlock::Catalog;
using unlock::Report;
using unlock::Settings;

std::wstring ToWide(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, 0);
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
    return out;
}

std::string GameDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::wstring p(path);
    auto slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return "";
    p = p.substr(0, slash + 1);
    int len = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, 0);
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, &out[0], len, nullptr, nullptr);
    return out;
}

bool ReadFileText(const std::string &path, std::string &out) {
    HANDLE h = CreateFileW(ToWide(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
    CloseHandle(h);
    return true;
}

bool WriteFileText(const std::string &path, const std::string &text) {
    HANDLE h = CreateFileW(ToWide(path).c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
    return true;
}

void Touch(const std::string &path) {
    HANDLE h = CreateFileW(ToWide(path).c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

// The data file, parsed. A server copied without it is not an error: the profile half of
// the unlock still works.
bool ReadJson(const std::string &path, json::Value &out) {
    std::string text;
    if (!ReadFileText(path, text)) return false;
    return json::parse(text, out);
}

// gamedb_min.json's XP ladder is the game's own: the last entry is the top level.
long long MaxXp(const json::Value &db) {
    const json::Value *xp = db.find("UFG::XPDefinitions");
    if (!xp || !xp->isArray() || xp->array.empty()) return 0;
    const json::Value *levels = xp->array.front().find("mAuthoredXPLevels");
    if (!levels || !levels->isArray() || levels->array.empty()) return 0;
    const json::Value &top = levels->array.back();
    if (top.type == json::Value::Type::Int) return top.integer;
    if (top.type == json::Value::Type::Double) return static_cast<long long>(top.number);
    return 0;
}

// The safehouse's upgrade ladder is the number of racket levels the game has.
int MaxRacketLevel(const json::Value &db) {
    const json::Value *meta = db.find("UFG::MetagameInfoWrapper");
    if (!meta || !meta->isArray() || meta->array.empty()) return 0;
    const json::Value *costs = meta->array.front().find("mSafehouseUpgradeCosts");
    if (!costs || !costs->isArray()) return 0;
    return static_cast<int>(costs->array.size());
}

int ErrandCount(const json::Value &errands) {
    const json::Value *list = errands.find("errands");
    return list && list->isArray() ? static_cast<int>(list->array.size()) : 0;
}

Settings SettingsFromGame(const json::Value &db, const json::Value &challenges) {
    Settings settings;
    settings.maxXp = MaxXp(db);
    settings.maxRacketLevel = MaxRacketLevel(db);
    settings.now = static_cast<long long>(std::time(nullptr));
    const json::Value *days = challenges.find("rotationDays");
    if (days && days->type == json::Value::Type::Int && days->integer > 0)
        settings.rotationSeconds = static_cast<int>(days->integer) * 86400;
    return settings;
}

}  // namespace

std::string UnlockEventsAndActivities(bool content) {
    const std::string dir = GameDir();
    if (dir.empty()) return "Unlock: game folder not found";

    json::Value favourites, stores, db, challenges, errands;
    const bool haveFavourites =
        ReadJson(dir + "LocalServer/data/favour_upgrades.json", favourites);
    const bool haveStores = ReadJson(dir + "LocalServer/data/store_items.json", stores);
    ReadJson(dir + "LocalServer/data/gamedb_min.json", db);
    ReadJson(dir + "LocalServer/data/challenges.json", challenges);
    const bool haveErrands = ReadJson(dir + "LocalServer/data/errands.json", errands);

    const Catalog catalog = unlock::ReadCatalog(favourites, stores);
    const Settings settings = SettingsFromGame(db, challenges);

    Report total;
    int edited = 0;
    bool any = false;

    // The server keeps the profile next to itself (LocalServer/state.json); an older
    // layout kept it in the game folder, so both are written when they exist.
    for (const std::string &path : ProfileFiles()) {
        json::Value root;
        if (!ReadJson(path, root) || !root.isObject()) continue;
        any = true;

        const std::string before = json::dump(root, 1);
        const Report one = unlock::ApplyStateRules(root, catalog, settings, content);
        const std::string after = json::dump(root, 1);
        if (after == before) {
            if (one.wasUnlocked) total.wasUnlocked = true;
            continue;
        }
        if (WriteFileText(path, after)) {
            ++edited;
            total.rivals += one.rivals;
            total.favours += one.favours;
            total.upgrades += one.upgrades;
            total.items += one.items;
            total.board = total.board || one.board;
            total.event = total.event || one.event;
            total.wasUnlocked = one.wasUnlocked;
        }
    }

    if (!any)
        return "Unlock: no profile yet - start the game once, then press this again";

    // The jobs need this build's state.py: an older copy has no idea what UnlockBag is,
    // and the menu would report success while the board stayed on the tutorial's jobs.
    bool serverKnows = false;
    {
        std::string source;
        if (ReadFileText(dir + "LocalServer/state.py", source))
            serverKnows = source.find("UnlockBag") != std::string::npos;
    }

    // The reload: server.py stats these two on every request and re-imports them when the
    // timestamp moves, which also re-reads state.json from disk.
    ReloadServer();

    const int jobs = ErrandCount(errands);
    std::string status = total.wasUnlocked ? "Already unlocked" : "Unlocked";
    status += ": ";
    if (haveErrands && jobs > 0) status += std::to_string(jobs) + " jobs";
    else status += "all jobs";
    status += total.rivals > 0 ? ", " + std::to_string(total.rivals) + " rivals freed"
                               : ", rivals free";
    status += total.event ? ", event on" : ", event running";
    if (content) {
        status += " | +" + std::to_string(total.favours) + " favours, +" +
                  std::to_string(total.upgrades) + " upgrades, +" + std::to_string(total.items) +
                  " items";
    }
    if (!haveFavourites) status += " (no favour data)";
    if (!haveStores) status += " (no store data)";
    if (!serverKnows)
        status += " - LocalServer/state.py is older than this menu, so the jobs stay"
                  " closed until it is updated";

    TW_Log("UnlockEvents: %s (%d file(s) edited, board reset %d)", status.c_str(), edited,
           total.board ? 1 : 0);
    return status;
}

std::string UnlockRaidsAndEvents() {
    return UnlockEventsAndActivities(false);
}

std::string GameFolder() { return GameDir(); }

bool ReadGameFile(const std::string &path, std::string &out) { return ReadFileText(path, out); }

bool WriteGameFile(const std::string &path, const std::string &text) {
    return WriteFileText(path, text);
}

bool DeleteGameFile(const std::string &path) {
    return DeleteFileW(ToWide(path).c_str()) != 0;
}

std::vector<std::string> ProfileFiles() {
    const std::string dir = GameDir();
    if (dir.empty()) return {};
    std::vector<std::string> out;
    for (const char *rel : { "state.json", "LocalServer/state.json" }) {
        HANDLE h = CreateFileW(ToWide(dir + rel).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        CloseHandle(h);
        out.push_back(dir + rel);
    }
    return out;
}

void ReloadServer() {
    const std::string dir = GameDir();
    if (dir.empty()) return;
    Touch(dir + "LocalServer/handlers.py");
}

}  // namespace game
}  // namespace tw

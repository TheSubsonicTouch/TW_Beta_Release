// freecam_unlock.cpp - make raids and events actually reachable.
//
// The menu's "Unlock all" gives you the content: vehicles, weapons, clothing, upgrades and
// top levels. It does not touch the two things that decide whether the world map offers you
// anything to *do*:
//
//   1. Every rival's turf is on a post-raid cooldown (state.py, RIVAL_RAID_COOLDOWN). The
//      client asks for rivals, is told how many seconds each is away, and with all seven
//      counting down the world map has nothing to raid. Clearing cooldown_until puts the
//      whole roster back in play immediately.
//
//   2. Jobs and the events behind them are chained: state.py refuses an errand whose
//      links have not been completed ("Raid 2 Rivals" links to "Raid A Rival"), and with
//      one link only the tutorial's first job is ever on offer. Marking the *prerequisite*
//      errands as completed opens the whole ladder. Only the errands that are somebody
//      else's prerequisite are marked, so the jobs themselves are still there to play -
//      marking every errand would empty the board instead of opening it.
//
// The server keeps the profile in memory, so state.json is edited and handlers.py touched,
// the same way the menu's level and unlock actions already work.
#include <windows.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "freecam_unlock.h"
#include "json_util.h"
#include "log.h"

namespace tw {
namespace game {
namespace {

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

// Every errand uid that some other errand lists as a prerequisite.
std::set<std::string> PrerequisiteErrands(const std::string &dir) {
    std::set<std::string> out;
    std::string text;
    if (!ReadFileText(dir + "LocalServer/data/errands.json", text)) {
        TW_Log("UnlockRaids: LocalServer/data/errands.json not found; job ladder left alone");
        return out;
    }
    json::Value root;
    if (!json::parse(text, root) || !root.isObject()) return out;
    const json::Value *list = root.find("errands");
    if (!list || !list->isArray()) return out;

    for (const json::Value &errand : list->array) {
        if (!errand.isObject()) continue;
        const json::Value *links = errand.find("links");
        if (!links || !links->isArray()) continue;
        for (const json::Value &link : links->array) {
            if (link.isString()) out.insert(link.str);
        }
    }
    return out;
}

}  // namespace

std::string UnlockRaidsAndEvents() {
    const std::string dir = GameDir();
    if (dir.empty()) return "Raids: game folder not found";

    const std::set<std::string> prerequisites = PrerequisiteErrands(dir);
    const char *files[] = { "state.json", "LocalServer/state.json" };
    int edited = 0, rivals = 0, errands = 0;

    for (const char *rel : files) {
        const std::string path = dir + rel;
        std::string text;
        if (!ReadFileText(path, text)) continue;
        json::Value root;
        if (!json::parse(text, root) || !root.isObject()) continue;

        bool changed = false;

        // 1. Every rival available now.
        json::Value *opponents = root.find("opponents");
        if (opponents && opponents->isArray()) {
            for (json::Value &opponent : opponents->array) {
                if (!opponent.isObject()) continue;
                json::Value *cooldown = opponent.find("cooldown_until");
                if (cooldown ? (cooldown->isNumber() && cooldown->integer == 0) : false) continue;
                // Only cooldown_until: state.py pops remaining_seconds itself, it is
                // computed from the timestamp on every read.
                opponent["cooldown_until"].setInt(0);
                ++rivals;
                changed = true;
            }
        }

        // 2. The job ladder: prerequisites only.
        json::Value *player = root.find("player");
        if (player && player->isObject() && !prerequisites.empty()) {
            json::Value &bag = (*player)["ErrandsBag"];
            if (!bag.isObject()) bag.type = json::Value::Type::Object;
            json::Value &completed = bag["Completed"];
            if (!completed.isObject()) completed.type = json::Value::Type::Object;
            for (const std::string &uid : prerequisites) {
                if (completed.find(uid)) continue;
                json::Value &entry = completed[uid];
                entry.type = json::Value::Type::Object;
                entry["Times"].setInt(1);
                entry["Last"].setInt(0);
                ++errands;
                changed = true;
            }
            // Force the board to be rebuilt on the next request.
            bag["Day"].setString("");
            changed = true;
        }

        if (changed && WriteFileText(path, json::dump(root, 1))) ++edited;
    }

    if (edited == 0) return "Raids: state.json not found or unreadable";

    Touch(dir + "LocalServer/handlers.py");

    char buf[160];
    std::snprintf(buf, sizeof buf, "Raids: %d rival(s) free to raid, %d job chain(s) opened",
                  rivals, errands);
    TW_Log("UnlockRaids: %s (%d file(s) edited)", buf, edited);
    return buf;
}

}  // namespace game
}  // namespace tw

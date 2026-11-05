// freecam_wardrobe.cpp - the file handling behind the wardrobe rows.
//
// The decisions (which pieces a wear replaces, what the profile looks like afterwards)
// are in freecam_unlock_rules.cpp and unit tested; this file reads the game's clothing
// table, the server's lookbook and the profile, and writes the result back.
#include "freecam_wardrobe.h"

#include <string>
#include <vector>

#include "freecam_unlock.h"
#include "freecam_unlock_rules.h"
#include "freecam_wardrobe_data.h"
#include "json_util.h"
#include "log.h"

namespace tw {
namespace game {
namespace {

using unlock::WardrobeIndex;
using unlock::WearReport;

// One row of a picker: the name the menu shows and the piece it means.
struct Row {
    std::string name;
    std::string uid;
};

// A named look from LocalServer/data/looks.json: a whole set, one piece per slot.
struct Look {
    std::string name;
    std::vector<std::string> pieces;
};

const char *const kBackupName = "LocalServer/wardrobe-backup.json";

std::vector<Row> g_tops;
std::vector<Row> g_pants;
std::vector<Look> g_looks;
bool g_tablesLoaded = false;

const WardrobeIndex &Index() {
    static WardrobeIndex index;
    if (index.uid.empty()) {
        for (int i = 0; i < wardrobe::kPieceCount; ++i) {
            index.uid.emplace_back(wardrobe::kPieces[i].uid);
            index.slot.emplace_back(wardrobe::kPieces[i].slot);
        }
    }
    return index;
}

void AddRows(const wardrobe::Piece *table, int count, std::vector<Row> &out) {
    for (int i = 0; i < count; ++i) out.push_back(Row{ table[i].name, table[i].uid });
}

// The lookbook is data the server already ships: id, name, theme, blurb and the pieces
// (uid + slot) of each named look. Only the pieces are needed here.
void LoadLooks() {
    std::string text;
    if (!ReadGameFile(GameFolder() + "LocalServer/data/looks.json", text)) return;
    json::Value root;
    if (!json::parse(text, root) || !root.isObject()) return;
    const json::Value *looks = root.find("looks");
    if (!looks || !looks->isArray()) return;
    for (const json::Value &entry : looks->array) {
        if (!entry.isObject()) continue;
        const json::Value *name = entry.find("name");
        const json::Value *pieces = entry.find("pieces");
        if (!name || !name->isString() || name->str.empty()) continue;
        if (!pieces || !pieces->isArray() || pieces->array.empty()) continue;
        Look look;
        look.name = name->str;
        for (const json::Value &piece : pieces->array) {
            if (!piece.isObject()) continue;
            const json::Value *uid = piece.find("uid");
            if (uid && uid->isString() && !uid->str.empty()) look.pieces.push_back(uid->str);
        }
        if (!look.pieces.empty()) g_looks.push_back(std::move(look));
    }
}

void LoadTables() {
    if (g_tablesLoaded) return;
    g_tablesLoaded = true;
    AddRows(wardrobe::kTops, wardrobe::kTopsCount, g_tops);
    AddRows(wardrobe::kPants, wardrobe::kPantsCount, g_pants);
    LoadLooks();
}

const Row *Find(const std::vector<Row> &rows, const std::string &name) {
    for (const Row &row : rows) {
        if (row.name == name) return &row;
    }
    return nullptr;
}

std::string NameOf(const std::string &uid) {
    for (int i = 0; i < wardrobe::kPieceCount; ++i) {
        if (uid == wardrobe::kPieces[i].uid) return wardrobe::kPieces[i].name;
    }
    return std::string();
}

// state.json, parsed. Both layouts are tried, the one the server actually uses first.
bool ReadProfile(std::string &path, json::Value &root) {
    const std::vector<std::string> files = ProfileFiles();
    for (const std::string &file : files) {
        std::string text;
        if (!ReadGameFile(file, text)) continue;
        if (!json::parse(text, root) || !root.isObject()) continue;
        path = file;
        return true;
    }
    return false;
}

// The worn list before the menu first changed it, kept next to the profile so the undo
// works across a restart.
void SaveBackup(const json::Value &root) {
    const json::Value *player = root.find("player");
    if (!player || !player->isObject()) return;
    const json::Value *custom = player->find("CustomizationBag");
    if (!custom || !custom->isObject()) return;
    json::Value backup;
    backup.type = json::Value::Type::Object;
    const json::Value *outfit = custom->find("ClothingOutfitCortexUID");
    const json::Value *list = custom->find("ClothingStoreItemUIDs");
    if (outfit && outfit->isString()) backup["ClothingOutfitCortexUID"].setString(outfit->str);
    json::Value &pieces = backup["ClothingStoreItemUIDs"];
    pieces.type = json::Value::Type::Array;
    if (list && list->isArray()) {
        for (const json::Value &item : list->array) {
            if (!item.isString() || item.str.empty()) continue;
            json::Value copy;
            copy.setString(item.str);
            pieces.array.push_back(std::move(copy));
        }
    }
    WriteGameFile(GameFolder() + kBackupName, json::dump(backup, 1));
}

std::string Apply(const std::vector<std::string> &pieces, const std::string &what) {
    std::string path;
    json::Value root;
    if (!ReadProfile(path, root))
        return "Wardrobe: no profile yet - start the game once, then press this again";

    SaveBackup(root);
    const WearReport report = unlock::WearPieces(root, Index(), pieces);
    if (!report.changed) return "Wardrobe: " + what + " is already what you are wearing";
    if (!WriteGameFile(path, json::dump(root, 1))) return "Wardrobe: could not write " + path;
    ReloadServer();
    TW_Log("Wardrobe: %s -> %d piece(s), %d added to the wardrobe", what.c_str(),
           report.worn, report.owned);
    return "Wardrobe: " + what + " on - " + std::to_string(report.worn) +
           " piece(s); it shows the next time the world loads";
}

// A wear of one slot: read-modify-write with the slot rule, so only that slot moves.
std::string ApplySlot(const std::string &slot, const std::string &piece,
                      const std::string &label) {
    std::string path;
    json::Value root;
    if (!ReadProfile(path, root))
        return "Wardrobe: no profile yet - start the game once, then press this again";

    SaveBackup(root);
    const unlock::WearReport report = unlock::ReplaceWornSlot(root, Index(), slot, piece);
    if (!report.changed) return "Wardrobe: " + label + " is already on";
    if (!WriteGameFile(path, json::dump(root, 1))) return "Wardrobe: could not write " + path;
    ReloadServer();
    TW_Log("Wardrobe: %s on -> %d piece(s), %d added to the wardrobe", label.c_str(),
           report.worn, report.owned);
    return "Wardrobe: " + label + " on - " + std::to_string(report.worn) +
           " piece(s); it shows the next time the world loads";
}

}  // namespace

std::vector<std::string> WardrobeTopNames() {
    LoadTables();
    std::vector<std::string> names;
    for (const Row &row : g_tops) names.push_back(row.name);
    return names;
}

std::vector<std::string> WardrobePantsNames() {
    LoadTables();
    std::vector<std::string> names;
    for (const Row &row : g_pants) names.push_back(row.name);
    return names;
}

std::vector<std::string> WardrobeLookNames() {
    LoadTables();
    std::vector<std::string> names;
    for (const Look &look : g_looks) names.push_back(look.name);
    return names;
}

std::string WardrobeWearTop(const std::string &name) {
    LoadTables();
    const Row *row = Find(g_tops, name);
    if (!row) return "Wardrobe: no top called " + name;
    return ApplySlot("torso", row->uid, name);
}

std::string WardrobeWearPants(const std::string &name) {
    LoadTables();
    const Row *row = Find(g_pants, name);
    if (!row) return "Wardrobe: no pants called " + name;
    return ApplySlot("legs", row->uid, name);
}

std::string WardrobeWearCombo(const std::string &top, const std::string &pants) {
    LoadTables();
    const Row *t = Find(g_tops, top);
    const Row *p = Find(g_pants, pants);
    if (!t && !p) return "Wardrobe: pick a top and a pair of pants first";
    std::vector<std::string> pieces;
    if (t) pieces.push_back(t->uid);
    if (p) pieces.push_back(p->uid);
    std::string what = t && p ? top + " with " + pants : (t ? top : pants);
    return Apply(pieces, what);
}

std::string WardrobeWearLook(const std::string &name) {
    LoadTables();
    for (const Look &look : g_looks) {
        if (look.name == name) return Apply(look.pieces, name);
    }
    return "Wardrobe: no look called " + name;
}

std::string WardrobeUndo() {
    std::string text;
    const std::string backupPath = GameFolder() + kBackupName;
    if (!ReadGameFile(backupPath, text)) return "Wardrobe: nothing to undo yet";
    json::Value backup;
    if (!json::parse(text, backup) || !backup.isObject())
        return "Wardrobe: the backup is unreadable - delete " + backupPath;

    std::string path;
    json::Value root;
    if (!ReadProfile(path, root)) return "Wardrobe: no profile to restore into";
    json::Value *player = root.find("player");
    if (!player || !player->isObject()) return "Wardrobe: the profile has no player";

    json::Value &custom = (*player)["CustomizationBag"];
    if (!custom.isObject()) {
        custom.type = json::Value::Type::Object;
        custom.object.clear();
    }
    const json::Value *outfit = backup.find("ClothingOutfitCortexUID");
    custom["ClothingOutfitCortexUID"].setString(outfit && outfit->isString() ? outfit->str
                                                                           : std::string("0"));
    json::Value &list = custom["ClothingStoreItemUIDs"];
    if (!list.isArray()) {
        list.type = json::Value::Type::Array;
        list.array.clear();
    }
    list.array.clear();
    const json::Value *pieces = backup.find("ClothingStoreItemUIDs");
    int count = 0;
    if (pieces && pieces->isArray()) {
        for (const json::Value &item : pieces->array) {
            if (!item.isString() || item.str.empty()) continue;
            json::Value copy;
            copy.setString(item.str);
            list.array.push_back(std::move(copy));
            ++count;
        }
    }
    if (!WriteGameFile(path, json::dump(root, 1))) return "Wardrobe: could not write " + path;
    ReloadServer();
    DeleteGameFile(backupPath);     // one undo per wear, not a stack of them
    TW_Log("Wardrobe: undone -> %d piece(s)", count);
    return "Wardrobe: back to what you were wearing (" + std::to_string(count) +
           " piece(s))";
}

std::string WardrobeStatus() {
    LoadTables();
    std::string path;
    json::Value root;
    if (!ReadProfile(path, root)) return "no profile yet";
    const json::Value *player = root.find("player");
    const json::Value *custom = player && player->isObject() ? player->find("CustomizationBag")
                                                            : nullptr;
    if (!custom || !custom->isObject()) return "nothing worn";
    const json::Value *list = custom->find("ClothingStoreItemUIDs");
    if (!list || !list->isArray() || list->array.empty()) return "nothing worn";

    std::string torso, legs;
    int count = 0;
    for (const json::Value &item : list->array) {
        if (!item.isString() || item.str.empty()) continue;
        ++count;
        const std::string slot = Index().SlotOf(item.str);
        if (slot == "torso" && torso.empty()) torso = NameOf(item.str);
        if (slot == "legs" && legs.empty()) legs = NameOf(item.str);
    }
    if (count == 0) return "nothing worn";
    std::string text;
    if (!torso.empty() && !legs.empty()) text = torso + ", " + legs;
    else if (!torso.empty()) text = torso;
    else if (!legs.empty()) text = legs;
    if (count > 2) {
        if (!text.empty()) text += " + ";
        text += std::to_string(count - 2) + " more";
    }
    if (text.empty()) text = std::to_string(count) + " pieces";
    return text;
}

bool WardrobeCanUndo() {
    std::string text;
    return ReadGameFile(GameFolder() + kBackupName, text);
}

}  // namespace game
}  // namespace tw

// freecam_unlock_rules.cpp - see the header for what these edit and why.
#include "freecam_unlock_rules.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>

namespace tw {
namespace unlock {
namespace {

bool Contains(const std::vector<std::string> &list, const std::string &value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

std::string StringField(const json::Value &object, const char *key) {
    const json::Value *v = object.find(key);
    if (!v || !v->isString()) return std::string();
    return v->str;
}

long long IntField(const json::Value &object, const char *key, long long fallback = 0) {
    const json::Value *v = object.find(key);
    if (!v) return fallback;
    if (v->type == json::Value::Type::Int) return v->integer;
    if (v->type == json::Value::Type::Double) return static_cast<long long>(v->number);
    return fallback;
}

void CollectStrings(const json::Value &object, const char *key, std::vector<std::string> &out) {
    const json::Value *v = object.find(key);
    if (!v || !v->isArray()) return;
    for (const json::Value &item : v->array) {
        if (item.isString() && !item.str.empty() && !Contains(out, item.str))
            out.push_back(item.str);
    }
}

// The array at `key`, created (and retyped) when it is missing or is something else. A
// profile written by an older server can be missing a whole bag.
json::Value &ArrayAt(json::Value &parent, const char *key) {
    json::Value &slot = parent[key];
    if (!slot.isArray()) {
        slot.type = json::Value::Type::Array;
        slot.array.clear();
    }
    return slot;
}

// A record to add to a bag. json::Value's operator[] adds members without changing the
// value's own type, so a record built that way would serialise as null.
json::Value NewObject() {
    json::Value v;
    v.type = json::Value::Type::Object;
    return v;
}

json::Value &ObjectAt(json::Value &parent, const char *key) {
    json::Value &slot = parent[key];
    if (!slot.isObject()) {
        slot.type = json::Value::Type::Object;
        slot.object.clear();
    }
    return slot;
}

// The highest id already used in a bag, so a new record does not collide with one. Both
// ids are decimal strings in the profile (the client rounds 64-bit JSON numbers).
long long MaxId(const json::Value &bag, const char *key, long long floor) {
    long long highest = floor;
    for (const json::Value &entry : bag.array) {
        if (!entry.isObject()) continue;
        const std::string text = StringField(entry, key);
        if (text.empty()) continue;
        const long long id = std::strtoll(text.c_str(), nullptr, 10);
        if (id > highest) highest = id;
    }
    return highest;
}

bool OwnsString(const json::Value &bag, const char *key, const std::string &value) {
    for (const json::Value &entry : bag.array) {
        if (entry.isObject() && StringField(entry, key) == value) return true;
    }
    return false;
}

// The turfs this profile keeps: the player's own, and each rival's, same shape.
void MaxOutTurf(json::Value &profile, const Settings &settings) {
    json::Value *turfbag = profile.find("TurfBag");
    if (!turfbag || !turfbag->isObject()) return;

    // The safehouse, warehouse and temple carry the build points, so they are the ones
    // that decide how much can be put on the turf.
    json::Value *turf = turfbag->find("Turf");
    if (turf && turf->isObject()) {
        for (const char *building : { "Safehouse", "Warehouse", "Temple" }) {
            json::Value *b = turf->find(building);
            if (b && b->isObject()) (*b)["RacketLevel"].setInt(settings.maxRacketLevel);
        }
    }
    json::Value *rackets = turfbag->find("Rackets");
    if (rackets && rackets->isArray()) {
        for (json::Value &racket : rackets->array) {
            if (racket.isObject()) racket["RacketLevel"].setInt(settings.maxRacketLevel);
        }
    }
}

}  // namespace

Catalog ReadCatalog(const json::Value &favourites, const json::Value &stores) {
    Catalog catalog;

    // favour_upgrades.json: favour definition UID -> {"upgrade": enforcer upgrade UID}.
    if (favourites.isObject()) {
        for (const auto &entry : favourites.object) {
            const json::Value &info = entry.second;
            if (!info.isObject() || entry.first.empty()) continue;
            catalog.favours.push_back(entry.first);
            const std::string upgrade = StringField(info, "upgrade");
            if (!upgrade.empty() && !Contains(catalog.upgrades, upgrade))
                catalog.upgrades.push_back(upgrade);
        }
    }

    // store_items.json: store UID -> {"type", "items": [...]}. An outfit also lists the
    // pieces it is made of; those are store items in their own right (the client puts
    // them in the wardrobe when the outfit is bought), so they are owned too.
    if (stores.isObject()) {
        for (const auto &entry : stores.object) {
            const json::Value &store = entry.second;
            if (!store.isObject()) continue;
            CollectStrings(store, "items", catalog.items);
            const json::Value *pieces = store.find("pieces");
            if (!pieces || !pieces->isObject()) continue;
            for (const auto &piece : pieces->object) {
                const json::Value &list = piece.second;
                if (!list.isArray()) continue;
                for (const json::Value &p : list.array) {
                    if (p.isString() && !p.str.empty() && !Contains(catalog.items, p.str))
                        catalog.items.push_back(p.str);
                }
            }
        }
    }
    return catalog;
}

Report ApplyStateRules(json::Value &state, const Catalog &catalog, const Settings &settings,
                       bool content) {
    Report report;
    if (!state.isObject()) return report;

    json::Value *player = state.find("player");
    if (!player || !player->isObject()) return report;

    // ---- the switch the server reads ---------------------------------------
    // Not one of the client's data bags: State.bags only serialises the bags it knows, so
    // this never reaches the game. state.py reads it to open the whole job catalogue.
    bool alreadyUnlocked = false;
    {
        json::Value &bag = ObjectAt(*player, "UnlockBag");
        const json::Value *all = bag.find("All");
        alreadyUnlocked = all && all->type == json::Value::Type::Bool && all->boolean;
        bag["All"].type = json::Value::Type::Bool;
        bag["All"].boolean = true;
    }
    report.wasUnlocked = alreadyUnlocked;

    // ---- the content --------------------------------------------------------
    if (content) {
        json::Value &data = ObjectAt(*player, "PlayerDataBag");
        if (settings.maxXp > 0) data["EnforcerXP"].setInt(settings.maxXp);
        if (settings.triadLevel > 0) data["TriadLevel"].setInt(settings.triadLevel);
        // Face rank is what the export missions ask for before the map will offer one;
        // it is paid in tens of points, so this is the whole ladder and then some.
        if (settings.maxFace > 0) data["Face"].setString(std::to_string(settings.maxFace));
        MaxOutTurf(*player, settings);

        // Favours: one instance per upgrade favour definition, which is what the favour
        // collector and the enforcer upgrade screen read.
        {
            json::Value &bag = ArrayAt(*player, "FavoursBag");
            long long next = MaxId(bag, "FavourID", 5000) + 1;
            for (const std::string &definition : catalog.favours) {
                if (OwnsString(bag, "FavourDefinitionUID", definition)) continue;
                json::Value entry = NewObject();
                entry["FavourID"].setString(std::to_string(next++));
                entry["SecondsElapsed"].setInt(0);
                entry["TotalSeconds"].setInt(0);
                entry["FavourDefinitionUID"].setString(definition);
                entry["IsNew"].setInt(1);
                bag.array.push_back(std::move(entry));
                ++report.favours;
            }
        }

        // Enforcer upgrades: {Item1: upgrade UID, Item2: equipped slot or null}. Merged,
        // not rebuilt - an upgrade already equipped (or handed over by the raid rewards)
        // keeps its slot.
        {
            json::Value &bag = ArrayAt(*player, "EnforcerUpgradesBag");
            for (const std::string &upgrade : catalog.upgrades) {
                if (OwnsString(bag, "Item1", upgrade)) continue;
                json::Value entry = NewObject();
                entry["Item1"].setString(upgrade);
                entry["Item2"];                  // created as null: owned, not equipped
                bag.array.push_back(std::move(entry));
                ++report.upgrades;
            }
        }

        // Everything the shops sell: clothing, outfits, vehicles, favour packs. The
        // client lists what is in the InventoryBag, so this is what fills the wardrobe,
        // the garage and the collections.
        {
            json::Value &bag = ArrayAt(*player, "InventoryBag");
            long long next = MaxId(bag, "InventoryItemID", 7000) + 1;
            std::string account = StringField(ObjectAt(*player, "PlayerDataBag"), "AccountID");
            if (account.empty()) account = "1";
            for (const std::string &item : catalog.items) {
                if (OwnsString(bag, "StoreItemID", item)) continue;
                json::Value entry = NewObject();
                entry["SecondsElapsed"].setInt(0);
                entry["TotalSeconds"].setInt(0);
                entry["StoreItemID"].setString(item);
                entry["InventoryItemID"].setString(std::to_string(next++));
                entry["AccountID"].setString(account);
                bag.array.push_back(std::move(entry));
                ++report.items;
            }
        }
    }

    // ---- the rivals ---------------------------------------------------------
    // RIVAL_RAID_COOLDOWN leaves a turf counting down after a raid, and the world map
    // offers nothing while it does. Only cooldown_until is written: remaining_seconds is
    // recomputed from it on every read.
    json::Value *opponents = state.find("opponents");
    if (opponents && opponents->isArray()) {
        for (json::Value &opponent : opponents->array) {
            if (!opponent.isObject()) continue;
            if (IntField(opponent, "cooldown_until", 0) == 0) continue;
            opponent["cooldown_until"].setInt(0);
            ++report.rivals;
        }
    }

    // ---- the job board ------------------------------------------------------
    // The board caches its three slots for the day. An empty Day makes state.py's
    // refresh_errands treat the next read as a new day and refill from the now-complete
    // pool; the slots themselves are left alone so a job that is done but unpaid is kept.
    // Only on the first press: the daily rollover already refills from the unlocked pool,
    // and re-clearing it on every press would reroll a board the player is working on.
    if (!alreadyUnlocked) {
        json::Value &bag = ObjectAt(*player, "ErrandsBag");
        bag["Day"].setString("");
        report.board = true;
    }

    // ---- the challenge ("events") --------------------------------------------
    // TrialsBag is state.py's, not a client bag: a window that has run out is replaced by
    // current_challenge itself, and it clears the opt-in when it does. Starting a fresh
    // window here is what makes the events screen show a challenge the player is already
    // taking part in, with its whole rotation ahead of it.
    {
        json::Value &bag = ObjectAt(*player, "TrialsBag");
        const bool running = IntField(bag, "Start", 0) > 0 &&
                             IntField(bag, "End", 0) > settings.now;
        if (!running) {
            // Nothing is running (or the window has closed, which also cleared the
            // opt-in): start one with the whole rotation ahead of it.
            bag["Start"].setInt(settings.now);
            bag["End"].setInt(settings.now + settings.rotationSeconds);
            bag["Index"].setInt(0);
            bag["Progress"].setInt(0);
            ArrayAt(bag, "Rewarded").array.clear();
            bag["Name"].setString("");
            report.event = true;
        }
        if (IntField(bag, "OptedIn", 0) != 1) {
            // Pressing "take part" in the events screen does the same thing
            // (handlers.py trialinstance/optin); this is the same flag, set for you.
            bag["OptedIn"].setInt(1);
            report.event = true;
        }
    }

    return report;
}

// ---- the wardrobe ----------------------------------------------------------------

std::string WardrobeIndex::SlotOf(const std::string &piece) const {
    for (std::size_t i = 0; i < uid.size() && i < slot.size(); ++i) {
        if (uid[i] == piece) return slot[i];
    }
    return std::string();
}

namespace {

// The worn list as plain strings. The client parses these as 64-bit ids, so the profile
// stores them as decimal strings and anything else in the array is dropped.
std::vector<std::string> WornPieces(const json::Value &custom) {
    std::vector<std::string> out;
    const json::Value *list = custom.find("ClothingStoreItemUIDs");
    if (!list || !list->isArray()) return out;
    for (const json::Value &item : list->array) {
        if (!item.isString() || item.str.empty()) continue;
        if (!Contains(out, item.str)) out.push_back(item.str);
    }
    return out;
}

// A piece in the InventoryBag is what makes the wardrobe list it. Bought outfits already
// put their pieces there; a piece worn by this file and never bought does not.
int OwnPiece(json::Value &player, const std::string &piece) {
    json::Value &bag = ArrayAt(player, "InventoryBag");
    if (OwnsString(bag, "StoreItemID", piece)) return 0;
    json::Value entry = NewObject();
    entry["SecondsElapsed"].setInt(0);
    entry["TotalSeconds"].setInt(0);
    entry["StoreItemID"].setString(piece);
    entry["InventoryItemID"].setString(std::to_string(MaxId(bag, "InventoryItemID", 7000) + 1));
    std::string account = StringField(ObjectAt(player, "PlayerDataBag"), "AccountID");
    entry["AccountID"].setString(account.empty() ? std::string("1") : account);
    bag.array.push_back(std::move(entry));
    return 1;
}

// The shared half of both entries: rewrite the list, own what is on it, and stop the last
// bought outfit from being what the client draws over the top.
WearReport WriteWorn(json::Value &player, const std::vector<std::string> &worn,
                     const std::vector<std::string> &pieces) {
    WearReport report;
    json::Value &custom = ObjectAt(player, "CustomizationBag");

    std::vector<std::string> before = WornPieces(custom);
    if (before == worn) return report;

    json::Value &list = ArrayAt(custom, "ClothingStoreItemUIDs");
    list.array.clear();
    for (const std::string &piece : worn) {
        json::Value item;
        item.setString(piece);
        list.array.push_back(std::move(item));
    }
    // "0" is what a profile that has never worn an outfit carries. Leaving a purchased
    // outfit in here would draw that outfit's own pieces on top of the mix.
    custom["ClothingOutfitCortexUID"].setString("0");
    for (const std::string &piece : pieces) report.owned += OwnPiece(player, piece);

    report.changed = true;
    report.worn = static_cast<int>(worn.size());
    return report;
}

}  // namespace

WearReport ReplaceWornSlot(json::Value &state, const WardrobeIndex &index,
                           const std::string &slot, const std::string &piece) {
    WearReport report;
    if (!state.isObject() || slot.empty()) return report;
    json::Value *player = state.find("player");
    if (!player || !player->isObject()) return report;

    const std::vector<std::string> before = WornPieces(ObjectAt(*player, "CustomizationBag"));
    std::vector<std::string> worn;
    for (const std::string &w : before) {
        // A piece this build cannot place is kept: losing the hat because the table has
        // no entry for it would be worse than leaving it on.
        if (!index.SlotOf(w).empty() && index.SlotOf(w) == slot) continue;
        worn.push_back(w);
    }
    if (!piece.empty()) {
        if (!Contains(worn, piece)) worn.push_back(piece);
    }
    return WriteWorn(*player, worn, piece.empty() ? std::vector<std::string>()
                                                  : std::vector<std::string>{ piece });
}

WearReport WearPieces(json::Value &state, const WardrobeIndex &index,
                      const std::vector<std::string> &pieces) {
    WearReport report;
    if (!state.isObject() || pieces.empty()) return report;
    json::Value *player = state.find("player");
    if (!player || !player->isObject()) return report;

    // Each slot the set covers is replaced; the hat, the shoes and the rest stay.
    std::vector<std::string> replaced;
    for (const std::string &piece : pieces) {
        const std::string slot = index.SlotOf(piece);
        if (!slot.empty() && !Contains(replaced, slot)) replaced.push_back(slot);
    }
    const std::vector<std::string> before = WornPieces(ObjectAt(*player, "CustomizationBag"));
    std::vector<std::string> worn;
    for (const std::string &piece : before) {
        if (Contains(replaced, index.SlotOf(piece))) continue;
        worn.push_back(piece);
    }
    for (const std::string &piece : pieces) {
        if (!Contains(worn, piece)) worn.push_back(piece);
    }
    return WriteWorn(*player, worn, pieces);
}

}  // namespace unlock
}  // namespace tw

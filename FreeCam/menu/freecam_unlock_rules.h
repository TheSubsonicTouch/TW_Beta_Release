// freecam_unlock_rules.h - what "unlock everything" writes into the profile, as plain
// functions on JSON.
//
// The menu's Unlock all content entry used to stop at the content (levels, vehicles,
// clothing, upgrades). The things that decide whether the game has anything to *do* were
// left as the tutorial left them:
//
//   * every rival's turf counting down its post-raid cooldown, so the world map offers
//     nothing to raid (state.py, RIVAL_RAID_COOLDOWN);
//   * a job board that hands out three jobs a day from a pool the story ladder has barely
//     opened (state.py, errand_pool: level gates, mErrandLinks, the 29 jobs the game data
//     switches off, the two behind the export chain);
//   * a challenge nobody has opted into, so the events screen has nothing running.
//
// This file is the part that decides what a profile looks like afterwards. It is kept
// away from the file handling (freecam_unlock.cpp) because it is the part that can be
// wrong quietly - a bag with the wrong shape, a job board that never rebuilds - and a
// unit test on a build machine can check it with no game, no Windows and no server.
//
// Everything here is idempotent: pressing the entry twice leaves the profile as the first
// press left it.
#pragma once

#include <string>
#include <vector>

#include "json_util.h"

namespace tw {
namespace unlock {

// What the unlock needs to know about the installed game, read from its own data files so
// nothing is hard-coded here.
struct Settings {
    long long maxXp = 0;             // XP of the top level (gamedb_min.json XPDefinitions)
    int maxRacketLevel = 0;          // top safehouse/racket level (MetagameInfoWrapper)
    int triadLevel = 50;             // what the triad screen shows at the top level
    long long now = 0;               // unix time: the challenge's window starts here
    int rotationSeconds = 7 * 86400; // data/challenges.json rotationDays, in seconds
};

// Everything the two catalogues sell, which is what the unlock hands over.
struct Catalog {
    std::vector<std::string> favours;    // favour definition UIDs (favour_upgrades.json keys)
    std::vector<std::string> upgrades;   // enforcer upgrade UIDs (their "upgrade" values)
    std::vector<std::string> items;      // store item UIDs (store_items.json "items")
};

// What the profile ended up with, for the menu's status line.
struct Report {
    int rivals = 0;          // turfs taken off their post-raid cooldown
    int favours = 0;         // favour definitions added to FavoursBag
    int upgrades = 0;        // enforcer upgrades added to EnforcerUpgradesBag
    int items = 0;           // store items added to InventoryBag
    bool board = false;      // the job board was told to rebuild
    bool event = false;      // the challenge was started and opted into
    bool wasUnlocked = false;   // the profile was already unlocked before this press
};

// Reads the two data files into the catalogue. Missing or malformed input gives an empty
// catalogue rather than an error: the profile side still works without it.
Catalog ReadCatalog(const json::Value &favourites, const json::Value &stores);

// Edits one profile (the whole state.json object: {"player": ..., "opponents": [...]}).
// `content` is false for the menu's smaller "raids and events" entry, which opens the
// opportunities but leaves the bags and levels alone.
Report ApplyStateRules(json::Value &state, const Catalog &catalog, const Settings &settings,
                       bool content);

}  // namespace unlock
}  // namespace tw

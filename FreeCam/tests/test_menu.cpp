// test_menu.cpp - checks the menu model without Windows, ImGui or the game.
//
// The menu's contents are decided in menu_core.cpp, which is plain C++: what the entries
// are, what order they come in, what the keys do to them. That is exactly the part that
// goes wrong quietly - an entry left behind after a feature is removed, a section title
// that eats the cursor, an action that never fires - so it is checked here, the same way
// the camera maths is.
//
// Built and run by run_tests.py; any C++17 compiler will do.
#include "../menu/freecam_unlock_rules.h"
#include "../menu/patched/menu_core.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

bool hasLabel(const tw::Menu& menu, const std::string& label) {
    for (const auto& item : menu.items())
        if (item.label == label) return true;
    return false;
}

bool hasHeader(const tw::Menu& menu, const std::string& label) {
    for (const auto& item : menu.items())
        if (item.header && item.label == label) return true;
    return false;
}

// Put the cursor on the entry with this label, the way the player would walk to it.
void selectLabel(tw::Menu& menu, const std::string& label) {
    for (std::size_t i = 0; i < menu.items().size(); ++i) {
        if (menu.items()[i].label == label) {
            menu.select(i);
            return;
        }
    }
    check(false, "no entry called " + label);
}

bool anyLabelContains(const tw::Menu& menu, const std::string& needle) {
    for (const auto& item : menu.items())
        if (item.label.find(needle) != std::string::npos) return true;
    return false;
}

// A full set of hooks: everything this build can actually do.
tw::DebugHooks fullHooks(int* toggles, int* picks, int* levels,
                         std::vector<std::string>* spawned) {
    tw::DebugHooks hooks;
    hooks.weatherPresets = { "Clear", "Rain" };
    hooks.levelTargets = { "Enforcer", "Rackets", "Triad", "Rivals" };
    hooks.shaderPresets = { "cinematic.ini", "gritty.ini" };
    hooks.setWeather = [picks](const std::string& w) { ++*picks; return "Weather: " + w; };
    hooks.setShader = [picks](const std::string& n) { ++*picks; return "Preset: " + n; };
    hooks.setFreeCam = [toggles](bool on) { ++*toggles; return std::string(on ? "on" : "off"); };
    hooks.setNoclip = [toggles](bool on) { ++*toggles; return std::string(on ? "flying" : "landed"); };
    hooks.setLevel = [levels](const std::string& t, int v) {
        ++*levels;
        return t + " " + std::to_string(v);
    };
    hooks.unlockAllContent = [] { return std::string("everything unlocked"); };
    hooks.unlockRaids = [] { return std::string("raids open"); };
    hooks.pedNames = { "object-physical-character-police",
                       "object-physical-character-player" };
    hooks.pedDefault = hooks.pedNames.front();
    hooks.spawnPed = [spawned](const std::string& model) {
        spawned->push_back(model);
        return "Spawn Ped: " + model;
    };
    hooks.noclipSpeeds = { "1x", "2x", "3x", "4x", "6x", "8x" };
    hooks.setNoclipSpeed = [picks](const std::string& s) {
        ++*picks;
        return "Noclip speed: " + s;
    };
    return hooks;
}

void testStructure() {
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels, &spawned));

    const auto lines = menu.renderLines();
    check(!lines.empty() && lines[0] == "TriadMenu", "the menu is titled TriadMenu");

    // What the menu offers, and in which order.
    check(hasLabel(menu, "Free Camera"), "Free Camera is there");
    check(hasLabel(menu, "Noclip (fly the player)"), "noclip is there");
    check(hasLabel(menu, "Noclip speed"), "the noclip speed picker is there");
    check(hasLabel(menu, "Weather"), "weather is there");
    check(hasLabel(menu, "ReShade preset"), "the ReShade preset picker is there");
    check(hasLabel(menu, "Level target"), "the level target is there");
    check(hasLabel(menu, "Level value"), "the level slider is there");
    check(hasLabel(menu, "Apply level"), "Apply level is there");
    check(hasLabel(menu, "Unlock all content"), "Unlock all content is there");
    check(hasLabel(menu, "Unlock raids and events"), "Unlock raids is there");

    check(hasLabel(menu, "Spawn Ped"), "Spawn Ped is in the menu");
    check(hasLabel(menu, "Ped model (list)"), "the ped list is under it");

    check(hasHeader(menu, "CAMERA") && hasHeader(menu, "SPAWN") &&
              hasHeader(menu, "WORLD") && hasHeader(menu, "PROGRESSION"),
          "the four sections are there");

    // What was taken out must stay out: those entries either could not work or were
    // replaced by hotkeys, and a menu that offers them is worse than a short one.
    // The old spawn entries (model/attitude/on-me/scan) were removed in build 17; what is
    // left is the single Spawn Ped entry, so these words must not appear twice.
    const char* gone[] = { "Teleport", "Hide HUD", "Time of", "Invincible", "Reload",
                           "NPC model", "NPC attitude", "on me", "Scan game" };
    for (const char* word : gone) {
        check(!anyLabelContains(menu, word), std::string("no entry mentions ") + word);
    }

    // The status line is empty until something is done.
    check(menu.status().empty(), "no status before the first action");
}

void testCursorSkipsHeaders() {
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels, &spawned));

    // The build leaves the cursor on the first usable entry, not on the CAMERA title.
    check(!menu.items()[menu.selectedIndex()].header, "the cursor starts on an entry");
    check(menu.items()[menu.selectedIndex()].label == "Free Camera",
          "the cursor starts on Free Camera");

    const std::size_t total = menu.items().size();
    int usable = 0;
    for (const auto& item : menu.items()) if (!item.header) ++usable;

    // Walking down the whole list must visit every usable entry once and never a title.
    std::vector<std::string> visited;
    for (int i = 0; i < usable - 1; ++i) {
        menu.handleKey(tw::Key::Down);
        const tw::MenuItem& item = menu.items()[menu.selectedIndex()];
        check(!item.header, "Down never lands on a section title");
        visited.push_back(item.label);
    }
    check(menu.items()[menu.selectedIndex()].label == "Unlock raids and events",
          "walking down ends on the last entry");
    menu.handleKey(tw::Key::Down);
    check(menu.items()[menu.selectedIndex()].label == "Free Camera",
          "Down wraps round to the first entry");

    menu.handleKey(tw::Key::Up);
    check(menu.items()[menu.selectedIndex()].label == "Unlock raids and events",
          "Up wraps round to the last entry");

    int titles = 0;
    for (const auto& item : menu.items()) if (item.header) ++titles;
    check(total == static_cast<std::size_t>(usable + titles),
          "the list is the usable entries plus the section titles");
    check(titles == 4, "CAMERA, SPAWN, WORLD, PROGRESSION");
}

void testActions() {
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels, &spawned));

    // Free Camera is first: Enter must flip it and call the hook.
    menu.select(0);
    menu.handleKey(tw::Key::Select);
    check(toggles == 1, "Enter on a toggle calls the hook");
    check(menu.status() == "on", "the toggle's answer becomes the status line");

    // Noclip is second.
    menu.handleKey(tw::Key::Down);
    menu.handleKey(tw::Key::Select);
    check(toggles == 2, "Enter on the second toggle calls its own hook");

    // Weather is a live selector: Left/Right moves through the options and applies the one
    // it lands on, because a setting that only takes effect on Enter is a setting people
    // think they have changed when they have not.
    menu.handleKey(tw::Key::Down);   // Noclip speed
    check(menu.items()[menu.selectedIndex()].label == "Noclip speed", "the speed picker follows");
    menu.handleKey(tw::Key::Down);   // Spawn Ped
    check(menu.items()[menu.selectedIndex()].label == "Spawn Ped", "then the spawn section");
    menu.handleKey(tw::Key::Down);   // Ped model (list)
    menu.handleKey(tw::Key::Down);   // Weather
    check(menu.items()[menu.selectedIndex()].label == "Weather", "down again reaches Weather");
    const std::size_t before = menu.items()[menu.selectedIndex()].optionIndex;
    menu.handleKey(tw::Key::Right);
    check(menu.items()[menu.selectedIndex()].optionIndex != before, "Right changes the choice");
    check(picks == 1, "Right applies the new choice");
    menu.handleKey(tw::Key::Left);
    check(menu.items()[menu.selectedIndex()].optionIndex == before, "Left puts it back");
    check(picks == 2, "Left applies it too");
    menu.handleKey(tw::Key::Select);
    check(picks == 3, "Enter on a selector picks it again");

    // The level pair: the slider is set, Apply reads both entries.
    selectLabel(menu, "Level value");
    check(menu.items()[menu.selectedIndex()].label == "Level value", "the slider is there");
    menu.handleKey(tw::Key::Right);
    menu.handleKey(tw::Key::Down);
    check(menu.items()[menu.selectedIndex()].label == "Apply level", "Apply is under it");
    menu.handleKey(tw::Key::Select);
    check(levels == 1, "Apply level calls the level hook");

    // Back closes the menu.
    menu.handleKey(tw::Key::Back);
    check(!menu.visible(), "Backspace closes the menu");
    check(menu.renderLines().empty(), "a closed menu draws nothing");
}

void testSpawnPed() {
    // Spawn Ped: one entry, a name typed into it, and Enter spawns that name. The list
    // underneath only fills the field in - it must not spawn by itself.
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels, &spawned));

    selectLabel(menu, "Spawn Ped");
    const tw::MenuItem& ped = menu.items()[menu.selectedIndex()];
    check(ped.kind == tw::ItemKind::Input, "Spawn Ped is a text entry");
    check(ped.text == "object-physical-character-police", "it starts on a known model");

    // Enter on it, untouched, spawns the model it shows.
    menu.handleKey(tw::Key::Select);
    check(spawned.size() == 1, "Enter on Spawn Ped spawns one ped");
    check(!spawned.empty() && spawned.back() == "object-physical-character-police",
          "and it spawns the model shown");

    // A name typed by hand - including one nobody has in a list - is what gets spawned.
    check(menu.setInputText("Spawn Ped", "  object-physical-character-taxi_driver_faceaction "),
          "the field takes a typed name");
    menu.handleKey(tw::Key::Select);
    check(spawned.size() == 2, "a typed name spawns once");
    check(spawned.size() == 2 &&
              spawned.back() == "object-physical-character-taxi_driver_faceaction",
          "and the spaces around it are trimmed");

    // An empty field says so rather than calling the spawner with nothing.
    check(menu.setInputText("Spawn Ped", "   "), "the field can be emptied");
    menu.handleKey(tw::Key::Select);
    check(spawned.size() == 2, "an empty name spawns nothing");
    check(menu.status().find("type a model name") != std::string::npos,
          "an empty name says why");

    // The list is a way to fill the field in, not a second spawn button.
    selectLabel(menu, "Ped model (list)");
    menu.handleKey(tw::Key::Right);
    check(spawned.size() == 2, "the list does not spawn on its own");
    const std::string* text = menu.inputText("Spawn Ped");
    check(text && *text == "object-physical-character-player", "the list fills the field in");
    menu.handleKey(tw::Key::Left);
    check(menu.inputText("Spawn Ped") && *menu.inputText("Spawn Ped") ==
              "object-physical-character-police",
          "walking the list backwards fills it in too");

    // Left/Right on the entry itself must not edit the text - the arrows are the caret's
    // while a name is being typed.
    selectLabel(menu, "Spawn Ped");
    const std::string before = *menu.inputText("Spawn Ped");
    menu.handleKey(tw::Key::Right);
    check(menu.inputText("Spawn Ped") && *menu.inputText("Spawn Ped") == before,
          "the arrows do not edit a typed name");

    // A menu with no spawner has no Spawn Ped entry and no SPAWN section.
    tw::Menu bare;
    tw::DebugHooks none;
    tw::buildDebugMenu(bare, none);
    check(!hasLabel(bare, "Spawn Ped"), "no spawner, no Spawn Ped entry");
    check(!hasHeader(bare, "SPAWN"), "no spawner, no SPAWN section");
}

void testNoclip() {
    // The noclip entries: the toggle, and a speed picker that is a setting rather than a
    // command. Both have to survive a hotkey changing them outside the menu.
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::DebugHooks hooks = fullHooks(&toggles, &picks, &levels, &spawned);
    bool noclipOn = false;
    hooks.setNoclip = [&noclipOn](bool on) {
        noclipOn = on;
        return std::string(on ? "Noclip: on" : "Noclip: off");
    };
    int speedIndex = 0;
    hooks.setNoclipSpeed = [&speedIndex](const std::string& s) {
        speedIndex = s == "8x" ? 5 : speedIndex;
        return "Noclip speed: " + s;
    };
    hooks.syncState = [&menu, &noclipOn, &speedIndex] {
        menu.setToggled("Noclip (fly the player)", noclipOn);
        menu.setOptionIndex("Noclip speed", static_cast<std::size_t>(speedIndex));
    };
    tw::buildDebugMenu(menu, hooks);

    selectLabel(menu, "Noclip (fly the player)");
    menu.handleKey(tw::Key::Select);
    check(noclipOn, "Enter on the noclip toggle turns it on");
    check(menu.items()[menu.selectedIndex()].toggled, "and the row says ON");

    // U was pressed in game: the state changed without the menu, and the next sync pass
    // has to put the row back in step.
    noclipOn = false;
    if (hooks.syncState) hooks.syncState();
    check(!menu.items()[menu.selectedIndex()].toggled, "a hotkey switch off shows up in the menu");

    selectLabel(menu, "Noclip speed");
    check(menu.items()[menu.selectedIndex()].label == "Noclip speed", "the speed picker is there");
    check(menu.items()[menu.selectedIndex()].applyOnChange, "the speed applies while browsing");
    for (int i = 0; i < 5; ++i) menu.handleKey(tw::Key::Right);
    check(menu.items()[menu.selectedIndex()].optionIndex == 5, "Right walks the speeds");
    check(speedIndex == 5, "and the last one applied");

    // A speed key pressed in game moves the picker too.
    speedIndex = 0;
    if (hooks.syncState) hooks.syncState();
    check(menu.items()[menu.selectedIndex()].optionIndex == 0, "the picker follows the hotkey");

    // Clicking (or Enter) on the picker must not undo what is selected.
    menu.handleKey(tw::Key::Select);
    check(menu.items()[menu.selectedIndex()].optionIndex == 0, "Enter keeps the choice");
}

// ---------------------------------------------------------------------------
// The unlock: what "Unlock all content" writes into a profile.
//
// freecam_unlock_rules.cpp is plain C++ over the JSON tree, so the whole of it can be
// checked here - the bag shapes the client reads, the job board reset, the challenge
// window - without Windows, the game or the server.
// ---------------------------------------------------------------------------

const char* kFreshProfile = R"({
 "version": 8,
 "player": {
  "PlayerDataBag": {"PlayerDataID": "1001", "AccountID": "1", "EnforcerXP": 0},
  "CurrencyBag": {"CleanMoney": "25000", "DirtyMoney": "5000", "Gold": "0"},
  "TurfBag": {"Turf": {"Safehouse": {"RacketLevel": 1},
                       "Warehouse": {"RacketLevel": 1}},
              "Rackets": [{"RacketLevel": 2}]},
  "FavoursBag": [],
  "EnforcerUpgradesBag": [],
  "InventoryBag": [{"SecondsElapsed": 0, "TotalSeconds": 0, "StoreItemID": "5001",
                    "InventoryItemID": "7005", "AccountID": "1"}],
  "CustomizationBag": {"ScalingPresetCortexUID": "0", "ClothingOutfitCortexUID": "0",
                       "ClothingStoreItemUIDs": []},
  "ErrandsBag": {"Day": "2026-01-01", "Slots": [null, null, null], "Completed": {},
                 "Changes": 0},
  "TrialsBag": {"Index": 0, "Start": 0, "End": 0, "Progress": 0, "Rewarded": [],
                "OptedIn": 0, "Name": ""}
 },
 "opponents": [{"type": 0, "cooldown_until": 1234567, "data": {}},
               {"type": 0, "cooldown_until": 0, "data": {}}]
})";

tw::unlock::Settings testSettings() {
    tw::unlock::Settings s;
    s.maxXp = 17177050;
    s.maxRacketLevel = 10;
    s.triadLevel = 50;
    s.now = 1767225600;                 // 2026-01-01T00:00:00Z
    s.rotationSeconds = 7 * 86400;
    return s;
}

tw::unlock::Catalog testCatalog() {
    tw::unlock::Catalog c;
    c.favours = {"1001", "1002"};
    c.upgrades = {"9001", "9002"};
    c.items = {"5001", "5002", "6001", "5003", "7001"};
    return c;
}

const tw::json::Value* at(const tw::json::Value& object, const char* key) {
    return object.find(key);
}

long long numberAt(const tw::json::Value& object, const char* key, long long fallback = -1) {
    const tw::json::Value* v = at(object, key);
    if (!v || v->type != tw::json::Value::Type::Int) return fallback;
    return v->integer;
}

std::string textAt(const tw::json::Value& object, const char* key) {
    const tw::json::Value* v = at(object, key);
    if (!v || !v->isString()) return std::string();
    return v->str;
}

void testUnlockCatalog() {
    tw::json::Value favourites;
    check(tw::json::parse(R"({
        "1001": {"upgrade": "9001", "name": "Iron Fist", "ftpe": false},
        "1002": {"upgrade": "9002", "name": "Second Wind", "ftpe": true}
    })", favourites), "the favour catalogue parses");

    tw::json::Value stores;
    check(tw::json::parse(R"({
        "3503": {"name": "Wardrobe Store", "type": "UFG::StoreClothing",
                 "items": ["5001", "5002"]},
        "1316": {"name": "Outfit Store", "type": "UFG::StoreOutfit", "items": ["6001"],
                 "pieces": {"6001": ["5001", "5003"]}},
        "1767": {"name": "Pack Store", "type": "UFG::StoreContactBundle", "items": ["7001"]}
    })", stores), "the store catalogue parses");

    const tw::unlock::Catalog catalog = tw::unlock::ReadCatalog(favourites, stores);
    check(catalog.favours.size() == 2, "both favour definitions are read");
    check(catalog.upgrades.size() == 2, "both enforcer upgrades are read");
    // 5001, 5002, 6001, 7001 from the three catalogues, plus 5003 only reachable as one of
    // the outfit's pieces; 5001 is listed twice and must appear once.
    check(catalog.items.size() == 5, "shop items, including outfit pieces, are read once each");

    tw::json::Value missing;
    const tw::unlock::Catalog empty = tw::unlock::ReadCatalog(missing, missing);
    check(empty.favours.empty() && empty.items.empty(),
          "a server without the data files gives an empty catalogue, not a crash");
}

void testUnlockProfile() {
    const tw::unlock::Settings settings = testSettings();
    const tw::unlock::Catalog catalog = testCatalog();

    // ---- the smaller entry: opportunities only -------------------------------
    {
        tw::json::Value state;
        check(tw::json::parse(kFreshProfile, state), "the fresh profile parses");
        const tw::unlock::Report report =
            tw::unlock::ApplyStateRules(state, catalog, settings, false);

        check(report.favours == 0 && report.items == 0 && report.upgrades == 0,
              "without the content half nothing is put in the bags");
        check(report.rivals == 1, "only the rival on cooldown is freed");
        check(report.board, "the job board is due for a rebuild");
        check(report.event, "the challenge is started");
        check(!report.wasUnlocked, "a fresh profile was not unlocked already");

        const tw::json::Value* player = at(state, "player");
        check(player && at(*player, "UnlockBag") &&
                  at(*at(*player, "UnlockBag"), "All")->boolean,
              "the profile carries the switch state.py reads");
        check(textAt(at(*player, "ErrandsBag") ? *at(*player, "ErrandsBag") : state, "Day").empty(),
              "the job board's day is cleared so it refills");
    }

    // ---- the full entry: content as well -------------------------------------
    tw::json::Value state;
    check(tw::json::parse(kFreshProfile, state), "the fresh profile parses again");
    const tw::unlock::Report report =
        tw::unlock::ApplyStateRules(state, catalog, settings, true);

    check(report.favours == 2, "one favour instance per upgrade favour definition");
    check(report.upgrades == 2, "both enforcer upgrades are owned");
    check(report.items == 4, "the four shop items not already owned are added");
    check(report.rivals == 1, "the rival on cooldown is freed");

    const tw::json::Value* player = at(state, "player");
    check(player != nullptr, "the profile is still there");
    if (player) {
        const tw::json::Value* data = at(*player, "PlayerDataBag");
        check(numberAt(*data, "EnforcerXP") == settings.maxXp, "the enforcer is at the top level");
        check(numberAt(*data, "TriadLevel") == settings.triadLevel,
              "the triad is at the top level");

        const tw::json::Value* turf = at(*player, "TurfBag");
        const long long safehouse =
            numberAt(*at(*at(*turf, "Turf"), "Safehouse"), "RacketLevel");
        check(safehouse == settings.maxRacketLevel, "the safehouse is at its top level");
        const long long racket = numberAt(at(*turf, "Rackets")->array.front(), "RacketLevel");
        check(racket == settings.maxRacketLevel, "every built racket is at its top level");

        // {Item1: upgrade, Item2: slot} - owned but not equipped.
        const tw::json::Value* upgrades = at(*player, "EnforcerUpgradesBag");
        check(upgrades->array.size() == 2, "both upgrades are in the bag");
        check(textAt(upgrades->array.front(), "Item1") == "9001",
              "the upgrade UID is the first field");
        check(at(upgrades->array.front(), "Item2")->type == tw::json::Value::Type::Null,
              "an owned upgrade starts unequipped, not in slot 0");

        const tw::json::Value* favours = at(*player, "FavoursBag");
        check(textAt(favours->array.front(), "FavourDefinitionUID") == "1001",
              "the favour names its definition");
        check(textAt(favours->array.front(), "FavourID") == "5001",
              "favour instance ids continue after the first-time-flow ones");

        const tw::json::Value* inventory = at(*player, "InventoryBag");
        check(inventory->array.size() == 5, "the four new items join the one already owned");
        // 7005 is taken, so the new records must not reuse it.
        bool seen7006 = false, unique = true;
        std::vector<std::string> ids;
        for (const tw::json::Value& item : inventory->array) {
            const std::string id = textAt(item, "InventoryItemID");
            if (id == "7006") seen7006 = true;
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) unique = false;
            ids.push_back(id);
        }
        check(seen7006, "inventory ids continue after the highest one already owned");
        check(unique, "no two inventory items share an id");

        const tw::json::Value* trials = at(*player, "TrialsBag");
        check(numberAt(*trials, "Start") == settings.now, "the challenge window starts now");
        check(numberAt(*trials, "End") == settings.now + settings.rotationSeconds,
              "the challenge runs for a whole rotation");
        check(numberAt(*trials, "OptedIn") == 1, "the player is opted into the challenge");
    }

    const tw::json::Value* opponents = at(state, "opponents");
    check(opponents->array.front().find("cooldown_until")->integer == 0,
          "the rival that was counting down can be raided now");

    // ---- pressing it twice ---------------------------------------------------
    {
        const std::string first = tw::json::dump(state, 1);
        const tw::unlock::Report again =
            tw::unlock::ApplyStateRules(state, catalog, settings, true);
        check(again.favours == 0 && again.items == 0 && again.upgrades == 0,
              "a second press adds no duplicate favours, upgrades or items");
        check(again.wasUnlocked, "the second press sees the profile already unlocked");
        check(!again.board, "a second press does not reroll a job board being worked on");
        check(first == tw::json::dump(state, 1), "a second press leaves the profile as it was");
    }
}

void testUnlockKeepsProgress() {
    // A profile in the middle of a challenge, and one saved by an older server that has
    // never heard of the job board or the trial bag.
    tw::json::Value state;
    check(tw::json::parse(R"({
     "player": {
      "PlayerDataBag": {"AccountID": "1", "EnforcerXP": 500},
      "FavoursBag": [], "EnforcerUpgradesBag": [], "InventoryBag": [],
      "TrialsBag": {"Index": 1, "Start": 1767225500, "End": 1767825600, "Progress": 7,
                    "Rewarded": [0], "OptedIn": 0, "Name": ""}
     },
     "opponents": []
    })", state), "a mid-challenge profile parses");

    const tw::unlock::Report report =
        tw::unlock::ApplyStateRules(state, testCatalog(), testSettings(), false);
    const tw::json::Value* trials = at(*at(state, "player"), "TrialsBag");
    check(numberAt(*trials, "Start") == 1767225500, "a running challenge keeps its window");
    check(numberAt(*trials, "Progress") == 7, "a running challenge keeps its progress");
    check(numberAt(*trials, "Index") == 1, "a running challenge keeps its rotation slot");
    check(numberAt(*trials, "OptedIn") == 1, "the player is opted in");
    check(report.event, "opting in counts as the event being switched on");

    // The bags an older save does not have are created rather than assumed.
    check(at(*at(state, "player"), "ErrandsBag") != nullptr, "a missing job board is created");
    check(textAt(*at(*at(state, "player"), "ErrandsBag"), "Day").empty(),
          "the new job board is due for a refill");

    tw::json::Value empty;
    check(tw::json::parse(R"({"player": null})", empty), "a profile without a player parses");
    const tw::unlock::Report none =
        tw::unlock::ApplyStateRules(empty, testCatalog(), testSettings(), true);
    check(none.favours == 0 && none.items == 0, "a profile without a player is left alone");
}

void testSparseHooks() {
    // A build (or a test) that can only unlock: no selectors and no camera hook at all.
    tw::Menu menu;
    menu.setVisible(true);
    tw::DebugHooks hooks;
    hooks.unlockAllContent = [] { return std::string("unlocked"); };
    hooks.setNoclip = [](bool) { return std::string("noclip"); };
    tw::buildDebugMenu(menu, hooks);

    check(hasHeader(menu, "PROGRESSION"), "the unlock section survives on its own");
    check(!hasHeader(menu, "WORLD"), "no WORLD section without weather or presets");
    check(!hasLabel(menu, "Weather"), "no empty weather selector");
    check(!hasLabel(menu, "Level target"), "no empty level selector");
    check(hasLabel(menu, "Free Camera"), "Free Camera is still listed");

    // Free Camera is listed without a hook: using it must say so rather than do nothing.
    menu.select(0);
    menu.handleKey(tw::Key::Select);
    check(menu.status().find("not available") != std::string::npos,
          "an unwired entry says it is not available");
}

}  // namespace

int main() {
    testStructure();
    testCursorSkipsHeaders();
    testActions();
    testSpawnPed();
    testNoclip();
    testSparseHooks();
    testUnlockCatalog();
    testUnlockProfile();
    testUnlockKeepsProgress();
    std::printf("menu: %d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

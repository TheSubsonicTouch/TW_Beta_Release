// test_menu.cpp - checks the menu model without Windows, ImGui or the game.
//
// The menu's contents are decided in menu_core.cpp, which is plain C++: what the entries
// are, what order they come in, what the keys do to them. That is exactly the part that
// goes wrong quietly - an entry left behind after a feature is removed, a section title
// that eats the cursor, an action that never fires - so it is checked here, the same way
// the camera maths is.
//
// Built and run by run_tests.py; any C++17 compiler will do.
#include "../menu/patched/menu_core.h"

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
    std::printf("menu: %d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

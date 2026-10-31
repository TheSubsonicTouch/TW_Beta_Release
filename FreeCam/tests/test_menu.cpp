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
tw::DebugHooks fullHooks(int* toggles, int* picks, int* levels) {
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
    return hooks;
}

void testStructure() {
    int toggles = 0, picks = 0, levels = 0;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels));

    const auto lines = menu.renderLines();
    check(!lines.empty() && lines[0] == "TriadMenu", "the menu is titled TriadMenu");

    // What the menu offers, and in which order.
    check(hasLabel(menu, "Free Camera"), "Free Camera is there");
    check(hasLabel(menu, "Noclip (fly the player)"), "noclip is there");
    check(hasLabel(menu, "Weather"), "weather is there");
    check(hasLabel(menu, "ReShade preset"), "the ReShade preset picker is there");
    check(hasLabel(menu, "Level target"), "the level target is there");
    check(hasLabel(menu, "Level value"), "the level slider is there");
    check(hasLabel(menu, "Apply level"), "Apply level is there");
    check(hasLabel(menu, "Unlock all content"), "Unlock all content is there");
    check(hasLabel(menu, "Unlock raids and events"), "Unlock raids is there");

    check(hasHeader(menu, "CAMERA") && hasHeader(menu, "WORLD") &&
              hasHeader(menu, "PROGRESSION"),
          "the three sections are there");

    // What was taken out must stay out: those entries either could not work or were
    // replaced by hotkeys, and a menu that offers them is worse than a short one.
    const char* gone[] = { "Spawn", "Teleport", "Hide HUD", "Time of", "Invincible",
                           "Reload" };
    for (const char* word : gone) {
        check(!anyLabelContains(menu, word), std::string("no entry mentions ") + word);
    }

    // The status line is empty until something is done.
    check(menu.status().empty(), "no status before the first action");
}

void testCursorSkipsHeaders() {
    int toggles = 0, picks = 0, levels = 0;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels));

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

    check(total == static_cast<std::size_t>(usable) + 3,
          "the list is the usable entries plus the three titles");
}

void testActions() {
    int toggles = 0, picks = 0, levels = 0;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels));

    // Free Camera is first: Enter must flip it and call the hook.
    menu.select(0);
    menu.handleKey(tw::Key::Select);
    check(toggles == 1, "Enter on a toggle calls the hook");
    check(menu.status() == "on", "the toggle's answer becomes the status line");

    // Noclip is second.
    menu.handleKey(tw::Key::Down);
    menu.handleKey(tw::Key::Select);
    check(toggles == 2, "Enter on the second toggle calls its own hook");

    // Weather is a selector: Left/Right moves through the options.
    menu.handleKey(tw::Key::Down);
    check(menu.items()[menu.selectedIndex()].label == "Weather", "down again reaches Weather");
    const std::size_t before = menu.items()[menu.selectedIndex()].optionIndex;
    menu.handleKey(tw::Key::Right);
    check(menu.items()[menu.selectedIndex()].optionIndex != before, "Right changes the choice");
    menu.handleKey(tw::Key::Left);
    check(menu.items()[menu.selectedIndex()].optionIndex == before, "Left puts it back");
    menu.handleKey(tw::Key::Select);
    check(picks == 1, "Enter on a selector picks it");

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
    testSparseHooks();
    std::printf("menu: %d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

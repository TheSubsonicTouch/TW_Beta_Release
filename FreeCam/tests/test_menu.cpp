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
    hooks.setPlayerFree = [toggles](bool on) {
        ++*toggles;
        return std::string(on ? "player free" : "player frozen");
    };
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
    // Weather sliders and the skybox: the same shape the game hooks have.
    hooks.setWeatherAmount = [picks](float v) {
        ++*picks;
        return "Rain amount: " + std::to_string(v);
    };
    hooks.setWeatherChance = [picks](float v) {
        ++*picks;
        return "Rain chance: " + std::to_string(v);
    };
    hooks.setWeatherWetness = [picks](float v) {
        ++*picks;
        return "Wetness: " + std::to_string(v);
    };
    hooks.setWeatherInterval = [picks](int s) {
        ++*picks;
        return "Weather interval: " + std::to_string(s);
    };
    hooks.setWeatherLock = [toggles](bool on) {
        ++*toggles;
        return std::string(on ? "Weather locked" : "Weather unlocked");
    };
    hooks.weatherAmount = [] { return 1.2f; };
    hooks.weatherChance = [] { return 0.25f; };
    hooks.weatherWetness = [] { return 0.0f; };
    hooks.weatherLocked = [] { return false; };
    hooks.skydomes = { "Automatic", "Sunny", "Cloudy", "Stormy", "Custom" };
    hooks.setSkydome = [picks](int index) {
        ++*picks;
        return "Skybox: index " + std::to_string(index);
    };
    hooks.skydomeCurrent = [] { return 2; };
    // The spawn side and the hologram. The preview is off to start with, which is the
    // state the menu has to open in: nothing is drawn until it is asked for.
    hooks.setSpawnHostile = [toggles](bool on) {
        ++*toggles;
        return std::string(on ? "Spawn side: hostile" : "Spawn side: friendly");
    };
    hooks.spawnHostile = [] { return false; };
    hooks.setHologramPreview = [toggles](bool on) {
        ++*toggles;
        return std::string(on ? "Spawn preview: on" : "Spawn preview: off");
    };
    hooks.hologramPreviewActive = [] { return false; };
    hooks.setHologramDistance = [picks](float m) {
        ++*picks;
        return "Spawn preview: " + std::to_string(static_cast<int>(m)) + " m";
    };
    hooks.hologramDistance = [] { return 3.0f; };

    // The camera's other modes and the path. Each is the same action its hotkey runs, so
    // here they only have to reach the right hook.
    hooks.setFreeze = [toggles](bool on) {
        ++*toggles;
        return std::string(on ? "camera frozen" : "camera flying");
    };
    hooks.frozen = [] { return false; };
    hooks.setFollow = [toggles](bool on) {
        ++*toggles;
        return std::string(on ? "following" : "not following");
    };
    hooks.following = [] { return false; };
    hooks.setLookAtPlayer = [toggles](bool on) {
        ++*toggles;
        return std::string(on ? "looking at the player" : "holding still");
    };
    hooks.lookingAtPlayer = [] { return false; };
    hooks.setFlySpeed = [picks](float v) {
        ++*picks;
        return "Fly speed: " + std::to_string(static_cast<int>(v));
    };
    hooks.flySpeed = [] { return 30.0f; };

    hooks.recordKeyframe = [picks] {
        ++*picks;
        return std::string("keyframe 1 recorded");
    };
    hooks.setPathPlaying = [toggles](bool on) {
        ++*toggles;
        return std::string(on ? "playing" : "playback stopped");
    };
    hooks.pathPlaying = [] { return false; };
    hooks.clearPath = [picks] {
        ++*picks;
        return std::string("cleared 1 keyframe");
    };
    hooks.playbackSpeeds = { "0.1x", "0.5x", "1x", "2x", "4x" };
    hooks.setPlaybackSpeed = [picks](int index) {
        ++*picks;
        return "Playback speed: index " + std::to_string(index);
    };
    hooks.keyframeCount = [] { return 3; };

    hooks.spawnerStatus = [] { return std::string("ready"); };
    hooks.resetSpawner = [picks] {
        ++*picks;
        return std::string("Spawner: reset");
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
    check(hasLabel(menu, "Player free (U)"), "the player's control toggle is there");
    check(hasLabel(menu, "Noclip (V)"), "noclip is there");
    check(hasLabel(menu, "Noclip speed"), "the noclip speed picker is there");
    check(hasLabel(menu, "Weather"), "weather is there");
    check(hasLabel(menu, "Rain amount"), "the rain amount slider is there");
    check(hasLabel(menu, "Rain chance"), "the rain chance slider is there");
    check(hasLabel(menu, "Wetness override"), "the wetness slider is there");
    check(hasLabel(menu, "Weather change interval"), "the interval slider is there");
    check(hasLabel(menu, "Lock weather"), "the weather lock is there");
    check(hasLabel(menu, "Skybox (skydome)"), "the skybox picker is there");
    check(hasLabel(menu, "ReShade preset"), "the ReShade preset picker is there");
    check(hasLabel(menu, "Level target"), "the level target is there");
    check(hasLabel(menu, "Level value"), "the level slider is there");
    check(hasLabel(menu, "Apply level"), "Apply level is there");
    check(hasLabel(menu, "Unlock all content"), "Unlock all content is there");
    check(hasLabel(menu, "Unlock raids and events"), "Unlock raids is there");

    check(hasLabel(menu, "Spawn Ped"), "Spawn Ped is in the menu");
    check(hasLabel(menu, "Ped model (list)"), "the ped list is under it");
    check(hasLabel(menu, "Hostile NPC"), "the side toggle is there");
    check(hasLabel(menu, "Spawn preview"), "the hologram toggle is there");
    check(hasLabel(menu, "Preview distance"), "and the distance that places it");
    check(hasLabel(menu, "Spawner"), "the spawner's own state is shown");
    check(hasLabel(menu, "Reset spawner (R)"), "and R has an entry");

    // Every camera mode the keys reach has an entry, and every entry names its key: a
    // feature that only exists on the keyboard is a feature the menu does not have.
    check(hasLabel(menu, "Freeze camera (O)"), "freeze is in the menu");
    check(hasLabel(menu, "Follow player (P)"), "follow is in the menu");
    check(hasLabel(menu, "Look at player (P)"), "aiming from a freeze is in the menu");
    check(hasLabel(menu, "Fly speed"), "the flying speed is in the menu");
    check(hasLabel(menu, "Record keyframe (L)"), "recording is in the menu");
    check(hasLabel(menu, "Play path (J)"), "playback is in the menu");
    check(hasLabel(menu, "Clear path (M)"), "clearing the path is in the menu");
    check(hasLabel(menu, "Playback speed"), "the playback speed is a picker");
    check(hasLabel(menu, "Keyframes"), "and how many are recorded is shown");

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
    // "Usable" is what the cursor can land on: not a section title, and not an Info row
    // (which is shown but never selected).
    int usable = 0;
    for (const auto& item : menu.items())
        if (!item.header && item.kind != tw::ItemKind::Info) ++usable;

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
    check(total == static_cast<std::size_t>(usable + titles + 2),
          "the list is the usable entries plus the section titles and the two info rows");
    check(titles == 5, "CAMERA, SPAWN, PATH, WORLD, PROGRESSION");
    // And in that order, so the section jumps below can be checked against something.
    {
        std::string order;
        for (const auto& item : menu.items()) {
            if (!item.header) continue;
            order += item.label;
            order += ' ';
        }
        check(order == "CAMERA SPAWN PATH WORLD PROGRESSION ", "the sections are in order: " + order);
    }
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

    // Under it, the player's own controls: the second toggle, with its own hook.
    menu.handleKey(tw::Key::Down);
    check(menu.items()[menu.selectedIndex()].label == "Player free (U)",
          "Player free follows Free Camera");
    menu.handleKey(tw::Key::Select);
    check(toggles == 2, "Enter on the second toggle calls its own hook");
    check(menu.status() == "player free", "and its answer is the status line");

    // Noclip is third.
    menu.handleKey(tw::Key::Down);
    menu.handleKey(tw::Key::Select);
    check(toggles == 3, "Enter on the third toggle calls the noclip hook");

    // Weather is a live selector: Left/Right moves through the options and applies the one
    // it lands on, because a setting that only takes effect on Enter is a setting people
    // think they have changed when they have not.
    menu.handleKey(tw::Key::Down);   // Noclip speed
    check(menu.items()[menu.selectedIndex()].label == "Noclip speed", "the speed picker follows");
    // The rest of the camera, then the PATH section (whose header the cursor steps over).
    menu.handleKey(tw::Key::Down);   // Freeze camera (O)
    menu.handleKey(tw::Key::Down);   // Follow player (P)
    menu.handleKey(tw::Key::Down);   // Look at player (P)
    menu.handleKey(tw::Key::Down);   // Fly speed
    check(menu.items()[menu.selectedIndex()].label == "Fly speed",
          "the camera's modes come before the spawner");
    // SPAWN is the second section, straight after CAMERA: it is the one people open the
    // menu for, so it does not sit at the bottom under the recording tools.
    menu.handleKey(tw::Key::Down);   // Spawn Ped, over the SPAWN header
    check(menu.items()[menu.selectedIndex()].label == "Spawn Ped", "then the spawn section");
    menu.handleKey(tw::Key::Down);   // Spawn now (B): the button beside the name field
    check(menu.items()[menu.selectedIndex()].label == "Spawn now (B)",
          "then the spawn button");
    menu.handleKey(tw::Key::Down);   // Ped model (list)
    check(menu.items()[menu.selectedIndex()].label == "Ped model (list)",
          "then the model list");
    menu.handleKey(tw::Key::Down);   // Hostile NPC
    menu.handleKey(tw::Key::Down);   // Spawn preview
    menu.handleKey(tw::Key::Down);   // Preview distance
    check(menu.items()[menu.selectedIndex()].label == "Preview distance",
          "the preview controls sit in the spawn section");
    menu.handleKey(tw::Key::Down);   // Reset spawner (R), over the Spawner info row
    check(menu.items()[menu.selectedIndex()].label == "Reset spawner (R)",
          "the reset is the last spawn entry");
    menu.handleKey(tw::Key::Down);   // Record keyframe (L), over the PATH header
    check(menu.items()[menu.selectedIndex()].label == "Record keyframe (L)",
          "then the path section");
    menu.handleKey(tw::Key::Down);   // Play path (J)
    menu.handleKey(tw::Key::Down);   // Clear path (M)
    menu.handleKey(tw::Key::Down);   // Playback speed, over the Keyframes info row
    check(menu.items()[menu.selectedIndex()].label == "Playback speed",
          "ending on the playback speed");
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

    // A menu with no spawner still has the SPAWN section and a Spawn Ped row: the row says
    // what it cannot do instead of the section silently not being there. (What a row does
    // without its hook is checked in testMissingHooksStillListed.)
    tw::Menu bare;
    tw::DebugHooks none;
    tw::buildDebugMenu(bare, none);
    check(hasLabel(bare, "Spawn Ped"), "without a spawner the row is still there");
    check(hasHeader(bare, "SPAWN"), "and so is the section");
}

void testPlayerFree() {
    // U and the "Player free (U)" entry: the player takes the controls while the camera
    // stays on, and gives them back to the camera on the next press. The row has to follow
    // the hotkey, because the hotkey is how it is normally used.
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::DebugHooks hooks = fullHooks(&toggles, &picks, &levels, &spawned);
    bool cameraOn = true, playerFree = false;
    hooks.setPlayerFree = [&playerFree, &cameraOn](bool on) {
        if (!cameraOn) return std::string("Player free: switch the free camera on first");
        playerFree = on;
        return std::string(on ? "Player free: move, fight, interact"
                              : "Player frozen: the keys fly the camera again");
    };
    hooks.setFreeCam = [&cameraOn, &playerFree](bool on) {
        cameraOn = on;
        if (!on) playerFree = false;      // switching the camera off gives them back
        return std::string(on ? "Free camera: on" : "Free camera: off");
    };
    hooks.syncState = [&menu, &playerFree, &cameraOn] {
        menu.setToggled("Free Camera", cameraOn);
        menu.setToggled("Player free (U)", cameraOn && playerFree);
    };
    tw::buildDebugMenu(menu, hooks);

    check(hasLabel(menu, "Player free (U)"), "the entry is listed");
    selectLabel(menu, "Player free (U)");
    menu.handleKey(tw::Key::Select);
    check(playerFree, "Enter on Player free hands the controls over");
    check(menu.items()[menu.selectedIndex()].toggled, "and the row says ON");

    // U in game: the state changed behind the menu's back, and the next sync pass shows it.
    playerFree = false;
    hooks.syncState();
    check(!menu.items()[menu.selectedIndex()].toggled, "a hotkey switch off shows up in the menu");

    // With the free camera off there is nothing to hand over, and the entry says so.
    cameraOn = false;
    menu.handleKey(tw::Key::Select);
    check(!playerFree, "the player is not freed while the camera is off");
    check(menu.status().find("switch the free camera on") != std::string::npos,
          "and the status line explains why");
    hooks.syncState();      // the frame loop does this once per frame
    check(!menu.items()[menu.selectedIndex()].toggled,
          "the row is off while the camera is off");
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
        menu.setToggled("Noclip (V)", noclipOn);
        menu.setOptionIndex("Noclip speed", static_cast<std::size_t>(speedIndex));
    };
    tw::buildDebugMenu(menu, hooks);

    selectLabel(menu, "Noclip (V)");
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
    // Noclip flies the character, not a camera: it must be offered even when the build
    // has no free camera hook at all, because the two are independent modes.
    check(hasLabel(menu, "Noclip (V)"),
          "noclip is offered without a free camera hook");
    selectLabel(menu, "Noclip (V)");
    menu.handleKey(tw::Key::Select);
    check(menu.items()[menu.selectedIndex()].toggled, "and it toggles on its own");
    check(menu.status() == "noclip", "its hook answered");

    // Free Camera is listed without a hook: using it must say so rather than do nothing.
    menu.select(0);
    menu.handleKey(tw::Key::Select);
    check(menu.status().find("not available") != std::string::npos,
          "an unwired entry says it is not available");
}

// The weather controls are the ones the game exposes as script bindings, so each has to
// reach its own hook and nothing else.
void testWeatherControls() {
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels, &spawned));

    // The sliders open on what the game is actually doing, not on zero.
    const tw::MenuItem* amount = menu.item("Rain amount");
    check(amount && amount->kind == tw::ItemKind::Slider, "rain amount is a slider");
    check(amount && amount->value == 1.2f, "the amount slider starts at the game's value");
    const tw::MenuItem* chance = menu.item("Rain chance");
    check(chance && chance->value == 0.25f, "the chance slider starts at the game's value");

    selectLabel(menu, "Rain amount");
    int before = picks;
    menu.handleKey(tw::Key::Right);
    check(picks == before + 1, "moving the amount slider calls its hook");
    check(menu.status().find("Rain amount") != std::string::npos, "and reports it");

    selectLabel(menu, "Weather change interval");
    before = picks;
    menu.handleKey(tw::Key::Right);
    check(picks == before + 1, "the interval slider calls its hook");

    // The lock is a toggle, and its hook is the one that runs.
    selectLabel(menu, "Lock weather");
    before = toggles;
    menu.handleKey(tw::Key::Select);
    const tw::MenuItem* lock = menu.item("Lock weather");
    check(lock && lock->toggled, "the lock toggles on");
    check(toggles == before + 1, "and calls the lock hook");
    check(menu.status() == "Weather locked", "with its own status line");
}

// The skybox list is empty until the world loads, so the entry is built with "Automatic"
// and filled in later; picking one has to reach setSkydome with the index that was chosen.
void testSkydome() {
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels, &spawned));

    const tw::MenuItem* sky = menu.item("Skybox (skydome)");
    check(sky && sky->options.size() == 5, "every skydome is offered");
    check(sky && sky->optionIndex == 2, "and the menu opens on the one in use");

    selectLabel(menu, "Skybox (skydome)");
    int before = picks;
    menu.handleKey(tw::Key::Right);          // Cloudy -> Stormy
    check(picks == before + 1, "picking a skydome calls its hook");
    check(menu.status() == "Skybox: index 3", "with the index that was picked");

    // An empty list still leaves exactly one usable choice.
    tw::DebugHooks bare = fullHooks(&toggles, &picks, &levels, &spawned);
    bare.skydomes.clear();
    bare.skydomeCurrent = nullptr;
    tw::Menu second;
    tw::buildDebugMenu(second, bare);
    const tw::MenuItem* only = second.item("Skybox (skydome)");
    check(only && only->options.size() == 1 && only->options[0] == "Automatic",
          "an unloaded world still offers Automatic");

    // setOptions refreshes the list without moving a selection that survives.
    tw::Menu third;
    tw::buildDebugMenu(third, fullHooks(&toggles, &picks, &levels, &spawned));
    third.setOptionIndex("Skybox (skydome)", 3);
    check(third.setOptions("Skybox (skydome)",
                           { "Automatic", "Sunny", "Cloudy", "Stormy", "Custom", "Night" }),
          "the list can be refreshed");
    check(third.item("Skybox (skydome)")->optionIndex == 3, "the choice is kept across a refresh");
    check(third.setOptions("Skybox (skydome)", { "Automatic", "Sunny" }),
          "and a shorter list is accepted");
    check(third.item("Skybox (skydome)")->optionIndex == 0, "with the cursor back on Automatic");
}

// The hologram entries. They are a drawing, not a spawn: the toggle only switches the
// preview on, and the distance slider is the same number the spawner would use - so what
// the figure shows is what the Spawn Ped entry above it would do.
void testHologram() {
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels, &spawned));

    const tw::MenuItem* preview = menu.item("Spawn preview");
    check(preview && preview->kind == tw::ItemKind::Toggle, "the preview is a toggle");
    check(preview && !preview->toggled, "and it starts off - nothing is drawn unasked");

    selectLabel(menu, "Spawn preview");
    int before = toggles;
    menu.handleKey(tw::Key::Select);
    check(menu.item("Spawn preview")->toggled, "it switches on");
    check(toggles == before + 1, "and calls the preview hook");
    check(menu.status() == "Spawn preview: on", "the status line says so");

    // Nothing was spawned by switching a drawing on.
    check(spawned.empty(), "switching the preview on spawns nothing");

    const tw::MenuItem* distance = menu.item("Preview distance");
    check(distance && distance->kind == tw::ItemKind::Slider, "the distance is a slider");
    check(distance && distance->value == 3.0f, "it opens on the spawner's own 3 m");
    check(distance && distance->minValue == 0.0f && distance->maxValue == 10.0f,
          "0 (on the player) to 10 m");
    check(distance && distance->step == 1.0f, "in whole metres, which is what the row shows");

    selectLabel(menu, "Preview distance");
    before = picks;
    menu.handleKey(tw::Key::Right);
    check(picks == before + 1, "moving the distance calls its hook");
    check(menu.status() == "Spawn preview: 4 m", "with the distance it moved to");

    // The side toggle is the same flag the H hotkey flips, and the label on the figure is
    // drawn from it: hostile and friendly must not be able to disagree.
    selectLabel(menu, "Hostile NPC");
    before = toggles;
    menu.handleKey(tw::Key::Select);
    check(menu.item("Hostile NPC")->toggled, "the side toggles to hostile");
    check(toggles == before + 1, "and calls the side hook");
    check(menu.status() == "Spawn side: hostile", "with its own status line");
    check(spawned.empty(), "changing the side spawns nothing either");

    // The preview needs its own hooks to do anything: a build that only has a spawner has
    // no figure to draw, so the rows are not built - but the SPAWN section still is.
    tw::DebugHooks bare;
    bare.setFreeCam = [](bool on) { return std::string(on ? "on" : "off"); };
    bare.spawnPed = [](const std::string&) { return std::string("x"); };
    tw::Menu second;
    tw::buildDebugMenu(second, bare);
    check(!second.item("Spawn preview") && !second.item("Preview distance"),
          "without the hologram hooks the preview entries are not built");
    check(hasHeader(second, "SPAWN"), "but the section is");
}

// Every camera mode the keys have is also an entry, and each entry reaches its own hook.
// This is the "all features are in the menu" test: one check per feature, so a mode that
// quietly loses its entry fails here rather than being noticed in game.
void testCameraModes() {
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels, &spawned));

    // Freeze: a toggle, like the modes above it.
    const tw::MenuItem* freeze = menu.item("Freeze camera (O)");
    check(freeze && freeze->kind == tw::ItemKind::Toggle, "freeze is a toggle");
    check(freeze && !freeze->toggled, "and it opens on what the camera is doing");
    selectLabel(menu, "Freeze camera (O)");
    int before = toggles;
    menu.handleKey(tw::Key::Select);
    check(toggles == before + 1, "freezing calls its hook");
    check(menu.item("Freeze camera (O)")->toggled, "the row switches on");
    check(menu.status() == "camera frozen", "and the hook's answer is the status");

    // Follow and look-at are the two halves of P, kept apart so the menu never has to
    // guess which one a press meant.
    selectLabel(menu, "Follow player (P)");
    before = toggles;
    menu.handleKey(tw::Key::Select);
    check(toggles == before + 1, "follow calls its hook");
    check(menu.status() == "following", "with its own answer");
    selectLabel(menu, "Look at player (P)");
    before = toggles;
    menu.handleKey(tw::Key::Select);
    check(toggles == before + 1, "look-at calls its own hook");
    check(menu.status() == "looking at the player", "with its own answer");

    // The flying speed is a slider over the range FreeCamSetSpeed accepts, opening on the
    // speed the camera is actually using.
    const tw::MenuItem* fly = menu.item("Fly speed");
    check(fly && fly->kind == tw::ItemKind::Slider, "fly speed is a slider");
    check(fly && fly->value == 30.0f, "opening on the camera's own 30 m/s");
    check(fly && fly->minValue == 1.0f && fly->maxValue == 60.0f, "1 to 60 m/s");
    selectLabel(menu, "Fly speed");
    before = picks;
    menu.handleKey(tw::Key::Right);
    check(picks == before + 1, "moving it calls its hook");
    check(menu.status() == "Fly speed: 31", "with the speed it moved to");

    // The path: record, play, clear, and the speed ladder.
    selectLabel(menu, "Record keyframe (L)");
    before = picks;
    menu.handleKey(tw::Key::Select);
    check(picks == before + 1, "recording calls its hook");
    check(menu.status() == "keyframe 1 recorded", "and reports the count");

    const tw::MenuItem* play = menu.item("Play path (J)");
    check(play && play->kind == tw::ItemKind::Toggle, "playback is a toggle, so it shows state");
    selectLabel(menu, "Play path (J)");
    before = toggles;
    menu.handleKey(tw::Key::Select);
    check(toggles == before + 1, "playing calls its hook");
    check(menu.status() == "playing", "and says so");

    selectLabel(menu, "Clear path (M)");
    before = picks;
    menu.handleKey(tw::Key::Select);
    check(picks == before + 1, "clearing calls its hook");
    check(menu.status() == "cleared 1 keyframe", "with what it cleared");

    const tw::MenuItem* speed = menu.item("Playback speed");
    check(speed && speed->kind == tw::ItemKind::Selector, "the playback speed is a picker");
    check(speed && speed->options.size() == 5, "with every rung on offer");
    selectLabel(menu, "Playback speed");
    before = picks;
    menu.handleKey(tw::Key::Right);          // 0.1x -> 0.5x
    check(picks == before + 1, "changing it applies at once");
    check(menu.status() == "Playback speed: index 1", "and reports the rung it landed on");

    // A play toggle that could not start (too few keyframes, camera off) is put back by
    // syncState rather than leaving the row saying ON over a camera that is not moving.
    menu.setToggled("Play path (J)", true);
    check(menu.item("Play path (J)")->toggled, "the row can be set from the state");
    menu.setToggled("Play path (J)", false);
    check(!menu.item("Play path (J)")->toggled, "and cleared again when it stops");

    // The two Info rows: shown, never selectable, and updated without churn.
    const tw::MenuItem* frames = menu.item("Keyframes");
    check(frames && frames->kind == tw::ItemKind::Info, "the keyframe count is an Info row");
    check(frames && frames->text == "3 recorded", "showing what the camera says");
    check(menu.setInfo("Keyframes", "4 recorded"), "an Info row can be updated");
    check(!menu.setInfo("No such row", "x"), "and an unknown label is refused");
    const tw::MenuItem* spawner = menu.item("Spawner");
    check(spawner && spawner->kind == tw::ItemKind::Info, "so is the spawner's state");
    check(spawner && spawner->text == "ready", "which opens on ready");

    // The cursor refuses to land on either of them, by walk or by click.
    for (std::size_t i = 0; i < menu.items().size(); ++i) {
        if (menu.items()[i].kind != tw::ItemKind::Info) continue;
        menu.select(i);
        check(menu.items()[menu.selectedIndex()].kind != tw::ItemKind::Info,
              "the cursor steps over an info row");
    }
    selectLabel(menu, "Keyframes");            // resolves to the row after it
    check(menu.items()[menu.selectedIndex()].kind != tw::ItemKind::Info,
          "selecting an info row lands on the next entry");
}

// PageDown and PageUp: the menu is longer than a short screen, so the sections are also
// reachable in one press. A jump must always land on something that can be used.
void testSectionJump() {
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, fullHooks(&toggles, &picks, &levels, &spawned));

    menu.select(0);
    check(menu.items()[menu.selectedIndex()].label == "Free Camera", "the jump starts at CAMERA");

    const char* forward[] = { "Spawn Ped", "Record keyframe (L)", "Weather", "Level target",
                              "Free Camera" };
    for (const char* want : forward) {
        menu.jumpSection(1);
        const std::string got = menu.items()[menu.selectedIndex()].label;
        check(got == want, std::string("PageDown lands on ") + want + ", got " + got);
        check(menu.items()[menu.selectedIndex()].kind != tw::ItemKind::Info,
              std::string("and never on an info row after jumping to ") + want);
    }

    // Backwards, from the top: the last section, then the one before it.
    menu.select(0);
    menu.jumpSection(-1);
    check(menu.items()[menu.selectedIndex()].label == "Level target",
          "PageUp from CAMERA wraps to the last section");
    menu.jumpSection(-1);
    check(menu.items()[menu.selectedIndex()].label == "Weather", "and then to WORLD");
    menu.jumpSection(-1);
    check(menu.items()[menu.selectedIndex()].label == "Record keyframe (L)", "and then to PATH");

    // A menu that is only entries (no titles) still moves, one row at a time.
    tw::Menu flat;
    flat.setVisible(true);
    tw::DebugHooks only;
    only.setFreeCam = [](bool on) { return std::string(on ? "on" : "off"); };
    tw::buildDebugMenu(flat, only);
    flat.select(0);
    check(flat.jumpSection(1), "a jump in a menu with no sections still moves");
    check(flat.items()[flat.selectedIndex()].kind != tw::ItemKind::Info,
          "and still skips info rows");
}

// A build that cannot spawn and cannot fly the player still shows both, saying why. This
// is the "every feature is visible" rule: a row that is left out looks like a feature that
// was never built, and there is nothing to act on.
void testMissingHooksStillListed() {
    tw::Menu menu;
    menu.setVisible(true);
    tw::DebugHooks bare;                       // no spawner, no noclip, no camera
    bare.setFreeCam = [](bool on) { return std::string(on ? "on" : "off"); };
    tw::buildDebugMenu(menu, bare);

    check(hasLabel(menu, "Noclip (V)"), "noclip is listed even with no noclip hook");
    check(hasHeader(menu, "SPAWN"), "the spawn section is there even with no spawner");
    check(hasLabel(menu, "Spawn Ped"), "and Spawn Ped is in it");

    // The noclip row refuses and puts itself back rather than pretending to be on.
    selectLabel(menu, "Noclip (V)");
    menu.handleKey(tw::Key::Select);
    check(!menu.item("Noclip (V)")->toggled, "the noclip row does not stay on");
    check(menu.status() == "Noclip: this build cannot fly the player",
          "and says why: " + menu.status());

    // Same for the spawner: an entry, and a status line that explains itself.
    selectLabel(menu, "Spawn Ped");
    menu.handleKey(tw::Key::Select);
    check(menu.status() == "Spawn Ped: this build cannot spawn characters",
          "the spawn row says why: " + menu.status());

    // The model list row exists from the first frame so syncState can fill it in later;
    // with no spawner it has no choices and says so when it is picked.
    const tw::MenuItem* list = menu.item("Ped model (list)");
    check(list && list->options.empty(), "the model list is there, with nothing in it yet");
}

// The menu's appearance is a plain value object, so it can be checked without a GPU: the
// clamps that keep a settings file from making the menu unreadable, and a save/load round
// trip that a restart depends on.
void testMenuStyle() {
    tw::MenuStyle style;                 // the shipped look
    check(style.themeIndex() == 0, "the shipped theme is Triad");
    check(style.background() == 0, "the shipped background is the dragon");
    check(style.backgroundAlpha() > 0.5f && style.backgroundAlpha() < 0.6f,
          "at a little over half strength");
    check(style.scale() == 1.0f, "and the text at its normal size");

    // Every theme is a complete set of colours, and none of them is invisible.
    check(tw::MenuThemeCount() == 4, "four themes");
    for (int i = 0; i < tw::MenuThemeCount(); ++i) {
        const tw::MenuColours& c = tw::MenuThemeAt(i);
        const std::string name = tw::MenuThemeNames()[i];
        check(c.text[3] > 0.9f, name + ": the row text is opaque");
        check(c.text[0] + c.text[1] + c.text[2] > 1.5f, name + ": and light enough to read");
        check(c.selected[3] > 0.3f, name + ": the selection bar is visible");
        check(c.panel[3] > 0.5f, name + ": the panel hides the game behind it");
    }
    check(tw::MenuThemeAt(-5).text[0] == tw::MenuThemeAt(0).text[0], "a low index clamps");
    check(tw::MenuThemeAt(99).text[0] == tw::MenuThemeAt(3).text[0], "a high index clamps");

    // The enum and the names are two lists that have to agree: the model stores an index,
    // the renderer switches on it, and a mismatch would draw the wrong picture without
    // anything failing. This is the check that pins them together.
    check(tw::MenuBackgroundCount() == 4, "four backgrounds");
    check(tw::MenuBackgroundCount() == static_cast<int>(tw::MenuBackground::Count),
          "and the names cover every enum value");
    check(std::string(tw::MenuBackgroundNames()[static_cast<int>(tw::MenuBackground::Dragon)]) ==
              "Dragon",
          "index 0 is the dragon");
    check(std::string(tw::MenuBackgroundNames()[static_cast<int>(tw::MenuBackground::DragonTiled)]) ==
              "Dragon (tiled)",
          "index 1 is the tiled dragon");
    check(std::string(tw::MenuBackgroundNames()[static_cast<int>(tw::MenuBackground::Pattern)]) ==
              "Pattern",
          "index 2 is the pattern");
    check(std::string(tw::MenuBackgroundNames()[static_cast<int>(tw::MenuBackground::None)]) ==
              "None",
          "index 3 is no picture");

    // Clamping, including the values a NaN slider could produce.
    style.setThemeIndex(99);
    check(style.themeIndex() == 3, "the theme index clamps at the top");
    style.setBackground(-1);
    check(style.background() == 0, "the background index clamps at zero");
    style.setBackgroundAlpha(4.0f);
    check(style.backgroundAlpha() == 1.0f, "contrast clamps to full");
    style.setBackgroundAlpha(-2.0f);
    check(style.backgroundAlpha() == 0.0f, "and to nothing");
    style.setScale(9.0f);
    check(style.scale() == tw::MenuStyle::kMaxScale, "the text cannot grow past its limit");
    style.setScale(0.0f);
    check(style.scale() == tw::MenuStyle::kMinScale, "nor shrink below it");

    // Reset puts every value back, and says whether it had anything to do.
    tw::MenuStyle fresh;
    check(!fresh.reset(), "resetting the shipped look changes nothing");
    fresh.setThemeIndex(2);
    fresh.setScale(1.4f);
    check(fresh.reset(), "resetting a changed look says so");
    check(fresh.themeIndex() == 0 && fresh.scale() == 1.0f, "and puts everything back");

    // The file format: save then load must come back to the same numbers.
    tw::MenuStyle saved;
    saved.setThemeIndex(1);
    saved.setBackground(0);
    saved.setBackgroundAlpha(0.25f);
    saved.setScale(1.25f);
    const std::string text = saved.save();
    tw::MenuStyle loaded;
    check(loaded.load(text), "a saved style loads");
    check(loaded.themeIndex() == 1 && loaded.background() == 0, "with its indices");
    check(loaded.backgroundAlpha() == 0.25f, "its contrast");
    check(loaded.scale() == 1.25f, "and its text size");
    check(loaded.save() == text, "and saves back to the same line");

    // A hand-edited file: unknown keys are ignored, missing ones keep their defaults, and
    // out-of-range values are clamped rather than accepted.
    tw::MenuStyle edited;
    check(edited.load("theme=3;scale=99;nonsense=4"), "a partial line still loads");
    check(edited.themeIndex() == 3, "the key it knew was used");
    check(edited.scale() == tw::MenuStyle::kMaxScale, "the out-of-range value was clamped");
    check(edited.background() == 0, "the key it did not mention kept its default");
    check(!edited.load(""), "an empty file changes nothing");
    check(!edited.load("hello"), "and neither does a line with no keys in it");
    check(edited.themeIndex() == 3, "and neither one disturbs what was there");

    // The rows: they write the style, and the style is the only copy.
    tw::MenuStyle live;
    int toggles = 0, picks = 0, levels = 0;
    std::vector<std::string> spawned;
    tw::DebugHooks hooks = fullHooks(&toggles, &picks, &levels, &spawned);
    hooks.style = &live;
    tw::Menu menu;
    menu.setVisible(true);
    tw::buildDebugMenu(menu, hooks);

    check(hasHeader(menu, "APPEARANCE"), "there is an APPEARANCE section");
    for (const char* label : { "Colours", "Background", "Background contrast", "Text size",
                               "Reset appearance" }) {
        check(hasLabel(menu, label), std::string("the appearance row ") + label + " is there");
    }

    // Colours: the picker opens on the current one and browsing it applies at once.
    const tw::MenuItem* colours = menu.item("Colours");
    check(colours && colours->options.size() == 4, "every theme is offered");
    check(colours && colours->optionIndex == 0, "the picker opens on Triad");
    selectLabel(menu, "Colours");
    menu.handleKey(tw::Key::Right);
    check(live.themeIndex() == 1, "moving the picker writes the style");
    check(menu.status() == "Colours: Neon", "and says which one");

    // Background, the same shape.
    selectLabel(menu, "Background");
    menu.handleKey(tw::Key::Right);
    check(live.background() == static_cast<int>(tw::MenuBackground::DragonTiled),
          "the background picker writes the style");
    check(menu.item("Background")->optionIndex ==
              static_cast<std::size_t>(tw::MenuBackground::DragonTiled),
          "and the row shows the picture it landed on");

    // The sliders write the values their labels promise, and the style clamps them.
    selectLabel(menu, "Background contrast");
    menu.handleKey(tw::Key::Left);
    check(live.backgroundAlpha() < 0.55f, "the contrast slider moves the style");
    selectLabel(menu, "Text size");
    menu.handleKey(tw::Key::Right);
    check(live.scale() > 1.0f, "the text size slider moves the style");

    // Reset puts the object back and moves both pickers with it, so the rows cannot end up
    // describing a look that is no longer being drawn.
    selectLabel(menu, "Reset appearance");
    menu.handleKey(tw::Key::Select);
    check(live.themeIndex() == 0 && live.background() == 0, "Reset restores the shipped look");
    check(menu.item("Colours")->optionIndex == 0, "the colours picker follows the reset");
    check(menu.item("Background")->optionIndex == 0, "and so does the background picker");
    check(menu.status() == "Appearance: back to the shipped look", "and says so");

    // A build with no style object simply has no appearance section - the menu is still
    // fully usable, it just cannot be restyled.
    tw::DebugHooks bare = fullHooks(&toggles, &picks, &levels, &spawned);
    bare.style = nullptr;
    tw::Menu plain;
    tw::buildDebugMenu(plain, bare);
    check(!hasHeader(plain, "APPEARANCE"), "no style object, no appearance section");
}

}  // namespace

int main() {
    testStructure();
    testCursorSkipsHeaders();
    testActions();
    testSpawnPed();
    testPlayerFree();
    testNoclip();
    testSparseHooks();
    testWeatherControls();
    testSkydome();
    testHologram();
    testCameraModes();
    testSectionJump();
    testMissingHooksStillListed();
    testMenuStyle();
    testUnlockCatalog();
    testUnlockProfile();
    testUnlockKeepsProgress();
    std::printf("menu: %d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// menu_core.h - platform-independent debug menu model for Triad Wars.
// No Windows or game API calls here, so it can be built and tested anywhere.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tw {

enum class Key { None, Up, Down, Left, Right, Select, Back, Toggle };

enum class ItemKind { Action, Toggle, Slider, Selector };

struct MenuItem {
    std::string label;
    ItemKind kind = ItemKind::Action;
    bool toggled = false;             // for Toggle
    float value = 0.0f;               // for Slider
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float step = 0.1f;
    std::vector<std::string> options; // for Selector
    std::size_t optionIndex = 0;      // for Selector
    std::function<void()> onAction;   // Action: runs on Select
    std::function<void(bool)> onToggle;   // Toggle: receives new state
    std::function<void(float)> onChange;  // Slider: receives new value
    // Selector: runs on Select with the current option; returns the status
    // line to display (empty string keeps the previous status).
    std::function<std::string(const std::string&)> onPick;
};

class Menu {
public:
    // Adds a submenu-free item list. Items keep insertion order.
    void addItem(MenuItem item);
    void clear();

    // Feed one key press. Returns true if the menu consumed it.
    bool handleKey(Key key);

    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }
    void toggleVisible() { visible_ = !visible_; }

    std::size_t selectedIndex() const { return selected_; }
    void select(std::size_t i) { if (i < items_.size()) selected_ = i; }
    const std::vector<MenuItem>& items() const { return items_; }

    // Text lines for a renderer to draw. The selected row starts with "> ".
    std::vector<std::string> renderLines() const;

    // Status line shown at the bottom of the menu (last action result).
    void setStatus(std::string s) { status_ = std::move(s); }
    const std::string& status() const { return status_; }

private:
    std::vector<MenuItem> items_;
    std::size_t selected_ = 0;
    bool visible_ = false;
    std::string status_;
};

// Builds the default debug menu. The game-specific callbacks are passed in
// so the core does not depend on any particular memory layout or address.
//
// String-returning hooks return the FULL status line, e.g. "Spawn: done" or
// "Spawn failed: game system not ready". Selector hooks are only called with
// a non-empty option; the wrapper in buildDebugMenu turns an empty option
// list into "<name>: no assets known".
struct DebugHooks {
    std::vector<std::string> vehicleNames;
    std::vector<std::string> npcNames;
    std::vector<std::string> weaponNames;
    std::vector<std::string> weatherPresets;
    std::function<std::string(const std::string&)> spawnVehicle;
    std::function<std::string(const std::string&)> spawnNpc;
    std::function<std::string(const std::string&)> spawnWeapon;
    std::function<std::string(const std::string&)> setWeather;
    std::vector<std::string> levelTargets;          // Enforcer/Rackets/Triad/Rivals
    std::function<std::string(const std::string&, int)> setLevel;  // target, 1..50
    std::vector<std::string> shaderPresets;
    std::function<std::string(const std::string&)> setShader;
    std::function<std::string()> teleportToWaypoint;
    std::function<std::string(bool)> setFreeCam;   // free camera on/off, returns the status line
    // NPC spawn: the model and the attitude are picked in the menu, and the spawn itself
    // is carried out by the camera thread (it needs the game's per-thread allocator), so
    // the hook returns a queued line rather than the result.
    std::function<std::string(const std::string&, bool)> spawnNpcInFront;  // model, hostile
    std::function<void(bool)> setInvincible;
    std::function<void(bool)> setHudHidden;
    std::function<void(float)> setTimeOfDay;   // 0.0 .. 24.0
    std::function<void()> reloadConfig;
    std::function<std::string()> unlockAllContent;
};

void buildDebugMenu(Menu& menu, const DebugHooks& hooks);

}  // namespace tw

// menu_core.h - platform-independent debug menu model for Triad Wars.
// No Windows or game API calls here, so it can be built and tested anywhere.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tw {

enum class Key { None, Up, Down, Left, Right, Select, Back, Toggle };

enum class ItemKind { Action, Toggle, Slider, Selector, Input };

struct MenuItem {
    std::string label;
    ItemKind kind = ItemKind::Action;
    // A section title: drawn on its own, never selected and never acted on. It is
    // there so a twelve-item list reads as four short ones.
    bool header = false;
    bool toggled = false;             // for Toggle
    float value = 0.0f;               // for Slider
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float step = 0.1f;
    std::string text;                 // for Input: the text being edited
    std::vector<std::string> options; // for Selector
    std::size_t optionIndex = 0;      // for Selector
    std::function<void()> onAction;   // Action: runs on Select
    // Selector: applies the new choice as soon as Left/Right changes it, rather than
    // waiting for Enter. Used where the choice is a setting rather than a command.
    bool applyOnChange = false;
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
    // Selecting a section title lands on the next real item instead, so the cursor is
    // always on something that can be used.
    void select(std::size_t i) {
        if (i >= items_.size()) return;
        for (std::size_t k = i; k < items_.size(); ++k) {
            if (!items_[k].header) { selected_ = k; return; }
        }
        if (items_[i].header) return;
        selected_ = i;
    }
    const std::vector<MenuItem>& items() const { return items_; }
    // The entry with this label, or null. Writable so the renderer can put what was typed
    // back into the model; everything else goes through the hooks and the key handling.
    MenuItem* item(const std::string& label);

    // Text lines for a renderer to draw. The selected row starts with "> ".
    std::vector<std::string> renderLines() const;

    // Status line shown at the bottom of the menu (last action result).
    void setStatus(std::string s) { status_ = std::move(s); }
    const std::string& status() const { return status_; }

    // Keep an entry in step with something that can also be changed from outside the menu
    // (a hotkey, or the game itself). Both return false when there is no such entry, so a
    // caller can stay unconditional.
    bool setToggled(const std::string& label, bool on);
    bool setOptionIndex(const std::string& label, std::size_t index);
    // The text of an Input entry, set from the renderer while it is being typed in and
    // from a selector when one fills it in.
    bool setInputText(const std::string& label, std::string text);
    const std::string* inputText(const std::string& label) const;

private:
    void step(int delta);   // move the cursor, skipping section titles

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
    // Everything below is optional: an entry with no hook is simply not built, except the
    // Free Camera toggle, which is listed either way and says so if it cannot work.

    // Camera.
    std::function<std::string(bool)> setFreeCam;   // on/off, returns the status line
    std::function<std::string(bool)> setNoclip;    // fly the player themselves
    std::vector<std::string> noclipSpeeds;         // "1x" .. "8x"; empty: no speed entry
    std::function<std::string(const std::string&)> setNoclipSpeed;

    // World.
    std::vector<std::string> weatherPresets;
    std::function<std::string(const std::string&)> setWeather;
    std::vector<std::string> shaderPresets;
    std::function<std::string(const std::string&)> setShader;

    // Spawn: the name is typed in the menu, or picked from the list, and the spawner puts
    // the character three metres in front of the player.
    std::vector<std::string> pedNames;          // "Ped model (list)"; empty: no list
    std::string pedDefault;                     // what the name field starts with
    std::function<std::string(const std::string&)> spawnPed;

    // Progression.
    std::vector<std::string> levelTargets;          // Enforcer/Rackets/Triad/Rivals
    std::function<std::string(const std::string&, int)> setLevel;  // target, 1..50
    std::function<std::string()> unlockAllContent;
    std::function<std::string()> unlockRaids;

    // Called once per frame before the menu is drawn, so that a toggle flipped by a hotkey
    // (U for noclip, or the camera being switched off elsewhere) shows the truth. This is
    // the one hook that is not the result of something the player did in the menu.
    std::function<void()> syncState;
};

void buildDebugMenu(Menu& menu, const DebugHooks& hooks);

}  // namespace tw

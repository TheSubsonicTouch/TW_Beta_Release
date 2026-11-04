// menu_core.h - platform-independent debug menu model for Triad Wars.
// No Windows or game API calls here, so it can be built and tested anywhere.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tw {

enum class Key { None, Up, Down, Left, Right, Select, Back, Toggle };

// Info is a row that shows something and does nothing: a value the game owns, which the
// menu can see but not set (how many keyframes are recorded, whether the spawner is
// blocked). The cursor steps over it exactly as it steps over a section title, so Up and
// Down still move from one usable entry to the next.
enum class ItemKind { Action, Toggle, Slider, Selector, Input, Info };

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
    std::string text;                 // Input: the text being edited; Info: the value shown
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

// ---------------------------------------------------------------------------
// How the menu looks.
// ---------------------------------------------------------------------------
// A plain value object, held by the renderer and handed to buildDebugMenu, so the rows
// that change it and the code that draws with it are looking at the same numbers with no
// round trip through the game thread. Everything here is clamped on the way in: a saved
// setting from an older build, or a slider dragged past its end, must not be able to make
// the menu unreadable.
struct MenuColours {
    float accent[4];      // the title, the selection edge, the hotkeys in the footer
    float text[4];        // the rows themselves
    float selected[4];    // the bar behind the selected row
    float header[4];      // section titles
    float status[4];      // the status line at the bottom
    float panel[4];       // the window's own background, behind the artwork
};

const char* const* MenuThemeNames();     // { "Triad", "Neon", "Ink", "Jade" }
int MenuThemeCount();
const MenuColours& MenuThemeAt(int index);   // clamped

// Which picture is drawn behind the rows, and how. Both artworks are in the DLL: the
// dragon large enough to fill the window, and the small pattern to tile behind it.
//
// The order here IS the order of MenuBackgroundNames() and of the row's choices - the two
// are checked against each other by the tests, because an index that means one thing to the
// model and another to the renderer is a background that silently draws the wrong picture.
enum class MenuBackground { Dragon = 0, DragonTiled, Pattern, None, Count };
const char* const* MenuBackgroundNames();    // { "Dragon", "Dragon (tiled)", "Pattern", "None" }
int MenuBackgroundCount();                   // matches MenuBackground::Count

class MenuStyle {
public:
    // The defaults: the Triad theme, the dragon, its artwork at a bit over half strength.
    MenuStyle();

    int themeIndex() const { return theme_; }
    void setThemeIndex(int i);
    const MenuColours& colours() const { return MenuThemeAt(theme_); }

    // 0 is the dragon stretched to cover the window, 1 the same picture tiled, 2 the small
    // pattern tiled, 3 nothing (the flat panel, which is what the text is readable against).
    int background() const { return background_; }
    void setBackground(int i);

    // How strongly the artwork shows through, 0..1. The row's default is the value the
    // menu shipped with; anything above keeps the text legible by tinting the panel too.
    float backgroundAlpha() const { return alpha_; }
    void setBackgroundAlpha(float a);

    // Text and row size, 0.85 (a little smaller than shipped) to 1.6.
    float scale() const { return scale_; }
    void setScale(float s);

    // Back to the values above. Returns true when anything actually changed.
    bool reset();

    // "theme=0;bg=1;alpha=0.55;scale=1.00". save() always produces a string that load()
    // reads back to the same values; load() returns false and changes nothing when the
    // string is not one of ours.
    std::string save() const;
    bool load(const std::string& text);

    static constexpr float kMinScale = 0.85f;
    static constexpr float kMaxScale = 1.60f;

private:
    int theme_ = 0;
    int background_ = 0;      // the dragon, which is what this build ships with
    float alpha_ = 0.55f;
    float scale_ = 1.0f;
};

// ---------------------------------------------------------------------------
// The skybox override.
// ---------------------------------------------------------------------------
// Also a plain value object, and for the same reason: the rows that set the sky, the K
// and Y hotkeys that switch it, and the renderer that paints it are three writers of one
// set of numbers. Nothing here touches the game - it only says what the sky should be -
// so the tests can check the clamps and the wrapping without a running Triad Wars.
class SkySettings {
public:
    SkySettings();

    // The switch. Off means the game owns its sky and whatever this module borrowed has
    // been put back.
    bool enabled() const { return enabled_; }
    void setEnabled(bool on) { enabled_ = on; }

    // Which of the game's skydomes to hold. 0 is "Automatic" - the game's own choice -
    // and 1..variantCount()-1 are the entries of its own list. The count arrives from the
    // game once the world has loaded, which is why it is set from outside.
    int variant() const { return variant_; }
    void setVariant(int i);
    int variantCount() const { return variantCount_; }
    void setVariantCount(int n);
    void nextVariant();                       // wraps, and wraps past Automatic too

    // Index into SkyColourName(). Moving a channel below selects Custom by itself.
    int colour() const { return colour_; }
    void setColour(int i);

    // How far toward the colour the sky is taken: 0 leaves it alone, 1 is all tint.
    float strength() const { return strength_; }
    void setStrength(float s);

    // The custom colour, 0..1 per channel.
    float channel(int i) const;
    void setChannel(int i, float v);

    // The colour to paint with: the preset's, or the custom channels'.
    void tint(float* r, float* g, float* b) const;

    static int ColourCount();
    static const char* ColourName(int i);
    static bool IsCustom(int i);              // the last entry of the list
    static void PresetTint(int i, float* r, float* g, float* b);

    static constexpr float kMinStrength = 0.0f;
    static constexpr float kMaxStrength = 1.0f;

    // "Golden hour at 45%", "custom 1.00/0.62/0.30 at 20%": one line for the menu's own
    // status, kept stable because the tests read it.
    std::string describe() const;

private:
    bool enabled_ = false;
    int variant_ = 0;          // Automatic
    int variantCount_ = 0;
    int colour_ = 1;           // Golden hour, so switching it on shows something at once
    float strength_ = 0.45f;
    float rgb_[3] = { 1.0f, 0.78f, 0.48f };
};

class Menu {
public:
    // Adds a submenu-free item list. Items keep insertion order.
    void addItem(MenuItem item);
    void clear();

    // Feed one key press. Returns true if the menu consumed it.
    bool handleKey(Key key);

    // Move the cursor to the first usable entry of the next (+1) or previous (-1) section,
    // wrapping at the ends. PageDown and PageUp: the menu is longer than a short screen, and
    // walking it a row at a time to reach SPAWN is how a feature ends up never being found.
    bool jumpSection(int delta);

    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }
    void toggleVisible() { visible_ = !visible_; }

    std::size_t selectedIndex() const { return selected_; }
    // Selecting a section title, or an Info row, lands on the next entry that can be used
    // instead, so the cursor is never on something that is only there to be read.
    void select(std::size_t i) {
        if (i >= items_.size()) return;
        for (std::size_t k = i; k < items_.size(); ++k) {
            if (!items_[k].header && items_[k].kind != ItemKind::Info) {
                selected_ = k;
                return;
            }
        }
        if (items_[i].header || items_[i].kind == ItemKind::Info) return;
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
    // Replaces a selector's choices in place. The skydome list only exists once the world
    // has loaded, so its entry is built with "Automatic" alone and filled in later; the
    // cursor is kept on whatever is selected now if that choice survives the change.
    bool setOptions(const std::string& label, std::vector<std::string> options);
    // The text of an Input entry, set from the renderer while it is being typed in and
    // from a selector when one fills it in.
    bool setInputText(const std::string& label, std::string text);
    // The value of an Info row. Called every frame from syncState, so it must be free of
    // side effects when the text has not changed - it returns false when there was no row.
    bool setInfo(const std::string& label, std::string text);
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
    std::function<std::string(bool)> setPlayerFree; // give the player the controls
    std::function<std::string(bool)> setNoclip;    // fly the player themselves
    std::vector<std::string> noclipSpeeds;         // "1x" .. "8x"; empty: no speed entry
    std::function<std::string(const std::string&)> setNoclipSpeed;

    // The rest of the camera. Freeze holds it in place while the player moves, follow pans
    // it along with the player, look-at aims it at the player from a freeze, and the fly
    // speed is how many metres a second it travels. Every one of these is state the camera
    // thread owns, so the hook queues the change and the row is kept honest by syncState.
    std::function<std::string(bool)> setFreeze;
    std::function<bool()> frozen;
    std::function<std::string(bool)> setFollow;
    std::function<bool()> following;
    std::function<std::string(bool)> setLookAtPlayer;
    std::function<bool()> lookingAtPlayer;
    std::function<std::string(float)> setFlySpeed;
    std::function<float()> flySpeed;

    // The recorded path. "Record" takes the camera's current pose, "play" flies the path
    // back over the time it was recorded, and "clear" forgets it. The speeds are the same
    // ladder the left and right arrows walk.
    std::function<std::string()> recordKeyframe;
    std::function<std::string(bool)> setPathPlaying;
    std::function<bool()> pathPlaying;
    std::function<std::string()> clearPath;
    std::vector<std::string> playbackSpeeds;            // "0.1x" .. "4x"
    std::function<std::string(int)> setPlaybackSpeed;   // index into playbackSpeeds
    std::function<int()> keyframeCount;                 // for the Info row

    // World. The weather is a continuous 0..2 amount in the game, so the presets are only
    // shortcuts to interesting points of it; the sliders below cover everything between.
    std::vector<std::string> weatherPresets;
    std::function<std::string(const std::string&)> setWeather;
    // Rain amount 0..2. Present only when the weather object can be reached, which is why
    // every weather control below is gated on its own hook rather than on the presets.
    std::function<std::string(float)> setWeatherAmount;
    std::function<std::string(float)> setWeatherChance;    // 0..1
    std::function<std::string(float)> setWeatherWetness;   // 0..1
    std::function<std::string(int)> setWeatherInterval;    // seconds between changes
    std::function<std::string(bool)> setWeatherLock;       // stop the game changing it
    std::function<float()> weatherAmount;                  // for the sliders' start position
    std::function<float()> weatherChance;
    std::function<float()> weatherWetness;
    std::function<bool()> weatherLocked;

    std::vector<std::string> shaderPresets;
    std::function<std::string(const std::string&)> setShader;

    // Spawn: the name is typed in the menu, or picked from the list, and the spawner puts
    // the character three metres in front of the player.
    std::vector<std::string> pedNames;          // "Ped model (list)"; empty: no list
    std::string pedDefault;                     // what the name field starts with
    std::function<std::string(const std::string&)> spawnPed;
    // Which side the spawned NPC is on. H flips the same flag in game, so the entry is kept
    // in step by syncState rather than being trusted to remember.
    std::function<std::string(bool)> setSpawnHostile;
    std::function<bool()> spawnHostile;

    // The hologram: a wireframe figure standing on the exact spot the next spawn would use,
    // labelled with the model it would spawn. Drawn over the game view, not in the menu, so
    // both answers are readable without the menu covering them.
    std::function<std::string(bool)> setHologramPreview;
    std::function<bool()> hologramPreviewActive;
    std::function<std::string(float)> setHologramDistance;   // 0..10 metres ahead
    std::function<float()> hologramDistance;

    // What the spawner is doing, shown rather than set: ready, blocked after a fault, or
    // the result of the last spawn. Reset is R.
    std::function<std::string()> spawnerStatus;
    std::function<std::string()> resetSpawner;

    // The model list is as long as the game files make it: the executable names a handful
    // and a scan of the installed data names the rest. The scan runs in the background, so
    // one row starts it again and another says how many models there are now.
    std::function<std::string()> rescanPedModels;
    std::function<std::string()> pedModelSummary;

    // The menu's own appearance, when this build can draw more than one look. The rows
    // write it directly, so the next frame is drawn with the new setting.
    MenuStyle* style = nullptr;

    // The sky. The settings object is the same one the K and Y hotkeys and the renderer
    // read (see freecam_sky.h), so the menu and the keys cannot disagree. The variant
    // names are the game's own skydome list, which is empty until the world has loaded;
    // the rows are refreshed from syncState once it is not.
    SkySettings* sky = nullptr;
    std::vector<std::string> skyVariants;      // "Automatic" then the game's skydomes
    std::function<std::string()> skyState;     // what the override is doing, for the Info row

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

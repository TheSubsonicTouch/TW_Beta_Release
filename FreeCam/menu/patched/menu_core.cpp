#include "menu_core.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace tw {

// ---------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------

namespace {

// Four looks, each a set of six colours. They are deliberately conservative - a menu over a
// dark game has to stay readable at every setting - so even the loudest one is a tint
// rather than a slab of colour.
const MenuColours kThemes[] = {
    // Triad: the colours the menu shipped with - gold on bone, red bars, cyan status.
    { { 0.93f, 0.78f, 0.42f, 1.0f },
      { 0.94f, 0.92f, 0.88f, 1.0f },
      { 0.55f, 0.10f, 0.10f, 0.55f },
      { 0.86f, 0.24f, 0.22f, 1.0f },
      { 0.55f, 0.85f, 0.92f, 1.0f },
      { 0.05f, 0.05f, 0.06f, 0.86f } },
    // Neon: the hologram's colours, for anyone who liked them on the spawn figure.
    { { 0.45f, 0.95f, 1.00f, 1.0f },
      { 0.88f, 0.97f, 1.00f, 1.0f },
      { 0.10f, 0.40f, 0.55f, 0.55f },
      { 0.35f, 0.80f, 0.95f, 1.0f },
      { 0.75f, 0.98f, 1.00f, 1.0f },
      { 0.03f, 0.07f, 0.10f, 0.88f } },
    // Ink: no colour at all - grey on near-black, for the least intrusive look.
    { { 0.85f, 0.85f, 0.85f, 1.0f },
      { 0.95f, 0.95f, 0.95f, 1.0f },
      { 0.25f, 0.25f, 0.25f, 0.60f },
      { 0.70f, 0.70f, 0.70f, 1.0f },
      { 0.80f, 0.80f, 0.80f, 1.0f },
      { 0.02f, 0.02f, 0.02f, 0.92f } },
    // Jade: the other end of the same street.
    { { 0.62f, 0.90f, 0.62f, 1.0f },
      { 0.92f, 0.98f, 0.92f, 1.0f },
      { 0.10f, 0.40f, 0.22f, 0.55f },
      { 0.42f, 0.80f, 0.52f, 1.0f },
      { 0.70f, 0.95f, 0.75f, 1.0f },
      { 0.03f, 0.07f, 0.05f, 0.88f } },
};
const int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));
const char* const kThemeNames[] = { "Triad", "Neon", "Ink", "Jade" };

const char* const kBackgroundNames[] = { "Dragon", "Dragon (tiled)", "Pattern", "None" };

constexpr float kMinAlpha = 0.0f;
constexpr float kMaxAlpha = 1.0f;

float clampf(float v, float lo, float hi) {
    if (!(v > lo)) v = lo;      // also catches NaN: !(NaN > lo) is true
    if (v > hi) v = hi;
    return v;
}

}  // namespace

const char* const* MenuThemeNames() { return kThemeNames; }

int MenuThemeCount() { return kThemeCount; }

const MenuColours& MenuThemeAt(int index) {
    if (index < 0) index = 0;
    if (index >= kThemeCount) index = kThemeCount - 1;
    return kThemes[index];
}

const char* const* MenuBackgroundNames() { return kBackgroundNames; }

int MenuBackgroundCount() { return static_cast<int>(MenuBackground::Count); }

// ---- the skybox override -------------------------------------------------

// The presets are named after what they do to a sky rather than after a colour chip: the
// sky underneath is not white, so "golden" is a direction to move it in, not a colour to
// replace it with. "Custom" is last, and is what the tint sliders select.
struct SkyColour {
    const char* name;
    float r, g, b;
};
const SkyColour kSkyColours[] = {
    { "Natural",     1.00f, 1.00f, 1.00f },   // the sky as the game draws it
    { "Golden hour", 1.00f, 0.78f, 0.48f },
    { "Warm amber",  1.00f, 0.62f, 0.30f },
    { "Rose dusk",   1.00f, 0.55f, 0.62f },
    { "Violet",      0.72f, 0.55f, 1.00f },
    { "Arctic",      0.62f, 0.85f, 1.00f },
    { "Emerald",     0.55f, 1.00f, 0.72f },
    { "Sepia",       0.92f, 0.86f, 0.72f },
    { "Crimson",     1.00f, 0.40f, 0.38f },
    { "Moonlight",   0.65f, 0.72f, 1.00f },
    { "Acid",        0.80f, 1.00f, 0.45f },
    { "Custom",      1.00f, 1.00f, 1.00f },   // the tint sliders; their values are used
};
const int kSkyColourCount = static_cast<int>(sizeof kSkyColours / sizeof kSkyColours[0]);

SkySettings::SkySettings() = default;

void SkySettings::setVariant(int i) {
    if (i < 0) i = 0;
    // Before the world has loaded there is no list to clamp to; the count is only trusted
    // once the game has handed one over.
    if (variantCount_ > 0 && i >= variantCount_) i = variantCount_ - 1;
    variant_ = i;
}

void SkySettings::setVariantCount(int n) {
    variantCount_ = n < 0 ? 0 : n;
    if (variantCount_ > 0 && variant_ >= variantCount_) variant_ = variantCount_ - 1;
    if (variant_ < 0) variant_ = 0;
}

void SkySettings::nextVariant() {
    // Automatic is a choice like any other, so the cycle runs -1, 0, 1, ... and wraps.
    // With no list yet there is nothing to cycle through.
    const int count = variantCount_ > 0 ? variantCount_ : 1;
    variant_ = (variant_ + 1) % count;
}

void SkySettings::setColour(int i) {
    if (i < 0) i = 0;
    if (i >= kSkyColourCount) i = kSkyColourCount - 1;
    colour_ = i;
    if (!IsCustom(i)) PresetTint(i, &rgb_[0], &rgb_[1], &rgb_[2]);
}

void SkySettings::setStrength(float s) { strength_ = clampf(s, kMinStrength, kMaxStrength); }

float SkySettings::channel(int i) const {
    if (i < 0) i = 0;
    if (i > 2) i = 2;
    return rgb_[i];
}

void SkySettings::setChannel(int i, float v) {
    if (i < 0) i = 0;
    if (i > 2) i = 2;
    rgb_[i] = clampf(v, 0.0f, 1.0f);
    // Moving a slider is asking for a colour the presets do not have, so the picker says
    // so rather than claiming the sky is still "Golden hour" at values that are not.
    colour_ = kSkyColourCount - 1;
}

void SkySettings::tint(float* r, float* g, float* b) const {
    const float* src = rgb_;
    float preset[3] = { 1.0f, 1.0f, 1.0f };
    if (!IsCustom(colour_)) {
        PresetTint(colour_, &preset[0], &preset[1], &preset[2]);
        src = preset;
    }
    if (r) *r = clampf(src[0], 0.0f, 1.0f);
    if (g) *g = clampf(src[1], 0.0f, 1.0f);
    if (b) *b = clampf(src[2], 0.0f, 1.0f);
}

int SkySettings::ColourCount() { return kSkyColourCount; }

const char* SkySettings::ColourName(int i) {
    if (i < 0) i = 0;
    if (i >= kSkyColourCount) i = kSkyColourCount - 1;
    return kSkyColours[i].name;
}

bool SkySettings::IsCustom(int i) { return i == kSkyColourCount - 1; }

void SkySettings::PresetTint(int i, float* r, float* g, float* b) {
    if (i < 0) i = 0;
    if (i >= kSkyColourCount) i = kSkyColourCount - 1;
    if (r) *r = kSkyColours[i].r;
    if (g) *g = kSkyColours[i].g;
    if (b) *b = kSkyColours[i].b;
}

std::string SkySettings::describe() const {
    char buf[128];
    if (IsCustom(colour_)) {
        std::snprintf(buf, sizeof buf, "custom %.2f/%.2f/%.2f at %.0f%%",
                      static_cast<double>(rgb_[0]), static_cast<double>(rgb_[1]),
                      static_cast<double>(rgb_[2]), static_cast<double>(strength_ * 100.0f));
    } else {
        std::snprintf(buf, sizeof buf, "%s at %.0f%%", ColourName(colour_),
                      static_cast<double>(strength_ * 100.0f));
    }
    return buf;
}

MenuStyle::MenuStyle() = default;

void MenuStyle::setThemeIndex(int i) {
    if (i < 0) i = 0;
    if (i >= MenuThemeCount()) i = MenuThemeCount() - 1;
    theme_ = i;
}

void MenuStyle::setBackground(int i) {
    if (i < 0) i = 0;
    if (i >= MenuBackgroundCount()) i = MenuBackgroundCount() - 1;
    background_ = i;
}

void MenuStyle::setBackgroundAlpha(float a) { alpha_ = clampf(a, kMinAlpha, kMaxAlpha); }

void MenuStyle::setScale(float s) { scale_ = clampf(s, kMinScale, kMaxScale); }

bool MenuStyle::reset() {
    const MenuStyle defaults;
    const bool changed = theme_ != defaults.theme_ || background_ != defaults.background_ ||
                         alpha_ != defaults.alpha_ || scale_ != defaults.scale_;
    *this = defaults;
    return changed;
}

std::string MenuStyle::save() const {
    char buf[128];
    std::snprintf(buf, sizeof buf, "theme=%d;bg=%d;alpha=%.2f;scale=%.2f", theme_,
                  background_, static_cast<double>(alpha_), static_cast<double>(scale_));
    return buf;
}

// A fixed, tolerant reader rather than anything clever: four keys, any order, unknown keys
// ignored, and every value clamped. A hand-edited file, or one written by an older build,
// loads what it can instead of losing every setting at once.
bool MenuStyle::load(const std::string& text) {
    if (text.empty()) return false;
    MenuStyle parsed;
    bool any = false;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(';', at);
        if (end == std::string::npos) end = text.size();
        const std::string pair = text.substr(at, end - at);
        at = end + 1;
        const std::size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        std::string key = pair.substr(0, eq);
        const std::string value = pair.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (key.empty() || value.empty()) continue;
        char* stop = nullptr;
        const double number = std::strtod(value.c_str(), &stop);
        if (stop == value.c_str()) continue;           // not a number: leave this one alone
        if (key == "theme") { parsed.setThemeIndex(static_cast<int>(number)); any = true; }
        else if (key == "bg") { parsed.setBackground(static_cast<int>(number)); any = true; }
        else if (key == "alpha") { parsed.setBackgroundAlpha(static_cast<float>(number)); any = true; }
        else if (key == "scale") { parsed.setScale(static_cast<float>(number)); any = true; }
    }
    if (!any) return false;
    *this = parsed;
    return true;
}

void Menu::addItem(MenuItem item) { items_.push_back(std::move(item)); }

void Menu::clear() {
    items_.clear();
    selected_ = 0;
}

// Section titles and Info rows are not selectable, so Up and Down step over both. A menu
// that is only titles (or empty) leaves the cursor alone.
namespace {
bool passive(const MenuItem& it) {
    return it.header || it.kind == ItemKind::Info;
}
}  // namespace

void Menu::step(int delta) {
    if (items_.empty()) return;
    const int n = static_cast<int>(items_.size());
    int i = static_cast<int>(selected_);
    for (int guard = 0; guard < n; ++guard) {
        i = (i + delta + n) % n;
        if (!passive(items_[i])) {
            selected_ = static_cast<std::size_t>(i);
            return;
        }
    }
}

bool Menu::jumpSection(int delta) {
    if (items_.empty() || delta == 0) return false;

    // The section starts, in order. Landing on a section means its first entry, so a jump
    // never leaves the cursor on a title or an Info row.
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].header) starts.push_back(i);
    }
    if (starts.empty()) {
        step(delta > 0 ? 1 : -1);
        return true;
    }

    std::size_t here = 0;
    for (std::size_t k = 0; k < starts.size(); ++k) {
        if (starts[k] <= selected_) here = k;
    }
    std::size_t target;
    if (delta > 0) {
        target = (here + 1 < starts.size()) ? here + 1 : 0;
    } else {
        target = (here > 0) ? here - 1 : starts.size() - 1;
    }

    for (std::size_t i = starts[target]; i < items_.size(); ++i) {
        if (!passive(items_[i])) {
            selected_ = i;
            return true;
        }
    }
    return false;
}

bool Menu::handleKey(Key key) {
    if (key == Key::Toggle) {
        toggleVisible();
        return true;
    }
    if (!visible_ || items_.empty()) return false;
    // The first entry can be a section title; put the cursor on a real item before any
    // key is acted on.
    if (selected_ >= items_.size()) selected_ = 0;
    if (passive(items_[selected_])) step(1);

    switch (key) {
    case Key::Up:
        step(-1);
        return true;
    case Key::Down:
        step(1);
        return true;
    case Key::Left:
    case Key::Right: {
        MenuItem& it = items_[selected_];
        // An Input's text is edited with the keyboard itself, so the arrows are the
        // caret's while it is being typed in - not a value to step.
        if (it.header || it.kind == ItemKind::Input || it.kind == ItemKind::Info) return true;
        float dir = (key == Key::Right) ? 1.0f : -1.0f;
        if (it.kind == ItemKind::Slider) {
            it.value = std::clamp(it.value + dir * it.step, it.minValue, it.maxValue);
            if (it.onChange) it.onChange(it.value);
        } else if (it.kind == ItemKind::Selector && !it.options.empty()) {
            std::size_t n = it.options.size();
            if (dir > 0.0f)
                it.optionIndex = (it.optionIndex + 1) % n;
            else
                it.optionIndex = (it.optionIndex + n - 1) % n;
            // A setting, not a command: the new choice takes effect as it is browsed.
            if (it.applyOnChange && it.onPick) {
                std::string status = it.onPick(it.options[it.optionIndex]);
                if (!status.empty()) status_ = std::move(status);
            }
        }
        return true;
    }
    case Key::Select: {
        MenuItem& it = items_[selected_];
        if (passive(it)) return true;
        switch (it.kind) {
        case ItemKind::Action:
        case ItemKind::Input:
            if (it.onAction) it.onAction();
            break;
        case ItemKind::Toggle:
            it.toggled = !it.toggled;
            if (it.onToggle) it.onToggle(it.toggled);
            break;
        case ItemKind::Slider:
            break;
        case ItemKind::Selector:
            if (it.onPick) {
                std::string opt;
                if (!it.options.empty()) opt = it.options[it.optionIndex];
                std::string status = it.onPick(opt);
                if (!status.empty()) status_ = std::move(status);
            }
            break;
        case ItemKind::Info:
            break;        // nothing to run, and it is not selectable anyway
        }
        return true;
    }
    case Key::Back:
        visible_ = false;
        return true;
    default:
        return false;
    }
}

std::vector<std::string> Menu::renderLines() const {
    std::vector<std::string> lines;
    if (!visible_) return lines;

    lines.push_back("TriadMenu");
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const MenuItem& it = items_[i];
        char buf[512];
        if (it.header) {
            std::snprintf(buf, sizeof buf, "  -- %s --", it.label.c_str());
            lines.emplace_back(buf);
            continue;
        }
        const char* prefix = (i == selected_) ? "> " : "  ";
        switch (it.kind) {
        case ItemKind::Action:
            std::snprintf(buf, sizeof buf, "%s%s", prefix, it.label.c_str());
            break;
        case ItemKind::Toggle:
            std::snprintf(buf, sizeof buf, "%s%s [%s]", prefix, it.label.c_str(),
                          it.toggled ? "ON" : "OFF");
            break;
        case ItemKind::Slider:
            std::snprintf(buf, sizeof buf, "%s%s < %.2f >", prefix, it.label.c_str(), it.value);
            break;
        case ItemKind::Selector: {
            const char* opt = it.options.empty() ? "-" : it.options[it.optionIndex].c_str();
            std::snprintf(buf, sizeof buf, "%s%s < %s >", prefix, it.label.c_str(), opt);
            break;
        }
        case ItemKind::Input:
            std::snprintf(buf, sizeof buf, "%s%s: %s", prefix, it.label.c_str(),
                          it.text.empty() ? "(type a name)" : it.text.c_str());
            break;
        case ItemKind::Info:
            // No cursor marker: it is never the selected row.
            std::snprintf(buf, sizeof buf, "  %s: %s", it.label.c_str(),
                          it.text.empty() ? "-" : it.text.c_str());
            break;
        }
        lines.emplace_back(buf);
    }
    if (!status_.empty()) lines.push_back("Status: " + status_);
    return lines;
}

MenuItem* Menu::item(const std::string& label) {
    for (MenuItem& it : items_)
        if (!it.header && it.label == label) return &it;
    return nullptr;
}

bool Menu::setInputText(const std::string& label, std::string text) {
    MenuItem* it = item(label);
    if (!it || it->kind != ItemKind::Input) return false;
    it->text = std::move(text);
    return true;
}

const std::string* Menu::inputText(const std::string& label) const {
    for (const MenuItem& it : items_)
        if (!it.header && it.label == label && it.kind == ItemKind::Input) return &it.text;
    return nullptr;
}

bool Menu::setInfo(const std::string& label, std::string text) {
    for (MenuItem& it : items_) {
        if (!it.header && it.label == label && it.kind == ItemKind::Info) {
            if (it.text == text) return true;      // called every frame: no churn when equal
            it.text = std::move(text);
            return true;
        }
    }
    return false;
}

bool Menu::setToggled(const std::string& label, bool on) {
    for (MenuItem& it : items_) {
        if (!it.header && it.label == label && it.kind == ItemKind::Toggle) {
            it.toggled = on;
            return true;
        }
    }
    return false;
}

bool Menu::setOptionIndex(const std::string& label, std::size_t index) {
    for (MenuItem& it : items_) {
        if (it.header || it.label != label || it.kind != ItemKind::Selector) continue;
        if (it.options.empty()) return true;
        const std::size_t clamped = index < it.options.size() ? index : it.options.size() - 1;
        it.optionIndex = clamped;
        return true;
    }
    return false;
}

bool Menu::setOptions(const std::string& label, std::vector<std::string> options) {
    for (MenuItem& it : items_) {
        if (it.header || it.label != label || it.kind != ItemKind::Selector) continue;
        if (options == it.options) return true;          // nothing changed: leave the cursor
        // Keep the current choice when it is still offered, so a refresh does not move the
        // cursor off the skydome the player just picked.
        const std::string current =
            it.options.empty() ? std::string() : it.options[it.optionIndex];
        it.options = std::move(options);
        it.optionIndex = 0;
        for (std::size_t i = 0; i < it.options.size(); ++i) {
            if (it.options[i] == current) { it.optionIndex = i; break; }
        }
        return true;
    }
    return false;
}

void buildDebugMenu(Menu& menu, const DebugHooks& hooks) {
    menu.clear();

    // Section titles. They are drawn but never selected, so Up and Down still walk the
    // whole menu in one motion - the titles only break it up on screen.
    auto header = [&menu](const char* text) {
        MenuItem it;
        it.label = text;
        it.header = true;
        menu.addItem(it);
    };

    auto makeSelector = [&menu](const std::string& name,
                                std::vector<std::string> options,
                                std::function<std::string(const std::string&)> fn) {
        MenuItem it;
        it.label = name;
        it.kind = ItemKind::Selector;
        it.options = std::move(options);
        it.onPick = [&menu, name, fn](const std::string& opt) -> std::string {
            if (!fn) return name + ": not available in this build";
            if (opt.empty()) return name + ": no assets known";
            return fn(opt);
        };
        return it;
    };

    // ---- Camera ----------------------------------------------------------
    header("CAMERA");

    MenuItem freecam;
    freecam.label = "Free Camera";
    freecam.kind = ItemKind::Toggle;
    freecam.onToggle = [&menu, fn = hooks.setFreeCam](bool on) {
        if (!fn) { menu.setStatus("Free camera: not available in this build"); return; }
        menu.setStatus(fn(on));
    };
    menu.addItem(freecam);

    // The player's own controls. With the camera flying they are held back, so the
    // keys are the camera's; this hands them to the character without moving the
    // camera, and the hotkey U does the same. It is listed only when the build can do
    // it, like the noclip pair below.
    if (hooks.setPlayerFree) {
        MenuItem playerFree;
        playerFree.label = "Player free (U)";
        playerFree.kind = ItemKind::Toggle;
        playerFree.onToggle = [&menu, fn = hooks.setPlayerFree](bool on) {
            menu.setStatus(fn(on));
        };
        menu.addItem(playerFree);
    }

    {
        // Always listed, for the same reason the SPAWN section is: a build that cannot fly
        // the player says so on the row instead of leaving it out and looking unfinished.
        MenuItem noclip;
        noclip.label = "Noclip (V)";
        noclip.kind = ItemKind::Toggle;
        noclip.onToggle = [&menu, fn = hooks.setNoclip](bool on) {
            if (fn) {
                menu.setStatus(fn(on));
                return;
            }
            menu.setToggled("Noclip (V)", false);   // it did not happen
            menu.setStatus("Noclip: this build cannot fly the player");
        };
        menu.addItem(noclip);
    }
    if (hooks.setNoclipSpeed && !hooks.noclipSpeeds.empty()) {
        MenuItem speed = makeSelector("Noclip speed", hooks.noclipSpeeds,
                                      hooks.setNoclipSpeed);
        speed.applyOnChange = true;   // the speed applies as it is browsed
        menu.addItem(speed);
    }

    // The other three camera modes, each the same action its hotkey runs. Freeze is the
    // one that hands the player the controls while the view holds still; follow is the
    // camera panning with them; look-at is a frozen camera turning to keep them in frame.
    if (hooks.setFreeze) {
        MenuItem freeze;
        freeze.label = "Freeze camera (O)";
        freeze.kind = ItemKind::Toggle;
        freeze.toggled = hooks.frozen ? hooks.frozen() : false;
        freeze.onToggle = [&menu, fn = hooks.setFreeze](bool on) { menu.setStatus(fn(on)); };
        menu.addItem(freeze);
    }
    if (hooks.setFollow) {
        MenuItem follow;
        follow.label = "Follow player (P)";
        follow.kind = ItemKind::Toggle;
        follow.toggled = hooks.following ? hooks.following() : false;
        follow.onToggle = [&menu, fn = hooks.setFollow](bool on) { menu.setStatus(fn(on)); };
        menu.addItem(follow);
    }
    if (hooks.setLookAtPlayer) {
        MenuItem look;
        look.label = "Look at player (P)";
        look.kind = ItemKind::Toggle;
        look.toggled = hooks.lookingAtPlayer ? hooks.lookingAtPlayer() : false;
        look.onToggle = [&menu, fn = hooks.setLookAtPlayer](bool on) { menu.setStatus(fn(on)); };
        menu.addItem(look);
    }
    if (hooks.setFlySpeed) {
        MenuItem fly;
        fly.label = "Fly speed";
        fly.kind = ItemKind::Slider;
        fly.minValue = 1.0f;
        fly.maxValue = 60.0f;
        fly.step = 1.0f;
        fly.value = hooks.flySpeed ? hooks.flySpeed() : 30.0f;
        fly.onChange = [&menu, fn = hooks.setFlySpeed](float v) { menu.setStatus(fn(v)); };
        menu.addItem(fly);
    }

    // ---- Spawn -----------------------------------------------------------
    // The section is always here, even in a build whose spawner did not come up. A missing
    // entry looks like a feature that was never built; an entry that says why it cannot
    // work is an answer, and it is the only thing that can be acted on.
    //
    // "Spawn Ped" spawns whatever character is named in it, at the distance the preview is
    // standing at, in front of the player. The name is typed into the entry itself - any
    // property set the game knows, not just the ones this build guesses - and the list
    // underneath fills it in for the names that are already known (built-ins and
    // FreeCam.npc.txt).
    header("SPAWN");

    if (hooks.spawnPed) {
        auto spawnPedFn = hooks.spawnPed;

        MenuItem ped;
        ped.label = "Spawn Ped";
        ped.kind = ItemKind::Input;
        ped.text = hooks.pedDefault;
        ped.onAction = [&menu, spawnPedFn]() {
            const MenuItem* it = menu.item("Spawn Ped");
            if (!it) return;
            // A name typed by hand tends to arrive with a space on the end.
            std::string name = it->text;
            const std::size_t first = name.find_first_not_of(" \t");
            const std::size_t last = name.find_last_not_of(" \t");
            name = (first == std::string::npos) ? std::string() : name.substr(first, last - first + 1);
            if (name.empty()) {
                menu.setStatus("Spawn Ped: type a model name first");
                return;
            }
            menu.setStatus(spawnPedFn(name));
        };
        menu.addItem(ped);

        // The same spawn as a plain button. The row above is a text field: it takes typing
        // and Enter, which is how a name that is not on the list gets in. This is the one
        // that can be clicked and does the obvious thing with what is already there.
        MenuItem now;
        now.label = "Spawn now (B)";
        now.kind = ItemKind::Action;
        now.onAction = [&menu, spawnPedFn]() {
            const MenuItem* it = menu.item("Spawn Ped");
            if (!it) return;
            std::string name = it->text;
            const std::size_t first = name.find_first_not_of(" \t");
            const std::size_t last = name.find_last_not_of(" \t");
            name = (first == std::string::npos) ? std::string() : name.substr(first, last - first + 1);
            if (name.empty()) {
                menu.setStatus("Spawn Ped: pick a model first");
                return;
            }
            menu.setStatus(spawnPedFn(name));
        };
        menu.addItem(now);
    } else {
        MenuItem ped;
        ped.label = "Spawn Ped";
        ped.kind = ItemKind::Action;
        ped.onAction = [&menu]() {
            menu.setStatus("Spawn Ped: this build cannot spawn characters");
        };
        menu.addItem(ped);
    }

    {
        // The row is there even when the list is empty: the spawner builds its list from the
        // game folder, which can still be unreadable when the menu is built, and syncState
        // refills the choices as soon as there are any. A selector that appears late would
        // be a row that is missing exactly while someone is looking for it.
        MenuItem list = makeSelector("Ped model (list)", hooks.pedNames,
                                     [&menu](const std::string& name) {
                                         menu.setInputText("Spawn Ped", name);
                                         return "Spawn Ped: " + name;
                                     });
        list.applyOnChange = true;
        menu.addItem(list);
    }

    // The model list is only as good as what has been read out of the game, and reading it
    // is a walk over the installed data that takes a while. So the row starts it in the
    // background, and the line under it says what the list holds now.
    if (hooks.rescanPedModels) {
        MenuItem scan;
        scan.label = "Rescan models";
        scan.kind = ItemKind::Action;
        scan.onAction = [&menu, fn = hooks.rescanPedModels]() { menu.setStatus(fn()); };
        menu.addItem(scan);
    }

    if (hooks.pedModelSummary) {
        MenuItem count;
        count.label = "Models found";
        count.kind = ItemKind::Info;
        count.text = hooks.pedModelSummary();
        menu.addItem(count);
    }

    if (hooks.setSpawnHostile) {
        MenuItem side;
        side.label = "Hostile NPC";
        side.kind = ItemKind::Toggle;
        side.toggled = hooks.spawnHostile ? hooks.spawnHostile() : false;
        side.onToggle = [&menu, fn = hooks.setSpawnHostile](bool on) {
            menu.setStatus(fn(on));
        };
        menu.addItem(side);
    }

    // The hologram. It is a drawing, not a spawn: nothing is created to show it, and it
    // follows the name in the field above, so what the figure says is what the button
    // would put down. The distance is the same number SpawnRequestAt takes.
    if (hooks.setHologramPreview) {
        MenuItem preview;
        preview.label = "Spawn preview";
        preview.kind = ItemKind::Toggle;
        preview.toggled = hooks.hologramPreviewActive ? hooks.hologramPreviewActive() : false;
        preview.onToggle = [&menu, fn = hooks.setHologramPreview](bool on) {
            menu.setStatus(fn(on));
        };
        menu.addItem(preview);
    }
    if (hooks.setHologramDistance) {
        MenuItem dist;
        dist.label = "Preview distance";
        dist.kind = ItemKind::Slider;
        dist.minValue = 0.0f;
        dist.maxValue = 10.0f;
        dist.step = 1.0f;
        dist.value = hooks.hologramDistance ? hooks.hologramDistance() : 3.0f;
        dist.onChange = [&menu, fn = hooks.setHologramDistance](float v) {
            menu.setStatus(fn(v));
        };
        menu.addItem(dist);
    }

    // What the spawner is doing - ready, waiting, or switched off after a fault - and R,
    // which is the only way back on after a fault. The status is shown rather than guessed
    // at, because a spawner that quietly does nothing is the worst state it can be in.
    if (hooks.spawnerStatus) {
        MenuItem state;
        state.label = "Spawner";
        state.kind = ItemKind::Info;
        state.text = hooks.spawnerStatus();
        menu.addItem(state);
    }
    if (hooks.resetSpawner) {
        MenuItem reset;
        reset.label = "Reset spawner (R)";
        reset.kind = ItemKind::Action;
        reset.onAction = [&menu, fn = hooks.resetSpawner]() { menu.setStatus(fn()); };
        menu.addItem(reset);
    }

    // ---- Props ------------------------------------------------------------
    // The same spawner as the section above, pointed at the objects instead of the people:
    // vehicles, weapons and every prop the game files name. The rows are the NPC ones - a
    // list, a button, a rescan and a count - because the two do the same thing and should
    // not have to be learned twice.
    if (hooks.spawnProp) {
        header("PROPS");

        if (!hooks.propNames.empty() || hooks.selectProp) {
            MenuItem list = makeSelector(
                "Prop model (list)", hooks.propNames, [&menu, fn = hooks.selectProp](const std::string& name) {
                    return fn ? fn(name) : (std::string("Prop: ") + name);
                });
            list.applyOnChange = true;
            menu.addItem(list);
        }

        MenuItem spawn;
        spawn.label = "Spawn prop (G)";
        spawn.kind = ItemKind::Action;
        spawn.onAction = [&menu, fn = hooks.spawnProp]() { menu.setStatus(fn()); };
        menu.addItem(spawn);

        if (hooks.setPropPreview) {
            MenuItem preview;
            preview.label = "Preview the prop";
            preview.kind = ItemKind::Toggle;
            preview.toggled = hooks.propPreviewActive ? hooks.propPreviewActive() : false;
            preview.onToggle = [&menu, fn = hooks.setPropPreview](bool on) {
                menu.setStatus(fn(on));
            };
            menu.addItem(preview);
        }

        if (hooks.rescanProps) {
            MenuItem scan;
            scan.label = "Rescan props";
            scan.kind = ItemKind::Action;
            scan.onAction = [&menu, fn = hooks.rescanProps]() { menu.setStatus(fn()); };
            menu.addItem(scan);
        }

        if (hooks.propSummary) {
            MenuItem count;
            count.label = "Props found";
            count.kind = ItemKind::Info;
            count.text = hooks.propSummary();
            menu.addItem(count);
        }
    }

    // ---- Path -------------------------------------------------------------
    // What the camera flew, played back: the same three keys (L/J/M) as entries, plus the
    // speed ladder the arrows walk. The Info row is the count, which is the one thing
    // needed to know whether a playback is possible at all.
    const bool anyPath = hooks.recordKeyframe || hooks.setPathPlaying || hooks.clearPath ||
                         !hooks.playbackSpeeds.empty() || hooks.keyframeCount;
    if (anyPath) {
        header("PATH");

        if (hooks.recordKeyframe) {
            MenuItem record;
            record.label = "Record keyframe (L)";
            record.kind = ItemKind::Action;
            record.onAction = [&menu, fn = hooks.recordKeyframe]() { menu.setStatus(fn()); };
            menu.addItem(record);
        }
        if (hooks.setPathPlaying) {
            MenuItem play;
            play.label = "Play path (J)";
            play.kind = ItemKind::Toggle;
            play.toggled = hooks.pathPlaying ? hooks.pathPlaying() : false;
            play.onToggle = [&menu, fn = hooks.setPathPlaying](bool on) {
                menu.setStatus(fn(on));
            };
            menu.addItem(play);
        }
        if (hooks.clearPath) {
            MenuItem clear;
            clear.label = "Clear path (M)";
            clear.kind = ItemKind::Action;
            clear.onAction = [&menu, fn = hooks.clearPath]() { menu.setStatus(fn()); };
            menu.addItem(clear);
        }
        if (!hooks.playbackSpeeds.empty()) {
            // applyOnChange: a speed is a setting, and hearing the shot at the new pace is
            // the whole point of changing it.
            MenuItem speeds = makeSelector("Playback speed", hooks.playbackSpeeds,
                                           [&menu, fn = hooks.setPlaybackSpeed,
                                            options = hooks.playbackSpeeds]
                                           (const std::string& label) -> std::string {
                                               if (!fn) return "Playback speed: not available";
                                               for (std::size_t i = 0; i < options.size(); ++i) {
                                                   if (options[i] == label) return fn(static_cast<int>(i));
                                               }
                                               return "Playback speed: " + label;
                                           });
            speeds.applyOnChange = true;
            menu.addItem(speeds);
        }
        if (hooks.keyframeCount) {
            MenuItem count;
            count.label = "Keyframes";
            count.kind = ItemKind::Info;
            const int n = hooks.keyframeCount();
            char buf[64];
            std::snprintf(buf, sizeof buf, "%d recorded", n);
            count.text = buf;
            menu.addItem(count);
        }
    }

    // ---- World -----------------------------------------------------------
    const bool anyWeather = !hooks.weatherPresets.empty() || hooks.setWeatherAmount ||
                            hooks.setWeatherChance || hooks.setWeatherWetness ||
                            hooks.setWeatherInterval || hooks.setWeatherLock;
    const bool anyWorld = anyWeather || !hooks.shaderPresets.empty();

    if (anyWorld) header("WORLD");

    if (!hooks.weatherPresets.empty()) {
        MenuItem weather = makeSelector("Weather", hooks.weatherPresets, hooks.setWeather);
        weather.applyOnChange = true;
        menu.addItem(weather);
    }

    // The amount slider is the real control: the presets above only jump to named points of
    // this range, and the game itself clamps to the same 0..2.
    if (hooks.setWeatherAmount) {
        MenuItem amount;
        amount.label = "Rain amount";
        amount.kind = ItemKind::Slider;
        amount.minValue = 0.0f;
        amount.maxValue = 2.0f;
        amount.step = 0.1f;
        amount.value = hooks.weatherAmount ? hooks.weatherAmount() : 0.0f;
        amount.onChange = [&menu, fn = hooks.setWeatherAmount](float v) {
            menu.setStatus(fn(v));
        };
        menu.addItem(amount);
    }

    if (hooks.setWeatherChance) {
        MenuItem chance;
        chance.label = "Rain chance";
        chance.kind = ItemKind::Slider;
        chance.minValue = 0.0f;
        chance.maxValue = 1.0f;
        chance.step = 0.05f;
        chance.value = hooks.weatherChance ? hooks.weatherChance() : 0.0f;
        chance.onChange = [&menu, fn = hooks.setWeatherChance](float v) {
            menu.setStatus(fn(v));
        };
        menu.addItem(chance);
    }

    if (hooks.setWeatherWetness) {
        MenuItem wet;
        wet.label = "Wetness override";
        wet.kind = ItemKind::Slider;
        wet.minValue = 0.0f;
        wet.maxValue = 1.0f;
        wet.step = 0.05f;
        wet.value = hooks.weatherWetness ? hooks.weatherWetness() : 0.0f;
        wet.onChange = [&menu, fn = hooks.setWeatherWetness](float v) {
            menu.setStatus(fn(v));
        };
        menu.addItem(wet);
    }

    if (hooks.setWeatherInterval) {
        MenuItem every;
        every.label = "Weather change interval";
        every.kind = ItemKind::Slider;
        every.minValue = 0.0f;
        every.maxValue = 600.0f;
        every.step = 30.0f;
        every.value = 0.0f;
        every.onChange = [&menu, fn = hooks.setWeatherInterval](float v) {
            menu.setStatus(fn(static_cast<int>(v + 0.5f)));
        };
        menu.addItem(every);
    }

    if (hooks.setWeatherLock) {
        MenuItem lock;
        lock.label = "Lock weather";
        lock.kind = ItemKind::Toggle;
        lock.toggled = hooks.weatherLocked ? hooks.weatherLocked() : false;
        lock.onToggle = [&menu, fn = hooks.setWeatherLock](bool on) {
            menu.setStatus(fn(on));
        };
        menu.addItem(lock);
    }

    if (!hooks.shaderPresets.empty()) {
        menu.addItem(makeSelector("ReShade preset", hooks.shaderPresets, hooks.setShader));
    }

    // ---- Sky -------------------------------------------------------------
    // One switch, the variant it holds and the colour it paints. The rows write the
    // settings object directly; freecam_sky.cpp holds the skydome and win_menu.cpp draws
    // the paint, so nothing here queues a job or blocks a frame.
    if (hooks.sky) {
        SkySettings* sky = hooks.sky;
        header("SKY");

        {
            MenuItem on;
            on.label = "Sky override";
            on.kind = ItemKind::Toggle;
            on.toggled = sky->enabled();
            on.onToggle = [sky, &menu](bool want) {
                sky->setEnabled(want);
                if (!want) {
                    menu.setStatus("Sky override: off - the game has its sky back");
                    return;
                }
                const MenuItem* row = menu.item("Sky variant");
                const std::size_t index = static_cast<std::size_t>(sky->variant());
                const std::string name = (row && index < row->options.size())
                                             ? row->options[index]
                                             : std::string("Automatic");
                menu.setStatus("Sky override: on - holding " + name + ", painted " +
                               sky->describe() + " over the sky");
            };
            menu.addItem(on);
        }

        {
            MenuItem variant;
            variant.label = "Sky variant";
            variant.kind = ItemKind::Selector;
            variant.options = hooks.skyVariants.empty()
                                  ? std::vector<std::string>{ "Automatic" }
                                  : hooks.skyVariants;
            // The list is the game's, so it also tells the settings how many skies there
            // are: without it "next" would have nothing to wrap round.
            sky->setVariantCount(static_cast<int>(variant.options.size()));
            variant.optionIndex = static_cast<std::size_t>(sky->variant());
            if (variant.optionIndex >= variant.options.size()) variant.optionIndex = 0;
            variant.applyOnChange = true;
            variant.onPick = [sky, &menu](const std::string& opt) -> std::string {
                const MenuItem* self = menu.item("Sky variant");
                const std::size_t index = self ? self->optionIndex : 0;
                sky->setVariant(static_cast<int>(index));
                // Picking a sky and leaving the override off would be picking it and
                // watching the weather take it away again, so the pick turns it on.
                const bool wasOn = sky->enabled();
                sky->setEnabled(true);
                return "Sky variant: " + opt + (wasOn ? " - held" : " - override on, so it stays");
            };
            menu.addItem(variant);
        }

        {
            MenuItem next;
            next.label = "Next sky";
            next.kind = ItemKind::Action;
            next.onAction = [sky, &menu]() {
                sky->nextVariant();
                sky->setEnabled(true);
                const MenuItem* row = menu.item("Sky variant");
                const std::size_t index = static_cast<std::size_t>(sky->variant());
                const std::string name = (row && index < row->options.size())
                                             ? row->options[index]
                                             : std::string("Automatic");
                menu.setStatus("Sky variant: " + name);
            };
            menu.addItem(next);
        }

        {
            std::vector<std::string> colours;
            for (int i = 0; i < SkySettings::ColourCount(); ++i)
                colours.emplace_back(SkySettings::ColourName(i));
            MenuItem colour = makeSelector(
                "Sky colour", colours, [sky](const std::string& name) {
                    for (int i = 0; i < SkySettings::ColourCount(); ++i) {
                        if (name != SkySettings::ColourName(i)) continue;
                        sky->setColour(i);
                        return "Sky colour: " + name +
                               (sky->enabled() ? std::string()
                                               : std::string(" - switch Sky override on"));
                    }
                    return std::string("Sky colour: ") + name;
                });
            colour.optionIndex = static_cast<std::size_t>(sky->colour());
            colour.applyOnChange = true;
            menu.addItem(colour);
        }

        {
            MenuItem strength;
            strength.label = "Sky colour strength";
            strength.kind = ItemKind::Slider;
            strength.minValue = SkySettings::kMinStrength;
            strength.maxValue = SkySettings::kMaxStrength;
            strength.step = 0.05f;
            strength.value = sky->strength();
            strength.onChange = [sky, &menu](float v) {
                sky->setStrength(v);
                char buf[64];
                std::snprintf(buf, sizeof buf, "Sky colour strength: %.0f%%",
                              static_cast<double>(v * 100.0f));
                menu.setStatus(buf);
            };
            menu.addItem(strength);
        }

        // The three channels. Picking a preset above fills these in; moving one of them
        // selects "Custom", so what the picker says is always what the sky is doing.
        static const char* const kChannelLabels[3] = { "Tint red", "Tint green", "Tint blue" };
        for (int c = 0; c < 3; ++c) {
            MenuItem channel;
            channel.label = kChannelLabels[c];
            channel.kind = ItemKind::Slider;
            channel.minValue = 0.0f;
            channel.maxValue = 1.0f;
            channel.step = 0.05f;
            channel.value = sky->channel(c);
            channel.onChange = [sky, &menu, c](float v) {
                sky->setChannel(c, v);
                char buf[96];
                std::snprintf(buf, sizeof buf, "%s: %.0f%% - custom colour", kChannelLabels[c],
                              static_cast<double>(v * 100.0f));
                menu.setStatus(buf);
            };
            menu.addItem(channel);
        }

        {
            MenuItem state;
            state.label = "Sky";
            state.kind = ItemKind::Info;
            state.text = hooks.skyState ? hooks.skyState() : std::string();
            menu.addItem(state);
        }
    }

    // ---- Wardrobe --------------------------------------------------------
    //
    // The wardrobe screen only offers the pieces the game files give a shop category, so
    // an outfit's own trousers cannot be worn with another outfit's top - the Deus Ex
    // trousers and the Swat top being the example that started this. Every piece here is
    // one the client already draws; "wearing" one rewrites the profile's piece list, and
    // the slot that was picked is the only slot it touches.
    if (!hooks.wardrobeTops.empty() || !hooks.wardrobePants.empty()) {
        header("WARDROBE");

        {
            MenuItem worn;
            worn.label = "Worn now";
            worn.kind = ItemKind::Info;
            worn.text = hooks.wardrobeStatus ? hooks.wardrobeStatus() : std::string();
            menu.addItem(worn);
        }

        auto wearSlot = [&menu](const char* label, std::vector<std::string> options,
                                std::function<std::string(const std::string&)> fn,
                                const char* what) {
            MenuItem it;
            it.label = label;
            it.kind = ItemKind::Selector;
            it.options = std::move(options);
            it.onPick = [&menu, fn, what](const std::string& opt) -> std::string {
                if (!fn) return std::string(what) + ": not available in this build";
                if (opt.empty()) return std::string(what) + ": no pieces known";
                return fn(opt);
            };
            menu.addItem(it);
        };

        wearSlot("Top", hooks.wardrobeTops, hooks.wearTop, "Top");
        wearSlot("Pants", hooks.wardrobePants, hooks.wearPants, "Pants");

        {
            // The point of the pickers: these two together, whatever the game files think
            // of either garment on its own.
            MenuItem wear;
            wear.label = "Wear this top and these pants";
            wear.kind = ItemKind::Action;
            auto combo = hooks.wearCombo;
            wear.onAction = [&menu, combo]() {
                if (!combo) {
                    menu.setStatus("Wardrobe: this build cannot write the profile");
                    return;
                }
                std::string top, pants;
                if (const MenuItem* row = menu.item("Top"))
                    if (!row->options.empty()) top = row->options[row->optionIndex];
                if (const MenuItem* row = menu.item("Pants"))
                    if (!row->options.empty()) pants = row->options[row->optionIndex];
                menu.setStatus(combo(top, pants));
            };
            menu.addItem(wear);
        }

        if (!hooks.wardrobeLooks.empty()) {
            MenuItem look;
            look.label = "Look";
            look.kind = ItemKind::Selector;
            look.options = hooks.wardrobeLooks;
            menu.addItem(look);

            MenuItem wear;
            wear.label = "Wear this look";
            wear.kind = ItemKind::Action;
            auto fn = hooks.wearLook;
            wear.onAction = [&menu, fn]() {
                if (!fn) {
                    menu.setStatus("Wardrobe: this build cannot write the profile");
                    return;
                }
                std::string name;
                if (const MenuItem* row = menu.item("Look"))
                    if (!row->options.empty()) name = row->options[row->optionIndex];
                if (name.empty()) {
                    menu.setStatus("Wardrobe: no looks in the server's data");
                    return;
                }
                menu.setStatus(fn(name));
            };
            menu.addItem(wear);
        }

        {
            MenuItem undo;
            undo.label = "Undo wardrobe changes";
            undo.kind = ItemKind::Action;
            auto fn = hooks.wardrobeUndo;
            undo.onAction = [&menu, fn]() {
                menu.setStatus(fn ? fn() : std::string("Wardrobe: nothing to undo"));
            };
            menu.addItem(undo);
        }
    }

    // ---- Progression -----------------------------------------------------
    if (!hooks.levelTargets.empty() || hooks.unlockAllContent) {
        header("PROGRESSION");
    }

    if (!hooks.levelTargets.empty()) {
        const std::size_t base = menu.items().size();
        MenuItem lt;
        lt.label = "Level target";
        lt.kind = ItemKind::Selector;
        lt.options = hooks.levelTargets;
        menu.addItem(lt);

        MenuItem lv;
        lv.label = "Level value";
        lv.kind = ItemKind::Slider;
        lv.value = 1.0f;
        lv.minValue = 1.0f;
        lv.maxValue = 50.0f;
        lv.step = 1.0f;
        menu.addItem(lv);

        MenuItem apply;
        apply.label = "Apply level";
        apply.onAction = [&menu, base, fn = hooks.setLevel]() {
            if (!fn) { menu.setStatus("Level: not available in this build"); return; }
            const MenuItem& t = menu.items()[base];
            const MenuItem& v = menu.items()[base + 1];
            const std::string target =
                t.options.empty() ? std::string() : t.options[t.optionIndex];
            menu.setStatus(fn(target, static_cast<int>(v.value)));
        };
        menu.addItem(apply);
    }

    if (hooks.unlockAllContent) {
        MenuItem unlock;
        unlock.label = "Unlock all content";
        unlock.onAction = [&menu, fn = hooks.unlockAllContent]() { menu.setStatus(fn()); };
        menu.addItem(unlock);
    }

    if (hooks.unlockRaids) {
        // The cooldowns come back after every raid, so this is a separate item rather
        // than something that only happens once with the content unlock.
        MenuItem raids;
        raids.label = "Unlock raids and events";
        raids.onAction = [&menu, fn = hooks.unlockRaids]() { menu.setStatus(fn()); };
        menu.addItem(raids);
    }

    // Put the cursor on the first entry that can be used, not on the CAMERA title.
    menu.select(0);

    // ---- Appearance -------------------------------------------------------
    // The menu's own look: a theme, the artwork behind it, how strongly that artwork shows
    // and how big the rows are. None of these touch the game - they change what this window
    // draws - so they are handled here in the model rather than through a game-thread job,
    // and the renderer reads the same object on the next frame.
    if (hooks.style) {
        header("APPEARANCE");

        MenuStyle* style = hooks.style;

        {
            std::vector<std::string> themes;
            for (int i = 0; i < MenuThemeCount(); ++i) themes.emplace_back(MenuThemeNames()[i]);
            MenuItem theme = makeSelector("Colours", themes,
                                          [style, &menu](const std::string& name) {
                                              for (int i = 0; i < MenuThemeCount(); ++i) {
                                                  if (name == MenuThemeNames()[i]) {
                                                      style->setThemeIndex(i);
                                                      return "Colours: " + name;
                                                  }
                                              }
                                              return std::string("Colours: ") + name;
                                          });
            theme.optionIndex = static_cast<std::size_t>(style->themeIndex());
            theme.applyOnChange = true;   // the whole window changes as it is browsed
            menu.addItem(theme);
        }

        {
            std::vector<std::string> backgrounds;
            for (int i = 0; i < MenuBackgroundCount(); ++i)
                backgrounds.emplace_back(MenuBackgroundNames()[i]);
            MenuItem bg = makeSelector("Background", backgrounds,
                                       [style](const std::string& name) {
                                           for (int i = 0; i < MenuBackgroundCount(); ++i) {
                                               if (name == MenuBackgroundNames()[i]) {
                                                   style->setBackground(i);
                                                   return "Background: " + name;
                                               }
                                           }
                                           return std::string("Background: ") + name;
                                       });
            bg.optionIndex = static_cast<std::size_t>(style->background());
            bg.applyOnChange = true;
            menu.addItem(bg);
        }

        {
            // "Contrast" is the name that says what it is for: the artwork competes with the
            // text, so this is how much of it to let through.
            MenuItem contrast;
            contrast.label = "Background contrast";
            contrast.kind = ItemKind::Slider;
            contrast.minValue = 0.0f;
            contrast.maxValue = 1.0f;
            contrast.step = 0.05f;
            contrast.value = style->backgroundAlpha();
            contrast.onChange = [style, &menu](float v) {
                style->setBackgroundAlpha(v);
                char buf[64];
                std::snprintf(buf, sizeof buf, "Background contrast: %.0f%%",
                              static_cast<double>(v * 100.0f));
                menu.setStatus(buf);
            };
            menu.addItem(contrast);
        }

        {
            MenuItem scale;
            scale.label = "Text size";
            scale.kind = ItemKind::Slider;
            scale.minValue = MenuStyle::kMinScale;
            scale.maxValue = MenuStyle::kMaxScale;
            scale.step = 0.05f;
            scale.value = style->scale();
            scale.onChange = [style, &menu](float v) {
                style->setScale(v);
                char buf[64];
                std::snprintf(buf, sizeof buf, "Text size: %.0f%%",
                              static_cast<double>(v * 100.0f));
                menu.setStatus(buf);
            };
            menu.addItem(scale);
        }

        {
            MenuItem reset;
            reset.label = "Reset appearance";
            reset.kind = ItemKind::Action;
            reset.onAction = [style, &menu]() {
                const bool changed = style->reset();
                menu.setOptionIndex("Colours", static_cast<std::size_t>(style->themeIndex()));
                menu.setOptionIndex("Background", static_cast<std::size_t>(style->background()));
                menu.setStatus(changed ? "Appearance: back to the shipped look"
                                       : "Appearance: already the shipped look");
            };
            menu.addItem(reset);
        }
    }

}
}  // namespace tw

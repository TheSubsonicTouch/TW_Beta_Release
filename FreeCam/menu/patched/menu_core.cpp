#include "menu_core.h"

#include <algorithm>
#include <cstdio>

namespace tw {

void Menu::addItem(MenuItem item) { items_.push_back(std::move(item)); }

void Menu::clear() {
    items_.clear();
    selected_ = 0;
}

// Section titles are not selectable, so Up and Down step over them. A menu that is
// only titles (or empty) leaves the cursor alone.
void Menu::step(int delta) {
    if (items_.empty()) return;
    const int n = static_cast<int>(items_.size());
    int i = static_cast<int>(selected_);
    for (int guard = 0; guard < n; ++guard) {
        i = (i + delta + n) % n;
        if (!items_[i].header) {
            selected_ = static_cast<std::size_t>(i);
            return;
        }
    }
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
    if (items_[selected_].header) step(1);

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
        if (it.header) return true;
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
        if (it.header) return true;
        switch (it.kind) {
        case ItemKind::Action:
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
        }
        lines.emplace_back(buf);
    }
    if (!status_.empty()) lines.push_back("Status: " + status_);
    return lines;
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

    if (hooks.setNoclip) {
        MenuItem noclip;
        noclip.label = "Noclip (fly the player)";
        noclip.kind = ItemKind::Toggle;
        noclip.onToggle = [&menu, fn = hooks.setNoclip](bool on) { menu.setStatus(fn(on)); };
        menu.addItem(noclip);
    }
    if (hooks.setNoclipSpeed && !hooks.noclipSpeeds.empty()) {
        MenuItem speed = makeSelector("Noclip speed", hooks.noclipSpeeds,
                                      hooks.setNoclipSpeed);
        speed.applyOnChange = true;   // the speed applies as it is browsed
        menu.addItem(speed);
    }

    // ---- World -----------------------------------------------------------
    if (!hooks.weatherPresets.empty()) {
        header("WORLD");
        MenuItem weather = makeSelector("Weather", hooks.weatherPresets, hooks.setWeather);
        weather.applyOnChange = true;
        menu.addItem(weather);
    }

    if (!hooks.shaderPresets.empty()) {
        if (hooks.weatherPresets.empty()) header("WORLD");
        menu.addItem(makeSelector("ReShade preset", hooks.shaderPresets, hooks.setShader));
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
}
}  // namespace tw

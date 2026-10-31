#include "menu_core.h"

#include <algorithm>
#include <cstdio>

namespace tw {

void Menu::addItem(MenuItem item) { items_.push_back(std::move(item)); }

void Menu::clear() {
    items_.clear();
    selected_ = 0;
}

bool Menu::handleKey(Key key) {
    if (key == Key::Toggle) {
        toggleVisible();
        return true;
    }
    if (!visible_ || items_.empty()) return false;

    switch (key) {
    case Key::Up:
        selected_ = (selected_ == 0) ? items_.size() - 1 : selected_ - 1;
        return true;
    case Key::Down:
        selected_ = (selected_ + 1) % items_.size();
        return true;
    case Key::Left:
    case Key::Right: {
        MenuItem& it = items_[selected_];
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
        }
        return true;
    }
    case Key::Select: {
        MenuItem& it = items_[selected_];
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

    lines.push_back("Triad Wars Debug Menu");
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const MenuItem& it = items_[i];
        char buf[512];
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

void buildDebugMenu(Menu& menu, const DebugHooks& hooks) {
    menu.clear();

    auto wrapAction = [&menu](const std::string& name, std::function<void()> fn) {
        return [&menu, name, fn]() {
            if (fn) {
                fn();
                menu.setStatus(name + ": done");
            } else {
                menu.setStatus(name + ": not wired yet");
            }
        };
    };

    auto makeSelector = [&menu](const std::string& name,
                                std::vector<std::string> options,
                                std::function<std::string(const std::string&)> fn) {
        MenuItem it;
        it.label = name;
        it.kind = ItemKind::Selector;
        it.options = std::move(options);
        it.onPick = [&menu, name, fn](const std::string& opt) -> std::string {
            if (!fn) return name + ": not wired yet";
            if (opt.empty()) return name + ": no assets known";
            return fn(opt);
        };
        return it;
    };

    // Spawn selectors removed: every spawn path goes through the game's
    // per-thread allocator, which is not initialized on the render thread,
    // so spawning always faults (game+0x1F3FEE). Weather is safe (direct
    // field write) and stays.
    menu.addItem(makeSelector("Weather", hooks.weatherPresets, hooks.setWeather));

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
            if (!fn) { menu.setStatus("Level: not wired yet"); return; }
            const MenuItem& t = menu.items()[base];
            const MenuItem& v = menu.items()[base + 1];
            const std::string target =
                t.options.empty() ? std::string() : t.options[t.optionIndex];
            menu.setStatus(fn(target, static_cast<int>(v.value)));
        };
        menu.addItem(apply);
    }

    MenuItem tp;
    tp.label = "Teleport to waypoint";
    tp.onAction = [&menu, fn = hooks.teleportToWaypoint]() {
        menu.setStatus(fn ? fn() : std::string("Teleport: not wired yet"));
    };
    menu.addItem(tp);

    // Free camera: the camera leaves the player and flies on its own.
    MenuItem freecam;
    freecam.label = "Free Camera";
    freecam.kind = ItemKind::Toggle;
    freecam.onToggle = [&menu, fn = hooks.setFreeCam](bool on) {
        if (!fn) { menu.setStatus("Free camera: not wired yet"); return; }
        menu.setStatus(fn(on));
    };
    menu.addItem(freecam);

    // NPC spawn. The two selectors keep their own choice; the action reads them, the way
    // "Apply level" reads its two items. The model list is whatever the spawn module can
    // name - the character property sets the game itself names, or FreeCam.npc.txt - and
    // "hostile" means "give it a faction that is not yours".
    if (!hooks.npcNames.empty()) {
        const std::size_t spawnBase = menu.items().size();

        MenuItem model;
        model.label = "NPC model";
        model.kind = ItemKind::Selector;
        model.options = hooks.npcNames;
        menu.addItem(model);

        MenuItem attitude;
        attitude.label = "NPC attitude";
        attitude.kind = ItemKind::Selector;
        attitude.options = { "Friendly", "Hostile" };
        menu.addItem(attitude);

        MenuItem spawn;
        spawn.label = "Spawn NPC 3 m ahead";
        spawn.onAction = [&menu, spawnBase, fn = hooks.spawnNpcInFront]() {
            if (!fn) { menu.setStatus("Spawn: not wired yet"); return; }
            const MenuItem& m = menu.items()[spawnBase];
            const MenuItem& a = menu.items()[spawnBase + 1];
            const std::string choice = m.options.empty() ? std::string() : m.options[m.optionIndex];
            menu.setStatus(fn(choice, a.optionIndex == 1));
        };
        menu.addItem(spawn);
    }

    MenuItem inv;
    inv.label = "Invincible";
    inv.kind = ItemKind::Toggle;
    inv.onToggle = [&menu, fn = hooks.setInvincible](bool on) {
        if (fn) { fn(on); menu.setStatus(std::string("Invincible ") + (on ? "on" : "off")); }
        else menu.setStatus("Invincible: not wired yet");
    };
    menu.addItem(inv);

    MenuItem hud;
    hud.label = "Hide HUD";
    hud.kind = ItemKind::Toggle;
    hud.onToggle = [&menu, fn = hooks.setHudHidden](bool on) {
        if (fn) { fn(on); menu.setStatus(std::string("HUD ") + (on ? "hidden" : "shown")); }
        else menu.setStatus("Hide HUD: not wired yet");
    };
    menu.addItem(hud);

    MenuItem tod;
    tod.label = "Time of day";
    tod.kind = ItemKind::Slider;
    tod.value = 12.0f;
    tod.minValue = 0.0f;
    tod.maxValue = 24.0f;
    tod.step = 0.5f;
    tod.onChange = [&menu, fn = hooks.setTimeOfDay](float v) {
        if (fn) { fn(v); menu.setStatus("Time set"); }
        else menu.setStatus("Time of day: not wired yet");
    };
    menu.addItem(tod);

    MenuItem reload;
    reload.label = "Reload config";
    reload.onAction = wrapAction("Reload", hooks.reloadConfig);
    menu.addItem(reload);

    if (!hooks.shaderPresets.empty()) {
        menu.addItem(makeSelector("Shader", hooks.shaderPresets, hooks.setShader));
    }

    // Unlock All Content
    MenuItem unlock;
    unlock.label = "Unlock All Content";
    unlock.onAction = [&menu, fn = hooks.unlockAllContent]() {
        if (!fn) { menu.setStatus("UnlockAll: not wired yet"); return; }
        menu.setStatus(fn());
    };
    menu.addItem(unlock);

}
}  // namespace tw

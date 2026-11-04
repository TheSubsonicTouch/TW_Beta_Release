#include "menu_core.h"

#include <algorithm>
#include <cstdio>

namespace tw {

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
    const bool anyWorld = anyWeather || !hooks.shaderPresets.empty() || hooks.setSkydome;

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

    if (hooks.setSkydome) {
        // The skydome list is filled in by syncState once the world has loaded, so the
        // entry starts with the one choice that always works.
        MenuItem sky;
        sky.label = "Skybox (skydome)";
        sky.kind = ItemKind::Selector;
        sky.options = hooks.skydomes;
        if (sky.options.empty()) sky.options.push_back("Automatic");
        sky.optionIndex = hooks.skydomeCurrent ? static_cast<std::size_t>(hooks.skydomeCurrent()) : 0;
        if (sky.optionIndex >= sky.options.size()) sky.optionIndex = 0;
        sky.applyOnChange = true;
        sky.onPick = [&menu, fn = hooks.setSkydome](const std::string& opt) -> std::string {
            const MenuItem* self = menu.item("Skybox (skydome)");
            const std::size_t index = self ? self->optionIndex : 0;
            (void)opt;
            return fn(static_cast<int>(index));
        };
        menu.addItem(sky);
    }

    if (!hooks.shaderPresets.empty()) {
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

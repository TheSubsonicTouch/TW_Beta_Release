// freecam_weather.h - weather and skybox for the Triad Wars debug menu.
//
// Everything here is a field write or a call into a routine the game already exports to
// its own Skookum script layer, found by disassembling the retail executable. The RVAs
// are for the build this mod targets (TriadWars.exe, sha256 6bd58dbd...32cf7b, the same
// one LocalServer/tools/patch_exe.py pins), and the field offsets are the ones the game's
// own script bindings read and write:
//
//   weather_set_amount      0x5766a0   writes  +0x34, +0x38, +0x3c   (clamped 0..2)
//   weather_get_amount      0x576540   reads   +0x34
//   weather_is_raining      0x576580   tests   +0x34 > 1.0
//   weather_target          0x576760   writes  +0x38, +0x3c
//   weather_rain_chance     0x5765e0   writes  +0x4c                 (clamped 0..1)
//   weather_randomize_interval 0x576640 writes +0x40                 (int seconds)
//   weather_lock            0x5765b0   swaps   +0x40 <-> +0x44
//   weather_wetness_override 0x5767c0  writes  +0x74
//
// The skybox is the game's own skydome list. set_skydome (0x571c00 -> 0x6d150) picks an
// entry out of the SkyMarshall's qArray by id, so the menu can offer every skydome the
// running game actually has rather than a list guessed here.
#pragma once

#include <string>
#include <vector>

namespace tw {
namespace game {

// ---- weather ---------------------------------------------------------------------

// True when the game's weather object can be reached. Everything below is a no-op that
// returns a failure line when it cannot, so the menu can stay unconditional.
bool WeatherAvailable();

// Names for the amount presets, in the order WeatherApplyMode understands them.
std::vector<std::string> WeatherModePresets();

// Sets the rain amount by preset name. Returns the status line.
std::string WeatherApplyMode(const std::string& preset);

// The full continuous range the game clamps to: 0 = dry, 1 = rain, 2 = the cap.
std::string WeatherApplyAmount(float amount);
float WeatherAmount();

// How likely the weather system is to rain on its own, 0..1.
std::string WeatherSetChance(float chance);
float WeatherChance();

// Surface wetness, overriding what the rain would produce.
std::string WeatherSetWetness(float wetness);
float WeatherWetness();

// Locked weather neither rains nor clears on its own: the game keeps its own change
// interval in +0x44 while +0x40 is zero, which is exactly what this swaps.
bool WeatherLocked();
std::string WeatherSetLocked(bool locked);

// Seconds between the game's own weather changes. Ignored while locked, and kept in the
// saved slot so unlocking restores it.
int WeatherInterval();
std::string WeatherSetInterval(int seconds);

// ---- skybox ----------------------------------------------------------------------

// True when the SkyMarshall and its skydome list can be reached. The list is empty until
// the world has loaded, so callers should keep asking.
bool SkydomeAvailable();

// "Automatic" first, then one entry per skydome the game has. Index 0 therefore means
// "let the weather decide", which is the game's own -1.
std::vector<std::string> SkydomeNames();

// Picks one entry of SkydomeNames(). Returns the status line.
std::string SkydomeSetIndex(int index);

// The entry SkydomeNames() would show for what the game is using now, or 0.
int SkydomeCurrentMenuIndex();

}  // namespace game
}  // namespace tw

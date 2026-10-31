// freecam_noclip.h - fly the player, not the camera.
//
// The free camera moves a camera that nobody owns; this moves the character. It keeps its
// own anchor position, moves it by what the keys ask for, and writes it into the player's
// world transform every tick. Because the position is written rather than walked to, the
// game's movement code never gets a say: walls, cars and the ground are simply not
// consulted, and the character hangs where it is put instead of falling.
//
// Only the translation is written. The twelve floats of the rotation block are left alone,
// so the character keeps its facing and the animation system keeps working - the same rule
// the spawner follows, for the same reason.
#pragma once

namespace tw {
namespace game {

// Read the keys and move the player. Call once per tick from the camera thread, which is
// the one that can afford to run at a few hundred hertz. Times itself, so it does not
// depend on any other loop's clock. Returns true when it wrote the player's transform,
// which is every tick while noclip is on - that is what holds the character in the air.
bool NoclipStep();

// Turns noclip on or off. Turning it on starts the camera thread if it is not running, so
// it works with the free camera switched off and never flown. Turning it off simply stops
// writing, and the game's own physics pick the character up from wherever they are, which
// is what makes it possible to land on a roof by flying there and switching it off.
void NoclipSet(bool on);
bool NoclipActive();

// The speed multiplier, 1x to 8x, for the menu and the corner display.
float NoclipSpeed();
void NoclipSpeedStep(int direction);        // one step along the list below
void NoclipSetSpeedIndex(int index);        // 0 = the slowest
int NoclipSpeedIndex();                     // index into NoclipSpeedLabels()
const char *const *NoclipSpeedLabels();     // { "1x", ..., "8x" }
int NoclipSpeedCount();

}  // namespace game
}  // namespace tw

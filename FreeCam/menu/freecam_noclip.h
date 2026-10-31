// freecam_noclip.h - fly the player, not the camera.
//
// The free camera moves a camera that nobody owns; this moves the character. Every tick
// it reads the player's own world transform, moves it by what the keys ask for, and writes
// it straight back. Because the position is written rather than walked to, nothing in the
// way matters: walls, cars and the ground are all collided with by the game's movement
// code, which never gets to run while noclip is on.
//
// Only the translation is written. The twelve floats of the rotation block are read and
// written back untouched, so the character keeps its facing and the animation system keeps
// working - the same rule the spawner follows, for the same reason.
#pragma once

namespace tw {
namespace game {

// Read the keys and move the player. Call once per tick from the camera thread, which is
// the one that can afford to run at a few hundred hertz. Times itself, so it does not
// depend on any other loop's clock. Returns true when it moved the player.
bool NoclipStep();

// Turns noclip on or off. Switching off simply stops writing: the game's own physics pick
// the character up from wherever they are, which is what makes it possible to land on a
// roof by flying there and then switching off.
void NoclipSet(bool on);
bool NoclipActive();

// The speed multiplier the arrows change, 1x to 8x, for the HUD line.
float NoclipSpeed();
void NoclipSpeedStep(int direction);

}  // namespace game
}  // namespace tw

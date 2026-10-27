// freecam_game.h - free camera for the Triad Wars debug menu.
//
// The camera lives in the game's own camera component. Those components are
// ordinary heap objects whose first member is a vtable, and the vtables are at
// fixed RVAs in TriadWars.exe, so the object can be found exactly rather than
// guessed at. Each component keeps its world transform and its view matrix in a
// block of three 4x4s; a world matrix and a view matrix are inverses of the same
// pose, which is what tells us which of the three is which.
#pragma once

#include <string>

namespace tw {
namespace game {

// True when the game module looks like the build this was written for.
bool FreeCamAvailable();

// Switches the camera on or off. Returns the status line for the menu.
std::string FreeCamSetActive(bool on);

bool FreeCamActive();

// Per-frame housekeeping. The camera also runs its own thread because the game
// rewrites its matrices every frame and we have to win that race; this is here
// so the render loop can nudge it (and is safe to call when free cam is off).
void FreeCamTick();

void FreeCamSetSpeed(float unitsPerSecond);
float FreeCamSpeed();

}  // namespace game
}  // namespace tw

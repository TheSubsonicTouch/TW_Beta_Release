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

// True while the free camera is flying and the player must not act: the game's input
// is held back so the character cannot move, attack or interact. It is false while the
// camera is frozen (O) or following the player (P), because then the player is meant to
// be in control.
bool FreeCamBlockingInput();

// One line of the little corner display: the key that was pressed and what it did.
// alpha goes from 1 to 0 as the line gets old, so the display fades out by itself.
struct FreeCamHudLine {
    char key[8];
    char text[44];
    float alpha;
};

// Fills out[] with the hotkey presses still worth showing, newest first, and returns
// how many there are. Safe to call when free cam has never been switched on.
int FreeCamHudLines(FreeCamHudLine *out, int max);

// Starts the camera thread if it is not running yet, and says whether it is. The thread
// is the only one that has run the game's per-thread allocator setup (see freecam_spawn.h),
// so a spawn asked for while the camera has never been switched on needs it started
// first - otherwise the request sits in the queue and nothing happens.
bool FreeCamEnsureThread();

// Per-frame housekeeping. The camera also runs its own thread because the game
// rewrites its matrices every frame and we have to win that race; this is here
// so the render loop can nudge it (and is safe to call when free cam is off).
void FreeCamTick();

void FreeCamSetSpeed(float unitsPerSecond);
float FreeCamSpeed();

}  // namespace game
}  // namespace tw

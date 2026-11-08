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

extern "C" {
#include "../src/freecam.h"
}

namespace tw {
namespace game {

// True when the game module looks like the build this was written for.
bool FreeCamAvailable();

// Switches the camera on or off. Returns the status line for the menu.
std::string FreeCamSetActive(bool on);

bool FreeCamActive();

// True while the free camera is flying and the player must not act: the game's input
// is held back so the character cannot move, attack or interact. It is false while the
// camera is frozen (O), following the player (P), or after U has freed the player
// (FreeCamPlayerFree), because then the player is meant to be in control.
bool FreeCamBlockingInput();

// U, and the menu entry of the same name: hand the game's controls to the character
// while the free camera stays on. The camera keeps the angle it has and stops reading
// its own keys, so the character can walk, fight and interact without WASD also flying
// the view; setting it back freezes the player again and the camera flies from there.
// Ignored while noclip is flying the character (the two would share the keys) and
// cleared whenever the camera is switched off, frozen or unfrozen, or followed.
bool FreeCamPlayerFree();
void FreeCamSetPlayerFree(bool free);

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

// True while the debug menu is open and is the one reading the keyboard. The camera
// thread and the spawner poll GetAsyncKeyState directly, which the menu's own messages do
// not stop, so they ask this before acting on a key: typing a name into the menu must not
// fly the camera or spawn something behind the menu.
void FreeCamSetMenuOpen(bool open);
bool FreeCamMenuOpen();

// Per-frame housekeeping. The camera also runs its own thread because the game
// rewrites its matrices every frame and we have to win that race; this is here
// so the render loop can nudge it (and is safe to call when free cam is off).
void FreeCamTick();

void FreeCamSetSpeed(float unitsPerSecond);
float FreeCamSpeed();

// --- what the debug menu can ask the camera to do ----------------------------
// Every mode the keys reach is also an entry in the menu, and both go through one
// implementation. The camera thread owns the flight state, so the menu does not write it:
// it queues an action, the camera thread runs it on its next tick, and the answer is
// handed back once:
//
//     FreeCamRequestAction(kFreeCamFreezeOn);
//     ...
//     std::string why;
//     if (FreeCamTakeActionStatus(&why)) menu.setStatus(why);
//
// The two "set" actions carry a number: a playback-speed rung, and the flying speed.
enum FreeCamAction {
    kFreeCamFreezeOn = 0,
    kFreeCamFreezeOff,
    kFreeCamFollowOn,
    kFreeCamFollowOff,
    kFreeCamLookOn,
    kFreeCamLookOff,
    kFreeCamKeyframe,
    kFreeCamPlay,
    kFreeCamStop,
    kFreeCamClearPath,
    kFreeCamSetPlaySpeed,     // arg: index into FreeCamPlaySpeedAt
    kFreeCamSetFlySpeed,      // arg: metres per second
    kFreeCamSetFov,           // arg: vertical degrees
    kFreeCamActionCount
};
void FreeCamRequestAction(int action, int arg = 0);          // any thread
bool FreeCamTakeActionStatus(std::string *out);              // true once per result

// Where the camera is, for the menu to show. Plain reads of state the camera thread owns:
// a toggle that is one tick behind is fine, a stale copy kept by the menu is not.
bool FreeCamFrozen();
bool FreeCamFollowing();
bool FreeCamLookingAt();
bool FreeCamPlaying();
int  FreeCamKeyframeCount();
float FreeCamFov();
const char* FreeCamFovStatus();

// The playback-speed ladder, and which rung the camera is on.
int   FreeCamPlaySpeedCount();
float FreeCamPlaySpeedAt(int index);
int   FreeCamPlaySpeedIndex();

// The camera's own view and projection matrices, for anything drawn over the game view
// (the spawn hologram). Returns false until the camera thread has found a camera; the
// caller transposes when *transposed is set, because the engine keeps some builds
// column-major. Read on the render thread, published by the camera thread, so it is a
// snapshot rather than a live view.
bool FreeCamCameraMatrices(fc_mat *view, fc_mat *proj, int *transposed);

}  // namespace game
}  // namespace tw

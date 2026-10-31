// input_block.h - keep the game's own controls away from the player while the free
// camera is flying.
//
// The camera flies; the character must not walk around underneath it, swing a punch or
// start a conversation while it does. O (freeze) and P (follow) hand control back, and
// then none of this is in the way.
//
// TriadWars.exe reads the keyboard through DirectInput (its import table has exactly one
// GUID, GUID_SysKeyboard) and the mouse through raw input (it registers usage page 1,
// usage 2 and nothing else). So there are three places to stand in front of:
//
//   * the DirectInput keyboard device's state, emptied of every key the camera does not
//     need - that is the one that moves the character;
//   * raw input, which is WM_INPUT messages - swallowed before the game's window
//     procedure sees them, which stops the mouse turning, aiming and shooting;
//   * the polling functions the game also imports (GetAsyncKeyState, GetKeyboardState),
//     in case anything else reads the keyboard that way. Only the game's own import
//     table is redirected, so this DLL and the menu keep reading the real keyboard.
//
// Every patch is a pointer swap that can be put back, and nothing here runs unless a
// camera is actually flying.
#pragma once

#include <windows.h>

namespace tw {
namespace game {

// Hold the game's controls back (true) or give them back (false). Cheap; call it every
// time the answer changes, and it does nothing when it is already in that state.
void InputBlockSetActive(bool on);
bool InputBlockActive();

// True when this window message must not reach the game. Called from the window
// procedure after ImGui has had its look at the message.
bool InputBlockSwallowMessage(UINT msg);

// Patches the game's import table, and finds the DirectInput keyboard device type so its
// state can be filtered. Safe to call more than once; hinst is the module DirectInput
// should be created from (the game's own).
void InputBlockInstall(HINSTANCE hinst);

// What the last install managed, for the log.
const char *InputBlockStatus();

// True once at least one of the three routes is in place; when it is false the caller
// may try InputBlockInstall() again the next time a camera starts flying.
bool InputBlockReady();

}  // namespace game
}  // namespace tw

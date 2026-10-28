Free camera for the Triad Wars debug menu
========================================

This folder is the patch to TheSubsonicTouch/TestingSkybox (the menu's source) that
adds a Free Camera entry, plus the built dinput8.dll.

WHAT WAS ADDED
--------------

    freecam_game.h / freecam_game.cpp     the camera: find it, hook it, fly it
    ../src/freecam.c, freecam.h           the portable half (poses, matrices, the movement
                                          state machine, world/view pairing and detection)
    patched/menu_core.h                   DebugHooks gains setFreeCam
    patched/menu_core.cpp                 the "Free Camera" toggle item
    patched/win_menu.cpp                  wires the hook and includes the new module

Nothing else in the menu changed.

THE MATRIX LAYOUT (build 6)
--------------------------

Builds 1 to 5 wrote the camera basis in the wrong order and the whole flight worked
backwards because of it. The log settles it: at 14:10:09 the same camera prints both
matrices, and they are inverses of each other.

    +0x40 view   [ -0.996 -0.014  0.086 |  0.087 -0.155  0.984 | -0.000  0.988  0.156
                   | 1477.117 -111.211 684.829 ]
    +0x80 world  [ -0.996  0.087 -0.000 | -0.014 -0.155  0.988 |  0.086  0.984  0.156
                   | 1411.382 -819.282 3.070 ]

* In the world matrix the rows ARE the axes: right (-0.996 0.087 0), up
  (-0.014 -0.155 0.988) and backwards (0.086 0.984 0.156), and the translation is the
  eye - which is the player's position, 1411.382 -819.282 3.070.
* In the view matrix the rows hold the COMPONENTS of those axes: row 0 is
  (right.x, up.x, backwards.x). Its translation is minus each axis' dot with the eye,
  so it is 1477.117 ..., not the position.
* The third axis is backwards, not forward: the camera looks down its own -Z, so
  forward = -(third axis). Decoded that way the camera looks 9 degrees down at a
  player standing 3.5 units away, at yaw 175. That is a camera, and the numbers are
  checked in tests/ (test_game_camera_matrix).

Reading the third ROW as forward - builds 1 to 5 - happens to be right only when the
camera faces exactly -Y, which the logged cameras all do; that is why the flight
seemed to work while the picture came out of the back of the camera and the mouse
felt inverted. Writing the world layout into the view slot did the same thing from
the other side. Both are fixed: fc_basis gives a right-handed frame (right = forward
x world up), fc_mat_from_pose writes both layouts, fc_pose_from_view reads the
components out of the third block (m[2], m[6], m[10]) and only falls back to a
forward-only comparison for a rolled camera.

WHY THE PLAYBACK KEY STOPPED WORKING, AND THE FIX
----------------------------------

Hotkey::Pressed() consumed the key's edge: the second call in the same tick always
answered no. Build 8 added a frozen-camera guard that asked it twice

    if (g_playKey.Pressed() && fc_frozen(&g_state)) { ... }
    else if (g_playKey.Pressed()) { ... the playback ... }

so when the camera was not frozen the first test ate the press and the second test saw
nothing: the playback key did nothing at all. Pressed() now latches its answer until
NewFrame(), which
the camera thread calls once per tick (BeginHotkeyFrame), so a key can be asked as often
as the code likes. The same bug had already been fixed once by hand for P; latching
means it cannot come back for any of them.

It also thaws a frozen camera before playing, rather than refusing: a playback is the
camera moving, so the key means "show me the shot".

The key itself is J since build 11 (K up to build 10), so it sits next to L and is out of
the way of the WASD hand.

WHAT BUILD 9 ADDED
------------------

K plays the path back as a camera move (fc_play_pose, used by fc_play_step):

  * a cubic Hermite spline through the keys, with the slopes taken from each key's
    neighbours (slope_of), so the camera passes every key with a continuous velocity
    instead of the corner a straight-line lerp produced;
  * the yaw is unwound along the path first, so a pair that crosses +-180 degrees is a
    small turn instead of a full spin;
  * the clock runs straight (the easing was removed in build 10: over a long path the
    first second of an eased shot looked like the playback had not started), so the shot
    lasts exactly as long as the flight did and moves from the first frame;
  * the two ends are the keys themselves, so a playback always begins and finishes on a
    recorded pose, and the pitch stays inside +-89 degrees.

fc_play_pose is public so the tests can sample the curve directly: test_cinematic_
playback checks the corner (velocity continuous through the middle key), the ease at
the start, the short way round at 180 degrees, and the landing on the last key.

WHAT BUILD 8 ADDED
------------------

A frozen camera can aim. This is the second of the three states the camera has:

  flying           the player's controls are held back, the camera flies
  frozen (O)       the player's controls come back, the camera is pinned
  frozen + P       the pinned camera turns to keep the player in frame - fc_look_at /
                   fc_aim_at, exponential smoothing with a 0.14 s time constant, short
                   way round at +-180 degrees, aimed one metre above the player
                   transform (which sits at the character's feet)

fc_freeze() now also drops a move-follow (a follow would carry the frozen camera along)
and thawing clears the aiming. The playback key while frozen thaws the camera first: a
playback would not be seen behind a freeze.

WHAT BUILD 7 ADDED
------------------

    input_block.h / input_block.cpp       holds the game's own controls back while the
                                          camera flies (details below)
    fc_follow_update_angles(st, y, p)     P turns the camera with the player's own camera
    fc_play_set_speed / _shift / _speed   the arrow keys' playback speed, 0.1x to 4x
                                          (fc_play_step multiplies dt by it)
    M                                     fc_path_clear, which was already there

WHY INPUT BLOCKING LOOKS THE WAY IT DOES
----------------------------------------

TriadWars.exe's import table says how it reads the controls: one GUID for DirectInput
(GUID_SysKeyboard, at 0x1cc0480), one RegisterRawInputDevices call registering usage
page 1 usage 2 (mouse) and nothing else, plus GetAsyncKeyState/GetKeyboardState.

So input_block.cpp stands in front of all three:

  * DirectInput keyboard. A throwaway keyboard device is created (on a hidden window,
    background and non-exclusive) only to find its class's vtable; slot 9 of that vtable
    is GetDeviceState and slot 10 GetDeviceData. Both are swapped for ours, which empty
    every key except the camera's own. The vtable belongs to dinput8.dll and is shared by
    every device of that class, so the game's own keyboard is covered even though it made
    it before us. Nothing is guessed: the slot is only patched if the function lives in
    dinput8.dll and answers a 256-byte keyboard request.
  * Raw input. WM_INPUT is swallowed in the window procedure after ImGui has seen it, so
    the mouse cannot turn the player, aim or fire. Key and button messages go too.
  * The polling functions. Only the game module's import table entries for
    GetAsyncKeyState and GetKeyboardState are redirected; this DLL's own calls still reach
    the real ones, which is what lets the camera read W A S D and the hotkeys.

All three are off unless a camera is flying, and O or P switch them off while it is.
The three keys a player needs while flying (P, O, L, K, M, F10, Insert, arrows, Enter,
Backspace, Escape, Tab) are passed through.

FOLLOW THAT PANS
----------------

The camera turns with the player's camera by reading the camera component's world matrix
whenever it is not the pose we last wrote - a value that is not ours is the game's own,
so the turn it has made since the last one is the pan to copy (fc_follow_update_angles,
short way round at +-180 degrees). While following, the mouse and WASD are not read for
the camera: the player is driving.

RESTORING WITHOUT A JUMP
------------------------

RestoreMatrices() now only writes the matrices saved when the flight started if the
camera still holds ours. If the game has written its own pose since, that one is newer
than the saved copy, and overwriting it is what made switching back and forth jump.

WHAT BUILD 6 ADDED
------------------

    fc_freeze(st, on) / fc_frozen(st)     hold the pose: fc_step ignores keys and mouse
                                          and returns the same matrix. Freezing also
                                          stops a playback and a follow (the follow is
                                          cleared by the hotkey), and switching free
                                          cam off clears the freeze.
    O                                     the freeze key, in HandleHotkeys.
    FreeCamHudLines / Hud()               eight slots of key + text, each with its own
                                          fade; drawn by renderFrame() in the top-right
                                          corner even when the menu is closed, and not
                                          drawn at all when every line has faded.

HOW THIS FINDS THE CAMERA
-------------------------

Guessing where the camera lives did not work. Build 2 scanned the heap for a camera
near the player. Build 3 walked the camera manager at 0x23e0150 and took what its
+0x50 pointed at - the log from the retail build says that object is not a camera:

    FreeCam: the engine's camera at 00007FF436CD96A0 did not look like a camera
    FreeCam: its first matrix [ 0.000 0.000 0.000 0.000 | ... ]

So build 4 stops guessing and asks the renderer instead.

The engine draws with a small leaf function, the camera's view accessor:

    0xcfc30   80 b9 00 01 00 00 00    cmp byte ptr [rcx+0x100], 0
              48 8d 81 10 01 00 00    lea rax, [rcx+0x110]
              75 04                   jne +4
              48 8d 41 40             lea rax, [rcx+0x40]
              f3 c3                   ret

It is called directly - 17 call sites, none through a vtable - so patching the body
catches every one of them, the renderer included. The patch is an absolute jump:

    FF 25 00 00 00 00      jmp qword ptr [rip+0]
    <8-byte address>

Why absolute: build 4 used `E9 rel32`, and the distance from this DLL to the game does
not fit in 32 bits. The truncated displacement sent the game somewhere arbitrary and
froze it the instant the toggle was clicked. A 64-bit address cannot overflow.

The original function is copied into executable memory first (only after checking that
nothing in it is position dependent), every other call runs that copy, and the write is
made with every other thread suspended - and skipped, retried, if any of them is
actually inside the function at the time.

That hook is also the answer to "which object is the camera": the renderer calls it
with the component in rcx, every frame. The hook records the object and how often it
was asked about, and the camera thread picks the object that is asked about most, that
stands near the player, and whose matrix really is a view matrix (a rigid, orthonormal
3x3 with a row-3 translation, checked by rebuilding the pose and comparing).

Nothing about the layout is assumed either: the view matrix sits at

    view = (pointer the accessor returned) - (object)

and the world matrix is 0x40 after it, because that is where the sibling accessor
0xcfc10 reads. Both come out of the game at run time.

WHAT THE FLIGHT DOES
--------------------

   * while free cam is on, a call to the view accessor for that object returns a view
     matrix built from our pose at the moment of the call. That is the whole trick:
     the game rewrites its own camera every frame, so a stored matrix would be
     overwritten before the frame that follows, but an answer cannot be;
   * the same matrices are written into the object as well, for code that reads the
     memory directly, and put back when the flight ends;
   * if the game switches cameras (car, cutscene) a different object starts being asked
     about and the flight carries on with it;
   * if the renderer stops asking about the camera we picked, it was the wrong one:
     it is put on a short ban list and another is chosen;
   * if the engine keeps its matrices column-major, the transposed form is recognised
     and written back the same way round;
   * F10 puts the original accessor bytes back as well as the camera, so there is a
     way out if the picture ever looks wrong.

BUILD 5: FOLLOWING AND KEYFRAMES
--------------------------------

Three additions, all in the portable half so they can be tested without Windows:

    fc_follow(st, player, on)       P. The camera keeps its offset from the player and
                                    moves by however far the player moved each tick
                                    (fc_follow_update), so flying by hand while
                                    following adds to the follow instead of fighting it.

    fc_key_add(st, now)             L. Appends the current pose and the time since the
                                    first keyframe. 64 keys, 0.02 s minimum spacing so
                                    no segment can divide by zero.

    fc_play_start/step/stop         K. Advances a clock by dt, finds the segment whose
                                    two keys bracket it, and eases position, yaw (the
                                    short way round, so a path through +-180 degrees
                                    does not spin) and pitch. The last key is played
                                    and playback stops there; pressing the key again
                                    replays from the start.

The mouse is natural: Windows reports screen y downwards, so the pitch takes the
movement off, and a mouse moving right lowers the yaw (which turns the camera right).
This was already true in build 5 - what was still wrong was the matrix layout above.
The existing invert_y setting flips the pitch back for anyone who prefers it.

Everything is guarded: VirtualQuery before every read, SEH around the scan and the
writes, the accessor located by RVA *and* by searching .text for its exact bytes (so a
game build with a different layout is survived rather than patched blind), a unique
match required before anything is patched, and the hook skipped - not forced - when no
match is found.

THE TWO BUGS THAT MADE EARLIER BUILDS DO NOTHING
------------------------------------------------

    fc_config_defaults(&g_state.cfg);
    fc_init(&g_state, &g_state.cfg);     /* wrong */

fc_init() memsets the whole state, cfg included, and then copies cfg in from the
pointer - which points into the state that was just zeroed. Speed, sprint, sensitivity
and max_distance all ended up 0, so even a camera that was found could never move or
look. fc_init() now copies the config first, and the menu keeps its config in a
separate object. The test suite has a case for exactly this.

The second was the jump overflow described above, which is what froze the game.

WHY IT DOES NOT FREEZE ANY MORE
-------------------------------

The menu calls its hooks from hkPresent, on the render thread, so nothing there may
walk the heap. FreeCamSetActive() only sets a flag and returns; the searching, the
flying and the restoring happen on a thread of its own, two milliseconds at a time.
The proxy hook only does a few pointer tests and a counter increment.

BUILDING
--------

    ./build_menu.sh /path/to/TestingSkybox

Needs zig (https://ziglang.org) - no Visual Studio, no Windows. zig ships the MinGW
headers and libraries, and the script generates the one import library MinGW is
missing (D3DCOMPILER_47.dll) so the rebuilt DLL imports the same things the original
did:

    D3DCOMPILER_47.dll!D3DCompile, d3d11.dll!D3D11CreateDeviceAndSwapChain,
    XINPUT1_3.dll!XInputGetState, WINMM.dll, and the three dinput8 exports.

freecam_game.cpp is compiled with -fms-extensions so it can use __try/__except.

The build needs spawn_guard.S from the menu project, which is not in the TestingSkybox
repository; the script uses a C fallback instead. Spawning is not reachable from the
menu any more (those items were removed), so the guarded call is never taken.

ADDRESSES (this build of TriadWars.exe)
---------------------------------------

    view accessor 0xcfc30    world accessor 0xcfc10   projection accessor 0xcfbf0
    manager 0x23e0150 (+0x50)                        player 0x23f25f8 -> +0x58 -> +0x80

    camera vtables  0x0182a800 Base     0x0182a9a0 Debug    0x01936140 Game
                    0x01936430 Chase    0x01936590 Follow   0x01936ed0 Animated
                    0x01937030 Script   0x01937300 Progress 0x01937da0 Turf

The accessors are found by their bytes, not only by these numbers; the vtables are
used for naming a camera in the log and for the last-resort heap scan.

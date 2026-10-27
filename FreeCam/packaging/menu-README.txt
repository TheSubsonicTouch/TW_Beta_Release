Triad Wars - Free Camera
========================

INSTALL
-------

  1. Extract this whole zip into your Triad Wars folder - the one with
     TriadWars.exe and Play-TriadWars.bat in it. You should end up with
     Install-FreeCam.bat and a FreeCam folder next to the game.

  2. Double-click Install-FreeCam.bat.

     It keeps the menu DLL you already have as dinput8.original.dll, then puts
     the menu with the Free Camera entry in its place. If the game folder has no
     menu installed yet, it just installs one.

  3. Start the game with Play-TriadWars.bat as usual.

  4. Once you are in the world (not the title screen or a loading screen), press
     Insert. Arrow down to "Free Camera" and press Enter.

To undo it, double-click Uninstall-FreeCam.bat - it puts your old dinput8.dll back.

Nothing happens until you run the installer: extracting the zip only adds the
FreeCam folder and two .bat files, it does not overwrite your menu.

The standalone\ folder holds the older hot-key build if you would rather not replace
the menu DLL at all. It is a separate program (FreeCam-Inject.exe) and is not part of
the menu install. You do not need both.


WHAT THE FREE CAMERA DOES
-------------------------

The game draws through a small function that hands out the camera's view matrix, and
it calls it with the camera in hand. The menu hooks that function, so it can see which
object the game is drawing with - not guess. It takes the object the renderer asks
about most often, checks that its matrix really is a camera's, and remembers where that
matrix sits inside it.

While free cam is on, that same function answers with a matrix built from your pose,
at the moment it is asked. The game rewrites its own camera every frame; an answer
cannot be overwritten, which a stored matrix can. The matrices in memory are written as
well, for the code that reads them directly, and put back when you switch off, so the
game carries on from its own camera and not from where you flew to.

WHAT CHANGED IN THIS BUILD
--------------------------

  - While the camera is flying, the player does not act any more. The character stands
    still, does not swing, shoot or start conversations, and does not turn with the
    mouse. The game's own controls are held back at three places (see below); nothing
    else about the game changes, and they come back the instant you switch the camera
    off or press O or P.
  - P now pans as well as follows: the camera keeps its framing and turns with the
    player's own camera, so walking and looking around stays where you framed it.
  - M forgets every recorded keyframe, so the next L starts a new scene instead of
    adding to the old one.
  - The left and right arrow keys change how fast a recorded path plays back, from 0.1x
    to 4x in steps (0.1, 0.2, 0.25, 0.33, 0.5, 0.75, 1, 1.25, 1.5, 2, 3, 4). The speed is
    shown in the top-right corner as it changes and stays set for the next playback.
  - Switching back and forth no longer glitches: turning the camera off only puts the
    old matrices back while the camera still holds ours, so a game pose written since
    is never overwritten by a stale one.

  - The mouse is no longer inverted, and this time the cause is known. The game
    keeps its camera matrix the other way round from how earlier builds wrote it:
    the rows of the view matrix hold the camera's right, up and backwards axes, and
    its world matrix holds the same three axes as rows - the two are inverses, which
    is why the translation of one is not the position and the other's is. Writing a
    world matrix into the view slot makes the picture look out of the back of the
    camera, and every control then works backwards. The matrices are now built the way
    the game's own camera builds them; the test suite checks the result against a
    camera matrix copied out of a real TW_MenuLog.txt, and it is the picture, not a
    sign in the mouse code, that is now right.
  - O freezes the camera where it is. The view stays put while your character walks,
    drives, fights and interacts - useful for watching a scene from a fixed angle. O
    again hands the camera back, and F10 clears the freeze as well.
  - A small hotkey display in the top-right corner shows what was just pressed (P, L,
    K, O, F10). It fades away by itself after a moment, so the corner is empty while
    you are just flying.
  - P and the keyframes from the last build are unchanged.

CONTROLS
--------

    Insert              open the debug menu
    Free Camera         toggle it in the menu
    W A S D             move forward, back, left, right
    Space / C           up / down
    Hold right mouse    look around (the pointer is held still while you do)
    Shift               five times faster
    Ctrl                a fifth of the speed
    P                   follow the player - the camera keeps its framing and goes
                        wherever the player goes; P again to stay put
    L                   record a keyframe where the camera is now
    K                   play the recorded keyframes back, at the speed they were flown
                        (or whatever the arrows were set to)
    Left / Right arrow  play the path back slower / faster, 0.1x to 4x
    M                   forget every keyframe, to start a new scene
    O                   freeze the camera where it is - the view stays put while the
                        player moves, fights and interacts; O again to unfreeze
    F10                 panic: put the camera back and switch off, from anywhere

The mouse is natural: push it forward and you look up, pull it back and you look down.
Moving it right turns right.

Follow and keyframes:

  - P pins the camera to the player: it keeps whatever offset it had when you pressed P,
    follows the player wherever they go, and turns with them as they look around. While
    P is on the player is in control; the camera is not flown by hand.
  - L records the camera's pose (where it is and where it looks). Press it again
    somewhere else. Two are enough; up to 64 are kept.
  - K plays them back, taking the same time the flight took, easing the position and
    the angle between each pair, then stops on the last one. Press K again to replay,
    or K while it is playing to stop and stay where it got to. Recording more keyframes
    adds to the same path until free cam is switched off - a path survives switching
    free cam off and on again, and is cleared when the game is restarted.
  - The arrow keys set the speed of that playback, 0.1x to 4x, shown as you press them
    (0.5x is slow motion, 2x is twice as fast). The speed you leave it at is kept.
  - M clears the path completely, so the next keyframes are a new scene.

While the path is playing, the mouse and movement keys are ignored: the path is driving.

The player's controls:

  - Free camera on, no O and no P: the camera is yours and the player's controls are
    held back. The character cannot walk, drive, attack or interact, and the mouse does
    not turn the character either. That is what makes the flight a camera and not a
    puppet show.
  - O (freeze) or P (follow) hand the controls straight back: move, fight, interact and
    look around as usual, while the camera keeps watching.
  - Switching free cam off, or F10, gives them back too - there is no state you can get
    stuck in.
  - How: the game reads the keyboard through DirectInput and the mouse through raw
    input. The keyboard device's state is emptied of everything except the camera's own
    keys, WM_INPUT (the raw mouse) is dropped before the game's window procedure sees
    it, and the game's imports of GetAsyncKeyState/GetKeyboardState are redirected. The
    DLL and the menu keep reading the real keyboard, which is why the hotkeys and the
    menu still work.

The freeze (O):

  - The camera stops where it is and stays there. The player is untouched, so WASD,
    attacking, driving and interacting all still work - you are just watching from a
    fixed angle.
  - While frozen, the mouse is not captured, so the right button goes to the game.
  - Freezing stops a follow (P) and a playback (K): both would drag the view along.
  - O again, F10, or switching free cam off all thaw it. Switching free cam off and on
    always starts unfrozen.

The hotkey display:

  - Sits in the top-right corner, one line per key pressed, newest on top.
  - Fades out a second after the press; nothing is drawn while it is empty.
  - It is drawn by the same overlay as the menu, so it works with the menu closed.

The menu's own keys (arrows, Enter, Backspace) still work, so you can open the menu
while flying and switch it off from there.


IF IT DOES NOT WORK
-------------------

TW_MenuLog.txt, next to TriadWars.exe, records everything. Load into the world
first - the camera cannot be told apart on a loading screen - then toggle it and
look for these lines, in this order:

    FreeCam: camera thread up (build 7: player controls held back, follow pans, playback speed)
    FreeCam: switched on, asking the renderer which camera it draws
    FreeCam: hooked the view accessor at 0000... with an absolute jump (original kept at ...)
    FreeCam: the renderer asked about 0000... for the first time
    FreeCam:   vtable 0000... (GameCameraComponent), base 0000..., so +0x182a800
    FreeCam:   +0x40 is a rigid transform
    FreeCam: using the camera the renderer asks for at 0000... view +0x40, world +0x80,
             view row sign 1, 41 calls, 4.6 units from the player
    FreeCam: on at x y z (yaw ..., pitch ...)
    FreeCam: camera at x y z, view accessor hooked, answered 118 of 120 calls, writes held 0

"hooked the view accessor" is what makes the camera findable at all, and "answered" is
the count of times the game asked for the view and got ours - that number climbing
means the game is drawing your pose. "writes held" only counts matrices written to
memory; the game overwrites those every frame and the answer overrides them, so it is
normal for "taken back" to be high as long as "answered" is climbing.

Right after the choice there is one line per candidate it chose from:

    FreeCam: candidate 0000... (GameCameraComponent), 41 calls, 4.6 units from the player

If it picked the wrong one it says so and picks again:

    FreeCam: the renderer never asked about 0000... in two seconds; picking another camera

These lines mean it fell back or gave up:

    FreeCam: the view accessor does not match this game build; leaving it alone
    FreeCam: the view accessor is hooked but the game has not called it in three seconds
    FreeCam: the manager's camera at 0000... did not look like one either
    FreeCam: waiting for the player transform - needed to tell a camera from the rest

The first two mean the accessor bytes were not found in your TriadWars.exe, or the game
does not call that function - send the log, it says what was looked for and where. The
last two mean you are not in the world yet, or a camera is nowhere near the player.

While flying, the player's controls are held back on purpose, so the character stays
put. If the character does walk around anyway, the log says what was managed:

    FreeCam: input blocking ready - 2 import(s) redirected, 2 keyboard device class(es) filtered
    FreeCam: player controls held back (the camera is flying)
    FreeCam: player controls given back (following the player)

Zero keyboard device classes means DirectInput could not be reached on this machine; the
raw mouse and the polling functions are still blocked, and P or O still work.


IS THIS SAFE FOR MY GAME
------------------------

It is the same menu, built from the same sources, with one extra file
(freecam_game.cpp) added and three small edits. Nothing in the old features was
changed, the crash guard is still installed at startup, and the camera only touches
memory it has checked first.

Keep a copy of the file you replace: rename your current dinput8.dll to
dinput8.original.dll before copying this one in. To go back, rename it back.


UNINSTALL
---------

Put your original dinput8.dll back, and delete the standalone folder if you used it.
Nothing else is written to the game folder except TW_MenuLog.txt.

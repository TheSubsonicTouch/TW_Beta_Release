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

The build before this one froze the game the moment Free Camera was switched on. That
was a bug in the hook: it redirected the game's camera function with a jump whose
distance had to fit in 32 bits, and the distance between this DLL and the game does not
fit. The jump went somewhere arbitrary. It is now an absolute jump, which cannot
overflow, the original bytes are copied first and every other call runs them, and the
patch is written with the game's other threads briefly paused so no thread can be
running the bytes while they change.

The build before *that* did nothing visible, for two separate reasons, both fixed:

  - the camera was taken from where the game's camera manager points, and the log shows
    that object is not a camera at all; the camera is now the one the renderer asks the
    view function about, which cannot be wrong;
  - the camera state was initialised with its own config passed in, and that call
    clears the state before copying the config, so speed and look sensitivity were zero.
    A camera that was found could not have moved.

F10 now also puts the game's original camera code back, not just the camera, so there
is a way out if anything ever looks wrong.

CONTROLS
--------

    Insert              open the debug menu
    Free Camera         toggle it in the menu
    W A S D             move forward, back, left, right
    Space / C           up / down
    Hold right mouse    look around (the pointer is held still while you do)
    Shift               five times faster
    Ctrl                a fifth of the speed
    F10                 panic: put the camera back and switch off, from anywhere

The menu's own keys (arrows, Enter, Backspace) still work, so you can open the menu
while flying and switch it off from there.


IF IT DOES NOT WORK
-------------------

TW_MenuLog.txt, next to TriadWars.exe, records everything. Load into the world
first - the camera cannot be told apart on a loading screen - then toggle it and
look for these lines, in this order:

    FreeCam: camera thread up (build 4: the renderer is asked which camera it draws)
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

While flying the player still receives W A S D, so your character may walk around
below you. That is the game reading its own input, not the camera.


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

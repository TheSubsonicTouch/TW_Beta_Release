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

Toggling it on did nothing you could see. Two separate faults were behind that.

The first was the camera. The previous build asked the game's camera manager for "the
camera" and wrote into what came back; the log proves that object is not a camera, so
nothing was ever drawn from it. This build takes the camera from the renderer instead,
which cannot be wrong about which one it is drawing.

The second was a zeroed config. The state object was initialised with its own config
passed in, and that call clears the state - config included - before copying the config
in, so speed, sprint and look sensitivity were all zero. Even a correct camera could
not have moved. Both are fixed, and the test suite covers the second.

An earlier build could also freeze the game: the search walked the whole heap on the
render thread. The search has moved to its own thread, two milliseconds at a time, and
the frame stays free. The accessor hook itself only does a few pointer tests.

The menu answers immediately and the camera responds as soon as the camera is found -
normally within a frame or two. If the game swaps cameras (getting into a car, a
cutscene) the flight carries on with the new one.

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
    FreeCam: hooked the view accessor at 0000... (original kept at 0000...)
    FreeCam: using the camera the renderer asks for at 0000... (GameCameraComponent),
             view +0x40, world +0x80, view row sign 1, 41 calls, 4.6 units from the player
    FreeCam: on at x y z (yaw ..., pitch ..., view row sign 1, the camera the renderer asks for)
    FreeCam: camera at x y z, view accessor hooked, answered 118 of 120 calls, writes held 0,
             taken back 120

"hooked the view accessor" is what makes the camera findable at all, and "answered" is
the count of times the game asked for the view and got ours - that number climbing
means the game is drawing your pose. "writes held" only counts the matrices written to
memory; the game overwrites those every frame, and the accessor answer overrides them,
so it is normal for "taken back" to be high as long as "answered" is climbing.

Right after the choice you also get the candidates it was chosen from, one line each:

    FreeCam: candidate 0000... (GameCameraComponent), 41 calls, 4.6 units from the player

If it picked the wrong one it says so and picks again:

    FreeCam: the renderer never asked about 0000... in two seconds; picking another camera

These lines mean it fell back or gave up:

    FreeCam: the view accessor does not match this game build; leaving it alone
    FreeCam: no accessor hook on this build; falling back to writing matrices only
    FreeCam: the manager's camera at 0000... did not look like one either
    FreeCam: waiting for the player transform - needed to tell a camera from the rest

The first two mean the accessor bytes were not found in your TriadWars.exe - send the
log, it says what was looked for and where. The last two mean you are not in the world
yet, or a camera is nowhere near the player.

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

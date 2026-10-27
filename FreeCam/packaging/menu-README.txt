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

The renderer asks the game's camera manager which camera it is about to draw, gets a
camera component, and reads that component's view matrix through a small function.
The menu does exactly the same lookup - manager at 0x23e0150, camera at +0x50 - and
checks the result (its vtable, its projection, its view/world pair) before touching
it. That is the difference from the first build: it drove whichever camera happened
to sit near the player, which is not the one being drawn.

While free cam is on, the menu also redirects that component's view accessor, so
anyone asking for its view gets a matrix rebuilt from your pose at that moment. The
game rewrites its camera every frame; an answer cannot be overwritten, which a stored
value can. The matrices in memory are written as well, for the code that reads them
directly. Switching off removes the redirect and puts the original pair back, so the
game carries on from its own camera and not from where you flew to.


WHAT CHANGED IN THIS BUILD
--------------------------

Toggling it on used to do nothing you could see.

The first build could also freeze the game: the search walked the game's whole heap
on the render thread, inside a frame - with a multi-gigabyte heap that is seconds of
a frozen frame, which Windows reports as "not responding". The search now runs on its
own thread, two milliseconds at a time, and the menu returns immediately; the walk
reads under a fault guard, so a page being released mid-read costs a skipped chunk
instead of a crash.

The second build stopped freezing but still moved nothing, because it wrote to a
camera component near the player rather than to the one the renderer reads, and
because a write per frame loses to the game's own camera update. This build goes
through the pointer the renderer goes through and answers the accessor call instead
of racing it.

The menu answers immediately and the camera starts moving as soon as the camera is
found - normally the same frame. If the game swaps cameras (getting into a car, a
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

    FreeCam: camera thread up (build 3: engine camera + accessor detours)
    FreeCam: switched on, resolving the camera the engine draws
    FreeCam: using the engine's active camera at 0000... (GameCameraComponent),
             view +0x40, world +0x80, view row sign 1, 4.2 units from the player
    FreeCam: hooked the view accessor at 0000... (original kept at 0000...)
    FreeCam: on at x y z (yaw ..., pitch ..., view row sign 1, the engine's active camera)
    FreeCam: camera at x y z, view accessor hooked, writes held 812, taken back 0

"hooked the view accessor" is the line that says the renderer is being answered by
us; "writes held" counts frames where the matrices in memory still held our pose.
While flying you should also see, if the game changes camera:

    FreeCam: the game switched cameras; carrying on from x y z

These lines mean it fell back or gave up:

    FreeCam: the engine's camera at 0000... did not look like a camera component;
             falling back to the heap scan
    FreeCam: the view accessor does not match this game build; leaving it alone
    FreeCam: waiting for the player transform - needed to tell a camera from the rest
    FreeCam: pass 3 over the heap found no camera; still looking

The first two mean the game build is not the one this was written against - send the
log, it says exactly what was found and where. The last two mean you are not in the
world yet, or a camera is nowhere near the player.

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

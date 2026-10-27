Triad Wars - Free Camera
========================

Copy dinput8.dll into the game folder (over the one that is already there), start
the game, press Insert and choose "Free Camera". Same key turns it off.

The standalone\ folder holds the older hot-key build if you would rather not replace
the menu DLL. You do not need both.


WHAT THE FREE CAMERA DOES
-------------------------

The game keeps a camera component (UFG::BaseCameraComponent and its eight
subclasses) holding a world transform and a view matrix. The menu finds the live
one by its vtable - those are at fixed addresses in TriadWars.exe, so this is a
lookup, not a guess - checks that the pair really is a world/view pair standing next
to your player, and then drives it from its own pose.

While it is on, a thread keeps re-writing that pair. The game rewrites its camera
every frame, so a single write per frame would be overwritten before the frame was
drawn. Switching off puts the original pair back, so the game carries on from its
own camera and not from where you flew to.


WHAT CHANGED IN THIS BUILD
--------------------------

The first build could freeze the game when you pressed Enter on Free Camera. The
search walked the game's whole heap on the render thread, inside the frame - with a
multi-gigabyte heap that is seconds of a frozen frame, which Windows reports as "not
responding".

Now the search runs on its own thread, a couple of milliseconds at a time, and the
menu returns immediately. Nothing the menu does can block a frame any more. The
walk also reads under a fault guard, so a page being released while it is being read
costs a skipped chunk instead of a crash.

The menu shows "Free camera: on - looking for it" for the second or two the search
takes, then the camera starts moving. Switching it on a second time is instant,
because the camera is remembered.


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
look for these lines:

    FreeCam: switched on, looking for the camera
    FreeCam: camera at 0000..., world +0x40, view +0x80, view row sign 1, 4.2 units ...
    FreeCam: on at x y z (yaw ..., pitch ...)
    FreeCam: camera at x y z, writes held 812, taken back 0

"writes held" means the game is rendering our pose. A high "taken back" count means
the game overwrites the camera after us every frame; the numbers say which.

While searching you may see:

    FreeCam: waiting for the player transform ...
    FreeCam: pass 3 over the heap found no camera ... still looking

The first means you are not in the world yet. The second means the game build lays
its cameras out differently from the one this was written against - send the log,
it says exactly what was found and where.

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

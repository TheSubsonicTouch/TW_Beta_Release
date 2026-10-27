Triad Wars - Free Camera
========================

There are two ways in, both in this zip.

  1. IN THE DEBUG MENU (recommended)
     dinput8.dll in this folder is your mod menu rebuilt with a Free Camera entry.
     Copy it into the game folder, over the one that is already there. Start the
     game as usual, press Insert to open the menu, choose "Free Camera" and press
     Enter. The status line tells you what happened. Choose it again to stop.

  2. STANDALONE HOT KEY
     The standalone\ folder holds a separate DLL with a Caps Lock toggle, if you
     would rather not replace the menu DLL at all. Its own README.txt explains it.

You do not need both. The menu version is the one that flies the real camera.


WHAT THE FREE CAMERA DOES
-------------------------

The game keeps a camera component (UFG::BaseCameraComponent and its eight
subclasses) that holds a world transform and a view matrix. The menu finds the
live one by its vtable - those are at fixed addresses in TriadWars.exe, so this is
a lookup, not a guess - checks that the matrix pair really is a world/view pair
standing next to your player, and then drives the pair from its own pose.

While free cam is on, a small thread keeps re-writing that pair. The game rewrites
its camera every frame, so a single write per frame would be overwritten before the
frame was drawn; writing continuously wins that race. Switching off puts the
original pair back, so the game carries on from its own camera and not from where
you flew to.


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

TW_MenuLog.txt, next to TriadWars.exe, records everything the camera does. Load into
the world first - the camera cannot be found on the loading screen - then toggle it
and look for these lines:

    FreeCam: player at x y z, scanning for camera components
    FreeCam: 2 camera component(s) found
    FreeCam: camera at 0000..., world +0x40, view +0x80, view row sign 1, 4.2 units ...
    FreeCam: on at x y z, yaw ..., pitch ...

If the last line is there, it is flying. If the menu only says "no camera found",
the log has the count and the reason - send that file and it can be fixed quickly.

While flying the player still receives W A S D, so your character may walk around
below you. That is the game reading its own input, not the camera.


IS THIS SAFE FOR MY GAME
------------------------

The menu DLL is the same menu, built from the same sources, with one extra file
(freecam_game.cpp) added and three small edits. Nothing in the old features was
changed, the crash guard is still installed at startup, and the camera itself only
ever touches memory it has checked with VirtualQuery first.

Keep a copy of the file you replace: rename your current dinput8.dll to
dinput8.original.dll before you copy this one in. To go back, rename it back.


UNINSTALL
---------

Put your original dinput8.dll back, and delete the standalone folder if you used it.
Nothing else is written to the game folder except TW_MenuLog.txt.

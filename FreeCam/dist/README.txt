Triad Wars - Free Camera
========================

A free camera for the offline Triad Wars build. Drop the four files in this folder into
your game folder, next to TriadWars.exe, and press Caps Lock to fly.


What it does
------------

* Caps Lock switches the free camera on and off.
* While it is on: W A S D to move, Space up, C down, hold the right mouse button to look
  around, Shift to go five times faster, Ctrl to creep.
* F10 is the panic key: the camera goes straight back where it came from.
* Switching free cam off puts the matrix back exactly as it was, so the game carries on
  from where it was, not from where you flew to.

Two modes, set in FreeCam.ini:

  player   The camera follows the player, so flying the player looks like a free camera.
           This is the default because it uses the one address in the game we know for
           certain: the player transform the debug menu's own ReadPlayerMatrix reads.
  camera   The camera matrix itself. The player stands still and only the view moves.
           This is the real thing, but it needs the address of the game's view matrix,
           and that address is not in the shipped code - see FreeCam.camera.ini.


Install
-------

Copy these files into the folder that holds TriadWars.exe:

    FreeCam.dll
    FreeCam-Inject.exe
    FreeCam.ini
    Play-TriadWars-FreeCam.bat
    FreeCam.camera.ini      (optional, a template for camera mode)

Then start the game with Play-TriadWars-FreeCam.bat instead of Play-TriadWars.bat.
The injector starts TriadWars.exe, waits for its window, loads FreeCam.dll into it, and
exits. Nothing is patched on disk and nothing else is replaced: dinput8.dll still belongs
to the debug menu and dxgi.dll still belongs to ReShade.

Already launched the game another way? Then run

    FreeCam-Inject.exe --attach

and it will find the running TriadWars.exe and load the camera into it.

The injector loads a DLL into the game's own process, which is what mod loaders do. Some
anti-virus tools dislike the pattern; if yours does, run --attach from the game folder or
add an exception for FreeCam-Inject.exe.


Controls
--------

    Caps Lock            free camera on / off
    W A S D              move forward, back, left, right
    Space / C            up / down
    Hold right mouse     look around (the pointer is held still while you do)
    Shift                sprint, five times the speed
    Ctrl                 slow, a fifth of the speed
    F10                  panic: put the camera back and switch off

Numbers live in FreeCam.ini (speed, sensitivity, the safety leash, the toggle and panic
keys). Edit and save: the DLL reloads the file within a couple of seconds, even mid-flight.


If it does nothing
------------------

FreeCam.log, next to the DLL, says what happened. The usual lines:

    matrix at 0x... (module: the game exe)          it found the transform, good
    no matrix at the configured address/chain yet   the chain did not lead anywhere
    free cam refused: that is not a transform       the address holds something else

"no matrix ... yet" means the game has not built that object so far - load into the world
first, then try Caps Lock again. If it stays that way, the game build probably lays the
player out at different offsets than the one the debug menu was written for; put the right
chain in FreeCam.ini, or set an absolute `address=`.

The camera is leashed to 4000 units from where it started, so a wrong matrix cannot send
you to the edge of the map. `max_distance=` changes that, and `restore=false` stops it
putting the matrix back when you switch off.


Uninstall
---------

Delete the five files. The game is untouched, so it starts normally with
Play-TriadWars.bat.


Source
------

FreeCam/src in the TriadWars local server repository: freecam.h and freecam.c hold the
matrix maths, the setting parser and the movement rules, win32.c the Windows side,
inject.c the loader. The rules are covered by unit tests that build anywhere:

    python FreeCam/tests/run_tests.py

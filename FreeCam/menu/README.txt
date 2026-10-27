Free camera for the Triad Wars debug menu
========================================

This folder is the patch to TheSubsonicTouch/TestingSkybox (the menu's source) that
adds a Free Camera entry, plus the built dinput8.dll.

WHAT WAS ADDED
--------------

    freecam_game.h / freecam_game.cpp     the camera: find it, work out its layout, fly it
    ../src/freecam.c, freecam.h           the portable half (poses, matrices, the movement
                                          state machine, world/view pairing and detection)
    patched/menu_core.h                   DebugHooks gains setFreeCam
    patched/menu_core.cpp                 the "Free Camera" toggle item
    patched/win_menu.cpp                  wires the hook and includes the new module

Nothing else in the menu changed.

WHY THE FIRST BUILD FROZE
-------------------------

FreeCamSetActive() called FindCamera() directly, and the menu calls hooks from
hkPresent - the render thread. FindCamera walked every writable page in the process
eight bytes at a time. A game with two gigabytes of heap is a quarter of a billion
reads in the middle of a frame: not a stutter, a hang.

The search is now a state machine on the camera's own thread:

    SearchStep()         walks at most kScanBudgetMs (2 ms) per call, keeps a cursor
                         in g_scanAddr and resumes exactly where it stopped, so no
                         region is skipped when the budget runs out mid-region
    ScanRegion()         walks one region in 64 KB fault-guarded chunks
    ScanChunk()          __try/__except around the reads: a page released while we
                         walk it skips one chunk instead of killing the process
    ScanChunkInner()     the hot loop: one unsigned range compare per qword rejects
                         almost everything, because the nine camera vtables all live
                         in a 1.1 MB window of the exe

FreeCamSetActive() now only sets a flag and returns; the thread does the searching,
the flying and the restoring. Nothing on the render thread blocks, at all.

HOW THE CAMERA IS FOUND
-----------------------

UFG::*CameraComponent objects start with a vtable pointer, and the nine camera
vtables are at fixed RVAs in TriadWars.exe:

    0x0182a800 Base          0x01936430 Chase       0x01937030 Script
    0x0182a9a0 Debug         0x01936590 Follow      0x01937300 Progression
    0x01936140 Game          0x01936ed0 Animated    0x01937da0 Turf

Each component keeps three 4x4s 0x40 apart at +0x40 (or +0x110 when the byte at
+0x100 is set): world transform, view matrix, projection. The view matrix is the
world matrix rebuilt from the same pose, which is what fc_view_sign checks, and the
world matrix has to stand next to the player - together those two checks reject
every other transform in the process.

Building the pair from the game's own matrices also settles which way round its view
matrix goes (row 2 is -forward or +forward), so the camera writes back exactly what
the game expects.

BUILDING
--------

    ./build_menu.sh /path/to/TestingSkybox

Needs zig (https://ziglang.org) and Python 3 - no Visual Studio, no Windows. zig
ships the MinGW headers and libraries, and the script generates the one import
library MinGW is missing (D3DCOMPILER_47.dll) so the rebuilt DLL imports the same
things the original did:

    D3DCOMPILER_47.dll!D3DCompile, d3d11.dll!D3D11CreateDeviceAndSwapChain,
    XINPUT1_3.dll!XInputGetState, and the three dinput8 exports.

freecam_game.cpp is compiled with -fms-extensions so the scan can use __try/__except.

The build needs spawn_guard.S from the menu project, which is not in the TestingSkybox
repository; the script uses a C fallback instead. Spawning is not reachable from the
menu any more (those items were removed), so the guarded call is never taken.

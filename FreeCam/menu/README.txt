Free camera for the Triad Wars debug menu
========================================

This folder is the patch to TheSubsonicTouch/TestingSkybox (the menu's source) that
adds a Free Camera entry, plus the built dinput8.dll.

WHAT WAS ADDED
--------------

    freecam_game.h / freecam_game.cpp     the camera: find it, work out its layout, fly it
    ../src/freecam.c, freecam.h           the portable half (ini-free here: poses, matrices,
                                          the movement state machine, world/view pairing)
    patched/menu_core.h                   DebugHooks gains setFreeCam
    patched/menu_core.cpp                 the "Free Camera" toggle item
    patched/win_menu.cpp                  wires the hook and includes the new module

Nothing else in the menu changed.

HOW THE CAMERA IS FOUND
-----------------------

UFG::*CameraComponent objects start with a vtable pointer, and the nine camera
vtables are at fixed RVAs in TriadWars.exe:

    0x0182a800 Base          0x01836430 Chase       0x01937030 Script
    0x0182a9a0 Debug         0x01936590 Follow      0x01937300 Progression
    0x01936140 Game          0x01936ed0 Animated    0x01937da0 Turf

A heap scan for those pointers finds the live camera in one pass. Each component
keeps three 4x4s 0x40 apart at +0x40 (or +0x110 when the byte at +0x100 is set):
world transform, view matrix, projection. The view matrix is the world matrix
rebuilt from the same pose, which is what fc_view_sign checks, and the world matrix
has to be standing next to the player - that pair of checks is what rejects every
other transform in the process.

Building the world/view pair from the game's own matrices also settles which way
round its view matrix goes (row 2 is -forward or +forward), so the camera writes
back exactly what the game expects.

BUILDING
--------

    ./build_menu.sh /path/to/TestingSkybox

Needs zig (https://ziglang.org) and Python 3 - no Visual Studio, no Windows. zig
ships the MinGW headers and libraries, and the script generates the one import
library that MinGW is missing (D3DCOMPILER_47.dll) so the rebuilt DLL imports the
same things the original did:

    D3DCOMPILER_47.dll!D3DCompile, d3d11.dll!D3D11CreateDeviceAndSwapChain,
    XINPUT1_3.dll!XInputGetState, and the three dinput8 exports.

The build needs spawn_guard.S from the menu project, which is not in the TestingSkybox
repository; the script uses a C fallback instead. Spawning is not reachable from the
menu any more (those items were removed), so the guarded call is never taken.

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

HOW THE GAME DRAWS ITS CAMERA
-----------------------------

Three facts, all read out of TriadWars.exe, decide the whole design.

 1. The renderer asks the camera manager for the camera it is about to draw:

        0x64220  call 0xcf9d0          rax = [0x23e0150], the manager
        0x64225  mov  rdi, [rax+0x50]  rdi = the camera in use
        0x64318  mov  rcx, rdi
        0x64318  call 0xcfc30          the view accessor, called on rdi

    One hop out of the manager is the component itself. The game keeps other
    camera components alive as well - a camera near the player is not necessarily
    the one being drawn - so this pointer is the only reliable answer.

 2. A camera component keeps three 4x4s 0x40 apart, at +0x40, or at +0x110 when
    the byte at +0x100 is set:

        view matrix    +0x40   accessor 0xcfc30
        world matrix   +0x80   accessor 0xcfc10
        projection    +0xc0   accessor 0xcfbf0

    the projection is the third one; the renderer copies the view and the
    projection out and multiplies them.

 3. The accessors are ordinary functions, called directly - 17 call sites for the
    view one, none of them through a vtable. Patching the function body therefore
    catches every caller, including the renderer.

WHAT THIS DOES
--------------

   1. Resolve the engine's camera: [0x23e0150] -> +0x50, and check it before
      trusting it - its vtable has to be one of the nine camera vtables, one of its
      three matrices has to be a projection, and the view has to be the world matrix
      rebuilt from the same pose. Only then is anything hooked.

   2. Detour the view (and world) accessor of that function. While free cam is on, a
      call for *that camera* returns a matrix rebuilt from our pose at the moment of
      the call; every other call, and every call while free cam is off, runs the
      original code from a copied trampoline. Detouring is what makes this work: the
      game rewrites its matrices every frame, so simply storing values loses the race
      to the frame that follows. An answer cannot be overwritten.

   3. Still write the same matrices into the object, so code that reads the memory
      directly sees our pose too.

   4. Follow the game when it swaps cameras (on foot, in a car, a cutscene): twice a
      second the manager is read again, and our pose moves to the new object instead
      of the flight silently ending.

   5. If the engine path does not validate - a different game build, say - fall back
      to the old heap scan for a camera vtable whose world matrix stands near the
      player. That path still works without the hook, it just has to win the write
      race; the log says which path was taken.

Everything is guarded: VirtualQuery before every read, SEH around the scan and the
writes, every detour byte-verified against the expected code before it is pinned, and
the hook skipped (not forced) if the game does not match.

WHY THE EARLIER BUILDS DID NOT MOVE THE CAMERA
----------------------------------------------

The first build searched the heap for a camera vtable and took the nearest one to the
player. That object is drawn only if the engine happens to be using it - and it is not
the one the renderer reads. It also wrote the matrices once per frame from its own
thread, which the game's own camera update overwrote before the frame was drawn. Both
mistakes are silent: correct-looking writes into memory nobody draws.

The third build fixes both by never writing "somewhere": it goes through the pointer
the renderer goes through, and answers at the moment the renderer asks.

WHY IT DOES NOT FREEZE ANY MORE
-------------------------------

The menu calls its hooks from hkPresent, on the render thread, so nothing there may
walk the heap. FreeCamSetActive() only sets a flag and returns. The searching, the
flying and the restoring all happen on a thread of its own, a couple of milliseconds
at a time:

    SearchStep()         walks at most kScanBudgetMs (2 ms) per call, keeps a cursor
                         in g_scanAddr and resumes exactly where it stopped, so no
                         region is skipped when the budget runs out mid-region
    ScanRegion()         walks one region in 64 KB fault-guarded chunks
    ScanChunk()          __try/__except around the reads: a page released while we
                         walk it skips one chunk instead of killing the process
    ScanChunkInner()     the hot loop: one unsigned range compare per qword rejects
                         almost everything, because the nine camera vtables all live
                         in a 1.1 MB window of the exe

BUILDING
--------

    ./build_menu.sh /path/to/TestingSkybox

Needs zig (https://ziglang.org) - no Visual Studio, no Windows. zig ships the MinGW
headers and libraries, and the script generates the one import library MinGW is
missing (D3DCOMPILER_47.dll) so the rebuilt DLL imports the same things the original
did:

    D3DCOMPILER_47.dll!D3DCompile, d3d11.dll!D3D11CreateDeviceAndSwapChain,
    XINPUT1_3.dll!XInputGetState, WINMM.dll, and the three dinput8 exports.

freecam_game.cpp is compiled with -fms-extensions so the scan can use __try/__except.

The build needs spawn_guard.S from the menu project, which is not in the TestingSkybox
repository; the script uses a C fallback instead. Spawning is not reachable from the
menu any more (those items were removed), so the guarded call is never taken.

CAMERA VTABLES AND ACCESSOR ADDRESSES (this build of TriadWars.exe)
-------------------------------------------------------------------

    0x0182a800 Base          0x01936430 Chase       0x01937030 Script
    0x0182a9a0 Debug         0x01936590 Follow      0x01937300 Progression
    0x01936140 Game          0x01936ed0 Animated    0x01937da0 Turf

    manager 0x23e0150 (+0x50 is the camera in use)     player 0x23f25f8 -> +0x58 -> +0x80
    view accessor 0xcfc30    world accessor 0xcfc10    projection accessor 0xcfbf0

Every one of those is checked before it is used; a mismatch is logged and the feature
degrades (heap scan, or plain writes) rather than crashing.

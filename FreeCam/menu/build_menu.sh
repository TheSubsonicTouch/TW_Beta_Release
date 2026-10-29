#!/bin/sh
# Builds the debug menu with the Free Camera entry, from the menu's own sources.
#
#   ./build_menu.sh /path/to/TestingSkybox
#
# Needs zig (https://ziglang.org) - it carries the MinGW headers and libraries, so
# this builds on Linux, macOS or Windows without Visual Studio.
#
# The menu's source repository does not contain spawn_guard.S (the crash-guarded
# trampoline), so the C fallback below stands in for it. Spawning is not reachable
# from the menu any more, so the guarded call is never taken.
set -e
cd "$(dirname "$0")"

SRC=${1:?usage: build_menu.sh /path/to/TestingSkybox}
ZIG=${ZIG:-zig}
OUT=build

if [ ! -f "$SRC/menu_core.cpp" ]; then
    echo "$SRC does not look like a TestingSkybox checkout" >&2
    exit 2
fi

echo "== assembling sources"
rm -rf "$OUT"
mkdir -p "$OUT/src" "$OUT/win" "$OUT/lib"

cp "$SRC"/*.h "$SRC"/*.cpp "$OUT/src/"
mv "$OUT/src/game_hooks.h" "$OUT/src/menu_core.h" "$OUT/" 2>/dev/null || true
cp "$OUT/src/spawn_guard.h" "$OUT/win/"
cp "$OUT/src/log.h" "$OUT/win/" 2>/dev/null || true

# the patched files and the new module, over the top of the checkout's copies
cp patched/menu_core.h "$OUT/menu_core.h"
cp patched/menu_core.h "$OUT/src/menu_core.h"
cp patched/menu_core.cpp "$OUT/src/menu_core.cpp"
cp patched/win_menu.cpp "$OUT/src/win_menu.cpp"
cp patched/dinput8_proxy.cpp "$OUT/src/dinput8_proxy.cpp"
cp freecam_menu.rc freecam_menu.manifest "$OUT/src/"
cp freecam_game.h freecam_game.cpp input_block.h input_block.cpp "$OUT/src/"
cp ../src/freecam.h ../src/freecam.c "$OUT/src/"

echo "== spawn guard fallback"
cat > "$OUT/src/spawn_guard_stub.cpp" <<'EOF'
// Stand-in for spawn_guard.S, which is not in the menu's public repository.
#include "../win/spawn_guard.h"
extern "C" {
volatile long g_spawnFaulted = 0;
volatile long g_spawnTries = 0;
volatile int tw_in_spawn = 0;
void* tw_spawn_target = nullptr;
void tw_spawn_fail(void) {}
void tw_spawn_guarded(void* idOut, void* hashPtr, const void* matrix, int flags) {
    typedef void (*Fn)(void*, void*, const void*, int, void*, void*);
    Fn fn = (Fn)tw_spawn_target;
    if (!fn) return;
    tw_in_spawn = 1;
    fn(idOut, hashPtr, matrix, flags, nullptr, nullptr);
    tw_in_spawn = 0;
}
}
EOF

echo "== import library for D3DCOMPILER_47.dll (MinGW ships 46 and below)"
printf 'LIBRARY D3DCOMPILER_47.dll\nEXPORTS\n    D3DCompile\n' > "$OUT/lib/d3dcompiler_47.def"
printf 'void D3DCompile(void) {}\n' > "$OUT/lib/stub.c"
$ZIG cc -target x86_64-windows-gnu -shared -o "$OUT/lib/fake.dll" "$OUT/lib/stub.c" \
    "$OUT/lib/d3dcompiler_47.def" -Wl,/implib:"$OUT/lib/d3dcompiler_47.lib"

echo "== compiling"
cd "$OUT"
for f in src/dinput8_proxy.cpp src/win_menu.cpp src/menu_core.cpp src/log.cpp \
         src/game_hooks.cpp src/asset_names.cpp src/spawn_guard_stub.cpp \
         src/freecam_game.cpp src/input_block.cpp src/imgui.cpp src/imgui_draw.cpp \
         src/imgui_tables.cpp src/imgui_widgets.cpp src/imgui_impl_win32.cpp \
         src/imgui_impl_dx11.cpp; do
    echo "   $f"
    $ZIG c++ -target x86_64-windows-gnu -std=c++17 -fms-extensions -c -O2 -w -I. -Iwin -Isrc \
        -o "$(basename "$f" .cpp).o" "$f"
done
echo "   src/freecam.c"
$ZIG cc -target x86_64-windows-gnu -std=c99 -c -O2 -w -Isrc -o freecam.o src/freecam.c

echo "== resources (version information and a manifest, for the scanners)"
$ZIG rc -- src/freecam_menu.rc freecam_menu.res.o

echo "== linking"
$ZIG c++ -target x86_64-windows-gnu -shared -O2 -o dinput8.dll \
    ./*.o lib/d3dcompiler_47.lib \
    -ld3d11 -ldxgi -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -lxinput1_3 \
    -ldinput8 -lversion -lwinmm

mkdir -p ../dist
cp dinput8.dll ../dist/dinput8.dll
rm -f ./*.pdb ./*.lib
echo "== done: dist/dinput8.dll"
ls -l ../dist/dinput8.dll

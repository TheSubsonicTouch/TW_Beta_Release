#!/bin/sh
# Builds the two Windows binaries and runs the unit tests.
#
#   ./build.sh                 uses `zig` from PATH
#   ZIG=/path/to/zig ./build.sh
#
# zig ships a MinGW-w64 toolchain, so this cross compiles from Linux or macOS and also
# works on Windows itself. Any x86_64-w64-mingw32-gcc would do the same job.
set -e
cd "$(dirname "$0")"
ZIG=${ZIG:-zig}
CC=${CC:-"$ZIG cc"}

echo "== resources (version information and a manifest)"
# An unsigned DLL with no version information is one of the things heuristics score
# badly; this gives it a name, a publisher and a description, as any normal application
# has. Nothing about what it does changes.
(cd src && "$ZIG" rc -- freecam_standalone.rc freecam_standalone.res.o \
           && "$ZIG" rc -- inject.rc inject.res.o)

echo "== free camera DLL"
$CC -target x86_64-windows-gnu -shared -O2 -Wall -Wextra \
    -o dist/FreeCam.dll src/freecam.c src/win32.c src/freecam_standalone.res.o \
    -lkernel32 -luser32

echo "== injector"
$CC -target x86_64-windows-gnu -O2 -Wall -Wextra \
    -o dist/FreeCam-Inject.exe src/inject.c src/inject.res.o -lkernel32

rm -f dist/*.pdb dist/*.lib dist/*.exp src/*.res.o   # zig leaves these behind; they are not shipped

echo "== unit tests"
python3 tests/run_tests.py

echo "== done:"
ls -l dist

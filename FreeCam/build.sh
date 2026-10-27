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

echo "== free camera DLL"
$CC -target x86_64-windows-gnu -shared -O2 -Wall -Wextra \
    -o dist/FreeCam.dll src/freecam.c src/win32.c -lkernel32 -luser32

echo "== injector"
$CC -target x86_64-windows-gnu -O2 -Wall -Wextra \
    -o dist/FreeCam-Inject.exe src/inject.c -lkernel32

rm -f dist/*.pdb dist/*.lib dist/*.exp   # zig leaves these behind; they are not shipped

echo "== unit tests"
python3 tests/run_tests.py

echo "== done:"
ls -l dist

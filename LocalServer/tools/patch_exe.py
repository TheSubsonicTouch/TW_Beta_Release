"""Null-check patch for the spot-shadow crash in TriadWars.exe (client build 1.6.0.0).

    python patch_exe.py status | apply | restore

The crash: an access violation reading address 0x58 at TriadWars.exe!14000548a, a few
seconds after the world loads, on roughly one launch in seven. The spot-shadow pass
(func 140031590) calls the composite-drawable submit routine (func 140005400) for every
character in the cull bucket. That routine checks that the drawable has a rig but not
that its LOD 0 model has finished loading, and dereferences the model pointer
([drawable + 0x5a8]) straight away.

The fix: the two instructions at 140005486 are replaced by a jump to 22 bytes of unused
padding at the end of .text, which stores the pointer as before, leaves through the
routine's own early-exit path (140005c4e) when it is null, and otherwise carries on. The
drawable is simply left out of that shadow map for the frame.

`apply` keeps the untouched executable next to the game as TriadWars.exe.orig. It works
while the game is running (the running copy is renamed, not overwritten); the patch takes
effect on the next launch. `restore` puts the original back and needs the game closed.
"""
import hashlib
import os
import struct
import sys

GAME_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EXE = os.path.join(GAME_DIR, "TriadWars.exe")
ORIG = EXE + ".orig"
ORIG_SHA256 = "6bd58dbdea8f8acb4d76c24eccff77c14cdd8652408f25c38f7ab8bcb832cf7b"

IMAGE_BASE = 0x140000000
TEXT_VA, TEXT_RAW = 0x140001000, 0x400          # .text: virtual address and file offset

SITE_VA = 0x140005486                           # mov [rbp-0x61], r8 ; mov r15, [r8+0x58]
SITE_OLD = bytes.fromhex("4c89459f" "4d8b7858")
RESUME_VA = 0x14000548E                         # mov [rbp+0x7f], r15
EXIT_VA = 0x140005C4E                           # the routine's early-exit epilogue
CAVE_VA = 0x1417C63E0                           # zero padding after the last function in .text
CAVE_LEN = 22


def _off(va):
    return va - TEXT_VA + TEXT_RAW


def _rel32(src_end, dst):
    return struct.pack("<i", dst - src_end)


def patches():
    """[(file offset, expected bytes, new bytes)]"""
    site = b"\xe9" + _rel32(SITE_VA + 5, CAVE_VA) + b"\x0f\x1f\x00"       # jmp cave ; nop
    cave = bytes.fromhex("4c89459f")                                      # mov [rbp-0x61], r8
    cave += bytes.fromhex("4d85c0")                                       # test r8, r8
    cave += b"\x0f\x84" + _rel32(CAVE_VA + len(cave) + 6, EXIT_VA)        # jz exit
    cave += bytes.fromhex("4d8b7858")                                     # mov r15, [r8+0x58]
    cave += b"\xe9" + _rel32(CAVE_VA + len(cave) + 5, RESUME_VA)          # jmp resume
    assert len(site) == len(SITE_OLD) and len(cave) == CAVE_LEN
    return [(_off(SITE_VA), SITE_OLD, site), (_off(CAVE_VA), bytes(CAVE_LEN), cave)]


def state(data):
    """'original', 'patched' or 'unknown' for the bytes of an executable."""
    old = all(data[o:o + len(a)] == a for o, a, b in patches())
    new = all(data[o:o + len(b)] == b for o, a, b in patches())
    return "original" if old else "patched" if new else "unknown"


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def status():
    print("TriadWars.exe: %s" % state(_read(EXE)))
    print("TriadWars.exe.orig: %s" % ("present" if os.path.exists(ORIG) else "absent"))


def apply():
    data = _read(EXE)
    st = state(data)
    if st == "patched":
        print("already patched")
        return
    if st != "original" or hashlib.sha256(data).hexdigest() != ORIG_SHA256:
        sys.exit("TriadWars.exe is not the build this patch was written for; nothing changed")
    if os.path.exists(ORIG):
        sys.exit("TriadWars.exe.orig already exists; nothing changed")
    out = bytearray(data)
    for o, a, b in patches():
        out[o:o + len(b)] = b
    tmp = EXE + ".new"
    with open(tmp, "wb") as f:
        f.write(out)
    os.rename(EXE, ORIG)            # allowed even while the game is running
    try:
        os.rename(tmp, EXE)
    except OSError:
        os.rename(ORIG, EXE)
        raise
    print("patched; original kept as TriadWars.exe.orig")


def restore():
    if not os.path.exists(ORIG):
        sys.exit("no TriadWars.exe.orig to restore")
    if hashlib.sha256(_read(ORIG)).hexdigest() != ORIG_SHA256:
        sys.exit("TriadWars.exe.orig is not the original executable; nothing changed")
    try:
        os.replace(ORIG, EXE)
    except PermissionError:
        sys.exit("close the game first")
    print("original executable restored")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    {"status": status, "apply": apply, "restore": restore}.get(cmd, status)()

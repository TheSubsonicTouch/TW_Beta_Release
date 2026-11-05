"""Pack everything needed to set this local server up on another copy of the game.

    python make_bundle.py [out.zip]        default: TriadWars-LocalServer-Handoff.zip in the game folder

The zip holds the handoff instructions, the launcher, the server with its data, the
tools and the reference data. It does NOT hold the game itself, the player's profile
(state.json), logs, crash dumps or backups. Paths inside the zip are relative to the game
folder, so it is extracted straight into the folder that contains TriadWars.exe.
"""
import glob
import os
import sys
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
INCLUDE = [
    "HANDOFF.txt",
    "Play-TriadWars.bat",
    "LocalServer/README.md",
    "LocalServer/server.py",
    "LocalServer/handlers.py",
    "LocalServer/state.py",
    "LocalServer/schema.py",
    "LocalServer/useroptions.py",
    "LocalServer/start.ps1",
    "LocalServer/cert.pem",
    "LocalServer/key.pem",
    "LocalServer/data/*.json",
    "LocalServer/tools/*.py",
    "LocalServer/devdata/*",
]


def main(out):
    files = []
    for pattern in INCLUDE:
        found = sorted(glob.glob(os.path.join(ROOT, *pattern.split("/"))))
        if not found:
            sys.exit("missing: " + pattern)
        files += [f for f in found if os.path.isfile(f)]
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in files:
            z.write(f, os.path.relpath(f, ROOT).replace(os.sep, "/"))
    print("%s: %d files, %.1f MB" % (out, len(files), os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "TriadWars-LocalServer-Handoff.zip"))

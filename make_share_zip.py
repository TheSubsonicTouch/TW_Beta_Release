"""Rebuild TriadWars-LocalServer-Share.zip from this checkout.

    python make_share_zip.py [out.zip]

The release zip holds the launcher, the readme and the LocalServer folder exactly
as they must sit next to TriadWars.exe after extraction. Paths inside the zip are
relative to the game folder. Run this after changing anything under LocalServer/.
"""
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(ROOT, "TriadWars-LocalServer-Share.zip")
INCLUDE = [
    "README.txt",
    "Play-TriadWars.bat",
    "Change-Look.bat",
    "LocalServer/README.md",
    "LocalServer/cert.pem",
    "LocalServer/key.pem",
    "LocalServer/server.py",
    "LocalServer/handlers.py",
    "LocalServer/state.py",
    "LocalServer/schema.py",
    "LocalServer/useroptions.py",
    "LocalServer/start.ps1",
    "LocalServer/data/*.json",
    "LocalServer/tools/*.py",
    "LocalServer/devdata/*",
]
# Never ship a profile, a log or a crash dump.
SKIP = {"state.json"}


def main(out=DEFAULT_OUT):
    files, missing = [], []
    for pattern in INCLUDE:
        if "*" in pattern:
            import glob
            found = sorted(glob.glob(os.path.join(ROOT, *pattern.split("/"))))
        else:
            path = os.path.join(ROOT, *pattern.split("/"))
            found = [path] if os.path.isfile(path) else []
        if not found:
            missing.append(pattern)
        files += [f for f in found if os.path.basename(f) not in SKIP]
    if missing:
        sys.exit("missing: " + ", ".join(missing))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in files:
            z.write(path, os.path.relpath(path, ROOT).replace(os.sep, "/"))
    print("%s: %d files, %.1f MB" % (out, len(files), os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main(*(sys.argv[1:2] or []))

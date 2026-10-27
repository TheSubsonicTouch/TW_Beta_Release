"""Package the free camera into TriadWars-FreeCam.zip.

    python FreeCam/make_zip.py

Extracting the zip never touches the game on its own: the DLL that replaces the
menu sits in a subfolder, and Install-FreeCam.bat copies it into place after saving
your current dinput8.dll as dinput8.original.dll. Running the uninstaller puts it
back.
"""
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MENU_DLL = os.path.join(HERE, "menu", "dist", "dinput8.dll")
STANDALONE = os.path.join(HERE, "dist")
PACKAGING = os.path.join(HERE, "packaging")
OUT = os.path.join(HERE, "TriadWars-FreeCam.zip")

TOP = [
    ("Install-FreeCam.bat", "Install-FreeCam.bat"),
    ("Uninstall-FreeCam.bat", "Uninstall-FreeCam.bat"),
    ("README.txt", "menu-README.txt"),
    ("FreeCam/dinput8.dll", MENU_DLL),
]

STANDALONE_FILES = ["FreeCam.dll", "FreeCam-Inject.exe", "FreeCam.ini", "FreeCam.camera.ini",
                    "Play-TriadWars-FreeCam.bat", "README.txt"]


def main():
    entries = []
    for arcname, name in TOP:
        path = name if os.path.isabs(name) else os.path.join(PACKAGING, name)
        if not os.path.exists(path):
            raise SystemExit("missing " + path)
        entries.append((arcname, path))
    for name in STANDALONE_FILES:
        path = os.path.join(STANDALONE, name)
        if not os.path.exists(path):
            raise SystemExit("missing " + path)
        entries.append(("standalone/" + name, path))

    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for arcname, path in entries:
            info = zipfile.ZipInfo(arcname, date_time=(2026, 10, 27, 12, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            binary = not arcname.lower().endswith(".bat") and not arcname.lower().endswith(".txt")
            info.external_attr = ((0o755 if not binary else 0o644) << 16)
            with open(path, "rb") as f:
                z.writestr(info, f.read())

    print("%s: %d files, %d bytes" % (os.path.basename(OUT), len(entries),
                                      os.path.getsize(OUT)))
    for arcname, path in entries:
        print("   %-42s %8d" % (arcname.replace("/", "\\"), os.path.getsize(path)))


if __name__ == "__main__":
    main()

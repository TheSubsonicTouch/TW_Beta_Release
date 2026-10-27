"""Package the free camera into TriadWars-FreeCam.zip.

    python FreeCam/make_zip.py

The zip is laid out so that extracting it into the game folder is the whole install:
dinput8.dll (the menu with a Free Camera entry) lands next to TriadWars.exe, and the
standalone hot-key build sits in a subfolder for anyone who would rather not replace
the menu DLL.
"""
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MENU_DLL = os.path.join(HERE, "menu", "dist", "dinput8.dll")
STANDALONE = os.path.join(HERE, "dist")
README = os.path.join(HERE, "packaging", "menu-README.txt")
OUT = os.path.join(HERE, "TriadWars-FreeCam.zip")

TOP = [("README.txt", README), ("dinput8.dll", MENU_DLL)]
STANDALONE_FILES = ["FreeCam.dll", "FreeCam-Inject.exe", "FreeCam.ini", "FreeCam.camera.ini",
                    "Play-TriadWars-FreeCam.bat", "README.txt"]


def main():
    entries = []
    for name, path in TOP:
        if not os.path.exists(path):
            raise SystemExit("missing " + path)
        entries.append((name, path))
    for name in STANDALONE_FILES:
        path = os.path.join(STANDALONE, name)
        if not os.path.exists(path):
            raise SystemExit("missing " + path)
        entries.append(("standalone/" + name, path))

    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for arcname, path in entries:
            info = zipfile.ZipInfo(arcname, date_time=(2026, 10, 27, 12, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(path, "rb") as f:
                z.writestr(info, f.read())

    size = os.path.getsize(OUT)
    print("%s: %d files, %d bytes" % (os.path.basename(OUT), len(entries), size))
    for arcname, path in entries:
        print("   %-40s %8d" % (arcname, os.path.getsize(path)))


if __name__ == "__main__":
    main()

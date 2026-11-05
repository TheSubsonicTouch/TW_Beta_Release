"""Package the free camera into TriadWars-FreeCam.zip.

    python FreeCam/make_zip.py

Extracting the zip never touches the game on its own: the DLL that replaces the
menu sits in a subfolder, and Install-FreeCam.bat copies it into place after saving
your current dinput8.dll as dinput8.original.dll. Running the uninstaller puts it
back.
"""
import hashlib
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MENU_DLL = os.path.join(HERE, "menu", "dist", "dinput8.dll")
PACKAGING = os.path.join(HERE, "packaging")
OUT = os.path.join(HERE, "TriadWars-FreeCam.zip")

# What goes in the zip.
#
# Two deliberate choices, both about the false virus warnings archives of game mods get:
#
#   * the menu DLL is shipped as "menu-plugin.dll" and the installer copies it to
#     dinput8.dll. A file called dinput8.dll inside a downloaded archive is the shape of
#     the classic "proxy DLL next to a game" trick, and scanners score it that way. The
#     file is a plugin until the installer puts it in place; once in place it is called
#     dinput8.dll, as the game requires.
#   * the standalone injector (FreeCam-Inject.exe) is no longer shipped. It uses
#     CreateRemoteThread/WriteProcessMemory, which every engine reads as a process
#     injector - and it is one, even if only for this camera. The menu DLL does not use
#     any of those calls. The standalone build still exists in the repo (./build.sh) for
#     anyone who wants it; it is not part of the release.
TOP = [
    ("Install-FreeCam.bat", "Install-FreeCam.bat"),
    ("Uninstall-FreeCam.bat", "Uninstall-FreeCam.bat"),
    ("README.txt", "menu-README.txt"),
    ("SHA256SUMS.txt", None),                 # generated below
    ("FreeCam/menu-plugin.dll", MENU_DLL),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    entries = []
    for arcname, name in TOP:
        if name is None:
            continue
        path = name if os.path.isabs(name) else os.path.join(PACKAGING, name)
        if not os.path.exists(path):
            raise SystemExit("missing " + path)
        entries.append((arcname, path))

    # The sums go in the zip so anyone can check what they downloaded without trusting
    # the place it came from - and so a scanner's report can be compared with the file.
    sums = "".join("%s  %s\n" % (sha256(path), arcname.replace("/", "\\"))
                   for arcname, path in entries)
    sums_path = os.path.join(PACKAGING, "SHA256SUMS.txt")
    with open(sums_path, "w", newline="\r\n") as f:
        f.write("SHA-256 of every file in this archive.\n"
                "Check one with:  certutil -hashfile \"FreeCam\\menu-plugin.dll\" SHA256\n"
                "or in PowerShell: Get-FileHash -Algorithm SHA256 <file>\n\n")
        f.write(sums)
    entries.insert(3, ("SHA256SUMS.txt", sums_path))

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
    print("   sha256 of the zip itself: %s" % sha256(OUT))
    for arcname, path in entries:
        print("   %-42s %8d" % (arcname.replace("/", "\\"), os.path.getsize(path)))


if __name__ == "__main__":
    main()

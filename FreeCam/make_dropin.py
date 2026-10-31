"""Package the debug menu as a drag-and-drop folder.

    python FreeCam/make_dropin.py

The other package (make_zip.py) ships the DLL as FreeCam\\menu-plugin.dll and needs
Install-FreeCam.bat to copy it into place. This one is the opposite trade: the DLL is
already called dinput8.dll, so it can simply be dragged into the game folder, at the cost
of looking like the "proxy DLL next to a game" pattern that scanners score badly.

Both packages carry the same DLL, byte for byte, built from the same sources - this script
does not rebuild anything, it only arranges what ./menu/build_menu.sh already produced.

The README is generated from packaging/menu-README.txt rather than kept as a second copy,
so the long explanations (what the camera does, the false-positive section, the log
troubleshooting) cannot drift apart between the two packages. Only the parts that are
genuinely different - how it goes in, how it comes out, and the one naming paragraph -
are replaced here, and each replacement fails loudly if its anchor is not found.
"""
import hashlib
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(HERE, "menu", "dist", "dinput8.dll")
README = os.path.join(HERE, "packaging", "menu-README.txt")
OUT_DIR = os.path.join(HERE, "dropin")
OUT_ZIP = os.path.join(HERE, "TriadWars-DebugMenu-DragAndDrop.zip")

PACKAGE_TITLE = "\r\n".join([
    "Triad Wars - Debug Menu, drag and drop",
    "======================================",
    "",
    "No installer and no .bat files in this package: it is dinput8.dll, this README and",
    "the hashes. Putting it in is one drag.",
    "",
    "QUICK START",
    "",
    "  1. If your game folder already has a dinput8.dll in it, rename it to",
    "     dinput8.original.dll first (Explorer: click it, press F2). Dragging over it",
    "     without that step loses the file you had.",
    "",
    "  2. Drag dinput8.dll into the folder that holds TriadWars.exe and",
    "     Play-TriadWars.bat.",
    "",
    "  3. Start the game as usual and get into the world - not the title screen, not a",
    "     loading screen. Press Insert, arrow down to \"Free Camera\" and press Enter.",
    "     F10 puts the camera back at any time, from anywhere.",
    "",
    "  KEY        WHAT IT DOES",
    "  Insert     open and close this menu",
    "  Arrows     move through the menu      Enter  choose      Backspace  back",
    "  Free Camera  the menu entry that turns the camera on and off",
    "  W A S D    fly                Space / C  up / down",
    "  right mouse  look around      Shift / Ctrl  faster / slower",
    "  L / J / M  record a keyframe / play the path / clear it",
    "  Left/Right playback speed     O  freeze      P  follow the player",
    "  B          spawn an NPC 3 m ahead of the player",
    "  N          next NPC model     H  hostile or friendly",
    "  F10        panic: put the camera back and switch off",
    "",
    "Everything below is the same document the installer package carries, with the",
    "install, uninstall and file-name paragraphs changed to match this layout.",
    "",
    "",
])

NEW_INSTALL = (
    "INSTALL - DRAG AND DROP\r\n"
    "-----------------------\r\n"
    "\r\n"
    "  1. Open this folder and your Triad Wars folder side by side.\r\n"
    "\r\n"
    "  2. If your game folder already has a dinput8.dll in it - the debug menu, or an\r\n"
    "     older version of this - rename it to dinput8.original.dll before you go on.\r\n"
    "     That is your undo button, and it takes one keypress.\r\n"
    "\r\n"
    "  3. Drag dinput8.dll into the game folder: the one with TriadWars.exe and\r\n"
    "     Play-TriadWars.bat in it. Windows may ask whether to replace a file - say yes\r\n"
    "     only if you already saved the old one in step 2.\r\n"
    "\r\n"
    "  4. Start the game with Play-TriadWars.bat as usual.\r\n"
    "\r\n"
    "  5. Once you are in the world (not the title screen or a loading screen), press\r\n"
    "     Insert. The menu comes up with a Free Camera entry and the NPC spawn entries.\r\n"
    "\r\n"
    "The other package does exactly this for you, backup included, and has the .bat files\r\n"
    "that put everything back. If you would rather double-click than drag, use that one.\r\n"
    "\r\n"
    "This package installs the menu build only - the one with the Free Camera entry. The\r\n"
    "older standalone build (a DLL loaded by a small injector program) is not shipped in\r\n"
    "either package: the injector is the file scanners object to most, and it did the one\r\n"
    "thing this mod otherwise never does, which is touch another process. It still builds\r\n"
    "from the repository with ./build.sh if you want it.\r\n"
)

NEW_UNINSTALL = (
    "UNINSTALL\r\n"
    "---------\r\n"
    "\r\n"
    "Delete dinput8.dll from the game folder, then rename dinput8.original.dll back to\r\n"
    "dinput8.dll. If the game folder never had a menu in it, deleting dinput8.dll is the\r\n"
    "whole uninstall. Nothing else is written to the game folder except TW_MenuLog.txt,\r\n"
    "and FreeCam.npc.txt if you made one for the spawner.\r\n"
    "\r\n"
    "Either way the game runs fine with no dinput8.dll there: that file is only the menu.\r\n"
)

OLD_AV_BULLET = (
    "  - the DLL is shipped as FreeCam\\menu-plugin.dll and the installer copies it to\r\n"
    "    dinput8.dll. A file called dinput8.dll sitting in a downloaded archive is the pattern\r\n"
    "    scanners look for; in place, it has to be called dinput8.dll, and that is the\r\n"
    "    installer's job.\r\n"
)

NEW_AV_BULLET = (
    "  - same DLL as the installer package, but this one is already called dinput8.dll,\r\n"
    "    because that is the whole point of dragging it in. That is the one thing the other\r\n"
    "    package does better: a file with that name inside a downloaded archive is the\r\n"
    "    pattern scanners look for, so there it ships as FreeCam\\menu-plugin.dll and the\r\n"
    "    installer renames it. If a scanner objects to this one, use that one - the bytes\r\n"
    "    are identical, and SHA256SUMS.txt shows it.\r\n"
)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def replace_section(text, header, new_body, next_header):
    """Swap the body under `header` for `new_body`, up to `next_header`.

    `next_header` of None means the section runs to the end of the document - which is
    the case for UNINSTALL, the last thing in the file."""
    start = text.find(header)
    if start < 0:
        raise SystemExit("anchor not found: " + header.strip())
    if next_header is None:
        return text[:start] + new_body
    end = text.find(next_header, start + len(header))
    if end < 0:
        raise SystemExit("end anchor not found after " + header.strip() + ": " + next_header.strip())
    return text[:start] + new_body + "\r\n" + text[end:]


def build_readme():
    with open(README, "r", encoding="utf-8", newline="") as f:
        text = f.read()

    # the title block of the installer package goes; this package has its own
    title_end = text.find("\r\n\r\n")
    if title_end < 0:
        raise SystemExit("could not find the title block")
    text = PACKAGE_TITLE + text[title_end + 4:]

    text = replace_section(text, "INSTALL\r\n-------\r\n", NEW_INSTALL,
                           "WHAT THE FREE CAMERA DOES")
    text = replace_section(text, "UNINSTALL\r\n---------\r\n", NEW_UNINSTALL, None)

    if OLD_AV_BULLET not in text:
        raise SystemExit("the false-positive paragraph about menu-plugin.dll was not found")

    # The new bullet talks about menu-plugin.dll on purpose, so it must not be caught by
    # the blanket rename below: park it behind a token and swap it in at the end.
    text = text.replace(OLD_AV_BULLET, "@@AVBULLET@@\r\n", 1)

    text = text.replace("certutil -hashfile FreeCam\\menu-plugin.dll SHA256",
                        "certutil -hashfile dinput8.dll SHA256")
    text = text.replace("FreeCam\\menu-plugin.dll", "dinput8.dll")

    # there is no installer in this package, so do not tell the reader to run one
    text = text.replace(
        "is loading an older dinput8.dll: run Install-FreeCam.bat again from this zip.",
        "is loading an older dinput8.dll: drag this package's dinput8.dll in again.")
    text = text.replace(
        "Put your original dinput8.dll back with Uninstall-FreeCam.bat.",
        "Put your original dinput8.dll back by deleting this one and renaming the backup.")

    if "@@AVBULLET@@" not in text:
        raise SystemExit("the placeholder for the false-positive paragraph went missing")
    text = text.replace("@@AVBULLET@@\r\n", NEW_AV_BULLET, 1)
    return text


def main():
    if not os.path.exists(DLL):
        raise SystemExit("missing %s - build it with ./menu/build_menu.sh first" % DLL)

    os.makedirs(OUT_DIR, exist_ok=True)
    readme = build_readme()

    dll_out = os.path.join(OUT_DIR, "dinput8.dll")
    readme_out = os.path.join(OUT_DIR, "README.txt")
    sums_out = os.path.join(OUT_DIR, "SHA256SUMS.txt")

    with open(DLL, "rb") as src, open(dll_out, "wb") as dst:
        dst.write(src.read())
    with open(readme_out, "w", encoding="utf-8", newline="") as f:
        f.write(readme)

    entries = [("dinput8.dll", dll_out), ("README.txt", readme_out)]
    # newline="" plus explicit CRLF: writing \r\n into a file opened with newline="\r\n"
    # would turn into \r\r\n, which certutil would then not match.
    with open(sums_out, "w", encoding="utf-8", newline="") as f:
        f.write("SHA-256 of every file in this package.\r\n"
                "Check one with:  certutil -hashfile dinput8.dll SHA256\r\n"
                "or in PowerShell: Get-FileHash -Algorithm SHA256 <file>\r\n\r\n")
        for name, path in entries:
            f.write("%s  %s\r\n" % (sha256(path), name))
    entries.append(("SHA256SUMS.txt", sums_out))

    with zipfile.ZipFile(OUT_ZIP, "w", zipfile.ZIP_DEFLATED) as z:
        for name, path in entries:
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 27, 12, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = ((0o755 if name.endswith(".dll") else 0o644) << 16)
            with open(path, "rb") as f:
                z.writestr(info, f.read())

    print("%s: %d files, %d bytes" % (os.path.basename(OUT_ZIP), len(entries),
                                      os.path.getsize(OUT_ZIP)))
    print("   sha256 of the zip itself: %s" % sha256(OUT_ZIP))
    for name, path in entries:
        print("   %-20s %9d" % (name, os.path.getsize(path)))
    print("   the loose files are in %s, ready to drag" % os.path.relpath(OUT_DIR, HERE))


if __name__ == "__main__":
    main()

"""Package the debug menu as a drop-in-ready folder and archive.

    python FreeCam/make_dropin.py

The other package (make_zip.py) ships the DLL as FreeCam\\menu-plugin.dll and needs
Install-FreeCam.bat to copy it into place. This one is the opposite trade: the DLL is
already called dinput8.dll, so it can simply be dragged into the game folder, at the cost
of looking like the "proxy DLL next to a game" pattern that scanners score badly.

Both packages carry the same DLL, byte for byte, built from the same sources - this script
does not rebuild anything, it only arranges what ./menu/build_menu.sh already produced.

The folder itself (FreeCam/DropIn-Ready) is committed as well as zipped: it is what gets
dragged, so what is in the repository is the thing the instructions describe. Run this
script again after a rebuild and commit both the folder and the zip.

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
OUT_DIR = os.path.join(HERE, "DropIn-Ready")
OUT_ZIP = os.path.join(HERE, "TriadWars-DropIn-Ready.zip")

PACKAGE_TITLE = "\r\n".join([
    "Triad Wars - Debug Menu, drop-in ready",
    "======================================",
    "",
    "No installer and no .bat files in this package: it is dinput8.dll, START-HERE.txt,",
    "this README and the hashes. Putting it in is one drag.",
    "",
    "START-HERE.txt is the short version - three steps and the keys. Read that first;",
    "this document is the long one, and everything in it still applies.",
    "",
    "QUICK START",
    "",
    "  1. If your game folder already has a dinput8.dll in it, rename it to",
    "     dinput8.original.dll first (Explorer: click it, press F2). Dragging over it",
    "     without that step loses the file you had.",
    "",
    "  2. Select everything in this folder (Ctrl+A) and drag the selection into the",
    "     folder that holds TriadWars.exe and Play-TriadWars.bat. The files, not the",
    "     folder: a folder inside a folder is one level too deep, and the game only",
    "     looks for dinput8.dll right next to TriadWars.exe.",
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

START_HERE = "\r\n".join([
    "Triad Wars - Debug Menu: the drop-in install",
    "============================================",
    "",
    "You need this folder and the folder the game is installed in - the one with",
    "TriadWars.exe and Play-TriadWars.bat in it.",
    "",
    "1. SAVE WHAT IS THERE",
    "   Look in the game folder for a file called dinput8.dll. If it is already there,",
    "   click it, press F2 and rename it to dinput8.original.dll. That is your undo,",
    "   and it takes one keypress. Most game folders do not have one.",
    "",
    "2. DRAG THE FILES, NOT THE FOLDER",
    "   Select everything in this folder (Ctrl+A) and drag the selection onto the game",
    "   folder. If Windows asks to replace a file, say yes - you saved the old one in",
    "   step 1.",
    "",
    "   Do not drag the folder itself: the game looks for dinput8.dll right next to",
    "   TriadWars.exe, and a folder inside a folder is one level too deep, where it is",
    "   simply never loaded. If that happens, open the inner folder and move the files",
    "   up one level - nothing is broken, it just was not seen.",
    "",
    "3. PLAY",
    "   Start the game with Play-TriadWars.bat, get into the world (not the title",
    "   screen, not a loading screen), and press Insert for the menu.",
    "",
    "   Insert        open and close the menu",
    "   Arrows, Enter move through it and choose",
    "   Free Camera   the entry that turns the flying camera on",
    "   W A S D       fly            Space / C   up / down      right mouse  look",
    "   B             spawn an NPC 3 m ahead      N  next model   H  hostile/friendly",
    "   F10           panic: camera back, everything off",
    "",
    "   WORLD -> \"Sky override\" paints the sky, and SPAWN -> \"Models found\" reads the",
    "   game files so the model list holds the NPCs this installation actually has.",
    "",
    "TO REMOVE IT",
    "   Delete dinput8.dll from the game folder, and rename dinput8.original.dll back",
    "   to dinput8.dll if you made one. Nothing else is added except TW_MenuLog.txt",
    "   (and FreeCam.npc.txt, if the spawner wrote one).",
    "",
    "CHECKING THE FILE (optional)",
    "   certutil -hashfile dinput8.dll SHA256      (Get-FileHash in PowerShell)",
    "   The hash should match SHA256SUMS.txt in this folder.",
    "",
    "IF A SCANNER OBJECTS",
    "   README.txt next to this file explains why an unsigned DLL a game loads gets",
    "   flagged, and what can be done about it. It has the full feature list and the",
    "   troubleshooting steps as well.",
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
    "  3. Drag the files themselves into the game folder: the one with TriadWars.exe\r\n"
    "     and Play-TriadWars.bat in it. Select all of them (Ctrl+A) and drag the\r\n"
    "     selection - not the folder, which would land one level too deep where the\r\n"
    "     game never looks. Windows may ask whether to replace a file - say yes only if\r\n"
    "     you already saved the old one in step 2.\r\n"
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
    # The folder is committed, so anything left in it from an older run would be shipped
    # as well. Keep exactly the four files this script writes.
    for name in os.listdir(OUT_DIR):
        if name not in ("START-HERE.txt", "dinput8.dll", "README.txt", "SHA256SUMS.txt"):
            os.remove(os.path.join(OUT_DIR, name))
    readme = build_readme()

    dll_out = os.path.join(OUT_DIR, "dinput8.dll")
    readme_out = os.path.join(OUT_DIR, "README.txt")
    sums_out = os.path.join(OUT_DIR, "SHA256SUMS.txt")
    start_out = os.path.join(OUT_DIR, "START-HERE.txt")

    with open(DLL, "rb") as src, open(dll_out, "wb") as dst:
        dst.write(src.read())
    with open(readme_out, "w", encoding="utf-8", newline="") as f:
        f.write(readme)
    with open(start_out, "w", encoding="utf-8", newline="") as f:
        f.write(START_HERE)

    # START-HERE first: it is the file a new player should open, and it is the one that
    # has to be read before the DLL is dragged anywhere.
    entries = [("START-HERE.txt", start_out), ("dinput8.dll", dll_out),
               ("README.txt", readme_out)]
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

"""Package the debug menu as the smallest thing that can be dragged in.

    python FreeCam/make_simple.py

The installer package (make_zip.py) needs its .bat, and the documented drop-in
package (make_dropin.py) carries a 58 KB README next to the DLL. This one is for a
player who wants to hand someone a folder and say "drag this in": one DLL and one
page of text, nothing to read twice, nothing to run.

    FreeCam/Simple-Drop/
        dinput8.dll       <- the menu, already named for the game to load it
        START-HERE.txt    <- three steps, the keys, and how to undo it

Zipped as FreeCam/TriadWars-Simple-Drop.zip with the same two files at the top
level, so "Extract here" in the game folder is the same install as the folder.

It does not rebuild anything: it copies what ./menu/build_menu.sh produced, which
is the same binary the other two packages carry. Run it after a rebuild, then
commit the folder and the zip.
"""
import hashlib
import os
import shutil
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(HERE, "menu", "dist", "dinput8.dll")
OUT_DIR = os.path.join(HERE, "Simple-Drop")
OUT_ZIP = os.path.join(HERE, "TriadWars-Simple-Drop.zip")

# The page. Written with CRLF because it is opened in Notepad on Windows.
START_HERE = "\r\n".join([
    "Triad Wars - Debug Menu",
    "=======================",
    "",
    "Everything in this folder goes into the folder that has TriadWars.exe in it.",
    "",
    "1. If the game folder already has a file called dinput8.dll, rename that one to",
    "   dinput8.original.dll first. That is your undo, and most folders do not have one.",
    "",
    "2. Select BOTH files here (Ctrl+A) and drag them onto the game folder. Say yes if",
    "   Windows asks to replace something. Drag the files, not this folder: the game",
    "   looks for dinput8.dll right next to TriadWars.exe and does not look inside",
    "   folders.",
    "",
    "3. Start the game, get into the world - not the title screen, not a loading",
    "   screen - and press Insert.",
    "",
    "   Insert            open and close the menu",
    "   Arrows, Enter     move through it and choose",
    "   Free Camera       turns the flying camera on",
    "   W A S D           fly          Space / C  up / down       right mouse  look",
    "   B / N / H         spawn an NPC 3 m ahead / next NPC / hostile",
    "   G / T             spawn a prop 3 m ahead / next prop",
    "   V                 noclip (fly the character itself, through walls)",
    "   F10               panic: camera back, everything off",
    "",
    "   SPAWN's model list starts with the rival triads - 18K, Sun On Yee, Water Street,",
    "   the Dog Boys and the elite enforcers. Pick one and it spawns hostile.",
    "",
    "   The menu has a PROPS section with the game's own objects - vehicles, weapons,",
    "   and every prop the game files name - and draws a crate where the next one will",
    "   land. GRAB/SPAWN rows say which key does the same thing.",
    "",
    "TO TAKE IT OUT",
    "   Delete dinput8.dll from the game folder, and rename dinput8.original.dll back",
    "   to dinput8.dll if you made one. The only file it writes is TW_MenuLog.txt",
    "   (plus FreeCam.npc.txt / FreeCam.props.txt if you ran a model scan).",
    "",
    "CHECKING THE FILE (optional)",
    "   certutil -hashfile dinput8.dll SHA256",
    "",
    "If a virus scanner objects, that is the unsigned DLL a game loads, not what the",
    "DLL does: the long explanation (and everything else the menu can do) is in",
    "README.txt in the other package, TriadWars-DropIn-Ready.zip.",
    "",
])


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    if not os.path.isfile(DLL):
        raise SystemExit("%s is missing - run menu/build_menu.sh first" % DLL)

    os.makedirs(OUT_DIR, exist_ok=True)
    for name in os.listdir(OUT_DIR):
        if name not in ("dinput8.dll", "START-HERE.txt"):
            os.remove(os.path.join(OUT_DIR, name))

    dll_out = os.path.join(OUT_DIR, "dinput8.dll")
    start_out = os.path.join(OUT_DIR, "START-HERE.txt")
    shutil.copyfile(DLL, dll_out)
    # newline="" so the CRLFs already in START_HERE are the only ones written; opening with
    # newline="\r\n" would turn each of them into CR CR LF.
    with open(start_out, "w", newline="", encoding="utf-8") as f:
        f.write(START_HERE)

    entries = [("START-HERE.txt", start_out), ("dinput8.dll", dll_out)]
    with zipfile.ZipFile(OUT_ZIP, "w", zipfile.ZIP_DEFLATED) as z:
        for name, path in entries:
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 5, 12, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as f:
                z.writestr(info, f.read())

    print("%s: %d files, %d bytes" % (os.path.basename(OUT_ZIP), len(entries),
                                      os.path.getsize(OUT_ZIP)))
    print("   dinput8.dll  %d bytes, sha256 %s" % (os.path.getsize(dll_out), sha256(dll_out)))
    print("   the two loose files are in %s, ready to drag" % os.path.relpath(OUT_DIR, HERE))


if __name__ == "__main__":
    main()

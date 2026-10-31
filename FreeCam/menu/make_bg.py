"""Turn the menu's background artwork into a header the DLL can compile in.

    python FreeCam/menu/make_bg.py

The DLL carries the artwork as a resource (freecam_menu.rc, RCDATA 2) and hands the PNG
bytes to Windows Imaging Component at startup, so no C++ file has to hold the picture. The
PNG rather than raw pixels keeps about four megabytes out of the DLL: 640x640 is 1.6 MB
raw and 320 KB as a palette PNG.

The source is assets/triadmenu_dragon.png (1024x1024, red-on-black dragon tattoo pattern).
It is resized to 640 and reduced to 128 colours here, so the shipped asset is reproducible
from the artwork with one command.
"""
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "assets", "triadmenu_dragon.png")
OUT_PNG = os.path.join(HERE, "assets", "triadmenu_bg.png")

SIZE = 640
COLORS = 128

def main():
    art = Image.open(SRC).convert("RGB")
    art = art.resize((SIZE, SIZE), Image.LANCZOS)
    art = art.convert("P", palette=Image.ADAPTIVE, colors=COLORS)
    art.save(OUT_PNG, optimize=True)
    data = open(OUT_PNG, "rb").read()

    print("assets/triadmenu_bg.png: %dx%d, %d colours, %d bytes" % (SIZE, SIZE, COLORS, len(data)))
    print("(freecam_menu.rc embeds it as RCDATA 2; nothing else is generated)")


if __name__ == "__main__":
    main()

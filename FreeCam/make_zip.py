"""Package dist/ into TriadWars-FreeCam.zip, the thing to hand to a player.

    python FreeCam/make_zip.py
"""
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(HERE, "dist")
OUT = os.path.join(HERE, "TriadWars-FreeCam.zip")
FILES = ["FreeCam.dll", "FreeCam-Inject.exe", "FreeCam.ini", "FreeCam.camera.ini",
         "Play-TriadWars-FreeCam.bat", "README.txt"]


def main():
    missing = [f for f in FILES if not os.path.exists(os.path.join(DIST, f))]
    if missing:
        raise SystemExit("missing from dist/: " + ", ".join(missing))
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for name in FILES:
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 27, 12, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(os.path.join(DIST, name), "rb") as f:
                z.writestr(info, f.read())
    size = os.path.getsize(OUT)
    print("%s: %d files, %d bytes" % (os.path.basename(OUT), len(FILES), size))


if __name__ == "__main__":
    main()

"""Build the small add-on zip. The existing LocalServer is required separately."""
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

here = Path(__file__).resolve().parent
files = ['README.txt', 'Player-Model-Changer.bat', 'model_manager.py', 'Mods/Example-Swat/model.json']
out = here.parent / 'TriadWars-PlayerModelChanger.zip'
with ZipFile(out, 'w', ZIP_DEFLATED) as archive:
    for name in files:
        info = ZipInfo('PlayerModelChanger/' + name, (2026, 1, 1, 0, 0, 0))
        info.compress_type = ZIP_DEFLATED
        archive.writestr(info, (here / name).read_bytes())
print(out, out.stat().st_size, 'bytes')

"""Decode / edit the client's XOR-obfuscated UserOptions.dat.

    python useroptions.py show
    python useroptions.py set <Key> <Value>      e.g.  set Environment localhost
    python useroptions.py restore                put back UserOptions.dat.orig
"""
import os
import re
import shutil
import sys

KEY = b"62d76d90779620f566f679dd661720253"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH = os.path.join(ROOT, "UserOptions.dat")
ORIG = PATH + ".orig"


def xor(data):
    return bytes(b ^ KEY[i % len(KEY)] for i, b in enumerate(data))


def read():
    with open(PATH, "rb") as f:
        return xor(f.read()).decode("utf-8")


def write(text):
    if not os.path.exists(ORIG):
        shutil.copy2(PATH, ORIG)
    with open(PATH, "wb") as f:
        f.write(xor(text.encode("utf-8")))


def set_param(text, key, value):
    pat = re.compile(r'(<Param key="%s">)(.*?)(</Param>)' % re.escape(key))
    if pat.search(text):
        return pat.sub(lambda m: m.group(1) + value + m.group(3), text)
    line = '<Param key="%s">%s</Param>' % (key, value)
    return text.replace("</UserOptions>", line + "\r\n</UserOptions>")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "show"
    if cmd == "show":
        print(read())
    elif cmd == "set":
        write(set_param(read(), sys.argv[2], sys.argv[3]))
        print(read())
    elif cmd == "restore":
        shutil.copy2(ORIG, PATH)
        print(read())

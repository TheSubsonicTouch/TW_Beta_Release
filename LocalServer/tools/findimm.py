"""findimm.py <decimal uid> ... : where a 64-bit object UID is hard-coded in the client's code.
The client picks several stores and tutorial items this way, so this shows which screen
uses which store."""
import struct
import sys

from twre import *

if __name__ == "__main__":
    name, va, vsize, raw, rsize, ch = SECS[0]
    for arg in sys.argv[1:]:
        uid = int(arg)
        pat = struct.pack("<Q", uid)
        hits = []
        i = raw
        while True:
            i = data.find(pat, i, raw + rsize)
            if i < 0:
                break
            f = func_of(off2va(i))
            hits.append("%x (func %x)" % (off2va(i), f[0] if f else 0))
            i += 1
        print("%d = %x: %s" % (uid, uid, ", ".join(hits) or "not found"))

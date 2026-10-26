"""Decode the GlobalDNA reflected-object database from the unpacked Global.big chunk,
using the type schema in Reflection.rdb.

    python gamedb.py summary
    python gamedb.py show <TypeName> [max]
    python gamedb.py export <out.json>
"""
import collections
import json
import struct
import sys

import rdbtype
from rdb import NAMES, h64

SRC = "unpack/Global_0279af88.bin"
d = open(SRC, "rb").read()

TYPES = rdbtype.TYPES            # name -> (size, [(fname, ftype, off, count, flags)], src)
TYPEFLAGS = {}
ENUMS = {}                       # enum type name -> {value: label}
for off, tag, p in rdbtype.records():
    if tag == 0xE57F8E15:
        name = NAMES.get(struct.unpack_from("<Q", p, 8)[0])
        TYPEFLAGS[name] = struct.unpack_from("<I", p, 0x1C)[0]
    elif tag == 0x2BD08593:
        name = NAMES.get(struct.unpack_from("<Q", p, 0)[0])
        vals = {}
        for i in range(8, len(p) - 8, 16):
            lab, val = struct.unpack_from("<QQ", p, i)
            vals[val] = NAMES.get(lab, "#%x" % lab)
        ENUMS[name] = vals

# ---- locate the GlobalDNA chunk and its tables -----------------------------
CH = None
o = 0
while o + 16 <= len(d):
    tag, size = struct.unpack_from("<II", d, o)
    if b"GlobalDNA" in d[o + 16:o + 16 + 0x60]:
        CH = o + 16
        break
    o += 16 + size
assert CH is not None, "GlobalDNA chunk not found"
N_OBJ, N_TYPES, N_FIX, N_TYPED = struct.unpack_from("<IIII", d, CH + 0x58)


def rel(pos):
    return pos + struct.unpack_from("<q", d, pos)[0]


TYPED = rel(CH + 0x80)
FIX = rel(CH + 0x78)

FIXUPS = {}
for i in range(N_FIX):
    p = FIX + 24 * i
    a, b, n = struct.unpack_from("<qqQ", d, p)
    FIXUPS[p + a] = (p + 8 + b, n)

OBJECTS = collections.defaultdict(list)   # type name -> [addr]
for i in range(N_TYPED):
    p = TYPED + 16 * i
    t, off = struct.unpack_from("<Qq", d, p)
    name = NAMES.get(t)
    if name in ("UFG::qReflectHandleBase", "UFG::qSymbol", "UFG::qSymbolUC", "UFG::qWiseSymbol"):
        continue
    OBJECTS[name].append(p + 8 + off)

PRIM = {
    "bool": "<?", "char": "<b", "signed char": "<b", "unsigned char": "<B",
    "short": "<h", "unsigned short": "<H", "int": "<i", "unsigned int": "<I",
    "long": "<i", "unsigned long": "<I", "long long": "<q", "unsigned long long": "<Q",
    "float": "<f", "double": "<d",
}
SYMBOLS = {}     # optional reverse map hash -> string, filled by callers


def decode(tname, addr, depth=0):
    if tname in PRIM:
        v = struct.unpack_from(PRIM[tname], d, addr)[0]
        return round(v, 6) if isinstance(v, float) else v
    if tname in ENUMS:
        size = TYPES[tname][0] if tname in TYPES else 4
        v = struct.unpack_from("<I" if size == 4 else "<Q", d, addr)[0]
        return ENUMS[tname].get(v, v)
    if tname in ("UFG::qSymbol", "UFG::qSymbolUC", "UFG::qWiseSymbol"):
        v = struct.unpack_from("<I", d, addr)[0]
        return SYMBOLS.get(v, "sym:0x%08x" % v)
    if tname == "UFG::qReflectString":
        fx = FIXUPS.get(addr)
        if not fx:
            return ""
        return d[fx[0]:fx[0] + fx[1]].split(b"\0")[0].decode("utf-8", "replace")
    if tname and tname.startswith("UFG::qReflectArray<"):
        inner = tname[len("UFG::qReflectArray<"):-1].strip()
        fx = FIXUPS.get(addr)
        n = struct.unpack_from("<I", d, addr + 8)[0]
        if not fx or n == 0:
            return []
        isize = (TYPES[inner][0] if inner in TYPES else struct.calcsize(PRIM.get(inner, "<I")))
        if depth > 6:
            return "<%d x %s>" % (n, inner)
        return [decode(inner, fx[0] + k * isize, depth + 1) for k in range(n)]
    if tname and tname.startswith("UFG::qReflectHandle<"):
        tuid, nuid = struct.unpack_from("<QQ", d, addr + 0x10)
        return {"$handle": str(nuid), "$type": NAMES.get(tuid, "#%x" % tuid)} if nuid else None
    if tname in TYPES:
        size, fields, src = TYPES[tname]
        out = {}
        for fn, ft, fo, cnt, fl in fields:
            if fn == ft or fn.startswith("UFG::q"):      # base class / intrusive node
                if ft in TYPES and not ft.startswith("UFG::qReflectObjectType<") and not ft.startswith("UFG::qNode") and not ft.startswith("UFG::qTree"):
                    out.update(decode(ft, addr + fo, depth + 1) or {})
                elif ft.startswith("UFG::qReflectObjectType<"):
                    # CRTP base: "UFG::qReflectObjectType<T, Base>" -> decode Base fields
                    base = ft[ft.index(",") + 1:-1].strip()
                    if base != "UFG::qReflectObject" and base in TYPES:
                        out.update(decode(base, addr + fo, depth + 1) or {})
                continue
            if cnt > 1:
                isize = TYPES[ft][0] if ft in TYPES else struct.calcsize(PRIM.get(ft, "<I"))
                out[fn] = [decode(ft, addr + fo + k * isize, depth + 1) for k in range(cnt)]
            else:
                out[fn] = decode(ft, addr + fo, depth + 1)
        return out
    return "<?%s>" % tname


def obj(tname, addr):
    o = {"$uid": str(struct.unpack_from("<Q", d, addr + 8)[0]), "$type": tname}
    o.update(decode(tname, addr))
    return o


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "summary":
        print("chunk @%x  objects=%d types=%d fixups=%d typed=%d" % (CH, N_OBJ, N_TYPES, N_FIX, N_TYPED))
        for t, l in sorted(OBJECTS.items(), key=lambda kv: -len(kv[1])):
            print("%6d  %s" % (len(l), t))
    elif cmd == "show":
        mx = int(sys.argv[3]) if len(sys.argv) > 3 else 5
        for a in OBJECTS[sys.argv[2]][:mx]:
            print(json.dumps(obj(sys.argv[2], a), indent=1)[:6000])
    elif cmd == "export":
        out = {t: [obj(t, a) for a in l] for t, l in OBJECTS.items() if t}
        json.dump(out, open(sys.argv[2], "w"), indent=1)
        print("exported", sum(len(v) for v in out.values()), "objects of", len(out), "types")

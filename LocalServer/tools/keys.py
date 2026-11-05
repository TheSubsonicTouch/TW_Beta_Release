"""keys.py <callback-va|endpoint-substring> ... : typed list of JSON keys a response parser reads."""
import sys
from twre import *
import cb as _cb
CONV = {0x140094950: "int", 0x1401c3a20: "u64str", 0x1401c3f30: "i64str", 0x140094710: "float",
        0x140094550: "bool", 0x1401b3f30: "string", 0x140455d00: "i64", 0x140455d40: "int",
        0x140455cd0: "float", 0x140455c60: "bool", 0x140096170: "array", 0x140460f20: "<mDataBags>",
        0x1404844f0: "<reflect>", 0x1401aab30: "handle"}
def typed(va, depth=0, seen=None, out=None):
    seen = seen if seen is not None else set()
    out = out if out is not None else []
    f = func_of(va)
    if not f or f[0] in seen: return out
    seen.add(f[0])
    o = va2off(f[0]); pending = None
    for ins in md.disasm(data[o:o + (f[1] - f[0])], f[0]):
        note = annotate(ins)
        if '"' in note and ins.mnemonic == "lea":
            s = note.split('"')[1]
            if pending and pending[0] != s: out.append(("  " * depth + pending[0], pending[1] or "?"))
            if pending and pending[0] == s: continue
            pending = [s, None]
        elif ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
            t = int(ins.op_str, 16)
            if t in CONV and pending and pending[1] is None:
                pending[1] = CONV[t]
            elif ins.mnemonic == "call" and depth < 3:
                cf = func_of(t)
                if cf and 0x140450000 <= cf[0] < 0x140500000 and cf[0] not in seen and t not in (0x140484f30, 0x140455c90, 0x140451c60, 0x140453b80):
                    if pending: out.append(("  " * depth + pending[0], pending[1] or "object")); pending = None
                    typed(t, depth + 1, seen, out)
    if pending: out.append(("  " * depth + pending[0], pending[1] or "?"))
    return out
if __name__ == "__main__":
    for arg in sys.argv[1:]:
        if arg.startswith("0x"):
            vas = [(arg, int(arg, 16))]
        else:
            vas = []
            for va, s in find_strings(arg):
                for r in C["refs"].get(va, []):
                    f = func_of(r)
                    if f:
                        for c in _cb.callbacks(f[0]): vas.append((s, c))
        done = set()
        for name, va in vas:
            if (name, va) in done: continue
            done.add((name, va))
            ks = [(k, t) for k, t in typed(va) if not any(x in k for x in ("qArray", "qReflectArray", "::", "%"))]
            print("%s  [cb %x]: %s" % (name, va, ", ".join("%s:%s" % (k.strip(), t) for k, t in ks)))

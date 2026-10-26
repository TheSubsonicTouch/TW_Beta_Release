"""cb.py <endpoint-substring> ... : find the request function for an endpoint string, then summarise its response callback(s)."""
import sys
from twre import *

def callbacks(fva):
    f = func_of(fva)
    o = va2off(f[0]); out = []; last_r9 = None
    for ins in md.disasm(data[o:o + (f[1] - f[0])], f[0]):
        if ins.mnemonic == "lea" and ins.op_str.startswith("r9, [rip"):
            last_r9 = ins.address + ins.size + ins.operands[1].mem.disp
        elif ins.mnemonic == "call" and ins.op_str == "0x1402544a0" and last_r9:
            out.append(last_r9); last_r9 = None
    return out

def deep(va, depth, seen):
    """summary of va plus any directly-called helper that itself references strings (one level)."""
    f = func_of(va)
    if not f or f[0] in seen: return
    seen.add(f[0])
    print("  " * depth + "== %x-%x" % f)
    o = va2off(f[0]); calls = []
    for ins in md.disasm(data[o:o + (f[1] - f[0])], f[0]):
        note = annotate(ins)
        if '"' in note:
            print("  " * depth + "   %x %s" % (ins.address, note.strip(" ;")))
        elif ins.mnemonic == "call" and ins.op_str.startswith("0x"):
            calls.append(int(ins.op_str, 16))
    if depth < 2:
        for c in calls:
            cf = func_of(c)
            if cf and 0x140450000 <= cf[0] < 0x140500000 and cf[0] not in seen:
                deep(c, depth + 1, seen)

for arg in (sys.argv[1:] if __name__ == "__main__" else []):
    for va, s in find_strings(arg):
        for r in C["refs"].get(va, []):
            f = func_of(r)
            if not f: continue
            cbs = callbacks(f[0])
            print('#### "%s"  request fn %x  callbacks %s' % (s, f[0], ["%x" % c for c in cbs]))
            for c in cbs:
                deep(c, 0, set())

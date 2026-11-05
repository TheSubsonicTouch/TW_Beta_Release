"""Small reverse-engineering helpers for TriadWars.exe.

usage:
  re.py info
  re.py xref <string-substring | 0xVA> [...]      code locations referencing the string/VA
  re.py dis <0xVA> [count]                        disassemble from VA
  re.py func <0xVA>                               disassemble the whole function containing VA
  re.py calls <0xVA>                              callers of a function (direct call/jmp rel32)
"""
import sys, re, struct, bisect, pickle, os
import pefile, capstone

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.normpath(os.path.join(HERE, "..", "..", "TriadWars.exe"))
CACHE = os.path.join(HERE, "re_cache.pkl")

pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
data = pe.__data__
SECS = [(s.Name.rstrip(b"\0").decode(), BASE + s.VirtualAddress, s.Misc_VirtualSize,
         s.PointerToRawData, s.SizeOfRawData, s.Characteristics) for s in pe.sections]


def va2off(va):
    for n, v, vs, p, ps, c in SECS:
        if v <= va < v + max(vs, ps):
            o = va - v
            if o < ps:
                return p + o
    return None


def off2va(off):
    for n, v, vs, p, ps, c in SECS:
        if p <= off < p + ps:
            return v + off - p
    return None


def cstr(va, maxlen=200):
    o = va2off(va)
    if o is None:
        return None
    e = data.find(b"\0", o, o + maxlen)
    if e < 0:
        e = o + maxlen
    s = data[o:e]
    if len(s) >= 3 and all(32 <= b < 127 or b in (9, 10, 13) for b in s):
        return s.decode("ascii")
    return None


def load_cache():
    if os.path.exists(CACHE):
        with open(CACHE, "rb") as f:
            return pickle.load(f)
    c = {}
    # imports
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    imps = {}
    for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for i in e.imports:
            nm = i.name.decode() if i.name else "ord%d" % i.ordinal
            imps[i.address] = "%s!%s" % (e.dll.decode(), nm)
    c["imps"] = imps
    # functions from .pdata
    funcs = []
    d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
    o = pe.get_offset_from_rva(d.VirtualAddress)
    for i in range(0, d.Size, 12):
        b, e, u = struct.unpack_from("<III", data, o + i)
        if b:
            funcs.append((BASE + b, BASE + e))
    funcs.sort()
    c["funcs"] = funcs
    # rip-relative references: scan executable sections for modrm (mod=00, rm=101) after common opcodes
    refs = {}
    for n, v, vs, p, ps, ch in SECS:
        if not ch & 0x20000000:
            continue
        blob = data[p:p + ps]
        # lea/mov/cmp etc with rip-relative disp32: [REX] opcode modrm disp32
        for m in re.finditer(rb"[\x48\x4c\x49\x4d\x40-\x4f]?[\x8d\x8b\x89\x3b\x39\x03\x63\x85][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", blob, re.S):
            end = m.end() + 4
            if end > len(blob):
                continue
            disp = struct.unpack_from("<i", blob, m.end())[0]
            tgt = v + end + disp
            refs.setdefault(tgt, []).append(v + m.start())
        # call/jmp rel32
        for m in re.finditer(rb"[\xe8\xe9]", blob):
            end = m.end() + 4
            if end > len(blob):
                continue
            disp = struct.unpack_from("<i", blob, m.end())[0]
            tgt = v + end + disp
            if v <= tgt < v + ps:
                refs.setdefault(("c", tgt), []).append(v + m.start())
    c["refs"] = refs
    with open(CACHE, "wb") as f:
        pickle.dump(c, f)
    return c


C = load_cache()
FUNCS = C["funcs"]
FSTARTS = [f[0] for f in FUNCS]


def func_of(va):
    i = bisect.bisect_right(FSTARTS, va) - 1
    if i >= 0 and FUNCS[i][0] <= va < FUNCS[i][1]:
        return FUNCS[i]
    return None


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def annotate(ins):
    notes = []
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            t = ins.address + ins.size + op.mem.disp
            if t in C["imps"]:
                notes.append(C["imps"][t])
            else:
                s = cstr(t)
                if s is not None:
                    notes.append('"%s"' % s[:100])
                else:
                    notes.append("[%x]" % t)
        elif op.type == capstone.x86.X86_OP_IMM and ins.mnemonic in ("call", "jmp"):
            pass
    return ("   ; " + " ".join(notes)) if notes else ""


def dis(va, count=60, end=None):
    o = va2off(va)
    n = 0
    size = (end - va) if end else count * 15
    for ins in md.disasm(data[o:o + size], va):
        print("%x  %-7s %s%s" % (ins.address, ins.mnemonic, ins.op_str, annotate(ins)))
        n += 1
        if end is None and n >= count:
            break


def leaf(va, limit=400):
    """Disassemble a function with no unwind info: stop at the first ret/jmp followed by padding."""
    o = va2off(va)
    n = 0
    for ins in md.disasm(data[o:o + limit * 15], va):
        print("%x  %-7s %s%s" % (ins.address, ins.mnemonic, ins.op_str, annotate(ins)))
        n += 1
        nxt = va2off(ins.address + ins.size)
        if ins.mnemonic in ("ret", "jmp") and data[nxt] in (0xCC, 0x90):
            break
        if n >= limit:
            break


def summary(va):
    """Compact view of a function: only string references and call targets, in order."""
    f = func_of(va)
    if f is None:
        print("no unwind info for %x" % va)
        return
    print("== func %x-%x (%d bytes)" % (f[0], f[1], f[1] - f[0]))
    o = va2off(f[0])
    for ins in md.disasm(data[o:o + (f[1] - f[0])], f[0]):
        note = annotate(ins)
        if ins.mnemonic == "call":
            print("  %x  call %s%s" % (ins.address, ins.op_str, note))
        elif '"' in note:
            print("  %x  %s %s%s" % (ins.address, ins.mnemonic, ins.op_str.split(",")[0], note))


def find_strings(sub):
    out = []
    b = sub.encode()
    i = 0
    while True:
        i = data.find(b, i)
        if i < 0:
            break
        s = i
        while s > 0 and 32 <= data[s - 1] < 127:
            s -= 1
        va = off2va(s)
        if va:
            out.append((va, cstr(va) or sub))
        i += 1
    return sorted(set(out))


def xref(arg):
    if arg.startswith("0x"):
        targets = [(int(arg, 16), arg)]
    else:
        targets = find_strings(arg)
    for va, s in targets:
        # a ref may point into the middle of the string when substring; check start only
        rs = C["refs"].get(va, [])
        print('%x "%s" -> %d refs' % (va, s[:80], len(rs)))
        for r in rs:
            f = func_of(r)
            print("    %x  (func %s)" % (r, "%x-%x" % f if f else "?"))


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "info":
        print("base %x" % BASE)
        for s in SECS:
            print("%-8s va=%x vsize=%x raw=%x rsize=%x ch=%x" % s)
        print("funcs", len(FUNCS), "imports", len(C["imps"]), "ref targets", len(C["refs"]))
        dl = {}
        for a, n in C["imps"].items():
            dl.setdefault(n.split("!")[0], []).append(n.split("!")[1])
        for k, v in dl.items():
            print(k, len(v), ", ".join(sorted(v))[:1500])
    elif cmd == "xref":
        for a in sys.argv[2:]:
            xref(a)
    elif cmd == "dis":
        dis(int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 60)
    elif cmd == "func":
        f = func_of(int(sys.argv[2], 16))
        if f is None:
            leaf(int(sys.argv[2], 16))
        else:
            print("func %x-%x" % f)
            dis(f[0], end=f[1])
    elif cmd == "sum":
        for a in sys.argv[2:]:
            summary(int(a, 16))
    elif cmd == "calls":
        t = int(sys.argv[2], 16)
        for r in C["refs"].get(("c", t), []):
            f = func_of(r)
            print("%x  (func %s)" % (r, "%x-%x" % f if f else "?"))

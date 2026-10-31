"""vtable.py <function VA> : find the vtable(s) that hold a function and list the other methods
with the strings each one references. Usually enough to name the class a crash happened in."""
import struct
import sys

from twre import *

TEXT_LO, TEXT_HI = SECS[0][1], SECS[0][1] + SECS[0][2]


def strings_of(fva):
    f = func_of(fva)
    if not f:
        return []
    out = []
    o = va2off(f[0])
    for ins in md.disasm(data[o:o + (f[1] - f[0])], f[0]):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                s = cstr(ins.address + ins.size + op.mem.disp)
                if s and s not in out:
                    out.append(s)
    return out


def is_method(v):
    return TEXT_LO <= v < TEXT_HI and func_of(v) is not None and func_of(v)[0] == v


if __name__ == "__main__":
    target = int(sys.argv[1], 16)
    needle = struct.pack("<Q", target)
    i = 0
    while True:
        i = data.find(needle, i)
        if i < 0:
            break
        if off2va(i):
            start = i
            while is_method(struct.unpack_from("<Q", data, start - 8)[0]):
                start -= 8
            print("vtable at %x (function is slot %d)" % (off2va(start), (i - start) // 8))
            for n in range(60):
                v = struct.unpack_from("<Q", data, start + 8 * n)[0]
                if not TEXT_LO <= v < TEXT_HI:
                    break
                print("  [%2d] %x %s" % (n, v, "; ".join(strings_of(v)[:14])[:400]))
        i += 1

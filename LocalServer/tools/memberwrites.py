"""memberwrites.py <member offset> <from VA> <to VA> [context lines] : list the instructions
in a code range that store to [reg + offset], with the instructions leading up to each.
Answers "who sets this field?" once a crash has pointed at a null member."""
import re
import struct
import sys

from twre import *

if __name__ == "__main__":
    member = int(sys.argv[1], 16)
    lo, hi = va2off(int(sys.argv[2], 16)), va2off(int(sys.argv[3], 16))
    context = int(sys.argv[4]) if len(sys.argv) > 4 else 14
    seen = set()
    for m in re.finditer(re.escape(struct.pack("<I", member)), data[lo:hi]):
        f = func_of(off2va(lo + m.start()))
        if not f or f in seen:
            continue
        seen.add(f)
        o = va2off(f[0])
        ins_list = list(md.disasm(data[o:o + (f[1] - f[0])], f[0]))
        for k, ins in enumerate(ins_list):
            if ins.mnemonic == "mov" and ("+ 0x%x]" % member) in ins.op_str.split(",")[0]:
                print("---- func %x-%x" % f)
                for j in ins_list[max(0, k - context):k + 3]:
                    print("  %x  %-6s %s%s" % (j.address, j.mnemonic, j.op_str, annotate(j)))

import sys, glob, os, struct
from minidump.minidumpfile import MinidumpFile
from twre import *
path = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(os.path.dirname(EXE), "crash_reports", "*.dmp")), key=os.path.getmtime)
print(path)
mf = MinidumpFile.parse(path)
mods = [(m.baseaddress, m.baseaddress + m.size, os.path.basename(m.name)) for m in mf.modules.modules]
def modof(a):
    for b, e, n in mods:
        if b <= a < e: return n, b
    return None, 0
exe_base = [b for b, e, n in mods if n.lower() == "triadwars.exe"][0]
slide = exe_base - BASE
print("exe base %x slide %x" % (exe_base, slide))
exc = mf.exception.exception_records[0]
er = exc.ExceptionRecord
print("exception code %s addr %x thread %x" % (er.ExceptionCode, er.ExceptionAddress, exc.ThreadId))
print("params", [hex(p) for p in er.ExceptionInformation[:er.NumberParameters]])
reader = mf.get_reader()
# context of faulting thread
ctx = None
for t in mf.threads.threads:
    if t.ThreadId == exc.ThreadId:
        th = t
import io
buf = open(path, "rb").read()
cl = exc.ThreadContext
c = buf[cl.Rva:cl.Rva + cl.DataSize]
names = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15","rip"]
regs = dict(zip(names, struct.unpack_from("<17Q", c, 0x78)))
print(" ".join("%s=%x" % (n, regs[n]) for n in names))
def desc(a):
    n, b = modof(a)
    if n is None: return None
    if n.lower() == "triadwars.exe":
        va = a - slide
        f = func_of(va)
        return "TriadWars.exe!%x (func %s)" % (va, "%x" % f[0] if f else "?")
    return "%s+%x" % (n, a - b)
print("RIP:", desc(regs["rip"]))
# raw stack scan for return addresses
sp = regs["rsp"]
try:
    stack = reader.read(sp, 0x1800)
except Exception as e:
    stack = reader.read(sp, 0x400)
seen = 0
for i in range(0, len(stack) - 8, 8):
    v = struct.unpack_from("<Q", stack, i)[0]
    d = desc(v)
    if d and "TriadWars.exe" in d:
        # plausible return address: preceded by a call
        va = v - slide
        o = va2off(va)
        if o and (data[o-5] == 0xE8 or data[o-6:o-4] == b"\xff\x15" or data[o-2] == 0xff or data[o-3] == 0xff):
            print("  [rsp+%04x] %s" % (i, d)); seen += 1
            if seen > 40: break

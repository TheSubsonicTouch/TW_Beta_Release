"""Parser for Reflection.rdb: names are CRC-64/Jones (reflected, init ~0) hashes into the string table."""
import os, struct, sys
PATH=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Reflection.rdb"))
d=open(PATH,"rb").read()
strend=0x10+struct.unpack_from("<I",d,4)[0]
strs=[s.decode("latin1") for s in d[0x10:strend].split(b"\0") if s]
M=0xFFFFFFFFFFFFFFFF
T=[]
for i in range(256):
    c=i
    for _ in range(8): c=(c>>1)^0x95AC9329AC4BC9B5 if c&1 else c>>1
    T.append(c)
def h64(s):
    if isinstance(s,str): s=s.encode("latin1")
    c=M
    for b in s: c=T[(c^b)&0xFF]^(c>>8)
    return c
NAMES={h64(s):s for s in strs}
def records():
    off=strend
    while off+16<=len(d):
        tag,size,size2,z=struct.unpack_from("<IIII",d,off)
        yield off,tag,d[off+16:off+16+size]
        off+=16+size
if __name__=="__main__":
    recs=list(records())
    import collections
    print(len(recs),"records; tags:",collections.Counter("%08x"%t for _,t,_ in recs).most_common(8))
    n=int(sys.argv[1]) if len(sys.argv)>1 else 3
    for off,tag,p in recs[:n]:
        print("--- record @%x tag %08x size %x"%(off,tag,len(p)))
        for i in range(0,len(p),8):
            v=struct.unpack_from("<Q",p,i)[0]
            print("   +%03x %016x %s"%(i,v,NAMES.get(v,"")))

import struct, sys, collections
from rdb import *
TYPES={}; OTHER=collections.defaultdict(list)
for off,tag,p in records():
    if tag==0xe57f8e15:
        name=NAMES.get(struct.unpack_from("<Q",p,8)[0])
        size=struct.unpack_from("<Q",p,0x10)[0]
        fields=[]
        for i in range(0x28,len(p),0x28):
            fn,ft,fo,cnt,fl=struct.unpack_from("<5Q",p,i)
            fields.append((NAMES.get(fn,"#%x"%fn),NAMES.get(ft,"#%x"%ft),fo,cnt,fl))
        TYPES[name]=(size,fields,NAMES.get(struct.unpack_from("<Q",p,0)[0],""))
    else:
        OTHER[tag].append((off,p))
def show(name,depth=0,seen=None):
    seen=seen or set()
    if name not in TYPES: print("  "*depth+"?? "+str(name)); return
    size,fields,src=TYPES[name]
    print("  "*depth+"%s  (size 0x%x)  [%s]"%(name,size,src))
    for fn,ft,fo,cnt,fl in fields:
        print("  "*depth+"   +%03x %-34s %s%s"%(fo,fn,ft,"" if cnt==1 else " x%d"%cnt))
if __name__=="__main__":
    if sys.argv[1]=="list":
        pat=sys.argv[2].lower() if len(sys.argv)>2 else ""
        for n in sorted(k for k in TYPES if k and pat in k.lower()): print(n, hex(TYPES[n][0]), len(TYPES[n][1]))
    elif sys.argv[1]=="other":
        for tag,l in OTHER.items():
            print("tag %08x: %d records"%(tag,len(l)))
            for off,p in l[:2]:
                for i in range(0,min(len(p),0x70),8):
                    v=struct.unpack_from("<Q",p,i)[0]; print("   +%03x %016x %s"%(i,v,NAMES.get(v,"")))
                print("   ...")
    else:
        for n in sys.argv[1:]: show(n)

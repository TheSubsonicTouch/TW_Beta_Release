"""qSymbol hashes: CRC-32 (poly 04C11DB7, MSB-first, init ~0, no final xor); the UC variant upper-cases a-z."""
T=[]
for i in range(256):
    c=i<<24
    for _ in range(8): c=((c<<1)^0x04C11DB7)&0xFFFFFFFF if c&0x80000000 else (c<<1)&0xFFFFFFFF
    T.append(c)
def sym(s,upper=False):
    if isinstance(s,str): s=s.encode("latin1")
    if upper: s=s.upper()
    h=0xFFFFFFFF
    for b in s: h=T[((h>>24)^b)&0xFF]^((h<<8)&0xFFFFFFFF)
    return h
if __name__=="__main__":
    import re,sys
    targets={0xa386b349:1,0xaec59590:2,0xaa048827:3,0xb443d822:4,0xb082c595:5,0xbdc1e34c:6,0xb900fefb:7}
    words=set()
    for line in open("strings_ascii.txt",encoding="utf-8"):
        s=line.rstrip("\n")[9:]
        if len(s)<=40: words.add(s)
        for w in re.findall(r"[A-Za-z_][A-Za-z_0-9]{2,30}",s): words.add(w)
    found=[]
    for w in words:
        for cand in (w, w.rstrip("0123456789")):
            for sep in ("","_","0","_0"," "):
                for up in (False,True):
                    h=sym(cand+sep+"1",up)
                    if h in targets and targets[h]==1: found.append((cand+sep+"1",up))
                h=sym(cand,False)
                if h in targets: found.append((cand,False,targets[h]))
                h=sym(cand,True)
                if h in targets: found.append((cand,True,targets[h]))
    print(len(words),"candidate words; found:",sorted(set(found)))

"""Decompress QCMP chunks from Triad Wars .big archives (algorithm lifted from TriadWars.exe @1401ba7a0)."""
import struct, sys, os, re

MAGIC = b"PMCQ\x01\x00\x01\x00"

def decompress(buf, off=0):
    data_off = struct.unpack_from("<I", buf, off + 8)[0]
    comp, uncomp = struct.unpack_from("<QQ", buf, off + 0x10)
    i = off + data_off
    end = off + comp
    out = bytearray()
    hist = [0] * 32
    hidx = 0
    while i < end:
        b = buf[i]; i += 1
        if b < 0x20:
            n = b + 1
            out += buf[i:i + n]; i += n
            continue
        l3 = b >> 5
        if l3 == 1:
            e = hist[b & 0x1F]
            offset = e & 0xFFFF; length = e >> 16
        else:
            offset = ((b & 0x1F) << 8) | buf[i]; i += 1
            if l3 == 7:
                length = buf[i]; i += 1
            else:
                length = l3
            length += 1
            hist[hidx] = (length << 16) | offset
            hidx = (hidx + 1) & 31
        start = len(out) - offset
        if offset >= length:
            out += out[start:start + length]
        else:
            for k in range(length):
                out.append(out[start + k])
    return bytes(out), comp, uncomp

def chunks(path):
    buf = open(path, "rb").read()
    for m in re.finditer(re.escape(MAGIC), buf):
        yield m.start(), buf

if __name__ == "__main__":
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for path in sys.argv[2:]:
        base = os.path.splitext(os.path.basename(path))[0]
        n = bad = total = 0
        for off, buf in chunks(path):
            try:
                out, comp, uncomp = decompress(buf, off)
            except Exception as e:
                bad += 1; continue
            if len(out) != uncomp:
                bad += 1
            n += 1; total += len(out)
            open(os.path.join(outdir, "%s_%08x.bin" % (base, off)), "wb").write(out)
        print("%s: %d chunks, %d size mismatches, %d bytes out" % (base, n, bad, total))

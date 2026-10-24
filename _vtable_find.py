import pefile, struct, sys

PATH = r"C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game"
pe = pefile.PE(PATH, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(PATH, "rb").read()

secs = []
for s in pe.sections:
    secs.append((s.Name.rstrip(b"\x00").decode(), s.VirtualAddress, s.PointerToRawData,
                 max(s.Misc_VirtualSize, s.SizeOfRawData)))

def va_to_off(va):
    rva = va - base
    for name, srva, praw, size in secs:
        if srva <= rva < srva + size:
            return praw + (rva - srva)
    return None

def off_to_va(off):
    for name, srva, praw, size in secs:
        if praw <= off < praw + size:
            return base + srva + (off - praw)
    return None

rdata = None
for name, srva, praw, size in secs:
    if name == ".rdata":
        rdata = (srva, praw, size)
rva, praw, size = rdata
blob = data[praw:praw + size]

targets = [int(x, 16) for x in sys.argv[1:]] or [0x71B9A0]
for t in targets:
    pat = struct.pack("<I", t)
    hits = []
    i = blob.find(pat)
    while i != -1:
        hits.append(praw + i)
        i = blob.find(pat, i + 4)
    print("== target %08X vtables/xrefs in .rdata: %d" % (t, len(hits)))
    for off in hits:
        vtable_va = None
        # walk back to the start of the table (previous entry not a code pointer)
        start = off
        while start - 4 >= praw:
            prev = struct.unpack("<I", data[start - 4:start])[0]
            p = va_to_off(prev)
            if p is None or not (prev & 0x400000 == 0x400000 and prev < base + 0x900000):
                break
            start -= 4
        start_va = off_to_va(start)
        print("   entry at %08X  table starts %08X  entry#%d" %
              (off_to_va(off), start_va, (off - start) // 4))
        # print first 8 entries
        ents = []
        for k in range(0, 40):
            if start + k * 4 + 4 > praw + size:
                break
            v = struct.unpack("<I", data[start + k * 4:start + k * 4 + 4])[0]
            ents.append(v)
            if k >= 11:
                break
        print("   ", " ".join("%08X" % e for e in ents))

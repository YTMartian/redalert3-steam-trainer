import pefile, struct, sys

PATH = r"C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game"
pe = pefile.PE(PATH, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(PATH, "rb").read()

secs = []
for s in pe.sections:
    secs.append((s.Name.rstrip(b"\x00").decode(), s.VirtualAddress, s.PointerToRawData,
                 max(s.Misc_VirtualSize, s.SizeOfRawData)))

def off_to_va(off):
    for name, srva, praw, size in secs:
        if praw <= off < praw + size:
            return base + srva + (off - praw)
    return None

def va_to_off(va):
    rva = va - base
    for name, srva, praw, size in secs:
        if srva <= rva < srva + size:
            return praw + (rva - srva)
    return None

rva, praw, size = None, None, None
for name, srva, p, sz in secs:
    if name == ".rdata":
        rva, praw, size = srva, p, sz
blob = data[praw:praw + size]

needle = sys.argv[1].encode() if len(sys.argv) > 1 else b"NAMED_TRANSFER_OWNERSHIP_PLAYER"
pos = data.find(needle)
va = off_to_va(pos)
print("string fileoff %X va %08X" % (pos, va))

pat = struct.pack("<I", va)
hits = []
i = blob.find(pat)
while i != -1:
    hits.append(praw + i)
    i = blob.find(pat, i + 4)
print("refs in .rdata:", len(hits))
for off in hits:
    print("  at fileoff %X (va %08X)" % (off, off_to_va(off)))
    # dump 8 dwords before/after
    vals = []
    for k in range(-6, 7):
        o = off + k * 4
        vals.append((k, struct.unpack("<I", data[o:o + 4])[0]))
    for k, v in vals:
        print("    %+3d: %08X" % (k, v))

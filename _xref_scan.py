import struct, sys, pefile

path = r"C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game"
pe = pefile.PE(path, fast_load=True)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(path, "rb").read()

def va_to_off(va):
    rva = va - imgbase
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress), s.Name.rstrip(b"\x00").decode()
    return None, None

def find_dword(val, limit=40):
    pat = struct.pack("<I", val)
    hits = []
    i = data.find(pat)
    while i >= 0 and len(hits) < limit:
        hits.append(i)
        i = data.find(pat, i + 1)
    return hits

for v in [0xBF32E4, 0x8D7570, 0x8D74B0, 0xBEF72C, 0xBF3130]:
    print("==== value %08X" % v)
    for off in find_dword(v):
        # convert file offset to VA
        va = None
        for s in pe.sections:
            if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
                va = imgbase + s.VirtualAddress + (off - s.PointerToRawData)
                sec = s.Name.rstrip(b"\x00").decode()
                break
        print("  fileoff %08X va %s sec %s" % (off, hex(va) if va else "?", sec if va else "?"))

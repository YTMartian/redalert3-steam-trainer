import struct, pefile
path = r"C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game"
pe = pefile.PE(path, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
data = open(path, 'rb').read()

def va_to_off(va):
    rva = va - base
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress), s.Name.rstrip(b"\0").decode()
    return None, None

for vt in (0xBF32E4, 0xBEF72C, 0xBF3130):
    off, sec = va_to_off(vt)
    print("vtable %08X off=%s sec=%s" % (vt, hex(off) if off else None, sec))
    if off is None: continue
    vals = struct.unpack_from("<16I", data, off)
    print("   ", " ".join("%08X" % v for v in vals))
    for i, v in enumerate(vals):
        if base <= v < base + 0x900000:
            print("    [%02X] -> RVA %X" % (i, v - base))

import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

path = r"C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game"
pe = pefile.PE(path, fast_load=True)
imgbase = pe.OPTIONAL_HEADER.ImageBase
print("imagebase", hex(imgbase))

def va_to_off(va):
    rva = va - imgbase
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress), s.Name.rstrip(b"\x00").decode()
    return None, None

targets = [int(x, 16) for x in sys.argv[1:]] or [0x601643]
md = Cs(CS_ARCH_X86, CS_MODE_32)
data = open(path, "rb").read()

for rva in targets:
    va = imgbase + rva
    off, sec = va_to_off(va)
    print("=" * 70)
    print("RVA %08X  VA %08X  fileoff %s sec %s" % (rva, va, hex(off) if off else None, sec))
    start = max(0, off - 0x60)
    code = data[start:off + 0x40]
    for ins in md.disasm(code, va - 0x60):
        mark = ">>" if ins.address == va else "  "
        print("%s %08X  %-24s %s %s" % (mark, ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))

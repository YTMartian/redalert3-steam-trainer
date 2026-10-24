import pefile, sys
from capstone import *

PATH = r"C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game"
pe = pefile.PE(PATH, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(PATH, "rb").read()

def va_to_off(va):
    rva = va - base
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

start = int(sys.argv[1], 16)
length = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x200
off = va_to_off(start)
code = data[off:off + length]
md = Cs(CS_ARCH_X86, CS_MODE_32)
for i in md.disasm(code, start):
    print("%08X  %-22s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))

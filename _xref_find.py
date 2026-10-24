import pefile, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

PATH = r"C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game"
pe = pefile.PE(PATH, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(PATH, "rb").read()

text = None
for s in pe.sections:
    if s.Name.rstrip(b"\x00") == b".text":
        text = (s.VirtualAddress, s.PointerToRawData, s.Misc_VirtualSize)
rva, praw, size = text
code = data[praw:praw + size]

targets = [int(x, 16) for x in sys.argv[1:]] or [0x71B9A0]
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.skipdata = True
for t in targets:
    print("== xrefs to %08X" % t)
    found = []
    for ins in md.disasm(code, base + rva):
        if ins.mnemonic in ("call", "jmp") and ins.op_str == ("0x%x" % t):
            found.append(ins.address)
    for a in found:
        print("   %08X" % a)
    print("   total", len(found))

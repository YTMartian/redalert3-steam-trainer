import pefile, sys
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
code_va = base + rva

target = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x418
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.skipdata = True
hits = []
for ins in md.disasm(code, code_va):
    ops = ins.op_str
    if ("+ 0x%x]" % target) in ops or ("+ 0x%X]" % target) in ops:
        hits.append(ins)
print("hits", len(hits))
for ins in hits[:200]:
    print("%08X  %-20s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))

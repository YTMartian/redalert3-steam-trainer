import pefile, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

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

md = Cs(CS_ARCH_X86, CS_MODE_32)
pat = b"\xa1\x2c\xde\xce\x00"
idx = []
i = data.find(pat)
while i != -1:
    idx.append(i)
    i = data.find(pat, i + 4)
print("mov eax,[0xcede2c] count", len(idx))
shown = 0
for off in idx:
    # look for +0x28 / +0x30 accesses nearby
    window = data[off:off + 0x60]
    if b"\x28" not in window and b"\x30" not in window:
        continue
    va = off_to_va(off)
    if va is None:
        continue
    print("=" * 70)
    for ins in md.disasm(data[off:off + 0x80], va):
        print("%08X  %-18s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
        if ins.address > va + 0x50:
            break
    shown += 1
    if shown >= 6:
        break

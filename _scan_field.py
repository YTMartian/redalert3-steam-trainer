import pefile, re, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

PATH = r"C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game"
pe = pefile.PE(PATH, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(PATH, "rb").read()

secs = []
for s in pe.sections:
    name = s.Name.rstrip(b"\x00").decode()
    secs.append((name, s.VirtualAddress, s.PointerToRawData,
                 max(s.Misc_VirtualSize, s.SizeOfRawData)))

def va_of_off(off):
    for name, rva, praw, size in secs:
        if praw <= off < praw + size:
            return base + rva + (off - praw), name
    return None, None

def off_of_va(va):
    rva = va - base
    for name, srva, praw, size in secs:
        if srva <= rva < srva + size:
            return praw + (rva - srva), name
    return None, None

target_off = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x418

# find the .text section
text = None
for name, rva, praw, size in secs:
    if name == ".text":
        text = (rva, praw, size)
rva, praw, size = text
code = data[praw:praw + size]
code_va = base + rva

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = False

def disp_bytes(off):
    # little endian 4-byte displacement
    return bytes([off & 0xFF, (off >> 8) & 0xFF, (off >> 16) & 0xFF, (off >> 24) & 0xFF])

d = disp_bytes(target_off)
hits = []
for i in range(len(code) - 6):
    b = code[i]
    if b not in (0x8B, 0x89, 0x8D):  # mov r,[r+disp] / mov [r+disp],r / lea
        continue
    if code[i + 2:i + 6] == d:
        modrm = code[i + 1]
        mod = modrm >> 6
        rm = modrm & 7
        if mod == 2 and rm in (0, 1, 2, 3, 5, 6, 7):
            hits.append(i)

print("hits", len(hits))
for i in hits:
    va, _ = va_of_off(praw + i)
    start = max(0, i - 0x30)
    print("=" * 70)
    for ins in md.disasm(code[start:i + 0x40], code_va + start):
        mark = ">>" if ins.address == va else "  "
        print("%s %08X  %-20s %s %s" % (mark, ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))

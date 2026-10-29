"""Disassemble the RayaTrainer patch-set builder.

The builder at 0x1005A340 records, for each frame-rate unlock patch site, an
RVA plus two byte buffers.  Which buffer is the patched one, and what exact
offset it lands on, could not be settled from decompiler output -- so read the
actual instructions here.
"""
import sys

import capstone
import pefile

DLL = r"C:\Users\YTMartian\Desktop\RayaTrainer-v0.9.0-win-x86-self-contained\RayaTrainer.Agent.dll"


def load(path):
    pe = pefile.PE(path, fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    sections = []
    for s in pe.sections:
        name = s.Name.rstrip(b"\x00").decode("latin1")
        sections.append((name, s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData,
                         s.SizeOfRawData))
    return pe, image_base, sections


def rva_bytes(pe, sections, start_rva, size):
    for name, va, vsize, raw, rawsize in sections:
        if va <= start_rva < va + max(vsize, rawsize):
            off = start_rva - va
            avail = max(0, rawsize - off)
            data = pe.__data__[raw + off: raw + off + min(size, avail)]
            return data, name
    return b"", "?"


def disasm(pe, sections, start_rva, count, base_va=0x10000000):
    code, sec = rva_bytes(pe, sections, start_rva, count * 8)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False
    va = base_va + start_rva
    out = []
    for insn in md.disasm(code, va):
        out.append(insn)
        if len(out) >= count:
            break
    return out, sec


def main():
    start = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x5A340
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 220
    pe, image_base, sections = load(DLL)
    print(f"image_base={image_base:#x}")
    insns, sec = disasm(pe, sections, start, count)
    print(f"disassembling 0x10000000+{start:x} in section {sec}\n")
    for i in insns:
        print(f"0x{i.address:08X}  {i.mnemonic:8s} {i.op_str}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

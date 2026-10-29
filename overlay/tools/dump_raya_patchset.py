"""Recover RayaTrainer's complete frame-rate patchset from its own code.

The eight .data constants are reproduced and still do not unlock 60fps, so the
patchset we transcribed is incomplete. Rather than guess, disassemble the
constructor that builds it and read every (RVA, original bytes, patched bytes)
triple straight out of the code -- including any .text sites.

The header comment names the builder as RVA 0x5A340 (VA 0x1005A340), which
matches a base of 0x10000000. We decode instructions and pull out:
  * immediates that look like RVAs into the game image
  * byte runs loaded via immediate stores / rep movs

Output is a readable listing of the region plus any candidate triples found.
"""
from __future__ import annotations

import struct
import sys

DLL = r"C:\Users\YTMartian\Desktop\RayaTrainer-v0.9.0-win-x86-self-contained\RayaTrainer.Agent.dll"


def main() -> int:
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32

    pe = pefile.PE(DLL, fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    data = open(DLL, "rb").read()

    # Map RVA <-> file offset
    sections = []
    for s in pe.sections:
        name = s.Name.rstrip(b"\x00").decode("latin1")
        sections.append((s.VirtualAddress, s.Misc_VirtualSize,
                         s.SizeOfRawData, s.PointerToRawData, name))

    def rva_to_off(rva: int) -> int | None:
        for va, vs, rs, pr in ((a, b, c, d) for a, b, c, d, _n in sections):
            if va <= rva < va + max(vs, rs):
                return pr + (rva - va)
        return None

    def off_to_rva(off: int) -> int | None:
        for va, vs, rs, pr in ((a, b, c, d) for a, b, c, d, _n in sections):
            if pr <= off < pr + rs:
                return va + (off - pr)
        return None

    print(f"image base {image_base:#x}")
    print("sections:")
    for va, vs, rs, pr, name in sections:
        print(f"  {name:<8} rva={va:#08x} vsize={vs:#08x} raw={rs:#08x} off={pr:#08x}")

    target_rva = 0x5A340
    off = rva_to_off(target_rva)
    if off is None:
        print("builder RVA not found in any section")
        return 1

    # Disassemble a generous window around the builder
    start = off
    window = data[start:start + 0x1200]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    print()
    print(f"== disassembly from RVA {target_rva:#x} (VA {image_base + target_rva:#x}) ==")
    rvas_seen: list[int] = []
    for ins in md.disasm(window, image_base + target_rva):
        text = f"{ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}"
        print(text)
        # Collect immediate operands that look like game-image RVAs
        for op in ins.operands:
            if op.type == 2:  # IMM
                v = op.imm & 0xFFFFFFFF
                if 0x1000 <= v <= 0x950000:
                    rvas_seen.append(v)
        if ins.mnemonic == "ret" and ins.address > image_base + target_rva:
            break

    print()
    print("== candidate game RVAs referenced as immediates ==")
    for v in sorted(set(rvas_seen)):
        print(f"  {v:#08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

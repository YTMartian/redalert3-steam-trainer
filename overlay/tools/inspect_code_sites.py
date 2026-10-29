"""Locate the game binary and report what lives at RayaTrainer's code sites.

Two .text patches were recovered statically:
  0x2FFDAC  an absolute address 0x00CE0DFC -> 0x00BC644C
  0x31F457  movss xmm1,[edx+0xF0] -> jmp 0x00BCA420

0x00BCA420 is base+0x7CA420, which is inside the image but past .text. Knowing
which section it sits in decides the approach: if it is a spare data region,
RayaTrainer is using it as a code cave and we would have to reproduce its
trampoline; if it is ordinary code, the "patch" just calls existing engine code.

Prints the section table, the file bytes at both patch sites, and whether the
jump target looks like real code, a cave, or all zeros on disk.
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p


def main() -> int:
    import pefile

    path = p.find_game_file()
    if not path:
        print("could not find ra3_1.12.game")
        return 1
    print(f"game: {path}")

    pe = pefile.PE(path, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    print(f"image base {base:#x}")

    secs = []
    print()
    print(f"{'section':<10}{'vstart':>10}{'vsize':>10}{'rawsize':>10}{'fileoff':>10}  flags")
    for s in pe.sections:
        name = s.Name.rstrip(b"\x00").decode("latin1")
        ch = s.Characteristics
        flags = []
        if ch & 0x20000000:
            flags.append("X")
        if ch & 0x80000000:
            flags.append("W")
        if ch & 0x40000000:
            flags.append("R")
        secs.append((s.VirtualAddress, s.Misc_VirtualSize,
                     s.SizeOfRawData, s.PointerToRawData, name, ch))
        print(f"{name:<10}{s.VirtualAddress:#010x}{s.Misc_VirtualSize:#010x}"
              f"{s.SizeOfRawData:#010x}{s.PointerToRawData:#010x}  {''.join(flags)}")

    def read_rva(rva: int, n: int) -> bytes | None:
        for va, vs, rs, pr, _name, _ch in secs:
            if va <= rva < va + max(vs, rs):
                off = pr + (rva - va)
                if off + n > pr + rs:
                    return None
                data = open(path, "rb").read()
                return data[off:off + n]
        return None

    def which(rva: int) -> str:
        for va, vs, rs, pr, name, ch in secs:
            if va <= rva < va + max(vs, rs):
                ex = "exec" if ch & 0x20000000 else "NO-exec"
                return f"{name} ({ex})"
        return "outside image"

    print()
    print("== the two patch sites, as they exist on disk (pre-patch) ==")
    for rva, note in ((0x2FFDAC, "absolute address operand"),
                      (0x31F457, "movss load")):
        raw = read_rva(rva, 8)
        print(f"  {rva:#08x}  {which(rva):<22} {raw.hex(' ') if raw else 'unreadable'}"
              f"   ({note})")

    print()
    print("== where the patches point ==")
    for rva, why in ((0x7C644C, "pointer target (cave copy +4)"),
                     (0x7CA420, "jmp target")):
        raw = read_rva(rva, 32)
        nonzero = sum(1 for b in (raw or b"") if b)
        print(f"  {rva:#08x}  {which(rva):<22} "
              f"nonzero={nonzero}/32")
        if raw:
            print(f"      {raw.hex(' ')}")

    print()
    print("== sanity: original bytes at 0x31F457 really are movss ==")
    raw = read_rva(0x31F457, 8)
    if raw:
        print(f"  {raw.hex(' ')}")
        print("  f3 0f 10 8a <disp32>  = movss xmm1, dword ptr [edx+disp]")
        print(f"  disp = {struct.unpack('<i', raw[4:8])[0]:#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Find who reads each timing constant, to tell logic time from animation time.

The 60fps patch doubles eight .data constants. Measured behaviour after the
half-time fix on the tick is: battle clock 1.00x, but unit motion, building
animation and water still run at 2x. So the two are driven by different values --
halving the tick's dt only covered the simulation timeline.

To find the animation time source, each constant is located in .text as an
absolute memory operand and the reads are disassembled. Constants are printed with
their original and patched float values so the doubled/unchanged families stay
distinguishable, and reader counts are ranked: a value read from the render path
is the animation scale, one read only around the tick is simulation time.

Usage:
    python xref_constants.py
"""
from __future__ import annotations

import struct
import sys

import static_sites as S

# rva -> label, matching fps_unlock.cpp
CONSTANTS = {
    0x8B4098: "logicFPS (int 15 -> 30)",
    0x8B409C: "updateFPS (int 30 -> 60 via redirect)",
    0x8E0DEC: "0.015 -> 0.03",
    0x8E0DBC: "66.667 -> 33.333",
    0x8E0DF8: "15.0 -> 30.0",
    0x8E0DF0: "30.0 -> 60.0",
    0x8E0DF4: "33.333 -> 16.667",
    0x8E0E34: "0.0667 -> 0.0333",
    0x8E0ED4: "0.0333 -> 0.01667",
    0x8E0DFC: "0.03 (frame budget)",
}


def main() -> int:
    pe = S.PE(S.BINARY)
    base = pe.base
    text_rva = next(va for name, va, *_ in pe.sections if name == ".text")
    text_size = next(v for name, va, v, *_ in pe.sections if name == ".text")
    blob = pe.read(text_rva, text_size)

    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    for rva, label in CONSTANTS.items():
        va = base + rva
        # current on-disk value tells the ORIGINAL float, which identifies the
        # semantic (1/15, 1/30, 15, 30, 1000/15 ...) regardless of the patch.
        raw = pe.read(rva, 4)
        as_int = struct.unpack("<i", raw)[0] if raw else 0
        as_float = struct.unpack("<f", raw)[0] if raw else 0.0
        print(f"\n=== 0x{rva:X}  {label}")
        print(f"    on disk: int={as_int}  float={as_float:.6g}  "
              f"bytes={raw.hex(' ')}")

        needle = struct.pack("<I", va)
        hits = []
        start = 0
        while True:
            i = blob.find(needle, start)
            if i < 0:
                break
            hits.append(text_rva + i)
            start = i + 1
        if not hits:
            print("    no code references")
            continue
        print(f"    referenced from {len(hits)} place(s)")

        for h_rva in hits[:8]:
            # Walk back to an instruction boundary so the read shows in context.
            s = max(0, h_rva - text_rva - 0x14)
            print(f"    --- ref at {base + h_rva:#010x}")
            for ins in md.disasm(blob[s:h_rva - text_rva + 0x18],
                                 base + text_rva + s):
                if ins.address > base + h_rva + 0x0C:
                    break
                mark = "  <<< reads the constant" \
                    if ins.address <= base + h_rva < ins.address + ins.size else ""
                print(f"        {ins.address:#010x}  {ins.mnemonic:<7}"
                      f"{ins.op_str}{mark}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

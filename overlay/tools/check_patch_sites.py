"""Resolve the 60fps patch sites against the on-disk ra3_1.12.game.

The .data section of this build is packed (virtual size != raw size and RVA is
not the file offset), so a site's bytes must be located through the section
table.  This prints, for every site, what the file actually contains next to
what fps_unlock.cpp expects -- so a "version mismatch" can be explained
instead of guessed at.
"""
from __future__ import annotations

import os
import struct
import sys

import pefile

GAME = r"H:\steam client\steamapps\common\Command and Conquer Red Alert 3\Data\ra3_1.12.game"

# rva -> (expected original, expected patched)  bytes are little-endian u32
SITES = {
    0x8B4098: ("logicFPS", 0x0000000F, 0x0000001E),
    0x8E0DEC: ("c_0.015", 0x3C75C290, 0x3CF5C290),
    0x8E0DBC: ("c_66.667", 0x42855555, 0x42055555),
    0x8E0DF8: ("c_15.0", 0x41700000, 0x41F00000),
    0x8E0DF0: ("c_30.0", 0x41F00000, 0x42700000),
    0x8E0DF4: ("c_33.333", 0x42055555, 0x41855555),
    0x8E0E34: ("c_0.0667", 0x3D888889, 0x3D088889),
    0x8E0ED4: ("c_0.0333", 0x3D088889, 0x3C888888),
}


def as_float(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else GAME
    if not os.path.isfile(path):
        print(f"missing {path}")
        return 2
    pe = pefile.PE(path, fast_load=True)
    print(f"file      : {path}")
    print(f"image base: {pe.OPTIONAL_HEADER.ImageBase:#x}")
    print(f"size of img: {pe.OPTIONAL_HEADER.SizeOfImage:#x}")
    print()
    print("sections:")
    for s in pe.sections:
        name = s.Name.rstrip(b"\x00").decode("latin1")
        print(f"  {name:8s} VA={s.VirtualAddress:#08x} vsize={s.Misc_VirtualSize:#08x} "
              f"raw={s.PointerToRawData:#08x} rsize={s.SizeOfRawData:#08x}")

    raw = open(path, "rb").read()
    print()
    print("sites:")
    bad = 0
    for rva, (label, want_old, want_new) in sorted(SITES.items()):
        off = pe.get_offset_from_rva(rva)
        sec = None
        for s in pe.sections:
            if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize,
                                                                 s.SizeOfRawData):
                sec = s.Name.rstrip(b"\x00").decode("latin1")
        if off is None or off < 0 or off + 4 > len(raw):
            print(f"  RVA {rva:08X} [{label}] -> unmapped")
            bad += 1
            continue
        (val,) = struct.unpack_from("<I", raw, off)
        state = ("ORIGINAL" if val == want_old
                 else "PATCHED" if val == want_new
                 else "UNEXPECTED")
        if state == "UNEXPECTED":
            bad += 1
        print(f"  RVA {rva:08X} [{label:9s}] sec={sec:8s} off={off:#08x} "
              f"file={val:#010x} ({as_float(val):+.6f})  expect_old={want_old:#010x} "
              f"({as_float(want_old):+.6f})  -> {state}")
    print()
    print(f"{bad} site(s) do not match the expected original value")
    return 0


if __name__ == "__main__":
    sys.exit(main())

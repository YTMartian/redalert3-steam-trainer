"""Compare the old and new address arithmetic for every patch site.

The overlay logged "mismatch" for all eight sites because va_of() subtracted the
preferred base again (g_base + (rva - 0x400000) == rva when g_base is 0x400000),
so the reads landed in .text and returned code bytes like 5B5D5E5F.

This resolves both formulas against the live engine and prints what each one
actually sees, so the fix (va = base + rva) is proven rather than assumed.
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import scan_game_processes

SITES = [
    (0x8B4098, "logicFPS   ", 0x0000000F, 0x0000001E),
    (0x8E0DEC, "0.015/0.03 ", 0x3C75C290, 0x3CF5C290),
    (0x8E0DBC, "66.7/33.3  ", 0x42855555, 0x42055555),
    (0x8E0DF8, "15.0/30.0  ", 0x41700000, 0x41F00000),
    (0x8E0DF0, "30.0/60.0  ", 0x41F00000, 0x42700000),
    (0x8E0DF4, "33.3/16.7  ", 0x42055555, 0x41855555),
    (0x8E0E34, "0.0667/0.03 ", 0x3D888889, 0x3D088889),
    (0x8E0ED4, "0.0333/0.01 ", 0x3D088889, 0x3C888889),
]


def u32(t: "p.Target", addr: int) -> int | None:
    raw = t.read(addr, 4)
    return struct.unpack("<I", raw)[0] if raw else None


def verdict(u: int | None, off: int, on: int) -> str:
    if u is None:
        return "unreadable"
    if u == off:
        return "OFF value (correct)"
    if u == on:
        return "ON value (correct)"
    return "wrong memory"


def main() -> int:
    found, _ = scan_game_processes()
    if not found:
        print("game not running")
        return 1
    h = found[0]
    t = p.Target(h["pid"])
    try:
        mods = t.modules()
        base, notes = p.resolve_base(t, mods)
        print(f"pid={h['pid']} ({h['name']})  base={base:#010x}")
        print(f"anchors: {notes}")
        print()

        old_hits = new_hits = 0
        print(f"{'RVA':<10}{'label':<12}{'old addr':>11}{'old read':>12}"
              f"{'new addr':>11}{'new read':>12}  verdict")
        print("-" * 92)
        for rva, label, off_v, on_v in SITES:
            old_addr = base + (rva - 0x400000)   # the buggy formula
            new_addr = base + rva                # the fixed formula
            ou = u32(t, old_addr)
            nu = u32(t, new_addr)
            if ou in (off_v, on_v):
                old_hits += 1
            if nu in (off_v, on_v):
                new_hits += 1
            print(f"{rva:#08x}  {label:<12}{old_addr:#011x}"
                  f"{('0x%08x' % ou) if ou is not None else 'unreadable':>12}"
                  f"  {new_addr:#011x}"
                  f"{('0x%08x' % nu) if nu is not None else 'unreadable':>12}"
                  f"  {verdict(nu, off_v, on_v)}")

        print()
        print(f"old formula matched {old_hits}/8 sites  -> "
              f"{'broken' if old_hits < 8 else 'would work'}")
        print(f"new formula matched {new_hits}/8 sites  -> "
              f"{'GOOD' if new_hits == 8 else 'STILL WRONG'}")
        print()
        print("Note: base + (rva - 0x400000) collapses to rva itself because")
        print("base is 0x400000, which is why every read hit .text and the UI")
        print("reported a version mismatch.")
        return 0 if new_hits == 8 else 1
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

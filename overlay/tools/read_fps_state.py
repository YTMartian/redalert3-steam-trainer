"""Read the live frame-rate globals and compare against the candidate patchset.

Decisive test: if RayaTrainer's 60fps works by rewriting these globals, then with
its toggle currently ON we should see 30/60 instead of 15/30. If they still read
15/30, RayaTrainer does not touch them and the hook lives somewhere else.
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import scan_game_processes


def f32(u: int) -> float:
    return struct.unpack("<f", struct.pack("<I", u))[0]


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
        if not base:
            print("base not resolved:", notes)
            return 1
        print(f"pid={h['pid']} ({h['name']})  base={base:#010x}")
        print("anchors:", notes)

        print()
        print("== engine frame-rate globals ==")
        for rva, label in ((0x8B4098, "logicFPS"), (0x8B409C, "updateFPS")):
            raw = t.read(base + rva, 4)
            if raw is None:
                print(f"  {label} @{rva:#08x}: unreadable")
                continue
            u = struct.unpack("<I", raw)[0]
            print(f"  {label:<9} @{rva:#08x} = {u:<6} (0x{u:08x})")

        print()
        print("== candidate .data sites (expected OFF -> ON) ==")
        table = getattr(p, "EXPECTED_DATA", {}) or {}
        for rva, pair in sorted(table.items()):
            a, b = pair
            raw = t.read(base + rva, 4)
            if raw is None:
                print(f"  {rva:#08x}: unreadable")
                continue
            u = struct.unpack("<I", raw)[0]
            if u == a:
                tag = "OFF-value"
            elif u == b:
                tag = "ON-value  <-- patched?"
            else:
                tag = "neither"
            print(f"  {rva:#08x} now=0x{u:08x} ({f32(u):>10.5f})  "
                  f"off=0x{a:08x} ({f32(a):.5f})  on=0x{b:08x} ({f32(b):.5f})"
                  f"   {tag}")

        print()
        print("== does the engine image look hooked? ==")
        # Report any JMP patched at the very start of the known logic update fn.
        for rva in (0x255166, 0x265560):
            raw = t.read(base + rva, 8)
            if raw:
                print(f"  {rva:#08x}: {raw.hex(' ')}")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

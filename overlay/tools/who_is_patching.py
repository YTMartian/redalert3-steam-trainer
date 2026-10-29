"""Check which modules are attached and what the frame constants currently say.

Measuring "RayaTrainer's 60fps is 2x" is only meaningful if our own overlay is not
also patching the same process. The two would be indistinguishable from the
outside, so this prints the loaded modules and the two constants before anything
else is concluded.
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes


def main() -> int:
    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    t = p.Target(found[0]["pid"])
    try:
        mods = t.modules()
        names = [m[2] for m in mods]
        base, _notes = p.resolve_base(t, mods)
        print(f"pid={found[0]['pid']} ({found[0]['name']})  base={base:#010x}")
        print()
        print("overlay :", [n for n in names if "overlay" in n.lower()] or "NO")
        print("trainer :", [n for n in names if "raya" in n.lower()
                            or "keystone" in n.lower()] or "NO")
        print()
        for rva, label in ((0x8B4098, "logicFPS"), (0x8B409C, "updateFPS")):
            raw = t.read(base + rva, 4)
            val = struct.unpack("<i", raw)[0] if raw else None
            print(f"  {label:9} at {rva:#x} = {val}")
        # The cave lives in .text padding and only exists while a 60fps patch is on.
        cave = t.read(base + 0x7C6420, 16)
        print(f"  cave      at 0x7c6420 = "
              f"{cave.hex() if cave else '?'}"
              f"  ({'patched' if cave and any(cave) else 'all zero'})")
        print()
        for rva in (0x265555, 0x265560):
            raw = t.read(base + rva, 8)
            print(f"  site {rva:#x} = {raw.hex() if raw else '?'}")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

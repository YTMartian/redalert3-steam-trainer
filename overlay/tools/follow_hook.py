"""Follow the live JMP at 0x265560 and dump what it lands on.

If RayaTrainer's 60fps is a code hook, the trampoline target tells us whether the
hook lives in the engine image or in memory the trainer allocated. That decides
whether our own overlay can reproduce the same trick with a plain byte patch.
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import scan_game_processes


def dump(t: p.Target, addr: int, n: int) -> str:
    raw = t.read(addr, n)
    if raw is None:
        return "<unreadable>"
    return raw.hex(" ")


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
        print(f"base={base:#010x}  {notes}")
        mod_size = 0
        for b, s, name in mods:
            if b == base:
                mod_size = s
                break
        print(f"engine image: {base:#010x} .. {base + mod_size:#010x}"
              f"  (size {mod_size:#x})")

        rva = 0x265560
        va = base + rva
        head = t.read(va, 16)
        print()
        print(f"@ {rva:#08x} ({va:#010x}): {head.hex(' ')}")
        if head and head[0] == 0xE9:
            rel = struct.unpack("<i", head[1:5])[0]
            tgt = va + 5 + rel
            print(f"  -> E9 jmp rel32 = {rel:+#x}  target={tgt:#010x}")
            print(f"  target inside image? "
                  f"{'yes' if base <= tgt < base + mod_size else 'NO (allocated outside image)'}")
            print(f"  target rva = {tgt - base:#x}")
            print()
            print("  trampoline head (64 bytes):")
            raw = t.read(tgt, 64)
            if raw:
                for i in range(0, len(raw), 16):
                    print(f"    {tgt + i:#010x}  {raw[i:i+16].hex(' ')}")
            # Follow a second hop (hook stubs often chain)
            if raw and raw[0] == 0xE9:
                rel2 = struct.unpack("<i", raw[1:5])[0]
                tgt2 = tgt + 5 + rel2
                print(f"  chained jmp -> {tgt2:#010x}")
                raw2 = t.read(tgt2, 64)
                if raw2:
                    for i in range(0, len(raw2), 16):
                        print(f"    {tgt2 + i:#010x}  {raw2[i:i+16].hex(' ')}")

        print()
        print("== nearby engine bytes for a sanity comparison ==")
        for r2 in (0x265550, 0x265570, 0x2655A0):
            print(f"  {r2:#08x}: {dump(t, base + r2, 16)}")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

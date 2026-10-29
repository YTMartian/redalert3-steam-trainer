"""Find the code that advances the battle clock (logic->+0x50).

The frame-rate site runs the update loop 60 times a second, and the engine keeps
the original 2:1 update-to-logic ratio, so logic ends up at 30Hz instead of the
original 15Hz. That ratio is why the 60fps patch doubles game speed.

The clock that has to stay at 15Hz is the counter at [logic+0x50] -- the same one
read_battle_clock divides by 15. So the plan is: find every place that bumps it,
identify the increment that happens once per logic tick, and then let it run on
every second update instead of every one. That is the decoupling.

This locates the candidates statically: every absolute reference to the battle
logic global 0x8DDE84 is disassembled and kept if it touches +0x50 within its
window.

Usage:
    python find_clock_writer.py
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

BATTLE_LOGIC_GLOBAL = 0x8DDE84
CLOCK_OFFSET = 0x50
WINDOW = 0x60


def main() -> int:
    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    t = p.Target(found[0]["pid"])
    try:
        mods = t.modules()
        base, notes = p.resolve_base(t, mods)
        print(f"pid={found[0]['pid']}  base={base:#010x}  {notes}\n")

        # Locate the module extent so only its image is scanned.
        mext = None
        for mbase, msize, name in mods:
            if mbase == base:
                mext = (mbase, msize, name)
                break
        if not mext:
            print("[!] engine module not found in the module list")
            return 1
        blob = t.read(base, mext[1]) or b""
        print(f"scanned {mext[2]}  {len(blob):#x} bytes")

        needle = struct.pack("<I", base + BATTLE_LOGIC_GLOBAL)
        hits = []
        start = 0
        while True:
            i = blob.find(needle, start)
            if i < 0:
                break
            hits.append(i)
            start = i + 1
        print(f"absolute references to {base + BATTLE_LOGIC_GLOBAL:#010x}: "
              f"{len(hits)}\n")

        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True

        # Disassemble backwards a little so the load and its uses appear together.
        interesting = []
        for off in hits:
            s = max(0, off - 0x30)
            code = blob[s:off + WINDOW]
            va = base + s
            txt = [f"  --- ref at {base + off:#010x} ---"]
            uses_clock = False
            for ins in md.disasm(code, va):
                mark = ""
                if f"+ 0x{CLOCK_OFFSET:x}" in ins.op_str or \
                        f"+ {CLOCK_OFFSET:#x}" in ins.op_str:
                    mark = "   <== clock field"
                    if ins.mnemonic in ("inc", "add", "mov", "xadd", "lock"):
                        mark += " WRITES"
                        uses_clock = True
                if ins.address == base + off:
                    mark += "  [global ref]"
                txt.append(f"  {ins.address:#010x}  {ins.mnemonic:<7}"
                           f"{ins.op_str}{mark}")
            if uses_clock:
                interesting.append((base + off, txt))

        print(f"references whose window touches +{CLOCK_OFFSET:#x}: "
              f"{len(interesting)}\n")
        for va, txt in interesting:
            print("\n".join(txt))
            print()
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

"""Resolve the logic tick function from the live vtable and disassemble it.

The two call sites in 0x655040 both go through [logic]->vtable+0x34, so the actual
time-advancing code is whatever that slot points at. Static scanning could not find
the clock increment because the tick receives the logic object as `this` (in ecx),
so the `[0xcdde84]` load happens in the caller, well outside the window any
"global load then write +0x50" pattern would cover.

Reading the vtable entry answers it directly, and read-only: it names the function
and then the disassembly shows whether it writes logic+0x50 and by how much. That
increment is what has to become fractional.

Usage:
    python probe_tick_fn.py
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

LOGIC_GLOBAL = 0x8DDE84
TICK_SLOT = 0x34
CLOCK_OFFSET = 0x50


def main() -> int:
    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    t = p.Target(found[0]["pid"])
    try:
        base, notes = p.resolve_base(t, t.modules())
        size = next((m[1] for m in t.modules() if m[0] == base), 0)
        print(f"pid={found[0]['pid']}  base={base:#010x}\n  {notes}")

        raw = t.read(base + LOGIC_GLOBAL, 4)
        if not raw:
            print("[!] logic global unreadable")
            return 1
        logic = struct.unpack("<I", raw)[0]
        print(f"logic object   {logic:#010x}")
        if not (0x10000 <= logic < 0x7FFFFFFF):
            print("[!] no live battle logic object (not in a match?)")
            return 1

        vt_raw = t.read(logic, 4)
        vtbl = struct.unpack("<I", vt_raw)[0] if vt_raw else 0
        print(f"vtable         {vtbl:#010x}")
        if not (0x10000 <= vtbl < 0x7FFFFFFF):
            print("[!] vtable pointer looks invalid")
            return 1

        slot_raw = t.read(vtbl + TICK_SLOT, 4)
        tick = struct.unpack("<I", slot_raw)[0] if slot_raw else 0
        print(f"[vtbl+{TICK_SLOT:#x}]  {tick:#010x}"
              f"   (module +{tick - base:#x})")

        clk = t.read(logic + CLOCK_OFFSET, 4)
        print(f"clock (+{CLOCK_OFFSET:#x})  "
              f"{struct.unpack('<I', clk)[0] if clk else 'unreadable'}")

        # Disassemble the tick and look for any write to +0x50 relative to a register
        # that could be `this` (ecx on entry, or whatever it gets copied into).
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True
        code = t.read(tick, 0x180) or b""
        print(f"\n--- {tick:#010x} ---")
        for ins in md.disasm(code, tick):
            note = ""
            if f"+ {CLOCK_OFFSET:#x}]" in ins.op_str and \
                    ins.mnemonic in ("mov", "add", "inc", "sub"):
                note = "   <<< touches the clock field"
            if ins.address > tick + 0x170:
                break
            print(f"  {ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}{note}")

        # Also report which registers the first few instructions establish as this.
        print()
        print("first instructions decide which register is `this`; any")
        print(f"write to [reg+{CLOCK_OFFSET:#x}] there is the time advance")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

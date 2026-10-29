"""Find the increment of logic->+0x50 by opcode, then confirm the object.

Searching for the absolute address of the battle-logic global is hopeless: 0xCDDE84
appears 3329 times in the image and most of those are unrelated data. The
increment itself has a much narrower signature, so this searches for the
instruction instead:

    FF 4x 50        inc  dword ptr [reg+0x50]      (mod=01, /0, disp8=0x50)
    FF 8x 50 00 00 00  inc dword ptr [reg+0x50]    (mod=10, disp32)
    add dword ptr [reg+0x50], imm8                 (83 4x 50 xx)

and keeps only the hits where the same register was loaded from the battle-logic
global shortly before. That distinguishes the simulation clock from the many
other objects that also have a field at +0x50.

Usage:
    python find_clock_inc.py
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

BATTLE_LOGIC_GLOBAL = 0x8DDE84
CLOCK = 0x50
REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


def candidate_offsets(blob: bytes):
    """Yield (offset, reg, kind, length) for instructions writing [reg+0x50].

    The clock is not necessarily bumped with `inc`: a compiler often reloads the
    field, adds, and stores it back (`mov`/`add`), which a pure `inc` scan misses
    entirely. Store forms are included for that reason.
    """
    n = len(blob)
    for i in range(n - 8):
        b = blob[i]
        nb = blob[i + 1]
        mod01_rm = 0x40 <= nb <= 0x47          # [reg+disp8]
        mod10_rm = 0x80 <= nb <= 0x87          # [reg+disp32]
        reg3 = (nb >> 3) & 7

        # inc dword ptr [reg+0x50]   (FF /0)
        if b == 0xFF and reg3 == 0:
            if mod01_rm and blob[i + 2] == CLOCK:
                yield i, nb & 7, "inc [reg+0x50]", 3
            elif mod10_rm and struct.unpack_from("<I", blob, i + 2)[0] == CLOCK:
                yield i, nb & 7, "inc [reg+0x50]", 6
        # add dword ptr [reg+0x50], imm8   (83 /0)
        if b == 0x83 and reg3 == 0 and mod01_rm and blob[i + 2] == CLOCK:
            yield i, nb & 7, "add [reg+0x50], imm8", 4
        # mov dword ptr [reg+0x50], r32   (89 /r)
        if b == 0x89:
            if mod01_rm and blob[i + 2] == CLOCK:
                yield i, nb & 7, "mov [reg+0x50], r32", 3
            elif mod10_rm and struct.unpack_from("<I", blob, i + 2)[0] == CLOCK:
                yield i, nb & 7, "mov [reg+0x50], r32", 6
        # add dword ptr [reg+0x50], r32   (01 /r)
        if b == 0x01:
            if mod01_rm and blob[i + 2] == CLOCK:
                yield i, nb & 7, "add [reg+0x50], r32", 3
            elif mod10_rm and struct.unpack_from("<I", blob, i + 2)[0] == CLOCK:
                yield i, nb & 7, "add [reg+0x50], r32", 6


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
        size = next((m[1] for m in mods if m[0] == base), 0)
        blob = t.read(base, size) or b""
        print(f"scanning {len(blob):#x} bytes for increments of [reg+0x{CLOCK:x}]")

        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True

        needle = struct.pack("<I", base + BATTLE_LOGIC_GLOBAL)
        hits = []
        for off, reg, kind, _ln in candidate_offsets(blob):
            # Does this register get loaded from the battle-logic global nearby?
            window_start = max(0, off - 0x80)
            window = blob[window_start:off]
            loads = []
            pos = 0
            while True:
                i = window.find(needle, pos)
                if i < 0:
                    break
                loads.append(window_start + i)
                pos = i + 1
            if not loads:
                continue
            hits.append((off, reg, kind, loads))

        print(f"increments of [reg+0x{CLOCK:x}] with the global loaded nearby: "
              f"{len(hits)}\n")
        for off, reg, kind, loads in hits:
            va = base + off
            s = max(0, off - 0x40)
            print(f"--- {kind} using {REGS[reg]} at {va:#010x} "
                  f"({len(loads)} global ref(s) before) ---")
            for ins in md.disasm(blob[s:off + 0x30], base + s):
                mark = "  <<< HERE" if ins.address == va else ""
                print(f"  {ins.address:#010x}  {ins.mnemonic:<7}"
                      f"{ins.op_str}{mark}")
            print()
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

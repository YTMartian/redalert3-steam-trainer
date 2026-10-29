"""Resolve hook 41's handler through the exact dispatch the dispatcher uses.

Disassembly of 0x5FFEC9C0 shows:

    lea   eax, [ecx - 1]                    ; ecx = hook id
    cmp   eax, 0x30                         ; ids 1..49
    ja    <ret>
    movzx eax, byte ptr [eax + 0x5ffede7c]  ; id -> packed index
    jmp   dword ptr [eax*4 + 0x5ffeddf8]    ; packed index -> handler

So the lookup is two-step and must be replayed exactly: skipping the byte map
lands on an unrelated handler. This resolves id 41 the same way the game does,
then disassembles the handler that actually runs.
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

BYTE_MAP = 0x5FFEDE7C
TABLE = 0x5FFEDDF8
ID = 41


def main() -> int:
    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    h = found[0]
    t = p.Target(h["pid"])
    try:
        mods = t.modules()
        base, notes = p.resolve_base(t, mods)
        print(f"base={base:#010x}  {notes}")

        print(f"byte map {BYTE_MAP:#010x}:")
        bm = t.read(BYTE_MAP, 0x31)
        if not bm:
            print("  unreadable")
            return 1
        for i in range(0, len(bm), 16):
            print(f"  [{i:02x}] " + " ".join(f"{b:02x}" for b in bm[i:i + 16]))

        idx = ID - 1
        packed = bm[idx]
        print()
        print(f"hook id {ID} -> idx {idx} (0x{idx:x}) -> byte map = {packed}")

        raw = t.read(TABLE + packed * 4, 4)
        if not raw:
            print("  table entry unreadable")
            return 1
        handler = struct.unpack("<I", raw)[0]
        print(f"handler = {handler:#010x}")

        # Show the full table for context.
        print()
        print("resolved handlers for all ids:")
        seen = {}
        for ident in range(1, len(bm) + 1):
            pk = bm[ident - 1]
            r = t.read(TABLE + pk * 4, 4)
            if not r:
                continue
            hv = struct.unpack("<I", r)[0]
            seen.setdefault(hv, []).append(ident)
        for hv, ids in sorted(seen.items()):
            marks = ""
            if ID in ids:
                marks = "   <== hook 41"
            print(f"  {hv:#010x}  ids={ids}{marks}")

        print()
        print(f"===== hook 41 handler @ {handler:#010x} =====")
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True
        body = t.read(handler, 0x600)
        if not body:
            print("  unreadable")
            return 1
        calls = []
        for ins in md.disasm(body, handler):
            print(f"  {ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}")
            if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == 2:
                calls.append(ins.operands[0].imm & 0xFFFFFFFF)
            if ins.mnemonic in ("ret", "int3"):
                break
        if calls:
            print()
            print("calls made by the handler:", [hex(c) for c in calls])
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

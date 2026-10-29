"""Disassemble a specific function range from the live process.

The hook-41 chain resolved to a dispatcher at 0x5FFEC9C0 whose body is the
actual id -> handler selection. This dumps it in full, following only within the
function (stops at ret), and reports any table it indexes so the handler for
id 41 can be read out.

Usage:
    python dump_range.py 0x5FFEC9C0 [byte_count]
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes


def main() -> int:
    addr = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x5FFEC9C0
    size = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x400

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
        body = t.read(addr, size)
        if not body:
            print(f"{addr:#010x} unreadable")
            return 1

        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True

        tables = []
        for ins in md.disasm(body, addr):
            print(f"  {ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}")
            # any absolute address operand that lives in trainer/agent memory
            for op in ins.operands:
                if op.type == 3:  # MEM
                    d = op.mem.disp & 0xFFFFFFFF
                    if 0x10000000 <= d < 0x70000000:
                        tables.append((ins.address, d, ins.mnemonic, ins.op_str))
            if ins.mnemonic in ("ret", "int3"):
                break

        if tables:
            print()
            print("== absolute memory operands (potential tables/globals) ==")
            seen = set()
            for at, d, mn, txt in tables:
                if (d, mn) in seen:
                    continue
                seen.add((d, mn))
                print(f"  {at:#010x}  {mn} {txt}   -> {d:#010x}")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

"""Inspect the state of RayaTrainer's hook and whoever owns its dispatcher.

Our overlay reproduces the eight .data constants byte-for-byte and the engine
still runs at 30, so the constants alone are evidently not the whole unlock.
RayaTrainer also installs a永久 JMP at 0x265560 whose trampoline calls a
dispatcher at a fixed address. If that code is still mapped, we can see whether
the hook is present, who owns the dispatcher, and whether it is gated.

Prints, for the live game:
  * the bytes at 0x265560 and where the JMP points
  * whether that target and the dispatcher are readable / which module owns them
  * the trampoline body
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import scan_game_processes

HOOK_RVA = 0x265560


def owner(modules, addr: int) -> str:
    for base, size, name in modules:
        if base <= addr < base + size:
            return f"{name}+{addr - base:#x}"
    return "NOT inside any loaded module (heap/VirtualAlloc)"


def dump(t: "p.Target", addr: int, n: int) -> bytes:
    return t.read(addr, n) or b""


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
        print(f"== hook site 0x{HOOK_RVA:X} ==")
        va = base + HOOK_RVA
        head = dump(t, va, 16)
        print(f"  {va:#010x}: {head.hex(' ')}")
        if head[:1] == b"\xE9":
            rel = struct.unpack("<i", head[1:5])[0]
            tgt = va + 5 + rel
            print(f"  JMP PRESENT -> {tgt:#010x}  [{owner(mods, tgt)}]")
            code = dump(t, tgt, 48)
            print(f"  trampoline: {code.hex(' ')}")
            # The stub we saw earlier did: push 0x29; mov eax,<dispatcher>; call eax
            disp = None
            for i in range(len(code) - 5):
                if code[i] == 0xB8:  # mov eax, imm32
                    cand = struct.unpack("<I", code[i + 1:i + 5])[0]
                    if 0x10000 < cand < 0x80000000:
                        disp = cand
                        break
            if disp:
                print(f"  dispatcher  {disp:#010x}  [{owner(mods, disp)}]")
                dcode = dump(t, disp, 32)
                print(f"  dispatcher bytes: {dcode.hex(' ')}")
            else:
                print("  (no mov eax,imm32 found in trampoline)")
        else:
            print("  NO JMP: hook site holds ordinary engine code")
            print("  -> RayaTrainer's hook is NOT installed right now")

        print()
        print("== our overlay loaded? ==")
        ours = [(b, s, n) for b, s, n in mods
                if "ra3_overlay" in n.lower() or "mustcode" in n.lower()]
        print(f"  {ours if ours else 'NO (DLL not injected)'}")

        print()
        print("== raya / trainer modules ==")
        raya = [(b, s, n) for b, s, n in mods
                if "raya" in n.lower() or "agent" in n.lower()]
        print(f"  {raya if raya else 'none (it writes memory externally)'}")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

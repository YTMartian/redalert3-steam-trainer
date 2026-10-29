"""Report who is currently patching the engine, and what they left behind.

Two independent things can be active at once, and they produce very different
results:

  * our overlay  -- 8 .data + 12 .code sites, applied from ra3_overlay_v4.dll
  * RayaTrainer  -- the same sites, PLUS Hook 41
                    (_BackFrameRateUnlockGameUpdate), which lives in memory the
                    trainer allocated and which we cannot copy

If the trainer is attached while our overlay is also injected, the second
overwrites the first and the result is the trainer's behaviour -- so "our port
still feels wrong" would really be a statement about the trainer.

Run with the game in a match. Read-only.
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

# Hook 41's installation site, from the captured diff.
HOOK41_RVA = 0x265555

# The 8 .data constants (off value, on value).
DATA_SITES = [
    (0x8B4098, "logicFPS  ", 0x0F, 0x1E),
    (0x8E0DEC, "0.015/0.03", 0x3C75C290, 0x3CF5C290),
    (0x8E0DBC, "66.7/33.3 ", 0x42855555, 0x42055555),
    (0x8E0DF8, "15.0/30.0 ", 0x41700000, 0x41F00000),
    (0x8E0DF0, "30.0/60.0 ", 0x41F00000, 0x42700000),
    (0x8E0DF4, "33.3/16.7 ", 0x42055555, 0x41855555),
    (0x8E0E34, "0.0667/03 ", 0x3D888889, 0x3D088889),
    (0x8E0ED4, "0.0333/01 ", 0x3D088889, 0x3C888889),
]

# A few of the 12 code sites, as the "patched" byte strings.
CODE_SITES = {
    0x17FF0A: bytes.fromhex("4864bc"),             # -> 0xBC6448
    0x2FFDAC: bytes.fromhex("4c64bc"),             # -> 0xBC644C
    0x31F457: bytes.fromhex("e9c46f4a00909090"),   # jmp trampoline
}


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
        print(f"pid={h['pid']} ({h['name']})  base={base:#010x}")
        print(f"anchors: {notes}")
        print()

        ours = [n for _b, _s, n in mods
                if "ra3_overlay" in n.lower() or "mustcode" in n.lower()]
        print(f"our overlay: {'LOADED ' + str(ours) if ours else 'not loaded'}")

        # RayaTrainer's hook lands outside every module image; if the trainer is
        # attached its code is mapped somewhere anonymous, which the module list
        # cannot show. The presence of its JMP at 0x265555 is the tell.
        hook = t.read(base + HOOK41_RVA, 8)
        trainer_hook = False
        if hook and hook[:1] == b"\xE9":
            rel = struct.unpack("<i", hook[1:5])[0]
            tgt = base + HOOK41_RVA + 5 + rel
            inside = any(b <= tgt < b + s for b, s, _n in mods)
            trainer_hook = not inside
            owner = ("engine image" if inside
                     else "anonymous memory -> RayaTrainer")
            print(f"Hook 41 @0x{HOOK41_RVA:X}: jmp {tgt:#010x}  [{owner}]")
        else:
            print(f"Hook 41 @0x{HOOK41_RVA:X}: {hook.hex(' ') if hook else '?'}"
                  f"  (not hooked)")

        print()
        on = off = other = 0
        for rva, label, off_v, on_v in DATA_SITES:
            raw = t.read(base + rva, 4)
            if not raw:
                print(f"  {rva:#08x} unreadable")
                continue
            v = struct.unpack("<I", raw)[0]
            if v == on_v:
                on += 1
                tag = "ON"
            elif v == off_v:
                off += 1
                tag = "off"
            else:
                other += 1
                tag = f"other 0x{v:08x}"
            print(f"  {rva:#08x} {label} {tag}")
        print(f"data constants: {on} ON / {off} off / {other} other")

        print()
        for rva, want in CODE_SITES.items():
            got = t.read(base + rva, len(want))
            print(f"  code {rva:#08x}: "
                  f"{'patched' if got == want else 'original/unpatched'}")

        print()
        print("== verdict ==")
        if trainer_hook:
            print("[!] RayaTrainer's Hook 41 IS installed in this process.")
            print("[!] It hooks the game update using code only the trainer has,")
            print("[!] so what you are feeling includes the trainer's own logic.")
            print("[!] To test our port alone: close RayaTrainer, restart the")
            print("[!] game, inject only overlay\\bin\\main.exe.")
        elif ours:
            print("[+] Our overlay is the only thing patching the engine.")
            print("[+] The bytes match the trainer's 60fps state exactly.")
        else:
            print("[=] Nothing is patched right now.")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

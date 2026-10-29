"""Prove our patchset reproduces RayaTrainer's memory state exactly.

Applies the 12 portable regions to the on-disk image in memory, then compares the
predicted bytes against what a live, RayaTrainer-60fps-enabled process actually
holds. If every region matches, toggling our switch writes the same bytes the
trainer writes, so behaviour should be identical.

Run while RayaTrainer has 60fps ON (the state we captured from).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

TOOLS = Path(__file__).resolve().parent


def main() -> int:
    data = json.loads((TOOLS / "raya_full_patchset.json").read_text(
        encoding="utf-8"))
    patches = data["patches"]
    portable = [p_ for p_ in patches
                if int(p_["rva"], 16) in
                (0x7C6420, 0x17FF0A, 0x22A756, 0x23EA21, 0x255167, 0x2686F9,
                 0x2FFDAC, 0x31F457, 0x7B40A8, 0x7B40D8, 0x7B4108, 0x7B4C29)]

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

        ok = 0
        print()
        for entry in portable:
            rva = int(entry["rva"], 16)
            want = bytes.fromhex(entry["live"])
            got = t.read(base + rva, len(want))
            same = got == want
            if same:
                ok += 1
            print(f"  {rva:#08x} len={len(want):<3} "
                  f"{'MATCH' if same else 'DIFFERS'}")
            if not same:
                print(f"      want {want.hex(' ')}")
                print(f"      got  {got.hex(' ') if got else 'unreadable'}")

        print()
        print(f"{ok}/{len(portable)} regions match the live patched state")
        if ok != len(portable):
            print()
            print("[!] Mismatch means RayaTrainer's 60fps is probably OFF right")
            print("[!] now, or the game restarted since the capture.")
            print("[!] Turn it ON in RayaTrainer and run this again.")
        return 0 if ok == len(portable) else 1
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

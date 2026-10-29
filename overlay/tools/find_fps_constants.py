"""Locate the 60fps timing constants in the live RA3 process.

The RVAs recovered from RayaTrainer's builder do not hold these constants on
this install (reading them returns machine code), so instead of guessing where
they live, scan the loaded image for the exact 4-byte float payloads and print
the RVA of every hit.

Run while the game is IN A MATCH (the values are BSS at startup; they only get
written once the engine initialises its timing).

    python find_fps_constants.py
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p

# name -> bytes of the ORIGINAL (vanilla 30fps) value
WANTED = {
    "c_0.015": bytes.fromhex("90C2753C"),    # 0.015
    "c_66.667": bytes.fromhex("55558542"),   # 66.6667
    "c_15.0": bytes.fromhex("00007041"),     # 15.0
    "c_30.0": bytes.fromhex("0000F041"),     # 30.0
    "c_33.333": bytes.fromhex("55550542"),   # 33.3333
    "c_0.0667": bytes.fromhex("8988883D"),   # 0.0666667
    "c_0.0333": bytes.fromhex("8988083D"),   # 0.0333333
    "c_16.667": bytes.fromhex("55558541"),   # 16.6667 (patched value of 33.333)
}

# RayaTrainer's expected RVAs, for comparison against what we actually find.
EXPECTED = {
    "c_0.015": 0x8E0DEC,
    "c_66.667": 0x8E0DBC,
    "c_15.0": 0x8E0DF8,
    "c_30.0": 0x8E0DF0,
    "c_33.333": 0x8E0DF4,
    "c_0.0667": 0x8E0E34,
    "c_0.0333": 0x8E0ED4,
}


def resolve():
    pids = p.find_pids(p.PROCESS_NAME)
    if not pids:
        return None, None
    for pid in pids:
        t = p.Target(pid)
        base, notes = p.resolve_base(t, t.modules())
        if base:
            print(f"[i] pid={pid} base={base:#010x}")
            return t, base
        t.close()
    return None, None


def main() -> int:
    t, base = resolve()
    if not t:
        print("[!] No process with ra3_1.12.game loaded.")
        print("[!] Start a skirmish first (the launcher alone is not enough).")
        return 2

    try:
        size = p.TEXT_RVA + p.TEXT_SIZE + 0x200000
        print(f"[i] scanning {size / 1048576:.1f} MB of image ...")
        image = t.read_committed(base, size, chunk=0x40000)

        logic = None
        for off in range(0, min(len(image), 0x1000000) - 4, 4):
            if image[off:off + 4] == b"\x0F\x00\x00\x00":
                logic = off
                break
        print()
        print(f"first 15 (logicFPS candidate) at RVA {logic:#08X}"
              if logic is not None else "no 15 found")

        print()
        print("## constant hits")
        for name, pat in WANTED.items():
            hits = []
            start = 0
            while True:
                i = image.find(pat, start)
                if i < 0:
                    break
                hits.append(i)
                start = i + 1
            exp = EXPECTED.get(name)
            exp_txt = f"expected {exp:#08X}" if exp else "(no expectation)"
            if not hits:
                print(f"{name:10s} {exp_txt:22s} NOT FOUND")
            else:
                shown = ", ".join(f"{h:#08X}" for h in hits[:8])
                more = f" (+{len(hits) - 8} more)" if len(hits) > 8 else ""
                mark = ""
                if exp is not None and exp in hits:
                    mark = "  <- matches RayaTrainer"
                print(f"{name:10s} {exp_txt:22s} hits: {shown}{more}{mark}")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

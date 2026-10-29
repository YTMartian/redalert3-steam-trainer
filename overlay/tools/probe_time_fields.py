"""Sample every time-like field of the logic object to find the 2x one.

The half-time decouple made the battle clock (logic+0x50) advance at exactly
1.00x, yet unit motion, building animation and water still look doubled. That means
the visuals read a different accumulator, and guessing which one wastes the user's
time.

This samples a set of candidate fields over a known real interval and prints the
rate of each, so the field that still runs at twice the baseline is identified by
measurement. Integers are reported in units/second, floats in units/second with the
raw value printed too; a field that is 2x while +0x50 is 1x is the animation clock.

Baseline for comparison: originally logic+0x50 advances 15/s (that is what
read_sim_seconds divides by), and the engine's logic rate is 15Hz.

Usage:
    python probe_time_fields.py [seconds]
"""
from __future__ import annotations

import struct
import sys
import time

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

LOGIC_GLOBAL = 0x8DDE84

INT_FIELDS = [0x50, 0x54, 0x58, 0x5C, 0x148, 0x15C, 0x170, 0x174, 0x178,
              0x198, 0x1A0, 0x1A4, 0x1A8, 0x1AC, 0x31C]
FLOAT_FIELDS = [0x60, 0x64, 0x68, 0x6C, 0x70, 0x74]


def main() -> int:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0

    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    t = p.Target(found[0]["pid"])
    try:
        base, notes = p.resolve_base(t, t.modules())
        raw = t.read(base + LOGIC_GLOBAL, 4)
        if not raw:
            print("[!] logic global unreadable")
            return 1
        logic = struct.unpack("<I", raw)[0]
        if not (0x10000 <= logic < 0x7FFFFFFF):
            print("[!] not in a match")
            return 1
        print(f"pid={found[0]['pid']}  base={base:#010x}  logic={logic:#010x}")
        print(f"  {notes}")
        print(f"sampling {secs:.1f}s\n")

        def snap():
            out = {}
            for off in INT_FIELDS:
                b = t.read(logic + off, 4)
                out[("i", off)] = struct.unpack("<i", b)[0] if b else None
            for off in FLOAT_FIELDS:
                b = t.read(logic + off, 4)
                out[("f", off)] = struct.unpack("<f", b)[0] if b else None
            return out

        a = snap()
        t0 = time.perf_counter()
        time.sleep(secs)
        z = snap()
        real = time.perf_counter() - t0
        print(f"real interval {real:.3f}s\n")

        print(f"{'field':10} {'start':>14} {'end':>14} {'per sec':>10}")
        rows = []
        for key in a:
            kind, off = key
            v0, v1 = a[key], z[key]
            if v0 is None or v1 is None:
                continue
            d = (v1 - v0) / real
            name = f"+{off:#04x}" + ("f" if kind == "f" else "")
            rows.append((abs(d), name, v0, v1, d))
        for _ad, name, v0, v1, d in sorted(rows, reverse=True):
            if abs(d) < 1e-6:
                continue
            print(f"{name:10} {v0:>14.4f} {v1:>14.4f} {d:>10.4f}")

        print()
        print("reference: logic+0x50 is the battle clock, 15 units = 1 simulated")
        print("second, so 15.0/s means 1.00x. Any other field at 30/s")
        print("(or a float at double the baseline) is the 2x animation clock.")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

"""Tune the trampoline's speed multiplier and measure the resulting game speed.

The code patchset renders at 60 but advances the simulation at 0.67x. The lever
is the float at the head of the cave:

    0x7C6420  movss xmm1,[edx+0xF0]
              mulss xmm1,[0xBC6440]    ; <-- this multiplier, 0.25 as captured
              jmp   0x31F45F

Whatever it scales, changing it changes how far the simulation advances per
update, so it is the natural knob for correcting 0.67x back to 1.0x. This sweeps
it and reports speed, so the right value is measured rather than guessed.

Only the code group is applied: the .data constants stay at their original
values, so the 2x from logicFPS 15->30 stays out of the picture.

Usage:
    python tune_multiplier.py [seconds_per_value] [value ...]
"""
from __future__ import annotations

import ctypes
import struct
import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

CAVE_MULT_VA = 0x7C6440   # module-relative, same base as the patchset RVAs


def main() -> int:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
    values = ([float(v) for v in sys.argv[2:]]
              or [0.25, 0.3125, 0.375, 0.4375, 0.5])

    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    h = found[0]
    k32, handle, write, werr = A.open_writer(h["pid"])
    if not handle:
        return 2

    t = p.Target(h["pid"])
    base = 0
    rows = []
    try:
        base, _notes = p.resolve_base(t, t.modules())
        data, code = A.load_sites()

        if A.read_sim_seconds(t, base) is None:
            print("[!] not in a match; start a skirmish and rerun")
            return 1

        # Code group only, with the logic constants untouched.
        fails = A.apply_state(write, base, data, code, False, True)
        if fails:
            print(f"[!] {len(fails)} write(s) failed ({fails[0]}, err={werr()})")
            return 1
        time.sleep(0.5)

        print(f"pid={h['pid']}  base={base:#010x}   code group only")
        print(f"multiplier at {CAVE_MULT_VA:#x}, {secs:.0f}s per value\n")

        for v in values:
            raw = struct.pack("<f", v)
            if not write(base + CAVE_MULT_VA, raw):
                print(f"  [!] cannot write multiplier {v} (err={werr()})")
                continue
            back = t.read(base + CAVE_MULT_VA, 4)
            if back != raw:
                shown = struct.unpack("<f", back)[0] if back else None
                print(f"  [!] multiplier {v} did not stick "
                      f"(read back {shown})")
                continue
            time.sleep(0.4)
            s0 = A.read_sim_seconds(t, base)
            t0 = time.perf_counter()
            time.sleep(secs)
            s1 = A.read_sim_seconds(t, base)
            real = time.perf_counter() - t0
            if s0 is None or s1 is None:
                print(f"  multiplier {v:<7} left the match mid-sample")
                continue
            speed = (s1 - s0) / real
            rows.append((v, speed))
            print(f"  multiplier {v:<7} speed {speed:5.2f}x   "
                  f"implied {v / speed if speed else 0:.4f} per 1.0x")
    finally:
        try:
            if base:
                A.apply_state(write, base, data, code, False, False)
                print("\nreverted, including the multiplier")
        except Exception:
            pass
        t.close()
        k32.CloseHandle(handle)

    if len(rows) >= 2:
        print()
        print(f"{'multiplier':>12} {'speed':>8}")
        for v, s in rows:
            print(f"{v:>12.4f} {s:>7.2f}x")
        # Speed is proportional to the multiplier iff v/speed is constant.
        per = [v / s for v, s in rows if s > 0.01]
        spread = (max(per) - min(per)) / (sum(per) / len(per))
        print()
        if spread < 0.08:
            target = sum(per) / len(per)
            print(f"linear: speed = multiplier / {target:.4f}")
            print(f"for 1.00x use multiplier = {target:.4f}")
        else:
            print(f"not linear (per-unit spread {spread * 100:.0f}%); "
                  f"pick the value whose measured speed is closest to 1.00x")
    return 0


if __name__ == "__main__":
    sys.exit(main())

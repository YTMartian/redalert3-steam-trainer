"""Sweep logicFPS with only the frame-rate site applied.

The redirects turned out to be two independent effects:

    0x2686F9    raises the frame rate to 60 and doubles simulated time (2.00x)
    0x255167    divides simulated time by ~3 (0.335x)
    the other seven   no measurable effect

so there is no subset that lands between 2.00x and 0.67x. The frame rate switch is
a single site, and the only remaining knob that can scale simulated time without
touching the frame rate is the logicFPS constant itself.

Measured before (with every redirect applied) doubling logicFPS 15->30 multiplied
speed by ~3, which is why intermediate values are worth trying here: with only
0x2686F9 on, a value that halves 2.00x gives 60fps at 1.00x.

logicFPS at 0x8B4098 is not redirected, so it is written directly. updateFPS is
supplied by the trampoline at 0x7C6448 and held at 60.

Usage:
    python sweep_logic_only.py [seconds] [value ...]
"""
from __future__ import annotations

import struct
import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from split_code import CAVE_RVA
from sweep_fps import frames_after, LOG

LOGIC_FPS = 0x8B4098
CAVE_UPDATE_FPS = 0x7C6448
FPS_SITE = 0x2686F9


def main() -> int:
    secs = 10.0
    args = sys.argv[1:]
    if args:
        try:
            secs = float(args[0])
            args = args[1:]
        except ValueError:
            pass
    values = [int(v) for v in args] or [4, 6, 8, 10, 12, 15, 20, 30]

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
        sites = {rva: (o, n) for rva, o, n in code}

        if A.read_sim_seconds(t, base) is None:
            print("[!] not in a match; start a skirmish and rerun")
            return 1

        A.apply_state(write, base, data, code, False, False)
        write(base + CAVE_RVA, sites[CAVE_RVA][1])
        write(base + FPS_SITE, sites[FPS_SITE][1])
        write(base + CAVE_UPDATE_FPS, struct.pack("<i", 60))

        print(f"pid={h['pid']}  base={base:#010x}")
        print(f"only {FPS_SITE:#x} applied (60fps switch), "
              f"updateFPS(cave)=60, {secs:.0f}s per logicFPS\n")

        for v in values:
            if not write(base + LOGIC_FPS, struct.pack("<i", v)):
                print(f"  [!] cannot write logicFPS={v} (err={werr()})")
                continue
            back = t.read(base + LOGIC_FPS, 4)
            got = struct.unpack("<i", back)[0] if back else None
            if got != v:
                print(f"  [!] logicFPS={v} did not stick (read back {got})")
                continue
            time.sleep(0.8)
            off = LOG.stat().st_size if LOG.exists() else 0
            s0 = A.read_sim_seconds(t, base)
            t0 = time.perf_counter()
            time.sleep(secs)
            s1 = A.read_sim_seconds(t, base)
            real = time.perf_counter() - t0
            pr, _es, n = frames_after(off)
            if s0 is None or s1 is None:
                print(f"  logicFPS {v:<3} left the match mid-sample")
                continue
            speed = (s1 - s0) / real
            rows.append((v, speed, pr, n))
            ftxt = "  n/a" if pr is None else f"{pr:5.1f}"
            print(f"  logicFPS {v:<3} speed {speed:5.2f}x   "
                  f"Present {ftxt} fps   ({n} samples)")
    finally:
        try:
            if base:
                A.apply_state(write, base, data, code, False, False)
                print("\nreverted (logicFPS back to 15, cave cleared)")
        except Exception:
            pass
        t.close()
        k32.CloseHandle(handle)

    print()
    print(f"{'logicFPS':>9} {'speed':>7} {'fps':>6}")
    for v, speed, pr, _n in rows:
        ftxt = "-" if pr is None else f"{pr:.0f}"
        print(f"{v:>9} {speed:>6.2f}x {ftxt:>6}")
    good = [r for r in rows if r[2] and abs(r[1] - 1.0) < 0.06 and r[2] >= 50]
    print()
    if good:
        print("60fps at normal speed with logicFPS:",
              ", ".join(str(r[0]) for r in good))
    else:
        print("nothing landed on 1.00x with 60fps")
    return 0


if __name__ == "__main__":
    sys.exit(main())

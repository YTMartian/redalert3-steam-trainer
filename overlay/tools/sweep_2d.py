"""Two-dimensional sweep: cave updateFPS across logicFPS, with the divide site on.

The reachable points found so far were all measured with cave updateFPS pinned at
60. But the engine derives the logic rate from updateFPS by an integer step, and
that step is not the only input: the site at 0x255167 shifts it too. Holding one
of the two inputs fixed risks looking at a ladder that simply does not contain
(60fps, 15Hz), when a different raw updateFPS might.

Each cell reports both numbers, because the target is a point, not a curve:
60fps at 1.00x. Cave updateFPS is varied, so the frame rate is an output and can
land below 60 -- that is a legitimate result, not a failure of the harness.

Sites applied: 0x2686F9 (frame rate switch) and 0x255167 (the divide), which is
the only pair whose combination produced anything other than 2.00x in the
one-at-a-time sweep.

Usage:
    python sweep_2d.py [seconds]
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
DIV_SITE = 0x255167

CAVE_VALUES = [60, 66, 72, 80, 90]
LOGIC_VALUES = [15, 18, 20, 24, 30]


def main() -> int:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 7.0

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
    grid = {}
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
        write(base + DIV_SITE, sites[DIV_SITE][1])

        print(f"pid={h['pid']}  base={base:#010x}  {secs:.0f}s per cell")
        print(f"sites {FPS_SITE:#x} + {DIV_SITE:#x}\n")
        print(f"{'cave':>5} {'logic':>6} {'speed':>7} {'fps':>6}")

        for cave in CAVE_VALUES:
            write(base + CAVE_UPDATE_FPS, struct.pack("<i", cave))
            for lf in LOGIC_VALUES:
                write(base + LOGIC_FPS, struct.pack("<i", lf))
                time.sleep(0.7)
                off = LOG.stat().st_size if LOG.exists() else 0
                s0 = A.read_sim_seconds(t, base)
                t0 = time.perf_counter()
                time.sleep(secs)
                s1 = A.read_sim_seconds(t, base)
                real = time.perf_counter() - t0
                pr, _es, _n = frames_after(off)
                if s0 is None or s1 is None:
                    print(f"{cave:>5} {lf:>6}   left the match")
                    continue
                speed = (s1 - s0) / real
                grid[(cave, lf)] = (speed, pr)
                ftxt = "   n/a" if pr is None else f"{pr:6.1f}"
                flag = ""
                if pr and abs(speed - 1.0) < 0.06 and pr >= 55:
                    flag = "   <== BOTH"
                print(f"{cave:>5} {lf:>6} {speed:>6.2f}x {ftxt}{flag}")
    finally:
        try:
            if base:
                A.apply_state(write, base, data, code, False, False)
                print("\nreverted (logicFPS 15, cave cleared)")
        except Exception:
            pass
        t.close()
        k32.CloseHandle(handle)

    winners = [(k, v) for k, v in grid.items()
               if v[1] and abs(v[0] - 1.0) < 0.06 and v[1] >= 55]
    print()
    if winners:
        for (cave, lf), (speed, pr) in winners:
            print(f"cave updateFPS={cave}, logicFPS={lf}: "
                  f"{speed:.2f}x at {pr:.1f}fps")
    else:
        print("no cell gave 60fps at 1.00x; the constant ladder has no such point")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Sweep the redirected updateFPS value and see how speed and fps trade off.

Every redirected site reads updateFPS from the cave at 0x7C6448, so the value put
there is what the engine actually uses, regardless of what the .data global says.
That makes it the single clean knob for this question:

    if speed falls as fps rises along a straight line, frame rate and simulation
    rate are the same quantity in this engine and no constant-only patch can give
    60fps at normal speed -- the decoupling RayaTrainer does in its Hook 41 would
    be required. If instead there is a value with both 60fps and 1.00x, that value
    is the answer.

Runs with the cave and the redirects in place, d0 (logicFPS) left at its original
value, and the cave float untouched.

Usage:
    python sweep_rate.py [seconds] [value ...]
"""
from __future__ import annotations

import struct
import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from split_code import CAVE_RVA, UPD_RVAS
from sweep_fps import frames_after, LOG

CAVE_UPDATE_FPS = 0x7C6448   # int the 9 redirected sites now read


def main() -> int:
    secs = 12.0
    args = sys.argv[1:]
    if args:
        try:
            secs = float(args[0])
            args = args[1:]
        except ValueError:
            pass
    values = [int(v) for v in args] or [30, 40, 45, 50, 60]

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
        for rva in UPD_RVAS:
            write(base + rva, sites[rva][1])

        print(f"pid={h['pid']}  base={base:#010x}   "
              f"cave updateFPS at {CAVE_UPDATE_FPS:#x}, {secs:.0f}s per value\n")

        for v in values:
            if not write(base + CAVE_UPDATE_FPS, struct.pack("<i", v)):
                print(f"  [!] cannot write updateFPS={v} (err={werr()})")
                continue
            back = t.read(base + CAVE_UPDATE_FPS, 4)
            if back != struct.pack("<i", v):
                print(f"  [!] updateFPS={v} did not stick")
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
                print(f"  updateFPS {v:<4} left the match mid-sample")
                continue
            speed = (s1 - s0) / real
            rows.append((v, speed, pr, n))
            ftxt = "  n/a" if pr is None else f"{pr:5.1f}"
            print(f"  updateFPS {v:<4} speed {speed:4.2f}x   Present {ftxt} fps"
                  f"   ({n} samples)")
    finally:
        try:
            if base:
                A.apply_state(write, base, data, code, False, False)
                print("\nreverted to the original bytes")
        except Exception:
            pass
        t.close()
        k32.CloseHandle(handle)

    print()
    print(f"{'cave updateFPS':>15} {'speed':>7} {'fps':>6}")
    for v, speed, pr, _n in rows:
        ftxt = "-" if pr is None else f"{pr:.0f}"
        print(f"{v:>15} {speed:>6.2f}x {ftxt:>6}")
    both = [r for r in rows if r[2] and abs(r[1] - 1.0) < 0.05 and r[2] >= 50]
    print()
    if both:
        print("candidates with 1.00x and >=50fps:", [r[0] for r in both])
    else:
        print("no single value gives both; speed and fps move together here")
    return 0


if __name__ == "__main__":
    sys.exit(main())

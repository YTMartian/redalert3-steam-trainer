"""Test the GameSpeedFactor scalar as a way to cancel the 2x.

Constants cannot produce 60fps at 15Hz logic: the engine snaps the logic rate to
update/N with integer N, and the reachable points with the frame-rate site on are
(60fps, 10Hz), (60fps, 20Hz) and (45fps, 15Hz). None is (60fps, 15Hz).

The remaining candidate is a continuous multiplier rather than a rate. The overlay
already reads one at [[0xCE8138]+0x30], labelled GameSpeedFactor, and the game has
a speed option of its own, so this measures whether writing it scales simulated
time. If it does, the frame-rate site (2.00x) with a factor of 0.5 is exactly
1.00x at 60fps, with no hook.

The test runs the factor alone against the untouched baseline first: a value that
does nothing there will not compose with the patch either, and checking that first
avoids chasing a dead end.

Usage:
    python sweep_gamespeed.py [seconds] [factor ...]
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

GLOBAL_OBJ = 0x8E8138      # module RVA of the pointer to the options object
OFF_GAME_SPEED = 0x30
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
    factors = [float(v) for v in args] or [1.0, 0.75, 0.5, 0.25]

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

        raw = t.read(base + GLOBAL_OBJ, 4)
        obj = struct.unpack("<I", raw)[0] if raw else 0
        if not (0x10000 <= obj < 0x7FFFFFFF):
            print(f"[!] options object not readable "
                  f"([{GLOBAL_OBJ:#x}]={obj:#x})")
            return 1
        field = obj + OFF_GAME_SPEED
        orig = t.read(field, 4)
        orig_f = struct.unpack("<f", orig)[0] if orig else None
        print(f"pid={h['pid']}  base={base:#010x}")
        print(f"options object {obj:#010x}   GameSpeedFactor at {field:#010x}"
              f" = {orig_f}")

        def measure(label: str):
            off = LOG.stat().st_size if LOG.exists() else 0
            s0 = A.read_sim_seconds(t, base)
            t0 = time.perf_counter()
            time.sleep(secs)
            s1 = A.read_sim_seconds(t, base)
            real = time.perf_counter() - t0
            pr, _es, n = frames_after(off)
            if s0 is None or s1 is None:
                print(f"  {label:22} left the match mid-sample")
                return None
            speed = (s1 - s0) / real
            print(f"  {label:22} speed {speed:5.2f}x   Present "
                  f"{'n/a' if pr is None else f'{pr:5.1f}'} fps   ({n} samples)")
            rows.append((label, speed, pr))
            return speed

        # --- phase 1: factor alone, no patches at all --------------------
        print("\n=== factor alone (no 60fps patch) ===")
        A.apply_state(write, base, data, code, False, False)
        time.sleep(0.5)
        measure("baseline (untouched)")
        for f in factors:
            if f == 1.0:
                continue
            if not write(field, struct.pack("<f", f)):
                print(f"  [!] cannot write factor {f} (err={werr()})")
                continue
            back = t.read(field, 4)
            got = struct.unpack("<f", back)[0] if back else None
            if got is None or abs(got - f) > 1e-4:
                print(f"  [!] factor {f} did not stick (read back {got})")
                continue
            time.sleep(0.8)
            measure(f"factor {f}")

        # --- phase 2: factor with the 60fps site ------------------------
        print("\n=== factor with the 60fps site (2.00x baseline) ===")
        A.apply_state(write, base, data, code, False, False)
        write(base + CAVE_RVA, sites[CAVE_RVA][1])
        write(base + FPS_SITE, sites[FPS_SITE][1])
        write(base + CAVE_UPDATE_FPS, struct.pack("<i", 60))
        write(base + LOGIC_FPS, struct.pack("<i", 15))
        time.sleep(0.5)
        measure("60fps site, factor 1.0")
        for f in factors:
            if f == 1.0:
                continue
            if not write(field, struct.pack("<f", f)):
                continue
            time.sleep(0.8)
            measure(f"60fps site, factor {f}")
    finally:
        try:
            if base:
                write(field, orig)                      # put the factor back
                A.apply_state(write, base, data, code, False, False)
                print("\nreverted (factor and patches restored)")
        except Exception:
            pass
        t.close()
        k32.CloseHandle(handle)

    print()
    print(f"{'case':24} {'speed':>7} {'fps':>6}")
    for label, speed, pr in rows:
        ftxt = "-" if pr is None else f"{pr:.0f}"
        print(f"{label:24} {speed:>6.2f}x {ftxt:>6}")
    good = [r for r in rows if r[2] and abs(r[1] - 1.0) < 0.06 and r[2] >= 50]
    print()
    if good:
        print("60fps at normal speed:", ", ".join(r[0] for r in good))
    else:
        print("the factor did not cancel the 2x")
    return 0


if __name__ == "__main__":
    sys.exit(main())

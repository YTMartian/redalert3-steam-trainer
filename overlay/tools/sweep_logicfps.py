"""Sweep logicFPS while updateFPS is held at 60.

The two rates are separate controls:
  * updateFPS  -> how often the frame/update pass runs   (60 here)
  * logicFPS   -> how fast simulated time advances per pass

Measured endpoints with updateFPS=60:
    logicFPS 15 (original) -> 0.67x at 60fps   (logic too slow)
    logicFPS 30 (doubled)  -> 2.00x at 60fps   (logic too fast)

Those two are the only points tested so far, and they suggest logicFPS is a
multiplier rather than a rate. If any intermediate integer lands near 1.00x while
the frame rate stays at 60, the decoupling needs no hook at all -- just the right
constant. That is the cheapest possible outcome, so it is worth one sweep.

logicFPS lives at 0x8B4098 as int32 and is not one of the redirected sites, so it
can be set directly. updateFPS is set inside the trampoline at 0x7C6448.

Usage:
    python sweep_logicfps.py [seconds] [value ...]
"""
from __future__ import annotations

import struct
import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from split_code import CAVE_RVA, UPD_RVAS
from sweep_fps import LOG, frames_after

LOGIC_FPS = 0x8B4098
CAVE_UPDATE_FPS = 0x7C6448


def main() -> int:
    secs = 10.0
    args = sys.argv[1:]
    if args:
        try:
            secs = float(args[0])
            args = args[1:]
        except ValueError:
            pass
    values = [int(v) for v in args] or [15, 18, 20, 22, 24, 26, 28, 30]

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

        # Frame patches only: redirects plus the cave, no .data constants yet.
        A.apply_state(write, base, data, code, False, False)
        write(base + CAVE_RVA, sites[CAVE_RVA][1])
        for rva in UPD_RVAS:
            write(base + rva, sites[rva][1])
        write(base + CAVE_UPDATE_FPS, struct.pack("<i", 60))

        print(f"pid={h['pid']}  base={base:#010x}   updateFPS(cave)=60, "
              f"{secs:.0f}s per logicFPS\n")

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
            print(f"  logicFPS {v:<3} speed {speed:5.2f}x   Present {ftxt} fps"
                  f"   ({n} samples)")
    finally:
        try:
            if base:
                A.apply_state(write, base, data, code, False, False)
                print("\nreverted to the original bytes (logicFPS back to 15)")
        except Exception:
            pass
        t.close()
        k32.CloseHandle(handle)

    print()
    print(f"{'logicFPS':>9} {'speed':>7} {'fps':>6}")
    for v, speed, pr, _n in rows:
        ftxt = "-" if pr is None else f"{pr:.0f}"
        print(f"{v:>9} {speed:>6.2f}x {ftxt:>6}")
    good = [r for r in rows if r[2] and abs(r[1] - 1.0) < 0.05 and r[2] >= 50]
    print()
    if good:
        print("BOTH goals met at logicFPS:",
              ", ".join(str(r[0]) for r in good))
    else:
        print("no logicFPS gave 1.00x at >=50fps with these settings")
    return 0


if __name__ == "__main__":
    sys.exit(main())

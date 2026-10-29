"""Test site combinations crossed with logicFPS values.

Why this exists: constants alone clamp simulated time to discrete rates. With the
frame-rate site on, logicFPS snaps to 10/20/30/45 Hz and 15 Hz (the original
logic rate) is not reachable, so no single knob gives 60fps at 1.00x.

Two measured factors do line up arithmetically though:

    0x2686F9 alone                  2.00x   (logic 30 Hz)
    0x2686F9 + 0x255167             0.67x   (logic 10 Hz)
    logicFPS 20 instead of 15        x1.50

0.67 x 1.50 = 1.00, so "frame site + divide site + raised logicFPS" is worth
measuring: if the multipliers compose and the frame rate stays at 60, that
combination is the unlock, with no hook at all.

The same script also verifies whether the naive pairing really composes, because
clamping could make the second factor act on the pre-division rate instead.

Usage:
    python sweep_combo.py [seconds] [logicFPS ...]
    (site pairs are fixed below so every run is comparable)
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


def main() -> int:
    secs = 10.0
    args = sys.argv[1:]
    if args:
        try:
            secs = float(args[0])
            args = args[1:]
        except ValueError:
            pass
    values = [int(v) for v in args] or [12, 15, 18, 20, 22, 24]

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
        write(base + DIV_SITE, sites[DIV_SITE][1])
        write(base + CAVE_UPDATE_FPS, struct.pack("<i", 60))

        print(f"pid={h['pid']}  base={base:#010x}")
        print(f"sites {FPS_SITE:#x} + {DIV_SITE:#x}, updateFPS(cave)=60")
        print(f"that pair alone measured 0.67x at 60fps\n")

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
                print("\nreverted (logicFPS back to 15)")
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
        print("60fps at normal speed:", ", ".join(str(r[0]) for r in good))
    else:
        print("no combination reached 1.00x at 60fps")
    return 0


if __name__ == "__main__":
    sys.exit(main())

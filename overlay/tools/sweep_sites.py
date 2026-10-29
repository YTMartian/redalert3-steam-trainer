"""Apply the updateFPS redirects one at a time to separate their purposes.

All nine redirect sites rewrite the same load `[0x8B409C]` (updateFPS) to read the
value in the trampoline instead, so from the outside they look identical. But an
engine uses that number for more than one job -- at least the frame cap and the
logic timestep -- and applying all nine at once moves both, which is why the whole
set gives 60fps at the wrong speed.

If some sites only drive the frame cap, turning on just those should raise the
frame rate while leaving simulated time at 1.00x. That would be a 60fps unlock
with no hook at all.

Each run applies: the cave (its data must exist first), exactly one redirect, and
nothing else -- logicFPS stays at its original 15.

Usage:
    python sweep_sites.py [seconds]
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

CAVE_UPDATE_FPS = 0x7C6448
LOGIC_FPS = 0x8B4098


def main() -> int:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0

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

        order = sorted(UPD_RVAS)
        print(f"pid={h['pid']}  base={base:#010x}  {secs:.0f}s per site")
        print("baseline first, then each redirect alone, then all of them\n")

        runs = [("none", [])] + [(f"{rva:#x}", [rva]) for rva in order] + \
               [("all", order)]

        for label, picks in runs:
            A.apply_state(write, base, data, code, False, False)
            write(base + CAVE_RVA, sites[CAVE_RVA][1])
            write(base + CAVE_UPDATE_FPS, struct.pack("<i", 60))
            write(base + LOGIC_FPS, struct.pack("<i", 15))  # original logic rate
            for rva in picks:
                write(base + rva, sites[rva][1])

            time.sleep(0.8)
            landed = sum(1 for rva in picks
                         if t.read(base + rva, 3) == sites[rva][1])
            off = LOG.stat().st_size if LOG.exists() else 0
            s0 = A.read_sim_seconds(t, base)
            t0 = time.perf_counter()
            time.sleep(secs)
            s1 = A.read_sim_seconds(t, base)
            real = time.perf_counter() - t0
            pr, es, n = frames_after(off)
            if s0 is None or s1 is None:
                print(f"  {label:10} left the match mid-sample")
                continue
            speed = (s1 - s0) / real
            rows.append((label, speed, pr, len(picks), landed))
            ftxt = "  n/a" if pr is None else f"{pr:5.1f}"
            print(f"  {label:10} speed {speed:5.2f}x   Present {ftxt} fps"
                  f"   ({landed}/{len(picks) if picks else 0} landed)")
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
    print(f"{'site':12} {'speed':>7} {'fps':>6}  sites")
    for label, speed, pr, npick, landed in rows:
        ftxt = "-" if pr is None else f"{pr:.0f}"
        print(f"{label:12} {speed:>6.2f}x {ftxt:>6}  {landed}/{npick}")
    print()
    fast = [r for r in rows if r[2] and r[2] >= 50]
    if fast:
        print("sites that raised the frame rate:")
        for label, speed, pr, npick, _l in fast:
            verdict = "KEEP (speed ok)" if abs(speed - 1.0) < 0.06 else \
                      "affects speed too"
            print(f"  {label:12} {pr:5.1f} fps  {speed:.2f}x  -> {verdict}")
    else:
        print("no single redirect raised the frame rate (fps needs the overlay "
              "injected to be measured at all)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

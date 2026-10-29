"""Sweep the (logicFPS, updateFPS, budget) triples and measure fps and speed.

The pacer turned out to derive its frame time rather than use a constant:

    0x6687BA  ecx = logicFPS
    0x6687D4  ecx = (updateFPS * 10) / logicFPS      ; target, x10 fixed point
    0x6687F7  [edi+0x17c] -> smoothed to ecx / 10     ; = updateFPS / logicFPS
    0x668722  budget = 66.6667 / ([edi+0x17c] * [0xcb4644])

so frame time is driven by the *ratio* updateFPS/logicFPS, while the tick spreading
lives elsewhere (0x655140 with its own division by 6). Measured points:

    split                fps    speed
    logic15 upd30        30     1.00x     ratio 2
    logic15 upd30 budget/2 60    2.00x
    logic15 upd60        60     0.67x     ratio 4

The first two say ticks scale with fps; the third contradicts a pure fps/ratio law,
so the relationship has to be measured rather than derived. Ratios below 6 take a
different branch at 0x655175 (`cmp edi,6`), which is the likely culprit.

Each config is applied, checked for fps and clock, then reverted to the given
baseline so configs cannot combine.

Usage:
    python sweep_timing.py
"""
from __future__ import annotations

import struct
import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from sweep_fps import frames_after, LOG

BUDGET = 0x8E0DBC
LOGIC = 0x8B4098
UPDATE = 0x8B409C

ORIG = {
    BUDGET: struct.pack("<f", 66.6666666),
    LOGIC: struct.pack("<I", 15),
    UPDATE: struct.pack("<I", 30),
}

# (label, logicFPS, updateFPS, budget constant as 66.667/n)
CONFIGS = [
    # frame_ms = budget_const / R (measured), so R=3 needs the original budget to
    # land on 45fps. At 45fps, 1/3 ticks/frame is exactly the original 15 ticks/s:
    # same simulation rate as vanilla, only the render rate is higher.
    ("R3 45fps 15tick", 15, 45, 66.6666666),
    ("R2 30fps (vanilla timings)", 15, 30, 66.6666666),
]


def measure(t, base, write, secs: float):
    off = LOG.stat().st_size if LOG.exists() else 0
    s0 = A.read_sim_seconds(t, base)
    if s0 is None:
        return None, None
    t0 = time.perf_counter()
    time.sleep(secs)
    s1 = A.read_sim_seconds(t, base)
    real = time.perf_counter() - t0
    pr, _es, _n = frames_after(off)
    if s1 is None:
        return None, None
    return pr, (s1 - s0) / real


def main() -> int:
    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    h = found[0]
    k32, handle, write, werr = A.open_writer(h["pid"])
    if not handle:
        return 2
    t = p.Target(h["pid"])
    try:
        base, _notes = p.resolve_base(t, t.modules())
        print(f"pid={h['pid']}  base={base:#010x}\n")

        # Baseline first so the sweep has a reference in the same session.
        for rva, orig in ORIG.items():
            write(base + rva, orig)
        time.sleep(1.0)
        pr, sp = measure(t, base, write, 6.0)
        print(f"{'baseline logic15 upd30':38} fps={pr if pr else 0:6.1f} "
              f"speed={sp if sp else 0:5.2f}x")
        print()

        print(f"{'config':38} {'fps':>7} {'speed':>8}")
        results = []
        for label, logic, upd, budget in CONFIGS:
            write(base + BUDGET, struct.pack("<f", budget))
            write(base + LOGIC, struct.pack("<I", logic))
            write(base + UPDATE, struct.pack("<I", upd))
            time.sleep(1.2)
            pr, sp = measure(t, base, write, 6.0)
            results.append((label, pr, sp))
            print(f"{label:38} {pr if pr else 0:7.1f} {sp if sp else 0:7.2f}x")
            # revert between configs so nothing carries over
            for rva, orig in ORIG.items():
                write(base + rva, orig)
            time.sleep(0.8)

        print()
        best = [r for r in results if r[2] and abs(r[2] - 1.0) < 0.12
                and r[1] and r[1] > 55]
        if best:
            print(f"[+] 60fps at 1.00x achieved by: {best[0][0]}")
        else:
            print("[i] no config yet gives 60fps at 1.00x; see the columns above")
        return 0
    finally:
        t.close()
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())

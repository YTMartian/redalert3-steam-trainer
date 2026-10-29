"""Map ticks-per-frame against the pacer ratio, all held at 60fps.

The frame time is `budget_constant / R` where R = updateFPS/logicFPS (0x6687D4), so
holding R * 16.667 as the budget constant keeps every configuration at 60fps and
lets R be the only variable. Speed can then be read as ticks per frame:

    ticks/second = fps * ticks_per_frame   ->   15/s at 60fps is 0.25 ticks/frame

Measured so far, sampling only the clock: R=2 gives 0.50 ticks/frame (2.00x at
60fps) and R=4,5,6 all give 0.167 (0.67x). That is not 1/R, so the ratio alone does
not explain it; ticks/frame has to be mapped rather than derived.

Here the clock and the Present counter are sampled together over one interval, so
ticks/frame comes directly from concurrent deltas instead of from an assumed 15/s.
R=3 is the interesting gap: 0.25 would be exactly 1.00x.

Usage:
    python map_tick_pacing.py
"""
from __future__ import annotations

import struct
import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from sweep_fps import FRAME, LOG

BUDGET = 0x8E0DBC
LOGIC = 0x8B4098
UPDATE = 0x8B409C
PERIOD = 16.6666666      # ms; budget constant that pairs with R=1 for 60fps

ORIG = {
    BUDGET: struct.pack("<f", 66.6666666),
    LOGIC: struct.pack("<I", 15),
    UPDATE: struct.pack("<I", 30),
}


def sample(t, base, secs: float):
    """Return (fps, ticks_per_second, ticks_per_frame).

    The overlay logs `frames ... Present=N` every 200 frames, and `Present` only
    advances in steps, so pairing a log delta with an unrelated clock read is wrong
    at short windows. Instead every new Present step is recorded together with the
    clock read at that moment (and the log's own timestamp), and the deltas are
    taken between the first and last step. That keeps frames and clock aligned.
    """
    off = LOG.stat().st_size if LOG.exists() else 0
    rows = []
    last_present = None
    deadline = time.perf_counter() + secs
    while time.perf_counter() < deadline:
        time.sleep(0.12)
        if not LOG.exists():
            continue
        with LOG.open("r", encoding="utf-8", errors="replace") as f:
            f.seek(off)
            chunk = f.read()
            off = f.tell()
        for line in chunk.splitlines():
            m = FRAME.search(line)
            if not m:
                continue
            present = int(m.group(6))
            if present == last_present:
                continue
            clock = A.read_sim_seconds(t, base)
            if clock is None:
                return None, None, None
            hh, mm, ss = (int(g) for g in m.groups()[:3])
            ms = int(m.group(4))
            rows.append((hh * 3600 + mm * 60 + ss + ms / 1000.0, present, clock))
            last_present = present

    if len(rows) < 3:
        return None, None, None
    t0, p0, c0 = rows[0]
    t1, p1, c1 = rows[-1]
    span = t1 - t0
    frames = p1 - p0
    if span <= 0 or frames <= 0:
        return None, None, None
    return frames / span, (c1 - c0) / span, (c1 - c0) / frames


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
        print(f"pid={h['pid']}  base={base:#010x}")
        print("target: 60 fps with 0.25 ticks/frame (15 ticks/s = 1.00x)\n")

        for rva, orig in ORIG.items():
            write(base + rva, orig)
        time.sleep(1.0)
        fps, tps, tpf = sample(t, base, 6.0)
        if fps:
            print(f"{'baseline (30fps)':24} fps={fps:6.1f} "
                  f"ticks/s={tps:6.2f} ticks/frame={tpf:5.3f}")

        print(f"\n{'R=upd/logic':16} {'logic':>6} {'upd':>5} {'fps':>7} "
              f"{'ticks/s':>8} {'ticks/frm':>10} {'speed':>7}")
        for R in (2, 3, 4, 5, 6):
            logic, upd = 15, 15 * R
            budget_const = PERIOD * R
            try:
                write(base + BUDGET, struct.pack("<f", budget_const))
                write(base + LOGIC, struct.pack("<I", logic))
                write(base + UPDATE, struct.pack("<I", upd))
                time.sleep(1.2)
                fps, tps, tpf = sample(t, base, 6.0)
            finally:
                # revert before reporting, so a failed sample cannot leave a config
                for rva, orig in ORIG.items():
                    write(base + rva, orig)
            if fps is None:
                print(f"{R:<16} {logic:>6} {upd:>5}   sample failed")
                time.sleep(0.8)
                continue
            print(f"{R:<16} {logic:>6} {upd:>5} {fps:>7.1f} {tps:>8.2f} "
                  f"{tpf:>10.3f} {tps / 15.0:>6.2f}x")
            time.sleep(0.8)

        print()
        print("0.25 ticks/frame at 60fps is the goal. If no R reaches it, the tick")
        print("spread is quantised (0x655140 divides by 6), and the remaining option")
        print("is to keep 30fps pacing and make only the renderer run at 60.")
        return 0
    finally:
        t.close()
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())

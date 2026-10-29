"""Run the four patch states back to back and report speed + FPS for each.

The 60fps port renders at 60 but runs the simulation at 2.01x. The patchset is
two independent groups, so measuring each one on its own says exactly which
group is responsible, without touching RayaTrainer's Hook 41:

    revert   no patches      -> the baseline to compare against
    code     .text only      -> address redirects + trampoline cave
    logic    .data only      -> logicFPS and the tick-length constants
    full     both            -> the state that misbehaves

Speed is read from the battle clock (simulated seconds per real second, 1.0 is
normal). FPS is read back out of the overlay log's frame counters, which keeps
the verdict objective instead of a matter of feel.

The overlay's own 60fps toggle must be OFF: with it ON, tick() re-applies the
full set every half second and every measurement becomes "full".

Usage:
    python ab_test.py [seconds_per_state]
"""
from __future__ import annotations

import re
import sys
import time
from datetime import datetime
from pathlib import Path

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

LOG = Path(__file__).resolve().parent.parent / "bin" / "ra3_overlay.log"
LINE = re.compile(
    r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]\s+"
    r"frames EndScene=(-?\d+) Present=(-?\d+)")

STATES = [
    ("revert", False, False),
    ("code", False, True),
    ("logic", True, False),
    ("full", True, True),
]


def fps_since(offset: int):
    """Average presented FPS from frame-counter lines written after `offset`."""
    if not LOG.exists():
        return None, None
    with LOG.open("r", encoding="utf-8", errors="replace") as f:
        f.seek(offset)
        chunk = f.read()
    samples = []
    for line in chunk.splitlines():
        m = LINE.match(line.strip())
        if not m:
            continue
        hh, mm, ss, ms = (int(g) for g in m.groups()[:4])
        samples.append((datetime(2000, 1, 1, hh, mm, ss, ms * 1000),
                        int(m.group(5)), int(m.group(6))))
    if len(samples) < 2:
        return None, None
    rates = []
    for a, b in zip(samples, samples[1:]):
        dt = (b[0] - a[0]).total_seconds()
        # The counter is only logged every 300 frames, so at 30fps two lines are
        # ~10s apart. Filtering on a "gap looks swallowed" threshold would throw
        # away exactly the slow cases we want to measure.
        if 0 < dt <= 60.0:
            rates.append((b[2] - a[2]) / dt)
    if not rates:
        return None, None
    return sum(rates) / len(rates), (samples[-1][1] - samples[0][1]) / max(
        1e-6, (samples[-1][0] - samples[0][0]).total_seconds())


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
    rows = []
    base = 0
    try:
        base, _notes = p.resolve_base(t, t.modules())
        data, code = A.load_sites()

        if A.read_sim_seconds(t, base) is None:
            print("[!] not in a match; start a skirmish and rerun")
            return 1

        # Guard: if the overlay's 60fps toggle is ON, its tick() restores the
        # full set within ~500ms and every measurement would just read "full".
        fails = A.apply_state(write, base, data, code, False, False)
        if fails:
            print(f"[!] cannot write to the target at all "
                  f"({len(fails)} failures, e.g. {fails[0]}, err={werr()})")
            return 1
        time.sleep(1.5)
        on_d, on_c = A.applied_counts(t, base, data, code)
        if on_d or on_c:
            print("[!] something keeps re-applying the patches "
                  f"({on_d} logic + {on_c} code sites came back).")
            print("[!] Turn the overlay's 60fps toggle OFF "
                  "(杂项操作 -> 解锁60帧), then rerun.")
            return 1

        print(f"pid={h['pid']}  base={base:#010x}  "
              f"{len(data)} logic + {len(code)} code sites")
        print(f"each state is held {secs:.0f}s\n")

        for name, wl, wc in STATES:
            fails = A.apply_state(write, base, data, code, wl, wc)
            if fails:
                print(f"  [!] {name}: {len(fails)} write(s) failed "
                      f"(text={fails[0]}, err={werr()})")
            time.sleep(0.5)  # let the engine pick the new constants up
            on_d, on_c = A.applied_counts(t, base, data, code)
            if (on_c if wc else 0) != (len(code) if wc else 0):
                print(f"  [!] {name}: code did not all stick "
                      f"({on_c}/{len(code)})")
            offset = LOG.stat().st_size if LOG.exists() else 0
            sim0 = A.read_sim_seconds(t, base)
            real0 = time.perf_counter()
            time.sleep(secs)
            sim1 = A.read_sim_seconds(t, base)
            real = time.perf_counter() - real0
            if sim0 is None or sim1 is None:
                print(f"  [!] {name}: left the match mid-sample")
                rows.append((name, None, None))
                continue
            speed = (sim1 - sim0) / real
            present, endscene = fps_since(offset)
            rows.append((name, speed, present))
            print(f"  {name:7} speed {speed:5.2f}x   "
                  f"Present {present if present is None else round(present, 1)} fps"
                  f"   EndScene {endscene if endscene is None else round(endscene, 1)} fps")
    finally:
        # Never leave experimental bytes in the process.
        try:
            if base:
                A.apply_state(write, base, data, code, False, False)
                print("\neverything reverted to the original bytes")
        except Exception:
            pass
        t.close()
        k32.CloseHandle(handle)

    print()
    print(f"{'state':8} {'speed':>8} {'Present fps':>12}")
    for name, speed, present in rows:
        sp = "-" if speed is None else f"{speed:.2f}x"
        pr = "-" if present is None else f"{present:.1f}"
        print(f"{name:8} {sp:>8} {pr:>12}")
    print()
    print("Reading it:")
    print("  a state with speed ~1.0x but Present ~60 means 60fps at normal")
    print("  game speed; whichever group that is, is the one to keep.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

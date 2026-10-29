"""Compute FPS from the overlay log's frame counter lines.

d3d9_hook logs `frames EndScene=.. Present=.. SwapPresent=..` whenever the
running total crosses a multiple of 300, and game_api::log prefixes each line
with a wall-clock stamp, so consecutive lines give an exact frame rate:

    fps = delta(frames) / delta(time)

That makes the 60fps experiments measurable instead of a matter of feel.
Segments are separated by a gap in the log (mode switches, pauses) so each
experiment window reports on its own.

Usage:
    python fps_report.py [log_path] [--tail N]
"""
from __future__ import annotations

import re
import sys
from datetime import datetime
from pathlib import Path

DEFAULT = (Path(__file__).resolve().parent.parent / "bin" / "ra3_overlay.log")
LINE = re.compile(
    r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]\s+"
    r"frames EndScene=(-?\d+) Present=(-?\d+) SwapPresent=(-?\d+)")
# Segments are split when the sample-to-sample gap exceeds this many seconds.
# The frame counter is logged every 300 frames, so at 30fps consecutive lines are
# already ~10s apart; a smaller threshold would split every segment into singles.
GAP_S = 25.0


def main() -> int:
    argv = sys.argv[1:]
    tail = 200
    path = DEFAULT
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--tail":
            i += 1
            if i < len(argv):
                tail = int(argv[i])
        elif a.startswith("--tail="):
            tail = int(a.split("=", 1)[1])
        else:
            path = Path(a)
        i += 1
    if not path.exists():
        print(f"[!] no log at {path}")
        return 2

    samples = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = LINE.match(line.strip())
        if not m:
            continue
        hh, mm, ss, ms = (int(g) for g in m.groups()[:4])
        base = datetime(2000, 1, 1, hh, mm, ss, ms * 1000)
        samples.append((base, int(m.group(5)), int(m.group(6))))
    if len(samples) < 2:
        print("[!] fewer than two frame samples; play a match a little longer")
        return 1

    samples = samples[-tail:]
    print(f"{path}")
    print(f"{len(samples)} frame samples\n")

    seg = []
    prev = None
    for cur in samples:
        if prev:
            dt = (cur[0] - prev[0]).total_seconds()
            if dt > GAP_S:
                flush(seg)
                seg = []
                continue
            if dt <= 0:
                prev = cur
                continue
            seg.append((cur[0], (cur[1] - prev[1]) / dt, (cur[2] - prev[2]) / dt))
        prev = cur
    flush(seg)
    return 0


def flush(seg) -> None:
    if not seg:
        return
    es = [s[1] for s in seg]
    pr = [s[2] for s in seg]
    span = (seg[-1][0] - seg[0][0]).total_seconds()
    print(f"{seg[0][0].strftime('%H:%M:%S')} .. "
          f"{seg[-1][0].strftime('%H:%M:%S')}  ({span:.1f}s)")
    print(f"  EndScene   avg {sum(es)/len(es):6.1f} fps"
          f"   min {min(es):6.1f}   max {max(es):6.1f}")
    print(f"  Present    avg {sum(pr)/len(pr):6.1f} fps"
          f"   min {min(pr):6.1f}   max {max(pr):6.1f}")
    print()


if __name__ == "__main__":
    sys.exit(main())

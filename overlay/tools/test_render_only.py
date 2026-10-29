"""Test whether splitting the 60fps patch into render-only constants is enough.

The unlock currently applies eight .data constants and they come in two families:

    render side   d4 0x8E0DF0  30.0    -> 60.0      (update fps)
                  d5 0x8E0DF4  33.333  -> 16.667    (1000/30 -> 1000/60)
                  d7 0x8E0ED4  0.0333  -> 0.01667   (1/30 -> 1/60)

    logic side    d0 0x8B4098  15      -> 30        (logicFPS)
                  d1 0x8E0DEC  0.015   -> 0.03      (1/66.667)
                  d2 0x8E0DBC  66.667  -> 33.333    (1000/15)
                  d3 0x8E0DF8  15.0    -> 30.0
                  d6 0x8E0E34  0.0667  -> 0.0333    (1/15)

Both families are doubled together, which doubles the logic rate as well as the
frame rate. If the render side alone is enough to reach 60fps, then leaving the
logic side at its original 15Hz should give 60fps at 1.00x with no code hook --
which is the whole goal, achieved with constants only.

Each case applies the full .text patch set (that is what raises the frame rate)
and then a chosen family of constants, and reports simulated speed and Present fps
so the two halves of the result are visible together.

Usage:
    python test_render_only.py [seconds]
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

CAVE_UPDATE_FPS = 0x7C6448

RENDER = {0x8E0DF0, 0x8E0DF4, 0x8E0ED4}
LOGIC = {0x8B4098, 0x8E0DEC, 0x8E0DBC, 0x8E0DF8, 0x8E0E34}

# name -> set of constant RVAs to patch
CASES = [
    ("code only (no data)", set()),
    ("code + render", set(RENDER)),
    ("code + render + d1", set(RENDER) | {0x8E0DEC}),
    ("code + render + d3", set(RENDER) | {0x8E0DF8}),
    ("code + render + d1,d3", set(RENDER) | {0x8E0DEC, 0x8E0DF8}),
    ("code + all data", set(RENDER) | set(LOGIC)),
]


def main() -> int:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 9.0

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
        data_by_rva = {rva: (o, n) for rva, o, n in data}

        if A.read_sim_seconds(t, base) is None:
            print("[!] not in a match; start a skirmish and rerun")
            return 1

        print(f"pid={h['pid']}  base={base:#010x}  {secs:.0f}s per case")
        print("full .text patch set + cave updateFPS=60 in every case\n")
        print(f"{'case':26} {'speed':>7} {'fps':>6}")

        def revert_all():
            for rva, o, _n in reversed(code):
                write(base + rva, o)
            for rva, o, _n in reversed(data):
                write(base + rva, o)

        for name, subset in CASES:
            revert_all()
            # Constants first: no redirect may point at a half-written trampoline.
            for rva in sorted(subset):
                o_n = data_by_rva.get(rva)
                if o_n:
                    write(base + rva, o_n[1])
            for rva, _o, n in code:
                write(base + rva, n)
            # The cave's own updateFPS copy is what the redirects read.
            write(base + CAVE_UPDATE_FPS, struct.pack("<i", 60))
            time.sleep(0.9)

            off = LOG.stat().st_size if LOG.exists() else 0
            s0 = A.read_sim_seconds(t, base)
            t0 = time.perf_counter()
            time.sleep(secs)
            s1 = A.read_sim_seconds(t, base)
            real = time.perf_counter() - t0
            pr, _es, n_s = frames_after(off)
            if s0 is None or s1 is None:
                print(f"{name:26}   left the match mid-sample")
                continue
            speed = (s1 - s0) / real
            rows.append((name, speed, pr))
            ftxt = "   n/a" if pr is None else f"{pr:6.1f}"
            flag = ""
            if pr and abs(speed - 1.0) < 0.06 and pr >= 55:
                flag = "   <== GOAL"
            print(f"{name:26} {speed:>6.2f}x {ftxt}{flag}")
    finally:
        try:
            if base:
                for rva, o, _n in reversed(code):
                    write(base + rva, o)
                for rva, o, _n in reversed(data):
                    write(base + rva, o)
                print("\nreverted to original bytes")
        except Exception:
            pass
        t.close()
        k32.CloseHandle(handle)

    winners = [r for r in rows if r[2] and abs(r[1] - 1.0) < 0.06 and r[2] >= 55]
    print()
    if winners:
        for name, speed, pr in winners:
            print(f"60fps at normal speed: {name}  ({speed:.2f}x, {pr:.1f}fps)")
    else:
        print("render-only constants were not enough on their own")
    return 0


if __name__ == "__main__":
    sys.exit(main())

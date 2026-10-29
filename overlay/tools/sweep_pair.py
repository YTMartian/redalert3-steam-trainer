"""Find a subset of redirect sites that keeps 60fps at normal game speed.

Clean-environment measurements (overlay only, no trainer) show the redirects are
not interchangeable:

    none                 1.01x   30 fps
    0x2686f9 alone       2.00x   60 fps     <- raises the frame rate
    all nine             0.67x   60 fps     <- and overshoots the logic rate

So 0x2686F9 by itself is the frame-rate switch, and the other eight drag simulated
time down past 1.00x. The target value lies between them, which means some subset
of those eight, combined with 0x2686F9, should land on 60fps at 1.00x -- no hook
required.

Strategy: measure each of the other eight on top of 0x2686F9 to get its marginal
effect, then test the combination whose product of effects is closest to halving
2.00x. Each marginal is also reported on its own so the arithmetic is checkable.

Usage:
    python sweep_pair.py [seconds]
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
FPS_SITE = 0x2686F9

# 0x22A756 is a 9-byte blob holding two pointers; a 3-byte prefix write leaves it
# inconsistent, so it is excluded from combination search and reported separately.
UNRELIABLE = {0x22A756}


def others():
    return [r for r in sorted(UPD_RVAS) if r != FPS_SITE]


def run(t, write, base, data, code, sites, picks, secs, logic_fps_value):
    A.apply_state(write, base, data, code, False, False)
    write(base + CAVE_RVA, sites[CAVE_RVA][1])
    write(base + CAVE_UPDATE_FPS, struct.pack("<i", 60))
    write(base + LOGIC_FPS, struct.pack("<i", logic_fps_value))
    for rva in picks:
        write(base + rva, sites[rva][1])
    time.sleep(0.8)
    off = LOG.stat().st_size if LOG.exists() else 0
    s0 = A.read_sim_seconds(t, base)
    t0 = time.perf_counter()
    time.sleep(secs)
    s1 = A.read_sim_seconds(t, base)
    real = time.perf_counter() - t0
    pr, _es, n = frames_after(off)
    if s0 is None or s1 is None:
        return None, pr, n
    return (s1 - s0) / real, pr, n


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

        print(f"pid={h['pid']}  base={base:#010x}  {secs:.0f}s per run")
        print(f"anchor site {FPS_SITE:#x} (the one that raises fps)\n")

        alone, fps_alone, _n = run(t, write, base, data, code, sites,
                                   [FPS_SITE], secs, 15)
        if alone is None:
            print("[!] could not sample; not in a match?")
            return 1
        print(f"  {FPS_SITE:#x} alone          speed {alone:5.2f}x   "
              f"Present {'n/a' if fps_alone is None else f'{fps_alone:5.1f}'}")
        rows.append((f"{FPS_SITE:#x}", alone, fps_alone, 1.0))

        marg = []
        for rva in others():
            sp, pr, _n = run(t, write, base, data, code, sites,
                             [FPS_SITE, rva], secs, 15)
            if sp is None:
                print(f"  {FPS_SITE:#x}+{rva:#x:<9} left the match")
                continue
            ratio = sp / alone if alone else float("nan")
            marg.append((rva, sp, pr, ratio))
            note = "" if rva not in UNRELIABLE else "  (unreliable blob)"
            tag = f"{FPS_SITE:#x}+{rva:#x}"
            print(f"  {tag:18} speed {sp:5.2f}x   "
                  f"Present {'n/a' if pr is None else f'{pr:5.1f}'}"
                  f"   x{ratio:4.2f} vs alone{note}")
            rows.append((f"{FPS_SITE:#x}+{rva:#x}", sp, pr, ratio))
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
    print("marginal multipliers (how much each site scales the 60fps speed):")
    for rva, sp, pr, ratio in [(r[0], r[1], r[2], r[3]) for r in rows[1:]]:
        print(f"  {rva}  {sp:.2f}x   factor {ratio:.3f}")
    print()
    print("next: pick the sites whose product is closest to 0.5 "
          f"(2.00x -> 1.00x) and test that subset with "
          "sweep_subset.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())

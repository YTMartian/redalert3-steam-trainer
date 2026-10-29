"""Find the data constant that cancels the 0.67x introduced by updateFPS 30->60.

`upd` alone gives 60fps but 0.67x. The logicFPS constant alone gives 2.00x. So the
.data constants and updateFPS clearly interact, but not by simple multiplication:
full (both) is 2.00x, not 0.67 x 2.00.

What is known (measured):

    none  1.00x     upd  0.67x     logic(all 8 data)  2.00x     full  2.00x

So `upd` stops mattering once the data constants are all applied, which means the
tick rate is derived from updateFPS *and* the data constants together. The aim is
the combination that lands on 1.00x while updateFPS stays at 60, because that is
60fps rendering at normal game speed.

This holds `upd` on and sweeps the eight .data constants, singly and in the pairs
that look coupled by value (0.0667/0.0333 and friends), and reports speed.

Usage:
    python sweep_data.py [seconds]
"""
from __future__ import annotations

import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from split_code import C3_RVA, CAVE_RVA, JMP_RVA, UPD_RVAS

# Order matches apply_subset.DATA_SITES.
NAMES = [
    "logicFPS 15->30",
    "0.015->0.03",
    "66.667->33.333",
    "15.0->30.0",
    "30.0->60.0",
    "33.333->16.667",
    "0.0667->0.0333",
    "0.0333->0.01667",
]

DEFAULT_COMBOS = [
    ("", []),
    ("d0", [0]),
    ("d1", [1]),
    ("d2", [2]),
    ("d3", [3]),
    ("d4", [4]),
    ("d5", [5]),
    ("d6", [6]),
    ("d7", [7]),
    ("d6+d7", [6, 7]),
    ("d0+d6", [0, 6]),
    ("d0+d6+d7", [0, 6, 7]),
    ("all", list(range(8))),
]


def parse_combos(argv):
    if not argv:
        return DEFAULT_COMBOS
    out = []
    for token in argv:
        idx = [int(t) for t in token.replace("d", "").split("+") if t]
        out.append((token, sorted(set(idx))))
    return out


def main() -> int:
    secs = 5.0
    args = sys.argv[1:]
    if args and not args[0].startswith("d") and args[0].replace("+", "").isdigit():
        secs = float(args[0])
        args = args[1:]
    combos = parse_combos(args)

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

        print(f"pid={h['pid']}  base={base:#010x}   holding upd on, "
              f"{secs:.0f}s per combo\n")

        for label, picks in combos:
            # Clean slate: original bytes everywhere.
            A.apply_state(write, base, data, code, False, False)
            # Cave must exist before any redirect points into it.
            write(base + CAVE_RVA, sites[CAVE_RVA][1])
            for rva in UPD_RVAS:
                write(base + rva, sites[rva][1])
            for i in picks:
                rva, _o, n = data[i]
                write(base + rva, n)

            time.sleep(0.6)
            landed = sum(1 for rva in UPD_RVAS
                         if t.read(base + rva, 3) == sites[rva][1])
            d_ok = sum(1 for i in picks
                       if t.read(base + data[i][0], len(data[i][2]))
                       == data[i][2])
            s0 = A.read_sim_seconds(t, base)
            t0 = time.perf_counter()
            time.sleep(secs)
            s1 = A.read_sim_seconds(t, base)
            real = time.perf_counter() - t0
            if s0 is None or s1 is None:
                print(f"  {label or 'upd only':10} left the match mid-sample")
                continue
            speed = (s1 - s0) / real
            rows.append((label or "upd only", speed))
            detail = "" if not picks else f"  data {d_ok}/{len(picks)}"
            print(f"  {label or 'upd only':10} speed {speed:5.2f}x"
                  f"   upd {landed}/9{detail}")
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
    print(f"{'combo':12} {'speed':>8}  data constant(s)")
    for label, speed in rows:
        picks = dict(DEFAULT_COMBOS).get(label, [])
        desc = ", ".join(NAMES[i] for i in picks) or "(none)"
        print(f"{label:12} {speed:>7.2f}x  {desc}")
    best = min(rows, key=lambda r: abs(r[1] - 1.0)) if rows else None
    if best:
        print()
        print(f"closest to normal speed with 60fps rendering: "
              f"{best[0]} at {best[1]:.2f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())

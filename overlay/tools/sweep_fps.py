"""Measure game speed and presented frame rate together for patch combinations.

The frame counter is logged every 300 frames and carries the overlay's menu flag,
so a rate must be derived from the counter deltas over the sample window and never
from adjacent lines alone:

    present_fps = (last.Present - first.Present) / elapsed

Known measurements on the 1.12 build:

    upd only            0.67x    30 fps     <- updateFPS redirected to 60
    upd + d0            1.00x    30 fps     <- correct speed, still 30 fps
    all (full patch)    2.00x    60 fps     <- what the user reported

So speed and frame rate are controlled by different constants and a combination
exists that lands on 1.00x at 60 fps. This sweeps candidates and reports both,
because a config that fixes only one of the two is not a fix.

Usage:
    python sweep_fps.py [seconds] [combo ...]      combo example: upd+d0+d4
"""
from __future__ import annotations

import re
import sys
import time
from pathlib import Path

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from split_code import C3_RVA, CAVE_RVA, JMP_RVA, UPD_RVAS

LOG = Path(__file__).resolve().parent.parent / "bin" / "ra3_overlay.log"
FRAME = re.compile(
    r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]\s+"
    r"frames EndScene=(\d+) Present=(\d+).*?menu=(\d)")
NAMES = [
    "logicFPS 15->30", "0.015->0.03", "66.667->33.333", "15.0->30.0",
    "30.0->60.0", "33.333->16.667", "0.0667->0.0333", "0.0333->0.01667",
]
DEFAULT = ["upd", "upd+d0", "upd+d4", "upd+d0+d4", "upd+d0+d3",
           "upd+d0+d3+d4", "upd+d0+d4+d7", "upd+d0+d4+d6+d7", "all"]


def frames_after(offset: int):
    """(present_fps, endscene_fps, n_samples) from counter lines after offset."""
    if not LOG.exists():
        return None, None, 0
    with LOG.open("r", encoding="utf-8", errors="replace") as f:
        f.seek(offset)
        chunk = f.read()
    rows = []
    for line in chunk.splitlines():
        m = FRAME.search(line)
        if not m:
            continue
        hh, mm, ss, ms = (int(g) for g in m.groups()[:4])
        rows.append(((hh * 3600 + mm * 60 + ss) + ms / 1000.0,
                     int(m.group(5)), int(m.group(6)), int(m.group(7))))
    if len(rows) < 2:
        return None, None, len(rows)
    span = rows[-1][0] - rows[0][0]
    if span <= 0:
        return None, None, len(rows)
    return ((rows[-1][2] - rows[0][2]) / span,
            (rows[-1][1] - rows[0][1]) / span,
            len(rows))


def parse(combo: str):
    c = combo.lower()
    if c == "all":
        return True, list(range(8)), {}
    toks = [t for t in c.split("+") if t]
    upd = "upd" in toks
    picks = sorted({int(t[1:]) for t in toks if re.fullmatch(r"d[0-7]", t)})
    extra = {"c3": "c3" in toks, "jmp": "jmp" in toks}
    return upd, picks, extra


def main() -> int:
    secs = 12.0
    args = sys.argv[1:]
    if args:
        try:
            secs = float(args[0])
            args = args[1:]
        except ValueError:
            pass
    combos = args or DEFAULT

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

        print(f"pid={h['pid']}  base={base:#010x}  {secs:.0f}s per combo\n")
        for combo in combos:
            upd, picks, extra = parse(combo)

            A.apply_state(write, base, data, code, False, False)
            write(base + CAVE_RVA, sites[CAVE_RVA][1])
            if upd:
                for rva in UPD_RVAS:
                    write(base + rva, sites[rva][1])
            if extra.get("c3"):
                write(base + C3_RVA, sites[C3_RVA][1])
            if extra.get("jmp"):
                write(base + JMP_RVA, sites[JMP_RVA][1])
            for i in picks:
                rva, _o, n = data[i]
                write(base + rva, n)

            time.sleep(0.8)
            off = LOG.stat().st_size if LOG.exists() else 0
            s0 = A.read_sim_seconds(t, base)
            t0 = time.perf_counter()
            time.sleep(secs)
            s1 = A.read_sim_seconds(t, base)
            real = time.perf_counter() - t0
            pr, es, n = frames_after(off)
            if s0 is None or s1 is None:
                print(f"  {combo:18} left the match mid-sample")
                continue
            speed = (s1 - s0) / real
            rows.append((combo, speed, pr, n))
            fps_txt = "  n/a" if pr is None else f"{pr:5.1f}"
            print(f"  {combo:18} speed {speed:4.2f}x   Present {fps_txt} fps"
                  f"   ({n} samples)")
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
    print(f"{'combo':20} {'speed':>7} {'fps':>6}")
    for combo, speed, pr, _n in rows:
        ftxt = "-" if pr is None else f"{pr:.0f}"
        print(f"{combo:20} {speed:>6.2f}x {ftxt:>6}")
    good = [r for r in rows if r[2] and abs(r[1] - 1.0) < 0.05
            and r[2] >= 50]
    print()
    if good:
        print("meets both goals (1.00x speed, >=50fps):",
              ", ".join(r[0] for r in good))
    else:
        print("no combo met both goals; the closest are listed above")
    return 0


if __name__ == "__main__":
    sys.exit(main())

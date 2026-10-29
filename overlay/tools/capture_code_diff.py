"""Capture the code patches that actually gate RayaTrainer's 60fps.

A full-image diff drowns in timer noise, but code does not churn: .text only
changes when someone patches it. This snapshots just the engine's .text plus the
scratch area RayaTrainer appears to use for its trampolines/copies, before and
after flipping its toggle, so the result is a short, meaningful list.

Static analysis of the trainer already suggested at least two code sites:
  0x2FFDAC  pointer 0x00CE0DFC -> 0x00BC644C   (redirect to its own copy)
  0x31F457  movss xmm1,[edx+0xF0] -> jmp 0x00BCA420
This confirms them and finds any others.

Usage:
    python capture_code_diff.py            # RayaTrainer toggle ON first
    python capture_code_diff.py --reverse  # toggle OFF first
"""
from __future__ import annotations

import json
import struct
import sys
import time

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

# Engine .text is RVA 0x1000..; 0x6C0000 is comfortably past its end while the
# later data sections stay out of the snapshot.
TEXT_START = 0x1000
TEXT_LEN = 0x6C0000 - 0x1000

# The scratch/cave window the pointer swap points into.
CAVE_START = 0x7B0000
CAVE_LEN = 0x22000


def pause(msg: str, fallback: int = 25) -> None:
    if sys.stdin is not None and sys.stdin.isatty():
        try:
            input(msg)
            return
        except EOFError:
            pass
    print(msg.rstrip() + f" (no input; auto-continuing in {fallback}s)")
    for left in range(fallback, 0, -1):
        print(f"\r  {left}s ...", end="", flush=True)
        time.sleep(1.0)
    print("\r  go        ")


def snap(t: "p.Target", base: int) -> dict:
    return {
        "text": t.read_committed(base + TEXT_START, TEXT_LEN, chunk=0x20000),
        "cave": t.read_committed(base + CAVE_START, CAVE_LEN, chunk=0x10000),
    }


def diff_runs(a: bytes, b: bytes, merge_gap: int = 8):
    out = []
    i = 0
    n = min(len(a), len(b))
    while i < n:
        if a[i] == b[i]:
            i += 1
            continue
        start = i
        last = i
        while i < n and i - last < merge_gap:
            if a[i] != b[i]:
                last = i
            i += 1
        out.append((start, a[start:last + 1], b[start:last + 1]))
    return out


def main() -> int:
    rev = "--reverse" in sys.argv
    la, lb = ("ON", "OFF") if rev else ("OFF", "ON")
    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    h = found[0]
    t = p.Target(h["pid"])
    try:
        mods = t.modules()
        base, notes = p.resolve_base(t, mods)
        if not base:
            print("base unresolved:", notes)
            return 2
        print(f"pid={h['pid']} ({h['name']})  base={base:#010x}")

        print()
        print(f"=== step 1/2: RayaTrainer 60fps should be {la} ===")
        pause(f"press Enter to snapshot .text while {la}... ")
        a = snap(t, base)

        print()
        print(f"=== step 2/2: flip RayaTrainer's 60fps to {lb}, then confirm ===")
        pause(f"press Enter to snapshot .text while {lb}... ")
        b = snap(t, base)

        report = []
        print()
        total = 0
        for region, start_rva in (("text", TEXT_START), ("cave", CAVE_START)):
            runs = diff_runs(a[region], b[region])
            if not runs:
                print(f"## {region}: no changes")
                continue
            print(f"## {region}: {len(runs)} changed region(s)  [{la} -> {lb}]")
            for off, br, ar in runs:
                total += 1
                rva = start_rva + off
                print(f"   RVA {rva:#08x}  len={len(br)}")
                print(f"      {la:<5} {br.hex(' ')}")
                print(f"      {lb:<5} {ar.hex(' ')}")
                report.append({
                    "region": region,
                    "rva": f"{rva:08X}",
                    "length": len(br),
                    f"before_{la.lower()}": br.hex(),
                    f"after_{lb.lower()}": ar.hex(),
                })

        print()
        if total == 0:
            print("[!] No code changed. Either the toggle does not touch .text on")
            print("[!] this build, or the flip did not take effect.")
            return 4

        out = f"code_patchset_{la.lower()}_to_{lb.lower()}.json"
        with open(out, "w", encoding="utf-8") as f:
            json.dump({"direction": f"{la} -> {lb}", "patches": report},
                      f, indent=2)
        print(f"[+] {total} code region(s) changed")
        print(f"[i] wrote {out} -- send this back.")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

"""Measure each part of the code patchset on its own.

The code group splits into three independent mechanisms, and the whole group
lands at 0.67x, so one of them is doing something other than raising the frame
rate:

    cave   0x7C6420  48 bytes in .text padding; holds the host for the values
                      the redirects point at. Always written: it is only data
                      and nothing jumps into it unless the jmp site is on.
    upd    the 8 pointer rewrites 0x8B409C -> 0x7C6448 (updateFPS 30 -> 60)
    c3     0x2FFDAC: 0x8E0DFC -> 0x7C644C (a 0.03 float)
    jmp    0x31F457: `movss xmm1,[edx+0xF0]` -> cave, which multiplies by the
           float at 0x7C6440 before jumping back

Never applies a redirect without the cave, because the targets it points at live
inside that padding and would otherwise read zeros.

Usage:
    python split_code.py [seconds] [combo ...]
    combos: upd c3 jmp  (default: none upd c3 jmp upd+c3 upd+jmp c3+jmp all)
"""
from __future__ import annotations

import struct
import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

CAVE_RVA = 0x7C6420
UPD_RVAS = {0x17FF0A, 0x22A756, 0x23EA21, 0x255167, 0x2686F9,
            0x7B40A8, 0x7B40D8, 0x7B4108, 0x7B4C29}
C3_RVA = 0x2FFDAC
JMP_RVA = 0x31F457

DEFAULT_COMBOS = ["none", "upd", "c3", "jmp", "upd+c3", "upd+jmp",
                  "c3+jmp", "all"]


def parts(combo: str) -> tuple[bool, bool, bool]:
    c = combo.lower()
    if c == "all":
        return True, True, True
    if c == "none":
        return False, False, False
    toks = {t.strip() for t in c.split("+") if t.strip()}
    unknown = toks - {"upd", "c3", "jmp"}
    if unknown:
        raise ValueError(f"unknown part(s): {sorted(unknown)}")
    return "upd" in toks, "c3" in toks, "jmp" in toks


def main() -> int:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
    combos = sys.argv[2:] or DEFAULT_COMBOS

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
        cave = sites[CAVE_RVA]

        if A.read_sim_seconds(t, base) is None:
            print("[!] not in a match; start a skirmish and rerun")
            return 1

        print(f"pid={h['pid']}  base={base:#010x}  {secs:.0f}s per combo\n")
        for combo in combos:
            want_upd, want_c3, want_jmp = parts(combo)

            # Clean slate, then the cave, then only the requested redirects.
            A.apply_state(write, base, data, code, False, False)
            if not write(base + CAVE_RVA, cave[1]):
                print(f"  [!] cave write failed (err={werr()})")
                continue
            if want_upd:
                for rva in UPD_RVAS:
                    write(base + rva, sites[rva][1])
            if want_c3:
                write(base + C3_RVA, sites[C3_RVA][1])
            if want_jmp:
                write(base + JMP_RVA, sites[JMP_RVA][1])

            time.sleep(0.6)
            # Confirm the redirects really landed.
            landed = sum(1 for rva in UPD_RVAS
                         if t.read(base + rva, 3) == sites[rva][1])
            s0 = A.read_sim_seconds(t, base)
            t0 = time.perf_counter()
            time.sleep(secs)
            s1 = A.read_sim_seconds(t, base)
            real = time.perf_counter() - t0
            if s0 is None or s1 is None:
                print(f"  {combo:8} left the match mid-sample")
                continue
            speed = (s1 - s0) / real
            rows.append((combo, speed, landed))
            print(f"  {combo:8} speed {speed:5.2f}x   upd landed {landed}/9")
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
    print(f"{'combo':10} {'speed':>8}")
    for combo, speed, _l in rows:
        print(f"{combo:10} {speed:>7.2f}x")
    print()
    print("The part whose removal brings speed back to 1.00x is the suspect.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

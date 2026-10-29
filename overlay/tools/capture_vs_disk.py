"""Diff the running game against its own file to recover the complete patchset.

Comparing runtime memory with the on-disk image is far cleaner than diffing two
live snapshots: .text is read-only and identical to disk in a clean process, so
every difference is a genuine code patch -- including the trampoline RayaTrainer
builds in the zero gap between .text and .rdata (around 0x7C6448).

Ranges compared:
  * .text and the padding gap before .rdata   0x01000 .. 0x7C7000
  * the cave window inside .rdata             0x7C7000 .. 0x7D2000
.data is deliberately excluded: past its raw size it is BSS that the engine
initialises at runtime, so it always differs and would swamp the output. The
eight frame constants are sampled separately instead.

IMPORTANT: run with ONLY RayaTrainer attached and its 60fps ON. If our overlay
is injected it also patches code and its hooks would show up as noise.

Writes raya_full_patchset.json with every changed region and both byte strings,
which is enough to replay the patch byte-for-byte.
"""
from __future__ import annotations

import json
import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

TEXT_LO, TEXT_HI = 0x1000, 0x7C7000
CAVE_LO, CAVE_HI = 0x7C7000, 0x7D2000

# Known frame-rate constants, for a separate sanity read.
DATA_SITES = [0x8B4098, 0x8E0DEC, 0x8E0DBC, 0x8E0DF8,
              0x8E0DF0, 0x8E0DF4, 0x8E0E34, 0x8E0ED4]


def merge_runs(a: bytes, b: bytes, gap: int = 16):
    out = []
    i = 0
    n = min(len(a), len(b))
    while i < n:
        if a[i] == b[i]:
            i += 1
            continue
        start = i
        last = i
        while i < n and i - last < gap:
            if a[i] != b[i]:
                last = i
            i += 1
        out.append((start, a[start:last + 1], b[start:last + 1]))
    return out


def main() -> int:
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

        ours = [n for _b, _s, n in mods
                if "ra3_overlay" in n.lower() or "mustcode" in n.lower()]
        if ours:
            print()
            print(f"[!] our overlay is injected: {ours}")
            print("[!] Its own code hooks would pollute this diff.")
            print("[!] Restart the game, attach ONLY RayaTrainer with 60fps ON,")
            print("[!] then run this again.")
            return 3

        path = p.find_game_file()
        if not path:
            print("could not find ra3_1.12.game on disk")
            return 2
        print(f"disk image: {path}")

        import pefile
        pe = pefile.PE(path, fast_load=True)
        disk = open(path, "rb").read()
        secs = [(s.VirtualAddress, s.Misc_VirtualSize, s.SizeOfRawData,
                 s.PointerToRawData) for s in pe.sections]

        def disk_bytes(rva: int, n: int) -> bytes:
            for va, vs, rs, pr in secs:
                if va <= rva < va + max(vs, rs):
                    off = pr + (rva - va)
                    avail = pr + rs - off
                    if avail <= 0:
                        return b"\x00" * n
                    real = min(n, avail)
                    return disk[off:off + real] + b"\x00" * (n - real)
            return b"\x00" * n  # gap between sections / outside any section

        report = []
        for lo, hi, label in ((TEXT_LO, TEXT_HI, "text+gap"),
                              (CAVE_LO, CAVE_HI, "rdata-cave")):
            size = hi - lo
            live = t.read_committed(base + lo, size, chunk=0x20000)
            shadow = b"".join(
                disk_bytes(lo + o, min(0x10000, size - o))
                for o in range(0, size, 0x10000))
            runs = merge_runs(shadow, live)
            # .rdata is full of pointers that the loader relocates, so it
            # differs from disk even in a pristine process. A genuine code cave
            # is carved out of zero padding, so only keep runs whose disk side
            # is all zero there.
            if label == "rdata-cave":
                runs = [r for r in runs if all(b == 0 for b in r[1])]
            print()
            print(f"== {label}: {len(runs)} changed region(s) ==")
            for off, before, after in runs:
                rva = lo + off
                print(f"  RVA {rva:#08x}  len={len(after)}")
                print(f"      disk {before[:32].hex(' ')}"
                      + (" ..." if len(before) > 32 else ""))
                print(f"      live {after[:32].hex(' ')}"
                      + (" ..." if len(after) > 32 else ""))
                report.append({"region": label, "rva": f"{rva:08X}",
                               "length": len(before),
                               "disk": before.hex(), "live": after.hex()})

        print()
        print("== frame constants (should be the ON values) ==")
        for rva in DATA_SITES:
            raw = t.read(base + rva, 4)
            print(f"  {rva:#08x} = {raw.hex(' ') if raw else 'unreadable'}")

        print()
        print(f"{len(report)} patch region(s) total")
        out = "raya_full_patchset.json"
        with open(out, "w", encoding="utf-8") as f:
            json.dump({"base": base, "patches": report}, f, indent=2)
        print(f"[i] wrote {out}")
        print("[i] Send that file back -- it is the complete replay list.")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

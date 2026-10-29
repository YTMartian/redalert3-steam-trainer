"""Statically find RayaTrainer's hook dispatch table inside its own DLL.

The generated stubs call a dispatcher with (context, hook id). With 40+ hooks a
compiler almost always emits a jump table, i.e. `jmp dword ptr [eax*4 + table]`,
which encodes as:

    FF 24 85 <disp32>

24 = ModRM(mod=00, reg=100 = /4, rm=100 = SIB)
85 = SIB(scale=10 /*4*/, index=000 /*eax*/, base=101 /*disp32 follows*/)

Locating that table gives the handler for any id, including 41, without needing
the game running.
"""
from __future__ import annotations

import struct
import sys

DLL = (r"C:\Users\YTMartian\Desktop\RayaTrainer-v0.9.0-win-x86-self-contained"
       r"\RayaTrainer.Agent.dll")
TARGET_ID = 0x29   # 41


def main() -> int:
    import pefile
    pe = pefile.PE(DLL, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = open(DLL, "rb").read()

    secs = {}
    for s in pe.sections:
        name = s.Name.rstrip(b"\x00").decode("latin1")
        secs[name] = (s.VirtualAddress, s.Misc_VirtualSize,
                      s.SizeOfRawData, s.PointerToRawData)

    def rva_to_off(rva):
        for va, vs, rs, pr in secs.values():
            if va <= rva < va + max(vs, rs):
                if pr == 0:
                    return None
                return pr + (rva - va)
        return None

    def read_u32_va(va):
        off = rva_to_off(va - base)
        if off is None or off + 4 > len(data):
            return None
        return struct.unpack("<I", data[off:off + 4])[0]

    text_va, text_vs, _rs, text_pr = secs[".text"]
    text = data[text_pr:text_pr + _rs]
    text_lo, text_hi = text_va, text_va + text_vs

    hits = []
    for i in range(len(text) - 7):
        if text[i] != 0xFF or text[i + 1] != 0x24 or text[i + 2] != 0x85:
            continue
        disp = struct.unpack("<I", text[i + 3:i + 7])[0]
        hits.append((text_va + i, disp))

    print(f"DLL base {base:#x}  .text {text_lo:#x}..{text_hi:#x}")
    print(f"jump-table candidates: {len(hits)}\n")

    best = None
    for site, table in hits:
        if not (0x100000 <= table < 0x300000):
            continue
        vals = []
        for n in range(512):
            v = read_u32_va(table + n * 4)
            if v is None or not (text_lo <= (v - base) < text_hi):
                break
            vals.append(v)
        if len(vals) < 8:
            continue
        h41 = vals[TARGET_ID] if len(vals) > TARGET_ID else None
        flag = ""
        if h41:
            flag = f"  handler[41]={h41:#010x}"
            if best is None or len(vals) > best[1]:
                best = (table, len(vals), vals)
        print(f"site {site:#08x}  table {table:#08x}  "
              f"entries={len(vals):<4}{flag}")

    if not best:
        print("\nno usable table found")
        return 1

    table, n, vals = best
    print(f"\nchosen table {table:#010x} with {n} entries")
    for idx in list(range(0, 3)) + list(range(38, min(n, 45))):
        print(f"  [{idx:2d}] {vals[idx]:#010x}")
    print(f"\nhook 41 handler = {vals[TARGET_ID]:#010x}")
    print(f"  file offset = {rva_to_off(vals[TARGET_ID] - base):#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

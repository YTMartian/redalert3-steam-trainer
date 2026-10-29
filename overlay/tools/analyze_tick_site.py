"""Identify the virtual call RayaTrainer hooks at 0x265560, and who calls it.

The hook site sits on a global vtable dispatch:

    0x66555a  mov ecx, [0xce08bc]      ; a singleton
    0x665560  mov eax, [ecx]           ; its vtable
    0x665562  mov edx, [eax+0x10c]     ; virtual method
    0x665568  call edx

Skipping a frame here only helps if that method is the thing that advances
simulated time while rendering happens elsewhere. So this resolves the actual
target at runtime, and finds the callers of the enclosing function to see how
often it runs (once per frame, or only on some frames).

Read-only; run it with the game in a match.
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

SINGLETON_RVA = 0x8E08BC
VT_SLOT = 0x10C
FUNC_RVA = 0x265550      # start of the function containing the hook site
SITE_RVA = 0x265560


def find_callers(t, base: int, mods, target_va: int):
    """Scan executable ranges for E8 rel32 / E9 rel32 landing on target_va."""
    hits = []
    for mbase, msize, name in mods:
        if not name.lower().endswith((".game", ".exe", ".dll")):
            continue
        blob = t.read(mbase, min(msize, 0x1000000))
        if not blob:
            continue
        for i in range(len(blob) - 5):
            op = blob[i]
            if op not in (0xE8, 0xE9):
                continue
            rel = struct.unpack_from("<i", blob, i + 1)[0]
            if (mbase + i + 5 + rel) & 0xFFFFFFFF == target_va:
                hits.append((name, mbase + i, op))
    return hits


def main() -> int:
    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    t = p.Target(found[0]["pid"])
    try:
        mods = t.modules()
        base, notes = p.resolve_base(t, mods)
        print(f"pid={found[0]['pid']} ({found[0]['name']})  base={base:#010x}")
        print(f"anchors: {notes}\n")

        def inside(va: int):
            for mbase, msize, name in mods:
                if mbase <= va < mbase + msize:
                    return name, va - mbase
            return None, None

        raw = t.read(base + SINGLETON_RVA, 4)
        if not raw:
            print(f"[!] cannot read singleton at {base + SINGLETON_RVA:#x}")
            return 1
        obj = struct.unpack("<I", raw)[0]
        print(f"singleton [0xce08bc] = {obj:#010x}  ({inside(obj)[0]})")
        vt = t.read(obj, 4)
        if not vt:
            print("[!] object has no readable vtable")
            return 1
        vtable = struct.unpack("<I", vt)[0]
        print(f"vtable = {vtable:#010x}  ({inside(vtable)[0]})")

        slot = t.read(vtable + VT_SLOT, 4)
        if not slot:
            print("[!] vtable slot unreadable")
            return 1
        method = struct.unpack("<I", slot)[0]
        mname, mrva = inside(method)
        print(f"slot +{VT_SLOT:#x} = {method:#010x}   module={mname} rva="
              f"{'?' if mrva is None else hex(mrva)}")

        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True

        if method:
            print(f"\n=== method {method:#010x} (first 40 instructions) ===")
            body = t.read(method, 0x400) or b""
            n = 0
            for ins in md.disasm(body, method):
                print(f"  {ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}")
                n += 1
                if n >= 40 or ins.mnemonic in ("ret", "ret 4", "int3"):
                    break

        print(f"\n=== callers of the enclosing function {base + FUNC_RVA:#010x} "
              f"and the hook site {base + SITE_RVA:#010x} ===")
        for label, target in (("function", base + FUNC_RVA),
                              ("site", base + SITE_RVA)):
            hits = find_callers(t, base, mods, target)
            print(f"  {label} {target:#010x}: {len(hits)} caller(s)")
            for name, at, op in hits[:10]:
                print(f"    {name} {at:#010x} (op {op:#x})")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

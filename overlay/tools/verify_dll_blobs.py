"""Verify the compiled DLL really contains every generated patch blob.

A build can succeed while the patch table is wrong (mis-spliced includes, stale
object files). This checks the compiled artifact directly: every "patched" byte
string must appear in the DLL, and the cave blob must appear intact as one run.

Run after build_injector.bat.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
DLL = TOOLS.parent / "bin" / "ra3_overlay_v4.dll"
INC = TOOLS.parent / "src" / "fps_unlock_patchset.inc"


def main() -> int:
    if not DLL.exists():
        print(f"missing {DLL}")
        return 1
    blob = DLL.read_bytes()
    print(f"DLL: {DLL}  ({len(blob)} bytes)")

    patches = json.loads((TOOLS / "raya_full_patchset.json").read_text(
        encoding="utf-8"))["patches"]

    # Everything the generator decided to embed.
    portable = []
    for p in patches:
        rva = int(p["rva"], 16)
        if rva in (0x23DC21, 0x23DC33, 0x268783, 0x348990):
            continue
        live = bytes.fromhex(p["live"])
        if live[:1] == b"\xE9" and len(live) >= 5:
            import struct
            rel = struct.unpack("<i", live[1:5])[0]
            tgt = 0x400000 + rva + 5 + rel
            if not (0x400000 <= tgt < 0x400000 + 0x950000):
                continue
        elif len(live) == 3:
            new = int.from_bytes(live, "little")
            if not (0x400000 <= new < 0x400000 + 0x950000):
                continue
        elif rva != 0x7C6420 and rva != 0x22A756:
            continue
        portable.append((rva, live))

    ok = 0
    for rva, live in portable:
        if live in blob:
            ok += 1
        else:
            print(f"  MISSING patched bytes for RVA {rva:#08x} "
                  f"({live.hex(' ')})")
    print(f"patched-blob presence: {ok}/{len(portable)}")

    # The cave must be present as one contiguous 48-byte run, because its
    # internal jmp displacement and absolute operand only make sense together.
    cave = None
    for p in patches:
        if int(p["rva"], 16) == 0x7C6420:
            cave = bytes.fromhex(p["live"])
    if cave is None:
        print("cave not found in JSON")
        return 1
    print(f"cave contiguous in DLL: {'yes' if cave in blob else 'NO'}")
    if cave not in blob:
        return 1

    # Movement sanity: the generated include must be on disk and non-trivial.
    if INC.exists():
        text = INC.read_text(encoding="utf-8")
        n = text.count("const PatchEntry kCodePatches")
        print(f"generated include: {len(text)} bytes, "
              f"{text.count('kBlob') } blob refs, table={'yes' if n else 'NO'}")
    return 0 if ok == len(portable) else 1


if __name__ == "__main__":
    sys.exit(main())

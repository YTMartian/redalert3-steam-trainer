"""Classify every recovered patch by where its JMP leads.

Replaying all 58 regions blind would crash the game: RayaTrainer routes some
hooks into memory it VirtualAlloc'd itself, so those targets only contain valid
code while the trainer is loaded. Only patches that stay inside the engine image
are portable to our overlay.

For each region this prints the JMP target (or the absolute address it swaps)
and buckets it:
  IMAGE   target inside 0x400000..0xD50000  -> portable
  EXTERNAL target elsewhere (trainer heap)  -> NOT portable
  DATA    not a jump, an in-place operand   -> portable if it names image memory
"""
from __future__ import annotations

import json
import struct
import sys

BASE = 0x400000
IMG_LO, IMG_HI = 0x400000, 0x400000 + 0x950000

IMAGE_SECTIONS = [
    (0x1000, 0x7C541A, ".text"),
    (0x7C7000, 0x8AA1CA, ".rdata"),
    (0x8AB000, 0x930BA0, ".data"),
    (0x931000, 0x93101D, ".tls"),
    (0x932000, 0x932020, "rts.ver"),
    (0x933000, 0x94F598, ".rsrc"),
]


def section(rva: int) -> str:
    for lo, size, name in IMAGE_SECTIONS:
        if lo <= rva < lo + size:
            return name
    if 0x7C541A <= rva < 0x7C7000:
        return ".text-pad"
    return "-"


def classify_target(t: int) -> str:
    if IMG_LO <= t < IMG_HI:
        return f"IMAGE {section(t - BASE)}"
    return "EXTERNAL(trained-owned)"


def main() -> int:
    d = json.load(open("raya_full_patchset.json", encoding="utf-8"))
    patches = d["patches"]
    print(f"{len(patches)} regions\n")

    portable, external, data_ops = [], [], []
    for p in patches:
        rva = int(p["rva"], 16)
        live = bytes.fromhex(p["live"])
        disk = bytes.fromhex(p["disk"])

        if live[:1] == b"\xE9" and len(live) >= 5:
            rel = struct.unpack("<i", live[1:5])[0]
            tgt = BASE + rva + 5 + rel
            kind = classify_target(tgt)
            row = (rva, f"JMP -> {tgt:#010x}  {kind}", live, disk)
            if kind.startswith("IMAGE"):
                portable.append(row)
            else:
                external.append(row)
        elif len(live) == 3 and len(disk) == 3:
            # 3-byte absolute-address operand swap (little endian, high byte 00)
            old = int.from_bytes(disk, "little")
            new = int.from_bytes(live, "little")
            row = (rva, f"PTR {old:#010x} -> {new:#010x}  "
                        f"[{section(old - BASE)} -> {section(new - BASE)}]",
                   live, disk)
            if IMG_LO <= new < IMG_HI:
                portable.append(row)
            else:
                external.append(row)
        else:
            data_ops.append((rva, f"INPLACE {disk.hex(' ')} -> {live.hex(' ')}",
                             live, disk))

    for title, rows in (("PORTABLE: JMP/PTR into the engine image", portable),
                        ("NOT PORTABLE: hooks into trainer-allocated memory",
                         external),
                        ("IN-PLACE operand/byte edits", data_ops)):
        print(f"===== {title}: {len(rows)} =====")
        for rva, desc, live, disk in rows:
            print(f"  {rva:#08x}  {desc}")
        print()

    print("Summary: "
          f"portable={len(portable)} external={len(external)} "
          f"inplace={len(data_ops)}")

    # The trampoline cave is the one IMAGE-internal region that is code we must
    # copy verbatim. Show it decoded.
    print()
    print("===== trampoline cave 0x7C6420 =====")
    for p in patches:
        if int(p["rva"], 16) == 0x7C6420:
            live = bytes.fromhex(p["live"])
            print(f"  len={len(live)}")
            for i in range(0, len(live), 16):
                print(f"    {0x7C6420 + i:#08x}  {live[i:i+16].hex(' ')}")
            code = live[:21]
            print(f"  code: movss xmm1,[edx+0xF0] / mulss xmm1,[0xBC6440] / jmp")
            if len(live) >= 32:
                for off in (0x20, 0x24, 0x28, 0x2C):
                    if off + 4 <= len(live):
                        f = struct.unpack("<f", live[off:off + 4])[0]
                        print(f"    float @ RVA {0x7C6420 + off:#08x} = "
                              f"{f:.6g}  ({live[off:off+4].hex(' ')})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

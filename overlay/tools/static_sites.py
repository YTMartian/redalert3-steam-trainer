"""Disassemble patch sites straight from the on-disk engine binary.

Every site in the patch set turns out to be a read of a timing global being
redirected to a copy inside the cave:

    original  9c 40 cb  ->  [0x00CB409C] = updateFPS   (int)
    patched   48 64 bc  ->  [0x00BC6448] = cave copy of updateFPS

so what each site actually computes is decided by the instruction that carries
that operand. The site that divides simulated time by 3 is the interesting one:
if the divisor is encoded in the instruction, it can be re-encoded as 2 and
2.00x / 2 lands exactly on 1.00x at 60fps.

Doing this offline from the PE image means it works with the game closed, and it
never touches a running process. RVAs are read through the section table, so
frames and imports are not confused with loadable data.

Usage:
    python static_sites.py [rva ...]
"""
from __future__ import annotations

import struct
import sys

BINARY = r"C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game"
IMAGE_BASE = 0x00400000

SITES = {
    0x17FF0A: "redirect",
    0x22A756: "redirect (pointer blob)",
    0x23EA21: "redirect",
    0x255167: "divide by 3  <<< the knob",
    0x2686F9: "raises fps to 60 (2.00x)",
    0x2FFDAC: "reads the 0.03 frame budget",
    0x31F457: "scales by 0.25",
    0x7B40A8: "redirect",
    0x7B40D8: "redirect",
    0x7B4108: "redirect",
    0x7B4C29: "redirect",
    0x7C6420: "cave",
    0x7C6448: "cave updateFPS",
    0x8B4098: "logicFPS",
    0x8B409C: "updateFPS",
}


class PE:
    def __init__(self, path: str):
        self.blob = open(path, "rb").read()
        if self.blob[:2] != b"MZ":
            raise SystemExit("[!] not an MZ image")
        pe_off = struct.unpack_from("<I", self.blob, 0x3C)[0]
        if self.blob[pe_off:pe_off + 4] != b"PE\0\0":
            raise SystemExit("[!] not a PE image")
        nsec = struct.unpack_from("<H", self.blob, pe_off + 6)[0]
        opt_size = struct.unpack_from("<H", self.blob, pe_off + 20)[0]
        self.base = struct.unpack_from("<I", self.blob, pe_off + 24 + 28)[0]
        sec_off = pe_off + 24 + opt_size
        self.sections = []
        for i in range(nsec):
            o = sec_off + i * 40
            name = self.blob[o:o + 8].rstrip(b"\0").decode("latin-1")
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII",
                                                            self.blob, o + 8)
            self.sections.append((name, va, vsize, rawptr, rawsize))

    def rva_to_off(self, rva: int):
        for _name, va, vsize, rawptr, rawsize in self.sections:
            if va <= rva < va + max(vsize, rawsize):
                delta = rva - va
                if delta < rawsize:
                    return rawptr + delta
        return None

    def read(self, rva: int, n: int) -> bytes:
        off = self.rva_to_off(rva)
        if off is None:
            return b""
        return self.blob[off:off + n]

    def describe(self):
        print(f"image base {self.base:#010x}  sections:")
        for name, va, vsize, rawptr, rawsize in self.sections:
            mark = "  <== .text" if name == ".text" else ""
            print(f"  {name:8} va={va:#08x} vsize={vsize:#08x} "
                  f"raw={rawsize:#08x}{mark}")


def main() -> int:
    pe = PE(BINARY)
    print(f"{BINARY}  ({len(pe.blob)} bytes)")
    pe.describe()
    print()

    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    wanted = [int(a, 0) for a in sys.argv[1:]] or list(SITES)
    for rva in wanted:
        label = SITES.get(rva, "?")
        va = pe.base + rva
        site = pe.read(rva, 8)
        if not site:
            print(f"=== {label} @ {va:#010x}: not in a mapped section\n")
            continue
        back = 0x30
        ctx = pe.read(rva - back, back + 0x40)
        print(f"=== {label}  @ {va:#010x} (file ok, {len(site)} bytes) ===")
        print(f"  bytes at site: {site.hex(' ')}")
        for ins in md.disasm(ctx, va - back):
            if ins.address > va + 0x24:
                break
            mark = "   <<< SITE" if ins.address <= va < ins.address + ins.size else ""
            print(f"    {ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}{mark}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

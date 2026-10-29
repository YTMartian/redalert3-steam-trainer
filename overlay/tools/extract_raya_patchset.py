"""Extract every patch triple RayaTrainer builds, including .text sites.

The builder at RVA 0x5A340 constructs each entry the same way:

    mov dword ptr [eax], <original>     ; allocate + fill the "before" buffer
    mov dword ptr [eax], <patched>      ; fill the "after" buffer
    push <game RVA>
    call 0x10058A20                     ; assemble one record

So walking the listing and, for each call to the assembler, looking back for the
push and the immediate stores recovers (rva, original bytes, patched bytes)
without guessing. Sizes above 4 bytes repeat the store pattern, so runs of
stores are collected in order.

Run: python dump_raya_patchset.py --extract
"""
from __future__ import annotations

import struct
import sys

DLL = (r"C:\Users\YTMartian\Desktop\RayaTrainer-v0.9.0-win-x86-self-contained"
       r"\RayaTrainer.Agent.dll")
BUILDER_RVA = 0x5A340
ASSEMBLER_VA = 0x10058A20


def load():
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32

    pe = pefile.PE(DLL, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = open(DLL, "rb").read()
    secs = []
    for s in pe.sections:
        secs.append((s.VirtualAddress, s.Misc_VirtualSize,
                     s.SizeOfRawData, s.PointerToRawData))

    def rva_to_off(rva):
        for va, vs, rs, pr in secs:
            if va <= rva < va + max(vs, rs):
                return pr + (rva - va)
        return None

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    return pe, base, data, secs, rva_to_off, md


def disasm_function(md, data, base, rva, off, limit=0x4000):
    """Yield instructions until we've clearly left the function."""
    chunk = data[off:off + limit]
    out = []
    last = base + rva
    for ins in md.disasm(chunk, base + rva):
        out.append(ins)
        last = ins.address
        if ins.mnemonic == "ret" and len(out) > 40:
            break
        if ins.mnemonic == "int3":
            break
    return out


def imm_of(ins, n=0):
    """n-th immediate operand, or None."""
    seen = 0
    for op in ins.operands:
        if op.type == 2:  # X86_OP_IMM
            if seen == n:
                return op.imm & 0xFFFFFFFF
            seen += 1
    return None


def mem_disp(ins):
    """Displacement of dword ptr [reg + disp] style stores, or None."""
    if len(ins.operands) < 1:
        return None
    dst = ins.operands[0]
    if dst.type != 3:  # X86_OP_MEM
        return None
    return dst.mem.disp & 0xFFFFFFFF


def main() -> int:
    pe, base, data, secs, rva_to_off, md = load()
    off = rva_to_off(BUILDER_RVA)
    if off is None:
        print("builder not found")
        return 1
    ins_list = disasm_function(md, data, base, BUILDER_RVA, off)
    print(f"builder: {len(ins_list)} instructions, "
          f"{ins_list[0].address:#x} .. {ins_list[-1].address:#x}")

    entries = []
    # Walk: remember recent immediate stores, then on `push <rva>` + `call
    # assembler`, take the stores in order as original[] then patched[].
    pending_stores: list[int] = []
    pending_rva: int | None = None

    for ins in ins_list:
        mn = ins.mnemonic
        if mn == "mov" and "dword ptr" in ins.op_str and "]," in ins.op_str:
            v = imm_of(ins)
            if v is not None:
                pending_stores.append(v)
                continue
        if mn == "push":
            v = imm_of(ins)
            if v is not None and 0x1000 <= v <= 0x950000:
                pending_rva = v
                continue
        if mn == "call":
            v = imm_of(ins)
            if v == ASSEMBLER_VA and pending_rva is not None and pending_stores:
                # Even count: first half original, second half patched.
                stores = pending_stores
                half = len(stores) // 2
                if half >= 1 and len(stores) % 2 == 0:
                    entries.append((pending_rva, stores[:half], stores[half:]))
                pending_rva = None
                pending_stores = []
                continue
            # Any other call resets the accumulator for that site
            if pending_rva is not None:
                pending_rva = None
                pending_stores = []
            continue

    print()
    print(f"== {len(entries)} patch entries recovered ==")
    seen = set()
    for rva, before, after in entries:
        key = (rva, tuple(before), tuple(after))
        if key in seen:
            continue
        seen.add(key)
        b = b"".join(struct.pack("<I", x) for x in before)
        a = b"".join(struct.pack("<I", x) for x in after)
        kind = ".text" if rva < 0x6C0000 else (".data" if rva >= 0x8AB000 else "?")
        print(f"  RVA {rva:#08x} {kind:<6} len={len(b)}")
        print(f"      before {b.hex(' ')}")
        print(f"      after  {a.hex(' ')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

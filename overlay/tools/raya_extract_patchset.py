"""Extract RayaTrainer's frame-rate patch records from the agent DLL.

Each record is built by 0x1005A340 as: <old buffer><new buffer><rva>.  The
builder always constructs the ORIGINAL bytes first and the PATCHED bytes
second (verified on the logicFPS record: 0xF=15 first, 0x1E=30 second), so
pulling the immediates in stream order yields (old, new) pairs per site.

Emits a C++-ready table and a JSON dump.
"""
import json
import sys

import capstone
import pefile

DLL = r"C:\Users\YTMartian\Desktop\RayaTrainer-v0.9.0-win-x86-self-contained\RayaTrainer.Agent.dll"
BUILDER = 0x5A340
PATCH_FN = 0x10058A20
IMAGE_BASE = 0x10000000

# Local slots that ferry a value into the new buffer (not SEH state, not args).
FERRY_SLOTS = {-0x14, -0x18, -0x1C, -0x20, -0x2C}

SKIP_DEST = {-0x24, -0x4, -0x30, -0x28, -0x50, -0x6C}


def load(path):
    pe = pefile.PE(path, fast_load=True)
    secs = []
    for s in pe.sections:
        secs.append((s.Name.rstrip(b"\x00").decode("latin1"), s.VirtualAddress,
                     s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData))
    return pe, secs


def rva_bytes(pe, secs, rva, size):
    for _n, va, vsize, raw, rawsize in secs:
        if va <= rva < va + max(vsize, rawsize):
            off = rva - va
            return pe.__data__[raw + off: raw + off + size]
    return b""


def main():
    pe, secs = load(DLL)
    code = rva_bytes(pe, secs, BUILDER, 0x6000)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    records = []
    pending = []   # values since last rva
    pending_rva = None

    for insn in md.disasm(code, IMAGE_BASE + BUILDER):
        ops = insn.operands
        # mov <mem>, imm32  -> a value definition
        if insn.mnemonic == "mov" and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_MEM \
                and ops[1].type == capstone.x86.X86_OP_IMM:
            imm = ops[1].imm & 0xFFFFFFFF
            mem = ops[0].mem
            disp = mem.disp
            if mem.base == capstone.x86.X86_REG_EBP:
                if disp in SKIP_DEST:
                    continue
                if disp in FERRY_SLOTS:
                    if imm:
                        pending.append(imm)
                    continue
                continue
            # store into an allocated buffer, e.g. [eax], [ecx]
            if imm:
                pending.append(imm)
        elif insn.mnemonic == "push" and len(ops) == 1 and ops[0].type == capstone.x86.X86_OP_IMM:
            imm = ops[0].imm & 0xFFFFFFFF
            if imm < 0x1000000:            # an RVA, not an address
                pending_rva = imm
        elif insn.mnemonic == "call" and ops and ops[0].type == capstone.x86.X86_OP_IMM:
            target = ops[0].imm & 0xFFFFFFFF
            if target == PATCH_FN and pending_rva is not None:
                vals = pending[-2:] if len(pending) >= 2 else pending
                if len(vals) == 2:
                    records.append({"rva": pending_rva, "old": vals[0], "new": vals[1]})
                else:
                    records.append({"rva": pending_rva, "values": vals})
                pending = []
                pending_rva = None
        if insn.mnemonic == "ret":
            # function epilogue reached
            if insn.address > IMAGE_BASE + BUILDER + 0x100:
                break

    print(f"records: {len(records)}\n")
    for r in records:
        if "old" in r:
            print(f"RVA {r['rva']:08X}  old={r['old']:08X}  new={r['new']:08X}")
        else:
            print(f"RVA {r['rva']:08X}  values={[hex(v) for v in r['values']]}")

    with open("raya_patchset.json", "w", encoding="utf-8") as f:
        json.dump(records, f, indent=2)
    print("\nwrote raya_patchset.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())

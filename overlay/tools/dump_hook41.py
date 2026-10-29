"""Follow RayaTrainer's Hook 41 all the way to its handler, from the live process.

The chain is followed by pointer only, never guessed:

  0x265560            the jmp RayaTrainer installs
    -> stub           pushfd/pushal, push ctx+id, call dispatcher
      -> 0x600927b0   fxsave wrapper, then call the real dispatcher
        -> core       indexes (id -> handler) through a table
          -> handler  the code that reconciles simulation time with 60 ticks/s

Two indirections are expected before the table, so calls are followed
transitively (depth 3) instead of assuming the first dispatcher holds it.
Both table shapes are supported:

    movzx eax, byte ptr [reg + map]   ; id -> packed index
    jmp dword ptr [eax*4 + table]     ; packed index -> handler

Requires: game in a match, RayaTrainer attached, 60fps ON. Read-only.
"""
from __future__ import annotations

import struct
import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

SITE1_RVA = 0x265555
SITE2_RVA = 0x265560
TARGET_ID = 0x29


def md32():
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    return md


def dis_function(dump: bytes, addr: int, limit: int = 200, indent: str = "    "):
    """Disassemble one function, stopping at its first return.

    Disassembling a fixed byte window runs straight past `ret` into whatever
    function happens to follow, which pollutes the listing and invents call
    targets that belong to unrelated code.
    """
    out = []
    md = md32()
    for i, ins in enumerate(md.disasm(dump, addr)):
        if i >= limit:
            break
        out.append((ins, f"{indent}{ins.address:#010x}  "
                         f"{ins.mnemonic:<7}{ins.op_str}"))
        if ins.mnemonic == "ret" or ins.mnemonic == "int3":
            break
    return out


def dis(dump: bytes, addr: int, limit: int = 120, indent: str = "    "):
    return dis_function(dump, addr, limit, indent)


def jmp_target(t, addr):
    raw = t.read(addr, 8)
    if not raw or raw[0] != 0xE9:
        return None
    return addr + 5 + struct.unpack("<i", raw[1:5])[0]


def call_targets(ins_list):
    """Resolve call destinations, including `mov reg, imm32; call reg`.

    RayaTrainer's stubs use indirect calls (`mov eax, imm32; call eax`), so
    looking only for an immediate operand on the call instruction finds nothing.
    Register-held immediates are tracked instead.
    """
    out = []
    reg_imm: dict[str, int] = {}
    for ins, _line in ins_list:
        if ins.mnemonic == "mov" and len(ins.operands) == 2:
            dst, src = ins.operands
            if dst.type == 1 and src.type == 2:  # reg <- imm
                name = ins.reg_name(dst.reg)
                reg_imm[name] = src.imm & 0xFFFFFFFF
                continue
        if ins.mnemonic in ("mov", "lea"):
            # writing a register by other means invalidates the cached value
            if ins.operands and ins.operands[0].type == 1:
                reg_imm.pop(ins.reg_name(ins.operands[0].reg), None)
        if ins.mnemonic == "call":
            if ins.operands and ins.operands[0].type == 2:
                out.append(ins.operands[0].imm & 0xFFFFFFFF)
            elif ins.operands and ins.operands[0].type == 1:
                name = ins.reg_name(ins.operands[0].reg)
                if name in reg_imm:
                    out.append(reg_imm[name])
    return [v for v in out if v > 0x10000]


def find_indirection(dump: bytes, addr: int):
    """Return (byte_map_va, table_va, direct_index_base) from a dispatcher body.

    Collects a byte/word map (`movzx/movsx eax, byte ptr [reg + disp]`) and a
    scaled jump table (`jmp dword ptr [reg*4 + disp]`).
    """
    byte_map = None
    table = None
    for ins, _line in dis(dump, addr, 400, ""):
        txt = ins.op_str
        if ins.mnemonic in ("movzx", "movsx") and "byte ptr" in txt:
            if " + " in txt:
                try:
                    v = int(txt.split(" + ")[-1].rstrip("]"), 16)
                    if 0x10000000 <= v < 0x11000000:
                        byte_map = v
                except ValueError:
                    pass
        if ins.mnemonic == "jmp" and "ptr" in txt and "*4" in txt:
            if " + " in txt:
                try:
                    v = int(txt.split(" + ")[-1].rstrip("]"), 16)
                    if 0x10000000 <= v < 0x11000000:
                        table = v
                except ValueError:
                    pass
    return byte_map, table


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
        print(f"pid={h['pid']} ({h['name']})  base={base:#010x}")
        print(f"anchors: {notes}")

        # --- stub ----------------------------------------------------------
        stub = None
        for rva in (SITE2_RVA, SITE1_RVA):
            tgt = jmp_target(t, base + rva)
            if tgt:
                stub = tgt
                print(f"\nstub from 0x{rva:X} -> {stub:#010x}")
                break
        if not stub:
            print("[!] no jmp at either site: turn RayaTrainer's 60fps ON")
            return 3
        code = t.read(stub, 96)
        for _i, line in dis(code, stub, 16):
            print(line)

        hook_id = None
        stub_list = dis(code, stub, 16, "")
        for ins, _l in stub_list:
            if ins.mnemonic == "push" and ins.operands:
                op = ins.operands[0]
                if op.type == 2 and 0 <= (op.imm & 0xFFFFFFFF) < 0x200:
                    hook_id = (op.imm & 0xFFFFFFFF)
        print(f"hook id = {hook_id} (expect {TARGET_ID})")

        # --- follow calls until a table shows up ---------------------------
        queue = call_targets(stub_list)

        seen = set()
        depth = 0
        table = None
        byte_map = None
        owner = None
        while queue and depth < 4 and table is None:
            addr = queue.pop(0)
            if addr in seen:
                continue
            seen.add(addr)
            body = t.read(addr, 1024)
            if not body:
                continue
            print()
            print(f"== dispatcher candidate {addr:#010x} ==")
            for _i, line in dis(body, addr, 60):
                print(line)
            bm, tb = find_indirection(body, addr)
            if tb:
                table, byte_map, owner = tb, bm, addr
                break
            # no table yet: follow the calls inside it
            for v in call_targets(dis(body, addr, 200, "")):
                if v not in seen:
                    queue.append(v)
            depth += 1

        if not table:
            print()
            print("[!] no `jmp dword ptr [reg*4+disp]` found in 4 hops.")
            print("[!] The dispatch may be a chain of compares instead.")
            print("[!] Send this output back -- the listing shows the real shape.")
            return 4

        print()
        print(f"jump table {table:#010x}   byte map "
              f"{f'{byte_map:#010x}' if byte_map else 'none'}   "
              f"(found in {owner:#010x})")

        def entry(idx: int):
            raw = t.read(table + idx * 4, 4)
            return struct.unpack("<I", raw)[0] if raw else None

        cands = []
        direct = entry(TARGET_ID)
        if direct:
            cands.append(("direct index", direct))
        if byte_map:
            bm = t.read(byte_map + TARGET_ID, 1)
            if bm:
                packed = bm[0]
                v = entry(packed)
                if v:
                    cands.append((f"byte-map[{TARGET_ID}]={packed}", v))

        for how, addr in cands:
            inside = any(bb <= addr < bb + ss for bb, ss, _n in mods)
            print()
            print(f"===== handler ({how}) {addr:#010x} "
                  f"[{'engine image' if inside else 'trainer memory'}] =====")
            hc = t.read(addr, 768)
            if not hc:
                print("  unreadable")
                continue
            for _i, line in dis(hc, addr, 150):
                print(line)
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

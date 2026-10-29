"""Disassemble every patch site, before and after, with surrounding context.

The measured effect of the divide site (0x255167) is that it turns the 60fps
patch's 2.00x into 0.67x -- a factor of 3. If that factor is an integer encoded in
the instruction itself (a shift, a divisor, or a compare against 2), then it can
be re-encoded as 2 instead of 3, and 2.00x / 2 = 1.00x at 60fps, which is the
target. That makes the exact encoding of the site the whole question.

This dumps each site's original bytes, the replacement bytes, and the decoded
form of both, plus the instructions immediately before and after so the changed
operand can be read in context. Static analysis only: nothing is written to the
game, so it is safe to run in any state.

Usage:
    python dump_sites.py [rva ...]
"""
from __future__ import annotations

import sys

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

SITES = {
    0x17FF0A: "redirect",
    0x22A756: "redirect (pointer blob)",
    0x23EA21: "redirect",
    0x255167: "divide by 3  <<< the knob",
    0x2686F9: "raises fps to 60 (2.00x)",
    0x7B40A8: "redirect",
    0x7B40D8: "redirect",
    0x7B4108: "redirect",
    0x7B4C29: "redirect",
    0x7C6420: "cave",
    0x7C6448: "cave updateFPS",
    0x8B4098: "logicFPS",
    0x8B409C: "updateFPS",
}


def main() -> int:
    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    t = p.Target(found[0]["pid"])
    try:
        base, notes = p.resolve_base(t, t.modules())
        print(f"pid={found[0]['pid']}  base={base:#010x}  {notes}\n")

        import apply_subset as A
        data, code = A.load_sites()
        site_map = {rva: (o, n) for rva, o, n in code}

        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True

        wanted = [int(a, 0) for a in sys.argv[1:]] or list(SITES)
        for rva in wanted:
            label = SITES.get(rva, "?")
            if rva not in site_map:
                orig = t.read(base + rva, 16) or b""
                print(f"=== {label} @ {base + rva:#010x} (no patch defined) ===")
                print(f"  bytes: {orig.hex(' ')}\n")
                continue
            o, n = site_map[rva]
            addr = base + rva
            back, fwd = 0x24, 0x20
            ctx_o = t.read(addr - back, back + len(o) + fwd) or b""
            ctx_n = bytearray(ctx_o)
            ctx_n[back:back + len(n)] = n

            print(f"=== {label}  @ {addr:#010x} ({len(o)} bytes) ===")
            print(f"  original: {o.hex(' ')}")
            print(f"  patched : {n.hex(' ')}")
            for title, blob in (("original", bytes(ctx_o)), ("patched", bytes(ctx_n))):
                print(f"  -- {title} --")
                for ins in md.disasm(blob, addr - back):
                    if ins.address > addr + fwd:
                        break
                    mark = ""
                    if ins.address <= addr < ins.address + ins.size:
                        mark = "   <<< SITE"
                    print(f"    {ins.address:#010x}  {ins.mnemonic:<7}"
                          f"{ins.op_str}{mark}")
            print()

        if 0x255167 in wanted or not sys.argv[1:]:
            print("note: what divides timing by 3 is what we want at 2 instead")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

"""Find every caller of the battle-logic tick, and see which one runs per frame.

The catch-up loop at 0x6551b0 calls the simulation step through the battle-logic
singleton's vtable:

    mov  ecx, [0xcdde84]      ; battle logic object
    mov  edx, [ecx]           ; vtable
    mov  eax, [edx+0x34]      ; the step function
    push esi
    call eax

Emulating that loop shows it runs zero times at updateFPS/logicFPS = 4 (the 60fps
configuration), so it cannot be the thing that doubles simulated time. The doubling
therefore comes from a different call site that steps the logic once per frame
regardless of the frame rate.

This scans the image for the load-strip leading to a vtable+0x34 call on the
battle-logic object, so the call sites can be listed and ranked. Finding the
unconditional per-frame one is the prerequisite for a fractional accumulator: that
site is where every second call has to be suppressed.

Usage:
    python find_tick_callers.py [slot-hex]
"""
from __future__ import annotations

import struct
import sys

import static_sites as S

LOGIC_GLOBAL = 0x8DDE84
DEFAULT_SLOT = 0x34
WINDOW = 0x60

# Register that ends up holding the object, keyed by the byte before the imm32.
BY_MODRM = {0x0D: "ecx", 0x15: "edx", 0x1D: "ebx", 0x35: "esi", 0x3D: "edi"}


def find_loads(blob: bytes, base: int):
    """Yield (offset, register) for `mov reg, [0xCDDE84]` in the .text image."""
    needle = struct.pack("<I", base + LOGIC_GLOBAL)
    start = 0
    while True:
        i = blob.find(needle, start)
        if i < 0:
            return
        start = i + 1
        if i >= 1 and blob[i - 1] == 0xA1:
            yield i - 1, "eax"
        elif i >= 2 and blob[i - 2] == 0x8B and blob[i - 1] in BY_MODRM:
            yield i - 2, BY_MODRM[blob[i - 1]]


def norm(op: str) -> str:
    """Drop the size hint capstone prints, so operands compare by shape."""
    for prefix in ("dword ptr ", "word ptr ", "byte ptr ", "qword ptr ",
                   "xmmword ptr "):
        if op.startswith(prefix):
            return op[len(prefix):]
    return op


def main() -> int:
    slot = int(sys.argv[1], 16) if len(sys.argv) > 1 else DEFAULT_SLOT
    pe = S.PE(S.BINARY)
    va_base = pe.base
    text_rva = next(va for name, va, *_ in pe.sections if name == ".text")
    text_size = next(v for name, va, v, *_ in pe.sections if name == ".text")
    blob = pe.read(text_rva, text_size)
    print(f".text rva={text_rva:#x} size={text_size:#x} "
          f"va={va_base + text_rva:#010x}")

    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    loads = list(find_loads(blob, va_base))
    print(f"loads of the logic object: {len(loads)}")
    print(f"matching the exact strip: [0x{LOGIC_GLOBAL:x}] -> [obj] -> "
          f"[vtbl+{slot:#x}] -> call\n")

    hits = []
    for off, reg in loads:
        va = va_base + text_rva + off
        lines = []
        obj_reg = reg
        vtbl_reg = None
        fn_reg = None
        matched = False
        for ins in md.disasm(blob[off:off + WINDOW], va):
            lines.append(ins)
            m, ops = ins.mnemonic, ins.op_str
            parts = [norm(x.strip()) for x in ops.split(",")]
            # mov V, [obj_reg]
            if m == "mov" and vtbl_reg is None and len(parts) == 2 \
                    and parts[1] == f"[{obj_reg}]":
                vtbl_reg = parts[0]
            # mov F, [vtbl + slot]
            elif m == "mov" and vtbl_reg and fn_reg is None and \
                    len(parts) == 2 and parts[1] == f"[{vtbl_reg} + {slot:#x}]":
                fn_reg = parts[0]
            elif m == "call" and fn_reg and ops == fn_reg:
                matched = True
                break
            elif m == "call" and vtbl_reg and \
                    norm(ops) == f"[{vtbl_reg} + {slot:#x}]":
                matched = True
                break
            if ins.address > va + WINDOW - 8:
                break
        if matched:
            hits.append((va, reg, lines))

    print(f"exact matches: {len(hits)}\n")
    for va, reg, lines in hits:
        print(f"--- {va:#010x}  (object in {reg}) ---")
        for ins in lines:
            print(f"    {ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

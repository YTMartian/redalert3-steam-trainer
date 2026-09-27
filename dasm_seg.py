# -*- coding: utf-8 -*-
"""反汇编 trainer 运行时真正会写进游戏的 MC / MC2 机器码片段。

用法：
    python dasm_seg.py MC2 0x700 0x790
    python dasm_seg.py MC 0x600 0x660
"""
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

import trainer

BASES = (0x10000000, 0x10003100, 0x10005000, 0x10005040, 0x400000)

md = Cs(CS_ARCH_X86, CS_MODE_32)


def main():
    seg = (sys.argv[1] if len(sys.argv) > 1 else 'MC2').upper()
    lo = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3], 0) if len(sys.argv) > 3 else None

    mc, mc2 = trainer.build(trainer.ASM_TEXT, trainer.SYMBOLS, *BASES)
    code, base = (mc, BASES[0]) if seg == 'MC' else (mc2, BASES[1])
    if hi is None:
        hi = len(code)
    print('段 %s 共 %d 字节，反汇编 [0x%X .. 0x%X) 运行时基址 0x%08X'
          % (seg, len(code), lo, hi, base + lo))
    print('=' * 74)
    for insn in md.disasm(code[lo:hi], base + lo):
        print('  0x%04X  0x%08X  %-26s %s %s'
              % (insn.address - base, insn.address, insn.bytes.hex(),
                 insn.mnemonic, insn.op_str))
    return 0


if __name__ == '__main__':
    sys.exit(main())

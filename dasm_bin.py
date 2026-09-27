# -*- coding: utf-8 -*-
"""反汇编生成的 mustcode2.bin / mustcode.bin 片段。

用法：
    python dasm_bin.py mustcode2.bin 0x700 0x790
    python dasm_bin.py mustcode.bin 0x600 0x640
"""
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

md = Cs(CS_ARCH_X86, CS_MODE_32)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'mustcode2.bin'
    lo = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3], 0) if len(sys.argv) > 3 else len(open(path, 'rb').read())
    data = open(path, 'rb').read()
    print('%s  [0x%X .. 0x%X)  共 %d 字节' % (path, lo, hi, hi - lo))
    print('=' * 70)
    for insn in md.disasm(data[lo:hi], lo):
        print('  0x%04X  %-26s %s %s'
              % (insn.address, insn.bytes.hex(), insn.mnemonic, insn.op_str))
    return 0


if __name__ == '__main__':
    sys.exit(main())

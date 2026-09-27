# -*- coding: utf-8 -*-
"""反汇编 ra3_image.bin 中指定地址附近的代码。

用法：
    python dasm.py 0x71BA40 0x737A50 ...        # 每个地址默认前后各 0x60
    python dasm.py 0x71BA40:160                 # 指定字节数
"""
import json
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

meta = json.load(open('ra3_image.json', encoding='utf-8'))
img = open('ra3_image.bin', 'rb').read()
base = meta['base']
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


def dasm(va, back=0x60, fwd=0x60):
    lo = va - back
    hi = va + fwd
    print('=' * 74)
    print('0x%08X  （窗口 0x%08X .. 0x%08X，<<< 标记目标地址）' % (va, lo, hi))
    print('=' * 74)
    seg = img[lo - base:hi - base]
    for insn in md.disasm(seg, lo):
        mark = '  <<<' if insn.address == va else ''
        print('  0x%08X  %-30s %-24s%s'
              % (insn.address, insn.bytes.hex(), '%s %s' % (insn.mnemonic, insn.op_str),
                 mark))
    print('')


def main():
    if not sys.argv[1:]:
        print(__doc__)
        return 1
    for spec in sys.argv[1:]:
        if ':' in spec:
            a, n = spec.split(':', 1)
            va = int(a, 16)
            n = int(n, 0)
            lo, hi = va, va + n          # 从 va 开始 dump n 字节
            print('=' * 74)
            print('0x%08X 开始，共 0x%X 字节' % (va, n))
            print('=' * 74)
            for insn in md.disasm(img[lo - base:hi - base], lo):
                print('  0x%08X  %-30s %-24s'
                      % (insn.address, insn.bytes.hex(),
                         '%s %s' % (insn.mnemonic, insn.op_str)))
            print('')
        else:
            dasm(int(spec, 16))
    return 0


if __name__ == '__main__':
    sys.exit(main())

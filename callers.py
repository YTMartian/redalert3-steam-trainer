# -*- coding: utf-8 -*-
"""查找某个函数的所有 call 调用点，并打印调用点的上下文（判断返回值语义）。"""
import json
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

meta = json.load(open('ra3_image.json', encoding='utf-8'))
img = open('ra3_image.bin', 'rb').read()
base = meta['base']
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

text = [s for s in meta['sections'] if s['executable']][0]
t0 = base + text['va']
t1 = t0 + text['vsize']
code = img[text['va']:text['va'] + text['vsize']]


def callers_of(target):
    """找出所有 call rel32 到 target 的位置。"""
    out = []
    for i in range(len(code) - 5):
        if code[i] != 0xE8:
            continue
        rel = struct.unpack_from('<i', code, i + 1)[0]
        if t0 + i + 5 + rel == target:
            out.append(t0 + i)
    return out


def context(va, back=0x30, fwd=0x30):
    lo = max(t0, va - back)
    seg = img[lo - base:va + fwd - base]
    lines = []
    for insn in md.disasm(seg, lo):
        mark = '  <<< call' if insn.address == va else ''
        lines.append('      0x%08X  %-26s %s%s'
                     % (insn.address, insn.bytes.hex(),
                        '%s %s' % (insn.mnemonic, insn.op_str), mark))
    return lines


def main():
    targets = [int(a, 16) for a in sys.argv[1:]] or [0x0081DA70]
    for t in targets:
        cs = callers_of(t)
        print('=' * 74)
        print('目标 0x%08X ：%d 个调用点' % (t, len(cs)))
        print('=' * 74)
        for c in cs[:20]:
            print('  call @ 0x%08X' % c)
            for l in context(c):
                print(l)
            print('')
    return 0


if __name__ == '__main__':
    sys.exit(main())

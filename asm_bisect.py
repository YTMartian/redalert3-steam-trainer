# -*- coding: utf-8 -*-
"""逐行定位 MC / MC2 段里无法被 keystone 汇编的指令。

做法：把整段交给 keystone，若失败，则逐行替换成 nop 再试；
替换某行后能汇编成功，说明该行（或紧邻处）有问题。
用法：python asm_bisect.py [mc|mc2]
"""
import io
import json
import sys

import assemble

BASES = (0x10000000, 0x10003100, 0x10005000, 0x10005040, 0x400000)


def main():
    seg = (sys.argv[1] if len(sys.argv) > 1 else 'mc2').lower()
    sym = json.load(open('symbols.json', encoding='utf-8'))
    asm = io.open('mustcode_body.asm', encoding='utf-8').read()

    mc_lines, mc2_lines, _, _ = assemble.split_asm(asm, sym, *BASES)
    lines = mc2_lines if seg == 'mc2' else mc_lines
    base = BASES[1] if seg == 'mc2' else BASES[0]

    enc, n = assemble.ks.asm('\n'.join(lines), base)
    if enc is not None:
        print('[+] %s 段可以正常汇编（%d 行 / %d 字节）' % (seg, len(lines), len(enc)))
        return 0
    print('[!] %s 段汇编失败，开始逐行排查（%d 行）' % (seg, len(lines)))

    bad = []
    for i, line in enumerate(lines):
        s = line.strip()
        if not s or s.endswith(':') or s in ('nop',):
            continue
        trial = list(lines)
        trial[i] = 'nop'
        e2, _ = assemble.ks.asm('\n'.join(trial), base)
        if e2 is not None:
            bad.append((i, line))
            print('    可疑行 #%d: %r' % (i, line))

    if not bad:
        print('    （逐行替换都没能定位 —— 可能是标签重名或对齐溢出）')
    else:
        print('')
        print('    —— 上下文 ——')
        for i, line in bad:
            for j in range(max(0, i - 4), min(len(lines), i + 5)):
                mark = ' >>>' if j == i else '    '
                print('%s %4d| %s' % (mark, j, lines[j]))
            print('')
    return 0


if __name__ == '__main__':
    sys.exit(main())

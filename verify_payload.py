# -*- coding: utf-8 -*-
"""验证 trainer.py 的精简 build 与 payload.LABELS 是否严格一致。

这是一致性的关键：trainer 运行时用精简 build 汇编 ASM_TEXT（不做 nop 对齐），
而 hook 跳转用固化的 LABELS 偏移。二者必须完全对应，否则补丁会跳到错误位置。
"""
import json

import assemble
import trainer

BASES = (0x10000000, 0x10003100, 0x10005000, 0x10005040, 0x400000)

# assemble 侧（带对齐）
mc_a, mc2_a, lab_mc, lab_mc2 = assemble.build_from_src(
    trainer.ASM_TEXT, trainer.SYMBOLS, *BASES)

# trainer 侧（精简、不做对齐）
mc_t, mc2_t = trainer.build(
    trainer.ASM_TEXT, trainer.SYMBOLS, *BASES)

print('assemble MC  %d 字节 / trainer MC  %d 字节' % (len(mc_a), len(mc_t)))
print('assemble MC2 %d 字节 / trainer MC2 %d 字节' % (len(mc2_a), len(mc2_t)))
print('MC  字节一致 :', mc_a == mc_t)
print('MC2 字节一致 :', mc2_a == mc2_t)

ok = mc_a == mc_t and mc2_a == mc2_t

# LABELS 是否与 assemble 重算结果一致
lab = {'MC': lab_mc, 'MC2': lab_mc2}
p_lab = trainer.LABELS
same_lab = (p_lab.get('MC') == lab['MC'] and p_lab.get('MC2') == lab['MC2'])
print('LABELS 与重算结果一致 :', same_lab)
ok = ok and same_lab

# hook 目标是否落在合理位置（MC 段各块入口）
print()
print('--- hook 标签偏移抽查 ---')
for name, off in sorted(p_lab.get('MC', {}).items()):
    if name.startswith('mc_') and off < 0x1300:
        pass
print('mc_0=%s mc_29=%s mc_600=%s mc_700=%s mc_1120=%s' % (
    p_lab['MC'].get('mc_0'), p_lab['MC'].get('mc_29'),
    p_lab['MC'].get('mc_600'), p_lab['MC'].get('mc_700'),
    p_lab['MC'].get('mc_1120')))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
for nm, code, base in (('MC', mc_t, BASES[0]), ('MC2', mc2_t, BASES[1])):
    insns = list(md.disasm(code, base))
    addrs = set(i.address for i in insns)
    bad = 0
    for i in insns:
        if (i.mnemonic.startswith('j') or i.mnemonic == 'call') and i.operands and i.operands[0].type == 1:
            t = i.operands[0].imm
            if i.mnemonic.startswith('j') and base <= t < base + len(code) and t not in addrs:
                bad += 1
    print('%s: %d 条指令, 段内非法跳转 %d' % (nm, len(insns), bad))
    ok = ok and bad == 0

print()
print('总体验证:', 'OK' if ok else '失败')

# -*- coding: utf-8 -*-
import re, io, json, sys
sys.path.insert(0, '.')
import assemble
from keystone import Ks, KS_ARCH_X86, KS_MODE_32
ks = Ks(KS_ARCH_X86, KS_MODE_32)
MC, MC2 = 0x10000000, 0x10003100
FLAGS, IDB, MOD = 0x10005000, 0x10005040, 0x400000
back = assemble.load_back()
lines = assemble.load_asm()
syms = [('MC2', MC2), ('MC', MC), ('MOD', MOD), ('FLAGS', FLAGS), ('IDB', IDB)]
syms += [(k, MOD + v) for k, v in back.items()]
syms.append(('_ExitPlayerOneKillItMode', MC + 0x4D0))

mc, mc2 = [], []
cur = 'mc'
for line in lines:
    line = line.strip()
    if not line:
        continue
    if re.match(r'^[A-Za-z_][A-Za-z0-9_+]*:$', line):
        nm = line[:-1]
        if nm.startswith('MC2'): cur = 'mc2'
        elif nm.startswith('MC'): cur = 'mc'
        continue
    for name, val in syms:
        line = assemble.subst_sym(line, name, val)
    (mc2 if cur == 'mc2' else mc).append(line)

for tag, seg in [('MC', mc), ('MC2', mc2)]:
    for i, l in enumerate(seg):
        try:
            ks.asm(l, MC)
        except Exception as e:
            print('%s 行 %d 失败: %r -> %s' % (tag, i, l, e))
            break
    else:
        print('%s 全部单行 OK (%d 行)' % (tag, len(seg)))

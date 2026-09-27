# -*- coding: utf-8 -*-
"""诊断 assemble.compute_labels 的行/指令一一对应关系是否成立。"""
import json
import assemble as A
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

md = Cs(CS_ARCH_X86, CS_MODE_32)

MC = 0x10000000
MC2 = 0x10003100
FLAGS = 0x10005000
IDB = 0x10005040
MOD = 0x400000

asm_text = A.load_asm_text()
symbols = A.load_back()

# 复制 build_from_src 的分段逻辑，拿到 mc_lines / mc2_lines
import re
lines = asm_text.split('\n')
abs_syms = [('MC2', MC2), ('MC', MC), ('MOD', MOD), ('FLAGS', FLAGS), ('IDB', IDB)]
abs_syms += [(k, v) for k, v in symbols.items()]
mc_lines, mc2_lines = [], []
current = None

def subst_data(line):
    def repl_bracket(m):
        inner = m.group(1)
        for name, base in abs_syms:
            inner = A.subst_abs(inner, name, base)
        return '[' + inner + ']'
    return re.sub(r'\[([^\]]+)\]', repl_bracket, line)

def subst_code(line, seg):
    if seg == 'mc':
        line = re.sub(r'\bMC(\+0x[0-9a-fA-F]+)?\b', lambda m: A.tag_of(m.group(0)), line)
        line = re.sub(r'\bMC2(\+0x[0-9a-fA-F]+)?\b', lambda m: A.subst_abs(m.group(0), 'MC2', MC2), line)
    else:
        line = re.sub(r'\bMC2(\+0x[0-9a-fA-F]+)?\b', lambda m: A.tag_of(m.group(0)), line)
        line = re.sub(r'\bMC(\+0x[0-9a-fA-F]+)?\b', lambda m: A.subst_abs(m.group(0), 'MC', MC), line)
    line = re.sub(r'\b_Exit\w+\b', lambda m: A.tag_of(m.group(0)), line)
    return line

for line in lines:
    line = line.strip()
    if not line:
        continue
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_+]*):$', line)
    if m:
        name = m.group(1)
        if name.startswith('MC2'):
            current = 'mc2'; mc2_lines.append(A.tag_of(name) + ':')
        elif name.startswith('MC'):
            current = 'mc'; mc_lines.append(A.tag_of(name) + ':')
        elif name.startswith('MOD'):
            current = None
        elif name.startswith('_Exit'):
            (mc2_lines if current == 'mc2' else mc_lines).append(A.tag_of(name) + ':')
        continue
    if current is None:
        continue
    line = A.subst_abs(line, 'MOD', MOD)
    line = A.subst_abs(line, 'FLAGS', FLAGS)
    line = A.subst_abs(line, 'IDB', IDB)
    for name, val in symbols.items():
        line = re.sub(r'\b%s\b' % re.escape(name), '0x%X' % val, line)
    line = subst_data(line)
    line = subst_code(line, current)
    (mc2_lines if current == 'mc2' else mc_lines).append(line)

for seg_name, seg_lines, base in (('MC', mc_lines, MC), ('MC2', mc2_lines, MC2)):
    src = '\n'.join(seg_lines)
    enc, _ = A.ks.asm(src, base)
    code = bytes(enc)
    insns = list(md.disasm(code, base))
    n_inst_lines = sum(1 for l in seg_lines if not l.strip().endswith(':'))
    n_label_lines = sum(1 for l in seg_lines if l.strip().endswith(':'))
    print('=== %s ===' % seg_name)
    print('  bytes=%d  指令行=%d  capstone指令=%d  标签行=%d' %
          (len(code), n_inst_lines, len(insns), n_label_lines))
    if n_inst_lines == len(insns):
        print('  [OK] 行/指令一一对应')
    else:
        print('  [!] 不匹配，差 %d' % (n_inst_lines - len(insns)))
        # 找出第一处错位
        idx = 0
        for i, l in enumerate(seg_lines):
            l = l.strip()
            if not l:
                continue
            if l.endswith(':'):
                continue
            if idx < len(insns):
                sz = insns[idx].size
                # 估算 keystone 该行真实长度
                try:
                    one, _ = A.ks.asm(l, base + insns[idx].address - base)
                    real = len(one)
                except Exception as e:
                    print('  汇编失败:', l, e)
                    break
                if real != sz:
                    print('  [!] 第 %d 行长度不符: %r  keystone=%d capstone=%d' % (i, l, real, sz))
                    break
                idx += 1

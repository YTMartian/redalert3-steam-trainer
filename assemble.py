# -*- coding: utf-8 -*-
"""
汇编器 v4：标签方案（块内跳转用 keystone 标签自动对齐）
- 块内代码引用（jmp MC+0x29 等）→ keystone 标签 mc_29
- 数据引用（[MC+0x1000] 等）→ 绝对地址
- 跨段引用（MC 段内引用 MC2）→ 绝对地址
- _BackXXX / MOD / FLAGS / IDB → 绝对地址
- 内部标签 _ExitPlayerOneKillItMode → keystone 标签
"""
import re, io, json
from keystone import Ks, KS_ARCH_X86, KS_MODE_32
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

ks = Ks(KS_ARCH_X86, KS_MODE_32)
md = Cs(CS_ARCH_X86, CS_MODE_32)

def load_asm():
    return io.open('mustcode_body.asm', encoding='utf-8').read().split('\n')

def load_asm_text():
    return io.open('mustcode_body.asm', encoding='utf-8').read()

def load_back():
    return json.load(open('symbols.json'))

def parse_off(off):
    off = off.strip()
    if off.lower().startswith('0x'):
        return int(off, 16)
    return int(off, 10)

def norm_off(off):
    """'+0x29' → '29'（标签后缀）"""
    off = off.lstrip('+').lower()
    if off.startswith('0x'):
        off = off[2:]
    return off.lstrip('0') or '0'

def tag_of(sym):
    """符号 → keystone 标签名"""
    if sym.startswith('MC2'):
        return 'mc2_' + norm_off(sym[3:])
    if sym.startswith('MC'):
        return 'mc_' + norm_off(sym[2:])
    if sym.startswith('_Exit'):
        return 'exit_' + sym[1:].lower()
    return sym

def subst_abs(line, name, base):
    """name 或 name+偏移 → 合并绝对地址"""
    pat = re.compile(r'\b%s(\+[0-9A-Fa-fx]+)?\b' % re.escape(name))
    def repl(m):
        off = m.group(1)
        if off:
            return '0x%X' % (base + parse_off(off[1:]))
        return '0x%X' % base
    return pat.sub(repl, line)

# ---------------------------------------------------------------
# 段内标签对齐
#
# 原 CE 脚本约定：MC+0x29 / MC2+0x100 这类标签名 == 段内实际偏移。
# 跨段引用（MC2 段里 call MC+0x1120、MC 段里 je MC2+0x100）会被汇编成
# 绝对地址，因此该不变量一旦被破坏就会跳到错误指令上（游戏闪退）。
# 这里用 nop 填充，把这个不变量重新建立起来。
# ---------------------------------------------------------------
MC_ALIGN_TARGETS = [
    0x29, 0x6c, 0x9f, 0xc8, 0xf0, 0x118, 0x141, 0x175, 0x1ca, 0x250,
    0x294, 0x2bd, 0x2f9, 0x327, 0x35b, 0x394, 0x3d0, 0x41e, 0x461,
    0x4e5, 0x506, 0x600, 0x700, 0x800, 0x900, 0xa00, 0xa60, 0xaa0,
    0xb00, 0x1120, 0x1200,
]
MC2_ALIGN_TARGETS = [
    0x100, 0x200, 0x300, 0x400, 0x500, 0x600,
    0x700, 0x800, 0x900, 0xa00, 0xb00, 0xc00,
]


def align_pair(lines, raw, base, targets, seg):
    """向每个目标标签前插入 nop，使其实际偏移等于名字里的偏移。

    lines 与 raw 逐行一一对应：lines 是已替换符号、可直接汇编的形式，
    raw 是保留原始符号写法的同一行。nop 会同时插入两者，这样对齐结果
    可以固化回 mustcode_body.asm（mustcode_asm.py 的精简 build 也能复用）。
    """
    tag = 'mc2_' if seg == 'mc2' else 'mc_'
    for t in sorted(targets):
        name = tag + ('%x' % t)
        idx = None
        for i, l in enumerate(lines):
            if l.strip() == name + ':':
                idx = i
                break
        if idx is None:
            continue
        enc, _ = ks.asm('\n'.join(lines), base)
        labels = compute_labels(lines, bytes(enc), base)
        actual = labels.get(name)
        if actual is None:
            continue
        if actual > t:
            raise RuntimeError(
                '段 %s 标签 %s 已超出对齐边界（实际 0x%X > 目标 0x%X），请检查该块代码长度'
                % (seg, name, actual, t))
        if actual < t:
            pad = ['nop'] * (t - actual)
            lines[idx:idx] = pad
            raw[idx:idx] = pad
    return lines, raw


def split_asm(asm_text, symbols, mc_base, mc2_base, flags_base, idb_base, mod_base):
    """把 body 分段并替换符号。

    返回 (mc_lines, mc2_lines, mc_raw, mc2_raw)：
      *_lines 可直接交给 keystone 汇编；*_raw 是对应的原始符号写法。
    """
    back = symbols
    lines = asm_text.split('\n')

    # 绝对符号（数据引用和跨段引用用）
    abs_syms = [
        ('MC2', mc2_base),
        ('MC', mc_base),
        ('MOD', mod_base),
        ('FLAGS', flags_base),
        ('IDB', idb_base),
    ]
    # SYMBOLS（_BackXXX）里存的是 VA（绝对地址，已含 0x400000 基址），直接使用
    abs_syms += [(k, v) for k, v in back.items()]

    mc_lines, mc2_lines = [], []
    mc_raw, mc2_raw = [], []
    current = None

    def subst_data(line):
        """替换 [ ] 内的符号引用为绝对地址"""
        def repl_bracket(m):
            inner = m.group(1)
            for name, base in abs_syms:
                inner = subst_abs(inner, name, base)
            return '[' + inner + ']'
        return re.sub(r'\[([^\]]+)\]', repl_bracket, line)

    def subst_code(line, seg):
        """替换 [ ] 外的代码引用为标签（段内）或绝对地址（跨段）"""
        # 段内 MC/MC2 → 标签；跨段 → 绝对地址
        if seg == 'mc':
            # 本段 MC → 标签
            line = re.sub(r'\bMC(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: tag_of(m.group(0)), line)
            # 跨段 MC2 → 绝对地址
            line = re.sub(r'\bMC2(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: subst_abs(m.group(0), 'MC2', mc2_base), line)
        else:
            line = re.sub(r'\bMC2(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: tag_of(m.group(0)), line)
            line = re.sub(r'\bMC(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: subst_abs(m.group(0), 'MC', mc_base), line)
        # 内部标签 _Exit → 标签
        line = re.sub(r'\b_Exit\w+\b', lambda m: tag_of(m.group(0)), line)
        return line

    for line in lines:
        line = line.strip()
        if not line:
            continue
        orig = line

        # 标签定义行
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_+]*):$', line)
        if m:
            name = m.group(1)
            if name.startswith('MC2'):
                current = 'mc2'
                mc2_lines.append(tag_of(name) + ':')
                mc2_raw.append(name + ':')
            elif name.startswith('MC'):
                current = 'mc'
                mc_lines.append(tag_of(name) + ':')
                mc_raw.append(name + ':')
            elif name.startswith('MOD'):
                current = None
            elif name.startswith('_Exit'):
                # 内部标签定义（在 MC 段内）
                if current == 'mc2':
                    mc2_lines.append(tag_of(name) + ':')
                    mc2_raw.append(name + ':')
                else:
                    mc_lines.append(tag_of(name) + ':')
                    mc_raw.append(name + ':')
            # 其他（FLAGS+24 / _BackXXX）：忽略
            continue
        if current is None:
            continue

        # 绝对符号整行替换（MOD/FLAGS/IDB/_BackXXX，无论 [ ] 内外）
        line = subst_abs(line, 'MOD', mod_base)
        line = subst_abs(line, 'FLAGS', flags_base)
        line = subst_abs(line, 'IDB', idb_base)
        for name, val in back.items():
            line = re.sub(r'\b%s\b' % re.escape(name), '0x%X' % val, line)
        # [ ] 内的 MC/MC2 → 绝对地址（数据引用）
        line = subst_data(line)
        # [ ] 外的 MC/MC2 → 标签（段内）/ 绝对地址（跨段）
        line = subst_code(line, current)
        if current == 'mc2':
            mc2_lines.append(line)
            mc2_raw.append(orig.strip())
        else:
            mc_lines.append(line)
            mc_raw.append(orig.strip())

    return mc_lines, mc2_lines, mc_raw, mc2_raw


def aligned_body_text(asm_text, symbols, mc_base, mc2_base, flags_base, idb_base, mod_base):
    """返回把 nop 对齐固化进去后的 body 文本（仍是原始符号写法）。

    gen_payload.py 用它把对齐结果写回 mustcode_body.asm，
    这样 mustcode_asm.py 运行时那个不含 capstone、不做对齐的精简 build
    也能得到与 LABELS 完全一致的布局。
    """
    mc_lines, mc2_lines, mc_raw, mc2_raw = split_asm(
        asm_text, symbols, mc_base, mc2_base, flags_base, idb_base, mod_base)
    align_pair(mc_lines, mc_raw, mc_base, MC_ALIGN_TARGETS, 'mc')
    align_pair(mc2_lines, mc2_raw, mc2_base, MC2_ALIGN_TARGETS, 'mc2')
    return '\n'.join(mc_raw + mc2_raw)


def build_from_src(asm_text, symbols, mc_base, mc2_base, flags_base, idb_base, mod_base):
    mc_lines, mc2_lines, mc_raw, mc2_raw = split_asm(
        asm_text, symbols, mc_base, mc2_base, flags_base, idb_base, mod_base)
    mc_lines, _ = align_pair(mc_lines, mc_raw, mc_base, MC_ALIGN_TARGETS, 'mc')
    mc2_lines, _ = align_pair(mc2_lines, mc2_raw, mc2_base, MC2_ALIGN_TARGETS, 'mc2')

    mc_src = '\n'.join(mc_lines)
    mc2_src = '\n'.join(mc2_lines)
    mc_enc, _ = ks.asm(mc_src, mc_base)
    mc2_enc, _ = ks.asm(mc2_src, mc2_base)
    mc_labels = compute_labels(mc_lines, bytes(mc_enc), mc_base)
    mc2_labels = compute_labels(mc2_lines, bytes(mc2_enc), mc2_base)
    return bytes(mc_enc), bytes(mc2_enc), mc_labels, mc2_labels


def compute_labels(src_lines, code, base):
    """对照源码行与 capstone 反汇编，计算每个标签相对段的偏移"""
    labels = {}
    insns = list(md.disasm(code, base))
    idx = 0
    offset = 0
    for line in src_lines:
        line = line.strip()
        if not line:
            continue
        if line.endswith(':'):
            labels[line[:-1]] = offset
        else:
            if idx < len(insns):
                offset += insns[idx].size
                idx += 1
    return labels

def build(mc_base, mc2_base, flags_base, idb_base, mod_base):
    return build_from_src(load_asm_text(), load_back(), mc_base, mc2_base, flags_base, idb_base, mod_base)


if __name__ == '__main__':
    MC = 0x10000000
    MC2 = 0x10003100
    FLAGS = 0x10005000
    IDB = 0x10005040
    MOD = 0x400000
    mc, mc2, mc_labels, mc2_labels = build(MC, MC2, FLAGS, IDB, MOD)
    print('MustCode 段: %d 字节 (分配 0x3000)' % len(mc))
    print('MustCode2 段: %d 字节 (分配 0x1000)' % len(mc2))
    open('mustcode.bin', 'wb').write(mc)
    open('mustcode2.bin', 'wb').write(mc2)
    open('labels.json', 'w').write(json.dumps({
        'MC': mc_labels,
        'MC2': mc2_labels,
    }, indent=1))
    print('written mustcode.bin / mustcode2.bin / labels.json')

# -*- coding: utf-8 -*-
"""Assemble MustCode / MustCode2 at runtime addresses.

Used by overlay/tools/arm_mustcode.py. Labels are already aligned in payload.LABELS.
"""
import re

from keystone import Ks, KS_ARCH_X86, KS_MODE_32

MOD_BASE = 0x400000

# Resource hooks that crash in spectator mode.
PLAYER_HOOK_NAMES = {'PlayerID', 'Money', 'Power', 'SCPoint', 'HaveAllSC'}

ks = Ks(KS_ARCH_X86, KS_MODE_32)


def parse_off(off):
    off = off.strip()
    if off.lower().startswith('0x'):
        return int(off, 16)
    return int(off, 10)


def norm_off(off):
    off = off.lstrip('+').lower()
    if off.startswith('0x'):
        off = off[2:]
    return off.lstrip('0') or '0'


def tag_of(sym):
    if sym.startswith('MC2'):
        return 'mc2_' + norm_off(sym[3:])
    if sym.startswith('MC'):
        return 'mc_' + norm_off(sym[2:])
    if sym.startswith('_Exit'):
        return 'exit_' + sym[1:].lower()
    return sym


def subst_abs(line, name, base):
    pat = re.compile(r'\b%s(\+[0-9A-Fa-fx]+)?\b' % re.escape(name))

    def repl(m):
        off = m.group(1)
        if off:
            return '0x%X' % (base + parse_off(off[1:]))
        return '0x%X' % base
    return pat.sub(repl, line)


def build(asm_text, symbols, mc_base, mc2_base, flags_base, idb_base, mod_base):
    """Reassemble mustcode_body.asm at the addresses just allocated."""
    back = symbols
    lines = asm_text.split('\n')

    abs_syms = [
        ('MC2', mc2_base),
        ('MC', mc_base),
        ('MOD', mod_base),
        ('FLAGS', flags_base),
        ('IDB', idb_base),
    ]
    abs_syms += [(k, v) for k, v in back.items()]

    mc_lines, mc2_lines = [], []
    current = None

    def subst_data(line):
        def repl_bracket(m):
            inner = m.group(1)
            for name, base in abs_syms:
                inner = subst_abs(inner, name, base)
            return '[' + inner + ']'
        return re.sub(r'\[([^\]]+)\]', repl_bracket, line)

    def subst_code(line, seg):
        if seg == 'mc':
            line = re.sub(r'\bMC(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: tag_of(m.group(0)), line)
            line = re.sub(r'\bMC2(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: subst_abs(m.group(0), 'MC2', mc2_base), line)
        else:
            line = re.sub(r'\bMC2(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: tag_of(m.group(0)), line)
            line = re.sub(r'\bMC(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: subst_abs(m.group(0), 'MC', mc_base), line)
        line = re.sub(r'\b_Exit\w+\b', lambda m: tag_of(m.group(0)), line)
        return line

    for line in lines:
        line = line.strip()
        if not line:
            continue
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_+]*):$', line)
        if m:
            name = m.group(1)
            if name.startswith('MC2'):
                current = 'mc2'
                mc2_lines.append(tag_of(name) + ':')
            elif name.startswith('MC'):
                current = 'mc'
                mc_lines.append(tag_of(name) + ':')
            elif name.startswith('MOD'):
                current = None
            elif name.startswith('_Exit'):
                (mc2_lines if current == 'mc2' else mc_lines).append(tag_of(name) + ':')
            continue
        if current is None:
            continue
        line = subst_abs(line, 'MOD', mod_base)
        line = subst_abs(line, 'FLAGS', flags_base)
        line = subst_abs(line, 'IDB', idb_base)
        for name, val in back.items():
            line = re.sub(r'\b%s\b' % re.escape(name), '0x%X' % val, line)
        line = subst_data(line)
        line = subst_code(line, current)
        (mc2_lines if current == 'mc2' else mc_lines).append(line)

    mc_enc, _ = ks.asm('\n'.join(mc_lines), mc_base)
    mc2_enc, _ = ks.asm('\n'.join(mc2_lines), mc2_base)
    return bytes(mc_enc), bytes(mc2_enc)

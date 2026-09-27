# -*- coding: utf-8 -*-
"""
MustCode 转换器 v3
修复：数字正则排除字母/下划线边界；hook 地址映射在数字转换前。
"""
import re, io, json, struct
from hook_map import HOOKS

# x86 32 位寄存器编号与 ALU 指令 /digit 号
REG_ID = {'eax': 0, 'ecx': 1, 'edx': 2, 'ebx': 3, 'esp': 4, 'ebp': 5, 'esi': 6, 'edi': 7}
ALU_DIGIT = {'add': 0, 'or': 1, 'adc': 2, 'sbb': 3, 'and': 4, 'sub': 5, 'xor': 6, 'cmp': 7}


def encode_alu_mem_imm32(op, mem, imm):
    """把 add/cmp 等 [mem],小立即数 编码成 32 位立即数形式（81 /digit id，CE 5.5 语义）。

    keystone 会把 add [mem],0x1 优化成 83 /digit ib（4 字节），导致后续硬编码
    短跳转(db 74/75/eb)错位，因此这里手动编码成 7 字节的 32 位立即数形式。
    """
    m = re.match(r'^([a-z]{3})(?:\+(0x[0-9a-fA-F]+|\d+))?$', mem, re.I)
    if not m:
        return None
    reg = m.group(1).lower()
    if reg not in REG_ID:
        return None
    rm = REG_ID[reg]
    disp = int(m.group(2), 0) if m.group(2) else 0
    if disp == 0 and rm != 5:  # ebp 需要 disp8 以区分绝对寻址
        mod, disp_bytes = 0, b''
    elif -128 <= disp <= 127:
        mod, disp_bytes = 1, bytes([disp & 0xff])
    else:
        mod, disp_bytes = 2, struct.pack('<I', disp & 0xffffffff)
    digit = ALU_DIGIT[op]
    modrm = (mod << 6) | (digit << 3) | rm
    imm_bytes = struct.pack('<I', imm & 0xffffffff)
    return bytes([0x81, modrm]) + disp_bytes + imm_bytes

SRC = 'mustcode_raw.txt'
OLD2NEW = {h['orig']: h['new'] for h in HOOKS if h['new']}
AOBLEN = {h['orig']: len(h['aob']) // 2 for h in HOOKS}

def load_enable_body():
    txt = io.open(SRC, encoding='utf-8').read()
    m = re.search(r'\[ENABLE\](.*?)\[disable\]', txt, re.S)
    return m.group(1).split('\n')

def strip_comment(line):
    i = line.find('//')
    if i >= 0:
        line = line[:i]
    return line.strip()

def sym_replace(line):
    line = line.replace('Ra3_1.12.game', 'MOD')
    line = line.replace('ra3_1.12.game', 'MOD')
    line = line.replace('Mustcode', 'MC')
    line = line.replace('MustCode2', 'MC2')
    line = line.replace('MustCode', 'MC')
    line = line.replace('iEnable', 'FLAGS')
    line = re.sub(r'\bID\b', 'IDB', line, flags=re.IGNORECASE)
    return line

def convert_operands(s):
    s = re.sub(r'#([0-9a-fA-F]+)', r'0x\1', s)
    # 多字符 hex token（排除字母/下划线边界，避免误伤符号名和寄存器）
    s = re.sub(r'(?<![A-Za-z0-9_])([0-9a-fA-F]{2,})(?![A-Za-z0-9_])', r'0x\1', s)
    # 单字符 A-F 偏移（FLAGS+A / IDB+C → +0xA / +0xC），0-9 十进制等价无需转
    s = re.sub(r'\+([A-Fa-f])(?![0-9A-Za-z_])', r'+0x\1', s)
    # 单字符 A-F 立即数（cmp eax,A → cmp eax,0xA），排除寄存器名（esi/edi 等）
    s = re.sub(r',\s*([A-Fa-f])(?![0-9A-Za-z_])', r',0x\1', s)
    return s

def is_label(line):
    return bool(re.match(r'^[A-Za-z_][A-Za-z0-9_+]*:$', line))

def main():
    lines = load_enable_body()
    defines = {}
    out = []
    for raw in lines:
        line = strip_comment(raw)
        if not line or line == '|':
            continue
        if line.startswith(('fullaccess(', 'globalalloc(', 'label(')):
            continue
        if line.startswith('[ENABLE]') or line.startswith('[disable]'):
            continue
        m = re.match(r'define\(([^,]+),(.*)\)$', line)
        if m:
            defines[m.group(1).strip()] = m.group(2).strip()
            continue
        out.append(line)

    # 展开宏
    expanded = []
    for line in out:
        s = line.strip()
        expanded.append(defines.get(s, line))

    result = []
    back_labels = {}
    hook_orig = None

    for line in expanded:
        line = sym_replace(line)

        # hook 定义行（数字转换前检测）
        m = re.match(r'^MOD\+([0-9a-fA-F]+):$', line)
        if m:
            orig = int(m.group(1), 16)
            hook_orig = orig
            new = OLD2NEW.get(orig, orig)
            result.append('MOD+0x%X:' % new)
            continue

        # _BackXXX: 标签（记录返回点 RVA）
        m = re.match(r'^(_Back\w+):$', line)
        if m:
            lbl = m.group(1)
            if hook_orig is not None:
                newva = OLD2NEW.get(hook_orig, hook_orig)
                back_labels[lbl] = newva + AOBLEN.get(hook_orig, 0)
            result.append(line)
            continue

        # 标签行：偏移转 hex（MC+29: → MC+0x29:）
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)(\+[0-9a-fA-F]+):$', line)
        if m:
            result.append(m.group(1) + '+0x' + m.group(2)[1:] + ':')
            continue
        if is_label(line):
            result.append(line)
            continue

        # db 指令
        m = re.match(r'^(?i:db)\s+(.*)$', line)
        if m:
            bs = [x for x in m.group(1).split() if x]
            result.append('.byte ' + ','.join('0x' + x for x in bs))
            continue

        # 普通指令：助记符 + 操作数
        parts = line.split(None, 1)
        if len(parts) == 1:
            result.append(parts[0])
            continue
        inst = parts[0] + ' ' + convert_operands(parts[1])
        # 内存操作数 + 立即数：补 dword ptr（keystone 需要显式 size）
        m = re.match(r'^(mov|add|sub|cmp|or|xor|and|test|adc|sbb)\s+\[([^\]]+)\]\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*$', inst, re.I)
        if m and 'ptr' not in m.group(2).lower():
            op = m.group(1).lower()
            mem = m.group(2)
            immstr = m.group(3)
            imm = int(immstr, 0)
            # CE 5.5 对显式 32 位立即数保留 7 字节编码；keystone 会优化成 imm8(4字节)，
            # 导致后续硬编码短跳转(db 74/75/eb)错位。此处对 ALU 小立即数强制 32 位编码。
            if op in ALU_DIGIT and -128 <= imm <= 127:
                enc = encode_alu_mem_imm32(op, mem, imm)
                if enc is not None:
                    inst = '.byte ' + ','.join('0x%02x' % b for b in enc)
                else:
                    inst = '%s dword ptr [%s], %s' % (m.group(1), mem, immstr)
            else:
                inst = '%s dword ptr [%s], %s' % (m.group(1), mem, immstr)
        else:
            # 单操作数内存指令：补 dword ptr（dec/inc/not/neg [mem]）
            m = re.match(r'^(dec|inc|not|neg)\s+\[([^\]]+)\]\s*$', inst, re.I)
            if m and 'ptr' not in m.group(2).lower():
                inst = '%s dword ptr [%s]' % (m.group(1), m.group(2))
        result.append(inst)

    io.open('mustcode_body.asm', 'w', encoding='utf-8').write('\n'.join(result))
    json.dump(back_labels, open('symbols.json', 'w'), indent=2)
    print('written mustcode_body.asm (%d lines)' % len(result))
    print('_Back 标签 %d 个:' % len(back_labels))
    for k, v in sorted(back_labels.items()):
        print('  %-28s = 0x%08X' % (k, v))

if __name__ == '__main__':
    main()

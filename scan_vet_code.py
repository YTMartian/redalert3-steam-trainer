# -*- coding: utf-8 -*-
"""
离线静态分析 ra3_image.bin，定位「单位升级 / 星级」相关代码。

思路：
    已知星级字段位于 [[单位实体]+0x3CC] 对象内部（真实升级时 +0x24 会 +1）。
    要找到驱动星级的权威字段 / 升级函数，最可靠的做法是在游戏代码里找：
      1) 所有「引用了位移 0x3CC」的指令 —— 数量有限，能定位到操作星级组件的函数；
      2) 这些函数里，是否同时出现对 +0x24 / +0x10 的写操作；
      3) 与 Veterancy / Veteran / Heroic / LevelUp 等字符串交叉验证。

用法：
    python scan_vet_code.py            # 需要先跑过 dump_module.py

输出：scan_vet_code_result.txt
"""
import json
import re
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM, CS_OP_REG

DUMP_FILE = 'ra3_image.bin'
META_FILE = 'ra3_image.json'
OUT_FILE = 'scan_vet_code_result.txt'

VET_PTR_OFF = 0x3CC     # [实体+0x3CC] -> 星级组件
RANK_OFF = 0x24         # 星级候选
XP_OFF = 0x10           # 经验候选

KEYWORDS = [
    b'Veteran', b'veteran', b'VETERAN', b'Heroic', b'heroic',
    b'LevelUp', b'Level up', b'level up', b'LevelUpgraded',
    b'Experience', b'experience', b'Upgrade_Veteran', b'Chevron',
    b'Promote', b'promote', b'gainedVeterancy', b'SelectUnitLevelUp',
]

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

OUT = []


def log(s=''):
    OUT.append(s)


def mem_disp(op):
    """取内存操作数的位移（没有则 None）。"""
    if op.type == CS_OP_MEM:
        return op.mem.disp
    return None


def is_write(insn):
    """粗略判断指令是否写内存（目标操作数为内存）。"""
    if not insn.operands:
        return False
    m = insn.mnemonic
    first = insn.operands[0]
    if m in ('mov', 'movzx', 'movsx', 'add', 'sub', 'and', 'or', 'xor',
             'inc', 'dec', 'not', 'neg', 'shl', 'shr', 'sar', 'xchg'):
        return mem_disp(first) is not None
    return False


def sweep(code, va, on_insn):
    """线性扫描：遇到无法解码的字节就跳 1 字节继续（x86 指令流里混有数据）。"""
    mv = memoryview(code)
    n = len(mv)
    off = 0
    total = 0
    while off < n:
        progressed = False
        for insn in md.disasm(mv[off:], va + off):
            progressed = True
            on_insn(insn)
            total += 1
            off = insn.address + insn.size - va
        if not progressed:
            off += 1
    return total


def find_func_start(img, va, base, lo=0xC00):
    """向上找最近的 int3 填充 / 标准序言，估算函数起始地址。"""
    off = va - base
    start = max(0, off - lo)
    seg = img[start:off]
    # 优先：int3 填充之后即是函数起点
    idx = seg.rfind(b'\xcc\xcc\xcc')
    if idx >= 0:
        return base + start + idx + 3
    # 其次：标准序言 push ebp; mov ebp,esp
    idx = seg.rfind(b'\x55\x8b\xec')
    if idx >= 0:
        return base + start + idx
    idx = seg.rfind(b'\x8b\xff\x55\x8b\xec')   # mov edi,edi; push ebp; mov ebp,esp
    if idx >= 0:
        return base + start + idx
    return 0


def main():
    try:
        meta = json.load(open(META_FILE, encoding='utf-8'))
    except Exception as e:
        print('[!] 读不到 %s：%s' % (META_FILE, e))
        print('    请先以管理员身份运行 dump_module.py（等价于「定位星级代码.bat」）')
        return 1
    img = open(DUMP_FILE, 'rb').read()
    base = meta['base']
    sections = meta['sections']

    log('镜像: %s  base=0x%08X  size=0x%X  模块=%s'
        % (DUMP_FILE, base, meta['size_image'], meta.get('module', '?')))
    log('段表:')
    for s in sections:
        log('  %-8s VA=0x%08X vsize=0x%08X %s'
            % (s['name'], base + s['va'], s['vsize'],
               'CODE' if s['executable'] else ''))

    exec_secs = [s for s in sections if s['executable']]
    if not exec_secs:
        print('[!] 没找到可执行段')
        return 1

    # ============================================================
    # 1) 字符串搜索
    # ============================================================
    log('')
    log('=' * 70)
    log('1) 关键字字符串')
    log('=' * 70)
    str_hits = []          # (text, va)
    for kw in KEYWORDS:
        start = 0
        cnt = 0
        while True:
            i = img.find(kw, start)
            if i < 0:
                break
            va = base + i
            # 只保留看起来像 C 字符串的（后面有 \0，前面不是字母数字）
            nxt = img[i + len(kw):i + len(kw) + 1]
            prev = img[i - 1:i] if i > 0 else b'\x00'
            if nxt == b'\x00' and not (prev.isalnum() or prev == b'_'):
                str_hits.append((kw.decode(), va))
                cnt += 1
            start = i + 1
        if cnt:
            log('  %-22s 命中 %d' % (kw.decode(), cnt))

    log('')
    log('  --- 字符串地址 + 代码引用（push imm32 / mov reg,imm32）---')
    str_vas = {}
    for text, va in str_hits:
        str_vas.setdefault(va, text)
    refs = {}
    pat_push = re.compile(b'\x68(.{4})', re.S)
    for va, text in sorted(str_vas.items()):
        target = struct.pack('<I', va)
        found = []
        # push imm32
        st = 0
        while True:
            i = img.find(b'\x68' + target, st)
            if i < 0:
                break
            found.append(('push', base + i))
            st = i + 1
        # mov r32, imm32 (B8..BF)
        st = 0
        while True:
            i = -1
            j = img.find(target, st)
            while j >= 0:
                if img[j - 1] >= 0xB8 and img[j - 1] <= 0xBF:
                    i = j - 1
                    break
                j = img.find(target, j + 1)
            if i < 0:
                break
            found.append(('mov', base + i))
            st = i + 1
        # 数据里的裸指针
        st = 0
        while True:
            i = img.find(target, st)
            if i < 0:
                break
            found.append(('data', base + i))
            st = i + 1
        refs[va] = found
        log('  0x%08X %-22s 引用 %d 处' % (va, repr(text), len(found)))
        for kind, a in found[:12]:
            log('        %-4s @ 0x%08X' % (kind, a))

    # ============================================================
    # 2) 扫描引用了位移 0x3CC 的指令
    # ============================================================
    log('')
    log('=' * 70)
    log('2) 引用 [reg+0x%X]（星级组件指针）的指令' % VET_PTR_OFF)
    log('=' * 70)

    sites = []

    def on_insn(insn):
        for op in insn.operands:
            if mem_disp(op) == VET_PTR_OFF:
                sites.append(dict(va=insn.address, size=insn.size,
                                  text='%s %s' % (insn.mnemonic, insn.op_str),
                                  bytes=insn.bytes.hex()))
                return

    for s in exec_secs:
        off = s['va']
        code = img[off:off + s['vsize']]
        sweep(code, base + off, on_insn)

    # 按函数起点聚合
    groups = {}
    for site in sites:
        fs = find_func_start(img, site['va'], base)
        site['func'] = fs
        groups.setdefault(fs, []).append(site)

    log('  命中指令 %d 条，分布在 %d 个函数里' % (len(sites), len(groups)))
    log('')

    # 对每个函数做上下文反汇编，找 +0x24 / +0x10 的写操作
    interesting = []
    for fs, items in sorted(groups.items()):
        site_vas = set(i['va'] for i in items)
        lines = []
        lines.append('-' * 70)
        lines.append('函数 0x%08X   （%d 处引用 0x%X）' % (fs, len(items), VET_PTR_OFF))
        # 取该函数所在区间：从函数起点到最后一个 site + 0x200
        lo = fs if fs else min(i['va'] for i in items) - 0x100
        hi = max(i['va'] for i in items) + 0x200
        hi = min(hi, lo + 0x2000)
        seg = img[lo - base:hi - base]
        writes = []
        for insn in md.disasm(seg, lo):
            tag = ''
            d0 = mem_disp(insn.operands[0]) if insn.operands else None
            if d0 == RANK_OFF and is_write(insn):
                tag = '   <== 写 +0x%X（星级候选）' % RANK_OFF
                writes.append(('rank', insn.address))
            elif d0 == XP_OFF and is_write(insn):
                tag = '   <== 写 +0x%X（经验候选）' % XP_OFF
                writes.append(('xp', insn.address))
            elif d0 == VET_PTR_OFF:
                tag = '   <== 引用 +0x%X' % VET_PTR_OFF
            mark = '  <<< 目标指令' if insn.address in site_vas else ''
            lines.append('  0x%08X  %-34s%s%s'
                         % (insn.address, '%s %s' % (insn.mnemonic, insn.op_str),
                            tag, mark))
        if writes:
            interesting.append((fs, writes))
            lines.insert(2, '  [!!] 该函数内有对星级/经验字段的写：%s' % writes)
        log('\n'.join(lines))
        log('')

    log('=' * 70)
    log('3) 汇总：可能真正写星级的函数')
    log('=' * 70)
    if interesting:
        for fs, w in interesting:
            log('  0x%08X  写操作: %s' % (fs, w))
    else:
        log('  （没有函数同时出现「引用 +0x%X」和「写 +0x%X」）'
            % (VET_PTR_OFF, RANK_OFF))
        log('  说明星级字段不是这样被写的，需要换思路（例如通过 upgrade 系统）。')

    open(OUT_FILE, 'w', encoding='utf-8').write('\n'.join(OUT))
    print('[+] 已写出 %s（%d 行）' % (OUT_FILE, len(OUT)))
    print('    引用 0x%X 的指令 %d 条 / %d 个函数' % (VET_PTR_OFF, len(sites), len(groups)))
    print('    关键字字符串 %d 个' % len(str_vas))
    print('    可能写星级的函数 %d 个' % len(interesting))
    return 0


if __name__ == '__main__':
    sys.exit(main())

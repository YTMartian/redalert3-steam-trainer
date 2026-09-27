# -*- coding: utf-8 -*-
"""
离线分析 3：通过「星级组件」的虚函数表定位升级逻辑。

探测显示星级组件的第一个 dword = 0x00C35354（vtable）。本脚本：
  1) dump 该 vtable 附近的内存，看它是不是一串"连续的类虚表"；
  2) 读取 vtable 条目，挑出指向 .text 的函数指针并反汇编；
  3) 全 .text 搜索所有「写 [reg+0x24]」的指令模式（inc / add / mov imm），
     因为升级必定要递增这个字段。

用法：python analyze3.py
输出：analyze3_result.txt
"""
import json
import re
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

meta = json.load(open('ra3_image.json', encoding='utf-8'))
img = open('ra3_image.bin', 'rb').read()
base = meta['base']
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

VET_VTABLE_HINT = 0x00C35354        # 探测到的组件首 dword
RANK_OFF = 0x24

secs = {s['name']: s for s in meta['sections']}
text = secs.get('.text')


def in_text(va):
    if not text:
        return False
    return base + text['va'] <= va < base + text['va'] + text['vsize']


def rd32(va):
    o = va - base
    if o < 0 or o + 4 > len(img):
        return None
    return struct.unpack_from('<I', img, o)[0]


def rds(va, n=40):
    """读一段可打印字符串。"""
    o = va - base
    e = img.find(b'\x00', o, o + n)
    if e < 0:
        return None
    try:
        return img[o:e].decode('ascii')
    except Exception:
        return None


OUT = []


def log(s=''):
    OUT.append(s)


def main():
    log('镜像 base=0x%08X  size=0x%X' % (base, meta['size_image']))

    # ---------------- 1) vtable 区域 ----------------
    log('')
    log('=' * 74)
    log('1) 星级组件 vtable 区域（0x%08X 附近）' % VET_VTABLE_HINT)
    log('=' * 74)
    lo = VET_VTABLE_HINT - 0x80
    for va in range(lo, VET_VTABLE_HINT + 0x80, 4):
        v = rd32(va)
        s = rds(va)
        mark = ''
        if va == VET_VTABLE_HINT:
            mark = '   <=== 探测到的组件首 dword'
        if v is not None and in_text(v):
            mark += '   [-> 代码 0x%08X]' % v
        print('  0x%08X  %08X%s%s' % (va, v, ('  %r' % s) if s and len(s) > 2 else '', mark))

    # ---------------- 2) vtable 里的函数 ----------------
    log('')
    log('=' * 74)
    log('2) vtable(0x%08X) 的虚函数' % VET_VTABLE_HINT)
    log('=' * 74)
    vt_entries = [VET_VTABLE_HINT + 4 * i for i in range(0, 32)]
    funcs = []
    for i, ea in enumerate(vt_entries):
        v = rd32(ea)
        if v is None:
            break
        if in_text(v):
            funcs.append((i, v))
            log('  #%-2d 0x%08X -> 0x%08X' % (i, ea, v))
        else:
            log('  #%-2d 0x%08X -> 0x%08X（非代码，视为 vtable 结束）' % (i, ea, v))
            if i > 2:
                break

    log('')
    log('  --- 这些虚函数里，哪些写 [this+0x%X]？ ---' % RANK_OFF)
    for i, fva in funcs:
        body = img[fva - base:fva - base + 0x200]
        hits = []
        for insn in md.disasm(body, fva):
            if insn.operands and insn.operands[0].type == 3:   # CS_OP_MEM
                d = insn.operands[0].mem.disp
                m = insn.mnemonic
                if d == RANK_OFF and (m.startswith('mov') or m in
                                      ('inc', 'dec', 'add', 'sub', 'or', 'and')):
                    hits.append('0x%08X  %s %s' % (insn.address, m, insn.op_str))
        if hits:
            log('  #%-2d 0x%08X :' % (i, fva))
            for h in hits:
                log('        %s' % h)
    if not any(':' in l for l in OUT[-len(funcs) - 2:]):
        log('  （vtable 方法里没有直接写 +0x%X 的）' % RANK_OFF)

    # ---------------- 3) 全 text 搜索写 +0x24 ----------------
    log('')
    log('=' * 74)
    log('3) 全 .text 搜索「写 [reg+0x%X]」的指令' % RANK_OFF)
    log('=' * 74)
    t0 = base + text['va']
    t1 = t0 + text['vsize']
    code = img[text['va']:text['va'] + text['vsize']]

    # modrm 的高位 = 目标寄存器；对 [reg+disp8] 形式，mod=01，rm 指定基址
    # C7 /0 = mov dword [reg+0x24], imm32   -> C7 (mod=01,reg=000,rm=base) 24 imm32
    # FF /0 = inc dword [reg+0x24]          -> FF (mod=01,reg=000,rm=base) 24
    # FF /1 = dec                           -> FF (mod=01,reg=001,rm=base) 24
    # 83 /0 = add dword [reg+0x24], imm8    -> 83 (mod=01,reg=000,rm=base) 24 imm8
    patterns = [
        ('mov_imm', re.compile(rb'\xc7[\x40-\x47]\x24(.{4})', re.S)),
        ('inc',     re.compile(rb'\xff[\x40-\x47]\x24')),
        ('dec',     re.compile(rb'\xff[\x48-\x4f]\x24')),
        ('add_i8',  re.compile(rb'\x83[\x40-\x47]\x24(.)', re.S)),
    ]
    # rm=100 是 SIB，需要更宽松地处理；这里先覆盖常见 6 个寄存器
    regnames = {0: 'eax', 1: 'ecx', 2: 'edx', 3: 'ebx', 5: 'ebp', 6: 'esi', 7: 'edi'}

    found = []
    for kind, pat in patterns:
        for m in pat.finditer(code):
            va = t0 + m.start()
            rm = m.group(0)[1] & 7
            found.append((kind, va, regnames.get(rm, '?'), m.group(0).hex()))

    log('  命中 %d 处' % len(found))
    log('')
    log('  --- 逐处看上下文，找「附近有 0x3CC 引用」的可疑函数 ---')
    suspects = []
    for kind, va, reg, bs in sorted(found, key=lambda x: x[1]):
        # 附近 ±0x400 内是否有 0x3CC 的立即数位移引用
        lo = max(t0, va - 0x400)
        hi = min(t1, va + 0x400)
        near = img[lo - base:hi - base]
        has3cc = b'\xcc\x03\x00\x00' in near    # 0x3CC 作为 disp32
        has3cc8 = b'\xcc\x03' in near           # 0x3CC 作为 disp16/32 低位
        tag = ''
        if has3cc or has3cc8:
            tag = '   <<<<< 附近引用 0x3CC'
            suspects.append((kind, va, reg))
        log('  %-8s 0x%08X  [%s+0x%X]  %s%s' % (kind, va, reg, RANK_OFF, bs, tag))

    log('')
    log('  可疑（附近有 0x3CC 引用）的写操作：%d 处' % len(suspects))
    for kind, va, reg in suspects:
        log('    %-8s 0x%08X  [%s+0x%X]' % (kind, va, reg, RANK_OFF))

    open('analyze3_result.txt', 'w', encoding='utf-8').write('\n'.join(OUT))
    print('[+] 已写出 analyze3_result.txt（%d 行）' % len(OUT))
    print('')
    print('vtable 虚函数 %d 个；写 [reg+0x24] 指令 %d 处；可疑 %d 处'
          % (len(funcs), len(found), len(suspects)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

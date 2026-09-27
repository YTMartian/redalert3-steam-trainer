# -*- coding: utf-8 -*-
"""离线分析 4：确认星级计算函数所属的类，并读取关键常量。"""
import json
import re
import struct

meta = json.load(open('ra3_image.json', encoding='utf-8'))
img = open('ra3_image.bin', 'rb').read()
base = meta['base']


def rd(va, fmt='<I'):
    o = va - base
    if o < 0 or o + struct.calcsize(fmt) > len(img):
        return None
    return struct.unpack_from(fmt, img, o)[0]


def rdfloat(va):
    return rd(va, '<f')


def strs_after(va, n=8):
    """从 va 开始，把连续的可见字符串列出来。"""
    out = []
    o = va - base
    for m in re.finditer(rb'[\x20-\x7e]{5,}', img[o:o + 0x200]):
        out.append((base + o + m.start(), m.group().decode()))
        if len(out) >= n:
            break
    return out


def find_refs(addr):
    t = struct.pack('<I', addr)
    res = []
    st = 0
    while True:
        i = img.find(t, st)
        if i < 0:
            break
        res.append(base + i)
        st = i + 1
    return res


print('=' * 74)
print('1) 关键常量')
print('=' * 74)
for va in (0x00BE8A98, 0x00BE61D0):
    v = rd(va)
    f = rdfloat(va)
    print('  0x%08X = 0x%08X  float=%.6f  int=%d'
          % (va, v, f, struct.unpack('<i', struct.pack('<I', v))[0]))
    print('       上下文:', (img[va - base - 16:va - base + 16].hex()))

print('')
print('=' * 74)
print('2) 候选函数被哪些「表」引用（找类名）')
print('=' * 74)
for name, addr in [('星级计算', 0x007BC1A0),
                   ('写入等级', 0x0079A8B0),
                   ('经验累加', 0x00719F10),
                   ('虚表转发', 0x0081DA70),
                   ('重置', 0x007BC130)]:
    refs = find_refs(addr)
    print('')
    print('  %s 0x%08X : %d 处引用' % (name, addr, len(refs)))
    for r in refs[:6]:
        # 判断是否在一段连续的函数指针表里
        s = ''
        for a2, t in strs_after(r + 4, 3):
            s = t
            break
        # 往前看是不是函数指针
        prev = rd(r - 4)
        kind = ''
        if prev is not None and base + 0x1000 <= (prev or 0) < base + 0x8C541A:
            kind = '（前一个也是代码指针 → 表）'
        print('      @0x%08X %s  后续字符串=%r' % (r, kind, s))

print('')
print('=' * 74)
print('3) 找出所有「函数指针表 + 名称」的模式，定位 ExperienceTracker 系列')
print('=' * 74)
# vtable 区域在 0xC20000..0xC40000 之间，形如 [code,code,...,code] + "ClassName\0"
lo, hi = 0x00C20000, 0x00C40000
print('  扫描 0x%08X..0x%08X 找含 0x007BC1A0 或 0x0079A8B0 的表' % (lo, hi))
targets = {0x007BC1A0: '星级计算', 0x0079A8B0: '写入等级',
           0x00738730: '析构'}
for va in range(lo, hi, 4):
    v = rd(va)
    if v in targets:
        # 往前找表头（往前直到不是代码指针）
        st = va
        while st - 4 >= lo:
            p = rd(st - 4)
            if p is None or not (base + 0x1000 <= p < base + 0x8C541A):
                break
            st -= 4
        # 往后找末端
        en = va
        while en + 4 < hi:
            p = rd(en + 4)
            if p is None or not (base + 0x1000 <= p < base + 0x8C541A):
                break
            en += 4
        nm = strs_after(en + 4, 1)
        print('  表 0x%08X..0x%08X  含 %s@0x%08X  名称=%r'
              % (st, en, targets[v], va, nm[0][1] if nm else '?'))

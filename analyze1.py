# -*- coding: utf-8 -*-
"""离线分析 1：星级/升级相关的字符串表与代码引用。"""
import json
import re
import struct

meta = json.load(open('ra3_image.json', encoding='utf-8'))
img = open('ra3_image.bin', 'rb').read()
base = meta['base']


def s_at(va, maxlen=48):
    o = va - base
    if o < 0 or o >= len(img):
        return None
    e = img.find(b'\x00', o, o + maxlen)
    if e < 0:
        return None
    raw = img[o:e]
    try:
        return raw.decode('ascii')
    except Exception:
        return None


def dump_around(va, back=0x60, fwd=0x120):
    print('  --- 0x%08X 附近字符串 ---' % va)
    o = va - base
    # 从区域开头扫可打印串
    s = max(0, o - back)
    e = min(len(img), o + fwd)
    for m in re.finditer(rb'[\x20-\x7e]{4,}', img[s:e]):
        a = base + s + m.start()
        print('    0x%08X  %r' % (a, m.group().decode()))


print('=' * 72)
print('1) VETERAN 及其邻居')
print('=' * 72)
dump_around(0x00BD609C)

print('')
print('=' * 72)
print('2) 全镜像搜索星级相关单词')
print('=' * 72)
for kw in [b'ROOKIE', b'Rookie', b'rookie', b'ELITE', b'Elite', b'elite',
           b'HEROIC', b'Heroic', b'heroic', b'VETERAN', b'Veteran', b'veteran',
           b'VeterancyUpgrade', b'Upgrade_Veteran', b'VeteranUpgrade']:
    st = 0
    hits = []
    while True:
        i = img.find(kw, st)
        if i < 0:
            break
        # 要求是独立 C 字符串
        nxt = img[i + len(kw):i + len(kw) + 1]
        prev = img[i - 1:i] if i > 0 else b'\x00'
        if nxt == b'\x00' and not (prev.isalnum() or prev in (b'_',)):
            hits.append(base + i)
        st = i + 1
    if hits:
        print('  %-18s -> %s' % (kw.decode(), ' '.join('0x%08X' % h for h in hits)))

print('')
print('=' * 72)
print('3) 这些字符串被谁引用（push imm32 / mov reg,imm32 / 裸指针）')
print('=' * 72)


def refs_to(va):
    t = struct.pack('<I', va)
    out = []
    st = 0
    while True:
        i = img.find(t, st)
        if i < 0:
            break
        kind = 'data'
        if i >= 1 and 0xB8 <= img[i - 1] <= 0xBF:
            kind = 'mov'
        if i >= 1 and img[i - 1] == 0x68:
            kind = 'push'
        out.append((kind, base + i))
        st = i + 1
    return out


targets = []
for kw in [b'ROOKIE', b'ELITE', b'HEROIC', b'VETERAN']:
    st = 0
    while True:
        i = img.find(kw, st)
        if i < 0:
            break
        nxt = img[i + len(kw):i + len(kw) + 1]
        prev = img[i - 1:i] if i > 0 else b'\x00'
        if nxt == b'\x00' and not prev.isalnum():
            targets.append((kw.decode(), base + i))
        st = i + 1

for text, va in sorted(set(targets), key=lambda x: x[1]):
    r = refs_to(va)
    print('  0x%08X %-10s %d 处引用' % (va, text, len(r)))
    for kind, a in r[:10]:
        print('       %-5s @ 0x%08X' % (kind, a))

# -*- coding: utf-8 -*-
"""二级交叉引用：字符串 -> 指针表 -> 代码。

RA3 的模块名/INI 键名常以「字符串 + 指针表」形式存在，代码引用的是表的地址，
所以一级 xref 常为 0。这里做两级查找。
"""
import re
import struct
import sys

PATH = r'H:\steam client\steamapps\common\Command and Conquer Red Alert 3\Data\RA3_1.12.game'
IMAGE_BASE = 0x400000


def parse():
    data = open(PATH, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    sec0 = pe + 24 + opt_size
    secs = []
    for i in range(nsec):
        o = sec0 + i * 40
        name = data[o:o + 8].rstrip(b'\x00').decode('latin1')
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', data, o + 8)
        secs.append(dict(name=name, va=va, vsize=vsize, rawptr=rawptr, rawsize=rawsize))
    return data, secs


def sec_of(secs, va):
    rva = va - IMAGE_BASE
    for s in secs:
        if s['va'] <= rva < s['va'] + max(s['vsize'], s['rawsize']):
            return s
    return None


def deref_ptr(data, secs, va):
    s = sec_of(secs, va)
    if not s:
        return None
    off = s['rawptr'] + (va - IMAGE_BASE - s['va'])
    if off + 4 > len(data):
        return None
    return struct.unpack_from('<I', data, off)[0]


def find_word(data, secs, value, only=('.data', '.rdata')):
    out = []
    pat = struct.pack('<I', value)
    for s in secs:
        if s['name'] not in only:
            continue
        blob = data[s['rawptr']:s['rawptr'] + s['rawsize']]
        for m in re.finditer(re.escape(pat), blob):
            out.append(IMAGE_BASE + s['va'] + m.start())
    return out


def find_code_xref(data, secs, value):
    s = next(x for x in secs if x['name'] == '.text')
    blob = data[s['rawptr']:s['rawptr'] + s['rawsize']]
    pat = struct.pack('<I', value)
    return [IMAGE_BASE + s['va'] + m.start() for m in re.finditer(re.escape(pat), blob)]


def main():
    data, secs = parse()
    for spec in sys.argv[1:]:
        va = int(spec, 16)
        # 读取该 VA 处的字符串
        s = sec_of(secs, va)
        off = s['rawptr'] + (va - IMAGE_BASE - s['va'])
        txt = data[off:off + 64].split(b'\x00')[0].decode('latin1')
        print('=== 0x%08X  "%s" ===' % (va, txt))
        # 一级：谁在 .data/.rdata 里存了这个 VA
        holders = find_word(data, secs, va)
        print('  存有该字符串指针的位置: %d' % len(holders))
        for h in holders[:8]:
            print('    holder@0x%08X' % h)
        if not holders:
            # 退化：直接找代码引用
            for c in find_code_xref(data, secs, va)[:8]:
                print('    code ref@0x%08X' % c)
            continue
        # 二级：谁引用了 holder
        for h in holders[:4]:
            refs = find_code_xref(data, secs, h)
            print('    holder 0x%08X 被代码引用 %d 次:' % (h, len(refs)))
            for r in refs[:8]:
                print('       0x%08X' % r)


if __name__ == '__main__':
    main()

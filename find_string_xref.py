# -*- coding: utf-8 -*-
"""在 ra3_1.12.game 里搜索字符串（ASCII + UTF-16LE）并定位其被引用的代码。

用途：找单位升级/星级（veteran/elite/promote）相关逻辑的入口函数。
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


def find_strings(data, secs, needle):
    """返回 [(VA, 编码)]。"""
    hits = []
    nb = needle.encode('ascii')
    nb16 = needle.encode('utf-16-le')
    for s in secs:
        if s['name'] not in ('.rdata', '.data', '.text'):
            continue
        blob = data[s['rawptr']:s['rawptr'] + s['rawsize']]
        base_va = IMAGE_BASE + s['va']
        for m in re.finditer(re.escape(nb), blob):
            hits.append((base_va + m.start(), 'ascii', s['name']))
        for m in re.finditer(re.escape(nb16), blob):
            hits.append((base_va + m.start(), 'utf16', s['name']))
    return hits


def find_xrefs(data, secs, va):
    """在 .text 里找引用该 VA 的指令（push imm32 / mov reg,imm32 / 等）。"""
    text = next(s for s in secs if s['name'] == '.text')
    blob = data[text['rawptr']:text['rawptr'] + text['rawsize']]
    base_va = IMAGE_BASE + text['va']
    out = []
    pat = struct.pack('<I', va)
    for m in re.finditer(re.escape(pat), blob):
        out.append(base_va + m.start())
    return out


def main():
    needles = sys.argv[1:] or ['Veteran', 'Elite', 'Heroic', 'Promot', 'Rank']
    data, secs = parse()
    for needle in needles:
        hits = find_strings(data, secs, needle)
        print('=== "%s": %d 处 ===' % (needle, len(hits)))
        for va, enc, sec in hits[:12]:
            off = va - IMAGE_BASE
            # 打印周边字符串便于判断
            raw = data[next(s['rawptr'] for s in secs if s['rawptr'] <= off < s['rawptr'] + s['rawsize'])
                       + (off - next(s['va'] for s in secs if s['rawptr'] <= off < s['rawptr'] + s['rawsize'])):]
            ctx = raw[:64].split(b'\x00')[0]
            try:
                txt = ctx.decode('latin1')
            except Exception:
                txt = repr(ctx)
            xr = find_xrefs(data, secs, va)
            print('  VA=0x%08X (%s, %s) xrefs=%d  周边=%r' % (va, enc, sec, len(xr), txt[:48]))
            for x in xr[:6]:
                print('       ref@0x%08X' % x)


if __name__ == '__main__':
    main()

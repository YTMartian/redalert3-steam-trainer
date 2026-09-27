# -*- coding: utf-8 -*-
"""在 Steam 版 .text 里统计对某偏移的访问，用于判断字段/对象性质。"""
import re
import struct
import sys

PATH = r'H:\steam client\steamapps\common\Command and Conquer Red Alert 3\Data\RA3_1.12.game'
IMAGE_BASE = 0x400000

from capstone import Cs, CS_ARCH_X86, CS_MODE_32


def text():
    data = open(PATH, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    sec0 = pe + 24 + opt_size
    for i in range(nsec):
        o = sec0 + i * 40
        name = data[o:o + 8].rstrip(b'\x00').decode('latin1')
        if name == '.text':
            vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', data, o + 8)
            return data, data[rawptr:rawptr + rawsize], IMAGE_BASE + va


def find_offset_access(disp, limit=200):
    data, blob, base = text()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    hits = []
    # 用字节模式粗筛 disp32：小端
    pat = struct.pack('<I', disp)
    cands = [m.start() for m in re.finditer(re.escape(pat), blob)]
    print('偏移 0x%X 的 disp32 字节出现次数: %d' % (disp, len(cands)))
    # 反汇编较粗：从候选点前的 8 字节开始对齐反汇编，看是否命中
    seen = set()
    for c in cands:
        start = max(0, c - 10)
        for i in md.disasm(blob[start:c + 7], base + start):
            if i.address <= base + c < i.address + i.size:
                for op in i.operands:
                    if op.type == 3 and op.mem.disp == disp:  # MEM
                        if i.address not in seen:
                            seen.add(i.address)
                            hits.append((i.address, i.mnemonic + ' ' + i.op_str))
                break
    return hits


def main():
    disps = [int(x, 16) for x in sys.argv[1:]] or [0x3cc]
    for d in disps:
        hits = find_offset_access(d)
        print('=== [reg+0x%X] 命中 %d 处 ===' % (d, len(hits)))
        for va, txt in hits[:40]:
            print('  0x%08X  %s' % (va, txt))


if __name__ == '__main__':
    main()

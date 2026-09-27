# -*- coding: utf-8 -*-
"""打印 ExperienceTrackerObject 虚表，并反汇编 vtable[4]（0x10）指向的函数。"""
import json
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

meta = json.load(open('ra3_image.json', encoding='utf-8'))
img = open('ra3_image.bin', 'rb').read()
base = meta['base']
md = Cs(CS_ARCH_X86, CS_MODE_32)

VT = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0xC35354
n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x20

off = VT - base
print('vtable @ 0x%08X  (共 %d 项)' % (VT, n))
entries = []
for i in range(n):
    v = struct.unpack('<I', img[off + i * 4: off + i * 4 + 4])[0]
    entries.append(v)
    print('  +0x%02X  ->  0x%08X' % (i * 4, v))

for i in (0x10,):
    fn = entries[i // 4]
    if not (base <= fn < base + len(img)):
        print('\n[!] vtable+0x%X 不是代码指针: 0x%08X' % (i, fn))
        continue
    print('')
    print('=' * 74)
    print('vtable+0x%X = 0x%08X 反汇编' % (i, fn))
    print('=' * 74)
    seg = img[fn - base: fn - base + 0x100]
    for insn in md.disasm(seg, fn):
        print('  0x%08X  %-28s %s %s'
              % (insn.address, insn.bytes.hex(), insn.mnemonic, insn.op_str))

# -*- coding: utf-8 -*-
"""Find readers/writers of the logic-FPS global and the game-speed object."""
import struct
import sys

import capstone
import pefile

PATH = sys.argv[1] if len(sys.argv) > 1 else r'ra3_1.12.game'
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
pe = pefile.PE(PATH, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
BUF = open(PATH, 'rb').read()
SEC = [(s.VirtualAddress, max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData,
        s.Name.rstrip(b'\x00').decode('latin1')) for s in pe.sections]
for vaddr, size, praw, name in SEC:
    if name == '.text':
        LO, HI, TEXT = BASE + vaddr, BASE + vaddr + size, BUF[praw:praw + size]


def read(va, n):
    rva = va - BASE
    for vaddr, size, praw, _n in SEC:
        if vaddr <= rva < vaddr + size:
            return BUF[praw + (rva - vaddr):praw + (rva - vaddr) + n]
    return None


def dis(va, n=36, before=0):
    d = read(va - before, 16 * (n + 3))
    print('--- disasm %08X' % va)
    for i, ins in enumerate(md.disasm(d, va - before)):
        if i > n + 1:
            break
        print('   %08X  %-22s %s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))


def xrefs(addr, label, show=3):
    pat = struct.pack('<I', addr)
    print('##### xrefs to %08X (%s)' % (addr, label))
    st, n = 0, 0
    while True:
        i = TEXT.find(pat, st)
        if i < 0:
            break
        st = i + 1
        va = LO + i
        n += 1
        print('  @%08X' % va)
        if n <= show:
            dis(va, 24, 28)
            print()
    print('  total %d\n' % n)


xrefs(0x00CB4098, 'logic FPS int', 4)
xrefs(0x00CE811C, 'game speed holder ptr', 3)

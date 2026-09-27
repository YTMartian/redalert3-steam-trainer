# -*- coding: utf-8 -*-
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE('ra3_1.12.game')
base = pe.OPTIONAL_HEADER.ImageBase
raw = open('ra3_1.12.game', 'rb').read()
md = Cs(CS_ARCH_X86, CS_MODE_32)

def va2fo(va):
    rva = va - base
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.SizeOfRawData:
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

def dump(va, before, after, label=''):
    fo = va2fo(va)
    if fo is None:
        print('  [%08X] not in file' % va); return
    print('  ---- %s @ %08X ----' % (label, va))
    code = raw[fo-before:fo+after]
    for ins in md.disasm(code, va-before):
        m = ' <==' if ins.address == va else ''
        print('   %08X: %-9s %-28s%s' % (ins.address, ins.mnemonic, ins.op_str, m))

print('=== FastBuild3 平移推算 0x7350EF 附近 ===')
dump(0x7350E0, 0x0, 0x60, 'fld [esi+1bc]?')

print()
print('=== OneKill 0x766xxx 候选 ===')
for v in (0x76671a, 0x76674f, 0x769343):
    dump(v, 0x18, 0x28, 'movss [esi+4],xmm0')

print()
print('=== Zoom 0x5FE2xx 候选 ===')
for v in (0x5fe2e6, 0x5fe440, 0x5fe719):
    dump(v, 0x10, 0x18, 'movss [esi+44],xmm0')

print()
print('=== GodMode 全面搜索 mov edx,[eax+3c];mov ecx,esi ===')
# 搜索 8b503c 后跟 8bce 或 89f1
import re
for pat in (bytes.fromhex('8b503c8bce'), bytes.fromhex('8b503c89f1')):
    hits=[]; st=0
    while True:
        i=raw.find(pat,st)
        if i<0: break
        hits.append(i); st=i+1
    print('  AOB %s -> %d hits' % (pat.hex(), len(hits)))
    for h in hits:
        for s in pe.sections:
            if s.PointerToRawData <= h < s.PointerToRawData + s.SizeOfRawData:
                print('     va=0x%X' % (base + s.VirtualAddress + (h - s.PointerToRawData)))

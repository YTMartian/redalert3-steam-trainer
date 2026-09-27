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

print('=== OneKill: 0x766xxx 函数完整结构（找 [esi-8] 访问）===')
# 反汇编一段找 mov eax,[esi-8] 附近
fo = va2fo(0x766600)
code = raw[fo:fo+0x300]
for ins in md.disasm(code, 0x766600):
    if 'esi - 8' in ins.op_str or ins.address in (0x76671a, 0x76674f):
        print('   %08X: %-9s %-30s' % (ins.address, ins.mnemonic, ins.op_str))

print()
print('=== GodMode 候选反汇编 ===')
for v in (0x570582, 0x5705DF, 0x5F887E, 0x76E130):
    dump(v, 0x14, 0x20, 'mov edx,[eax+3c];mov ecx,esi')

print()
print('=== Zoom 候选反汇编 (0x5-0x6区) ===')
for v in (0x5db12c, 0x604e68, 0x60c350, 0x6f6938):
    dump(v, 0x10, 0x14, 'movss [esi+44],xmm0')

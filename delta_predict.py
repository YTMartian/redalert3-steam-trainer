# -*- coding: utf-8 -*-
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE('ra3_1.12.game')
base = pe.OPTIONAL_HEADER.ImageBase
raw = open('ra3_1.12.game', 'rb').read()
md = Cs(CS_ARCH_X86, CS_MODE_32)

def fo2va(fo):
    for s in pe.sections:
        if s.PointerToRawData <= fo < s.PointerToRawData + s.SizeOfRawData:
            return base + s.VirtualAddress + (fo - s.PointerToRawData)
    return None

def va2fo(va):
    rva = va - base
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.SizeOfRawData:
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

def aob(data, pat):
    res=[]; st=0
    while True:
        i=data.find(pat,st)
        if i<0: break
        res.append(i); st=i+1
    return res

def dump(va, before, after, label=''):
    fo = va2fo(va)
    if fo is None:
        print('  [%08X] not in file' % va); return
    print('  ---- %s @ %08X ----' % (label, va))
    for ins in md.disasm(raw[fo-before:fo+after], va-before):
        m = ' <==' if ins.address == va else ''
        print('   %08X: %-9s %-28s%s' % (ins.address, ins.mnemonic, ins.op_str, m))

# delta 规律: 大部分代码段 +0x3E3B0~+0x3E410
D = 0x3E400

print('=== OneKill (orig 0x3651AE, 预测≈0x7495AE) 候选 ===')
for v in (0x74589b, 0x74854d):
    dump(v, 0x14, 0x1c, 'movss [esi+4],xmm0')

print()
print('=== GodMode (orig 0x12EEDF, 预测≈0x57052F) 候选 ===')
for v in (0x570582, 0x5705df):
    dump(v, 0x10, 0x20, 'mov edx,[eax+3c];mov ecx,esi')

print()
print('=== OneKillData2 (orig 0x2E24E3, 预测≈0x320893) 候选 ===')
pat = bytes.fromhex('8b893c030000')
for h in aob(raw, pat):
    va = fo2va(h)
    if va and 0x310000 < va < 0x340000:
        dump(va, 0xc, 0x10, 'mov ecx,[ecx+33c]')

print()
print('=== OneKillData (orig 0x3FE714, 预测≈0x43C684) 候选 ===')
pat = bytes.fromhex('8b8e3c030000')
for h in aob(raw, pat):
    va = fo2va(h)
    if va and 0x420000 < va < 0x460000:
        dump(va, 0xc, 0x10, 'mov ecx,[esi+33c]')

print()
print('=== Zoom (orig 0x1EC7CD, 预测≈0x5EC7CD+0x3E400=0x5EAB7D) 候选 ===')
for v in (0x5fe2e6, 0x5fe440, 0x5fe719):
    dump(v, 0x14, 0x14, 'movss [esi+44],xmm0')

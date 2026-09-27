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

# OneKill 原始 VA 0x7651AE 附近（Steam 同地址），看是否仍是伤害函数
print('=== OneKill 原始地址 0x7651AE 附近（Steam 同地址）===')
dump(0x7651AE, 0x20, 0x20, '0x7651AE')

print()
print('=== GodMode: 搜索 [ebx+138] 访问（补丁特征：mov ecx,[ebx+138]）===')
pat = bytes.fromhex('8b8b38010000')
hits=[]; st=0
while True:
    i=raw.find(pat,st)
    if i<0: break
    hits.append(i); st=i+1
print('  mov ecx,[ebx+138] -> %d hits' % len(hits))
for h in hits[:20]:
    for s in pe.sections:
        if s.PointerToRawData <= h < s.PointerToRawData + s.SizeOfRawData:
            print('     va=0x%X' % (base + s.VirtualAddress + (h - s.PointerToRawData)))

# OneKillData 原始 VA 0x7FE714 附近
print()
print('=== OneKillData 原始地址 0x7FE714 附近 ===')
dump(0x7FE714, 0x18, 0x18, 'mov ecx,[esi+33c]')

print()
print('=== OneKillData2 原始地址 0x6E24E3 附近 ===')
dump(0x6E24E3, 0x18, 0x18, 'mov ecx,[ecx+33c]')

# GodMode 候选：反汇编更多 0x76xxxx/0x7xxxxx 区的（接近伤害函数）
print()
print('=== GodMode 候选 0x76E130 完整函数（含 [edi+33c]）===')
dump(0x76E130, 0x18, 0x28, '')
print()
print('=== GodMode 候选 0x7E08BA / 0x7E9F02 ===')
dump(0x7E08BA, 0x10, 0x18, '')
dump(0x7E9F02, 0x10, 0x18, '')

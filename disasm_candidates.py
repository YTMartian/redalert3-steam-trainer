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

def dump(va, before=0x10, after=0x20, label=''):
    fo = va2fo(va)
    if fo is None:
        print('  [%08X] not in file' % va)
        return
    start = va - before
    sfo = va2fo(start)
    if sfo is None:
        sfo = fo
        start = va
    code = raw[sfo:sfo + before + after]
    print('  --- %s @ %08X ---' % (label, va))
    for ins in md.disasm(code, start):
        marker = ' <== HOOK' if ins.address == va else ''
        print('   %08X: %-10s %-28s%s' % (ins.address, ins.mnemonic, ins.op_str, marker))

print('=== 1. OneKill 扩展 AOB: movss [esi+4],xmm0; test eax,eax ===')
pat = bytes.fromhex('f30f11460485c0')
hits = []
start = 0
while True:
    i = raw.find(pat, start)
    if i < 0:
        break
    hits.append(i)
    start = i + 1
for h in hits:
    # fo->va
    for s in pe.sections:
        if s.PointerToRawData <= h < s.PointerToRawData + s.SizeOfRawData:
            print('  va=0x%X (fileoff 0x%X)' % (base + s.VirtualAddress + (h - s.PointerToRawData), h))
            break
print('  total hits: %d' % len(hits))

print()
print('=== 2. EnemyCantBuild 预期位置 0x74D920 (FastBuild1+0x102) ===')
dump(0x74D920, 0x18, 0x30, 'EnemyCantBuild?')

print()
print('=== 3. SuperPower 3 候选 ===')
for va in (0x729f19, 0x7437a2, 0x75e77b):
    dump(va, 0x10, 0x18, 'mov ebx,[eax+418]')

print()
print('=== 4. DangerLevel 6 候选 (mov eax,[ecx+1278]) ===')
for va in (0x877070, 0x877090, 0x8770b0, 0x879dc0, 0x879de0, 0x879e00):
    dump(va, 0x8, 0x10, '')

print()
print('=== 5. FastBuild3 8 候选 (fld [esi+1bc]) ===')
for va in (0x73511f, 0x7905ce, 0x812c49, 0x8478eb, 0x88677f, 0x8bd0c1, 0x8c61fb, 0x97b984):
    dump(va, 0xc, 0x10, '')

print()
print('=== 6. GodMode: 搜索 mov edx,[eax+3c] (8b503c) 上下文 ===')
pat2 = bytes.fromhex('8b503c')
hits2 = []
start = 0
while True:
    i = raw.find(pat2, start)
    if i < 0 or len(hits2) >= 40:
        break
    hits2.append(i)
    start = i + 1
print('  mov edx,[eax+3c] hits (first 40): %d total cap' % len(hits2))
# 只展示其中 mov ecx,esi / mov ecx,esi 变体跟随的
for h in hits2:
    for s in pe.sections:
        if s.PointerToRawData <= h < s.PointerToRawData + s.SizeOfRawData:
            va = base + s.VirtualAddress + (h - s.PointerToRawData)
            nxt = raw[h+3:h+5]
            if nxt in (bytes.fromhex('8bce'), bytes.fromhex('89f1')):
                dump(va, 0xc, 0x14, 'mov edx,[eax+3c]; mov ecx,esi?')
            break

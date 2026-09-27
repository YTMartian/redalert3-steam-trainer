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

def dump_func(va, label):
    fo = va2fo(va)
    print('  ---- %s @ %08X ----' % (label, va))
    code = raw[fo-0x20:fo+0x40]
    start = va-0x20
    for ins in md.disasm(code, start):
        m = ' <==' if ins.address == va else ''
        print('   %08X: %-10s %-30s%s' % (ins.address, ins.mnemonic, ins.op_str, m))

# Zoom: 列出全部候选地址，聚类
print('=== Zoom 全部候选 (movss [esi+44],xmm0) ===')
zooms = aob(raw, bytes.fromhex('f30f114644'))
zoomvas = sorted(fo2va(h) for h in zooms if fo2va(h))
print('count=%d' % len(zoomvas))
print([hex(v) for v in zoomvas])

print()
print('=== OneKill 候选 (movss [esi+4],xmm0) 按区域分布 ===')
oks = aob(raw, bytes.fromhex('f30f114604'))
okvas = sorted(fo2va(h) for h in oks if fo2va(h))
print('count=%d' % len(okvas))
# 聚类：只打印 0x7xxxxx 及以上区域的（接近 OneKill 原始 0x7651AE）
print([hex(v) for v in okvas if v >= 0x700000])

print()
print('=== SuperPower 3 候选完整函数 ===')
for v in (0x729f19, 0x7437a2, 0x75e77b):
    dump_func(v, 'mov ebx,[eax+418]')

print()
print('=== SuperPower2 已定位 0x87CB89 完整函数 (参考) ===')
dump_func(0x87CB89, 'mov esi,[eax+50];mov eax,[ecx]')

print()
print('=== FastBuild3 候选完整函数 (fld [esi+1bc]) ===')
for v in (0x7905ce, 0x812c49, 0x8478eb, 0x88677f, 0x8bd0c1, 0x8c61fb, 0x97b984):
    dump_func(v, '')

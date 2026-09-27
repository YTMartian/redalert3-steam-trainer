# -*- coding: utf-8 -*-
"""为每个hook点构造更精确的AOB：尝试编码变体 + 同簇相对偏移验证"""
import pefile
from keystone import Ks, KS_ARCH_X86, KS_MODE_32
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

pe = pefile.PE('ra3_1.12.game')
base = pe.OPTIONAL_HEADER.ImageBase
raw = open('ra3_1.12.game', 'rb').read()
ks = Ks(KS_ARCH_X86, KS_MODE_32)
md = Cs(CS_ARCH_X86, CS_MODE_32)

def asm(insns):
    code, cnt = ks.asm('; '.join(insns))
    return bytes(code)

def aob_find_all(data, pattern, limit=200):
    res = []
    start = 0
    while True:
        i = data.find(pattern, start)
        if i < 0 or len(res) >= limit:
            break
        res.append(i)
        start = i + 1
    return res

def fo2va(fo):
    for s in pe.sections:
        if s.PointerToRawData <= fo < s.PointerToRawData + s.SizeOfRawData:
            return base + s.VirtualAddress + (fo - s.PointerToRawData)
    return None

# 22 hook points: name, orig RVA offset, instruction variants (list of alternative encodings)
# each variant is a list-of-instruction-strings
hooks = [
    ('PlayerID',       0x0FF95B, [['mov edx, dword ptr [eax + 0x28]', 'mov eax, dword ptr [edx + 0x20]']]),
    ('Money',          0x6CFDFE, [['add edi, dword ptr [eax + 4]', 'mov edx, dword ptr [ecx]']]),
    ('Power',          0x6CFD0D, [['mov eax, dword ptr [eax + 4]', 'mov ecx, dword ptr [esi + 0x3b0]']]),
    ('SCPoint',        0x6CFE6C, [['mov edi, dword ptr [eax + 0x34]', 'mov ecx, dword ptr [esi + 0x3c]']]),
    ('HaveAllSC',      0x6CFEB5, [['movss xmm0, dword ptr [edi + 0x2c]']]),
    ('FastBuild1',     0x30F42E, [['cvttss2si eax, dword ptr [esi + 0x1c]']]),
    ('FastBuild2',     0x30F38C, [['fld dword ptr [esi + 0x1c]', 'mov dword ptr [esi + 0x5c], eax']]),
    ('FastBuild3',     0x2F6CFF, [['fld dword ptr [esi + 0x1bc]']]),
    ('SuperPower',     0x2EBB69, [['mov ebx, dword ptr [eax + 0x418]']]),
    ('SuperPower2',    0x43EC19, [['mov esi, dword ptr [eax + 0x50]', 'mov eax, dword ptr [ecx]']]),
    ('DisableAllSP',   0x43ECA6, [['mov edx, dword ptr [ecx + 0x50]', 'cmp edx, dword ptr [eax + 0xc]']]),
    ('DisableAllSP2',  0x30A5F6, [['mov ecx, dword ptr [eax + 0x50]', 'cmp ecx, dword ptr [esi + 0x20]']]),
    ('Zoom',           0x1EC7CD, [['movss dword ptr [esi + 0x44], xmm0']]),
    ('Map',            0x3C226C, [['movss dword ptr [ebp + 0x260], xmm0']]),
    ('UnitAmmo',       0x128735, [['mov ecx, dword ptr [edx + edi*4]', 'test ecx, ecx']]),
    ('DangerLevel',    0x438ED0, [['mov eax, dword ptr [ecx + 0x1278]']]),
    ('OreMine',        0x2D4933, [['mov eax, dword ptr [eax + 8]', 'sub eax, dword ptr [ecx + 0x14]']]),
    ('EnemyCantBuild', 0x30F530, [
        ['add edx, dword ptr [eax + 4]', 'cmp edx, edi'],
        ['add edx, dword ptr [eax + 4]', 'cmp edi, edx'],
    ]),
    ('GodMode',        0x12EEDF, [
        ['mov edx, dword ptr [eax + 0x3c]', 'mov ecx, esi'],
        ['mov edx, dword ptr [eax + 0x3c]', 'mov ecx, esi'],
    ]),
    ('OneKill',        0x3651AE, [['movss dword ptr [esi + 4], xmm0']]),
    ('OneKillData',    0x3FE714, [['mov ecx, dword ptr [esi + 0x33c]']]),
    ('OneKillData2',   0x2E24E3, [['mov ecx, dword ptr [ecx + 0x33c]']]),
]

print('=== per-hook AOB (variants) ===')
results = {}
for name, off, variants in hooks:
    results[name] = {'orig': off, 'hits': []}
    for v in variants:
        try:
            pat = asm(v)
        except Exception as e:
            continue
        hits = aob_find_all(raw, pat)
        vas = [(fo2va(h), h) for h in hits]
        results[name]['hits'].append((pat, vas))
        n = len(hits)
        print('%-15s orig=0x%06X %-24s -> %d hits' % (name, off, pat.hex(), n))
        for va, fo in vas[:12]:
            print('    %s  (fileoff 0x%x)' % (hex(va) if va else '?', fo))
    print()

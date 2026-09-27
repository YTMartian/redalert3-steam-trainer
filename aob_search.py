# -*- coding: utf-8 -*-
import pefile
from keystone import Ks, KS_ARCH_X86, KS_MODE_32

pe = pefile.PE('ra3_1.12.game')
base = pe.OPTIONAL_HEADER.ImageBase
raw = open('ra3_1.12.game', 'rb').read()
ks = Ks(KS_ARCH_X86, KS_MODE_32)

def asm(insns):
    code, cnt = ks.asm('; '.join(insns))
    return bytes(code)

def aob_find_all(data, pattern):
    res = []
    start = 0
    while True:
        i = data.find(pattern, start)
        if i < 0:
            break
        res.append(i)
        start = i + 1
    return res

# 22 hook points: (offset, original instructions)
hooks = [
    (0xFF95B,  ['mov edx, dword ptr [eax + 0x28]', 'mov eax, dword ptr [edx + 0x20]']),
    (0x6CFDFE, ['add edi, dword ptr [eax + 4]', 'mov edx, dword ptr [ecx]']),
    (0x6CFD0D, ['mov eax, dword ptr [eax + 4]', 'mov ecx, dword ptr [esi + 0x3b0]']),
    (0x6CFE6C, ['mov edi, dword ptr [eax + 0x34]', 'mov ecx, dword ptr [esi + 0x3c]']),
    (0x6CFEB5, ['movss xmm0, dword ptr [edi + 0x2c]']),
    (0x30F42E, ['cvttss2si eax, dword ptr [esi + 0x1c]']),
    (0x30F38C, ['fld dword ptr [esi + 0x1c]', 'mov dword ptr [esi + 0x5c], eax']),
    (0x2F6CFF, ['fld dword ptr [esi + 0x1bc]']),
    (0x2EBB69, ['mov ebx, dword ptr [eax + 0x418]']),
    (0x43EC19, ['mov esi, dword ptr [eax + 0x50]', 'mov eax, dword ptr [ecx]']),
    (0x43ECA6, ['mov edx, dword ptr [ecx + 0x50]', 'cmp edx, dword ptr [eax + 0xc]']),
    (0x30A5F6, ['mov ecx, dword ptr [eax + 0x50]', 'cmp ecx, dword ptr [esi + 0x20]']),
    (0x1EC7CD, ['movss dword ptr [esi + 0x44], xmm0']),
    (0x3C226C, ['movss dword ptr [ebp + 0x260], xmm0']),
    (0x128735, ['mov ecx, dword ptr [edx + edi*4]', 'test ecx, ecx']),
    (0x438ED0, ['mov eax, dword ptr [ecx + 0x1278]']),
    (0x2D4933, ['mov eax, dword ptr [eax + 8]', 'sub eax, dword ptr [ecx + 0x14]']),
    (0x30F530, ['add edx, dword ptr [eax + 4]', 'cmp edx, edi']),
    (0x12EEDF, ['mov edx, dword ptr [eax + 0x3c]', 'mov ecx, esi']),
    (0x3651AE, ['movss dword ptr [esi + 4], xmm0']),
    (0x3FE714, ['mov ecx, dword ptr [esi + 0x33c]']),
    (0x2E24E3, ['mov ecx, dword ptr [ecx + 0x33c]']),
]

print('=== AOB SEARCH for hook point original bytes in Steam binary ===')
for i, (off, insns) in enumerate(hooks):
    try:
        pat = asm(insns)
    except Exception as e:
        print('[%2d] orig=0x%06X  ASM ERROR %s: %s' % (i, off, e, insns))
        continue
    hits = aob_find_all(raw, pat)
    # map file offset to RVA
    def fo2rva(fo):
        for s in pe.sections:
            if s.PointerToRawData <= fo < s.PointerToRawData + s.SizeOfRawData:
                return base + s.VirtualAddress + (fo - s.PointerToRawData)
        return None
    rvas = [fo2rva(h) for h in hits]
    print('[%2d] orig=0x%06X  AOB=%s  hits=%d' % (i, off, pat.hex(), len(hits)))
    for h, r in zip(hits[:8], rvas[:8]):
        print('       fo=0x%06X -> va=%s' % (h, hex(r) if r else '?'))

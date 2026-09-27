# -*- coding: utf-8 -*-
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from keystone import Ks, KS_ARCH_X86, KS_MODE_32

pe = pefile.PE('ra3_1.12.game')
base = pe.OPTIONAL_HEADER.ImageBase
data = open('ra3_1.12.game', 'rb').read()

md = Cs(CS_ARCH_X86, CS_MODE_32)
ks = Ks(KS_ARCH_X86, KS_MODE_32)

def rva_to_file(addr):
    rva = addr - base
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

def disasm_at(addr, n=6):
    off = rva_to_file(addr)
    if off is None:
        return None, b''
    code = data[off:off+64]
    out = []
    for ins in md.disasm(code, addr):
        out.append(ins)
        if len(out) >= n:
            break
    return out, code[:16]

# (address, expected-asm-list)
hooks = [
    (0x400000 + 0xFF95B,  ['mov edx, dword ptr [eax + 0x28]', 'mov eax, dword ptr [edx + 0x20]']),
    (0x400000 + 0x6CFDFE, ['add edi, dword ptr [eax + 4]', 'mov edx, dword ptr [ecx]']),
    (0x400000 + 0x6CFD0D, ['mov eax, dword ptr [eax + 4]', 'mov ecx, dword ptr [esi + 0x3b0]']),
    (0x400000 + 0x6CFE6C, ['mov edi, dword ptr [eax + 0x34]', 'mov ecx, dword ptr [esi + 0x3c]']),
    (0x400000 + 0x6CFEB5, ['movss xmm0, dword ptr [edi + 0x2c]']),
    (0x400000 + 0x30F42E, ['cvttss2si eax, dword ptr [esi + 0x1c]']),
    (0x400000 + 0x30F38C, ['fld dword ptr [esi + 0x1c]', 'mov dword ptr [esi + 0x5c], eax']),
    (0x400000 + 0x2F6CFF, ['fld dword ptr [esi + 0x1bc]']),
    (0x400000 + 0x2EBB69, ['mov ebx, dword ptr [eax + 0x418]']),
    (0x400000 + 0x43EC19, ['mov esi, dword ptr [eax + 0x50]', 'mov eax, dword ptr [ecx]']),
    (0x400000 + 0x43ECA6, ['mov edx, dword ptr [ecx + 0x50]', 'cmp edx, dword ptr [eax + 0xc]']),
    (0x400000 + 0x30A5F6, ['mov ecx, dword ptr [eax + 0x50]', 'cmp ecx, dword ptr [esi + 0x20]']),
    (0x400000 + 0x1EC7CD, ['movss dword ptr [esi + 0x44], xmm0']),
    (0x400000 + 0x3C226C, ['movss dword ptr [ebp + 0x260], xmm0']),
    (0x400000 + 0x128735, ['mov ecx, dword ptr [edx + edi*4]', 'test ecx, ecx']),
    (0x400000 + 0x438ED0, ['mov eax, dword ptr [ecx + 0x1278]']),
    (0x400000 + 0x2D4933, ['mov eax, dword ptr [eax + 8]', 'sub eax, dword ptr [ecx + 0x14]']),
    (0x400000 + 0x30F530, ['add edx, dword ptr [eax + 4]', 'cmp edx, edi']),
    (0x400000 + 0x12EEDF, ['mov edx, dword ptr [eax + 0x3c]', 'mov ecx, esi']),
    (0x400000 + 0x3651AE, ['movss dword ptr [esi + 4], xmm0']),
    (0x400000 + 0x3FE714, ['mov ecx, dword ptr [esi + 0x33c]']),
    (0x400000 + 0x2E24E3, ['mov ecx, dword ptr [ecx + 0x33c]']),
]

print('=== HOOK POINT VERIFICATION (Steam ra3_1.12.game) ===')
match = 0
for addr, expected in hooks:
    off = rva_to_file(addr)
    if off is None:
        print('[%08X] NOT IN FILE (outside any section?)' % addr)
        continue
    insns, raw = disasm_at(addr, len(expected))
    actual = ['%s %s' % (i.mnemonic, i.op_str) for i in insns]
    ok = actual[:len(expected)] == expected
    if ok:
        match += 1
        print('[%08X] MATCH  %s' % (addr, ' ; '.join(actual[:len(expected)])))
    else:
        print('[%08X] MISMATCH' % addr)
        print('    expected: %s' % ' ; '.join(expected))
        print('    actual  : %s' % ' ; '.join(actual))
        print('    raw     : %s' % raw.hex())

print()
print('MATCHED %d / %d hook points' % (match, len(hooks)))

# 函数调用地址
print()
print('=== INTERNAL FUNCTION CALL TARGETS ===')
funcs = {
    0x35C200: 'SelectUnitLevelUp',
    0x39EA50: 'DestroySelectUnit',
    0x3E4230: 'GetUnitData2',
    0x205240: 'CreateUnit',
    0x1ED4A0: 'GetMouseXYZinMap',
}
for off, name in funcs.items():
    addr = base + off
    foff = rva_to_file(addr)
    if foff is None:
        print('[%08X] %-20s NOT IN FILE' % (addr, name))
        continue
    insns, raw = disasm_at(addr, 3)
    print('[%08X] %-20s : %s' % (addr, name, ' ; '.join('%s %s' % (i.mnemonic, i.op_str) for i in insns)))

# 全局指针
print()
print('=== GLOBAL POINTERS ===')
globs = {
    0x8E08DC: 'GetUnitData base',
    0x8E6C58: 'GetUnitData2 this',
    0x8DAEFC: 'GetMouseXYZinMap base',
    0x8E9838: 'UnitID magic',
    0xE7565:  'Mustcode+1200 ptr',
}
for off, name in globs.items():
    addr = base + off
    foff = rva_to_file(addr)
    if foff is None:
        print('[%08X] %-20s NOT IN FILE' % (addr, name))
    else:
        print('[%08X] %-20s in file @ fileoff 0x%x' % (addr, name, foff))

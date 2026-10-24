# -*- coding: utf-8 -*-
"""Disassemble the modifier-related functions in the user's Steam ra3_1.12.game."""
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

PATH = r'C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game'
VA = {
    'ModifierFind': 0x4A6F50,
    'ApplyModifier': 0x756640,
}
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

pe = pefile.PE(PATH, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
with open(PATH, 'rb') as f:
    blob = f.read()
secs = [(s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData) for s in pe.sections]


def read_va(va, n):
    rva = va - base
    for va0, vsz, raw in secs:
        if va0 <= rva < va0 + vsz:
            off = raw + (rva - va0)
            return blob[off:off + n]
    return b''


for name, va in VA.items():
    code = read_va(va, 80)
    print('=== %s VA=0x%08X hex=%s' % (name, va, code[:16].hex()))
    for ins in md.disasm(code, va):
        print('   0x%08X  %-28s %s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
        if ins.mnemonic in ('ret', 'retn') or ins.address > va + 48:
            break
    print()

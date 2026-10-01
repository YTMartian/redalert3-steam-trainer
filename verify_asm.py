# -*- coding: utf-8 -*-
"""验证汇编结果：检查关键子块入口偏移是否正确对齐"""
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
md = Cs(CS_ARCH_X86, CS_MODE_32)

mc = open('mustcode.bin', 'rb').read()

# 关键子块入口偏移（原脚本标签）
KEYS = {
    0x0:   'PlayerID',
    0x29:  'Money',
    0x6c:  'Power',
    0x9f:  'SCPoint',
    0xc8:  'HaveAllSC',
    0xf0:  'FastBuild1',
    0x118: 'FastBuild2',
    0x141: 'FastBuild3',
    0x175: 'SuperPower',
    0x1ca: 'SuperPower2',
    0x250: 'DisableAllSP',
    0x294: 'DisableAllSP2',
    0x2f9: 'Map',
    0x327: 'UnitAmmo',
    0x35b: 'DangerLevel',
    0x394: 'OreMine',
    0x3d0: 'EnemyCantBuild',
    0x600: 'Money2/分发',
    0x700: '命令分发',
    0x800: 'SelectUnitLevelUp',
    0x900: 'DestroySelectUnit',
    0xA00: 'GetUnitData',
    0xA60: 'GetUnitData2',
    0xAA0: 'CreateUnit',
    0xB00: 'SetUnitState',
    0x1120: 'GetMouseXYZinMap',
    0x1200: 'PlayerID',
}

print('=== MustCode 子块入口验证 ===')
for off, name in sorted(KEYS.items()):
    if off >= len(mc):
        print('0x%-5X %-20s 超出长度' % (off, name))
        continue
    code = mc[off:off+12]
    insns = list(md.disasm(code, 0))
    txt = '; '.join('%s %s' % (i.mnemonic, i.op_str) for i in insns[:2])
    print('0x%-5X %-20s %s' % (off, name, txt))

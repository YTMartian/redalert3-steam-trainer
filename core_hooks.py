# -*- coding: utf-8 -*-
"""
核心功能版 hook 定义 —— 17 个已定位 hook 的完整注入信息
每个 hook: 名称 / Steam新VA / 原始指令AOB(hex) / jmp目标(MustCode+偏移)
"""
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

# 17 个已定位 hook（名称, Steam新RVA, AOB, jmp目标偏移）
# jmp 目标偏移 = MustCode 块内偏移（十六进制）
CORE_HOOKS = [
    ('PlayerID',       0x54119B, '8b50288b4220',          0x1200),
    ('Money',          0xA64E9E, '0378048b11',            0x29),
    ('Power',          0xA64DAD, '8b40048b8eb0030000',    0x6c),
    ('SCPoint',        0xA64F0C, '8b78348b4e3c',          0x9f),
    ('HaveAllSC',      0xA64F55, 'f30f10472c',            0xc8),
    ('FastBuild1',     0x74D81E, 'f30f2c461c',            0xf0),
    ('FastBuild2',     0x74D77C, 'd9461c89465c',          0x118),
    ('FastBuild3',     0x73511F, 'd986bc010000',          0x141),
    ('SuperPower',     0x729F19, '8b9818040000',          0x175),
    ('SuperPower2',    0x87CB89, '8b70508b01',            0x1ca),
    ('DisableAllSP',   0x87CC16, '8b51503b500c',          0x250),
    ('DisableAllSP2',  0x748A06, '8b48503b4e20',          0x294),
    ('Map',            0x8005BC, 'f30f118560020000',      0x2f9),
    ('UnitAmmo',       0x569D85, '8b0cba85c9',            0x327),
    ('DangerLevel',    0x877070, '8b8178120000',          0x35b),
    ('OreMine',        0x712EB3, '8b40082b4114',          0x394),
    ('EnemyCantBuild', 0x74D920, '0350043bd7',            0x3d0),
]

# 已定位的全局数据指针（.data 段 RVA 不变）
DATA_GLOBALS = [
    # Steam 版重定位：旧 0x8DB73C 已无任何代码引用；新地址经运行时差分扫描确认为 0x8E08DC
    # （结构不变：+0x5C=选中数量，+0x50=选中单位链表头，节点+8 → 对象+0x138=单位实体）
    ('GetUnitDataBase',    0x8E08DC),
    ('GetUnitData2This',   0x8E6C58),
    ('GetMouseXYZinMapBase', 0x8DAEFC),
    ('UnitIDMagic',        0x8E9838),
]

if __name__ == '__main__':
    for name, rva, aob, target in CORE_HOOKS:
        print('%-15s va=0x%08X len=%d jmp=MustCode+0x%X' % (name, 0x400000+rva, len(aob)//2, target))
    print()
    print('total %d hooks' % len(CORE_HOOKS))

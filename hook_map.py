# -*- coding: utf-8 -*-
"""
红警3 Steam 版适配 —— hook 点映射表
由 RedAlert3_Trainer_1.12_FINAL3.exe 逆向 + Steam ra3_1.12.game 交叉分析生成

每个 hook: (名称, 原始RVA偏移, 原始指令(AOB), Steam新VA, 状态)
状态: ok=已唯一定位 / guess=候选待验证 / todo=未定位
"""

# 原始模块基址 0x400000，所有偏移均为 RVA
HOOKS = [
    # --- 已确定 (17) ---
    dict(name='PlayerID',       orig=0x0FF95B, aob='8b50288b4220',           new=0x54119B, status='ok'),
    dict(name='Money',          orig=0x6CFDFE, aob='0378048b11',             new=0xA64E9E, status='ok'),
    dict(name='Power',          orig=0x6CFD0D, aob='8b40048b8eb0030000',     new=0xA64DAD, status='ok'),
    dict(name='SCPoint',        orig=0x6CFE6C, aob='8b78348b4e3c',           new=0xA64F0C, status='ok'),
    dict(name='HaveAllSC',      orig=0x6CFEB5, aob='f30f10472c',             new=0xA64F55, status='ok'),
    dict(name='FastBuild1',     orig=0x30F42E, aob='f30f2c461c',             new=0x74D81E, status='ok'),
    dict(name='FastBuild2',     orig=0x30F38C, aob='d9461c89465c',           new=0x74D77C, status='ok'),
    dict(name='FastBuild3',     orig=0x2F6CFF, aob='d986bc010000',           new=0x73511F, status='ok'),
    dict(name='SuperPower',     orig=0x2EBB69, aob='8b9818040000',           new=0x729F19, status='ok'),
    dict(name='SuperPower2',    orig=0x43EC19, aob='8b70508b01',             new=0x87CB89, status='ok'),
    dict(name='DisableAllSP',   orig=0x43ECA6, aob='8b51503b500c',           new=0x87CC16, status='ok'),
    dict(name='DisableAllSP2',  orig=0x30A5F6, aob='8b48503b4e20',           new=0x748A06, status='ok'),
    dict(name='Map',            orig=0x3C226C, aob='f30f118560020000',       new=0x8005BC, status='ok'),
    dict(name='UnitAmmo',       orig=0x128735, aob='8b0cba85c9',             new=0x569D85, status='ok'),
    dict(name='DangerLevel',    orig=0x438ED0, aob='8b8178120000',           new=0x877070, status='ok'),
    dict(name='OreMine',        orig=0x2D4933, aob='8b40082b4114',           new=0x712EB3, status='ok'),
    dict(name='EnemyCantBuild', orig=0x30F530, aob='0350043bd7',             new=0x74D920, status='ok'),
    # --- 待定 (5) ---
    dict(name='Zoom',           orig=0x1EC7CD, aob='f30f114644',             new=0x000000, status='todo'),
    dict(name='GodMode',        orig=0x12EEDF, aob='8b503c8bce',             new=0x000000, status='todo'),
    dict(name='OneKill',        orig=0x3651AE, aob='f30f114604',             new=0x000000, status='todo'),
    dict(name='OneKillData',    orig=0x3FE714, aob='8b8e3c030000',           new=0x000000, status='todo'),
    dict(name='OneKillData2',   orig=0x2E24E3, aob='8b893c030000',           new=0x000000, status='todo'),
]

# 内部函数调用 (用于高级功能: 选中单位升级/摧毁/创建/复制)
FUNCS = [
    dict(name='SelectUnitLevelUp', orig=0x35C200),
    dict(name='DestroySelectUnit', orig=0x39EA50),
    dict(name='GetUnitData2',      orig=0x3E4230),
    dict(name='CreateUnit',        orig=0x205240),
    dict(name='GetMouseXYZinMap',  orig=0x1ED4A0),
]

# 全局指针/变量
# 4 个在 .data 段(VirtAddr=RawPtr=0x8AB000), 数据段布局未重排, RVA 不变, 引用无需修改
# Mustcode1200 ptr(0xE7565) 在 .text 代码段, 需重定位(待定)
GLOBALS = [
    dict(name='GetUnitData base',     orig=0x8E08DC, section='.data',  need_reloc=False),
    dict(name='GetUnitData2 this',    orig=0x8E6C58, section='.data',  need_reloc=False),
    dict(name='GetMouseXYZinMap base', orig=0x8DAEFC, section='.data', need_reloc=False),
    dict(name='UnitID magic',         orig=0x8E9838, section='.data',  need_reloc=False),
    dict(name='Mustcode1200 ptr',     orig=0xE7565,  section='.text',  need_reloc=True),
]

if __name__ == '__main__':
    print('确定的 hook: %d / %d' % (sum(1 for h in HOOKS if h['status']=='ok'), len(HOOKS)))
    for h in HOOKS:
        tag = {'ok':'OK ','guess':'?  ','todo':'---'}[h['status']]
        if h['new']:
            print('  %s %-15s orig=0x%06X -> steam=0x%08X  aob=%s' % (tag, h['name'], h['orig'], h['new'], h['aob']))
        else:
            print('  %s %-15s orig=0x%06X -> (待定)        aob=%s' % (tag, h['name'], h['orig'], h['aob']))

# -*- coding: utf-8 -*-
"""离线自检：不需要游戏，用假内存验证指针链读取 + 地址换算 + 功能接线。"""
import io
import re
import struct

import trainer

# ---------- 1. 地址换算 ----------
gp = trainer.GameProcess()
gp.module_base = trainer.MOD_BASE            # 0x400000
assert gp.mgr_addr() == 0x00CE08DC, hex(gp.mgr_addr())
# MustCode 里是 [ra3_1.12.game+8E08DC] → 绝对址必须是 0xCE08DC
assert 0x8E08DC + trainer.MOD_BASE == gp.mgr_addr()
# FN_ADD_XP 是 VA（ra3_1.12.game+0x1173F0），va_of 后应保持不变
assert gp.va_of(trainer.FN_ADD_XP) == 0x005173F0, hex(gp.va_of(trainer.FN_ADD_XP))
print('[OK] 地址换算：mgr=0x%08X  FN_ADD_XP=0x%08X' % (gp.mgr_addr(), gp.va_of(trainer.FN_ADD_XP)))

# ---------- 2. 用假内存验证 selected_entities ----------
class FakeGP(trainer.GameProcess):
    """节点+0 → 下一节点；节点+8 → 对象；对象+0x138 → 实体。"""

    def __init__(self, n=3):
        super().__init__()
        self.module_base = trainer.MOD_BASE
        self.mem = {}
        mgr = 0x045DE340
        head = 0x0F4C1E00
        self.w(mgr + 0x5C, n)
        self.w(self.mgr_addr(), mgr)
        self.w(mgr + 0x50, head)
        nodes = [head + 0x10 * i for i in range(n)]
        for i, nd in enumerate(nodes):
            self.w(nd, nodes[(i + 1) % n])          # 环形链表
            obj = 0x08169380 + 0x100 * i
            self.w(nd + 8, obj)
            self.w(obj + 0x138, 0x080E0840 + 0x100 * i)

    def w(self, addr, val):
        self.mem[addr] = val & 0xFFFFFFFF

    def read(self, addr, size):
        out = bytearray()
        for i in range(size):
            out += struct.pack('<B', (self.mem.get(addr + i, 0) >> (8 * (i % 4))) & 0xFF) \
                if (addr + i) in self.mem else b'\x00'
        return bytes(out)

    def read_u32(self, addr):
        return self.mem.get(addr)


f = FakeGP(4)
ents = f.selected_entities()
print('[OK] selected_entities ->', ['0x%08X' % e for e in ents])
assert len(ents) == 4, ents
assert all(0x080E0000 <= e <= 0x080F0000 for e in ents)

# 空管理器 / 链表头为空 / 对象指针无效，都必须安全返回 []
f2 = FakeGP(0)
f2.w(f2.mgr_addr(), 0)
assert f2.selected_entities() == []

f3 = FakeGP(2)
f3.w(0x045DE340 + 0x50, 0)              # 管理器的链表头字段为空
assert f3.selected_entities() == []

f4 = FakeGP(1)
f4.w(0x0F4C1E00 + 8, 0)                 # 节点 → 对象指针无效
assert f4.selected_entities() == []
print('[OK] 空/悬空指针安全返回 []')

# ---------- 3. 功能接线 ----------
shown = set()
for _, keys in trainer.FEATURE_GROUPS:
    shown.update(keys)
assert not [k for k, _, _, _ in trainer.FEATURES if k not in shown]
assert not [k for k in trainer.HOTKEYS
            if k not in trainer.FEATURE_BY_KEY and not k.startswith('danger_')]
assert trainer.HOTKEYS['unit_rank'] == ([], 0x50)              # p
assert trainer.FEATURE_BY_KEY['unit_rank'][2] == 'engine'
assert not [x for x in trainer.FEATURES if x[3].get('cmd') == 8], 'cmd=8 应已停用'
print('[OK] 功能接线：%d 个功能，unit_rank=%s' %
      (len(trainer.FEATURES), trainer.FEATURE_BY_KEY['unit_rank']))

src = io.open('trainer.py', encoding='utf-8').read()
blk = src[src.index('def rank_up_via_engine'):src.index('def detach_and_restore')]
# 只允许调 FN_ADD_XP（0x71B290 只在注释里出现，不能真被调用）
calls = re.findall(r'call_remote\(\s*self\.va_of\((\w+)\)', blk, re.S)
assert calls == ['FN_ADD_XP'], calls
assert 'FN_RECALC' not in blk
assert 'va_of(MGR_GLOBAL)' not in src, 'RVA 不能走 va_of'
print('[OK] rank_up_via_engine 只调 %s，且数据指针用 module_base+RVA' % calls)

# ---------- 4. 资源接线（图标） ----------
import os

assert callable(trainer.apply_icon)
assert callable(trainer._resource_dir)
res = trainer._resource_dir()
assert os.path.isdir(res), res
for fn in ('icon.ico', 'icon.png'):
    p = os.path.join(res, fn)
    assert os.path.exists(p), '缺少 %s（跑 python make_icon.py 生成）' % p
    assert os.path.getsize(p) > 0, p

from PIL import Image
sizes = sorted(Image.open(os.path.join(res, 'icon.ico')).info.get('sizes', []))
assert (16, 16) in sizes, sizes
assert (256, 256) in sizes, sizes
# 打包脚本必须把图标交给 PyInstaller（嵌进 exe + 带进 _MEIPASS）
bat = io.open('build.bat', encoding='ascii').read()
assert '--icon icon.ico' in bat, 'build.bat 未嵌入 exe 图标'
assert '--add-data "icon.ico;."' in bat and '--add-data "icon.png;."' in bat, \
    'build.bat 未把图标带进 onefile 解包目录'
assert 'make_icon.py' in bat, 'build.bat 未在缺图标时自动生成'
print('[OK] 图标资源：%d 帧 %s，build.bat 已接线' % (len(sizes), sizes[0]))

print('\n全部通过。')

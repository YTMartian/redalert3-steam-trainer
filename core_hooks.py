# -*- coding: utf-8 -*-
"""
核心功能版 hook 定义 —— 17 个已定位 hook 的完整注入信息
每个 hook: 名称 / Steam VA / 原始指令AOB(hex) / jmp目标(MustCode+偏移)

地址是从模块基址 0x400000 起算的绝对 VA（ra3_1.12.game 关闭了 ASLR，加载基址
固定，所以 VA 与 RVA 只差 0x400000）。注意下面 DATA_GLOBALS 存的是 RVA，不是 VA，
两种约定不能混用；本表的 AOB 已按 VA 从 ra3_1.12.game 逐条离线校验通过。
"""
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

_md = Cs(CS_ARCH_X86, CS_MODE_32)


def _is_insn_start(code, addr, text_va):
    """地址是否落在一条指令的起点（用向后 1..15 字节反解判断）。"""
    base_off = addr - text_va
    for back in range(1, 16):
        off = base_off - back
        if off < 0:
            break
        insns = list(_md.disasm(bytes(code[off:off + back + 1]), addr - back))
        if insns and insns[0].address == addr - back and insns[0].size == back:
            return True
    return False


def scan_hazards(module_path='ra3_1.12.game', hooks=None):
    """找落在某个 hook 补丁区间内部的 jmp/jcc/call。

    5 字节 E9 会把整段被偷的字节一起盖掉。如果同一函数里还有分支落在
    [站点, 站点+长度) 里面，那条路会跳进 E9 的位移字节，把 stub 本该重放的
    指令整条跳过去 —— 典型后果就是 eip=1 的 use-after-free 式崩溃。
    Power / SCPoint 原来就是这样，所以现在都从分支头开始挂。
    """
    import struct
    hooks = hooks or CORE_HOOKS
    pe = pefile.PE(module_path, fast_load=True)
    data = open(module_path, 'rb').read()
    base = pe.OPTIONAL_HEADER.ImageBase
    text_va, code = None, None
    for sec in pe.sections:
        if sec.Name.startswith(b'.text'):
            text_va = base + sec.VirtualAddress
            code = data[sec.PointerToRawData:sec.PointerToRawData + sec.SizeOfRawData]
            break
    if code is None:
        return []
    ranges = [(va, va + len(aob) // 2, name) for name, va, aob, _ in hooks]
    hits = []
    for i in range(len(code) - 5):
        b = code[i]
        tgt = None
        if b in (0xE8, 0xE9):
            tgt = text_va + i + 5 + struct.unpack_from('<i', code, i + 1)[0]
        elif b == 0x0F and 0x80 <= code[i + 1] <= 0x8F:
            tgt = text_va + i + 6 + struct.unpack_from('<i', code, i + 2)[0]
        elif b == 0xEB:
            tgt = text_va + i + 2 + struct.unpack_from('<b', code, i + 1)[0]
        elif 0x70 <= b <= 0x7F:
            tgt = text_va + i + 2 + struct.unpack_from('<b', code, i + 1)[0]
        if tgt is None:
            continue
        src = text_va + i
        for lo, hi, name in ranges:
            if lo < tgt < hi and not (lo <= src < hi) and _is_insn_start(code, src, text_va):
                hits.append((name, src, tgt))
    return hits


# 17 个已定位 hook（名称, Steam VA, AOB, jmp目标偏移）
# jmp 目标偏移 = MustCode 块内偏移（十六进制）
CORE_HOOKS = [
    ('PlayerID',       0x54119B, '8b50288b4220',          0x1200),
    ('Money',          0xA64E9E, '0378048b11',            0x29),
    # Power/SCPoint 从分支头开始挂：原来的站点把函数内部 jmp 的落点
    # （0xA64DB0 / 0xA64F0F）包进了 E9 补丁，非空分支跳进补丁中间会跳过
    # 寄存器重载，进而 call 到错误对象。详见 README。
    ('Power',          0xA64DA7, '740433c0eb038b40048b8eb0030000', 0x6c),
    ('SCPoint',        0xA64EF6, '85c0750433ffeb118b802013000085c0750433ffeb038b7834', 0x9f),
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

# 已定位的全局数据指针（**RVA**，不是 VA；VA = RVA + 0x400000，如 0x8E08DC → 0xCE08DC）
DATA_GLOBALS = [
    # Steam 版重定位：旧 0x8DB73C 已无任何代码引用；新地址经运行时差分扫描确认为 0x8E08DC
    # （结构不变：+0x5C=选中数量，+0x50=选中单位链表头，节点+8 → 对象+0x138=单位实体）
    ('GetUnitDataBase',    0x8E08DC),
    ('GetUnitData2This',   0x8E6C58),
    ('GetMouseXYZinMapBase', 0x8DAEFC),
    ('UnitIDMagic',        0x8E9838),
]

if __name__ == '__main__':
    for name, va, aob, target in CORE_HOOKS:
        print('%-15s va=0x%08X len=%d jmp=MustCode+0x%X' % (name, va, len(aob)//2, target))
    print()
    print('total %d hooks' % len(CORE_HOOKS))
    import os
    if os.path.exists('ra3_1.12.game'):
        hits = scan_hazards()
        if hits:
            print()
            print('[警告] 这些分支会落进补丁区间，该 hook 应往前挪到分支头：')
            for name, src, tgt in hits:
                print('  %-13s 分支 %08X -> %08X' % (name, src, tgt))
        else:
            print()
            print('补丁区间内没有内部分支落点：OK')
    else:
        print()
        print('（当前目录没有 ra3_1.12.game，跳过分支落点检查）')

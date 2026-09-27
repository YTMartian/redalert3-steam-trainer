# -*- coding: utf-8 -*-
"""
单位星级（veterancy）字段定位工具（只读，不写入任何游戏内存）。

背景：
    原修改器脚本里的「单位升级」依赖零售版内部函数 ra3_1.12.game+35C200，
    但 Steam 版是重新编译的二进制，该地址在 Steam 版是一个无关的小函数
    （mov al,[ecx+0x112]; ret），直接调用会破坏栈和内存。
    因此必须在 Steam 版里重新定位「星级」字段，然后直接写值。

原理：
    同一个单位从「新兵(0星)」升到「老兵/精英」时，它的 Object/实体内存里
    会有一个小整数字段从 0 变成 1 或 2。本工具保存两次快照并自动 diff。

用法：
    1. 启动红警3进入遭遇战，选中 1 个单位（保持选中）。
    2. 运行「定位单位星级.bat」，脚本保存快照 A。
    3. 让该单位升级（打几个敌人 / 捡遭遇战里的升级箱子）。
    4. 保持选中该单位，再次运行脚本，自动 diff 并输出变化的字段。
    5. 把输出结果发给开发者，即可实现「一键升到满级」。

输出文件：find_veterancy_result.txt
"""
import json
import os
import struct
import sys

from trainer import GameProcess

MGR_GLOBAL = 0x8E08DC
SNAP = 'veterancy_snap.json'
OUT_FILE = 'find_veterancy_result.txt'

OUT = []


def log(s):
    print(s)
    OUT.append(s)


def is_ptr(v):
    return v is not None and 0x10000 <= v < 0x7FFF0000 and (v & 3) == 0


# 需要对比的内存区域：(标签, 基址取值函数, 长度)
RANGES = [
    ('Object',        lambda env: env['obj'],      0x600),
    ('实体',          lambda env: env['entity'],   0x500),
    ('血量对象',      lambda env: env['health'],   0x40),
    ('速度组件',      lambda env: env['speed'],    0x300),
    ('实体+0x3CC对象', lambda env: env['e3cc'],    0x200),
    ('Object+0x3CC对象', lambda env: env['o3cc'],  0x200),
]


def main():
    gp = GameProcess()
    ok, err = gp.attach()
    if not ok:
        log('[!] 附加失败：' + err)
        return 1
    log('[+] 已附加：PID=%d  基址=0x%X' % (gp.pid, gp.module_base))

    def r32(a):
        b = gp.read(a, 4)
        return struct.unpack('<I', b)[0] if len(b) == 4 else None

    g = gp.module_base + MGR_GLOBAL
    mgr = r32(g)
    if not is_ptr(mgr):
        log('[!] 选中的单位管理器指针无效。请先进入遭遇战并选中 1 个单位。')
        return 2
    count = r32(mgr + 0x5C)
    node = r32(mgr + 0x50)
    if not is_ptr(node):
        log('[!] 首节点无效。请确认已选中至少 1 个单位。')
        return 3
    if count and count != 1:
        log('[!] 当前选中 %d 个单位。本工具需要「只选中 1 个单位」以保证指针稳定。' % count)
        return 4
    obj = r32(node + 8)
    entity = r32(obj + 0x138) if is_ptr(obj) else None
    if not is_ptr(entity):
        log('[!] 单位实体无效。')
        return 5

    env = {
        'obj': obj,
        'entity': entity,
        'health': r32(entity + 0x33C),
        'speed': r32(entity + 0x374),
        'e3cc': r32(entity + 0x3CC),
        'o3cc': r32(obj + 0x3CC) if is_ptr(obj) else None,
    }
    log('')
    log('===== 当前单位指针 =====')
    for k, v in env.items():
        log('  %-8s = 0x%s' % (k, ('%08X' % v) if is_ptr(v) else '??'))

    # ---- 采集快照 ----
    snap = {'pids': gp.pid, 'env': {k: v for k, v in env.items()}, 'ranges': {}}
    for label, getter, size in RANGES:
        base = getter(env)
        if not is_ptr(base):
            snap['ranges'][label] = None
            continue
        raw = gp.read(base, size)
        snap['ranges'][label] = {'base': base, 'hex': raw.hex()}
        log('  采集 %-12s 0x%08X  %d 字节' % (label, base, len(raw)))

    # ---- 与上一次快照对比 ----
    if not os.path.exists(SNAP):
        open(SNAP, 'w', encoding='utf-8').write(json.dumps(snap))
        log('')
        log('===== 已保存快照 A（首次运行）=====')
        log('  请让该单位升级（打敌人 / 捡升级箱子），保持选中，然后再次运行本工具。')
        log('  结果文件：' + OUT_FILE)
        open(OUT_FILE, 'w', encoding='utf-8').write('\n'.join(OUT))
        return 0

    old = json.load(open(SNAP, encoding='utf-8'))
    open(SNAP, 'w', encoding='utf-8').write(json.dumps(snap))

    log('')
    log('===== 差分结果（快照 A -> B）=====')
    # 指针本身是否有变化
    for k in env:
        if old['env'].get(k) != env.get(k):
            log('  [指针变化] %s: 0x%s -> 0x%s'
                % (k, old['env'].get(k), env.get(k)))

    changes = []
    for label, getter, size in RANGES:
        o = old['ranges'].get(label)
        n = snap['ranges'].get(label)
        if not o or not n:
            continue
        ob = bytes.fromhex(o['hex'])
        nb = bytes.fromhex(n['hex'])
        if len(ob) != len(nb):
            log('  [%s] 长度不同，跳过' % label)
            continue
        for off in range(0, min(len(ob), len(nb)), 4):
            ov = struct.unpack_from('<I', ob, off)[0]
            nv = struct.unpack_from('<I', nb, off)[0]
            if ov != nv:
                # 过滤明显的指针跳动（两侧都是合法指针）
                kind = ''
                if is_ptr(ov) and is_ptr(nv):
                    kind = '（指针）'
                elif ov < 0x1000 and nv < 0x1000:
                    kind = ' ★小整数候选'
                changes.append((label, off, ov, nv, kind))

    if not changes:
        log('  没有发现任何字段变化。可能该单位并未升级，或升级数据不在这些区域内。')
    for label, off, ov, nv, kind in changes:
        of = struct.unpack('<f', struct.pack('<I', ov))[0]
        nf = struct.unpack('<f', struct.pack('<I', nv))[0]
        log('  [%s] +0x%03X: 0x%08X(%d / %.4f) -> 0x%08X(%d / %.4f) %s'
            % (label, off, ov, ov, of, nv, nv, nf, kind))

    # ---- 血量对比：用于确认单位是否真的升级 ----
    def fl(blob, off):
        if off + 4 > len(blob):
            return None
        return struct.unpack_from('<f', blob, off)[0]

    o = old['ranges'].get('血量对象')
    n = snap['ranges'].get('血量对象')
    if o and n:
        ob = bytes.fromhex(o['hex'])
        nb = bytes.fromhex(n['hex'])
        log('')
        log('----- 血量对比（用于确认单位是否真的升级）-----')
        for off, nm in ((4, '当前HP'), (0xC, 'HP上限2'), (0x10, 'HP上限')):
            log('  +0x%02X %-8s: %s  ->  %s' % (off, nm, fl(ob, off), fl(nb, off)))
        if fl(ob, 0x10) == fl(nb, 0x10):
            log('  [提示] HP 上限没有变化。RA3 升级通常会提升最大血量，')
            log('         如果这里没变，很可能该单位这次并没有真的升级。')
        else:
            log('  [OK] HP 上限发生变化，说明该单位确实升级了，结果可信。')

    log('')
    log('  说明：请重点看标了「★小整数候选」且从 0 变成 1/2、或从 1 变成 2 的字段。')
    log('  把本文件发给开发者即可实现「一键升到满级」。')

    open(OUT_FILE, 'w', encoding='utf-8').write('\n'.join(OUT))
    print('\n[+] 已保存 ' + OUT_FILE)
    return 0


if __name__ == '__main__':
    sys.exit(main())

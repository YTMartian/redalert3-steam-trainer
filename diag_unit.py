# -*- coding: utf-8 -*-
"""
单位操作偏移诊断工具（只读，不写入游戏内存，绝不会导致闪退）。

用途：
    定位 Steam 版「单位实体」到「血量对象」「速度对象」的正确偏移，
    修正修改器里可能是旧版编译的 +0x33C / +0x374 / +0x200。

用法：
    1. 启动红警3，进入遭遇战，在游戏里选中 1 个单位。
    2. 以管理员权限运行「诊断单位偏移.bat」。
    3. 结果自动保存到 diag_unit_result.txt，发给开发者。
"""
import struct
import sys

from trainer import GameProcess

MGR_GLOBAL = 0x8E08DC
OUT = []


def log(s):
    print(s)
    OUT.append(s)


def is_ptr(v):
    return v is not None and 0x10000 <= v < 0x7FFF0000 and (v & 3) == 0


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

    def rf(a):
        b = gp.read(a, 4)
        return struct.unpack('<f', b)[0] if len(b) == 4 else None

    def dump(addr, size, title, base_off=0):
        log('  --- %s (0x%08X, %d 字节) ---' % (title, addr, size))
        raw = gp.read(addr, size)
        for r in range(0, len(raw), 16):
            line = '    +0x%03X: ' % (base_off + r)
            for k in range(0, 16, 4):
                if r + k + 4 <= len(raw):
                    line += '%08X ' % struct.unpack_from('<I', raw, r + k)[0]
            log(line)

    g = gp.module_base + MGR_GLOBAL
    mgr = r32(g)
    log('')
    log('===== 指针链 =====')
    log('  [0x%08X] = 0x%s   <- 选中单位管理器' % (g, '%08X' % mgr if mgr else '??'))
    if not is_ptr(mgr):
        log('  [!] 管理器指针无效，请确认已进入遭遇战并选中单位。')
        return 2

    count = r32(mgr + 0x5C)
    node = r32(mgr + 0x50)
    log('  管理器 0x%08X  +0x50=0x%s  +0x5C(数量)=%s'
        % (mgr, ('%08X' % node) if node else '??', count))
    if not is_ptr(node):
        log('  [!] 首节点无效。')
        return 3

    data = r32(node + 8)
    entity = r32(data + 0x138) if is_ptr(data) else None
    log('  节点 0x%08X  +0x8=0x%s   ->  +0x138=0x%s   <- 单位实体'
        % (node, ('%08X' % data) if data else '??', ('%08X' % entity) if entity else '??'))
    if not is_ptr(entity):
        log('  [!] 单位实体无效。')
        return 4

    # ---- 速度链 ----
    log('')
    log('===== 速度链验证 =====')
    P = r32(entity + 0x374)
    log('  实体+0x374 = 0x%s' % (('%08X' % P) if P else '??'))
    if is_ptr(P):
        Q = r32(P + 0x200)
        log('    该对象 +0x200 = 0x%s' % (('%08X' % Q) if Q else '??'))
        if is_ptr(Q):
            R = r32(Q)
            log('      +0x0 = 0x%s' % (('%08X' % R) if R else '??'))
            if is_ptr(R):
                log('        R+0x8 = %s   R+0x40 = %s'
                    % (rf(R + 8), rf(R + 0x40)))
        # 直接 dump 该组件对象，便于人工核对正确偏移
        dump(P, 0x300, '[实体+0x374] 组件对象')

    # ---- 血量链 ----
    log('')
    log('===== 血量链验证 =====')
    T = r32(entity + 0x33C)
    log('  实体+0x33C = 0x%s' % (('%08X' % T) if T else '??'))
    if is_ptr(T):
        log('    +0x0=%s +0x4=%s +0x8=%s +0xC=%s +0x10=%s'
            % (rf(T), rf(T + 4), rf(T + 8), rf(T + 0xC), rf(T + 0x10)))
        dump(T, 0x40, '[实体+0x33C] 血量对象（含浮点解释）')

    # ---- 自动搜索速度对象：实体 -> P -> (P+o2) -> Q -> [Q]=R, R+8 浮点 ----
    log('')
    log('===== 自动搜索速度对象（两层指针，末级 +0x8 为倍率浮点）=====')
    ESZ = 0x800
    ebuf = gp.read(entity, ESZ)
    ent = list(struct.unpack('<%dI' % (len(ebuf) // 4), ebuf))
    found = 0
    for o1 in range(0, len(ent) * 4, 4):
        P1 = ent[o1 // 4]
        if not is_ptr(P1):
            continue
        pb = gp.read(P1, 0x400)
        if len(pb) < 0x400:
            continue
        pl = list(struct.unpack('<%dI' % (0x400 // 4), pb))
        for o2 in range(0, 0x400, 4):
            Q1 = pl[o2 // 4]
            if not is_ptr(Q1):
                continue
            R1 = r32(Q1)
            if not is_ptr(R1):
                continue
            f = rf(R1 + 8)
            if f is None or not (0.0001 < abs(f) < 1000):
                continue
            log('  实体+0x%03X -> 0x%08X, +0x%03X -> 0x%08X, [Q]=0x%08X, R+0x8=%.4f'
                % (o1, P1, o2, Q1, R1, f))
            found += 1
            if found >= 25:
                break
        if found >= 25:
            break
    if not found:
        log('  （未找到）')

    # ---- 自动搜索血量对象：实体+o -> T，T+4 / T+0x10 为合理浮点 ----
    log('')
    log('===== 自动搜索血量对象（+4 与 +0x10 均为 1..1e7 的浮点）=====')
    hf = 0
    for o in range(0, len(ent) * 4, 4):
        P1 = ent[o // 4]
        if not is_ptr(P1):
            continue
        f4 = rf(P1 + 4)
        f10 = rf(P1 + 0x10)
        fc = rf(P1 + 0xC)
        if f4 is None or f10 is None:
            continue
        if 1.0 <= f4 <= 1e7 and 1.0 <= f10 <= 1e7:
            log('  实体+0x%03X -> 0x%08X  +4=%.2f  +0xC=%s  +0x10=%.2f'
                % (o, P1, f4, ('%.2f' % fc) if fc is not None else '??', f10))
            hf += 1
            if hf >= 25:
                break
    if not hf:
        log('  （未找到）')

    # ---- 实体内存快照 ----
    log('')
    dump(entity, 0x400, '单位实体内存', 0)

    log('')
    log('诊断完成。结果已保存到 diag_unit_result.txt，请发给开发者。')
    open('diag_unit_result.txt', 'w', encoding='utf-8').write('\n'.join(OUT))
    print('\n[+] 已保存 diag_unit_result.txt')
    return 0


if __name__ == '__main__':
    sys.exit(main())

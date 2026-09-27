# -*- coding: utf-8 -*-
"""
星级字段探测工具（读 + 可选写入，交互式）。

用途：
    「满级(3星)」按钮写入 [[实体+0x3CC]+0x24]=3 后游戏里星级没变化，
    需要确认两件事：
      1) 写入到底有没有落进内存（读回 +0x24 看是否变成 3）；
      2) 该组件里哪个字段才是驱动星级显示的权威字段。

做法：
    第 1 次读取 -> 你在修改器里点「满级(3星)」-> 第 2 次读取 -> 自动对比。
    之后可选：把「经验值候选 +0x10」写大，再让你看游戏里星级是否变化。
    所有改动都会记录原值，结束时询问是否还原。

用法：
    1. 启动红警3进入遭遇战，只选中 1 个单位并保持选中。
    2. 修改器已「附加游戏」。
    3. 管理员身份运行「探测星级字段.bat」。

输出文件：rank_probe_result.txt
"""
import struct
import sys

from trainer import GameProcess

MGR_GLOBAL = 0x8E08DC
OUT = []
OUT_FILE = 'rank_probe_result.txt'
VET_SIZE = 0x120

# 本组件内我们认为与星级/经验相关的字段
RANK_OFF = 0x24
XP_OFF = 0x10
OTHER_XP_OFF = 0x0C


def log(s):
    print(s)
    OUT.append(s)


def is_ptr(v):
    return v is not None and 0x10000 <= v < 0x7FFF0000 and (v & 3) == 0


def save():
    open(OUT_FILE, 'w', encoding='utf-8').write('\n'.join(OUT))


def main():
    gp = GameProcess()
    ok, err = gp.attach()
    if not ok:
        log('[!] 附加失败：' + err)
        save()
        return 1
    log('[+] 已附加：PID=%d  基址=0x%X' % (gp.pid, gp.module_base))
    log('    提示：如果修改器没附加，请先点「附加游戏」，否则读不到正确数据。')

    def r32(a):
        b = gp.read(a, 4)
        return struct.unpack('<I', b)[0] if len(b) == 4 else None

    def chain():
        g = gp.module_base + MGR_GLOBAL
        mgr = r32(g)
        if not is_ptr(mgr):
            return None, '单位管理器指针无效（请先进入遭遇战并选中单位）'
        count = r32(mgr + 0x5C)
        node = r32(mgr + 0x50)
        if not is_ptr(node):
            return None, '首节点无效'
        obj = r32(node + 8)
        if not is_ptr(obj):
            return None, '对象无效'
        ent = r32(obj + 0x138)
        if not is_ptr(ent):
            return None, '单位实体无效'
        vet = r32(ent + 0x3CC)
        if not is_ptr(vet):
            return None, '星级组件 [实体+0x3CC] 为空（该单位类型可能没有此组件）'
        return dict(count=count, obj=obj, ent=ent, vet=vet), None

    def dump_vet(vet, title):
        log('')
        log('--- %s  (组件 0x%08X) ---' % (title, vet))
        raw = gp.read(vet, VET_SIZE)
        for off in range(0, len(raw), 4):
            if off + 4 > len(raw):
                break
            v = struct.unpack_from('<I', raw, off)[0]
            sv = struct.unpack_from('<i', raw, off)[0]
            f = struct.unpack_from('<f', raw, off)[0]
            mark = ''
            if off == RANK_OFF:
                mark = '  <== 星级候选(原脚本升级用)'
            elif off == XP_OFF:
                mark = '  <== 经验值候选(差分里 150->300)'
            elif off == OTHER_XP_OFF:
                mark = '  <== 差分里 1.0->450.0'
            log('  +0x%03X  int=%-11d 0x%08X  float=%-16.4f%s'
                % (off, sv, v, f, mark))

    # ---- 第 1 次读取 ----
    info, err = chain()
    if err:
        log('[!] ' + err)
        save()
        return 2
    log('')
    log('===== 指针链 =====')
    log('  选中数量=%s  对象=0x%08X  实体=0x%08X  星级组件=0x%08X'
        % (info['count'], info['obj'], info['ent'], info['vet']))

    vet1 = info['vet']
    raw1 = gp.read(vet1, VET_SIZE)
    dump_vet(vet1, '第 1 次读取（操作前）')
    log('')
    log('  >>> 重点关注：+0x%02X 星级候选 = %d ；+0x%02X 经验候选 = %d'
        % (RANK_OFF, struct.unpack_from('<i', raw1, RANK_OFF)[0],
           XP_OFF, struct.unpack_from('<i', raw1, XP_OFF)[0]))

    # ---- 请用户点按钮 ----
    log('')
    log('=' * 58)
    log('请在修改器里点击「满级(3星)」按钮（或按 ` 键），')
    log('然后回到这个窗口按回车继续。也可以先在游戏里确认它不生效。')
    log('=' * 58)
    try:
        input('   （点击完成后按回车）...')
    except EOFError:
        pass

    # ---- 第 2 次读取 ----
    info2, err = chain()
    if err:
        log('[!] 再次读取失败：' + err)
        save()
        return 3
    vet2 = info2['vet']
    raw2 = gp.read(vet2, VET_SIZE)
    dump_vet(vet2, '第 2 次读取（点击「满级」之后）')

    log('')
    log('===== 变化对比 =====')
    if vet2 != vet1:
        log('  [!] 星级组件地址变了：0x%08X -> 0x%08X（可能换了单位或被重新分配）' % (vet1, vet2))
    changed = 0
    for off in range(0, min(len(raw1), len(raw2)), 4):
        a = struct.unpack_from('<I', raw1, off)[0]
        b = struct.unpack_from('<I', raw2, off)[0]
        if a != b:
            changed += 1
            log('  +0x%03X  %d -> %d' % (off, struct.unpack_from('<i', raw1, off)[0],
                                        struct.unpack_from('<i', raw2, off)[0]))
    if not changed:
        log('  （没有任何字段发生变化）')

    rank_after = struct.unpack_from('<i', raw2, RANK_OFF)[0]
    log('')
    if rank_after == 3:
        log('[OK] 写入成功落地：+0x%02X 已经变成 %d。' % (RANK_OFF, rank_after))
        log('     说明「写内存」这条路是通的，但游戏显示不由 +0x%02X 驱动，' % RANK_OFF)
        log('     接下来测试是否由「经验值」驱动。')
    else:
        log('[!] +0x%02X 仍然是 %d，写入没有落地。' % (RANK_OFF, rank_after))
        log('    可能是该单位类型的星级组件不含此字段，或写入路径有问题。')

    # ---- 可选：写经验值试验 ----
    log('')
    log('-' * 58)
    try:
        ans = input('是否尝试把「经验值候选 +0x%02X」写大来测试？（y/N）: ' % XP_OFF)
    except EOFError:
        ans = 'n'
    if ans.strip().lower() in ('y', 'yes', '是'):
        rank_save = struct.unpack_from('<I', raw2, RANK_OFF)[0]
        xp_save = struct.unpack_from('<I', raw2, XP_OFF)[0]
        other_save = struct.unpack_from('<I', raw2, OTHER_XP_OFF)[0]
        log('  [原值] +0x%02X=%d  +0x%02X=%d  +0x%02X=%d'
            % (RANK_OFF, struct.unpack_from('<i', raw2, RANK_OFF)[0],
               XP_OFF, struct.unpack_from('<i', raw2, XP_OFF)[0],
               OTHER_XP_OFF, struct.unpack_from('<i', raw2, OTHER_XP_OFF)[0]))

        # 星级拉满 + 经验值拉大，两个一起写以覆盖“由经验推导星级”的情况
        gp.write(vet2 + RANK_OFF, struct.pack('<I', 3))
        gp.write(vet2 + XP_OFF, struct.pack('<I', 1000000))
        log('  已写入：+0x%02X = 3 ，+0x%02X = 1000000' % (RANK_OFF, XP_OFF))
        raw3 = gp.read(vet2, VET_SIZE)
        log('  读回：+0x%02X=%d  +0x%02X=%d'
            % (RANK_OFF, struct.unpack_from('<i', raw3, RANK_OFF)[0],
               XP_OFF, struct.unpack_from('<i', raw3, XP_OFF)[0]))

        log('')
        log('  >>> 请切回游戏，看该单位星级有没有变化（可能需要几秒刷新/移动一下）。')
        try:
            ans2 = input('  星级变成英雄级了吗？（y=成功 / 回车=没变化）: ')
        except EOFError:
            ans2 = ''
        if ans2.strip().lower() in ('y', 'yes', '是'):
            log('  [OK] 经验值/星级写入生效！把本文件发给开发者即可固化为按钮。')
            log('  保留本次修改（未还原）。')
        else:
            log('  仍未生效。')
            try:
                ans3 = input('  是否还原为原值？（Y/n）: ')
            except EOFError:
                ans3 = 'y'
            if ans3.strip().lower() not in ('n', 'no', '否'):
                gp.write(vet2 + RANK_OFF, struct.pack('<I', rank_save))
                gp.write(vet2 + XP_OFF, struct.pack('<I', xp_save))
                gp.write(vet2 + OTHER_XP_OFF, struct.pack('<I', other_save))
                log('  已还原原值。')

    log('')
    log('探测完成。结果已保存到 ' + OUT_FILE + '，请发给开发者。')
    save()
    print('\n[+] 已保存 ' + OUT_FILE)
    return 0


if __name__ == '__main__':
    sys.exit(main())

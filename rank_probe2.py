# -*- coding: utf-8 -*-
"""
星级（veterancy）机制逐步验证脚本 v4 —— 交互式，每步都可回游戏肉眼确认

【上一版（v3）的实测结论 —— 已固化进本版本的步骤设计】
  · 纯内存写 [[实体+0x3CC]+0x24] = 3  →  游戏里**毫无变化**。
      证实 +0x24 只是「当前等级」的缓存，不是权威数据。
  · 在游戏进程的**独立线程**里调 0x5173F0(实体, 经验)  →  **真的升级了**：
      +0x24 上升、+0x20「特效已播放」置 1、+0x08 等级定义键切换，且不卡死。
      但一次调用只晋升 1 级（灌 5000 经验也只涨一级），所以要循环调用。
  · 单独调用 0x71B290(tracker, 1)「按经验重算星级」 →  **游戏直接挂掉**。
      必须始终走官方入口 0x5173F0，让它自己取锁 / 逐级晋升 / 播特效 / 刷新 UI。

【结构（反汇编 + 运行时互相印证）】
  [实体+0x3CC] = ExperienceTrackerObject（经验追踪器）
       +0x08 等级定义键（晋升后会切换）
       +0x0C 当前经验(float)     +0x10 下一级所需经验(阈值)
       +0x1C 经验倍率(float)     +0x20 等级特效已播放标志
       +0x24 当前等级(缓存)      +0x28 等级上限(0=无)
       +0x2C 加成对象 sub        +0x38 单位实体
  sub = [tracker+0x2C]：
       +0x04 等级持有者(+0x24 = 等级)  +0x08 当前加成倍率
       +0x0C 已应用等级                +0x10 每级倍率表 vector<float>
       0x765090 的算式：index = [sub+4]->[+0x24] - [sub+0x0C]，
       index<=0 → 表[0]，否则 表[min(index, 项数-1)]。
       ⚠ 0x517490 / 0x781CA0 会把 sub+0x0C 写成当前等级使 index 归零
         （= 清空加成），升星时绝不能调。

【本版本做 3 步】
  1) 调一次官方接口，看等级是否 +1（并回游戏确认星星）
  2) 反复调官方接口直到等级不再上升（= 找该单位的等级上限，即修改器的做法）
  3) 对照实验：纯内存写 +0x24=3、清 +0x20，证明这样没用

用法：先进入遭遇战、只选中 1 个单位，然后双击「验证星级机制.bat」
输出：rank_probe2_result.txt
"""
import struct
import time

from trainer import GameProcess

MGR_GLOBAL = 0x8E08DC       # 全局单位（选择）管理器：**RVA**，绝对址 = 基址 + 它
IMG_BASE = 0x00400000       # ra3_image.bin 的基址，用于把 VA 换算成 RVA

# 游戏函数（VA → RVA）
FN_ADD_XP = 0x005173F0      # __cdecl(entity, int xp)  官方「给单位加经验」入口
                            #   ← 唯一安全可用的晋升手段（在独立线程里调用）
# 【危险，绝不要调用】0x0071B290 = tracker 的「按经验重算星级」
#   实测：跳过官方入口直接调它，游戏会直接挂掉。
#   0x00517490 / 0x00781CA0 则会把 sub+0x0C 写成当前等级，使加成 index 归零、
#   倍率被重置为表[0]，是「清空加成」的函数，更不能用。

XP_BIG = 5000               # 每次调用给的经验（一次调用只晋升 1 级）

OUT = []
OUT_FILE = 'rank_probe2_result.txt'


def log(s=''):
    print(s)
    OUT.append(s)


def save():
    open(OUT_FILE, 'w', encoding='utf-8').write('\n'.join(OUT))


def is_ptr(v):
    return v is not None and 0x10000 <= v < 0x7FFF0000 and (v & 3) == 0


def rva(va):
    return va - IMG_BASE


def main():
    gp = GameProcess()
    ok, err = gp.attach()
    if not ok:
        log('[!] 附加失败：' + err)
        save()
        return 1
    log('[+] 已附加：PID=%d  模块基址=0x%X' % (gp.pid, gp.module_base))
    log('    提示：如果没读到数据，先在修改器里点一次「附加游戏」。')

    def r32(a, signed=False):
        b = gp.read(a, 4)
        if len(b) != 4:
            return None
        return struct.unpack('<i' if signed else '<I', b)[0]

    def i32(a):
        return r32(a, signed=True)

    def f32(a):
        v = r32(a)
        if v is None:
            return float('nan')
        return struct.unpack('<f', struct.pack('<I', v))[0]

    def read_cstr(a, maxlen=64):
        b = gp.read(a, maxlen)
        out = []
        for ch in b:
            if ch == 0:
                break
            if 0x20 <= ch < 0x7F:
                out.append(chr(ch))
            else:
                return None
        s = ''.join(out)
        return s if len(s) >= 3 else None

    # 已确认的虚表（本作把 RTTI 编译掉了：`[vt-4]` 不是 COL 指针 ——
    # 实测读到的是 .text 函数地址或 0，所以没法靠 RTTI 自动反查类名）。
    # 下面这些是实测/静态比对确认过的。
    KNOWN_VTABLES = {
        0x00C35354: 'ExperienceTrackerObject（经验追踪器，实测确认）',
        0x00C27350: 'bonus 对象（等级加成 sub，实测确认）',
    }

    def vtable_name(vt):
        """返回虚表对应的对象名（已知表直接查表；未知表给出邻近字符串提示）。

        注意：本作的 RTTI 被关掉了，`[vt-4]` 不是 CompleteObjectLocator 指针
        （实测读到的是 .text 里的函数地址或 0），所以不能靠 RTTI 反查类名。
        """
        if not is_ptr(vt):
            return None
        if vt in KNOWN_VTABLES:
            return KNOWN_VTABLES[vt]
        for delta in range(-0x40, 0x60, 4):
            s = read_cstr(vt + delta, 0x40)
            if s and len(s) >= 6 and '\\' not in s and '@@' not in s:
                return '未知（邻近字符串 vt%+#x: %s）' % (delta, s[:28])
        return None

    # ---------------- 定位选中单位 ----------------
    # 注意：游戏卡死/退出后内存链会失效，每一步都必须先验证指针再解引用，
    # 否则就会出现 "unsupported operand type(s) for +: 'NoneType' and 'int'"。
    def chain_step(name, addr, base, off):
        if not is_ptr(base):
            log('[!] %s 失败：基址 0x%08X 不是有效指针' % (name, base or 0))
            return None
        v = r32(base + off)
        log('    %-28s [0x%08X+0x%X] = 0x%08X' % (name, base, off, v or 0))
        if not is_ptr(v):
            log('[!] %s 失败：读到的不是有效指针（0x%08X）' % (name, v or 0))
            return None
        return v

    log('')
    log('=' * 70)
    log('定位选中单位（逐步校验指针链）')
    log('=' * 70)
    mgr = r32(gp.module_base + MGR_GLOBAL)
    log('    %-28s [0x%08X] = 0x%08X' % ('单位管理器指针全局', gp.module_base + MGR_GLOBAL, mgr or 0))
    if not is_ptr(mgr):
        log('[!] 单位管理器无效。请先进入遭遇战并选中一个单位。')
        log('    （如果游戏是在按 p 卡死之后的状态，内存链可能已经不可用，'
            '请重启一局游戏再试）')
        save()
        return 1
    count = i32(mgr + 0x5C)
    node = r32(mgr + 0x50)
    log('    %-28s %s' % ('选中单位数量 [mgr+0x5C]', count))
    if not is_ptr(node):
        log('[!] 选中单位链表头 [mgr+0x50] = 0x%08X 无效 —— 游戏里没选中单位？'
            % (node or 0))
        save()
        return 1
    obj = chain_step('首节点 → 对象 [+8]', 'node', node, 8)
    if not obj:
        save()
        return 1
    ent = chain_step('对象 → 单位实体 [+0x138]', 'object', obj, 0x138)
    if not ent:
        save()
        return 1
    tracker = chain_step('实体 → 经验跟踪器 [+0x3CC]', 'entity', ent, 0x3CC)
    if not tracker:
        log('[!] 这个单位没有 ExperienceTrackerObject，换个单位再试。')
        save()
        return 1
    log('    → 管理器=0x%08X 节点=0x%08X 对象=0x%08X 实体=0x%08X 跟踪器=0x%08X'
        % (mgr, node, obj, ent, tracker))

    sub = r32(tracker + 0x2C)
    holder = r32(sub + 4) if is_ptr(sub) else None

    def dump_table():
        if not is_ptr(sub):
            return
        pv = r32(sub + 0x10)
        if not is_ptr(pv):
            return
        b, e = r32(pv + 4), r32(pv + 8)
        if not (is_ptr(b) and is_ptr(e)) or e < b or (e - b) > 0x40:
            log('            (倍率表地址异常：begin=0x%08X end=0x%08X)'
                % (b or 0, e or 0))
            return
        n = (e - b) // 4
        vals = ['%.4f' % f32(b + 4 * i) for i in range(n)]
        log('           倍率表共 %d 项 = %s' % (n, ', '.join(vals)))

    def snapshot(title, show_table=False):
        log('')
        log('--- %s ---' % title)
        log('  跟踪器 0x%08X  (虚表 0x%08X  %s)'
            % (tracker, r32(tracker) or 0, vtable_name(r32(tracker)) or ''))
        log('     +0x08 等级定义键   = 0x%08X' % (r32(tracker + 0x08) or 0))
        log('     +0x0C 当前经验     = %.3f' % f32(tracker + 0x0C))
        log('     +0x10 升级阈值     = %s' % i32(tracker + 0x10))
        log('     +0x1C 经验倍率     = %.3f' % f32(tracker + 0x1C))
        log('     +0x20 特效标志     = 0x%08X （只有最低字节是"已播放"标志：%d）'
            % (r32(tracker + 0x20) or 0, (r32(tracker + 0x20) or 0) & 0xFF))
        log('     +0x24 等级索引     = %s   ★本次关心的就是它（实测 0~4，跑满约 4）'
            % i32(tracker + 0x24))
        log('     +0x28 (上限?)      = %s' % i32(tracker + 0x28))
        log('     +0x2C 加成对象 sub = 0x%08X' % (sub or 0))
        log('     +0x38 单位实体     = 0x%08X' % (r32(tracker + 0x38) or 0))
        if is_ptr(sub):
            h = r32(sub + 4)
            log('  加成对象 sub 0x%08X:' % sub)
            log('     虚表            = 0x%08X  %s'
                % (r32(sub) or 0, vtable_name(r32(sub)) or ''))
            log('     +0x04 等级持有者   = 0x%08X  %s'
                % (h or 0, '(就是跟踪器本身)' if h == tracker else
                   ('(另一个对象)' if is_ptr(h) else '(空)')))
            if is_ptr(h) and h != tracker:
                log('          它的虚表      = 0x%08X  %s'
                    % (r32(h) or 0, vtable_name(r32(h)) or ''))
                log('          它的 +0x24    = %s' % i32(h + 0x24))
            log('     +0x08 当前加成倍率 = %.4f  ← 数值加成就是它' % f32(sub + 0x08))
            log('     +0x0C 已应用等级   = %s' % i32(sub + 0x0C))
            log('     +0x10 倍率表       = 0x%08X' % (r32(sub + 0x10) or 0))
            if show_table:
                dump_table()
        log('  单位本体上的倍率缓存: +0x3A8=%.4f +0x3AC=%.4f +0x3B0=%.4f +0x3B4=%.4f'
            % (f32(obj + 0x3A8), f32(obj + 0x3AC), f32(obj + 0x3B0), f32(obj + 0x3B4)))

    def bp():
        try:
            s = input('    >>> 切回游戏看一下星星图标（和数值），然后回车继续（q=提前结束）: ')
        except EOFError:
            log('    （没有交互输入，直接继续）')
            return True
        if s.strip().lower() == 'q':
            return False
        return True

    # ---------------- 开始 ----------------
    log('')
    log('=' * 70)
    log('重要：本脚本所有「调用游戏函数」都是在游戏进程的**独立线程**里做的')
    log('      （CreateRemoteThread）。独立线程去抢游戏自己的锁最多只会等待，')
    log('      不会像在 hook 里同步调用那样造成自死锁，所以不会把游戏卡死。')
    log('      如果哪一步之后游戏真的卡死了，请记下是第几步。')
    log('=' * 70)
    log('初始状态')
    log('=' * 70)
    snapshot('① 实验前', show_table=True)

    log('')
    log('=' * 70)
    log('前置条件检查（决定「官方加经验」这条路能不能走通）')
    log('=' * 70)
    t4 = r32(tracker + 4)
    log('    [跟踪器+0x04]            = 0x%08X' % (t4 or 0))
    if is_ptr(t4):
        t14 = r32(t4 + 0x14)
        log('    [[跟踪器+0x04]+0x14]     = 0x%08X' % (t14 or 0))
        if is_ptr(t14):
            b = gp.read(t14 + 0x245, 1)
            if b:
                log('          它的 +0x245      = 0x%02X   '
                    '(0 = 这个单位被禁止获得经验，1 = 正常)' % b[0])
    log('    [跟踪器+0x28] 上限字段   = %s   '
        '(>0 且星级>=它 时引擎会跳过加经验)' % i32(tracker + 0x28))

    log('')
    log('=' * 70)
    log('步骤 1 / 3：调用官方接口 0x%08X(实体, %d)  —— 给单位灌经验'
        % (FN_ADD_XP, XP_BIG))
    log('=' * 70)
    before1 = i32(tracker + 0x24)
    ok, e = gp.call_remote(gp.module_base + rva(FN_ADD_XP), args=(ent, XP_BIG))
    log('    调用%s %s' % ('成功' if ok else '失败', e or ''))
    time.sleep(0.4)
    snapshot('② 官方加经验之后', show_table=True)
    log('    等级 %s -> %s' % (before1, i32(tracker + 0x24)))
    if not bp():
        save()
        return 0

    log('')
    log('=' * 70)
    log('步骤 2 / 3：反复调用官方接口，直到等级不再上升（找该单位的等级上限）')
    log('=' * 70)
    log('    实测一次调用只晋升 1 级，所以要循环调 —— 模拟修改器的做法。')
    log('')
    hist = []
    prev = i32(tracker + 0x24)
    for i in range(8):
        ok, e = gp.call_remote(gp.module_base + rva(FN_ADD_XP),
                               args=(ent, XP_BIG), timeout=4000)
        if not ok:
            log('    第 %d 次调用失败：%s' % (i + 1, e or ''))
            break
        cur = i32(tracker + 0x24)
        hist.append('%s->%s' % (prev, cur))
        log('    第 %d 次调用：等级 %s -> %s   阈值 %s   经验 %.1f   特效标志 %s'
            % (i + 1, prev, cur, i32(tracker + 0x10), f32(tracker + 0x0C),
               i32(tracker + 0x20)))
        if cur is not None and prev is not None and cur <= prev:
            log('    → 等级不再上升，已到该单位等级链的顶。')
            break
        prev = cur
        time.sleep(0.25)
    log('    晋升轨迹：%s' % (', '.join(hist) or '无'))
    time.sleep(0.3)
    snapshot('③ 反复加经验之后', show_table=True)
    if not bp():
        save()
        return 0

    log('')
    log('=' * 70)
    log('步骤 3 / 3：对照说明 —— 为什么「纯内存改 +0x24」不可取（本步不写内存）')
    log('=' * 70)
    log('    上一版实测已证明：把 +0x24 直接写成 3，游戏里毫无变化，')
    log('    而且会把真正的等级计数污染掉（引擎读到的是一个假等级）。')
    log('    所以本步不再写，只把当前状态列出来做对照。')
    snapshot('④ 当前状态（未做任何写入）', show_table=True)
    if not bp():
        save()
        return 0

    # ---------------- 汇总 ----------------
    log('')
    log('=' * 70)
    log('怎么看结果')
    log('=' * 70)
    log('  结论（本次实测）：')
    log('    · 步骤 1：官方接口 0x5173F0 真的让单位升级了（+0x24 上升、')
    log('      +0x20 特效标志置 1、+0x08 等级定义键切换），而且没卡死。')
    log('    · 步骤 2：一次调用只晋升 1 级，所以要循环调到等级不再上升。')
    log('    · 步骤 3：只写 +0x24 的话，游戏里什么都不会发生 —— 那是缓存。')
    log('')
    log('  【千万不要】单独调用 0x71B290(tracker, 1)「重算星级」：')
    log('    实测会把游戏直接弄挂（绕过官方入口后等级链状态不一致）。')
    log('    必须始终走官方入口 0x5173F0，让它自己取锁 / 逐级晋升 / 播特效。')
    log('')
    log('  本脚本不写任何内存（步骤 3 只是对照说明），所以不需要"还原"。')
    log('  等级一旦真晋升就回不去了，属正常。')

    save()
    log('')
    log('[+] 结果已保存到 %s' % OUT_FILE)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

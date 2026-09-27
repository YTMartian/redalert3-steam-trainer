# -*- coding: utf-8 -*-
"""
自动定位 Steam 版红警3「选中单位管理器」——全内存差分扫描版。

为什么需要它：
    Steam 版 ra3_1.12.game 与原版 1.12 是【不同的编译版本】，
    .data 全局指针地址与对象内部偏移都可能不同，旧常量 0x8DB73C 已失效。

原理（不需要 Cheat Engine）：
    1. 玩家依次选中 4 / 3 / 2 / 1 个单位。
    2. 每轮扫描进程全部可写内存，记录「值 == 选中数量」的 dword 地址。
    3. 四轮取交集 —— 唯一同时等于 4、3、2、1 的字段，
       就是「选中单位数量」字段地址 C。
    4. 反向搜索指向该对象(M = C - off)的指针，得到稳定的全局指针 + 偏移。

用法：
    1. 启动红警3（-runver 1.12），进入遭遇战并造好 >= 4 个单位。
    2. 以管理员权限运行「定位单位管理器.bat」。
    3. 严格按提示在游戏里选中指定数量的单位，每次回窗口按回车。
    4. 结果自动保存到 unit_mgr_result.txt，发给开发者即可。
"""
import ctypes
import struct
import sys
import time

try:
    import numpy as np
except Exception:
    np = None

from trainer import GameProcess

MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
WRITABLE = {0x04, 0x08, 0x40, 0x80}         # RW / WC / ERW / EWC
CHUNK = 0x200000                             # 2MB
ROUNDS = [4, 3, 2, 1]
MAX_OFF = 0x200                              # 对象内偏移搜索范围
MAX_CAND = 24                                # 最多处理多少个候选
OUT = []


def log(s):
    print(s)
    OUT.append(s)


class MBI(ctypes.Structure):
    pass


def _build_mbi():
    if ctypes.sizeof(ctypes.c_void_p) == 8:
        MBI._fields_ = [('BaseAddress', ctypes.c_ulonglong),
                        ('AllocationBase', ctypes.c_ulonglong),
                        ('AllocationProtect', ctypes.c_uint32),
                        ('__pad1', ctypes.c_uint32),
                        ('RegionSize', ctypes.c_ulonglong),
                        ('State', ctypes.c_uint32),
                        ('Protect', ctypes.c_uint32),
                        ('Type', ctypes.c_uint32),
                        ('__pad2', ctypes.c_uint32)]
    else:
        MBI._fields_ = [('BaseAddress', ctypes.c_uint32),
                        ('AllocationBase', ctypes.c_uint32),
                        ('AllocationProtect', ctypes.c_uint32),
                        ('RegionSize', ctypes.c_uint32),
                        ('State', ctypes.c_uint32),
                        ('Protect', ctypes.c_uint32),
                        ('Type', ctypes.c_uint32)]
    return ctypes.sizeof(MBI)


MBI_SIZE = _build_mbi()


def enum_regions(gp):
    k32 = ctypes.windll.kernel32
    k32.VirtualQueryEx.restype = ctypes.c_size_t
    k32.VirtualQueryEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.POINTER(MBI), ctypes.c_size_t]
    regions = []
    addr = 0
    limit = 0x80000000
    mbi = MBI()
    while addr < limit:
        r = k32.VirtualQueryEx(ctypes.c_void_p(gp.handle), ctypes.c_void_p(addr),
                               ctypes.byref(mbi), MBI_SIZE)
        if not r:
            break
        base = mbi.BaseAddress & 0xFFFFFFFF
        size = mbi.RegionSize & 0xFFFFFFFF
        if (mbi.State == MEM_COMMIT and (mbi.Protect & PAGE_NOACCESS) == 0
                and (mbi.Protect & PAGE_GUARD) == 0
                and (mbi.Protect & 0xFF) in WRITABLE):
            regions.append((base, size))
        nxt = base + (size if size else 0x1000)
        if nxt <= addr:
            nxt = addr + 0x1000
        addr = nxt
    return regions


def iter_chunks(gp, regions, tag=''):
    """逐块产生 (绝对地址, 原始字节)。"""
    total = sum(s for _, s in regions)
    done = 0
    for (base, size) in regions:
        pos = 0
        while pos < size:
            n = min(CHUNK, size - pos)
            buf = gp.read(base + pos, n)
            yield (base + pos, buf)
            pos += n
            done += n
        if tag:
            sys.stdout.write('      %s %d/%d MB\r' % (tag, done // 0x100000, total // 0x100000))
            sys.stdout.flush()


def scan_value(gp, regions, want):
    """扫描全部区域，返回所有 == want 的 dword 绝对地址。"""
    if np is not None:
        parts = []
        for (b, buf) in iter_chunks(gp, regions, '扫描中'):
            m = len(buf) // 4
            if m == 0:
                continue
            arr = np.frombuffer(buf[:m * 4], dtype='<i4')
            idx = np.nonzero(arr == want)[0]
            if idx.size:
                parts.append(b + idx.astype(np.uint32) * 4)
        if not parts:
            return np.array([], dtype=np.uint32)
        return np.unique(np.concatenate(parts))
    res = set()
    for (b, buf) in iter_chunks(gp, regions, '扫描中'):
        for i in range(len(buf) // 4):
            if struct.unpack_from('<i', buf, i * 4)[0] == want:
                res.add(b + i * 4)
    return sorted(res)


def read_i32(gp, a):
    d = gp.read(a, 4)
    return struct.unpack('<i', d)[0] if len(d) == 4 else None


def find_pointers(gp, regions, val2info):
    """一次扫描，找出所有值命中 val2info 的 dword（= 指向候选对象的指针）。"""
    hits = []
    keys = None
    if np is not None:
        keys = np.array(sorted(val2info.keys()), dtype=np.int64)
    else:
        keys = set(val2info.keys())
    for (b, buf) in iter_chunks(gp, regions, '反查中'):
        if np is not None:
            m = len(buf) // 4
            if m == 0:
                continue
            a64 = np.frombuffer(buf[:m * 4], dtype='<i4').astype(np.int64)
            idx = np.nonzero(np.isin(a64, keys))[0]
            for i in idx:
                hits.append((int(b + int(i) * 4), int(a64[i])))
        else:
            for i in range(len(buf) // 4):
                v = struct.unpack_from('<i', buf, i * 4)[0]
                if v in keys:
                    hits.append((b + i * 4, v))
    return hits


def main():
    if np is None:
        log('[i] 未检测到 numpy，扫描较慢（不影响正确性）。')

    gp = GameProcess()
    ok, err = gp.attach()
    if not ok:
        log('[!] 附加失败：' + err)
        return 1
    log('[+] 已附加：PID=%d  模块=%s  基址=0x%X' % (gp.pid, gp.game_module, gp.module_base))

    log('[+] 枚举可写内存区域...')
    regions = enum_regions(gp)
    total_mb = sum(s for _, s in regions) // 0x100000
    log('[+] 可写区域 %d 个，共约 %d MB' % (len(regions), total_mb))
    log('')

    cand = None
    for n in ROUNDS:
        try:
            input('>>> 请在游戏里【只选中 %d 个单位】（先点空地取消全部选中），再回窗口按回车... ' % n)
        except (EOFError, KeyboardInterrupt):
            log('\n[!] 已取消。')
            return 3
        time.sleep(0.6)
        log('    扫描内存：查找值 == %d 的字段...' % n)
        s = scan_value(gp, regions, n)
        log('      命中 %d 个' % len(s))
        if cand is None:
            cand = s
        elif np is not None:
            cand = np.intersect1d(cand, s)
        else:
            ss = set(s)
            cand = [x for x in cand if x in ss]
        log('      累积候选：%d' % len(cand))
        if len(cand) == 0:
            log('[!] 候选归零：请确认每轮选中的数量与提示完全一致，然后重试。')
            return 2
        if len(cand) <= MAX_CAND:
            log('    候选已足够少，停止扫描。')
            break

    clist = [int(x) for x in cand][:MAX_CAND]
    log('')
    log('===== 候选「选中单位数量」字段 =====')
    for c in clist:
        log('  0x%08X  当前值=%s' % (c, read_i32(gp, c)))
    log('')

    val2info = {}
    for c in clist:
        for off in range(0, MAX_OFF + 1, 4):
            m = c - off
            if 0x10000 <= m < 0x7FFF0000:
                val2info[m] = (c, off)
    log('[+] 反查指向候选对象的指针（一次全内存扫描，值集合 %d 个）...' % len(val2info))
    hits = find_pointers(gp, regions, val2info)
    log('[+] 命中指针 %d 个' % len(hits))
    log('')

    if hits:
        log('===== 可能的全局指针 =====')
        seen = set()
        for (slot, mval) in hits:
            c, off = val2info[mval]
            if (slot, mval) in seen:
                continue
            seen.add((slot, mval))
            rva = slot - gp.module_base
            indata = 0x8AB000 <= rva < 0x931000
            log('  指针 0x%08X (RVA 0x%06X)%s -> 管理器 0x%08X  数量偏移 +0x%X'
                % (slot, rva, ' [.data]' if indata else '', mval, off))
            if indata:
                head = read_i32(gp, mval + 0x50)
                hs = ('0x%08X' % (head & 0xFFFFFFFF)) if head is not None else '??'
                log('      mgr+0x50 试读 = %s' % hs)
    else:
        log('未找到指向候选对象的指针。')

    if clist:
        log('')
        log('===== 候选对象内存（字段前后各 0x20）=====')
        for c in clist[:8]:
            log('  --- 数量字段 0x%08X ---' % c)
            buf = gp.read(c - 0x20, 0x40)
            if len(buf) == 0x40:
                for r in range(0, 0x40, 16):
                    line = '      +0x%02X: ' % (r - 0x20)
                    for k in range(0, 16, 4):
                        line += '%08X ' % struct.unpack_from('<I', buf, r + k)[0]
                    log(line)

    log('')
    log('扫描完成。结果已保存到 unit_mgr_result.txt，请发给开发者。')
    try:
        with open('unit_mgr_result.txt', 'w', encoding='utf-8') as f:
            f.write('\n'.join(OUT))
        print('\n[+] 结果已保存到 unit_mgr_result.txt')
    except Exception as e:
        print('[!] 保存失败：%s' % e)
    return 0


if __name__ == '__main__':
    sys.exit(main())

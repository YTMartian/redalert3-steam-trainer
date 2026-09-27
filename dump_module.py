# -*- coding: utf-8 -*-
"""
把游戏模块的内存镜像 dump 到本地文件，供离线静态分析（全程只读，不修改游戏）。

背景：
    「满级(3星)」写入 [[实体+0x3CC]+0x24]=3 能成功落地（读回就是 3），但游戏里星级
    没有变化，说明该字段不是驱动星级显示的权威字段。要找到真正的升级逻辑，最可靠
    的办法是在游戏的代码里定位「写星级字段」的那条指令，再直接调用游戏自己的升级
    函数 —— 这样星级、加成、UI 都会一起生效。

    本脚本把 ra3_1.XX.game 的整个镜像按内存原样 dump 出来，之后由 scan_vet_code.py
    离线分析（不需要游戏运行、不需要管理员）。

用法：
    1. 游戏随便什么界面都行（已启动即可），修改器已「附加游戏」。
    2. 管理员身份运行「定位星级代码.bat」。

输出：ra3_image.bin（镜像）+ ra3_image.json（段表 / 基址）
"""
import ctypes
import json
import struct
import sys
import time

from trainer import GameProcess

DUMP_FILE = 'ra3_image.bin'
META_FILE = 'ra3_image.json'
CHUNK = 0x10000
PAGE = 0x1000

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)


def read_chunk(gp, addr, size):
    """读一块；大块失败时退化为按页读，读不到的地方填 0。"""
    data = gp.read(addr, size)
    if len(data) == size:
        return data, size
    out = bytearray()
    bad = 0
    for off in range(0, size, PAGE):
        n = min(PAGE, size - off)
        d = gp.read(addr + off, n)
        if len(d) == n:
            out += d
        else:
            out += b'\x00' * n
            bad += n
    return bytes(out), bad


def process_path(gp):
    """取游戏进程的完整 exe 路径（拿不到就退回模块名）。"""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = ctypes.c_ulong(1024)
        # QueryFullProcessImageNameW
        fn = kernel32.QueryFullProcessImageNameW
        fn.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                       ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
        if fn(ctypes.c_void_p(gp.handle), 0, buf, ctypes.byref(size)):
            return buf.value
    except Exception:
        pass
    return gp.game_module


def main():
    gp = GameProcess()
    ok, err = gp.attach()
    if not ok:
        print('[!] 附加失败：%s' % err)
        return 1

    base = gp.module_base
    print('[+] 已附加：PID=%d' % gp.pid)
    print('    模块 : %s' % process_path(gp))
    print('    基址 : 0x%08X' % base)

    # ---- 解析 PE，取 SizeOfImage 与段表 ----
    head = gp.read(base, 0x400)
    if len(head) < 0x40 or head[:2] != b'MZ':
        print('[!] 读取到的不是 PE 镜像头（len=%d）' % len(head))
        return 2
    e_lfanew = struct.unpack_from('<I', head, 0x3C)[0]
    pe = gp.read(base + e_lfanew, 0x18 + 0xF0 + 40 * 16)
    if pe[:4] != b'PE\0\0':
        print('[!] PE 签名不对')
        return 2
    nsec = struct.unpack_from('<H', pe, 6)[0]
    size_opt = struct.unpack_from('<H', pe, 0x14)[0]
    size_image = struct.unpack_from('<I', pe, 0x18 + 0x38)[0]

    sections = []
    for i in range(min(nsec, 16)):
        o = 0x18 + size_opt + i * 40
        chunk = pe[o:o + 40]
        if len(chunk) < 40:
            break
        name = chunk[:8].rstrip(b'\x00').decode('ascii', 'replace')
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', chunk, 8)
        chars = struct.unpack_from('<I', chunk, 36)[0]
        sections.append(dict(name=name, va=va, vsize=vsize,
                             rawsize=rawsize, rawptr=rawptr,
                             executable=bool(chars & 0x20000000)))

    print('    PE   : SizeOfImage=0x%X  段数=%d' % (size_image, nsec))
    for s in sections:
        print('      %-8s VA=0x%08X vsize=0x%08X %s'
              % (s['name'], base + s['va'], s['vsize'],
                 'CODE' if s['executable'] else ''))

    if not (0x100000 <= size_image <= 0x20000000):
        print('[!] SizeOfImage 不合理（0x%X），终止' % size_image)
        return 2

    # ---- dump ----
    print('')
    print('正在 dump 0x%X 字节（%.1f MB）...' % (size_image, size_image / 1048576.0))
    t0 = time.time()
    parts = bytearray()
    bad_total = 0
    addr = base
    done = 0
    while done < size_image:
        n = min(CHUNK, size_image - done)
        data, bad = read_chunk(gp, addr + done, n)
        parts += data
        bad_total += bad
        done += n
        if done % (CHUNK * 64) == 0:
            print('   ... 0x%X / 0x%X' % (done, size_image))
    dt = time.time() - t0

    open(DUMP_FILE, 'wb').write(bytes(parts))
    json.dump(dict(base=base, size_image=size_image,
                   module=gp.game_module, pid=gp.pid,
                   sections=sections, unreadable=bad_total),
              open(META_FILE, 'w', encoding='utf-8'),
              ensure_ascii=False, indent=2)

    print('')
    print('[+] 完成，用时 %.1fs' % dt)
    print('    %s : %d 字节' % (DUMP_FILE, len(parts)))
    print('    %s : 段表' % META_FILE)
    if bad_total:
        print('    [!] 有 0x%X 字节读不到（已填 0，通常是未提交的页，不影响代码分析）'
              % bad_total)
    print('')
    print('接下来运行：python scan_vet_code.py   （不需要管理员）')
    return 0


if __name__ == '__main__':
    sys.exit(main())

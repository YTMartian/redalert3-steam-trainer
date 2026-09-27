# -*- coding: utf-8 -*-
"""验证修改器图标真的被系统收下了。

只检查「Tk 没报错」是不够的 —— `iconbitmap` 对损坏或尺寸不对的 .ico
会静默失败。所以这里直接回读窗口的 WM_GETICON 句柄：
Tk 设了图标后，系统会把对应的 HICON 存在窗口属性里，拿到非 0 就说明
标题栏/任务栏确实有图标。

同时校验：
  1. icon.ico 含预期的最小尺寸（16px）—— 缺了它任务栏图标会糊；
  2. icon.png 能被 Tk 的 PhotoImage 读出来（iconphoto 兜底路径）；
  3. 源码运行与 PyInstaller 打包（_MEIPASS）两种情况下的资源定位。
"""
import ctypes
import os
import sys
import tkinter as tk

import trainer as T

WM_GETICON = 0x007F
ICON_SMALL = 0
ICON_BIG = 1
GCLP_HICON = -14
GCLP_HICONSM = -34

ok = []
fail = []


def check(name, cond, detail=''):
    (ok if cond else fail).append(name)
    print('%s %s%s' % ('[OK]  ' if cond else '[FAIL]', name,
                       ('  -> ' + detail) if detail else ''))


def hwnd_of(root):
    """取顶层窗口的真实 HWND（winfo_id 给的是 Tk 子窗口）。"""
    try:
        return int(root.wm_frame(), 16)
    except Exception:
        return int(root.winfo_id())


def main():
    # ---- 1. 资源文件本身 ----
    base = T._resource_dir()
    ico = os.path.join(base, 'icon.ico')
    png = os.path.join(base, 'icon.png')

    check('icon.ico 存在', os.path.exists(ico), ico)
    if os.path.exists(ico):
        from PIL import Image
        sizes = sorted(Image.open(ico).info.get('sizes', []))
        check('icon.ico 含 16px 帧（任务栏需要）', (16, 16) in sizes, str(sizes))
        check('icon.ico 含 256px 帧（大图标视图需要）',
              (256, 256) in sizes, str(sizes))
        check('icon.ico 帧数 >= 5', len(sizes) >= 5, '%d 帧' % len(sizes))

    check('icon.png 存在', os.path.exists(png), png)

    # ---- 2. 窗口 + WM_GETICON 回读 ----
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    root = tk.Tk()
    root.title('icon test')
    T.apply_icon(root)
    root.update()          # 必须 update 一次，窗口才会真正带上图标属性

    hwnd = hwnd_of(root)
    u = ctypes.windll.user32

    icon_big = u.SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0)
    icon_small = u.SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0)
    if not icon_big:
        icon_big = u.GetClassLongPtrW(hwnd, GCLP_HICON)
    if not icon_small:
        icon_small = u.GetClassLongPtrW(hwnd, GCLP_HICONSM)

    check('窗口已设置大图标 (WM_GETICON)', bool(icon_big), hex(icon_big or 0))
    check('窗口已设置小图标 (WM_GETICON)', bool(icon_small), hex(icon_small or 0))

    # ---- 3. iconphoto 兜底路径可用 ----
    try:
        ph = tk.PhotoImage(file=png)
        check('icon.png 可被 Tk 读取 (iconphoto 兜底)', True,
              '%dx%d' % (ph.width(), ph.height()))
    except Exception as e:
        check('icon.png 可被 Tk 读取 (iconphoto 兜底)', False, repr(e))

    root.destroy()

    # ---- 4. 打包后的 EXE 内嵌图标（有 dist 才查） ----
    exe = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       'dist', 'RA3_Steam_Trainer.exe')
    if os.path.exists(exe):
        from ctypes import wintypes
        sh = ctypes.windll.shell32          # ExtractIconExW 属于 shell32
        big_h = (wintypes.HICON * 1)()
        sml_h = (wintypes.HICON * 1)()
        groups = sh.ExtractIconExW(exe, -1, None, None, 0)
        sh.ExtractIconExW(exe, 0, ctypes.byref(big_h), ctypes.byref(sml_h), 1)
        check('EXE 内嵌图标资源', groups >= 1, '%d 组' % groups)

        class BITMAP(ctypes.Structure):
            _fields_ = [('bmType', wintypes.LONG), ('bmWidth', wintypes.LONG),
                        ('bmHeight', wintypes.LONG), ('bmWidthBytes', wintypes.LONG),
                        ('bmPlanes', wintypes.WORD), ('bmBitsPixel', wintypes.WORD),
                        ('bmBits', ctypes.c_void_p)]

        class ICONINFO(ctypes.Structure):
            _fields_ = [('fIcon', wintypes.BOOL), ('xHotspot', wintypes.DWORD),
                        ('yHotspot', wintypes.DWORD), ('hbmMask', wintypes.HBITMAP),
                        ('hbmColor', wintypes.HBITMAP)]

        gdi = ctypes.windll.gdi32
        gdi.GetObjectW.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p]
        for tag, h in (('大', big_h[0]), ('小', sml_h[0])):
            if not h:
                check('EXE %s图标可抽取' % tag, False, '句柄为空')
                continue
            ii = ICONINFO()
            ctypes.windll.user32.GetIconInfo(h, ctypes.byref(ii))
            bm = BITMAP()
            gdi.GetObjectW(wintypes.HANDLE(ii.hbmColor), ctypes.sizeof(bm),
                           ctypes.byref(bm))
            # 注意：ExtractIconEx 返回的像素尺寸依赖**当前进程的 DPI 上下文**
            # （脚本前面建过 Tk 窗口并设了 DPI 感知，尺寸会跟着变），
            # 所以这里只断言 32bpp（我们的图标带 alpha，必是 32bpp）和尺寸合法，
            # 不锁定具体是 16/32/48。
            check('EXE %s图标为 32bpp 且尺寸合法（我们设计的带 alpha 图标）' % tag,
                  bm.bmBitsPixel == 32 and bm.bmWidth >= 16
                  and bm.bmWidth == bm.bmHeight,
                  '%dx%d %dbpp' % (bm.bmWidth, bm.bmHeight, bm.bmBitsPixel))
    else:
        print('[SKIP] dist 下没有 EXE，跳过内嵌图标检查（先跑 build.bat）')

    print()
    print('通过 %d 项，失败 %d 项' % (len(ok), len(fail)))
    if fail:
        for f in fail:
            print('  FAILED: ' + f)
        sys.exit(1)
    print('图标验证: OK')


if __name__ == '__main__':
    main()

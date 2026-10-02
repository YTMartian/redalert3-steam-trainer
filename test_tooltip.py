# -*- coding: utf-8 -*-
"""快速验证：侧栏布局、「满级(3星)」按钮存在、提示正确。"""
import ctypes
import time
import tkinter as tk

import trainer as T

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(1)
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass


def pump(root, seconds):
    end = time.time() + seconds
    while time.time() < end:
        try:
            root.update()
        except tk.TclError:
            return False
        time.sleep(0.02)
    return True


def all_toplevels(widget, acc=None):
    if acc is None:
        acc = []
    for w in widget.winfo_children():
        if isinstance(w, tk.Toplevel):
            acc.append(w)
        all_toplevels(w, acc)
    return acc


def main():
    root = tk.Tk()
    app = T.TrainerApp(root)
    pump(root, 0.6)
    root.lift()
    pump(root, 0.4)

    ok = True
    print('窗口尺寸: %dx%d' % (root.winfo_width(), root.winfo_height()))
    print('按钮总数: %d' % len(app.btn_widgets))
    print('侧栏分类: %d' % len(getattr(app, '_nav_btns', {})))

    # 切到「单位操作」页，再测满级按钮悬停（隐藏页上的 tip 坐标不可靠）
    unit_idx = next(i for i, (n, _) in enumerate(T.FEATURE_GROUPS) if n == '单位操作')
    app._show_group(unit_idx)
    pump(root, 0.3)

    btn = app.btn_widgets.get('unit_rank')
    print('unit_rank 按钮: %s  (文本 %r)' % (btn is not None, btn.cget('text') if btn else None))
    if btn is None:
        ok = False
    else:
        print('按钮文本:', btn.cget('text'))
        btn.event_generate('<Enter>')
        pump(root, 0.8)
        tips = all_toplevels(root)
        txt = ''
        if tips:
            # ToolTip 结构：Toplevel -> Frame -> Label
            def _first_label(w):
                if isinstance(w, tk.Label):
                    return w
                for ch in w.winfo_children():
                    found = _first_label(ch)
                    if found is not None:
                        return found
                return None
            lab = _first_label(tips[0])
            txt = lab.cget('text') if lab else ''
        print('悬停提示:', repr(txt))
        if 'P' not in txt:
            print('  [!] 提示里没有快捷键 P')
            ok = False
        btn.event_generate('<Leave>')
        pump(root, 0.4)

    x = root.winfo_rootx()
    y = root.winfo_rooty()
    from PIL import ImageGrab
    img = ImageGrab.grab(bbox=(x, y, x + root.winfo_width(), y + root.winfo_height()))
    img.save('gui_check.png')
    print('已保存 gui_check.png', img.size)

    root.destroy()
    print()
    print('GUI 校验:', 'OK' if ok else '失败')
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())

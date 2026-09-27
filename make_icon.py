# -*- coding: utf-8 -*-
"""生成修改器图标（红警主题）。

设计意图
--------
- **深色圆角底板 + 红警红描边**：与 `trainer.py` 的 GUI 配色一致
  （底板 `#1e1e26→#121216`，描边 `#e04a3f`）。
- **三颗五角星（军衔式三角排布）**：直接呼应本修改器的招牌功能
  「满级(3星)」——单位晋升到顶的星级徽记。红星同时也是《红色警戒》
  系列最标志性的符号。
- 星形内部做上亮下暗的竖直渐变，外缘描一圈浅色高光，
  保证在 16px 的任务栏里仍是一个清晰的「暗底红星」剪影。

工程做法
--------
- 先在目标尺寸的 `SS` 倍画布上绘制（超采样抗锯齿），再用 LANCZOS 降采样。
- **小尺寸分档**：笔触不会等比缩放，而是设下限（`max(1.0, …)`），
  且 16/24px 会加大辅星比例、去掉外发光 —— 否则细节会被采样吃掉、
  星星糊成一坨。所以每个尺寸都是**独立绘制**的，不是从 256 缩出来的。
- 输出多尺寸 ICO（16~256，每个尺寸都是原生绘制）+ 一张 256px PNG
  （给 tkinter 的 `iconphoto` 与 README 预览用）。

只依赖 Pillow。改设计请改本文件后重跑 `python make_icon.py`，
不要手改 `.ico`（二进制，改不动且无法复现）。
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ICO = os.path.join(HERE, 'icon.ico')
PNG = os.path.join(HERE, 'icon.png')
SHEET = os.path.join(HERE, 'icon_preview.png')

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SS = 8                    # 超采样倍数

# 配色（与 trainer.py 的 self.colors 对应）
BG_TOP = (30, 30, 38)      # #1e1e26
BG_BOT = (18, 18, 22)      # #121216
RIM = (224, 74, 63)        # #e04a3f 红警红
STAR_TOP = (255, 116, 100)
STAR_BOT = (176, 42, 30)
STAR_EDGE = (255, 176, 166)
GLOW = (255, 90, 70)


def star_points(cx, cy, outer, rot=-math.pi / 2):
    """五角星顶点（外/内半径比取黄金比的常用近似 0.382）。"""
    inner = outer * 0.382
    pts = []
    for i in range(10):
        r = outer if i % 2 == 0 else inner
        a = rot + i * math.pi / 5.0
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def vgrad(size, top, bot):
    """竖直渐变（横向均匀）。"""
    g = Image.new('RGB', (1, size[1]))
    px = g.load()
    for y in range(size[1]):
        t = y / max(1, size[1] - 1)
        px[0, y] = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
    return g.resize(size, Image.BILINEAR)


def render(n):
    """独立绘制尺寸为 n 的图标（在 n*SS 画布上画完再降采样），返回 RGBA。

    圆角矩形之外是**透明**的（而不是填深色），否则在浅色标题栏上会露出
    一个黑方块。降采样时对 RGB 做预乘、之后再除回来，避免半透明边缘
    混到透明区域的黑色而出现暗边。
    """
    S = n * SS
    u = S / 256.0                      # 以 256 为基准的逻辑单位

    # ---- 小尺寸分档参数 ----
    tiny = n <= 24                     # 极小尺寸：加粗笔触 / 去掉细节
    rim_w = max(1.6, (11 if tiny else 9) * u)
    radius = 46 * u

    # ---- 底板 ----
    img = Image.new('RGB', (S, S), BG_BOT)
    pad = rim_w / 2.0
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [pad, pad, S - 1 - pad, S - 1 - pad], radius=radius, fill=255)
    img.paste(vgrad((S, S), BG_TOP, BG_BOT), (0, 0), mask)

    # ---- 红色描边（外圈 - 内圈 得到干净的环形） ----
    ring = Image.new('L', (S, S), 0)
    ImageDraw.Draw(ring).rounded_rectangle(
        [pad, pad, S - 1 - pad, S - 1 - pad], radius=radius, fill=255)
    inner = Image.new('L', (S, S), 0)
    ImageDraw.Draw(inner).rounded_rectangle(
        [pad + rim_w, pad + rim_w, S - 1 - pad - rim_w, S - 1 - pad - rim_w],
        radius=max(0, radius - rim_w), fill=255)
    ring = Image.composite(Image.new('L', (S, S), 0), ring, inner)
    img.paste(Image.new('RGB', (S, S), RIM), (0, 0), ring)

    # ---- 三颗星：主星在上，两颗辅星在下方成三角 ----
    # 小尺寸下辅星加大，避免降到 16px 后消失成一粒灰点
    main_r = (62 if not tiny else 60) * u
    sub_r = (36 if tiny else 32) * u
    if tiny:
        main_cy, sub_cy, dx = 0.455, 0.762, 0.185
    else:
        main_cy, sub_cy, dx = 0.450, 0.760, 0.190

    stars = [(0.5, main_cy, main_r, 1.0),
             (0.5 - dx, sub_cy, sub_r, 0.90),
             (0.5 + dx, sub_cy, sub_r, 0.90)]

    edge_w = 0 if n < 32 else max(1, (5 if n < 64 else 4) * u)
    for cxr, cyr, r, dim in stars:
        pts = star_points(S * cxr, S * cyr, r)
        m = Image.new('L', (S, S), 0)
        ImageDraw.Draw(m).polygon(pts, fill=255)
        if dim != 1.0:
            m = m.point(lambda v, d=dim: int(v * d))
        top = tuple(int(STAR_TOP[i] * (0.62 + 0.38 * dim)) for i in range(3))
        bot = tuple(int(STAR_BOT[i] * (0.52 + 0.48 * dim)) for i in range(3))
        img.paste(vgrad((S, S), top, bot), (0, 0), m)

        # 外发光：只在 >=48px 加，小尺寸会糊
        if n >= 48:
            g = m.filter(ImageFilter.GaussianBlur(S / 40.0))
            img = Image.composite(Image.new('RGB', (S, S), GLOW), img,
                                  g.point(lambda v, d=dim: int(v * 0.5 * d)))
        if edge_w > 0:
            ImageDraw.Draw(img).line(pts + [pts[0]], fill=STAR_EDGE,
                                     width=int(edge_w), joint='curve')

    # ---- 加 alpha：圆角外透明 ----
    # 先把 RGB 按 alpha 预乘再降采样，否则透明区域的黑色会混进半透明边缘
    # 形成一圈暗边；降采样后再除回 alpha（un-premultiply）还原颜色。
    r, g, b = img.split()
    pm = Image.merge('RGBA', (
        Image.composite(r, Image.new('L', (S, S), 0), mask),
        Image.composite(g, Image.new('L', (S, S), 0), mask),
        Image.composite(b, Image.new('L', (S, S), 0), mask),
        mask))
    small = pm.resize((n, n), Image.LANCZOS)

    out = Image.new('RGBA', (n, n))
    src = small.load()
    dst = out.load()
    for y in range(n):
        for x in range(n):
            R, G, B, A = src[x, y]
            if A == 0:
                dst[x, y] = (0, 0, 0, 0)
            else:
                dst[x, y] = (min(255, (R * 255 + A // 2) // A),
                             min(255, (G * 255 + A // 2) // A),
                             min(255, (B * 255 + A // 2) // A), A)
    return out


def checker(size, a=(78, 78, 88), b=(58, 58, 66), k=8):
    """棋盘底，用来肉眼确认圆角外确实是透明而不是深色。"""
    im = Image.new('RGB', size, a)
    d = ImageDraw.Draw(im)
    for y in range(0, size[1], k):
        for x in range(0, size[0], k):
            if ((x // k) + (y // k)) % 2:
                d.rectangle([x, y, x + k - 1, y + k - 1], fill=b)
    return im


def main():
    frames = {n: render(n) for n in SIZES}

    frames[256].save(PNG, 'PNG')
    # 每个尺寸都是原生绘制，直接作为 ICO 的多尺寸帧写出
    frames[256].save(ICO, format='ICO', sizes=[(n, n) for n in SIZES])

    # 预览图：棋盘底 + 大图 + 各尺寸 3 倍放大并排
    sheet = checker((1180, 300))
    big = frames[256].resize((276, 276), Image.LANCZOS)
    sheet.paste(big, (12, 12), big)
    x = 306
    for n in SIZES:
        k = max(1, 96 // n)
        th = frames[n].resize((n * k, n * k), Image.NEAREST)
        sheet.paste(th, (x, 14), th)
        x += n * k + 10
    sheet.save(SHEET, 'PNG')

    # 自检：四角必须透明（否则说明圆角没做出来）
    for n in (16, 32, 256):
        px = frames[n].load()
        corners = [px[0, 0][3], px[n - 1, 0][3], px[0, n - 1][3], px[n - 1, n - 1][3]]
        assert max(corners) < 40, '尺寸 %d 的角不透明: %s' % (n, corners)
        assert px[n // 2, n // 2][3] > 200, '尺寸 %d 中心不透明' % n
    print('icon.ico      %6d bytes  (%s)' % (
        os.path.getsize(ICO), ', '.join(str(s) for s in SIZES)))
    print('icon.png      %6d bytes' % os.path.getsize(PNG))
    print('icon_preview.png %6d bytes' % os.path.getsize(SHEET))
    print('透明角自检: OK')


if __name__ == '__main__':
    main()

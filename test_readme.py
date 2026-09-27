# -*- coding: utf-8 -*-
"""README 结构自检：代码围栏必须成对、章节编号不能跳号、提到的文件要真的在。

这类文档问题手工很难发现，但会让 GitHub 上的整段内容渲染错位。
"""
import io
import os
import re
import sys

t = io.open('README.md', encoding='utf-8').read()
lines = t.split('\n')

bad = []

# 1. 代码围栏成对
fences = [i + 1 for i, l in enumerate(lines) if l.startswith('```')]
if len(fences) % 2:
    bad.append('代码围栏数为奇数（%d），第 %d 行未闭合' % (len(fences), fences[-1]))

# 2. 二级 / 三级编号连续
h2 = re.findall(r'^## ([一二三四五六七八九十]+)、', t, re.M)
if len(h2) != len(set(h2)):
    bad.append('二级标题编号重复: %s' % h2)
sub = re.findall(r'^### (\d+)\.(\d+)', t, re.M)
nums = [int(b) for a, b in sub if a == '5']
if nums and nums != list(range(1, len(nums) + 1)):
    bad.append('5.x 小节编号不连续: %s' % nums)

# 3. README 里引用的仓库内文件必须存在（只查反引号包住的常见扩展名）
refd = set(re.findall(r'`([A-Za-z0-9_.\-]+\.(?:py|bat|ico|png|json|asm|txt|bin|md))`', t))
refd |= set(re.findall(r'!\[[^\]]*\]\(([^)]+\.png)\)', t))
missing = sorted(f for f in refd
                 if not os.path.exists(f)
                 and not f.startswith('ra3')            # 游戏本体，故意不入库
                 and f not in ('icon_preview.png',))
missing = [f for f in missing if os.path.basename(f) == f]
if missing:
    bad.append('README 引用了不存在的文件: %s' % missing)

# 4. README 里贴的图片要真存在
for m in re.finditer(r'(?:!\[[^\]]*\]\(([^)]+)\)|<img[^>]*src="([^"]+)")', t):
    p = m.group(1) or m.group(2)
    if p and not p.startswith('http') and not os.path.exists(p):
        bad.append('README 引用的图片不存在: %s' % p)

print('围栏 %d 个，5.x 小节 %s，引用文件 %d 个'
      % (len(fences), nums or '无', len(refd)))
if bad:
    for b in bad:
        print('[FAIL] ' + b)
    sys.exit(1)
print('README 结构自检: OK')

# -*- coding: utf-8 -*-
"""
生成适配 Steam 版后的 AutoAssembler 脚本
将原 MustEnableCode 脚本中的 ra3_1.12.game+旧偏移 替换为 ra3_1.12.game+新偏移
"""
import re, io
from hook_map import HOOKS, FUNCS, GLOBALS

# 读取解析出的原脚本（parsed_trainer.txt 中 [0] MustEnableCode 的 SCRIPT 部分）
src = io.open('parsed_trainer.txt', encoding='utf-8').read()

# 提取 [0] 的 SCRIPT 块（从第一个 [ENABLE] 到对应的 [disable] 结束）
m = re.search(r'\[0\].*?SCRIPT:\n(.*?)\n\n\[1\]', src, re.S)
script = m.group(1)

# 去掉行首的 "| " 前缀（解析时的格式化）
lines = []
for ln in script.split('\n'):
    ln = ln.strip()
    if ln.startswith('| '):
        ln = ln[2:]
    lines.append(ln)
script = '\n'.join(lines)

# 构建 偏移 -> 新偏移 映射
addr_map = {}
for h in HOOKS:
    if h['new']:
        addr_map[h['orig']] = h['new']

# 替换所有 ra3_1.12.game+OFFSET 形式的引用
def repl(m):
    off_str = m.group(1).lower()
    try:
        off = int(off_str, 16)
    except ValueError:
        return m.group(0)
    if off in addr_map:
        return 'ra3_1.12.game+%X' % addr_map[off]
    return m.group(0)

# 大小写不敏感匹配 ra3_1.12.game+十六进制
out = re.sub(r'(?i)ra3_1\.12\.game\+([0-9a-f]+)', repl, script)

# 写入适配脚本
io.open('steam_adapted.asm', 'w', encoding='utf-8').write(out)

# 统计
replaced = 0
for h in HOOKS:
    if h['new']:
        replaced += 1
print('已生成 steam_adapted.asm')
print('替换了 %d 个 hook 地址' % replaced)

# 列出未替换的 hook 引用（待定）
todo = [h['name'] for h in HOOKS if not h['new']]
print('待定 hook (仍需零售版参考): %s' % ', '.join(todo))
print('待定函数 (仍需定位): %s' % ', '.join(f['name'] for f in FUNCS))

import time, re, io, sys
log = open('debug.log', 'w', encoding='utf-8')

def L(*a):
    log.write(' '.join(str(x) for x in a) + '\n')
    log.flush()

L('start')
from keystone import Ks, KS_ARCH_X86, KS_MODE_32
L('import keystone ok')
ks = Ks(KS_ARCH_X86, KS_MODE_32)
L('Ks() ok')

# 测试1：最小汇编
enc, c = ks.asm('mov eax, 1; ret', 0x1000)
L('min asm ok', len(enc))

lines = io.open('mustcode_body.asm', encoding='utf-8').read().split('\n')
L('read lines', len(lines))

# 测试2：符号替换（只 MOD/FLAGS/IDB，不含 _Back）
import json
back = json.load(open('symbols.json'))
abs_sym = {'MOD': 0x400000, 'FLAGS': 0x10005000, 'IDB': 0x10005040}
abs_sym.update(back)
L('abs_sym count', len(abs_sym))

t0 = time.time()
cnt = 0
for line in lines:
    line = line.strip()
    if not line:
        continue
    for name, val in abs_sym.items():
        line = re.sub(r'\b%s\b' % re.escape(name), '0x%X' % val, line)
    cnt += 1
L('sym replace done', cnt, 'rows in %.3f' % (time.time() - t0))

# 测试3：汇编前 30 个非标签行
t0 = time.time()
body = []
for line in lines:
    line = line.strip()
    if not line or line.endswith(':'):
        continue
    body.append(line)
    if len(body) >= 30:
        break
L('collect 30 rows ok', len(body))
for b in body[:5]:
    L('  ', repr(b))
src = '\n'.join(body)
L('try asm 30 rows...')
try:
    enc, c = ks.asm(src, 0x10000000)
    L('asm 30 rows ok', len(enc), 'bytes in %.3f' % (time.time() - t0))
except Exception as e:
    L('asm error:', repr(e))
log.close()

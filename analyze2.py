# -*- coding: utf-8 -*-
"""
离线分析 2：数据流追踪 —— 谁在操作「星级组件」（[x+0x3CC] 指向的对象）？

做法（轻量寄存器级追踪，只在基本块内有效但足够）：
  - 遇到 `mov rA, [rB + 0x3CC]`（rB 不是 esp/ebp）时，给 rA 打上 VET 标记；
  - 用 `mov rA, rB` 传播标记；任何其它写 rA 的指令清除标记；
  - 当带标记的寄存器被用作内存基址时，记录这次访问（含位移）。

重点看：位移为 0x24（星级候选）、0x10 / 0x0C（经验候选）的访问 —— 那些函数就是
真正的升级逻辑所在地。

用法：python analyze2.py     （需要 ra3_image.bin / ra3_image.json）
输出：analyze2_result.txt
"""
import json
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_MEM, CS_OP_REG

VET_OFF = 0x3CC
WATCH = {0x24: '星级候选', 0x10: '经验候选', 0x0C: '经验候选2'}

meta = json.load(open('ra3_image.json', encoding='utf-8'))
img = open('ra3_image.bin', 'rb').read()
base = meta['base']

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

# 视为「写第一个操作数」的助记符
WRITES_OP0 = set("""mov movzx movsx lea add sub adc sbb and or xor inc dec not neg
shl shr sar sal rol ror imul idiv div mul movss movsd movaps movups movd movq
pop xchg bswap cmove cmovne cmovz cmovnz cmova cmovb cmovg cmovl setz setnz
seta setb sets seto fld fstp""".split())

OUT = []


def log(s=''):
    OUT.append(s)


def reg_name(op):
    return md.reg_name(op.reg) if op.type == CS_OP_REG else None


def is_stack(name):
    return name in ('esp', 'ebp', 'sp', 'bp')


def scan_code(code, va, results):
    """线性扫描 + 重同步。results 收集 (kind, addr, text, disp)。"""
    tag = {}          # reg id -> 标记来源地址
    mv = memoryview(code)
    off = 0
    n = len(mv)
    while off < n:
        progressed = False
        for insn in md.disasm(mv[off:], va + off):
            progressed = True
            off = insn.address + insn.size - va
            ops = insn.operands

            # --- 1) 记录/传播 VET 标记 ---
            loaded = False
            if insn.mnemonic in ('mov', 'movzx', 'movsx') and len(ops) == 2:
                dst, src = ops
                dn, sn = reg_name(dst), reg_name(src)
                if dst.type == CS_OP_REG and src.type == CS_OP_MEM \
                        and src.mem.disp == VET_OFF and not is_stack(sn):
                    tag[dst.reg] = insn.address
                    results.append(('LOAD', insn.address,
                                    '%s %s' % (insn.mnemonic, insn.op_str), VET_OFF))
                    loaded = True
                elif dst.type == CS_OP_REG and src.type == CS_OP_REG:
                    if src.reg in tag:
                        tag[dst.reg] = tag[src.reg]
                    else:
                        tag.pop(dst.reg, None)

            # --- 2) 带标记寄存器的内存访问 ---
            if not loaded:
                for op in ops:
                    if op.type == CS_OP_MEM and op.mem.base in tag \
                            and not is_stack(md.reg_name(op.mem.base)):
                        results.append(('ACCESS', insn.address,
                                        '%s %s' % (insn.mnemonic, insn.op_str),
                                        op.mem.disp))

            # --- 3) 清除被覆盖的寄存器标记 ---
            if loaded:
                continue      # 刚打的标记不能被自己清掉
            if ops and insn.mnemonic not in ('cmp', 'test', 'push', 'call', 'jmp',
                                             'ret', 'nop'):
                first = ops[0]
                if insn.mnemonic == 'lea' and len(ops) == 2 \
                        and ops[1].type == CS_OP_MEM and ops[1].mem.base == first.reg:
                    pass   # lea r,[r+disp] 仍指向同一对象，保留标记
                elif first.type == CS_OP_REG and insn.mnemonic in WRITES_OP0:
                    tag.pop(first.reg, None)
                elif first.type == CS_OP_MEM:
                    pass   # 写内存不影响寄存器标记

            if insn.mnemonic in ('call', 'ret'):
                tag.clear()   # 跨函数调用后寄存器语义不可知
        if not progressed:
            off += 1
    return results


def main():
    secs = [s for s in meta['sections'] if s['executable']]
    log('镜像 base=0x%08X size=0x%X  模块=%s'
        % (base, meta['size_image'], meta.get('module', '?')))

    for s in secs:
        code = img[s['va']:s['va'] + s['vsize']]
        results = []
        scan_code(code, base + s['va'], results)
        log('段 %s：追踪到 %d 条相关指令' % (s['name'], len(results)))

        loads = [r for r in results if r[0] == 'LOAD']
        accs = [r for r in results if r[0] == 'ACCESS']
        log('  LOAD（取出星级组件指针）: %d 处' % len(loads))
        for a, t, _ in [(r[1], r[2], r[3]) for r in loads]:
            log('    0x%08X  %s' % (a, t))

        log('')
        log('  ACCESS（对星级组件的字段访问）: %d 处' % len(accs))
        # 按位移归类
        byd = {}
        for _, a, t, d in accs:
            byd.setdefault(d, []).append((a, t))
        for d in sorted(byd, key=lambda x: (x is None, x)):
            name = WATCH.get(d, '')
            mark = '  <<<<< 重点' if d in WATCH else ''
            log('    +0x%-4s %-10s %d 处%s' % ('%X' % d if d is not None else '?',
                                              name, len(byd[d]), mark))
            if d in WATCH or (d is not None and d < 0x40):
                for a, t in byd[d]:
                    log('        0x%08X  %s' % (a, t))

    open('analyze2_result.txt', 'w', encoding='utf-8').write('\n'.join(OUT))
    print('[+] 已写出 analyze2_result.txt（%d 行）' % len(OUT))

    # 控制台只打印重点
    print('')
    print('=== 访问 +0x24（星级候选）/ +0x10 / +0x0C 的代码 ===')
    for line in OUT:
        if '重点' in line or ('0x' in line and 'ACCESS' in line):
            print(line)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

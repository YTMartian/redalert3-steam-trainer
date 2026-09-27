# -*- coding: utf-8 -*-
"""自测 scan_vet_code 的指令扫描逻辑，用合成指令流（无需游戏/dump）。"""
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_MEM

import scan_vet_code as S

md = S.md
BASE = 0x400000


def asm(text, base=BASE):
    """用 keystone 组装合成代码（keystone 已是项目依赖）。"""
    from keystone import Ks, KS_ARCH_X86, KS_MODE_32
    ks = Ks(KS_ARCH_X86, KS_MODE_32)
    enc, cnt = ks.asm(text, base)
    return bytes(enc)


def main():
    src = """
    push ebp
    mov ebp, esp
    mov esi, [0x8E08DC]
    mov ecx, [esi+0x5C]
    mov edi, [esi+0x50]
    mov ebx, [edi+8]
    mov ebx, [ebx+0x138]
    mov eax, [ebx+0x3CC]
    test eax, eax
    jz done
    mov dword ptr [eax+0x24], 3
    mov dword ptr [eax+0x10], 300
done:
    mov edx, [eax+0x3CC]
    ret
    """
    code = b'\xcc' * 0x10 + asm(src)     # 前导 int3 填充，模拟真实段布局
    FUNC_START = BASE + 0x10
    code += b'\x00\xFF\x12'   # 故意插入非法字节，测试重同步

    # 找到第一条 / 第二条 0x3CC 指令的真实地址
    expect = []
    for insn in md.disasm(code, BASE):
        for op in insn.operands:
            if op.type == CS_OP_MEM and op.mem.disp == S.VET_PTR_OFF:
                expect.append(insn.address)

    found = []

    def on_insn(insn):
        for op in insn.operands:
            if op.type == CS_OP_MEM and op.mem.disp == S.VET_PTR_OFF:
                found.append((insn.address, '%s %s' % (insn.mnemonic, insn.op_str)))

    total = S.sweep(code, BASE, on_insn)

    print('反汇编指令数: %d' % total)
    print('命中 disp=0x%X 的指令: %d 条（期望 %d）'
          % (S.VET_PTR_OFF, len(found), len(expect)))
    for a, t in found:
        print('  0x%08X  %s' % (a, t))

    ok = len(found) == len(expect) and len(expect) == 2

    # 验证 find_func_start 能定位序言（前导 int3 之后）
    fs = S.find_func_start(code, expect[0], BASE)
    print('find_func_start(0x%08X) = 0x%08X  （期望 0x%08X）'
          % (expect[0], fs, FUNC_START))
    if fs != FUNC_START:
        ok = False

    # 验证写操作识别
    hits = []
    for insn in md.disasm(code, BASE):
        if insn.operands and S.is_write(insn):
            d0 = S.mem_disp(insn.operands[0])
            if d0 in (S.RANK_OFF, S.XP_OFF):
                hits.append((d0, insn.address, '%s %s' % (insn.mnemonic, insn.op_str)))
    print('识别到写 +0x24 / +0x10 的指令: %d 条' % len(hits))
    for d, a, t in hits:
        print('  +0x%X @ 0x%08X  %s' % (d, a, t))
    if len(hits) != 2:
        ok = False

    print()
    print('扫描逻辑自测:', 'OK' if ok else '失败')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

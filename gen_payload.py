# -*- coding: utf-8 -*-
"""
从开发期中间产物重新生成 payload.py（打包所需的内嵌数据）。

关键步骤：
  mustcode_body.asm 里的 MC / MC2 段原本靠 "标签名 == 段内偏移" 这一
  CE 脚本约定工作，但块长度并不整齐，跨段绝对引用（MC 段 je MC2+0x100、
  MC2 段 call MC+0x1120）会跳到错误指令上导致游戏闪退。
  这里先用 nop 把各块起始标签填充到其名字所指定的偏移，把结果固化回
  mustcode_body.asm，再据此生成 LABELS，保证 trainer.py 运行时那个
  不含 capstone、不做对齐的精简 build 也能得到完全一致的布局。

运行条件：需先完成 compile_mustcode.py（生成 mustcode_body.asm / symbols.json）。
用法：  python gen_payload.py
"""
import io
import json

import assemble
import core_hooks

# 开发期占位基址：只用于算标签偏移与跳转长度，与运行时实际分配地址无关
BASES = (0x10000000, 0x10003100, 0x10005000, 0x10005040, 0x400000)


def main():
    sym = json.load(open('symbols.json'))
    asm = io.open('mustcode_body.asm', encoding='utf-8').read()

    # 1) 对齐并把结果固化回 mustcode_body.asm（重复执行是幂等的）
    aligned = assemble.aligned_body_text(asm, sym, *BASES)
    io.open('mustcode_body.asm', 'w', encoding='utf-8').write(aligned)
    print('mustcode_body.asm 已对齐固化（%d 行）' % (aligned.count('\n') + 1))

    # 2) 用对齐后的文本重新汇编，得到与运行时布局一致的标签偏移
    mc, mc2, mc_labels, mc2_labels = assemble.build_from_src(aligned, sym, *BASES)
    lab = {'MC': mc_labels, 'MC2': mc2_labels}

    # 3) 固化标签必须与名字一致（跨段绝对引用依赖这一点）
    mismatched = []
    for seg in ('MC', 'MC2'):
        for k, v in lab[seg].items():
            if '_' not in k:
                continue
            head, suf = k.split('_', 1)
            if head not in ('mc', 'mc2'):
                continue
            try:
                exp = int(suf, 16)
            except ValueError:
                continue
            if v != exp:
                mismatched.append((k, v, exp))
    if mismatched:
        # 段内部标签（只在段内用本地标签引用）允许不对齐，仅提示
        print('[提示] 以下内部标签未对齐（段内本地引用，不影响正确性）：')
        for k, v, e in mismatched:
            print('   %-10s 实际 0x%X 名字 0x%X' % (k, v, e))
    else:
        print('标签对齐校验通过')

    hooks = core_hooks.CORE_HOOKS
    globals_ = core_hooks.DATA_GLOBALS

    with io.open('payload.py', 'w', encoding='utf-8') as w:
        w.write('# -*- coding: utf-8 -*-\n')
        w.write('# 自动生成，请勿手改。由 gen_payload.py 从\n')
        w.write('# mustcode_body.asm（已对齐）/ symbols.json / core_hooks.py 生成\n')
        w.write('ASM_TEXT = %r\n\n' % aligned)
        w.write('SYMBOLS = %r\n\n' % sym)
        w.write('LABELS = %r\n\n' % lab)
        w.write('CORE_HOOKS = %r\n\n' % hooks)
        w.write('DATA_GLOBALS = %r\n' % globals_)

    print('payload.py 已生成')
    print('  ASM_TEXT      %d 字节' % len(aligned))
    print('  SYMBOLS       %d 个返回标签' % len(sym))
    print('  LABELS        MC:%d / MC2:%d 个标签' % (len(mc_labels), len(mc2_labels)))
    print('  CORE_HOOKS    %d 个 hook' % len(hooks))
    print('  DATA_GLOBALS  %d 个' % len(globals_))
    print('  MC 段 %d 字节 / MC2 段 %d 字节' % (len(mc), len(mc2)))


if __name__ == '__main__':
    main()

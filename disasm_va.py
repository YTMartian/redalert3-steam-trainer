# -*- coding: utf-8 -*-
"""按 VA 反汇编 ra3_1.12.game（用于确认被 call 的游戏函数性质）。"""
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

PATH = r'H:\steam client\steamapps\common\Command and Conquer Red Alert 3\Data\RA3_1.12.game'
IMAGE_BASE = 0x400000


def sections():
    data = open(PATH, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[pe:pe + 4] == b'PE\x00\x00'
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    sec0 = pe + 24 + opt_size
    out = []
    for i in range(nsec):
        o = sec0 + i * 40
        name = data[o:o + 8].rstrip(b'\x00').decode('latin1')
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', data, o + 8)
        out.append((name, va, vsize, rawptr, rawsize))
    return data, out


def va_to_off(data, secs, va):
    rva = va - IMAGE_BASE
    for name, sva, vsize, rawptr, rawsize in secs:
        if sva <= rva < sva + max(vsize, rawsize):
            return rawptr + (rva - sva)
    return None


def main():
    targets = [int(x, 16) for x in sys.argv[1:]] or [0x75C200, 0x79EA50]
    data, secs = sections()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    print('节区:')
    for name, sva, vsize, rawptr, rawsize in secs:
        print('  %-8s VA=0x%08X vsize=0x%-6X raw=0x%X+0x%X' %
              (name, sva + IMAGE_BASE, vsize, rawptr, rawsize))
    for va in targets:
        off = va_to_off(data, secs, va)
        print()
        print('=== VA 0x%X (文件偏移 0x%X) ===' % (va, off if off else -1))
        if off is None:
            print('  无法映射')
            continue
        code = data[off:off + 160]
        for i in md.disasm(code, va):
            print('  0x%08X  %-24s %s %s' % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))


if __name__ == '__main__':
    main()

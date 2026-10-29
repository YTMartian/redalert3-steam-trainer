"""Extract RayaTrainer's hook stub template from its DLL.

The live stub lives in memory RayaTrainer allocates, so it disappears when the
trainer is not attached. Its bytes, however, come from the DLL, so the template
can be read statically and reproduced exactly.

The stub's shape is already known from a live capture:

    9C                pushfd
    60                pushal
    83 44 24 0C 04    add dword ptr [esp+0xc], 4     ; bumps the saved ESP
    89 E0             mov eax, esp
    50                push eax
    6A 29             push 0x29                      ; hook id 41
    B8 ........       mov eax, <dispatcher>
    FF D0             call eax
    83 C4 08          add esp, 8
    85 C0             test eax, eax
    74 ..             je  <action 0>
    83 F8 01          cmp eax, 1
    74 ..             je  <action 1>
    ...

This searches the file for that prefix and dumps the whole template, resolving
each action code to the epilogue it reaches.

Usage:
    python find_hook_stub.py [dll_path]
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

DEFAULT = Path(r"C:\Users\YTMartian\Desktop"
               r"\RayaTrainer-v0.9.0-win-x86-self-contained"
               r"\RayaTrainer.Agent.dll")

PREFIX = bytes([0x9C, 0x60, 0x83, 0x44, 0x24, 0x0C, 0x04, 0x89, 0xE0, 0x50])


def pe_sections(data: bytes):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    base = struct.unpack_from("<I", data, e_lfanew + 0x34)[0]
    off = e_lfanew + 24 + opt_size
    out = []
    for i in range(nsec):
        s = off + i * 40
        name = data[s:s + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
        out.append((name, va, vsize, rawptr, rawsize))
    return base, out


def main() -> int:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    if not path.exists():
        print(f"[!] not found: {path}")
        return 2
    data = path.read_bytes()
    base, sections = pe_sections(data)
    print(f"{path.name}  image base {base:#x}  {len(data)} bytes")
    for name, va, vsize, _rp, _rs in sections:
        print(f"  {name:8} va={va:#08x} vsize={vsize:#x}")

    hits = []
    start = 0
    while True:
        i = data.find(PREFIX, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    if not hits:
        print("\n[!] stub prefix not found; the template may be assembled at run time")
        return 1

    def file_to_va(off: int):
        for _n, va, vsize, rp, rs in sections:
            if rp <= off < rp + rs:
                return base + va + (off - rp)
        return None

    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    for off in hits:
        va = file_to_va(off)
        print(f"\n=== candidate at file {off:#x}  va {va:#x} ===")
        blob = data[off:off + 0x100]
        for ins in md.disasm(blob, va or 0):
            print(f"  {ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}")
            if ins.mnemonic in ("ret", "int3"):
                break
    return 0


if __name__ == "__main__":
    sys.exit(main())

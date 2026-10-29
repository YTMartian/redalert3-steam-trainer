"""Run the update loop at 60Hz while advancing time by half a step per frame.

The tick function (vtable+0x34 = 0x539b20) advances simulation time with:

    0x539bd6  fld1                             ; dt = 1.0
    0x539bd8  add dword ptr [edi+0x50], ebp    ; clock += 1
    0x539bdb  push ecx / fstp [esp]            ; hand dt to the timeline
    0x539be1  call 0x66d190                    ; advance by dt

and the frame-rate patches double how often that runs, which is why the result is
2.00x. Skipping whole calls is wrong: the same function also does mandatory
per-frame housekeeping (a global frame counter, animation refresh, mode checks),
and suppressing it froze the clock outright in testing.

The correct decoupling is to keep calling it every frame but advance less each
time: dt 1.0 -> 0.5 and the integer clock every other call. Total time per second
is unchanged (60 x 0.5 = 30 x 1.0) while temporal resolution doubles.

`fld1` (2 bytes) and `add dword [edi+0x50],ebp` (3 bytes) are contiguous at
0x539bd6, exactly 5 bytes, which is enough for a jump:

    0x539bd6  E9 rel32                  -> stub
    stub:     fld  dword [0.5f]
              inc [counter]; test al,1; jnz +4
              add  dword [edi+0x50], 1
              jmp  0x539bdb             (the original `push ecx`)

eax and ecx are dead across this point in the original code, so the counter code
is safe. edi is the logic object and is left untouched.

Usage:
    python hook_half_time.py test|install|remove|status [seconds]
"""
from __future__ import annotations

import ctypes
import struct
import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from sweep_fps import frames_after, LOG

SITE = 0x139BD6          # RVA of fld1 + add [edi+0x50],ebp (5 bytes)
RESUME = 0x139BDB        # RVA right after those 5 bytes
ORIG_LEN = 5
CAVE_UPDATE_FPS = 0x7C6448

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
PAGE_EXECUTE_READWRITE = 0x40


def build_stub(stub: int, counter: int, half: int, base: int) -> bytes:
    code = bytearray()
    code += b"\xD9\x05" + struct.pack("<I", half)          # fld dword [half]
    code += b"\xA1" + struct.pack("<I", counter)           # mov eax,[counter]
    code += b"\x40"                                         # inc eax
    code += b"\xA3" + struct.pack("<I", counter)           # mov [counter],eax
    code += b"\xA8\x01"                                     # test al,1
    code += b"\x75\x04"                                     # jnz +4 (skip the add)
    code += b"\x83\x47\x50\x01"                             # add dword [edi+0x50],1
    code += b"\xE9\x00\x00\x00\x00"                         # jmp RESUME
    at = len(code) - 4
    struct.pack_into("<i", code, at, (base + RESUME) - (stub + at + 4))
    return bytes(code)


def open_target():
    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return None
    h = found[0]
    k32, handle, write, werr = A.open_writer(h["pid"])
    if not handle:
        return None
    return h, k32, handle, write, werr


def main() -> int:
    mode = (sys.argv[1] if len(sys.argv) > 1 else "status").lower()
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

    got = open_target()
    if not got:
        return 2
    h, k32, handle, write, werr = got
    k32.VirtualAllocEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]
    k32.VirtualAllocEx.restype = ctypes.c_void_p
    k32.VirtualProtectEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                     ctypes.c_size_t, ctypes.c_ulong,
                                     ctypes.POINTER(ctypes.c_ulong)]
    k32.VirtualProtectEx.restype = ctypes.c_bool

    t = p.Target(h["pid"])
    base = 0
    try:
        base, _notes = p.resolve_base(t, t.modules())
        site_va = base + SITE
        orig = t.read(site_va, ORIG_LEN) or b""
        print(f"pid={h['pid']}  base={base:#010x}  site {site_va:#010x}")
        print(f"  bytes: {orig.hex(' ')}")
        # D9 E8 = fld1, 01 6F 50 = add dword [edi+0x50], ebp
        if orig != b"\xD9\xE8\x01\x6F\x50":
            print("  [!] unexpected bytes at the site; aborting")
            return 1
        if base != 0x400000:
            print(f"  [!] unexpected image base {base:#x}; stub offsets assume "
                  f"0x400000")

        if mode == "remove":
            print("  restoring..." if write(site_va, orig) else "  [!] failed")
            return 0

        if mode == "status":
            cur = t.read(site_va, ORIG_LEN) or b""
            print(f"  detoured: {cur[0] == 0xE9}")
            return 0

        # Full patch set gives 60fps at 2.00x; the stub halves the advance.
        data, code = A.load_sites()
        for rva, o, _n in reversed(code):
            write(base + rva, o)
        for rva, o, _n in reversed(data):
            write(base + rva, o)
        for rva, _o, n in data:
            write(base + rva, n)
        for rva, _o, n in code:
            write(base + rva, n)
        write(base + CAVE_UPDATE_FPS, struct.pack("<i", 60))

        page = k32.VirtualAllocEx(handle, None, 0x1000,
                                  MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE)
        if not page:
            print(f"[!] VirtualAllocEx failed (err={werr()})")
            return 1
        page = int(page)
        stub, counter, half = page + 0x100, page + 0x800, page + 0x900
        blob = build_stub(stub, counter, half, base)
        write(counter, b"\x00\x00\x00\x00")
        write(half, struct.pack("<f", 0.5))
        if not write(stub, blob):
            print(f"[!] stub write failed (err={werr()})")
            return 1
        print(f"  stub {stub:#010x} ({len(blob)} bytes)  counter {counter:#010x}"
              f"  dt const {half:#010x}")

        old = ctypes.c_ulong()
        k32.VirtualProtectEx(handle, ctypes.c_void_p(site_va), ORIG_LEN,
                             PAGE_EXECUTE_READWRITE, ctypes.byref(old))
        patch = b"\xE9" + struct.pack("<i", stub - (site_va + 5))
        if not write(site_va, patch):
            print(f"[!] detour write failed (err={werr()})")
            return 1
        k32.VirtualProtectEx(handle, ctypes.c_void_p(site_va), ORIG_LEN,
                             old.value, ctypes.byref(old))
        print(f"  half-time detour installed")

        if mode != "test":
            return 0

        time.sleep(1.2)
        off = LOG.stat().st_size if LOG.exists() else 0
        s0 = A.read_sim_seconds(t, base)
        t0 = time.perf_counter()
        time.sleep(secs)
        s1 = A.read_sim_seconds(t, base)
        real = time.perf_counter() - t0
        pr, _es, n = frames_after(off)
        cnt = t.read(counter, 4)
        calls = struct.unpack("<I", cnt)[0] if cnt else None
        if s0 is None or s1 is None:
            print("  [!] left the match while sampling")
        else:
            speed = (s1 - s0) / real
            print(f"\n  speed {speed:5.2f}x   Present "
                  f"{'n/a' if pr is None else f'{pr:5.1f}'} fps   "
                  f"({n} samples)   entries={calls}")
            print(f"  clock {s0:.2f} -> {s1:.2f}")
            ok = pr and abs(speed - 1.0) < 0.06 and pr >= 55
            print("  == GOAL: 60fps at normal speed ==" if ok
                  else "  == not the goal ==")

        if "--keep" not in sys.argv:
            write(site_va, orig)
            print("  detour removed (pass --keep to leave it installed)")
        return 0
    finally:
        t.close()
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())

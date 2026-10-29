"""Halve the per-frame logic tick with a skip-every-other-frame detour.

The data constants cannot produce 15Hz logic at 60fps: the engine only reaches
10Hz (0.67x), 20Hz (1.33x), 30Hz (2.00x) at 60fps, and 15Hz only at 45fps. So the
remaining option is to change how many ticks are run per frame.

The whole patch set gives 60fps with logic at 30Hz, i.e. 2.00x simulated time.
Halving the number of ticks gives 15Hz, exactly 1.00x, while leaving the frame
rate untouched. The engine has exactly two call sites for the logic step and both
sit in one function; this detours the unconditional one at 0x655158:

    0x655158  mov  ecx, [0xcdde84]     <- detour goes here (6 bytes)
    0x65515e  mov  edx, [ecx]
    0x655160  mov  eax, [edx+0x34]
    0x655163  push esi
    0x655164  call eax                 <- the per-frame tick
    0x655166  mov  eax, [0xcb409c]     <- resume here when skipping

The stub keeps a counter in a page allocated inside the target process: it
executes the original mov on even calls and jumps past the call on odd ones.
Nothing depends on ecx/edx/eax right after 0x655166, so skipping is safe.

Modes:
    install   allocate the stub page and detour 0x655158
    remove    restore the original 6 bytes
    status    report whether the detour is installed and the counter value
    test      install, sample speed and fps, then remove

Usage:
    python hook_tick_skip.py install|remove|status|test [seconds]
"""
from __future__ import annotations

import ctypes
import struct
import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from split_code import CAVE_RVA
from sweep_fps import frames_after, LOG

SITE = 0x255158          # RVA of `mov ecx,[0xcdde84]`
RESUME_SKIP = 0x255166   # right after the tick call
RESUME_TICK = 0x25515E   # execute the original mov, then continue
ORIG_LEN = 6
CAVE_UPDATE_FPS = 0x7C6448

PAGE_EXECUTE_READWRITE = 0x40
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000


def build_stub(stub_addr: int, counter_addr: int, base: int,
               enable_skip: bool = True) -> bytes:
    """inc counter / if odd skip the tick / else run the original load and resume.

    Two counters are kept: [counter] counts every entry, [counter+4] counts the
    entries that went on to execute the tick. Their ratio is what shows whether the
    alternating scheme is actually running half the ticks, which separates "the
    clock stopped" from "the skip never alternated".

    With enable_skip=False the stub is a transparent passthrough: it emulates the
    original 6 bytes and jumps back. Running that first proves the detour itself
    changes nothing, so any speed change can be attributed to the skipping rather
    than to a broken emulation.
    """
    tick = base + RESUME_TICK
    skip = base + RESUME_SKIP

    code = bytearray()
    if enable_skip:
        code += b"\xA1" + struct.pack("<I", counter_addr)       # mov eax,[calls]
        code += b"\x40"                                          # inc eax
        code += b"\xA3" + struct.pack("<I", counter_addr)       # mov [calls],eax
        code += b"\xA8\x01"                                      # test al,1
        code += b"\x75\x00"                                      # jnz skip
        jnz_at = len(code) - 1
        code += b"\xA1" + struct.pack("<I", counter_addr + 4)   # mov eax,[ran]
        code += b"\x40"                                          # inc eax
        code += b"\xA3" + struct.pack("<I", counter_addr + 4)   # mov [ran],eax
    else:
        jnz_at = None
    # Emulate the original instruction exactly: `mov ecx, dword ptr [0xcdde84]`
    # (8B 0D imm32). A `mov ecx, imm32` would load the address, not the pointer.
    code += b"\x8B\x0D" + struct.pack("<I", base + 0x8DDE84)
    code += b"\xE9\x00\x00\x00\x00"                       # jmp tick
    jmp_tick_at = len(code) - 4

    if enable_skip:
        skip_at = len(code)
        code += b"\xE9\x00\x00\x00\x00"                   # jmp skip
        jmp_skip_at = len(code) - 4
        code[jnz_at] = (skip_at - (jnz_at + 1)) & 0xFF
        struct.pack_into("<i", code, jmp_skip_at,
                         skip - (stub_addr + jmp_skip_at + 4))

    struct.pack_into("<i", code, jmp_tick_at,
                     tick - (stub_addr + jmp_tick_at + 4))
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
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 9.0
    # "test" alternates; "test-noskip" runs a transparent stub for comparison.
    enable_skip = mode != "test-noskip"
    if mode == "test-noskip":
        mode = "test"

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
        data, code = A.load_sites()
        site_va = base + SITE
        orig = t.read(site_va, 6) or b""
        if len(orig) != 6:
            print(f"[!] cannot read the site bytes (err={werr()})")
            return 1
        print(f"pid={h['pid']}  base={base:#010x}  site {site_va:#010x}")
        # Sanity: this must be the `mov ecx,[0xcdde84]` strip, not something else.
        expect = b"\x8B\x0D" + struct.pack("<I", base + 0x8DDE84)
        if orig[:2] != expect[:2] and orig != expect:
            print(f"  [!] unexpected bytes at the site: {orig.hex(' ')}")
            print(f"      expected {expect.hex(' ')}; aborting")
            return 1

        cur = t.read(site_va, 6)
        detoured = bool(cur) and cur[0] == 0xE9
        print(f"  bytes now: {cur.hex(' ') if cur else 'unreadable'}"
              f"   ({'detoured' if detoured else 'original'})")

        if mode == "status":
            if detoured:
                stub = site_va + 5 + struct.unpack("<i", cur[1:5])[0]
                print(f"  stub at {stub:#010x}")
                cnt = t.read(base + 0, 4)  # unused
                _ = cnt
            return 0

        if mode == "remove":
            if not write(site_va, orig):
                print(f"[!] restore failed (err={werr()})")
                return 1
            print("  detour removed, original bytes restored")
            return 0

        # install / test: add the full patch set so the frame rate is 60 and the
        # tick rate is 30Hz; the stub then halves it to 15Hz.
        for rva, o, n in reversed(code):
            write(base + rva, o)
        for rva, o, n in reversed(data):
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
        counter = page + 0x800
        stub = page + 0x100
        blob = build_stub(stub, counter, base, enable_skip)
        print(f"  stub {stub:#010x}  counter {counter:#010x}  "
              f"({len(blob)} bytes, skip={'on' if enable_skip else 'OFF'})")

        if not write(counter, b"\x00\x00\x00\x00"):
            print(f"[!] counter init failed (err={werr()})")
            return 1
        if not write(stub, blob):
            print(f"[!] stub write failed (err={werr()})")
            return 1

        old = ctypes.c_ulong()
        if not k32.VirtualProtectEx(handle, ctypes.c_void_p(site_va), 6,
                                    PAGE_EXECUTE_READWRITE, ctypes.byref(old)):
            print(f"[!] VirtualProtectEx failed (err={werr()})")
            return 1
        rel = stub - (site_va + 5)
        patch = b"\xE9" + struct.pack("<i", rel) + b"\x90"
        if not write(site_va, patch):
            print(f"[!] detour write failed (err={werr()})")
            return 1
        k32.VirtualProtectEx(handle, ctypes.c_void_p(site_va), 6,
                             old.value, ctypes.byref(old))
        print(f"  detour installed -> {stub:#010x}")

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
        cnt = t.read(counter, 8)
        calls = ran = None
        if cnt and len(cnt) == 8:
            calls, ran = struct.unpack("<II", cnt)
        if s0 is None or s1 is None:
            print("  [!] left the match while sampling")
        else:
            speed = (s1 - s0) / real
            print(f"\n  speed {speed:5.2f}x   Present "
                  f"{'n/a' if pr is None else f'{pr:5.1f}'} fps   "
                  f"({n} samples)")
            print(f"  entries={calls}  tick executed={ran}"
                  f"   clock {s0:.2f} -> {s1:.2f}")
            ok = pr and abs(speed - 1.0) < 0.06 and pr >= 55
            print("  == 60fps at normal speed ==" if ok else "  == not the goal ==")

        if "--keep" not in sys.argv:
            write(site_va, orig)
            print("  detour removed (pass --keep to leave it installed)")
        return 0
    finally:
        t.close()
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())

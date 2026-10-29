"""Run the logic tick 15 times a second while rendering at 60.

Measurement so far: with the full 60fps patch set the battle clock advances 30/s
and every per-tick system (unit motion, building animation, water) advances with the
tick count. Halving the tick's dt corrected the clock but left the tick running 30
times a second, so the visuals stayed doubled -- exactly what the user reported.

The baseline game runs the tick 15 times a second at 30fps. So the target is: keep
rendering at 60, but execute the tick 15 times a second. That is uniform -- the
clock, units and animations all advance per tick, so halving the tick rate halves
them together and keeps them consistent.

The tick is [logic]->vtable+0x34 (0x539B20), and its epilogue is `ret 4`, so a
skipped call can simply return. Entry bytes are 83 EC 2C 53 55
(`sub esp,0x2c; push ebx; push ebp`), which is 5 bytes -- enough for a jump.

Detour layout:

    even call: run the original 3 instructions, jump back to 0x539B25
    odd call : `ret 4` -- nothing pushed yet, so the stack is already correct

Since a skipped call touches no stack, the callee's `ret 4` convention is honoured
by returning directly.

This script also restores the half-time dt edit (0x539BD6) if the DLL applied it,
so the two mechanisms are never active together (that would give 0.5x).

Usage:
    python hook_tick_rate.py test|install|remove|status [seconds]
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

TICK_RVA = 0x139B20          # [logic]->vtable+0x34, == 0x539B20
RESUME_RVA = 0x139B25        # right after `sub esp,0x2c; push ebx; push ebp`
ORIG5 = b"\x83\xEC\x2C\x53\x55"
HALFTIME_RVA = 0x139BD6      # dt halving applied by the DLL
HALFTIME_ORIG = b"\xD9\xE8\x01\x6F\x50"

# The full-update path. The tick is entered with a frame argument; only the call
# whose argument equals 1 runs the real update (`cmp [esp+0x40],1 / jne 0x539bfa`),
# and the battle clock, movement and animation all advance inside that block:
#
#   0x539b6f  mov ecx, [0xce2330]     <- 6 bytes, skip target for this detour
#   ...        mode checks ...
#   0x539bd6  fld1 / add [edi+0x50],1 <- the clock
#   0x539bfa  (the "partial update" continuation)
#
# Skipping whole entries instead was wrong: it randomly discarded the arg==1 call,
# which froze the clock outright (measured 0.00x at every 4th call).
FULL_RVA = 0x139B6F
FULL_RESUME_RVA = 0x139B75
FULL_SKIP_RVA = 0x139BFA
ORIG6 = b"\x8B\x0D\x30\x23\xCE\x00"   # mov ecx,[0xce2330]

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
PAGE_EXECUTE_READWRITE = 0x40


def build_full_stub(stub: int, counter: int, base: int, step: int = 2) -> bytes:
    """Gate the full-update path: run it every `step`-th time, else jump past it.

    Emulating `mov ecx,[0xce2330]` is unnecessary on the skip path because the
    block being skipped re-establishes ecx itself; the skip jumps to 0x539bfa, the
    same place the engine's own `jne` would land, so the partial-update code still
    runs exactly once per frame.
    """
    if step < 2 or (step & (step - 1)) != 0:
        raise ValueError("step must be a power of two >= 2")
    code = bytearray()
    code += b"\xA1" + struct.pack("<I", counter)      # mov eax,[counter]
    code += b"\x40"                                    # inc eax
    code += b"\xA3" + struct.pack("<I", counter)      # mov [counter],eax
    if step == 2:
        code += b"\xA8\x01"                            # test al,1
    else:
        code += b"\x24" + bytes([step - 1])            # and al, step-1
    code += b"\x74\x00"                                # jz run (patched below)
    jz_at = len(code) - 1
    # skip path: jump to the engine's own partial-update landing point
    code += b"\xE9\x00\x00\x00\x00"
    jmp_skip_at = len(code) - 4
    run_at = len(code)
    code += b"\x8B\x0D" + struct.pack("<I", base + 0xCE2330)   # mov ecx,[0xce2330]
    code += b"\xE9\x00\x00\x00\x00"                   # jmp resume
    jmp_run_at = len(code) - 4

    code[jz_at] = (run_at - (jz_at + 1)) & 0xFF
    struct.pack_into("<i", code, jmp_skip_at,
                     (base + FULL_SKIP_RVA) - (stub + jmp_skip_at + 4))
    struct.pack_into("<i", code, jmp_run_at,
                     (base + FULL_RESUME_RVA) - (stub + jmp_run_at + 4))
    return bytes(code)


def build_stub(stub: int, counter: int, base: int, step: int = 2) -> bytes:
    """Counter gate: execute the tick on every `step`-th call, `ret 4` otherwise.

    step=2 executes half the calls, step=4 a quarter. The engine calls the tick from
    both the unconditional per-frame site and the catch-up loop, so the total entry
    rate is higher than the frame rate; the step has to be measured, not assumed.

    `and al, step-1` only works for powers of two, which keeps the stub to one
    instruction; that is enough because the observed rates are multiples of two.
    """
    if step < 2 or (step & (step - 1)) != 0:
        raise ValueError("step must be a power of two >= 2")
    code = bytearray()
    code += b"\xA1" + struct.pack("<I", counter)     # mov eax,[counter]
    code += b"\x40"                                   # inc eax
    code += b"\xA3" + struct.pack("<I", counter)     # mov [counter],eax
    if step == 2:
        code += b"\xA8\x01"                           # test al,1
    else:
        code += b"\x24" + bytes([step - 1])           # and al, step-1
    code += b"\x75\x0A"                               # jnz odd (+10)
    code += ORIG5                                     # emulated original bytes
    code += b"\xE9\x00\x00\x00\x00"                   # jmp resume
    at = len(code) - 4
    struct.pack_into("<i", code, at, (base + RESUME_RVA) - (stub + at + 4))
    code += b"\xC2\x04\x00"                           # ret 4
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
    step = 2
    if "--step" in sys.argv:
        step = int(sys.argv[sys.argv.index("--step") + 1])
    # --full gates only the full-update path (arg == 1), which is where the clock,
    # movement and animation all advance. Plain entry skipping discards that call at
    # random and freezes the clock.
    use_full = "--full" in sys.argv

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
        tick_va = base + TICK_RVA
        entry = t.read(tick_va, 5) or b""
        entry_txt = entry.hex(" ")
        detoured = entry[:1] == b"\xE9"
        print(f"pid={h['pid']}  base={base:#010x}  tick {tick_va:#010x}")
        print(f"  entry bytes: {entry_txt}"
              f"   ({'detoured' if detoured else 'original'})")
        if detoured:
            print("  restoring the old detour first")
            write(tick_va, ORIG5)
            entry = ORIG5
            entry_txt = entry.hex(" ")

        ht = t.read(base + HALFTIME_RVA, 5) or b""
        ht_installed = ht[:1] == b"\xE9"
        print(f"  half-time dt edit: "
              f"{'INSTALLED' if ht_installed else 'not installed'}")

        if mode == "remove":
            write(tick_va, ORIG5)
            write(base + FULL_RVA, ORIG6)
            print("  tick entry and full-update site restored")
            return 0

        if mode == "status":
            return 0

        if entry != ORIG5:
            print("  [!] unexpected entry bytes; aborting")
            return 1

        # Never run both mechanisms: dt halving plus tick skipping would be 0.5x.
        if ht_installed:
            if write(base + HALFTIME_RVA, HALFTIME_ORIG):
                print("  half-time dt edit reverted (tick skipping replaces it)")
            else:
                print(f"  [!] could not revert half-time edit (err={werr()})")
                return 1

        page = k32.VirtualAllocEx(handle, None, 0x1000,
                                  MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE)
        if not page:
            print(f"[!] VirtualAllocEx failed (err={werr()})")
            return 1
        page = int(page)
        stub, counter = page + 0x100, page + 0x800

        if use_full:
            target_rva, orig_bytes = FULL_RVA, ORIG6
            site_va = base + target_rva
            cur = t.read(site_va, len(orig_bytes)) or b""
            print(f"  full-update site {site_va:#010x}: {cur.hex(' ')}")
            if cur != orig_bytes:
                print("  [!] unexpected bytes at the full-update site; aborting")
                return 1
            blob = build_full_stub(stub, counter, base, step)
        else:
            target_rva, orig_bytes = TICK_RVA, ORIG5
            site_va = tick_va
            blob = build_stub(stub, counter, base, step)

        write(counter, b"\x00\x00\x00\x00")
        if not write(stub, blob):
            print(f"[!] stub write failed (err={werr()})")
            return 1
        print(f"  stub {stub:#010x} ({len(blob)} bytes)  counter {counter:#010x}"
              f"  step={step}  target="
              f"{'full-update' if use_full else 'tick entry'}")

        old = ctypes.c_ulong()
        k32.VirtualProtectEx(handle, ctypes.c_void_p(site_va), len(orig_bytes),
                             PAGE_EXECUTE_READWRITE, ctypes.byref(old))
        patch = b"\xE9" + struct.pack("<i", stub - (site_va + 5))
        if not write(site_va, patch):
            print(f"[!] detour write failed (err={werr()})")
            return 1
        k32.VirtualProtectEx(handle, ctypes.c_void_p(site_va), len(orig_bytes),
                             old.value, ctypes.byref(old))
        print(f"  detour installed (one call in {step} runs the full update)")

        if mode != "test":
            return 0

        time.sleep(1.5)
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
                  f"({n} samples)")
            print(f"  full-update entries/s ~ "
                  f"{calls / real if calls else 0:.1f}"
                  f" (1 in {step} executes)")
            print(f"  clock {s0:.2f} -> {s1:.2f}")

        if "--keep" not in sys.argv:
            write(site_va, orig_bytes)
            print("  detour removed (pass --keep to leave it installed)")
        return 0
    finally:
        t.close()
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())

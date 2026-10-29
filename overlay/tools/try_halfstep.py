"""Last route to 60fps at 1x: 30 half-speed ticks per second instead of 15.

The measurements rule out the direct route. Frame time is budget/R with
R = updateFPS/logicFPS, and ticks per frame is 1/R for R<=3 but collapses to 1/6
for R>=4 (0x655175 bails out with `cmp edi,6 / jge`). So at 60fps the engine offers
only 30 (R=2), 20 (R=3) or 10 (R>=4) ticks per second -- never the 15 that 1.00x
needs. 45fps (R=3, 15 ticks/s) was the only exact hit and it is rejected.

That leaves changing what one tick is worth rather than how many ticks run. At R=2
the engine does 30 ticks/s; if each tick advances the timeline by 0.5 instead of
1.0, the result is 15 simulated seconds per second -- 1.00x -- while the renderer
still presents 60 frames. The tick object's dt is loaded at 0x539BD6 as `fld1`,
which this replaces with `fld dword [0.5]` through a trampoline (the load needs 6
bytes and only 5 are available).

The clock half of this is already proven: this exact dt patch measured 1.00x while
rendering at 60. What is still unverified is whether unit motion, building
animation and the water share that dt. Earlier they stayed fast, but that test also
doubled the per-tick constants (0.015 -> 0.03 etc.), which would mask it. Here
those constants are left at their original values, so the answer is clean:

    animations normal  -> they follow dt, and this is done
    animations fast    -> they advance per tick, and the per-tick constants have to
                          be halved too (--halve does that)

Usage:
    python try_halfstep.py test [seconds]      apply, measure, revert
    python try_halfstep.py apply [--halve]     leave it applied for eyeballing
    python try_halfstep.py revert
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

DT_RVA = 0x139BD6           # fld1 ; add [edi+0x50], ebp
DT_RESUME_RVA = 0x139BDB
DT_ORIG = b"\xD9\xE8\x01\x6F\x50"

LOGIC_RVA, UPDATE_RVA, BUDGET_RVA = 0x8B4098, 0x8B409C, 0x8E0DBC

# Per-tick constants, original and halved. Left alone unless --halve is passed.
TICK_CONSTS = [
    (0x8E0DEC, 0.015, 0.0075),
    (0x8E0DF8, 15.0, 7.5),
    (0x8E0DF0, 30.0, 15.0),
    (0x8E0DF4, 33.3333321, 66.6666641),
    (0x8E0E34, 0.06666667, 0.03333333),
    (0x8E0ED4, 0.03333333, 0.01666667),
]

MEM_COMMIT, MEM_RESERVE = 0x1000, 0x2000
PAGE_EXECUTE_READWRITE = 0x40


def build_dt_stub(stub: int, counter: int, half: int, base: int) -> bytes:
    code = bytearray()
    code += b"\xD9\x05" + struct.pack("<I", half)      # fld dword [0.5f]
    code += b"\xA1" + struct.pack("<I", counter)       # mov eax,[counter]
    code += b"\x40"                                     # inc eax
    code += b"\xA3" + struct.pack("<I", counter)       # mov [counter],eax
    code += b"\xA8\x01"                                 # test al,1
    code += b"\x75\x04"                                 # jnz skip
    code += b"\x83\x47\x50\x01"                         # add [edi+0x50],1
    code += b"\xE9\x00\x00\x00\x00"                     # jmp resume
    at = len(code) - 4
    struct.pack_into("<i", code, at, (base + DT_RESUME_RVA) - (stub + at + 4))
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


def sample(t, base, secs: float):
    """fps from the Present counter, speed from the battle clock."""
    off = LOG.stat().st_size if LOG.exists() else 0
    s0 = A.read_sim_seconds(t, base)
    if s0 is None:
        return None, None
    t0 = time.perf_counter()
    time.sleep(secs)
    s1 = A.read_sim_seconds(t, base)
    real = time.perf_counter() - t0
    pr, _es, _n = frames_after(off)
    if s1 is None:
        return None, None
    return pr, (s1 - s0) / real


def main() -> int:
    mode = (sys.argv[1] if len(sys.argv) > 1 else "test").lower()
    secs = 8.0
    for a in sys.argv[2:]:
        if a.replace(".", "").isdigit():
            secs = float(a)
    halve = "--halve" in sys.argv

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
    try:
        base, _notes = p.resolve_base(t, t.modules())
        print(f"pid={h['pid']}  base={base:#010x}")
        cur = t.read(base + DT_RVA, 5) or b""
        installed = cur[:1] == b"\xE9"
        print(f"  dt trampoline: {'INSTALLED' if installed else 'not installed'}")

        if mode == "revert":
            write(base + DT_RVA, DT_ORIG)
            write(base + LOGIC_RVA, struct.pack("<I", 15))
            write(base + UPDATE_RVA, struct.pack("<I", 30))
            write(base + BUDGET_RVA, struct.pack("<f", 66.6666666))
            for rva, orig, _half in TICK_CONSTS:
                write(base + rva, struct.pack("<f", orig))
            print("  dt site, fps constants and per-tick constants restored")
            return 0

        if not installed:
            page = k32.VirtualAllocEx(handle, None, 0x1000,
                                      MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE)
            if not page:
                print(f"[!] VirtualAllocEx failed (err={werr()})")
                return 1
            page = int(page)
            stub, counter, half = page + 0x100, page + 0x800, page + 0x900
            write(half, struct.pack("<f", 0.5))
            write(counter, struct.pack("<I", 0))
            blob = build_dt_stub(stub, counter, half, base)
            if not write(stub, blob):
                print(f"[!] stub write failed (err={werr()})")
                return 1
            old = ctypes.c_ulong()
            k32.VirtualProtectEx(handle, ctypes.c_void_p(base + DT_RVA), 5,
                                 PAGE_EXECUTE_READWRITE, ctypes.byref(old))
            write(base + DT_RVA, b"\xE9" + struct.pack("<i", stub - (base + DT_RVA + 5)))
            k32.VirtualProtectEx(handle, ctypes.c_void_p(base + DT_RVA), 5,
                                 old.value, ctypes.byref(old))
            print(f"  dt trampoline installed (stub {stub:#010x}, dt 0.5)")

        # 60fps with R=2: the engine runs 30 ticks/s, each worth half a second-unit.
        write(base + LOGIC_RVA, struct.pack("<I", 15))
        write(base + UPDATE_RVA, struct.pack("<I", 30))
        write(base + BUDGET_RVA, struct.pack("<f", 33.3333321))
        print("  fps target 60 (budget 33.333), logicFPS 15, updateFPS 30")

        for rva, orig, half_v in TICK_CONSTS:
            write(base + rva, struct.pack("<f", half_v if halve else orig))
        print(f"  per-tick constants: {'HALVED' if halve else 'original'}")

        if mode != "test":
            print("\n  look at unit motion, building animation and the water.")
            print("  expected: 60fps with everything at normal speed.")
            return 0

        time.sleep(1.2)
        pr, sp = sample(t, base, secs)
        if pr is None or sp is None:
            print("  [!] left the match while sampling")
        else:
            print(f"\n  Present {pr:5.1f} fps   speed {sp:5.2f}x")
            print("  speed 1.00x here means the timeline follows dt; the remaining")
            print("  question is whether units and animation do too (visual).")

        if "--keep" not in sys.argv:
            write(base + DT_RVA, DT_ORIG)
            write(base + BUDGET_RVA, struct.pack("<f", 66.6666666))
            for rva, orig, _half in TICK_CONSTS:
                write(base + rva, struct.pack("<f", orig))
            print("  reverted (pass --keep to leave it applied)")
        return 0
    finally:
        t.close()
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())

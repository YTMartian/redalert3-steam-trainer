"""Apply patch subsets live so the cause of the "2x speed" can be isolated.

Reimplementing RayaTrainer's Hook 41 is not viable: its handler is a ~16KB-frame
function that mixes thread-local state, SEH and the whole update pump. So instead
of guessing, split our 20 sites into two independent groups and test them:

  logic  = the 8 .data constants (logicFPS 15 -> 30, tick-length floats)
  code   = the 12 .text sites (10 pointer redirects + the trampoline cave)

Hypothesis: the doubling of simulation speed comes from `logic`, while the
smoothness comes from `code`. If true, `code` alone gives 60fps at normal speed.

Modes:
  revert   restore every site to its on-disk value (clean baseline)
  logic    only the 8 .data constants
  code     only the 12 .text sites
  full     both (equivalent to the current 60fps toggle)
  status   just report what is applied now
  measure [secs]   read the battle clock and report simulated seconds per real
                   second: 1.0x is normal speed, 2.0x is the reported bug

The overlay's own 60fps toggle must be OFF during these experiments, otherwise
its tick() re-applies the full set and overwrites whatever this script writes.
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes

TOOLS = Path(__file__).resolve().parent

CODE_RVAS = {0x7C6420, 0x17FF0A, 0x22A756, 0x23EA21, 0x255167, 0x2686F9,
             0x2FFDAC, 0x31F457, 0x7B40A8, 0x7B40D8, 0x7B4108, 0x7B4C29}

DATA_RVAS = (0x8B4098, 0x8E0DEC, 0x8E0DBC, 0x8E0DF8,
             0x8E0DF0, 0x8E0DF4, 0x8E0E34, 0x8E0ED4)

# The .data constants are not in the captured JSON: capture_vs_disk.py compares
# code regions only (.data past its raw size is BSS and always differs), and the
# constants were sampled separately. So their byte pairs live here, matching
# fps_unlock.cpp exactly.
DATA_SITES = [
    (0x8B4098, "0f000000", "1e000000"),   # logicFPS int32 15 -> 30
    (0x8E0DEC, "90c2753c", "90c2f53c"),   # 0.015 -> 0.03
    (0x8E0DBC, "55558542", "55550542"),   # 66.667 -> 33.333
    (0x8E0DF8, "00007041", "0000f041"),   # 15.0 -> 30.0
    (0x8E0DF0, "0000f041", "00007042"),   # 30.0 -> 60.0
    (0x8E0DF4, "55550542", "55558541"),   # 33.333 -> 16.667
    (0x8E0E34, "8988883d", "8988083d"),   # 0.0667 -> 0.0333
    (0x8E0ED4, "8988083d", "8988883c"),   # 0.0333 -> 0.01667
]


def load_sites():
    patches = json.loads((TOOLS / "raya_full_patchset.json").read_text(
        encoding="utf-8"))["patches"]
    data = [(rva, bytes.fromhex(o), bytes.fromhex(n))
            for rva, o, n in DATA_SITES]
    code = []
    for e in patches:
        rva = int(e["rva"], 16)
        if rva in CODE_RVAS:
            code.append((rva, bytes.fromhex(e["disk"]), bytes.fromhex(e["live"])))
    # cave first when applying (nothing may jump into a half-written trampoline)
    code.sort(key=lambda t: (0 if t[0] == 0x7C6420 else 1, t[0]))
    return data, code


def read_sim_seconds(t, base):
    """Battle clock in seconds, or None when not in a match.

    Mirrors game_features.cpp read_battle_clock: [base+0x8DDE84] -> logic object,
    logic+0x148 = mode (must be a live battle mode), logic+0x50 = tick count at
    15 ticks per simulated second.
    """
    ptr = t.read(base + 0x8DDE84, 4)
    if not ptr:
        return None
    logic = struct.unpack("<I", ptr)[0]
    if not (0x10000 <= logic < 0x7FFFFFFF):
        return None
    mode = t.read(logic + 0x148, 4)
    if not mode:
        return None
    if struct.unpack("<I", mode)[0] not in (1, 2, 4, 6, 7):
        return None
    ticks = t.read(logic + 0x50, 4)
    if not ticks:
        return None
    n = struct.unpack("<I", ticks)[0]
    if n >= 15 * 6 * 3600:
        return None
    return n / 15.0


def measure(t, base, seconds: float) -> int:
    """Report simulated seconds per real second: 1.0 is normal speed."""
    start_sim = read_sim_seconds(t, base)
    if start_sim is None:
        print("[!] not in a match (or the battle clock is unreadable); "
              "start a skirmish first")
        return 1
    import time
    t0 = time.perf_counter()
    print(f"sampling {seconds:.0f}s of real time ...")
    time.sleep(seconds)
    end_sim = read_sim_seconds(t, base)
    real = time.perf_counter() - t0
    if end_sim is None:
        print("[!] left the match while sampling")
        return 1
    ratio = (end_sim - start_sim) / real
    print()
    print(f"  simulated {end_sim - start_sim:6.2f}s in {real:5.2f}s real")
    print(f"  speed ratio {ratio:5.2f}x  "
          f"({'normal' if 0.93 <= ratio <= 1.07 else 'scaled'})")
    print(f"  raw clock {start_sim:.2f} -> {end_sim:.2f}")
    return 0


def open_writer(pid: int):
    """Return (k32, handle, write_fn, err_fn).

    Every Win32 call needs an explicit prototype. With default ctypes prototypes
    the 64-bit PROCESS handle, the 32-bit target address and the SIZE_T all get
    marshalled as C int, which silently truncates instead of failing loudly.

    Page protection must be changed with VirtualProtectEx: VirtualProtect only
    affects the *calling* process, so against a remote address it fails with 487
    and every write is skipped. That failure is easy to miss because the write
    helper still returns False per site; it looked like "the patchset did not
    stick" rather than "the API was wrong".
    """
    import ctypes
    import ctypes.wintypes as wt

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = ctypes.c_void_p
    k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    k32.ReadProcessMemory.restype = wt.BOOL
    k32.ReadProcessMemory.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    k32.WriteProcessMemory.restype = wt.BOOL
    k32.WriteProcessMemory.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    k32.VirtualProtectEx.restype = wt.BOOL
    k32.VirtualProtectEx.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
        wt.DWORD, ctypes.POINTER(wt.DWORD)]

    raw = k32.OpenProcess(0x1F0FFF, False, pid)
    if not raw:
        print(f"[!] OpenProcess failed ({ctypes.get_last_error()}); "
              f"run this as administrator")
        return k32, None, None, None
    handle = ctypes.c_void_p(raw)

    def write(addr: int, buf: bytes) -> bool:
        old = wt.DWORD(0)
        if not k32.VirtualProtectEx(handle, ctypes.c_void_p(addr), len(buf),
                                    0x40, ctypes.byref(old)):
            return False
        got = ctypes.c_size_t(0)
        ok = k32.WriteProcessMemory(handle, ctypes.c_void_p(addr), buf,
                                    len(buf), ctypes.byref(got))
        k32.VirtualProtectEx(handle, ctypes.c_void_p(addr), len(buf), old.value,
                             ctypes.byref(old))
        return bool(ok) and got.value == len(buf)

    def err() -> int:
        return ctypes.get_last_error()

    return k32, handle, write, err


def apply_state(write, base: int, data, code,
                want_logic: bool, want_code: bool) -> list[str]:
    """Put the target exactly into one of the four states.

    Everything is reverted first so the caller never inherits bits of a previous
    mode. The cave is written first and cleared last: a redirect must never point
    at a half-written trampoline.

    Returns the list of failures instead of returning silently, so a broken write
    path can never be mistaken for "the patch had no effect".
    """
    fails: list[str] = []

    for rva, o, _n in reversed(code):
        if not write(base + rva, o):
            fails.append(f"revert code {rva:#x}")
    for rva, o, _n in reversed(data):
        if not write(base + rva, o):
            fails.append(f"revert data {rva:#x}")
    if want_code:
        for rva, _o, n in code:
            if not write(base + rva, n):
                fails.append(f"apply code {rva:#x}")
    if want_logic:
        for rva, _o, n in data:
            if not write(base + rva, n):
                fails.append(f"apply data {rva:#x}")
    return fails


def applied_counts(t, base, data, code):
    on_c = sum(1 for rva, _o, n in code if t.read(base + rva, len(n)) == n)
    on_d = sum(1 for rva, _o, n in data if t.read(base + rva, len(n)) == n)
    return on_d, on_c


def main() -> int:
    mode = sys.argv[1].lower() if len(sys.argv) > 1 else "status"
    if mode not in ("revert", "logic", "code", "full", "status", "measure"):
        print(__doc__)
        return 1

    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    h = found[0]

    k32, handle, write, werr = open_writer(h["pid"])
    if not handle:
        return 2

    t = p.Target(h["pid"])
    try:
        mods = t.modules()
        base, notes = p.resolve_base(t, mods)
        print(f"pid={h['pid']}  base={base:#010x}")
        print(f"anchors: {notes}")

        data, code = load_sites()

        if mode == "measure":
            secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
            return measure(t, base, secs)

        if mode == "status":
            print()
            for label, group in (("logic", data), ("code", code)):
                on = off = 0
                for rva, o, n in group:
                    cur = t.read(base + rva, len(n))
                    if cur == n:
                        on += 1
                    elif cur == o:
                        off += 1
                print(f"  {label}: {on} applied / {off} original"
                      f" / {len(group) - on - off} other")
            for rva, name in ((0x8B4098, "logicFPS"), (0x8B409C, "updateFPS")):
                raw = t.read(base + rva, 4)
                print(f"  {name} = "
                      f"{struct.unpack('<I', raw)[0] if raw else '?'}")
            return 0

        want = {"revert": (False, False), "logic": (True, False),
                "code": (False, True), "full": (True, True)}[mode]
        want_logic, want_code = want
        print(f"\nmode '{mode}': logic={want_logic} code={want_code}")

        fails = apply_state(write, base, data, code, want_logic, want_code)
        if fails:
            print(f"  [!] {len(fails)} write(s) failed, e.g. {fails[0]} "
                  f"(err={werr()})")

        print()
        on_d, on_c = applied_counts(t, base, data, code)
        print(f"  code sites applied:  {on_c}/{len(code)}")
        print(f"  logic sites applied: {on_d}/{len(data)}")
        for rva, name in ((0x8B4098, "logicFPS"), (0x8B409C, "updateFPS")):
            raw = t.read(base + rva, 4)
            print(f"  {name} = "
                  f"{struct.unpack('<I', raw)[0] if raw else '?'}")
        print()
        print("Objective check: python apply_subset.py measure 8")
        return 0
    finally:
        t.close()
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())

"""Test RayaTrainer's frame-limiter bytes alone, with no rate constants touched.

Reading the trainer's two single-byte edits showed they are not rate hacks:

    0x63DC21  1D -> 10   the game's own frame limiter
    0x63DC33  1D -> 10     while (timeGetTime() - last < 0x1D) spin;  29ms ~= 34fps
    0x668783  73 -> EB   `jae` -> `jmp` right after
                         `cmp eax,esi`, forcing the catch-up loop to run at most one
                         update per frame instead of looping.

That pair suggests the engine paces frames by real elapsed time and catches up by
running extra updates. If so, raising the limiter alone gives 60fps, and killing the
catch-up keeps one update per frame using that frame's real delta -- 1x, with no
constant doubling and no tick skipping.

Everything else we tried doubled the engine's rate constants, which is what made
units, animation and water run at 2x. So this test deliberately patches exactly
three bytes and nothing else: no updateFPS redirect, no logicFPS change, no cave.

Expect: Present near 60fps and the clock near 1.00x if the hypothesis holds.
If fps stays at 30, the limiter is not the only pacer and the updateFPS redirects
are still required.

Usage:
    python try_frame_limiter.py test [seconds]
    python try_frame_limiter.py apply|revert
"""
from __future__ import annotations

import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from sweep_fps import frames_after, LOG

# (rva, original byte, replacement) -- the trainer's three non-constant edits.
SITES = [
    (0x23DC21, b"\x1D", b"\x10"),   # frame limiter: 29ms -> 16ms
    (0x23DC33, b"\x1D", b"\x10"),
    (0x268783, b"\x73", b"\xEB"),   # jae -> jmp: one update per frame
]

# Site groups, so each byte's effect can be measured on its own. Measuring the set
# together said 2.00x, which does not say which byte did it; the limiter and the
# catch-up edit have opposite plausible effects, so they must be separated.
GROUPS = {
    "all": SITES,
    "limiter": SITES[:2],       # only the 29ms -> 16ms frame limiter bytes
    "catchup": SITES[2:],       # only `jae` -> `jmp` after cmp eax,esi
}


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
    mode = (sys.argv[1] if len(sys.argv) > 1 else "test").lower()
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
    group = "all"
    if "--only" in sys.argv:
        group = sys.argv[sys.argv.index("--only") + 1]
    if group not in GROUPS:
        print(f"[!] --only must be one of {sorted(GROUPS)}")
        return 1
    sites = GROUPS[group]

    got = open_target()
    if not got:
        return 2
    h, k32, handle, write, werr = got
    t = p.Target(h["pid"])
    try:
        base, _notes = p.resolve_base(t, t.modules())
        print(f"pid={h['pid']}  base={base:#010x}")

        state = []
        for rva, orig, new in sites:
            cur = t.read(base + rva, 1) or b""
            state.append("patched" if cur == new else
                         "original" if cur == orig else
                         f"unexpected({cur.hex()})")
            print(f"  {base + rva:#010x}: {state[-1]}")
        print()

        if mode == "revert":
            for rva, orig, _new in SITES:
                write(base + rva, orig)
            print("  bytes restored")
            return 0

        if mode == "apply":
            for rva, _orig, new in sites:
                write(base + rva, new)
            print(f"  bytes applied ({group}; no rate constants touched)")
            return 0

        # test: apply, measure, revert
        for rva, _orig, new in sites:
            write(base + rva, new)

        time.sleep(1.0)
        off = LOG.stat().st_size if LOG.exists() else 0
        t.sleep_marker = None
        s0 = A.read_sim_seconds(t, base)
        t0 = time.perf_counter()
        time.sleep(secs)
        s1 = A.read_sim_seconds(t, base)
        real = time.perf_counter() - t0
        pr, _es, n = frames_after(off)

        if s0 is None or s1 is None:
            print("  [!] left the match while sampling")
        else:
            print(f"\n  Present {'n/a' if pr is None else f'{pr:5.1f}'} fps"
                  f"   ({n} samples)")
            print(f"  speed   {(s1 - s0) / real:5.2f}x   (clock {s0:.2f} -> "
                  f"{s1:.2f})")
            print()
            print("  hypothesis: ~60fps with 1.00x means the engine paces by real")
            print("  time and the constant doubling was never needed.")
            print("  If fps stayed ~30, the updateFPS redirects are still required;")
            print("  if fps is 60 but speed is 2x, the simulation uses a fixed step.")

        if "--keep" not in sys.argv:
            for rva, orig, _new in sites:
                write(base + rva, orig)
            print("  bytes restored")
        return 0
    finally:
        t.close()
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())

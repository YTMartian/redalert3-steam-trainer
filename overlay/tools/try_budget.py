"""Change only the frame budget, leaving every rate constant alone.

Disassembling the real pacer (0x66871D, rva 0x2671D) showed how frames are timed:

    0x66871D  now = time()
    0x668722  budget = 66666.7 / ([obj+0x17c] * [0xcb4644])   ; ms this frame needs
    0x66874A  elapsed = now - last
    0x668761  if (elapsed >= budget) next frame               ; the `jae` at 0x268783
    0x668785  else spin until elapsed >= budget               ; the actual frame cap
    0x6687BA  ecx = logicFPS                                  ; ticks per frame

Measured: deleting the spin (`jae` -> `jmp`) gave exactly 2.00x, and changing
0x63DC21 (29ms -> 16ms) changed nothing -- so this spin is the real cap and
0x63DC21 belongs to a different loop.

The budget divides 66.6667, so halving that constant halves the frame time: 60fps.
The simulation is paced from real elapsed time and logicFPS, neither of which this
touches, so speed should stay 1.00x. That is the combination we want, and it is why
`logic` as a group was never tested cleanly -- it also doubles logicFPS, which at
0x6687BA doubles the ticks per frame outright.

Groups:
    budget      0x8E0DBC  66.667 -> 33.333   (frame time halved)
    budget4     0x8E0DBC  66.667 -> 16.667   (frame time quartered, uncapped)
    ticks       0x8B4098  logicFPS 15 -> 30   (the known 2x switch, for reference)
    frametime   0x8E0DF4  33.333 -> 16.667   (reported timestep, paired with budget)
    budget_ft   budget + frametime

Usage:
    python try_budget.py revert
    python try_budget.py apply budget
    python try_budget.py measure 8
"""
from __future__ import annotations

import struct
import sys
import time

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from sweep_fps import frames_after, LOG

CONSTS = {
    # rva: (original bytes, replacement bytes, what it does)
    0x8E0DBC: ("55558542", "55550542", "frame budget 66.667 -> 33.333 (60fps)"),
    0x8E0DF4: ("55550542", "55558541", "reported timestep 33.333 -> 16.667"),
    0x8B4098: ("0f000000", "1e000000", "logicFPS 15 -> 30 (the known 2x)"),
    0x8B409C: ("1e000000", "3c000000", "updateFPS 30 -> 60"),
}

# Measured law: ticks/second = fps * logicFPS / updateFPS.
#   baseline      30fps, 15/30 = 1/2 -> 15 ticks/s -> 1.00x
#   budget halved 60fps, 15/30 = 1/2 -> 30 ticks/s -> 2.00x
# So halving the frame time doubles speed only because the ratio stays 1/2. To get
# 60fps at 15 ticks/s the ratio must become 1/4: updateFPS 60 with logicFPS left at
# 15. RayaTrainer doubles both (15->30, 30->60), keeping 1/2 -- which is exactly why
# it always ran at 2x. Neither constant is touched by the ratio-only group.
GROUPS = {
    "budget": [0x8E0DBC],
    "budget4": [(0x8E0DBC, "55558542", "5555a841")],   # 66.667 -> 16.667
    "ticks": [0x8B4098],
    "updatefps": [0x8B409C],
    "frametime": [0x8E0DF4],
    "budget_ft": [0x8E0DBC, 0x8E0DF4],
    # the candidate: 60fps frame pace with the tick ratio widened to 1/4
    "ratio": [0x8E0DBC, 0x8B409C],
}


def entries(group: str):
    out = []
    for item in GROUPS[group]:
        if isinstance(item, tuple):
            rva, orig, new = item
            out.append((rva, bytes.fromhex(orig), bytes.fromhex(new)))
        else:
            orig, new, _label = CONSTS[item]
            out.append((item, bytes.fromhex(orig), bytes.fromhex(new)))
    return out


def main() -> int:
    mode = (sys.argv[1] if len(sys.argv) > 1 else "measure").lower()

    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    h = found[0]
    k32, handle, write, werr = A.open_writer(h["pid"])
    if not handle:
        return 2
    t = p.Target(h["pid"])
    try:
        base, _notes = p.resolve_base(t, t.modules())
        print(f"pid={h['pid']}  base={base:#010x}")

        if mode == "revert":
            for rva, orig, new in (entries("budget") + entries("ticks")
                                   + entries("frametime") + entries("updatefps")):
                write(base + rva, orig)
            print("  budget / frametime / logicFPS / updateFPS restored")
            return 0

        if mode == "apply":
            group = sys.argv[2] if len(sys.argv) > 2 else "budget"
            if group not in GROUPS:
                print(f"[!] group must be one of {sorted(GROUPS)}")
                return 1
            for rva, _orig, new in entries(group):
                write(base + rva, new)
            print(f"  applied '{group}': "
                  f"{', '.join(f'{base + rva:#x}' for rva, _o, _n in entries(group))}")
            for rva, _orig, _new in entries(group):
                raw = t.read(base + rva, 4) or b""
                print(f"    {base + rva:#010x} = {struct.unpack('<f', raw)[0]:.4g}"
                      if len(raw) == 4 else "    ?")
            print("  (logicFPS left at "
                  f"{struct.unpack('<I', t.read(base + 0x8B4098, 4))[0]})")
            return 0

        # measure: report fps and speed for whatever is applied now
        secs = float(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].replace(
            ".", "").isdigit() else 8.0
        lf = struct.unpack("<I", t.read(base + 0x8B4098, 4))[0]
        bd = struct.unpack("<f", t.read(base + 0x8E0DBC, 4))[0]
        print(f"  logicFPS={lf}  frame budget constant={bd:.4g}")
        off = LOG.stat().st_size if LOG.exists() else 0
        s0 = A.read_sim_seconds(t, base)
        t0 = time.perf_counter()
        time.sleep(secs)
        s1 = A.read_sim_seconds(t, base)
        real = time.perf_counter() - t0
        pr, _es, n = frames_after(off)
        if s0 is None or s1 is None:
            print("  [!] left the match while sampling")
            return 1
        print(f"  Present {'n/a' if pr is None else f'{pr:5.1f}'} fps"
              f"   speed {(s1 - s0) / real:5.2f}x   ({n} samples)")
        print()
        print("  want: ~60 fps at 1.00x. 60fps with 2.00x means the budget does not")
        print("  drive the cap and the engine counts ticks per frame instead.")
        return 0
    finally:
        t.close()
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())

"""Emulate the engine's per-frame tick-catchup count.

The function at 0x655140 computes how many simulation steps to run in the current
frame, from R = updateFPS / logicFPS:

    0x655166  mov  eax, [0xcb409c]        ; updateFPS
    0x65516d  div  [0xcb4098]             ; R = updateFPS / logicFPS
    0x655175  cmp  edi, 6 / jge           ; R >= 6 bails out
    0x65517e  lea  ecx, [esi-1]

then runs the loop at 0x6551b0 calling [0xcdde84]->vtable+0x34 once per step.

The integer sequence is exact here, including the signed magic-number division, so
this reproduces what the CPU computes rather than approximating it. The numbers
are what decide whether halving the count is a single clean edit.

Usage:
    python emulate_ticks.py
"""
from __future__ import annotations


def _s32(x: int) -> int:
    x &= 0xFFFFFFFF
    return x - 0x100000000 if x >= 0x80000000 else x


def _sdiv(a: int, b: int) -> int:
    """x86 idiv truncates toward zero, unlike Python's floor division."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def ticks(n: int, ratio: int) -> int:
    """Simulation steps run for frame n at the given updateFPS/logicFPS ratio."""
    if ratio <= 0 or ratio >= 6:
        return 0
    ecx = _s32((n - 1) * ratio)
    prod = ecx * 0x2AAAAAAB
    edx = _s32(prod >> 32)
    sign = (edx & 0xFFFFFFFF) >> 31          # shr ecx,0x1f on edx
    eax = edx + sign + 1                     # lea eax,[edx+ecx+1]
    eax = eax + eax * 2                      # lea eax,[eax+eax*2]
    eax = eax + eax                          # add eax,eax
    eax = _s32(eax)
    eax = _sdiv(eax, ratio)                  # idiv edi
    end = eax + 1                            # add edi, 1
    return max(0, end - (n + 1))


def main() -> int:
    cols = list(range(1, 13))
    print("simulation steps per frame, by ratio R = updateFPS/logicFPS")
    print("  R " + "".join(f"n={n:<4}" for n in cols))
    for r in range(1, 7):
        print(f" {r:>2} " + "".join(f"{ticks(n, r):<6}" for n in cols))

    print()
    print("steady state (n large), steps per frame:")
    for r in range(1, 6):
        vals = {ticks(n, r) for n in range(40, 60)}
        print(f"  R={r}: {sorted(vals)}")

    print()
    print("what matters for the unlock:")
    print("  R=4 with the ratio unchanged  -> 60fps but 2x simulated time")
    print("  R=4 with the step count halved -> 60fps at 1x simulated time")
    r2 = ticks(50, 2)
    r4 = ticks(50, 4)
    print(f"  steps at R=2 (30fps, normal speed): {r2}")
    print(f"  steps at R=4 (60fps, 2x speed)    : {r4}")
    print(f"  ratio between them                : "
          f"{r4 / r2 if r2 else float('nan'):.3f}x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

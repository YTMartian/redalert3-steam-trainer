"""Prove whether RayaTrainer's 60fps is the float table, the code hook, or both.

A full-image diff of a live game is useless: timers, counters and frame state
change every tick, so real signal drowns in noise. This narrows the question to
the nine addresses that actually matter and prints a clean before/after:

  * the eight float/int constants that look like "double every tick rate"
  * the JMP at 0x265560 (Hook 41 / _BackFrameRateUnlockGameUpdate)

Run it while RayaTrainer's 60fps is ON, then flip the toggle OFF and press Enter.
Whichever rows revert are the real patch surface.
"""
from __future__ import annotations

import struct
import sys
import time

import fps_patch_probe as p
from list_ra3_processes import scan_game_processes

LOGIC_RVA = 0x8B4098
UPDATE_RVA = 0x8B409C
HOOK_RVA = 0x265560

# (rva, label, expected_off_u32, expected_on_u32)
SITES = [
    (0x8B4098, "logicFPS   ", 0x0000000F, 0x0000001E),
    (0x8E0DEC, "0.015/0.03 ", 0x3C75C290, 0x3CF5C290),
    (0x8E0DBC, "66.7/33.3  ", 0x42855555, 0x42055555),
    (0x8E0DF8, "15.0/30.0  ", 0x41700000, 0x41F00000),
    (0x8E0DF0, "30.0/60.0  ", 0x41F00000, 0x42700000),
    (0x8E0DF4, "33.3/16.7  ", 0x42055555, 0x41855555),
    (0x8E0E34, "0.0667/0.03 ", 0x3D888889, 0x3D088889),
    (0x8E0ED4, "0.0333/0.01 ", 0x3D088889, 0x3C888889),
]


def f32(u: int) -> float:
    return struct.unpack("<f", struct.pack("<I", u))[0]


def read_state(t: "p.Target", base: int) -> dict:
    st: dict[int, int | None] = {}
    for rva, _label, _off, _on in SITES:
        raw = t.read(base + rva, 4)
        st[rva] = struct.unpack("<I", raw)[0] if raw else None
    hook = t.read(base + HOOK_RVA, 8)
    st[HOOK_RVA] = hook
    st[UPDATE_RVA] = struct.unpack("<I", t.read(base + UPDATE_RVA, 4))[0] \
        if t.read(base + UPDATE_RVA, 4) else None
    return st


def pause(msg: str, fallback: int = 25) -> None:
    """Wait for Enter, falling back to a countdown when stdin is not usable.

    isatty() is not trustworthy here: the shell can report a TTY while stdin is
    already at EOF, so input() raises instead of returning. Catch that rather
    than letting the capture die after the game has been set up.
    """
    if sys.stdin is not None and sys.stdin.isatty():
        try:
            input(msg)
            return
        except EOFError:
            pass
    print(msg.rstrip() + f" (no input available; auto-continuing in {fallback}s)")
    for left in range(fallback, 0, -1):
        print(f"\r  {left}s ...", end="", flush=True)
        time.sleep(1.0)
    print("\r  go        ")


def classify(u: int | None, off: int, on: int) -> str:
    if u is None:
        return "unreadable"
    if u == off:
        return "OFF value"
    if u == on:
        return "ON value"
    return "NEITHER"


def main() -> int:
    found, _ = scan_game_processes()
    if not found:
        print("game not running -- start it and enter a match first")
        return 1
    h = found[0]
    t = p.Target(h["pid"])
    try:
        mods = t.modules()
        base, notes = p.resolve_base(t, mods)
        print(f"pid={h['pid']} ({h['name']})  base={base:#010x}")
        print(f"anchors: {notes}")

        print()
        print("=== step 1/2: RayaTrainer 60fps must be ON right now ===")
        print("    (the game should currently feel like it runs at 60)")
        pause("press Enter to sample the ON state... ")
        on_state = read_state(t, base)

        print()
        print("=== step 2/2: switch RayaTrainer's 60fps OFF, then confirm ===")
        pause("turn it OFF now, then press Enter to sample the OFF state... ")
        off_state = read_state(t, base)

        print()
        print(f"{'RVA':<10}{'label':<12}{'ON sample':>12}{'OFF sample':>12}"
              f"   {'ON was':<10}{'OFF now':<10}{'reverted?'}")
        print("-" * 86)
        reverted = []
        for rva, label, off_v, on_v in SITES:
            a = on_state.get(rva)
            b = off_state.get(rva)
            ca, cb = classify(a, off_v, on_v), classify(b, off_v, on_v)
            rev = "YES" if (ca == "ON value" and cb == "OFF value") else ""
            if rev:
                reverted.append(rva)
            fa = f"{f32(a):.5f}" if a is not None and a & 0x7F800000 != 0x7F800000 else str(a)
            fb = f"{f32(b):.5f}" if b is not None and b & 0x7F800000 != 0x7F800000 else str(b)
            print(f"{rva:#08x}  {label:<12}"
                  f"{fa:>12}{fb:>12}   {ca:<10}{cb:<10}{rev}")

        print()
        print("== hook site 0x265560 ==")
        for tag, st in (("ON ", on_state), ("OFF", off_state)):
            raw = st.get(HOOK_RVA) or b""
            looks_hook = "E9 jmp" if raw[:1] == b"\xE9" else "plain code"
            print(f"  {tag}: {raw[:8].hex(' ')}   ({looks_hook})")
        on_raw = on_state.get(HOOK_RVA) or b""
        off_raw = off_state.get(HOOK_RVA) or b""
        hook_reverted = on_raw[:1] == b"\xE9" and off_raw[:1] != b"\xE9"

        print()
        print("== verdict ==")
        if reverted:
            print(f"[+] {len(reverted)} float/int constant(s) really are patched by "
                  f"the toggle:")
            for r in reverted:
                print(f"      {r:#08x}")
        else:
            print("[=] No constant reverted: the float table is NOT what the")
            print("    toggle changes (it is engine-initialised, or the trainer")
            print("    keeps it patched independently of the toggle).")
        if hook_reverted:
            print("[+] The JMP at 0x265560 was removed when toggled OFF -> the")
            print("    60fps unlock is that code hook (Hook 41 pump).")
        else:
            print("[=] The JMP at 0x265560 is still present with the toggle OFF:")
            print("    it is either not the 60fps hook, or the trainer leaves its")
            print("    hook installed and only gates it internally.")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

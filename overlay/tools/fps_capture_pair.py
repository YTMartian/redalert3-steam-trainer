"""Capture RayaTrainer's 60fps patchset by diffing every module in the process.

Why a full-image diff
---------------------
Earlier attempts hardcoded RayaTrainer's RVAs and patched the wrong bytes: the
sites read back as machine code. The reason is that RayaTrainer's target build
is not the one installed here -- it keys the variant on the game-update
function at RVA 0x265560, while this install has it at 0x255166 (a different
1.12 variant, Chinese SKU).

So stop trusting any address list. Snapshot every loaded module's image, flip
the toggle in RayaTrainer, snapshot again, and let the diff name the sites.
Diffing all modules also covers the case where the trainer patches RA3.exe
rather than ra3_1.12.game.

Usage -- game must already be IN A MATCH:

    python fps_capture_pair.py

It pauses twice so you only have to flip the toggle once, then prints every
changed region as (module, RVA, before bytes, after bytes).
"""
from __future__ import annotations

import json
import os
import struct
import sys
import time

import fps_patch_probe as p
from list_ra3_processes import explain_missing, is_engine_module, scan_game_processes

MAX_REPORT_PER_MODULE = 48
# Skip modules that change every frame no matter what (nothing to learn there)
SKIP_MODULES = {"ra3_overlay_v4.dll", "ra3_overlay.dll"}


def here(name: str) -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), name)


def size_of_image(t: "p.Target", base: int) -> int:
    head = t.read(base, 0x400)
    if not head or head[:2] != b"MZ":
        return 0
    lfanew = struct.unpack_from("<I", head, 0x3C)[0]
    if lfanew + 0x60 > len(head) or head[lfanew:lfanew + 4] != b"PE\x00\x00":
        return 0
    return struct.unpack_from("<I", head, lfanew + 0x38)[0]


def snapshot(t: "p.Target", modules) -> dict[str, dict]:
    out = {}
    for base, _size, name in modules:
        low = name.lower()
        if low in SKIP_MODULES:
            continue
        size = size_of_image(t, base)
        if size <= 0 or size > 0x4000000:  # ignore absurd/unknown
            continue
        out[name] = {
            "base": base,
            "bytes": t.read_committed(base, size, chunk=0x40000),
        }
    return out


def diff_runs(a: bytes, b: bytes) -> list[tuple[int, bytes, bytes]]:
    """Changed runs, merging gaps under 8 bytes so a patch stays in one piece."""
    out = []
    i = 0
    n = min(len(a), len(b))
    while i < n:
        if a[i] == b[i]:
            i += 1
            continue
        start = i
        last = i
        while i < n and i - last < 8:
            if a[i] != b[i]:
                last = i
            i += 1
        end = last + 1
        out.append((start, a[start:end], b[start:end]))
    return out


def resolve(wait: bool = False) -> tuple["p.Target", list] | None:
    """Find the process holding the RA3 engine module.

    Searched by module name, not process name: the launcher (RA3.exe, 9 MB) and
    the engine payload (ra3_1.12.game) both want 0x400000 and cannot coexist in
    one address space, so the process actually running the game is often named
    something else on this install.
    """
    while True:
        found, denied = scan_game_processes()
        if found:
            break
        if not wait:
            explain_missing(denied)
            return None
        print("[i] waiting for the game to reach an in-match state "
              "(Ctrl+C to give up) ...", end="", flush=True)
        try:
            for _ in range(20):
                time.sleep(1.0)
                retry, _ = scan_game_processes()
                if retry:
                    found = retry
                    break
        except KeyboardInterrupt:
            print()
            return None
        print()
        if found:
            break

    h = found[0]
    if len(found) > 1:
        print(f"[i] {len(found)} candidates, using pid={h['pid']}")
    t = p.Target(h["pid"])
    print(f"[i] pid={h['pid']} ({h['name']})  engine module at "
          f"{h['engine'][0][0]:#010x}  ({len(h['modules'])} modules)")
    return t, h["modules"]


def pause(msg: str, fallback_seconds: int = 20) -> None:
    """Wait for Enter, but do not crash when stdin is not interactive.

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
    print(msg.rstrip() + f" (no input available; auto-continuing in {fallback_seconds}s)")
    for left in range(fallback_seconds, 0, -1):
        print(f"\r  {left}s ...", end="", flush=True)
        time.sleep(1.0)
    print("\r  go        ")


def main() -> int:
    wait = "--wait" in sys.argv
    # --reverse: the toggle is currently ON (the trainer persists its desired
    # state and re-applies on attach), so capture ON first and diff against OFF.
    rev = "--reverse" in sys.argv
    label_a, label_b = ("ON", "OFF") if rev else ("OFF", "ON")
    got = resolve(wait)
    if not got:
        return 2
    t, modules = got
    try:
        print()
        if rev:
            print("=== step 1/2: leave RayaTrainer's 60fps ON ===")
            print("    (it is already ON if the game is currently running fast)")
            pause("press Enter to take the ON snapshot... ")
        else:
            print("=== step 1/2: RayaTrainer's 60fps toggle must be OFF ===")
            pause("press Enter to take the OFF snapshot... ")
        a = snapshot(t, modules)
        print(f"[i] captured {len(a)} module(s) as {label_a}")

        print()
        if rev:
            print("=== step 2/2: now switch RayaTrainer's 60fps OFF ===")
            pause("turn it OFF now, then press Enter to take the OFF snapshot... ")
        else:
            print("=== step 2/2: now switch RayaTrainer's 60fps ON ===")
            pause("turn it ON now, then press Enter to take the ON snapshot... ")
        b = snapshot(t, modules)
        print(f"[i] captured {len(b)} module(s) as {label_b}")

        total = 0
        report = []
        for name in sorted(set(a) & set(b)):
            runs = diff_runs(a[name]["bytes"], b[name]["bytes"])
            if not runs:
                continue
            total += len(runs)
            print()
            print(f"## {name}  base={a[name]['base']:#010x}  "
                  f"{len(runs)} changed region(s)  [{label_a} -> {label_b}]")
            for start, bf, af in runs[:MAX_REPORT_PER_MODULE]:
                print(f"  RVA {start:08X}  len={len(bf)}")
                print(f"    {label_a:<5} " + " ".join(f"{x:02X}" for x in bf[:24])
                      + (" ..." if len(bf) > 24 else ""))
                print(f"    {label_b:<5} " + " ".join(f"{x:02X}" for x in af[:24])
                      + (" ..." if len(af) > 24 else ""))
                report.append({
                    "module": name,
                    "base": a[name]["base"],
                    "rva": f"{start:08X}",
                    "length": len(bf),
                    f"bytes_{label_a.lower()}": bf.hex(),
                    f"bytes_{label_b.lower()}": af.hex(),
                })
            if len(runs) > MAX_REPORT_PER_MODULE:
                print(f"  ... {len(runs) - MAX_REPORT_PER_MODULE} more")

        print()
        if total == 0:
            print("[!] Nothing changed anywhere. Either the toggle was not flipped,")
            print("[!] or RayaTrainer patches memory outside the module images")
            print("[!] (it allocates its own trampolines, which a module diff")
            print("[!] can never see). Try the --reverse direction as well.")
            return 4

        out = here("fps_patchset.json")
        payload = {
            "direction": f"{label_a} -> {label_b}",
            "patches": report,
        }
        with open(out, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2)
        print(f"[+] {total} changed region(s) total")
        print(f"[i] wrote {out}")
        print("[i] Send that file (or this console output) back.")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

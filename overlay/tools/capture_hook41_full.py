"""Capture RayaTrainer's Hook 41 in full, and measure what it actually does.

Two questions, one run:

  1. Is the stub installed, and what are its action codes? The stub routes the
     dispatcher's return value (0..4) to different epilogues, so the tail of the
     stub is what decides whether a frame runs logic, is skipped, or spins.
     The live bytes are the only source: the DLL assembles this stub at run time,
     so there is no template to read from disk.

  2. Does RayaTrainer's own 60fps unlock keep the game at normal speed? Every
     measurement so far combined its patches with ours, so its own behaviour has
     never been isolated. If it also runs at 2x there is nothing to replicate; if
     it sits at 1.00x then its hook is worth reproducing exactly.

Speed is read from the battle clock and frame rate from the overlay log's Present
counter, the same instruments used for the patch experiments.

Prerequisite: the game must be launched through RayaTrainer with its own
"Frame Rate Unlock 60fps" switch ON, so the stub exists.

Usage:
    python capture_hook41_full.py [seconds]
"""
from __future__ import annotations

import struct
import sys
import time
from pathlib import Path

import apply_subset as A
import fps_patch_probe as p
from list_ra3_processes import explain_missing, scan_game_processes
from sweep_fps import LOG, frames_after

SITE_RVAS = (0x265560, 0x265555)
OUT = Path(__file__).resolve().parent / "hook41_handler.txt"


def main() -> int:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0

    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    h = found[0]
    t = p.Target(h["pid"])
    try:
        mods = t.modules()
        base, notes = p.resolve_base(t, mods)
        print(f"pid={h['pid']} ({h['name']})  base={base:#010x}")
        print(f"anchors: {notes}")

        # Any Raya module present? Without it there is no hook to capture.
        # modules() yields (base, size, name) triples.
        raya = [m[2] for m in mods if "raya" in m[2].lower() or
                "mustcode" in m[2].lower() or "keystone" in m[2].lower()]
        print(f"\ntrainer modules: "
              f"{raya if raya else 'none (not attached)'}")

        stub = None
        for rva in SITE_RVAS:
            raw = t.read(base + rva, 8)
            print(f"  site {rva:#x}: {raw.hex() if raw else '?'}")
            if raw and raw[0] == 0xE9:
                stub = base + rva + 5 + struct.unpack("<i", raw[1:5])[0]
                print(f"    jmp -> {stub:#010x}")
                break
        if not stub:
            print("\n[!] no Hook 41 jmp installed.")
            print("[!] Launch the game through RayaTrainer with its own")
            print("[!] 'Frame Rate Unlock 60fps' ON, then rerun.")
            return 3

        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        # Operands (and therefore branch targets) are only populated in detail mode.
        md.detail = True

        # --- full stub ---------------------------------------------------
        print(f"\n=== stub {stub:#010x} (full) ===")
        blob = t.read(stub, 0x120) or b""
        targets = {}
        for ins in md.disasm(blob, stub):
            print(f"  {ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}")
            if ins.mnemonic.startswith("j") and ins.operands:
                op = ins.operands[0]
                if op.type == 2:
                    targets[ins.address] = op.imm & 0xFFFFFFFF
            if ins.mnemonic in ("ret", "int3"):
                break
        inside = {a for a in targets.values() if stub <= a < stub + 0x120}
        print(f"\n  branch targets inside the stub: "
              f"{[hex(a) for a in sorted(inside)]}")

        # --- what the action codes lead to --------------------------------
        print("\n=== epilogues reached by each action code ===")
        for addr in sorted(inside):
            print(f"  --- {addr:#010x} ---")
            for ins in md.disasm(t.read(addr, 0x40) or b"", addr):
                print(f"      {ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}")
                if ins.mnemonic == "jmp" and ins.operands and \
                        ins.operands[0].type == 2:
                    break
                if ins.mnemonic in ("ret", "int3"):
                    break

        # --- behaviour ----------------------------------------------------
        print(f"\n=== behaviour with RayaTrainer's 60fps ON ({secs:.0f}s) ===")
        if A.read_sim_seconds(t, base) is None:
            print("  [!] not in a match")
            return 1
        off = LOG.stat().st_size if LOG.exists() else 0
        s0 = A.read_sim_seconds(t, base)
        t0 = time.perf_counter()
        time.sleep(secs)
        s1 = A.read_sim_seconds(t, base)
        real = time.perf_counter() - t0
        if s0 is None or s1 is None:
            print("  [!] left the match mid-sample")
            return 1
        speed = (s1 - s0) / real
        pr, es, n = frames_after(off)
        print(f"  speed {speed:.2f}x   Present "
              f"{'n/a' if pr is None else f'{pr:.1f}'} fps   "
              f"EndScene {'n/a' if es is None else f'{es:.1f}'}   "
              f"({n} samples)")
        print()
        if abs(speed - 1.0) < 0.06:
            print("  => RayaTrainer keeps normal speed at 60fps: its hook is the")
            print("     mechanism to reproduce.")
        else:
            print("  => RayaTrainer's own unlock also changes simulation speed.")
            print("     Replicating its hook would reproduce the same 2x, so the")
            print("     hook is not the missing piece.")

        # --- handler ------------------------------------------------------
        BM, TBL = 0x5FFEDE7C, 0x5FFEDDF8
        bm = t.read(BM, 0x31)
        if bm and stub:
            packed = bm[40]
            raw = t.read(TBL + packed * 4, 4)
            if raw:
                handler = struct.unpack("<I", raw)[0]
                print(f"\n=== hook 41 handler {handler:#010x} -> {OUT.name} ===")
                body = t.read(handler, 0x1400) or b""
                lines = []
                for ins in md.disasm(body, handler):
                    lines.append(f"{ins.address:#010x}  {ins.mnemonic:<7}"
                                 f"{ins.op_str}")
                    if ins.mnemonic in ("ret", "int3") and len(lines) > 200:
                        break
                OUT.write_text("\n".join(lines), encoding="utf-8")
                print(f"  wrote {len(lines)} instructions")
        return 0
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())

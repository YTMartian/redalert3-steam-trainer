"""Find the process that actually has the RA3 engine module mapped.

Process *names* are unreliable here: RA3.exe (the 9 MB launcher) sits at
0x400000 during startup, while the engine payload ``ra3_1.12.game`` also wants
0x400000 -- they cannot coexist in one address space. So the process running
the game may well be named something else (this install ships a ``runme.exe``
loader). Searching by name is therefore the wrong test.

This scans every process and reports the ones with a module whose name looks
like the RA3 engine, which is what the capture tools need.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys

import fps_patch_probe as p

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


def all_processes() -> list[tuple[int, str]]:
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)
    out = []
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    if kernel32.Process32FirstW(snap, ctypes.byref(e)):
        while True:
            out.append((e.th32ProcessID, e.szExeFile))
            if not kernel32.Process32NextW(snap, ctypes.byref(e)):
                break
    kernel32.CloseHandle(snap)
    return out


def is_engine_module(name: str) -> bool:
    low = name.lower()
    return ".game" in low and ("ra3" in low or "redalert" in low)


def find_game_processes(verbose: bool = False) -> list[dict]:
    """Every process holding an RA3 engine module, with its module list."""
    found, _denied = scan_game_processes(verbose)
    return found


def scan_game_processes(verbose: bool = False) -> tuple[list[dict], list[tuple[int, str]]]:
    """Find engine processes; also report RA3-ish ones we were not allowed to read.

    The denied list matters: the injector requests administrator rights, so the
    game can end up elevated and a normal-privilege Python sees nothing. That
    looks exactly like "the game is not running", which is a confusing dead end.
    """
    found = []
    denied = []
    pids = all_processes()
    if verbose:
        print(f"[i] scanning {len(pids)} processes for an RA3 engine module ...")
    for pid, name in pids:
        try:
            t = p.Target(pid)
        except OSError:
            low = name.lower()
            if any(k in low for k in ("ra3", "redalert", "runme", "raya")):
                denied.append((pid, name))
            continue
        try:
            mods = t.modules()
            engine = [m for m in mods if is_engine_module(m[2])]
            if engine:
                found.append({"pid": pid, "name": name, "modules": mods,
                              "engine": engine})
                if verbose:
                    for base, size, mname in engine:
                        print(f"[+] pid={pid} {name}: {mname} at "
                              f"{base:#010x} size={size:#09x}")
            elif verbose:
                pass
        finally:
            t.close()
    return found, denied


def explain_missing(denied: list[tuple[int, str]], verbose: bool = True) -> None:
    """Print the most likely reason nothing was found."""
    print("[!] No process has an RA3 engine module (ra3_*.game) mapped.")
    print()
    if denied:
        print("[!] But these RA3-ish processes ARE running yet could not be read:")
        for pid, name in denied:
            print(f"      pid={pid} {name}")
        print()
        print("[!] That is almost certainly a privilege problem -- the game is")
        print("[!] running elevated (the injector requests administrator).")
        print("[!] Re-run this from an Administrator PowerShell:")
        print("[!]   right-click PowerShell -> Run as administrator")
        return
    print("[!] The game is either not running, or still at the launcher stage.")
    print("[!] Steps: start a skirmish, get fully in-game, confirm RayaTrainer")
    print("[!] is attached, then run this again.")
    print("[!] Or pass --wait and leave this window open while you start it.")
    if verbose:
        print("[!] Quick check: python list_ra3_processes.py")


def main() -> int:
    pids = all_processes()
    print(f"total processes: {len(pids)}")

    print()
    print("processes holding an RA3 engine module:")
    found, denied = scan_game_processes(verbose=True)
    if not found:
        print("  (none)")
        print()
        explain_missing(denied)
        pids_all = pids
        kind = [f"{n}({p})" for p, n in pids_all
                if any(k in n.lower() for k in ("ra3", "redalert", "runme", "raya"))]
        print()
        print(f"RA3-ish process names currently running: {kind if kind else '(none)'}")
        return 1

    for h in found:
        print()
        print(f"pid={h['pid']} ({h['name']}) -- {len(h['modules'])} modules")
        big = [m for m in h["modules"] if m[1] >= 0x100000]
        for base, size, mname in sorted(big, key=lambda m: -m[1]):
            tag = "  <-- engine" if is_engine_module(mname) else ""
            print(f"   {base:#010x} size={size:#09x} {mname}{tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

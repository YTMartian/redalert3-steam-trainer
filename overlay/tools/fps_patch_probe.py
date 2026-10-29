"""Snapshot / diff the RA3 1.12 frame-rate patchset from the running game.

Why this exists
---------------
RayaTrainer's "Frame Rate Unlock 60fps" installs a fixed patchset instead of
poking values at runtime.  Its Steam 1.12 variant is what needs porting.  The
patch list was partially recovered from the agent's builder, but the new bytes
for the code sites could not be pinned down from decompiler output, and a wrong
byte crashes the game.

So measure instead of guess.  This tool snapshots the whole ``.text`` section
plus the interesting ``.data`` constants, so a diff between "toggle OFF" and
"toggle ON" reveals the exact patchset -- including any site not anticipated.

Usage
-----
    python fps_patch_probe.py --label before    # 60fps toggle OFF
    python fps_patch_probe.py --label after     # 60fps toggle ON
    python fps_patch_probe.py --diff before after

Notes
-----
* The module base is resolved from the real module list (``ra3_1.12.game``) and
  validated against the runtime logicFPS/updateFPS anchors.  A hardcoded
  0x400000 produced garbage on an earlier attempt because the module is
  relocated, so a bogus base is a hard error here.
* ``.text`` has file offset == RVA on this build, so snapshots are directly
  comparable with the on-disk file too.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import sys

PROCESS_NAME = "RA3.exe"
MODULE_NAME = "ra3_1.12.game"

# Anchor: [base+0x8B4098] = logicFPS (15 normally), [base+0x8B409C] = updateFPS
# (30).  Used purely to prove the resolved base is the right module.
ANCHOR_LOGIC_RVA = 0x8B4098
ANCHOR_UPDATE_RVA = 0x8B409C
PLAUSIBLE_FPS = {15, 30, 45, 60, 75, 90, 120, 144, 240}

TEXT_RVA = 0x1000
TEXT_SIZE = 0x7C6000  # .text virtual size on ra3_1.12.game

# Data constants the 60fps patch changes, with the pairs recovered statically
# from the agent's builder.  Listed so a run can be self-checked: a snapshot
# that matches neither column means we are looking at the wrong memory.
EXPECTED_DATA = {
    0x8B4098: (0x0000000F, 0x0000001E),  # logicFPS 15 -> 30
    0x8E0DEC: (0x3C75C290, 0x3CF5C290),  # 0.015   -> 0.03
    0x8E0DBC: (0x42855555, 0x42055555),  # 66.667  -> 33.333
    0x8E0DF8: (0x41700000, 0x41F00000),  # 15.0    -> 30.0
    0x8E0DF0: (0x41F00000, 0x42700000),  # 30.0    -> 60.0
    0x8E0DF4: (0x42055555, 0x41855555),  # 33.333  -> 16.667
    0x8E0E34: (0x3D888889, 0x3D088889),  # 0.0667  -> 0.0333
    0x8E0ED4: (0x3D088889, 0x3C888889),  # 0.0333  -> 0.01667
}

OTHER_DATA = [0x8E690C, 0x8EABEC, 0x7C6420, 0x7B40A8, 0x7B40D8, 0x7B4108,
              0x7B4C29, 0xBC6448, 0xBC644C, 0xCE0DFC]

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
LIST_MODULES_ALL = 0x03
TH32CS_SNAPPROCESS = 0x00000002
MEM_COMMIT = 0x1000

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.CloseHandle.restype = wt.BOOL
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.ReadProcessMemory.restype = wt.BOOL
kernel32.ReadProcessMemory.argtypes = [
    wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
kernel32.VirtualQueryEx.restype = ctypes.c_size_t
kernel32.VirtualQueryEx.argtypes = [
    wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t
]

psapi.EnumProcessModulesEx.restype = wt.BOOL
psapi.EnumProcessModulesEx.argtypes = [
    wt.HANDLE, ctypes.POINTER(wt.HMODULE), wt.DWORD,
    ctypes.POINTER(wt.DWORD), wt.DWORD,
]
psapi.GetModuleBaseNameW.restype = wt.DWORD
psapi.GetModuleBaseNameW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", wt.LPVOID), ("SizeOfImage", wt.DWORD),
                ("EntryPoint", wt.LPVOID)]


psapi.GetModuleInformation.restype = wt.BOOL
psapi.GetModuleInformation.argtypes = [
    wt.HANDLE, wt.HMODULE, ctypes.POINTER(MODULEINFO), wt.DWORD
]


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", wt.LPVOID), ("AllocationBase", wt.LPVOID),
        ("AllocationProtect", wt.DWORD), ("RegionSize", ctypes.c_size_t),
        ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD),
    ]


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


def here(name: str) -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), name)


def find_pids(name: str) -> list[int]:
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == wt.HANDLE(-1).value:
        return []
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    pids = []
    if kernel32.Process32FirstW(snap, ctypes.byref(entry)):
        while True:
            if entry.szExeFile.lower() == name.lower():
                pids.append(entry.th32ProcessID)
            if not kernel32.Process32NextW(snap, ctypes.byref(entry)):
                break
    kernel32.CloseHandle(snap)
    return pids


class Target:
    def __init__(self, pid: int):
        self.handle = kernel32.OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid
        )
        if not self.handle:
            raise OSError(ctypes.get_last_error(), f"OpenProcess({pid}) failed")
        self.pid = pid

    def read(self, address: int, size: int) -> bytes | None:
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t(0)
        ok = kernel32.ReadProcessMemory(
            self.handle, ctypes.c_void_p(address), ctypes.cast(buf, wt.LPVOID),
            size, ctypes.byref(got),
        )
        if not ok or got.value != size:
            return None
        return buf.raw

    def read_committed(self, address: int, size: int, chunk: int = 0x10000) -> bytes:
        """Read a range, zero-filling anything not committed/readable."""
        out = bytearray()
        offset = 0
        while offset < size:
            n = min(chunk, size - offset)
            data = self.read(address + offset, n)
            if data is None:
                out.extend(b"\x00" * n)
            else:
                out.extend(data)
            offset += n
        return bytes(out)

    def modules(self) -> list[tuple[int, int, str]]:
        needed = wt.DWORD(0)
        psapi.EnumProcessModulesEx(
            self.handle, None, 0, ctypes.byref(needed), LIST_MODULES_ALL
        )
        if needed.value == 0:
            return []
        # 取一次容量后进程里可能又加载了模块，第二次的 got 会超过第一次的
        # 估算值；多留一截并把越界的部分截掉，避免 IndexError。
        count = needed.value // ctypes.sizeof(wt.HMODULE) + 64
        arr = (wt.HMODULE * count)()
        got = wt.DWORD(0)
        if not psapi.EnumProcessModulesEx(
            self.handle, arr, ctypes.sizeof(arr), ctypes.byref(got), LIST_MODULES_ALL
        ):
            return []
        n = min(got.value // ctypes.sizeof(wt.HMODULE), count)
        out = []
        for i in range(n):
            name = ctypes.create_unicode_buffer(320)
            psapi.GetModuleBaseNameW(self.handle, arr[i], name, 320)
            mi = MODULEINFO()
            psapi.GetModuleInformation(
                self.handle, arr[i], ctypes.byref(mi), ctypes.sizeof(mi)
            )
            base = ctypes.cast(mi.lpBaseOfDll, ctypes.c_void_p).value or 0
            out.append((base, mi.SizeOfImage, name.value))
        return out

    def close(self) -> None:
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = None


def resolve_base(target: Target, modules) -> tuple[int, list[str]]:
    notes = []
    named = [(b, n) for b, _s, n in modules if n.lower() == MODULE_NAME.lower()]
    if not named:
        notes.append(f"!! {MODULE_NAME!r} not among {len(modules)} modules")
        return 0, notes

    best = 0
    for base, name in named:
        logic = target.read(base + ANCHOR_LOGIC_RVA, 4)
        update = target.read(base + ANCHOR_UPDATE_RVA, 4)
        if not logic or not update:
            notes.append(f"base {base:#x} ({name}): anchors unreadable")
            continue
        lv = struct.unpack("<I", logic)[0]
        uv = struct.unpack("<I", update)[0]
        ok = lv in PLAUSIBLE_FPS and uv in PLAUSIBLE_FPS
        notes.append(
            f"base {base:#x} ({name}) logicFPS={lv} updateFPS={uv} "
            + ("validated" if ok else "implausible")
        )
        if ok and not best:
            best = base
    return best, notes


def find_game_file() -> str | None:
    env = os.environ.get("RA3_GAME_FILE")
    cands = [env] if env else []
    for drive in "CDEFGHIJKLMNOPQRSTUVWXYZ":
        for prefix in (
            f"{drive}:\\steam client\\steamapps\\common\\",
            f"{drive}:\\Program Files (x86)\\Steam\\steamapps\\common\\",
            f"{drive}:\\SteamLibrary\\steamapps\\common\\",
        ):
            cands.append(
                prefix + "Command and Conquer Red Alert 3\\Data\\ra3_1.12.game"
            )
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def do_snapshot(label: str, pid_override: int | None) -> int:
    pids = find_pids(PROCESS_NAME)
    if not pids:
        print(f"[!] {PROCESS_NAME} is not running.")
        return 2
    if len(pids) > 1:
        print(f"[i] multiple {PROCESS_NAME}: {pids}")

    # The launcher and the real game can both be named RA3.exe.  Pick whichever
    # process actually has ra3_1.12.game loaded and validate it.
    candidates = [pid_override] if pid_override else pids
    base = 0
    target = None
    for pid in candidates:
        t = Target(pid)
        found, notes = resolve_base(t, t.modules())
        print(f"[i] pid={pid}")
        for n in notes:
            print(f"#  {n}")
        if found:
            base, target = found, t
            break
        t.close()
        print("#  -> no ra3_1.12.game here, skipping")

    if not base or target is None:
        print()
        print("[!] Could not find a process with ra3_1.12.game loaded.")
        print("[!] The RA3.exe you see is probably still the launcher. Start a")
        print("[!] skirmish (enter the match, not just the menu) and run again.")
        return 3

    print(f"[i] module base = {base:#010x}")
    t = target
    try:
        text = t.read_committed(base + TEXT_RVA, TEXT_SIZE)
        with open(here(f"fps_probe_{label}.text.bin"), "wb") as f:
            f.write(text)

        data = {}
        for rva in list(EXPECTED_DATA) + OTHER_DATA:
            raw = t.read(base + rva, 4)
            if raw is not None:
                data[f"{rva:08X}"] = raw.hex()

        # Self-check against the statically recovered pairs.
        checks = []
        for rva, (orig, patched) in EXPECTED_DATA.items():
            raw = t.read(base + rva, 4)
            if raw is None:
                checks.append((rva, "unreadable", None))
                continue
            v = struct.unpack("<I", raw)[0]
            if v == orig:
                state = "ORIGINAL"
            elif v == patched:
                state = "PATCHED"
            else:
                state = "UNEXPECTED"
            checks.append((rva, state, v))

        logic = t.read(base + ANCHOR_LOGIC_RVA, 4)
        update = t.read(base + ANCHOR_UPDATE_RVA, 4)
        info = {
            "label": label,
            "pid": pid,
            "base": base,
            "logic_fps": struct.unpack("<I", logic)[0] if logic else None,
            "update_fps": struct.unpack("<I", update)[0] if update else None,
            "data": data,
        }
        with open(here(f"fps_probe_{label}.json"), "w", encoding="utf-8") as f:
            json.dump(info, f, indent=2)

        print()
        print(f"[i] logicFPS={info['logic_fps']}  updateFPS={info['update_fps']}")
        print("[i] static-pair self-check:")
        for rva, state, v in checks:
            val = "-" if v is None else f"{v:#010x}"
            print(f"    {rva:08X}  {state:10s} {val}")
        states = {s for _r, s, _v in checks}
        if states == {"PATCHED"}:
            print("[+] all 8 data constants are in PATCHED state (60fps ON)")
        elif states == {"ORIGINAL"}:
            print("[+] all 8 data constants are ORIGINAL (60fps OFF)")
        else:
            print("[~] mixed/unknown state -- capture again for a clean pair")
        print()
        print(f"[i] wrote fps_probe_{label}.text.bin ({len(text)} bytes)")
        return 0
    finally:
        t.close()


def do_diff(a_label: str, b_label: str) -> int:
    def load(label):
        binary = here(f"fps_probe_{label}.text.bin")
        j = here(f"fps_probe_{label}.json")
        if not (os.path.isfile(binary) and os.path.isfile(j)):
            raise SystemExit(f"missing snapshot for label {label!r}")
        with open(binary, "rb") as f:
            text = f.read()
        with open(j, encoding="utf-8") as f:
            meta = json.load(f)
        return text, meta

    a_text, a_meta = load(a_label)
    b_text, b_meta = load(b_label)
    print(f"# {a_label}: base={a_meta['base']:#x} logicFPS={a_meta['logic_fps']} "
          f"updateFPS={a_meta['update_fps']}")
    print(f"# {b_label}: base={b_meta['base']:#x} logicFPS={b_meta['logic_fps']} "
          f"updateFPS={b_meta['update_fps']}")
    if a_meta["base"] != b_meta["base"]:
        print("[!] different module bases; run-to-run comparison is invalid")
        return 4

    if len(a_text) != len(b_text):
        print("[!] snapshot sizes differ")
        return 4

    diffs = []
    i = 0
    n = len(a_text)
    while i < n:
        if a_text[i] != b_text[i]:
            start = i
            while i < n and a_text[i] != b_text[i]:
                i += 1
            diffs.append((start, a_text[start:i], b_text[start:i]))
        else:
            i += 1

    print()
    print(f"## .text differences: {len(diffs)} run(s)")
    for start, before, after in diffs:
        rva = TEXT_RVA + start
        print(f"\nRVA {rva:08X}  len={len(before)}")
        print("  off   " + " ".join(f"{k:02d}" for k in range(min(len(before), 16))))
        print("  before" + " " + " ".join(f"{b:02X}" for b in before[:16]))
        print("  after " + " " + " ".join(f"{b:02X}" for b in after[:16]))
        if len(before) > 16:
            print(f"  ...  ({len(before) - 16} more bytes)")

    print()
    print("## .data differences")
    for k in sorted(set(a_meta["data"]) | set(b_meta["data"])):
        av = a_meta["data"].get(k)
        bv = b_meta["data"].get(k)
        if av != bv:
            print(f"RVA {k}  {av} -> {bv}")

    out = here(f"fps_patch_{a_label}_{b_label}.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(
            {
                "from": a_label,
                "to": b_label,
                "base": a_meta["base"],
                "text_patches": [
                    {
                        "rva": f"{TEXT_RVA + s:08X}",
                        "length": len(bf),
                        "before": bf.hex(),
                        "after": af.hex(),
                    }
                    for s, bf, af in diffs
                ],
                "data": {
                    k: {"from": a_meta["data"].get(k), "to": b_meta["data"].get(k)}
                    for k in sorted(set(a_meta["data"]) | set(b_meta["data"]))
                    if a_meta["data"].get(k) != b_meta["data"].get(k)
                },
            },
            f,
            indent=2,
        )
    print(f"\n[i] wrote {out}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--label", help="snapshot name, e.g. before / after")
    ap.add_argument("--diff", nargs=2, metavar=("A", "B"), help="compare two snapshots")
    ap.add_argument("--pid", type=int, default=None)
    ap.add_argument("--process", default=PROCESS_NAME)
    args = ap.parse_args()

    if args.diff:
        return do_diff(args.diff[0], args.diff[1])
    return do_snapshot(args.label or "dump", args.pid)


if __name__ == "__main__":
    sys.exit(main())

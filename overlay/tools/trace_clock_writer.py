"""Catch the instruction that advances the battle clock, with a hardware write watchpoint.

Static searching failed twice: no `inc [reg+0x50]` exists in the image, and the six
`mov [reg+0x50], r32` sites found all *copy* the clock out into entity fields. The
real writer reaches the logic object through a member pointer rather than the
0x8DDE84 global, so it cannot be found by looking for the global near the store.

A hardware watchpoint sidesteps the whole question: set a 4-byte write breakpoint
on logic+0x50 and let the CPU report the exact EIP on the first write. No code is
patched, and the breakpoint is cleared before the process is released, so the game
runs unmodified throughout.

How the attach stays safe:

  * DebugActiveProcess makes us the debugger, so the single-step exception caused
    by the watchpoint comes to us instead of faulting the game.
  * Every other exception is forwarded with DBG_EXCEPTION_NOT_HANDLED, so the
    game's own first-chance handling (which is constant in this engine) is intact.
  * On the hit, Dr7 is cleared in that thread's context and the event is consumed
    with DBG_CONTINUE. The instruction has not executed yet, so resuming at EIP
    re-executes it untouched.
  * Detach happens in a finally block, including on timeout or Ctrl-C.

Usage:
    python trace_clock_writer.py [timeout-seconds]
"""
from __future__ import annotations

import ctypes
import struct
import sys
import time
from ctypes import wintypes as w

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
advapi = ctypes.WinDLL("advapi32", use_last_error=True)

THREAD_GET_CONTEXT = 0x0008
THREAD_SET_CONTEXT = 0x0010
THREAD_SUSPEND_RESUME = 0x0002
THREAD_QUERY_INFORMATION = 0x0040

CONTEXT_i386 = 0x00010000
CONTEXT_CONTROL = CONTEXT_i386 | 0x00000001
CONTEXT_INTEGER = CONTEXT_i386 | 0x00000002
CONTEXT_SEGMENTS = CONTEXT_i386 | 0x00000004
CONTEXT_FLOATING_POINT = CONTEXT_i386 | 0x00000008
CONTEXT_DEBUG_REGISTERS = CONTEXT_i386 | 0x00000010
CONTEXT_FULL = (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS
                | CONTEXT_FLOATING_POINT)

DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
EXCEPTION_DEBUG_EVENT = 1
CREATE_THREAD_DEBUG_EVENT = 2
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_THREAD_DEBUG_EVENT = 4
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
UNLOAD_DLL_DEBUG_EVENT = 7
OUTPUT_DEBUG_STRING_EVENT = 8
RIP_EVENT = 9
EXCEPTION_SINGLE_STEP = 0x80000004

BATTLE_LOGIC_GLOBAL = 0x8DDE84
CLOCK_OFFSET = 0x50


class FLOATING_SAVE_AREA(ctypes.Structure):
    _fields_ = [("ControlWord", w.DWORD), ("StatusWord", w.DWORD),
                ("TagWord", w.DWORD), ("ErrorOffset", w.DWORD),
                ("ErrorSelector", w.DWORD), ("DataOffset", w.DWORD),
                ("DataSelector", w.DWORD),
                ("RegisterArea", ctypes.c_byte * 80),
                ("Cr0NpxState", w.DWORD)]


class CONTEXT(ctypes.Structure):
    """i386 CONTEXT, layout must match the OS exactly: Dr0..Dr7 sit at the front."""
    _fields_ = [("ContextFlags", w.DWORD),
                ("Dr0", w.DWORD), ("Dr1", w.DWORD), ("Dr2", w.DWORD),
                ("Dr3", w.DWORD), ("Dr6", w.DWORD), ("Dr7", w.DWORD),
                ("FloatSave", FLOATING_SAVE_AREA),
                ("SegGs", w.DWORD), ("SegFs", w.DWORD),
                ("SegEs", w.DWORD), ("SegDs", w.DWORD),
                ("Edi", w.DWORD), ("Esi", w.DWORD), ("Ebx", w.DWORD),
                ("Edx", w.DWORD), ("Ecx", w.DWORD), ("Eax", w.DWORD),
                ("Ebp", w.DWORD), ("Eip", w.DWORD), ("SegCs", w.DWORD),
                ("EFlags", w.DWORD), ("Esp", w.DWORD), ("SegSs", w.DWORD),
                ("ExtendedRegisters", ctypes.c_byte * 512)]


class EXCEPTION_RECORD(ctypes.Structure):
    _fields_ = [("ExceptionCode", w.DWORD), ("ExceptionFlags", w.DWORD),
                ("ExceptionRecord", ctypes.c_void_p),
                ("ExceptionAddress", ctypes.c_void_p),
                ("NumberParameters", w.DWORD),
                ("ExceptionInformation", ctypes.c_size_t * 15)]


class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("ExceptionRecord", EXCEPTION_RECORD), ("dwFirstChance", w.DWORD)]


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [("dwDebugEventCode", w.DWORD),
                ("dwProcessId", w.DWORD),
                ("dwThreadId", w.DWORD),
                ("payload", ctypes.c_byte * 192)]


k32.DebugActiveProcess.argtypes = [w.DWORD]
k32.DebugActiveProcess.restype = w.BOOL
k32.DebugActiveProcessStop.argtypes = [w.DWORD]
k32.DebugActiveProcessStop.restype = w.BOOL
k32.WaitForDebugEvent.argtypes = [ctypes.POINTER(DEBUG_EVENT), w.DWORD]
k32.WaitForDebugEvent.restype = w.BOOL
k32.ContinueDebugEvent.argtypes = [w.DWORD, w.DWORD, w.DWORD]
k32.ContinueDebugEvent.restype = w.BOOL
k32.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.OpenThread.restype = w.HANDLE
k32.GetThreadContext.argtypes = [w.HANDLE, ctypes.POINTER(CONTEXT)]
k32.GetThreadContext.restype = w.BOOL
k32.SetThreadContext.argtypes = [w.HANDLE, ctypes.POINTER(CONTEXT)]
k32.SetThreadContext.restype = w.BOOL
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.OpenProcess.restype = w.HANDLE
k32.CloseHandle.argtypes = [w.HANDLE]
k32.CloseHandle.restype = w.BOOL
k32.SuspendThread.argtypes = [w.HANDLE]
k32.SuspendThread.restype = w.DWORD
k32.ResumeThread.argtypes = [w.HANDLE]
k32.ResumeThread.restype = w.DWORD
k32.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
k32.CreateToolhelp32Snapshot.restype = w.HANDLE
k32.Thread32First.argtypes = [w.HANDLE, ctypes.c_void_p]
k32.Thread32First.restype = w.BOOL
k32.Thread32Next.argtypes = [w.HANDLE, ctypes.c_void_p]
k32.Thread32Next.restype = w.BOOL
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = w.BOOL


def enable_debug_privilege() -> bool:
    """SeDebugPrivilege is required to attach to a process owned by another token."""
    TOKEN_ADJUST_PRIVILEGES, TOKEN_QUERY = 0x0020, 0x0008
    SE_PRIVILEGE_ENABLED = 0x00000002

    class LUID(ctypes.Structure):
        _fields_ = [("LowPart", w.DWORD), ("HighPart", w.LONG)]

    class LUID_AND_ATTRIBUTES(ctypes.Structure):
        _fields_ = [("Luid", LUID), ("Attributes", w.DWORD)]

    class TOKEN_PRIVILEGES(ctypes.Structure):
        _fields_ = [("PrivilegeCount", w.DWORD),
                    ("Privileges", LUID_AND_ATTRIBUTES * 1)]

    tok = w.HANDLE()
    if not advapi.OpenProcessToken(k32.GetCurrentProcess(),
                                   TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                                   ctypes.byref(tok)):
        return False
    try:
        luid = LUID()
        if not advapi.LookupPrivilegeValueW(None, "SeDebugPrivilege",
                                            ctypes.byref(luid)):
            return False
        tp = TOKEN_PRIVILEGES()
        tp.PrivilegeCount = 1
        tp.Privileges[0].Luid = luid
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED
        return bool(advapi.AdjustTokenPrivileges(tok, False, ctypes.byref(tp), 0,
                                                 None, None))
    finally:
        k32.CloseHandle(tok)


def set_watchpoint(thread_id: int, address: int, on: bool) -> bool:
    """Arm or clear a 4-byte write watchpoint in Dr0 on one thread.

    Dr7 encoding for slot 0: L0 = bit 0, RW0 = bits 16-17 (01 = write),
    LEN0 = bits 18-19 (11 = 4 bytes).

    The thread is suspended around the context write: SetThreadContext on a
    running thread can be dropped, because the thread may overwrite its context
    from its own saved state on the next kernel transition.

    Wow64Get/SetThreadContext are required here: the game is 32-bit on 64-bit
    Windows, and the plain context APIs then operate on the 64-bit context, which
    accepts the call and stores debug registers that the 32-bit thread never
    sees. That failure is silent -- the watchpoint simply never fires.
    """
    h = k32.OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT
                       | THREAD_SUSPEND_RESUME, False, thread_id)
    if not h:
        return False
    try:
        if k32.SuspendThread(h) == 0xFFFFFFFF:
            return False
        try:
            ctx = CONTEXT()
            ctx.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS
            if not get_ctx(h, ctypes.byref(ctx)):
                return False
            if on:
                ctx.Dr0 = address
                ctx.Dr6 = 0
                ctx.Dr7 = 0x00000001 | (0x1 << 16) | (0x3 << 18)
            else:
                ctx.Dr0 = 0
                ctx.Dr6 = 0
                ctx.Dr7 = 0
            ctx.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS
            return bool(set_ctx(h, ctypes.byref(ctx)))
        finally:
            k32.ResumeThread(h)
    finally:
        k32.CloseHandle(h)


def context_getter():
    """Prefer the Wow64 context APIs, which is what a 32-bit target needs."""
    for name in ("Wow64GetThreadContext", "GetThreadContext"):
        if hasattr(k32, name):
            return getattr(k32, name), name
    return k32.GetThreadContext, "GetThreadContext"


def context_setter():
    for name in ("Wow64SetThreadContext", "SetThreadContext"):
        if hasattr(k32, name):
            return getattr(k32, name), name
    return k32.SetThreadContext, "SetThreadContext"


get_ctx, GET_CTX_NAME = context_getter()
set_ctx, SET_CTX_NAME = context_setter()
for _fn in (get_ctx, set_ctx):
    _fn.argtypes = [w.HANDLE, ctypes.POINTER(CONTEXT)]
    _fn.restype = w.BOOL


def read_watchpoint(thread_id: int):
    """Read back Dr0/Dr7 so a silently ignored write is visible."""
    h = k32.OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, False,
                       thread_id)
    if not h:
        return None
    try:
        k32.SuspendThread(h)
        try:
            ctx = CONTEXT()
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS
            if not get_ctx(h, ctypes.byref(ctx)):
                return None
            return ctx.Dr0, ctx.Dr7
        finally:
            k32.ResumeThread(h)
    finally:
        k32.CloseHandle(h)


def thread_ids(pid: int):
    """Every thread of the process, including ones created before we attached."""
    TH32CS_SNAPTHREAD = 0x00000004

    class THREADENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD),
                    ("th32ThreadID", w.DWORD), ("th32OwnerProcessID", w.DWORD),
                    ("tpBasePri", w.LONG), ("tpDeltaPri", w.LONG),
                    ("dwFlags", w.DWORD)]

    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if snap == w.HANDLE(-1).value:
        return []
    out = []
    try:
        e = THREADENTRY32()
        e.dwSize = ctypes.sizeof(THREADENTRY32)
        ok = k32.Thread32First(snap, ctypes.byref(e))
        while ok:
            if e.th32OwnerProcessID == pid:
                out.append(e.th32ThreadID)
            ok = k32.Thread32Next(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    return out


def read_u32(hproc, address: int):
    buf = ctypes.c_uint32()
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(hproc, ctypes.c_void_p(address),
                               ctypes.byref(buf), 4, ctypes.byref(got))
    return buf.value if ok and got.value == 4 else None


def disasm_around(base: int, size: int, hproc, eip: int, back: int = 0x50):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    start = max(base, eip - back)
    data = ctypes.create_string_buffer(eip - start + 0x20)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(hproc, ctypes.c_void_p(start), data,
                                 len(data), ctypes.byref(got)):
        print("  (cannot read code around EIP)")
        return
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    for ins in md.disasm(bytes(data.raw[:got.value]), start):
        mark = "   <<< WRITES THE CLOCK" if ins.address == eip else ""
        print(f"  {ins.address:#010x}  {ins.mnemonic:<7}{ins.op_str}{mark}")
        if ins.address > eip + 0x10:
            break


def main() -> int:
    timeout = float(sys.argv[1]) if len(sys.argv) > 1 else 45.0

    if not enable_debug_privilege():
        print("[!] could not enable SeDebugPrivilege; attach may be refused")

    from list_ra3_processes import explain_missing, scan_game_processes
    found, denied = scan_game_processes()
    if not found:
        explain_missing(denied)
        return 2
    pid = found[0]["pid"]
    hproc = k32.OpenProcess(0x0410, False, pid)   # VM_READ | QUERY_INFORMATION
    if not hproc:
        print(f"[!] OpenProcess failed (err={ctypes.get_last_error()})")
        return 2

    try:
        # The base is resolved by the probe module so the address matches whatever
        # the loader actually mapped.
        import fps_patch_probe as p
        t = p.Target(pid)
        base, _notes = p.resolve_base(t, t.modules())
        logic = read_u32(hproc, base + BATTLE_LOGIC_GLOBAL)
        t.close()
        if not logic or not (0x10000 <= logic < 0x7FFFFFFF):
            print(f"[!] battle logic object not readable via "
                  f"{base + BATTLE_LOGIC_GLOBAL:#010x}")
            return 1
        clock = logic + CLOCK_OFFSET
        v0 = read_u32(hproc, clock)
        print(f"pid={pid}  logic={logic:#010x}  clock={clock:#010x} = {v0}")
        if v0 is None:
            print("[!] clock not readable; not in a match?")
            return 1

        if not k32.DebugActiveProcess(pid):
            print(f"[!] DebugActiveProcess failed "
                  f"(err={ctypes.get_last_error()}); is something already "
                  f"attached (RayaTrainer, x64dbg, Cheat Engine)?")
            return 1
        print("attached as debugger; arming the watchpoint on every thread")

        armed_threads = set()
        failed_threads = []
        hit = None
        deadline = time.time() + timeout
        ev = DEBUG_EVENT()
        last_status = time.time()
        v_start = v0

        def arm(tid):
            if tid in armed_threads:
                return
            if set_watchpoint(tid, clock, True):
                armed_threads.add(tid)
            else:
                failed_threads.append(tid)

        # Threads that existed before the attach each report through
        # CREATE_THREAD_DEBUG_EVENT, so arming happens there. Enumerating and
        # suspending every thread up front only adds a chance to destabilise the
        # process: the debug loop already suspends the reporting thread.
        print(f"waiting for thread events to arm the watchpoint "
              f"(context api: {GET_CTX_NAME} / {SET_CTX_NAME})\n")
        try:
            while time.time() < deadline:
                if not k32.WaitForDebugEvent(ctypes.byref(ev), 500):
                    err = ctypes.get_last_error()
                    if err == 121:        # ERROR_SEM_TIMEOUT, just idle
                        # Periodic status: proves the game is live and the
                        # watchpoint survived, rather than failing silently.
                        if time.time() - last_status > 5:
                            now = read_u32(hproc, clock)
                            delta = "n/a" if now is None else f"+{now - v_start}"
                            print(f"  armed={len(armed_threads)} "
                                  f"refused={len(failed_threads)} "
                                  f"clock={now} ({delta})")
                            if armed_threads and now is None:
                                print("  [!] clock unreadable: the match ended "
                                      "or the object was freed")
                            last_status = time.time()
                        continue
                    print(f"[!] WaitForDebugEvent failed (err={err})")
                    break
                code, tid = ev.dwDebugEventCode, ev.dwThreadId
                cont = DBG_CONTINUE

                if code == CREATE_PROCESS_DEBUG_EVENT:
                    # The payload starts with hFile; close it and keep debugging.
                    hfile = ctypes.cast(ev.payload,
                                        ctypes.POINTER(ctypes.c_void_p))[0]
                    if hfile:
                        k32.CloseHandle(ctypes.c_void_p(hfile))
                    arm(tid)
                elif code == CREATE_THREAD_DEBUG_EVENT:
                    arm(tid)
                elif code == EXCEPTION_DEBUG_EVENT:
                    info = EXCEPTION_DEBUG_INFO.from_buffer_copy(bytes(ev.payload))
                    rec = info.ExceptionRecord
                    if rec.ExceptionCode == EXCEPTION_SINGLE_STEP:
                        eip = rec.ExceptionAddress or 0
                        if set_watchpoint(tid, clock, False):
                            hit = (tid, eip)
                        k32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
                        break
                    # Anything else belongs to the game: do not swallow it.
                    cont = DBG_EXCEPTION_NOT_HANDLED

                k32.ContinueDebugEvent(pid, tid, cont)

            if hit:
                tid, eip = hit
                print(f"\nclock written by EIP {eip:#010x}  "
                      f"(module +{eip - base:#x}) on thread {tid}")
                print(f"armed threads: {len(armed_threads)}\n")
                disasm_around(base, 0, hproc, eip)
                now = read_u32(hproc, clock)
                print(f"\nclock now {now} (was {v0})")
            else:
                now = read_u32(hproc, clock)
                print(f"\n[!] no write to the clock within {timeout:.0f}s")
                print(f"    clock {v0} -> {now}  (advanced: {now != v0})")
                print(f"    armed {len(armed_threads)} thread(s), "
                      f"{len(failed_threads)} refused "
                      f"{failed_threads[:4]}")
                print("    if the clock advanced, the watchpoint never fired: "
                      "the write is not a store to this dword, or it happens on "
                      "a thread that refuses context access")
        finally:
            for tid in list(armed_threads):
                set_watchpoint(tid, clock, False)
            k32.DebugActiveProcessStop(pid)
            print("detached; the game keeps running")
        return 0
    finally:
        k32.CloseHandle(hproc)


if __name__ == "__main__":
    sys.exit(main())

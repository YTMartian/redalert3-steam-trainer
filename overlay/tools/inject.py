# -*- coding: utf-8 -*-
"""Inject overlay/bin/ra3_overlay.dll into ra3_1.12.game via LoadLibraryW.

Requires Administrator. Prefer ASCII path for the DLL (this repo path is fine).
Do not run the Python MustCode trainer hooks at the same time during feasibility tests.

Works from 64-bit Python: resolves LoadLibraryW inside the 32-bit target
(SysWOW64 kernel32 export RVA + remote module base), instead of using the
injector's 64-bit kernel32 address.
"""
from __future__ import print_function

import ctypes
import os
import struct
import sys
from ctypes import wintypes

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04
TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

# Pointer-sized types — required on 64-bit Python (default HANDLE/HMODULE can overflow).
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = ctypes.c_void_p
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.VirtualAllocEx.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD
]
kernel32.VirtualAllocEx.restype = ctypes.c_void_p
kernel32.VirtualFreeEx.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD
]
kernel32.VirtualFreeEx.restype = wintypes.BOOL
kernel32.WriteProcessMemory.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t)
]
kernel32.WriteProcessMemory.restype = wintypes.BOOL
kernel32.ReadProcessMemory.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t)
]
kernel32.ReadProcessMemory.restype = wintypes.BOOL
kernel32.CreateRemoteThread.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
    ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p
]
kernel32.CreateRemoteThread.restype = ctypes.c_void_p
kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.GetExitCodeThread.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)]
kernel32.GetExitCodeThread.restype = wintypes.BOOL
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
kernel32.Process32FirstW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel32.Process32FirstW.restype = wintypes.BOOL
kernel32.Process32NextW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel32.Process32NextW.restype = wintypes.BOOL
kernel32.Module32FirstW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel32.Module32FirstW.restype = wintypes.BOOL
kernel32.Module32NextW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel32.Module32NextW.restype = wintypes.BOOL


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ('dwSize', wintypes.DWORD),
        ('cntUsage', wintypes.DWORD),
        ('th32ProcessID', wintypes.DWORD),
        ('th32DefaultHeapID', ctypes.POINTER(ctypes.c_ulong)),
        ('th32ModuleID', wintypes.DWORD),
        ('cntThreads', wintypes.DWORD),
        ('th32ParentProcessID', wintypes.DWORD),
        ('pcPriClassBase', ctypes.c_long),
        ('dwFlags', wintypes.DWORD),
        ('szExeFile', wintypes.WCHAR * 260),
    ]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ('dwSize', wintypes.DWORD),
        ('th32ModuleID', wintypes.DWORD),
        ('th32ProcessID', wintypes.DWORD),
        ('GlblcntUsage', wintypes.DWORD),
        ('ProccntUsage', wintypes.DWORD),
        ('modBaseAddr', ctypes.c_void_p),
        ('modBaseSize', wintypes.DWORD),
        ('hModule', ctypes.c_void_p),
        ('szModule', wintypes.WCHAR * 256),
        ('szExePath', wintypes.WCHAR * 260),
    ]


def find_pid(exe_name='ra3_1.12.game'):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == INVALID_HANDLE_VALUE:
        return None
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    pid = None
    if kernel32.Process32FirstW(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == exe_name.lower():
                pid = pe.th32ProcessID
                break
            if not kernel32.Process32NextW(snap, ctypes.byref(pe)):
                break
    kernel32.CloseHandle(snap)
    return pid


def find_remote_module(pid, module_name):
    """Return (base, path) for a module inside the target, or (None, None)."""
    flags = TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32
    snap = kernel32.CreateToolhelp32Snapshot(flags, pid)
    if not snap or snap == INVALID_HANDLE_VALUE:
        return None, None
    me = MODULEENTRY32W()
    me.dwSize = ctypes.sizeof(MODULEENTRY32W)
    base = None
    path = None
    want = module_name.lower()
    if kernel32.Module32FirstW(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == want or me.szExePath.lower().endswith('\\' + want):
                base = me.modBaseAddr
                path = me.szExePath
                break
            if not kernel32.Module32NextW(snap, ctypes.byref(me)):
                break
    kernel32.CloseHandle(snap)
    return base, path


def find_remote_module_base(pid, module_name='kernel32.dll'):
    base, _ = find_remote_module(pid, module_name)
    return base


def _pe_export_rva(pe_path, export_name):
    """Return RVA of export_name from a PE file on disk (e.g. SysWOW64\\kernel32.dll)."""
    with open(pe_path, 'rb') as f:
        data = f.read()
    if data[:2] != b'MZ':
        raise ValueError('not PE: %s' % pe_path)
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b'PE\0\0':
        raise ValueError('bad PE signature: %s' % pe_path)
    # Optional header magic: PE32=0x10B, PE32+=0x20B
    magic = struct.unpack_from('<H', data, e_lfanew + 0x18)[0]
    if magic == 0x10B:
        export_dir_rva = struct.unpack_from('<I', data, e_lfanew + 0x78)[0]
    elif magic == 0x20B:
        export_dir_rva = struct.unpack_from('<I', data, e_lfanew + 0x88)[0]
    else:
        raise ValueError('unknown optional header magic 0x%X' % magic)
    if not export_dir_rva:
        raise ValueError('no export directory')

    def rva_to_off(rva):
        # Section headers start after optional header
        num_sections = struct.unpack_from('<H', data, e_lfanew + 6)[0]
        size_opt = struct.unpack_from('<H', data, e_lfanew + 0x14)[0]
        sec = e_lfanew + 0x18 + size_opt
        for i in range(num_sections):
            off = sec + i * 40
            virt_size = struct.unpack_from('<I', data, off + 8)[0]
            virt_addr = struct.unpack_from('<I', data, off + 12)[0]
            raw_size = struct.unpack_from('<I', data, off + 16)[0]
            raw_ptr = struct.unpack_from('<I', data, off + 20)[0]
            size = max(virt_size, raw_size)
            if virt_addr <= rva < virt_addr + size:
                return raw_ptr + (rva - virt_addr)
        raise ValueError('RVA 0x%X not in any section' % rva)

    exp_off = rva_to_off(export_dir_rva)
    num_names = struct.unpack_from('<I', data, exp_off + 0x18)[0]
    addr_of_funcs = struct.unpack_from('<I', data, exp_off + 0x1C)[0]
    addr_of_names = struct.unpack_from('<I', data, exp_off + 0x20)[0]
    addr_of_ords = struct.unpack_from('<I', data, exp_off + 0x24)[0]
    names_off = rva_to_off(addr_of_names)
    ords_off = rva_to_off(addr_of_ords)
    funcs_off = rva_to_off(addr_of_funcs)
    want = export_name if isinstance(export_name, bytes) else export_name.encode('ascii')
    for i in range(num_names):
        name_rva = struct.unpack_from('<I', data, names_off + i * 4)[0]
        name_off = rva_to_off(name_rva)
        end = data.index(b'\0', name_off)
        if data[name_off:end] == want:
            ordinal = struct.unpack_from('<H', data, ords_off + i * 2)[0]
            func_rva = struct.unpack_from('<I', data, funcs_off + ordinal * 4)[0]
            return func_rva
    raise ValueError('export not found: %s' % export_name)


def resolve_remote_export(pid, export_name):
    """Address of an export inside the target process's kernel32."""
    k32_base = find_remote_module_base(pid, 'kernel32.dll')
    if not k32_base:
        return None, 'kernel32.dll not found in target (is the process running?)'

    candidates = []
    windir = os.environ.get('SystemRoot', r'C:\Windows')
    if sys.maxsize > 2 ** 32:
        candidates.append(os.path.join(windir, 'SysWOW64', 'kernel32.dll'))
    candidates.append(os.path.join(windir, 'System32', 'kernel32.dll'))

    last_err = None
    name = export_name if isinstance(export_name, bytes) else export_name.encode('ascii')
    for pe_path in candidates:
        if not os.path.isfile(pe_path):
            continue
        try:
            rva = _pe_export_rva(pe_path, name)
            return int(k32_base) + int(rva), None
        except Exception as exc:
            last_err = str(exc)
            continue
    return None, 'failed to resolve %s RVA (%s)' % (export_name, last_err)


def resolve_remote_loadlibrary_w(pid):
    return resolve_remote_export(pid, b'LoadLibraryW')


def unload_module_by_name(hproc, pid, module_name, max_frees=8):
    """Repeated FreeLibrary until the module disappears (handles refcount > 1)."""
    free_lib, err = resolve_remote_export(pid, b'FreeLibrary')
    if not free_lib:
        return False, 'FreeLibrary unresolved: %s' % err
    notes = []
    for i in range(max_frees):
        base, _ = find_remote_module(pid, module_name)
        if not base:
            return True, '; '.join(notes) if notes else 'not loaded'
        thread = kernel32.CreateRemoteThread(
            hproc, None, 0, ctypes.c_void_p(free_lib), ctypes.c_void_p(int(base)),
            0, None
        )
        if not thread:
            return False, 'FreeLibrary CreateRemoteThread failed err=%d' % ctypes.get_last_error()
        kernel32.WaitForSingleObject(thread, 10000)
        kernel32.CloseHandle(thread)
        notes.append('FreeLibrary#%d on 0x%X' % (i + 1, int(base)))
        import time
        time.sleep(0.2)
    still, _ = find_remote_module(pid, module_name)
    if still:
        return False, '%s; still present @0x%X — restart the game' % (
            '; '.join(notes), int(still))
    return True, '; '.join(notes)


def inject(dll_path, pid=None):
    dll_path = os.path.abspath(dll_path)
    if not os.path.isfile(dll_path):
        return False, u'找不到覆盖层 DLL：\n%s' % dll_path
    if not dll_path.lower().endswith('.dll'):
        return False, u'目标不是 DLL：\n%s' % dll_path

    dll_path = os.path.normpath(dll_path)
    if pid is None:
        pid = find_pid()
    if not pid:
        return False, u'没有找到游戏进程。\n请先启动红警 3，再运行注入器。'

    hproc = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not hproc:
        return False, u'无法打开游戏进程（错误 %d）。\n请用管理员身份运行注入器。' % ctypes.get_last_error()

    # Drop any previous overlay builds so DllMain runs again on LoadLibrary.
    unload_notes = []
    for name in ('ra3_overlay_v4.dll', 'ra3_overlay_v3.dll', 'ra3_overlay_v2.dll',
                 'ra3_overlay_fix.dll', 'ra3_overlay_feat.dll', 'ra3_overlay_mh.dll',
                 'ra3_overlay_new.dll', 'ra3_overlay.dll'):
        ok_u, msg_u = unload_module_by_name(hproc, pid, name)
        unload_notes.append('%s: %s' % (name, msg_u))
        if not ok_u and 'still present' in msg_u:
            kernel32.CloseHandle(hproc)
            return False, (
                u'旧的覆盖层还在游戏里，卸不掉（%s）。\n'
                u'请先完全退出红警 3，再重新进游戏后注入。'
            ) % name

    load_lib, err = resolve_remote_loadlibrary_w(pid)
    if not load_lib:
        kernel32.CloseHandle(hproc)
        return False, err or u'无法在游戏进程里找到 LoadLibraryW。'

    path_bytes = (dll_path + '\0').encode('utf-16-le')
    remote = kernel32.VirtualAllocEx(
        hproc, None, len(path_bytes), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    )
    if not remote:
        kernel32.CloseHandle(hproc)
        return False, u'无法在游戏里分配内存（错误 %d）。' % ctypes.get_last_error()

    written = ctypes.c_size_t(0)
    buf = ctypes.create_string_buffer(path_bytes)
    if not kernel32.WriteProcessMemory(
        hproc, remote, buf, len(path_bytes), ctypes.byref(written)
    ):
        kernel32.VirtualFreeEx(hproc, remote, 0, MEM_RELEASE)
        kernel32.CloseHandle(hproc)
        return False, u'无法把 DLL 路径写入游戏（错误 %d）。' % ctypes.get_last_error()

    thread = kernel32.CreateRemoteThread(
        hproc, None, 0, ctypes.c_void_p(load_lib), remote, 0, None
    )
    if not thread:
        err_code = ctypes.get_last_error()
        kernel32.VirtualFreeEx(hproc, remote, 0, MEM_RELEASE)
        kernel32.CloseHandle(hproc)
        return False, u'无法在游戏里启动加载线程（错误 %d）。' % err_code

    kernel32.WaitForSingleObject(thread, 15000)
    exit_code = wintypes.DWORD(0)
    kernel32.GetExitCodeThread(thread, ctypes.byref(exit_code))
    kernel32.CloseHandle(thread)
    kernel32.VirtualFreeEx(hproc, remote, 0, MEM_RELEASE)
    kernel32.CloseHandle(hproc)

    if exit_code.value == 0:
        return False, (
            u'DLL 没有载入成功。\n'
            u'请确认 ra3_overlay_v4.dll 和注入器在同一目录，且没有被安全软件隔离。\n'
            u'日志：%TEMP%\\ra3_overlay.log'
        )
    return True, (
        u'覆盖层已载入。\n'
        u'进程 PID：%d\n'
        u'进局后按 Home 打开菜单。'
    ) % pid


def _msg(ok, detail):
    title = u'注入成功' if ok else u'注入失败'
    text = (u'注入成功。\n\n' if ok else u'注入失败。\n\n') + (detail or u'')
    try:
        user32 = ctypes.WinDLL('user32', use_last_error=True)
        user32.MessageBoxW.argtypes = [
            ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint
        ]
        user32.MessageBoxW.restype = ctypes.c_int
        flags = 0x0 | (0x40 if ok else 0x10) | 0x40000 | 0x10000
        user32.MessageBoxW(None, text, title, flags)
    except Exception:
        print(title)
        print(text)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    # When frozen by PyInstaller, look next to the exe.
    if getattr(sys, 'frozen', False):
        here = os.path.dirname(sys.executable)
    candidates = [
        os.path.normpath(os.path.join(here, 'ra3_overlay_v4.dll')),
        os.path.normpath(os.path.join(here, 'ra3_overlay.dll')),
        os.path.normpath(os.path.join(here, '..', 'bin', 'ra3_overlay_v4.dll')),
        os.path.normpath(os.path.join(here, '..', 'bin', 'ra3_overlay_v3.dll')),
        os.path.normpath(os.path.join(here, '..', 'bin', 'ra3_overlay_v2.dll')),
        os.path.normpath(os.path.join(here, '..', 'bin', 'ra3_overlay_fix.dll')),
        os.path.normpath(os.path.join(here, '..', 'bin', 'ra3_overlay_feat.dll')),
        os.path.normpath(os.path.join(here, '..', 'bin', 'ra3_overlay_mh.dll')),
        os.path.normpath(os.path.join(here, '..', 'bin', 'ra3_overlay_new.dll')),
        os.path.normpath(os.path.join(here, '..', 'bin', 'ra3_overlay.dll')),
    ]
    default_dll = None
    for c in candidates:
        if os.path.isfile(c):
            default_dll = c
            break
    if default_dll is None:
        default_dll = candidates[0]
    dll = sys.argv[1] if len(sys.argv) > 1 else default_dll
    ok, msg = inject(dll)
    print((u'注入成功' if ok else u'注入失败') + u'\n' + msg)
    _msg(ok, msg)
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())

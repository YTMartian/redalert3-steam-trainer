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
import hashlib
import os
import shutil
import struct
import sys
import time
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


def find_ra3_game_processes():
    """Every running ra3_*.game process as (pid, exe name)."""
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == INVALID_HANDLE_VALUE:
        return []
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    found = []
    if kernel32.Process32FirstW(snap, ctypes.byref(pe)):
        while True:
            name = pe.szExeFile
            low = name.lower()
            if low.startswith('ra3') and low.endswith('.game'):
                found.append((pe.th32ProcessID, name))
            if not kernel32.Process32NextW(snap, ctypes.byref(pe)):
                break
    kernel32.CloseHandle(snap)
    return found


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


def bundle_root():
    """PyInstaller extract dir. Empty when running inject.py directly."""
    if not getattr(sys, 'frozen', False):
        return ''
    return getattr(sys, '_MEIPASS', '') or ''


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.digest()


def _copy_if_different(src, dst):
    """Copy src over dst when the bytes differ. Returns (ok, note)."""
    if not os.path.isfile(src):
        return False, u'missing'
    try:
        if os.path.isfile(dst) and os.path.getsize(src) == os.path.getsize(dst):
            if _sha256_file(src) == _sha256_file(dst):
                return True, u'same'
    except OSError:
        pass
    last_err = u''
    for _attempt in range(6):
        tmp = dst + '.tmp'
        try:
            parent = os.path.dirname(dst)
            if parent and not os.path.isdir(parent):
                os.makedirs(parent)
            shutil.copy2(src, tmp)
            os.replace(tmp, dst)
            return True, u'copied'
        except OSError as exc:
            last_err = str(exc)
            try:
                if os.path.isfile(tmp):
                    os.remove(tmp)
            except OSError:
                pass
            time.sleep(0.25)
    return False, last_err or u'copy failed'


def extract_bundled_payload(dest_dir, include_dll=True, only_dll=False, bundle=None):
    """Unpack DLL, arm helper and name tables next to the injector exe.

    unit_names.txt is created only when missing, so a hand-edited override stays.
    Returns a list of error strings. A DLL still locked by the game is reported
    as 'dll-locked: ...'.
    """
    root = bundle if bundle is not None else bundle_root()
    if not root:
        return []
    errors = []

    def take(name, required, dll_file=False):
        src = os.path.join(root, name)
        dst = os.path.join(dest_dir, name)
        if not os.path.isfile(src):
            if required:
                errors.append(u'%s 不在注入器里' % name)
            return
        ok, note = _copy_if_different(src, dst)
        if ok:
            return
        if dll_file:
            errors.append(u'dll-locked: %s' % note)
        else:
            errors.append(u'无法写出 %s（%s）' % (name, note))

    if include_dll or only_dll:
        take('ra3_overlay_v4.dll', True, dll_file=True)
    if only_dll:
        return errors

    take('arm_mustcode.dll', True)
    take('unit_names_csf.txt', True)
    names_dst = os.path.join(dest_dir, 'unit_names.txt')
    names_src = os.path.join(root, 'unit_names.txt')
    if os.path.isfile(names_src) and not os.path.isfile(names_dst):
        take('unit_names.txt', False)
    return errors


def battlenet_client_running():
    """RA3 online client. Match the exe name, not the install folder."""
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == INVALID_HANDLE_VALUE:
        return False
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    names = ('ra3.battlenet.client.exe', 'ra3battlenet.exe')
    found = False
    if kernel32.Process32FirstW(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() in names:
                found = True
                break
            if not kernel32.Process32NextW(snap, ctypes.byref(pe)):
                break
    kernel32.CloseHandle(snap)
    return found


def inject(dll_path, pid=None):
    if battlenet_client_running():
        return False, u'请关闭战网进程（RA3BattleNet）'
    dll_path = os.path.abspath(dll_path)
    if not os.path.isfile(dll_path):
        return False, u'找不到覆盖层 DLL：\n%s' % dll_path
    if not dll_path.lower().endswith('.dll'):
        return False, u'目标不是 DLL：\n%s' % dll_path

    dll_path = os.path.normpath(dll_path)
    wrong_version = None
    if pid is None:
        games = find_ra3_game_processes()
        for game_pid, game_name in games:
            if game_name.lower() == 'ra3_1.12.game':
                pid = game_pid
                break
        if not pid and games:
            wrong_version = games[0][1]
    if wrong_version:
        return False, u'__WRONG_VERSION__:' + wrong_version
    if not pid:
        return False, u'没有找到游戏进程。\n请先启动红警 3，再运行修改器。'

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

    if getattr(sys, 'frozen', False):
        bundled_dll = os.path.normpath(os.path.join(
            os.path.dirname(sys.executable), 'ra3_overlay_v4.dll'))
        if os.path.normcase(os.path.abspath(dll_path)) == os.path.normcase(bundled_dll):
            dll_errors = extract_bundled_payload(
                os.path.dirname(bundled_dll), only_dll=True)
            if dll_errors:
                kernel32.CloseHandle(hproc)
                return False, (
                    u'无法更新 DLL，文件仍被占用。\n'
                    u'请先完全退出红警 3，再运行修改器。'
                )

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
            u'注入器会把 DLL 解到自己旁边。请确认没有被安全软件隔离。\n'
            u'日志：和修改器同一个目录的 ra3_overlay.log'
        )
    return True, (
        u'覆盖层已载入。\n'
        u'进程 PID：%d\n'
        u'进局后按 Home 打开菜单。'
    ) % pid


def guide_image_paths():
    """The two Steam screenshots that show how to set -runver 1.12."""
    names = ('image_1.png', 'image_2.png')
    roots = []
    if getattr(sys, 'frozen', False):
        meipass = getattr(sys, '_MEIPASS', '')
        roots.append(os.path.join(meipass, 'v1.12_change_method'))
        roots.append(os.path.join(os.path.dirname(sys.executable), 'v1.12_change_method'))
    here = os.path.dirname(os.path.abspath(__file__))
    roots.append(os.path.normpath(os.path.join(here, '..', '..', 'v1.12_change_method')))
    roots.append(os.path.normpath(os.path.join(here, '..', 'bin', 'v1.12_change_method')))
    for root in roots:
        paths = [os.path.normpath(os.path.join(root, name)) for name in names]
        if all(os.path.isfile(path) for path in paths):
            return paths
    return None


def _guide_text(running_name):
    return (
        u'注入失败。当前运行的是 %s，不是 1.12。\n'
        u'请按下面两步把启动版本改成 1.12，然后完全退出并重新启动游戏，再运行本修改器。\n'
        u'\n'
        u'1. 在 Steam 库中右键《命令与征服：红色警戒 3》，选择「属性」。\n'
        u'2. 打开「通用」，在启动选项中填入 -runver 1.12。'
    ) % running_name


def show_runver_guide(running_name):
    """Popup with the two setup screenshots. Falls back to a text box."""
    paths = guide_image_paths()
    text = _guide_text(running_name)
    if not paths or not _show_guide_window(text, paths):
        _msg(False, text)
        return False
    return True


def _show_guide_window(text, paths):
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    gdi32 = ctypes.WinDLL('gdi32', use_last_error=True)
    gdiplus = ctypes.WinDLL('gdiplus', use_last_error=True)
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass

    class GdiplusStartupInput(ctypes.Structure):
        _fields_ = [
            ('GdiplusVersion', ctypes.c_uint32),
            ('DebugEventCallback', ctypes.c_void_p),
            ('SuppressBackgroundThread', ctypes.c_int),
            ('SuppressExternalCodecs', ctypes.c_int),
        ]

    class RECT(ctypes.Structure):
        _fields_ = [
            ('left', ctypes.c_long), ('top', ctypes.c_long),
            ('right', ctypes.c_long), ('bottom', ctypes.c_long),
        ]

    class PAINTSTRUCT(ctypes.Structure):
        _fields_ = [
            ('hdc', ctypes.c_void_p),
            ('fErase', wintypes.BOOL),
            ('rcPaint', RECT),
            ('fRestore', wintypes.BOOL),
            ('fIncUpdate', wintypes.BOOL),
            ('rgbReserved', ctypes.c_byte * 32),
        ]

    class WNDCLASSW(ctypes.Structure):
        _fields_ = [
            ('style', ctypes.c_uint),
            ('lpfnWndProc', ctypes.c_void_p),
            ('cbClsExtra', ctypes.c_int),
            ('cbWndExtra', ctypes.c_int),
            ('hInstance', ctypes.c_void_p),
            ('hIcon', ctypes.c_void_p),
            ('hCursor', ctypes.c_void_p),
            ('hbrBackground', ctypes.c_void_p),
            ('lpszMenuName', ctypes.c_wchar_p),
            ('lpszClassName', ctypes.c_wchar_p),
        ]

    class MSG(ctypes.Structure):
        _fields_ = [
            ('hwnd', ctypes.c_void_p),
            ('message', ctypes.c_uint),
            ('wParam', ctypes.c_size_t),
            ('lParam', ctypes.c_ssize_t),
            ('time', wintypes.DWORD),
            ('pt_x', ctypes.c_long),
            ('pt_y', ctypes.c_long),
        ]

    class SCROLLINFO(ctypes.Structure):
        _fields_ = [
            ('cbSize', ctypes.c_uint),
            ('fMask', ctypes.c_uint),
            ('nMin', ctypes.c_int),
            ('nMax', ctypes.c_int),
            ('nPage', ctypes.c_uint),
            ('nPos', ctypes.c_int),
            ('nTrackPos', ctypes.c_int),
        ]

    user32.DrawTextW.argtypes = [
        ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint]
    user32.FillRect.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    user32.GetDC.argtypes = [ctypes.c_void_p]
    user32.GetDC.restype = ctypes.c_void_p
    user32.ReleaseDC.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    user32.BeginPaint.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    user32.BeginPaint.restype = ctypes.c_void_p
    user32.EndPaint.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    user32.InvalidateRect.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.BOOL]
    user32.GetClientRect.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    user32.MoveWindow.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.BOOL]
    user32.DestroyWindow.argtypes = [ctypes.c_void_p]
    user32.PostQuitMessage.argtypes = [ctypes.c_int]
    user32.ShowWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user32.UpdateWindow.argtypes = [ctypes.c_void_p]
    user32.SetScrollInfo.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, wintypes.BOOL]
    user32.SetScrollInfo.restype = ctypes.c_int
    user32.GetScrollInfo.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    user32.GetScrollInfo.restype = wintypes.BOOL
    user32.AdjustWindowRect.argtypes = [ctypes.c_void_p, ctypes.c_uint, wintypes.BOOL]
    user32.GetSystemMetrics.argtypes = [ctypes.c_int]
    user32.GetSystemMetrics.restype = ctypes.c_int
    user32.LoadCursorW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    user32.LoadCursorW.restype = ctypes.c_void_p
    user32.RegisterClassW.argtypes = [ctypes.c_void_p]
    user32.RegisterClassW.restype = wintypes.ATOM
    user32.CreateWindowExW.argtypes = [
        ctypes.c_uint, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    user32.CreateWindowExW.restype = ctypes.c_void_p
    user32.DefWindowProcW.argtypes = [
        ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    user32.DefWindowProcW.restype = ctypes.c_ssize_t
    user32.GetMessageW.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
    user32.GetMessageW.restype = ctypes.c_int
    user32.TranslateMessage.argtypes = [ctypes.c_void_p]
    user32.DispatchMessageW.argtypes = [ctypes.c_void_p]
    user32.DispatchMessageW.restype = ctypes.c_ssize_t
    gdi32.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
    gdi32.CreateCompatibleDC.restype = ctypes.c_void_p
    gdi32.CreateCompatibleBitmap.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    gdi32.CreateCompatibleBitmap.restype = ctypes.c_void_p
    gdi32.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    gdi32.SelectObject.restype = ctypes.c_void_p
    gdi32.CreateFontW.argtypes = [
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint,
        ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_wchar_p]
    gdi32.CreateFontW.restype = ctypes.c_void_p
    gdi32.DeleteObject.argtypes = [ctypes.c_void_p]
    gdi32.BitBlt.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_void_p, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
    gdi32.SetBkMode.argtypes = [ctypes.c_void_p, ctypes.c_int]
    gdi32.SetTextColor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    gdi32.CreateSolidBrush.argtypes = [ctypes.c_uint]
    gdi32.CreateSolidBrush.restype = ctypes.c_void_p
    gdi32.GetStockObject.argtypes = [ctypes.c_int]
    gdi32.GetStockObject.restype = ctypes.c_void_p
    gdiplus.GdiplusStartup.argtypes = [
        ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p, ctypes.c_void_p]
    gdiplus.GdiplusStartup.restype = ctypes.c_int
    gdiplus.GdiplusShutdown.argtypes = [ctypes.c_size_t]
    gdiplus.GdipCreateBitmapFromFile.argtypes = [
        ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_void_p)]
    gdiplus.GdipCreateBitmapFromFile.restype = ctypes.c_int
    gdiplus.GdipGetImageWidth.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)]
    gdiplus.GdipGetImageWidth.restype = ctypes.c_int
    gdiplus.GdipGetImageHeight.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)]
    gdiplus.GdipGetImageHeight.restype = ctypes.c_int
    gdiplus.GdipCreateFromHDC.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    gdiplus.GdipCreateFromHDC.restype = ctypes.c_int
    gdiplus.GdipDrawImageRectI.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    gdiplus.GdipDrawImageRectI.restype = ctypes.c_int
    gdiplus.GdipDeleteGraphics.argtypes = [ctypes.c_void_p]
    gdiplus.GdipDisposeImage.argtypes = [ctypes.c_void_p]
    kernel32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    kernel32.GetModuleHandleW.restype = ctypes.c_void_p

    token = ctypes.c_size_t()
    startup = GdiplusStartupInput(1, None, 0, 0)
    if gdiplus.GdiplusStartup(ctypes.byref(token), ctypes.byref(startup), None) != 0:
        return False

    images = []
    try:
        for path in paths:
            bmp = ctypes.c_void_p()
            if gdiplus.GdipCreateBitmapFromFile(path, ctypes.byref(bmp)) != 0 or not bmp.value:
                return False
            width = ctypes.c_uint()
            height = ctypes.c_uint()
            gdiplus.GdipGetImageWidth(bmp, ctypes.byref(width))
            gdiplus.GdipGetImageHeight(bmp, ctypes.byref(height))
            images.append((bmp, int(width.value), int(height.value)))

        margin = 20
        content_w = 900
        max_img_w = content_w - margin * 2
        scaled = []
        for bmp, width, height in images:
            draw_w = min(width, max_img_w)
            draw_h = max(1, int(round(height * (draw_w / float(width)))))
            scaled.append((bmp, draw_w, draw_h))

        screen_dc = user32.GetDC(None)
        mem_dc = gdi32.CreateCompatibleDC(screen_dc)
        font = gdi32.CreateFontW(
            -18, 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0, u'Microsoft YaHei UI')
        old_font = gdi32.SelectObject(mem_dc, font)
        text_rect = RECT(margin, margin, content_w - margin, margin + 20)
        DT_WORDBREAK = 0x10
        DT_CALCRECT = 0x400
        user32.DrawTextW(mem_dc, text, -1, ctypes.byref(text_rect), DT_WORDBREAK | DT_CALCRECT)
        text_h = max(40, text_rect.bottom - text_rect.top)
        gdi32.SelectObject(mem_dc, old_font)

        # Keep both screenshots, including the launch-option box, on screen.
        screen_h = user32.GetSystemMetrics(1)
        footer = 52
        budget = max(360, screen_h - 180 - footer - text_h - 16)
        natural = sum(item[2] for item in scaled) + 16 * len(scaled)
        if natural > budget and natural > 0:
            fit = budget / float(natural)
            fitted = []
            for bmp, draw_w, draw_h in scaled:
                fitted.append((
                    bmp,
                    max(1, int(round(draw_w * fit))),
                    max(1, int(round(draw_h * fit))),
                ))
            scaled = fitted

        y = margin + text_h + 16
        slots = []
        for bmp, draw_w, draw_h in scaled:
            x = margin + (max_img_w - draw_w) // 2
            slots.append((bmp, x, y, draw_w, draw_h))
            y += draw_h + 16
        content_h = y + 8

        mem_bmp = gdi32.CreateCompatibleBitmap(screen_dc, content_w, content_h)
        old_bmp = gdi32.SelectObject(mem_dc, mem_bmp)
        brush = gdi32.CreateSolidBrush(0x00F4F4F4)
        fill = RECT(0, 0, content_w, content_h)
        user32.FillRect(mem_dc, ctypes.byref(fill), brush)
        gdi32.DeleteObject(brush)
        old_font = gdi32.SelectObject(mem_dc, font)
        gdi32.SetBkMode(mem_dc, 1)
        gdi32.SetTextColor(mem_dc, 0x00222222)
        text_rect = RECT(margin, margin, content_w - margin, margin + text_h + 4)
        user32.DrawTextW(mem_dc, text, -1, ctypes.byref(text_rect), DT_WORDBREAK)
        gdi32.SelectObject(mem_dc, old_font)

        graphics = ctypes.c_void_p()
        if gdiplus.GdipCreateFromHDC(mem_dc, ctypes.byref(graphics)) != 0:
            return False
        for bmp, x, top, draw_w, draw_h in slots:
            gdiplus.GdipDrawImageRectI(graphics, bmp, x, top, draw_w, draw_h)
        gdiplus.GdipDeleteGraphics(graphics)
        user32.ReleaseDC(None, screen_dc)

        state = {
            'mem_dc': mem_dc,
            'mem_bmp': mem_bmp,
            'old_bmp': old_bmp,
            'font': font,
            'content_w': content_w,
            'content_h': content_h,
            'scroll': 0,
            'view_h': content_h,
            'button': None,
        }

        LRESULT = ctypes.c_ssize_t
        WNDPROC = ctypes.WINFUNCTYPE(
            LRESULT, ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t)

        WM_DESTROY = 0x0002
        WM_CLOSE = 0x0010
        WM_PAINT = 0x000F
        WM_ERASEBKGND = 0x0014
        WM_VSCROLL = 0x0115
        WM_MOUSEWHEEL = 0x020A
        WM_COMMAND = 0x0111
        WM_SIZE = 0x0005
        SB_VERT = 1
        SIF_ALL = 0x17

        def set_scroll(hwnd):
            info = SCROLLINFO()
            info.cbSize = ctypes.sizeof(SCROLLINFO)
            info.fMask = SIF_ALL
            info.nMin = 0
            info.nMax = max(0, state['content_h'] - 1)
            info.nPage = max(1, state['view_h'])
            info.nPos = state['scroll']
            user32.SetScrollInfo(hwnd, SB_VERT, ctypes.byref(info), True)
            user32.GetScrollInfo(hwnd, SB_VERT, ctypes.byref(info))
            state['scroll'] = info.nPos

        def clamp_scroll(delta):
            limit = max(0, state['content_h'] - state['view_h'])
            state['scroll'] = max(0, min(limit, state['scroll'] + delta))

        def wndproc(hwnd, msg, wparam, lparam):
            if msg == WM_ERASEBKGND:
                return 1
            if msg == WM_SIZE:
                client = RECT()
                user32.GetClientRect(hwnd, ctypes.byref(client))
                footer = 52
                state['view_h'] = max(1, (client.bottom - client.top) - footer)
                if state['button']:
                    user32.MoveWindow(
                        state['button'],
                        max(0, (client.right - 120) // 2),
                        state['view_h'] + 10,
                        120, 32, True)
                set_scroll(hwnd)
                user32.InvalidateRect(hwnd, None, True)
                return 0
            if msg == WM_VSCROLL:
                code = wparam & 0xFFFF
                if code == 0:
                    clamp_scroll(-40)
                elif code == 1:
                    clamp_scroll(40)
                elif code == 2:
                    clamp_scroll(-state['view_h'])
                elif code == 3:
                    clamp_scroll(state['view_h'])
                elif code in (5, 4):
                    state['scroll'] = (wparam >> 16) & 0xFFFF
                    clamp_scroll(0)
                elif code == 6:
                    return 0
                set_scroll(hwnd)
                user32.InvalidateRect(hwnd, None, False)
                return 0
            if msg == WM_MOUSEWHEEL:
                delta = ctypes.c_short((wparam >> 16) & 0xFFFF).value
                clamp_scroll(-int(delta / 120) * 60)
                set_scroll(hwnd)
                user32.InvalidateRect(hwnd, None, False)
                return 0
            if msg == WM_PAINT:
                ps = PAINTSTRUCT()
                hdc = user32.BeginPaint(hwnd, ctypes.byref(ps))
                gdi32.BitBlt(
                    hdc, 0, 0, state['content_w'], state['view_h'],
                    state['mem_dc'], 0, state['scroll'], 0x00CC0020)
                user32.EndPaint(hwnd, ctypes.byref(ps))
                return 0
            if msg == WM_COMMAND or msg == WM_CLOSE:
                user32.DestroyWindow(hwnd)
                return 0
            if msg == WM_DESTROY:
                user32.PostQuitMessage(0)
                return 0
            return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

        callback = WNDPROC(wndproc)
        _show_guide_window._callback = callback
        hinst = kernel32.GetModuleHandleW(None)
        cls_name = u'RA3RunverGuide'
        wc = WNDCLASSW()
        wc.lpfnWndProc = ctypes.cast(callback, ctypes.c_void_p)
        wc.hInstance = hinst
        wc.hCursor = user32.LoadCursorW(None, ctypes.c_void_p(32512))
        wc.hbrBackground = gdi32.GetStockObject(0)
        wc.lpszClassName = cls_name
        user32.RegisterClassW(ctypes.byref(wc))

        style = 0x00CF0000 | 0x00200000  # OVERLAPPEDWINDOW | VSCROLL
        footer = 52
        screen_h = user32.GetSystemMetrics(1)
        screen_w = user32.GetSystemMetrics(0)
        view_h = min(content_h, max(240, screen_h - 140 - footer))
        state['view_h'] = view_h
        outer = RECT(0, 0, content_w, view_h + footer)
        user32.AdjustWindowRect(ctypes.byref(outer), style, False)
        win_w = outer.right - outer.left
        win_h = outer.bottom - outer.top
        x = max(0, (screen_w - win_w) // 2)
        y = max(0, (screen_h - win_h) // 2)
        hwnd = user32.CreateWindowExW(
            0x00000008, cls_name, u'注入失败', style,
            x, y, win_w, win_h, None, None, hinst, None)
        if not hwnd:
            return False
        state['button'] = user32.CreateWindowExW(
            0, u'BUTTON', u'关闭', 0x50000000 | 0x00000001,
            (content_w - 120) // 2, view_h + 10, 120, 32,
            hwnd, ctypes.c_void_p(1), hinst, None)
        set_scroll(hwnd)
        user32.ShowWindow(hwnd, 1)
        user32.UpdateWindow(hwnd)
        msg = MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        return True
    finally:
        for bmp, _width, _height in images:
            gdiplus.GdipDisposeImage(bmp)
        gdiplus.GdiplusShutdown(token)


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
        unpack_errors = [
            item for item in extract_bundled_payload(here)
            if not item.startswith(u'dll-locked:')
        ]
        if unpack_errors:
            _msg(False, u'无法释出修改器文件：\n' + u'\n'.join(unpack_errors))
            return 1
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
    if (not ok) and msg.startswith(u'__WRONG_VERSION__:'):
        running = msg.split(u':', 1)[1]
        print(u'注入失败\n' + _guide_text(running))
        show_runver_guide(running)
        return 1
    print((u'注入成功' if ok else u'注入失败') + u'\n' + msg)
    _msg(ok, msg)
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())

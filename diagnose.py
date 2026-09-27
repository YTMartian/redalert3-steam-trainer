# -*- coding: utf-8 -*-
"""
诊断脚本：运行游戏后执行，输出游戏进程名、模块基址，并校验 hook 处字节。
用法（游戏运行中）：  python diagnose.py
"""
import ctypes
from ctypes import wintypes

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.Process32First.restype = wintypes.BOOL
kernel32.Process32Next.restype = wintypes.BOOL
kernel32.Module32First.restype = wintypes.BOOL
kernel32.Module32Next.restype = wintypes.BOOL
kernel32.OpenProcess.restype = ctypes.c_void_p
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.ReadProcessMemory.restype = wintypes.BOOL
kernel32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
PROCESS_VM_READ = 0x10
PROCESS_QUERY_INFORMATION = 0x400

# PlayerID hook 信息
PLAYERID_RVA = 0x54119B
PLAYERID_AOB = bytes.fromhex('8b50288b4220')


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD),
                ('th32ProcessID', wintypes.DWORD),
                ('th32DefaultHeapID', ctypes.POINTER(ctypes.c_ulong)),
                ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
                ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', ctypes.c_long),
                ('dwFlags', wintypes.DWORD), ('szExeFile', ctypes.c_char * 260)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('th32ModuleID', wintypes.DWORD),
                ('th32ProcessID', wintypes.DWORD), ('GlblcntUsage', wintypes.DWORD),
                ('ProccntUsage', wintypes.DWORD), ('modBaseAddr', ctypes.POINTER(ctypes.c_byte)),
                ('modBaseSize', wintypes.DWORD), ('hModule', ctypes.c_void_p),
                ('szModule', ctypes.c_char * 256), ('szExePath', ctypes.c_char * 260)]


def enum_processes():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    procs = []
    if snap == INVALID_HANDLE_VALUE:
        return procs
    e = PROCESSENTRY32()
    e.dwSize = ctypes.sizeof(PROCESSENTRY32)
    if kernel32.Process32First(snap, ctypes.byref(e)):
        while True:
            name = e.szExeFile.decode('gbk', errors='ignore')
            procs.append((e.th32ProcessID, name))
            if not kernel32.Process32Next(snap, ctypes.byref(e)):
                break
    kernel32.CloseHandle(snap)
    return procs


def enum_modules(pid):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    mods = []
    if snap == INVALID_HANDLE_VALUE:
        return mods
    m = MODULEENTRY32()
    m.dwSize = ctypes.sizeof(MODULEENTRY32)
    if kernel32.Module32First(snap, ctypes.byref(m)):
        while True:
            name = m.szModule.decode('gbk', errors='ignore')
            base = ctypes.cast(m.modBaseAddr, ctypes.c_void_p).value
            mods.append((name, base, m.modBaseSize))
            if not kernel32.Module32Next(snap, ctypes.byref(m)):
                break
    kernel32.CloseHandle(snap)
    return mods


def read_proc(pid, addr, size):
    h = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        return None
    buf = ctypes.create_string_buffer(size)
    rd = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(ctypes.c_void_p(h), ctypes.c_void_p(addr), buf, size, ctypes.byref(rd))
    kernel32.CloseHandle(ctypes.c_void_p(h))
    return buf.raw[:rd.value]


print('=== 1. 所有 ra3 相关进程 ===')
game_procs = []
for pid, name in enum_processes():
    if 'ra3' in name.lower():
        game_procs.append((pid, name))
        print('  pid=%6d  name=%r' % (pid, name))

if not game_procs:
    print('  （未找到任何 ra3 相关进程，游戏可能未启动）')

print()
print('=== 2. 游戏进程模块列表 ===')
for pid, name in game_procs:
    print('--- pid %d (%s) ---' % (pid, name))
    for modname, base, size in enum_modules(pid):
        marker = ''
        if 'game' in modname.lower() or 'ra3' in modname.lower():
            marker = ' <=='
        print('  base=0x%08X  size=0x%X  %s%s' % (base, size, modname, marker))

print()
print('=== 3. hook 处字节校验（PlayerID @ +0x54119B）===')
for pid, name in game_procs:
    # 找 ra3_1.12.game 模块基址
    target_base = None
    for modname, base, size in enum_modules(pid):
        if modname.lower() == 'ra3_1.12.game':
            target_base = base
            break
    print('--- pid %d (%s) ---' % (pid, name))
    if target_base is None:
        print('  未找到 ra3_1.12.game 模块！')
        # 尝试主模块基址 0x400000 读一下
        for test_base in (0x400000,):
            data = read_proc(pid, test_base + PLAYERID_RVA, 6)
            print('  尝试 base=0x%X: 读到 %s' % (test_base, data.hex() if data else '(读取失败)'))
        continue
    data = read_proc(pid, target_base + PLAYERID_RVA, 6)
    print('  ra3_1.12.game base = 0x%X' % target_base)
    print('  PlayerID 处读到: %s' % (data.hex() if data else '(读取失败)'))
    print('  期望 AOB:        %s' % PLAYERID_AOB.hex())
    print('  匹配: %s' % ('YES' if data == PLAYERID_AOB else 'NO'))
    # 也试 0x400000
    if target_base != 0x400000:
        data2 = read_proc(pid, 0x400000 + PLAYERID_RVA, 6)
        print('  按 base=0x400000 读到: %s  匹配=%s' % (data2.hex() if data2 else '(失败)', data2 == PLAYERID_AOB))

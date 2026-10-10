# -*- coding: utf-8 -*-
"""Arm MustCode into ra3_1.12.game.

Called by the overlay DLL after it VirtualAlloc's the four segments.
This avoids the broken relocatable embedded blob (which differed from live
keystone output and crashed on match).

Usage:
  python arm_mustcode.py <pid> <mc> <mc2> <flags> <idb> [spectate=0|1]

Writes %TEMP%\\ra3_overlay_arm_result.txt with OK/FAIL line.
"""
from __future__ import print_function

import os
import struct
import sys

if getattr(sys, 'frozen', False):
    # PyInstaller onefile: payload/trainer/keystone live in _MEIPASS
    ROOT = getattr(sys, '_MEIPASS', os.path.dirname(sys.executable))
    sys.path.insert(0, os.path.dirname(sys.executable))
    # Ensure keystone.dll beside the extracted keystone package is loadable
    ks_dir = os.path.join(ROOT, 'keystone')
    if os.path.isdir(ks_dir):
        os.environ['PATH'] = ks_dir + os.pathsep + os.environ.get('PATH', '')
else:
    ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, ROOT)

from payload import (  # noqa: E402
    ASM_TEXT, SYMBOLS, LABELS, CORE_HOOKS, EP1_CORE_HOOKS, ep1_asm_and_symbols,
)
from mustcode_asm import build, MOD_BASE, PLAYER_HOOK_NAMES  # noqa: E402

import ctypes
from ctypes import wintypes

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
kernel32.OpenProcess.restype = ctypes.c_void_p
kernel32.VirtualProtectEx.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD)
]
kernel32.WriteProcessMemory.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t)
]
kernel32.ReadProcessMemory.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t)
]

PROCESS_ALL_ACCESS = 0x1F0FFF
PAGE_EXECUTE_READWRITE = 0x40
RESULT = os.path.join(os.environ.get('TEMP', '.'), 'ra3_overlay_arm_result.txt')


def result(ok, msg):
    with open(RESULT, 'w', encoding='utf-8') as f:
        f.write(('OK: ' if ok else 'FAIL: ') + msg + '\n')
    print(('OK: ' if ok else 'FAIL: ') + msg)
    return 0 if ok else 1


def read_mem(h, addr, n):
    buf = (ctypes.c_char * n)()
    got = ctypes.c_size_t(0)
    if not kernel32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n,
                                      ctypes.byref(got)):
        return b''
    return bytes(buf[:got.value])


def write_mem(h, addr, data):
    buf = ctypes.create_string_buffer(data)
    written = ctypes.c_size_t(0)
    return bool(kernel32.WriteProcessMemory(
        h, ctypes.c_void_p(addr), buf, len(data), ctypes.byref(written)))


def write_code(h, addr, data):
    old = wintypes.DWORD(0)
    if not kernel32.VirtualProtectEx(h, ctypes.c_void_p(addr), len(data),
                                     PAGE_EXECUTE_READWRITE, ctypes.byref(old)):
        return False
    ok = write_mem(h, addr, data)
    kernel32.VirtualProtectEx(h, ctypes.c_void_p(addr), len(data), old,
                              ctypes.byref(old))
    return ok


def va_of(module_base, va):
    return module_base + (va - MOD_BASE)


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


def process_module_names(pid):
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.Module32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
    kernel32.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
    snap = kernel32.CreateToolhelp32Snapshot(0x00000008 | 0x00000010, pid)
    if not snap or snap == wintypes.HANDLE(-1).value:
        return []
    me = MODULEENTRY32W()
    me.dwSize = ctypes.sizeof(MODULEENTRY32W)
    names = []
    if kernel32.Module32FirstW(snap, ctypes.byref(me)):
        while True:
            names.append(me.szModule.lower())
            if not kernel32.Module32NextW(snap, ctypes.byref(me)):
                break
    kernel32.CloseHandle(snap)
    return names


def select_profile(pid):
    names = process_module_names(pid)
    if 'ra3ep1_1.0.game' in names:
        asm, sym = ep1_asm_and_symbols()
        return asm, sym, EP1_CORE_HOOKS, 'ra3ep1_1.0.game'
    if 'ra3_1.12.game' in names:
        return ASM_TEXT, SYMBOLS, CORE_HOOKS, 'ra3_1.12.game'
    if any(n.startswith('ra3ep1') for n in names):
        return None
    return ASM_TEXT, SYMBOLS, CORE_HOOKS, 'assumed-1.12'


def main():
    if len(sys.argv) < 6:
        return result(False, 'usage: arm_mustcode.py pid mc mc2 flags idb [spectate]')
    pid = int(sys.argv[1], 0)
    mc = int(sys.argv[2], 0)
    mc2 = int(sys.argv[3], 0)
    flags = int(sys.argv[4], 0)
    idb = int(sys.argv[5], 0)
    spectate = int(sys.argv[6], 0) if len(sys.argv) > 6 else 0

    module_base = MOD_BASE  # RA3 1.12 has no ASLR

    h = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not h:
        return result(False, 'OpenProcess failed err=%d' % ctypes.get_last_error())

    profile = select_profile(pid)
    if profile is None:
        kernel32.CloseHandle(h)
        return result(False, 'uprising build is not ra3ep1_1.0.game')
    asm_text, symbols, hooks, game_name = profile

    try:
        mc_bytes, mc2_bytes = build(
            asm_text, symbols, mc, mc2, flags, idb, module_base)
        if not write_mem(h, mc, mc_bytes):
            return result(False, 'WriteProcessMemory MustCode failed')
        if not write_mem(h, mc2, mc2_bytes):
            return result(False, 'WriteProcessMemory MustCode2 failed')
        # init flags like trainer
        write_mem(h, flags + 0x24, b'\xA0\xA5\x86\x65')
        write_mem(h, flags + 0x20, b'\x00\x00\x00\x00')

        # probe PlayerID
        probe = next(x for x in hooks if x[0] == 'PlayerID')
        probe_va, probe_aob = probe[1], probe[2]
        expect = bytes.fromhex(probe_aob)
        got = read_mem(h, va_of(module_base, probe_va), len(expect))
        if got != expect:
            return result(False, 'probe failed @0x%X got %s expect %s' % (
                va_of(module_base, probe_va), got.hex(), expect.hex()))

        installed = 0
        for name, hook_va_abs, aob, target_off in hooks:
            if spectate and name in PLAYER_HOOK_NAMES:
                continue
            if not spectate:
                pass  # install all in normal mode (same as trainer normal attach)
            aob_len = len(aob) // 2
            tag = 'mc_' + format(target_off, 'x')
            label_off = LABELS['MC'].get(tag)
            if label_off is None:
                return result(False, 'missing label %s' % tag)
            hook_va = va_of(module_base, hook_va_abs)
            jmp_target = mc + label_off
            rel = jmp_target - (hook_va + 5)
            patch = b'\xE9' + struct.pack('<i', rel) + b'\x90' * (aob_len - 5)
            if not write_code(h, hook_va, patch):
                return result(False, 'patch %s failed' % name)
            installed += 1

        return result(True, 'armed %d hooks for %s mc=0x%X' % (
            installed, game_name, mc))
    except Exception as exc:
        return result(False, 'exception: %s' % exc)
    finally:
        kernel32.CloseHandle(h)


if __name__ == '__main__':
    raise SystemExit(main())

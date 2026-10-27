# -*- coding: utf-8 -*-
"""Arm MustCode into ra3_1.12.game.

Called by the overlay DLL after it VirtualAlloc's the four segments.
This avoids the broken relocatable embedded blob (which differed from live
keystone output and crashed on match).

Usage:
  python arm_mustcode.py <pid> <mc> <mc2> <flags> <idb> [spectate=0|1]

Every hook site is checked against its recorded AOB before any byte is written;
sites that disagree are skipped (and named on the result line) instead of being
blindly overwritten with an E9 jump.

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


def token_note():
    """本进程的权限画像，写进 FAIL 行方便判断是不是被降权启动的。"""
    try:
        advapi = ctypes.WinDLL('advapi32', use_last_error=True)
        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.GetCurrentProcess.restype = ctypes.c_void_p
        advapi.OpenProcessToken.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                            ctypes.POINTER(ctypes.c_void_p)]
        advapi.GetTokenInformation.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                               ctypes.c_void_p, ctypes.c_uint32,
                                               ctypes.POINTER(ctypes.c_uint32)]
        tok = ctypes.c_void_p()
        if not advapi.OpenProcessToken(k32.GetCurrentProcess(), 0x0008, ctypes.byref(tok)):
            return 'token=?'
        parts = []
        need = ctypes.c_uint32(0)
        buf = ctypes.create_string_buffer(64)
        if advapi.GetTokenInformation(tok, 25, buf, 64, ctypes.byref(need)):
            sid = ctypes.cast(buf, ctypes.POINTER(ctypes.c_void_p))[0]
            raw = ctypes.cast(ctypes.c_void_p(sid), ctypes.POINTER(ctypes.c_ubyte))
            cnt = raw[1]
            if 1 <= cnt <= 15:
                idx = 8 + 4 * (cnt - 1)
                sub = ctypes.cast(ctypes.c_void_p(sid + idx),
                                  ctypes.POINTER(ctypes.c_uint32))[0]
                parts.append('il=0x%X' % sub)
        val = ctypes.c_uint32(0)
        if advapi.GetTokenInformation(tok, 21, ctypes.byref(val), 4, ctypes.byref(need)):
            parts.append('restricted=%d' % (1 if val.value else 0))
        val = ctypes.c_uint32(0)
        if advapi.GetTokenInformation(tok, 29, ctypes.byref(val), 4, ctypes.byref(need)):
            parts.append('appcontainer=%d' % (1 if val.value else 0))
        k32.CloseHandle(tok)
        return ' '.join(parts) if parts else 'token=?'
    except Exception as exc:
        return 'token-err=%s' % exc


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

    # 结果文件路径由调用方给（提权启动时 %TEMP% 不是它改过的目录）。
    global RESULT
    if len(sys.argv) > 7 and sys.argv[7]:
        RESULT = sys.argv[7]
    # Both ra3_1.12.game and ra3ep1_1.0.game load at 0x400000 with ASLR off and
    # zero base relocations, so the PE's own image base is the runtime base.
    module_base = MOD_BASE

    h = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not h:
        # err=5 时把本进程的令牌画像写进结果行：比游戏低（低完整性/受限令牌）就是
        # 启动时被降权挡住了；如果窄权限能打开，则只是权限申请过大。
        err = ctypes.get_last_error()
        narrow = 0x0008 | 0x0010 | 0x0020 | 0x0400
        h2 = kernel32.OpenProcess(narrow, False, pid)
        if h2:
            kernel32.CloseHandle(h2)
            narrow_note = 'narrow-ok'
        else:
            narrow_note = 'narrow-err=%d' % ctypes.get_last_error()
        return result(False, 'OpenProcess failed err=%d %s self=%s' % (err, narrow_note, token_note()))

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

        # Pass 1: verify every site against its AOB before touching any code.
        # A mismatch means the binary (or another tool) differs, so that hook's
        # 5-byte E9 would corrupt live instructions -- skip it, keep the rest.
        def already_armed(addr):
            head = read_mem(h, addr, 5)
            if len(head) != 5 or head[0] != 0xE9:
                return False
            rel = struct.unpack('<i', head[1:5])[0]
            dest = addr + 5 + rel
            return mc <= dest < mc + len(mc_bytes)

        todo, skipped = [], []
        for name, hook_va_abs, aob, target_off in hooks:
            if spectate and name in PLAYER_HOOK_NAMES:
                continue
            aob_len = len(aob) // 2
            hook_va = va_of(module_base, hook_va_abs)
            if aob_len < 5:
                skipped.append('%s(aob %d<5)' % (name, aob_len))
                continue
            expect = bytes.fromhex(aob)
            got = read_mem(h, hook_va, aob_len)
            patched = already_armed(hook_va)
            if got != expect and not patched:
                note = 'unreadable' if not got else got.hex()
                if got[:1] == b'\xE9':
                    note += ' e9-patched'
                skipped.append('%s(@0x%X %s)' % (name, hook_va, note))
                continue
            tag = 'mc_' + format(target_off, 'x')
            label_off = LABELS['MC'].get(tag)
            if label_off is None:
                skipped.append('%s(no label %s)' % (name, tag))
                continue
            todo.append((name, hook_va, aob_len, mc + label_off, not patched))

        # Pass 2: patch only the verified sites, and count real successes.
        armed = 0
        for name, hook_va, aob_len, jmp_target, needs_patch in todo:
            if needs_patch:
                rel = jmp_target - (hook_va + 5)
                patch = b'\xE9' + struct.pack('<i', rel) + b'\x90' * (aob_len - 5)
                if not write_code(h, hook_va, patch):
                    skipped.append('%s(write failed)' % name)
                    continue
            armed += 1

        if armed == 0:
            return result(False, 'no hook armed for %s; rejected: %s' % (
                game_name, '; '.join(skipped) or 'none matched'))
        msg = 'armed %d/%d hooks for %s mc=0x%X' % (
            armed, armed + len(skipped), game_name, mc)
        if skipped:
            msg += ' skipped: ' + '; '.join(skipped)
        return result(True, msg)
    except Exception as exc:
        return result(False, 'exception: %s' % exc)
    finally:
        kernel32.CloseHandle(h)


if __name__ == '__main__':
    raise SystemExit(main())

# -*- coding: utf-8 -*-
"""
红警3 (Command & Conquer: Red Alert 3) Steam 版核心修改器
基于 RedAlert3_Trainer_1.12_FINAL3.exe 逆向适配，覆盖 17 个已验证 hook + 单位速度/血量操作。

使用方法：
  1. 先启动游戏 RA3.exe（进入游戏后）。
  2. 运行本修改器，点击「附加游戏」。
  3. 通过界面按钮或全局热键开关各项功能。
"""
import ctypes
import math
import os
import queue
import random
import struct
import sys
import threading
import time
from ctypes import wintypes

from keystone import Ks, KS_ARCH_X86, KS_MODE_32

try:
    from payload import ASM_TEXT, SYMBOLS, LABELS, CORE_HOOKS
except ImportError:
    ASM_TEXT, SYMBOLS, LABELS, CORE_HOOKS = '', {}, {}, []

# ============================================================
# Win32 API
# ============================================================
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
user32 = ctypes.WinDLL('user32', use_last_error=True)

# 显式声明原型，避免 64 位句柄/地址被截断
kernel32.OpenProcess.restype = ctypes.c_void_p
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.VirtualAllocEx.restype = ctypes.c_void_p
kernel32.VirtualAllocEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
kernel32.WriteProcessMemory.restype = wintypes.BOOL
kernel32.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel32.CreateRemoteThread.restype = ctypes.c_void_p
kernel32.CreateRemoteThread.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, wintypes.DWORD]
kernel32.ReadProcessMemory.restype = wintypes.BOOL
kernel32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.Process32First.restype = wintypes.BOOL
kernel32.Process32Next.restype = wintypes.BOOL
kernel32.Module32First.restype = wintypes.BOOL
kernel32.Module32Next.restype = wintypes.BOOL
kernel32.VirtualProtectEx.restype = wintypes.BOOL
kernel32.VirtualProtectEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]

user32.GetCursorPos.restype = wintypes.BOOL
user32.GetCursorPos.argtypes = [ctypes.c_void_p]
user32.ScreenToClient.restype = wintypes.BOOL
user32.ScreenToClient.argtypes = [wintypes.HWND, ctypes.c_void_p]
user32.EnumWindows.restype = wintypes.BOOL
user32.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.GetWindow.restype = wintypes.HWND
user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]


class POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]


PROCESS_ALL_ACCESS = 0x001F0FFF
PROCESS_VM_OPERATION = 0x0008
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
PROCESS_QUERY_INFORMATION = 0x0400
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
PAGE_EXECUTE_READWRITE = 0x40
PAGE_SIZE = 0x1000
TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

# 游戏固定基址（无 ASLR）
MOD_BASE = 0x400000

# 选中单位管理器全局指针。**注意这是 RVA**（相对模块基址的偏移）：
#   MustCode 里写的是 `mov esi,[ra3_1.12.game+8E08DC]`，
#   即绝对地址 0x400000 + 0x8E08DC = 0xCE08DC。
#   所以这里必须用 module_base + MGR_GLOBAL_RVA，
#   不能走 va_of()（那个是给 VA 用的，会把地址再抬高一个 0x400000）。
MGR_GLOBAL_RVA = 0x8E08DC

# 本地玩家对象全局指针（VA 0xCEDE2C）。观战/观察者时模板 [player+0x28] 为空，
# 或模板带 +0x106 / +0x123C 标志（见游戏函数 0x877010 / 0x877580）。
LOCAL_PLAYER_RVA = 0x8EDE2C

# 观战时会崩的资源类 hook：观战自动卸载，有本地势力时再装上
PLAYER_HOOK_NAMES = {'PlayerID', 'Money', 'Power', 'SCPoint', 'HaveAllSC'}

# 观战模式可安全安装的 hook：不依赖本地玩家 ID / 资源模板。
# DisableAllSP* 无条件拖超武冷却；SuperPower* 在 [ID]==0（未装 PlayerID）时
# 会把所有非空归属都当「敌方」，效果等同禁用全场超武。
SPECTATE_HOOK_NAMES = {
    'DisableAllSP', 'DisableAllSP2', 'SuperPower', 'SuperPower2',
}

# 星级 / 摧毁相关的游戏函数（VA）
FN_ADD_XP = 0x005173F0       # __cdecl(entity, int xp)  官方加经验接口（唯一安全）
FN_DESTROY = 0x007DCDF0      # __thiscall(entity; args=6,0x19,0) 官方摧毁，ret 0xC
FN_CREATE_UNIT = 0x006440F0  # __cdecl(flags, template, pos*, owner, owner+0x10)
# Steam 屏幕像素 → 地图世界坐标（__cdecl(in*{x,y:i32}, out*{xyz:f32}, flag, flag)）。
# 由 Zoom(0x62B82D) 相对零售 GetMouse 偏移定位；调用约定与 MustCode GetMouseXYZinMap 同形。
FN_GET_MOUSE_XYZ = 0x0062C500
# Steam 重定位：原 MustCode CreateUnit(零售 0x205240) 的等价工厂入口。
# 签名与 MustCode+AA0 一致：push owner+0x10 / owner / pos / template / 0; add esp,14
# 【危险·绝不要调用】0x0071B290 = tracker 的「按经验重算星级」。
#   实测：跳过官方入口直接调它，游戏会直接挂掉（等级链状态不一致）。

# 各内存段分配大小
MC_SIZE = 0x3000
MC2_SIZE = 0x1000
FLAGS_SIZE = 0x100
IDB_SIZE = 0x100

ks = Ks(KS_ARCH_X86, KS_MODE_32)


# ============================================================
# 汇编器（从 assemble.py 精简，移除 capstone，标签偏移已固化在 payload.LABELS）
# ============================================================
def parse_off(off):
    off = off.strip()
    if off.lower().startswith('0x'):
        return int(off, 16)
    return int(off, 10)


def norm_off(off):
    off = off.lstrip('+').lower()
    if off.startswith('0x'):
        off = off[2:]
    return off.lstrip('0') or '0'


def tag_of(sym):
    if sym.startswith('MC2'):
        return 'mc2_' + norm_off(sym[3:])
    if sym.startswith('MC'):
        return 'mc_' + norm_off(sym[2:])
    if sym.startswith('_Exit'):
        return 'exit_' + sym[1:].lower()
    return sym


def subst_abs(line, name, base):
    import re
    pat = re.compile(r'\b%s(\+[0-9A-Fa-fx]+)?\b' % re.escape(name))

    def repl(m):
        off = m.group(1)
        if off:
            return '0x%X' % (base + parse_off(off[1:]))
        return '0x%X' % base
    return pat.sub(repl, line)


def build(asm_text, symbols, mc_base, mc2_base, flags_base, idb_base, mod_base):
    """把 mustcode_body.asm 用运行时实际分配地址重汇编，返回 (mc_bytes, mc2_bytes)。"""
    import re
    back = symbols
    lines = asm_text.split('\n')

    abs_syms = [
        ('MC2', mc2_base),
        ('MC', mc_base),
        ('MOD', mod_base),
        ('FLAGS', flags_base),
        ('IDB', idb_base),
    ]
    # SYMBOLS（_BackXXX）里存的是 VA（绝对地址，已含 0x400000 基址），直接使用
    abs_syms += [(k, v) for k, v in back.items()]

    mc_lines, mc2_lines = [], []
    current = None

    def subst_data(line):
        def repl_bracket(m):
            inner = m.group(1)
            for name, base in abs_syms:
                inner = subst_abs(inner, name, base)
            return '[' + inner + ']'
        return re.sub(r'\[([^\]]+)\]', repl_bracket, line)

    def subst_code(line, seg):
        if seg == 'mc':
            line = re.sub(r'\bMC(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: tag_of(m.group(0)), line)
            line = re.sub(r'\bMC2(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: subst_abs(m.group(0), 'MC2', mc2_base), line)
        else:
            line = re.sub(r'\bMC2(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: tag_of(m.group(0)), line)
            line = re.sub(r'\bMC(\+0x[0-9a-fA-F]+)?\b',
                          lambda m: subst_abs(m.group(0), 'MC', mc_base), line)
        line = re.sub(r'\b_Exit\w+\b', lambda m: tag_of(m.group(0)), line)
        return line

    for line in lines:
        line = line.strip()
        if not line:
            continue
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_+]*):$', line)
        if m:
            name = m.group(1)
            if name.startswith('MC2'):
                current = 'mc2'
                mc2_lines.append(tag_of(name) + ':')
            elif name.startswith('MC'):
                current = 'mc'
                mc_lines.append(tag_of(name) + ':')
            elif name.startswith('MOD'):
                current = None
            elif name.startswith('_Exit'):
                (mc2_lines if current == 'mc2' else mc_lines).append(tag_of(name) + ':')
            continue
        if current is None:
            continue
        line = subst_abs(line, 'MOD', mod_base)
        line = subst_abs(line, 'FLAGS', flags_base)
        line = subst_abs(line, 'IDB', idb_base)
        for name, val in back.items():
            line = re.sub(r'\b%s\b' % re.escape(name), '0x%X' % val, line)
        line = subst_data(line)
        line = subst_code(line, current)
        (mc2_lines if current == 'mc2' else mc_lines).append(line)

    mc_enc, _ = ks.asm('\n'.join(mc_lines), mc_base)
    mc2_enc, _ = ks.asm('\n'.join(mc2_lines), mc2_base)
    return bytes(mc_enc), bytes(mc2_enc)


# ============================================================
# 内存操作（ctypes 直连 kernel32，避免额外依赖）
# ============================================================
class GameProcess:
    def __init__(self):
        self.pid = 0
        self.handle = 0
        self.module_base = 0
        self.game_module = ''
        self.mc_base = 0
        self.mc2_base = 0
        self.flags_base = 0
        self.idb_base = 0
        self.hooked = False
        self.installed_hooks = []  # [(name, va_abs, aob), ...] 已打补丁，脱离时只还原这些
        self.auto_spectator = False  # True=智能模式，随观战状态装卸资源 hook
        self._spectator_cached = None  # 上次检测到的观战状态（用于状态栏提示）
        # 调用桩是共享的：多线程（例如连按两次 p）同时写会把机器码写坏，
        # 所以所有 call_remote 串行化。
        self._call_lock = threading.Lock()
        self._stub = 0
        self._eax_slot = 0   # 远程调用 EAX 返回值暂存槽（游戏进程内）
        self._clone_seq = 0  # 连续复制计数，用于错开落点避免叠坐标崩溃

    # ---- 进程 / 模块查找 ----
    def find_game(self):
        """查找游戏逻辑进程 ra3_1.XX.game（RA3.exe 只是启动器，不附加它）。"""
        pid, modname = self._find_game_process()
        if not pid:
            return None
        self.pid = pid
        self.game_module = modname
        # 游戏主模块基址（无 ASLR + RELOCS_STRIPPED，正常应为 0x400000）
        self.module_base = self._find_module(pid, modname) or MOD_BASE
        return pid

    @staticmethod
    def _find_game_process():
        """枚举进程，返回 (pid, 模块名)。优先 ra3_1.12.game，其次任意 ra3_*.game，兜底 RA3.exe。"""
        class PROCESSENTRY32(ctypes.Structure):
            _fields_ = [('dwSize', wintypes.DWORD),
                        ('cntUsage', wintypes.DWORD),
                        ('th32ProcessID', wintypes.DWORD),
                        ('th32DefaultHeapID', ctypes.POINTER(ctypes.c_ulong)),
                        ('th32ModuleID', wintypes.DWORD),
                        ('cntThreads', wintypes.DWORD),
                        ('th32ParentProcessID', wintypes.DWORD),
                        ('pcPriClassBase', ctypes.c_long),
                        ('dwFlags', wintypes.DWORD),
                        ('szExeFile', ctypes.c_char * 260)]
        snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        if snap == INVALID_HANDLE_VALUE:
            return 0, ''
        entry = PROCESSENTRY32()
        entry.dwSize = ctypes.sizeof(PROCESSENTRY32)
        game_candidates = []   # (pid, name)
        ra3exe_pid = 0
        if kernel32.Process32First(snap, ctypes.byref(entry)):
            while True:
                name = entry.szExeFile.decode('gbk', errors='ignore')
                ln = name.lower()
                if ln.startswith('ra3') and ln.endswith('.game'):
                    game_candidates.append((entry.th32ProcessID, name))
                elif ln == 'ra3.exe':
                    ra3exe_pid = entry.th32ProcessID
                if not kernel32.Process32Next(snap, ctypes.byref(entry)):
                    break
        kernel32.CloseHandle(snap)

        # 优先 1.12 版本
        for pid, name in game_candidates:
            if name.lower() == 'ra3_1.12.game':
                return pid, name
        # 其他 .game 版本（如 1.13）
        if game_candidates:
            return game_candidates[0]
        # 兜底：RA3.exe
        if ra3exe_pid:
            return ra3exe_pid, 'RA3.exe'
        return 0, ''

    @staticmethod
    def _find_pid(name):
        class PROCESSENTRY32(ctypes.Structure):
            _fields_ = [('dwSize', wintypes.DWORD),
                        ('cntUsage', wintypes.DWORD),
                        ('th32ProcessID', wintypes.DWORD),
                        ('th32DefaultHeapID', ctypes.POINTER(ctypes.c_ulong)),
                        ('th32ModuleID', wintypes.DWORD),
                        ('cntThreads', wintypes.DWORD),
                        ('th32ParentProcessID', wintypes.DWORD),
                        ('pcPriClassBase', ctypes.c_long),
                        ('dwFlags', wintypes.DWORD),
                        ('szExeFile', ctypes.c_char * 260)]
        snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        if snap == INVALID_HANDLE_VALUE:
            return 0
        entry = PROCESSENTRY32()
        entry.dwSize = ctypes.sizeof(PROCESSENTRY32)
        target = name.lower()
        result = 0
        if kernel32.Process32First(snap, ctypes.byref(entry)):
            while True:
                exe = entry.szExeFile.decode('gbk', errors='ignore').lower()
                if exe == target:
                    result = entry.th32ProcessID
                    break
                if not kernel32.Process32Next(snap, ctypes.byref(entry)):
                    break
        kernel32.CloseHandle(snap)
        return result

    @staticmethod
    def _find_module(pid, modname):
        class MODULEENTRY32(ctypes.Structure):
            _fields_ = [('dwSize', wintypes.DWORD),
                        ('th32ModuleID', wintypes.DWORD),
                        ('th32ProcessID', wintypes.DWORD),
                        ('GlblcntUsage', wintypes.DWORD),
                        ('ProccntUsage', wintypes.DWORD),
                        ('modBaseAddr', ctypes.POINTER(ctypes.c_byte)),
                        ('modBaseSize', wintypes.DWORD),
                        ('hModule', ctypes.c_void_p),
                        ('szModule', ctypes.c_char * 256),
                        ('szExePath', ctypes.c_char * 260)]
        snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
        if snap == INVALID_HANDLE_VALUE:
            return 0
        entry = MODULEENTRY32()
        entry.dwSize = ctypes.sizeof(MODULEENTRY32)
        target = modname.lower()
        result = 0
        if kernel32.Module32First(snap, ctypes.byref(entry)):
            while True:
                mod = entry.szModule.decode('gbk', errors='ignore').lower()
                if mod == target:
                    result = ctypes.cast(entry.modBaseAddr, ctypes.c_void_p).value
                    break
                if not kernel32.Module32Next(snap, ctypes.byref(entry)):
                    break
        kernel32.CloseHandle(snap)
        return result

    # ---- 附加 / 分配 ----
    def va_of(self, va):
        """CORE_HOOKS 里的地址字段是 VA（绝对虚拟地址，含 0x400000 基址）。
        转成当前模块基址下的实际地址（游戏无 ASLR，模块固定 0x400000）。"""
        return self.module_base + (va - MOD_BASE)

    def attach(self):
        if not self.find_game():
            return False, '未找到游戏进程（请先启动游戏 RA3.exe）'
        self.handle = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, self.pid)
        if not self.handle:
            return False, '打开进程失败（请以管理员身份运行）'
        return True, ''

    def detach(self):
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = 0

    def alloc(self, size):
        addr = kernel32.VirtualAllocEx(
            ctypes.c_void_p(self.handle), None, size,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
        return addr or 0

    def write(self, addr, data):
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        written = ctypes.c_size_t(0)
        ok = kernel32.WriteProcessMemory(
            ctypes.c_void_p(self.handle), ctypes.c_void_p(addr),
            buf, len(data), ctypes.byref(written))
        return bool(ok) and written.value == len(data)

    def write_code(self, addr, data):
        """写入代码段（.text 只读页需先临时改为可写，写完恢复保护）。"""
        start = addr & ~(PAGE_SIZE - 1)
        end = (addr + len(data) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)
        span = end - start
        old = wintypes.DWORD(0)
        if not kernel32.VirtualProtectEx(
                ctypes.c_void_p(self.handle), ctypes.c_void_p(start), span,
                PAGE_EXECUTE_READWRITE, ctypes.byref(old)):
            return False
        ok = self.write(addr, data)
        kernel32.VirtualProtectEx(
            ctypes.c_void_p(self.handle), ctypes.c_void_p(start), span,
            old.value, ctypes.byref(old))
        return ok

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        read = ctypes.c_size_t(0)
        kernel32.ReadProcessMemory(
            ctypes.c_void_p(self.handle), ctypes.c_void_p(addr),
            buf, size, ctypes.byref(read))
        return buf.raw[:read.value]

    # ---- 在目标进程里直接调用游戏函数（诊断/实验用） ----
    def call_remote(self, addr, this=None, args=(), cleanup=True, timeout=4000,
                    capture_eax=False):
        """在游戏进程内调用一个 x86 函数。

        addr    目标函数地址（绝对地址）
        this    传入 ecx（__thiscall 的 this），None 表示不管 ecx
        args    参数列表，按 cdecl 顺序（本方法会反向压栈）
        cleanup True 表示按 __cdecl 由调用方清栈（__thiscall 无栈参数时无影响）
        capture_eax  True 时额外返回函数 EAX（用于 CreateUnit 等判断是否成功）

        返回 (ok, err) 或 capture_eax 时 (ok, err, eax)。
        注意：会短暂冻结游戏，属正常现象。
        """
        with self._call_lock:
            return self._call_remote_locked(
                addr, this, args, cleanup, timeout, capture_eax)

    def _call_remote_locked(self, addr, this=None, args=(), cleanup=True,
                            timeout=4000, capture_eax=False):
        code = bytearray(b'\x60')                       # pushad 保存全部寄存器
        if this is not None:
            code += b'\xb9' + struct.pack('<I', this & 0xFFFFFFFF)
        for a in reversed(list(args)):
            code += b'\x68' + struct.pack('<I', a & 0xFFFFFFFF)
        code += b'\xb8' + struct.pack('<I', addr & 0xFFFFFFFF)
        code += b'\xff\xd0'                             # call eax
        if cleanup and args:
            code += b'\x81\xc4' + struct.pack('<I', 4 * len(args))
        if capture_eax:
            if not self._eax_slot:
                self._eax_slot = self.alloc(4)
                if not self._eax_slot:
                    if capture_eax:
                        return False, '分配 EAX 槽失败', 0
                    return False, '分配 EAX 槽失败'
            # call 后 EAX 是返回值；在 popad 之前写入槽
            code += b'\xa3' + struct.pack('<I', self._eax_slot & 0xFFFFFFFF)
        code += b'\x61\xc3'                             # popad; ret

        if not getattr(self, '_stub', None):
            self._stub = self.alloc(0x100)
            if not self._stub:
                if capture_eax:
                    return False, '分配调用桩内存失败', 0
                return False, '分配调用桩内存失败'
        stub = self._stub
        if not self.write(stub, b'\xcc' * 0x100):
            if capture_eax:
                return False, '清理调用桩失败', 0
            return False, '清理调用桩失败'
        if not self.write(stub, bytes(code)):
            if capture_eax:
                return False, '写入调用桩失败', 0
            return False, '写入调用桩失败'

        if capture_eax and self._eax_slot:
            self.write(self._eax_slot, b'\x00\x00\x00\x00')

        h = kernel32.CreateRemoteThread(
            ctypes.c_void_p(self.handle), None, 0,
            ctypes.c_void_p(stub), None, 0, None)
        if not h:
            err = 'CreateRemoteThread 失败 (err=%d)' % ctypes.get_last_error()
            if capture_eax:
                return False, err, 0
            return False, err
        waited = kernel32.WaitForSingleObject(ctypes.c_void_p(h), timeout)
        kernel32.CloseHandle(ctypes.c_void_p(h))
        if waited == 0x102:                 # WAIT_TIMEOUT
            # 远端线程可能还在跑：绝不能复用这块桩内存（会被下一次写入踩坏），
            # 直接丢弃，下次调用重新分配。
            self._stub = 0
            err = '调用超时（>%d ms），已放弃这一次' % timeout
            if capture_eax:
                return False, err, 0
            return False, err
        eax = 0
        if capture_eax and self._eax_slot:
            raw = self.read(self._eax_slot, 4)
            if len(raw) == 4:
                eax = struct.unpack('<I', raw)[0]
            return True, None, eax
        return True, None

    # ---- 注入 ----
    def _probe_module(self):
        """用 PlayerID 处 AOB 校验模块版本。成功返回 None，失败返回错误串。"""
        probe = next((h for h in CORE_HOOKS if h[0] == 'PlayerID'), CORE_HOOKS[0])
        probe_va, probe_aob = probe[1], probe[2]
        probe_len = len(probe_aob) // 2
        got = self.read(self.va_of(probe_va), probe_len)
        expect = bytes.fromhex(probe_aob)
        if got == expect:
            return None
        if self.game_module and self.game_module.lower() != 'ra3_1.12.game':
            return ('检测到游戏模块为 %s，但本修改器适配 ra3_1.12.game。'
                    '请在 Steam 启动选项里添加 -runver 1.12 后重启游戏'
                    % self.game_module)
        return ('基址校验失败：模块 0x%X 地址 0x%X 处读到 %s，期望 %s'
                '（游戏版本不符或基址错误）'
                % (self.module_base, self.va_of(probe_va), got.hex(), expect.hex()))

    def _patch_one_hook(self, name, hook_va_abs, aob, target_off):
        """给单个 hook 打补丁。成功返回 None，失败返回错误串。"""
        hook_va = self.va_of(hook_va_abs)
        aob_len = len(aob) // 2
        tag = 'mc_' + format(target_off, 'x')
        label_off = LABELS['MC'].get(tag)
        if label_off is None:
            return '缺少标签 %s (%s)' % (tag, name)
        jmp_target = self.mc_base + label_off
        rel = jmp_target - (hook_va + 5)
        if not (-0x80000000 <= rel <= 0x7FFFFFFF):
            return '%s hook 跳转距离超出 ±2GB（MC 段分配地址过远）' % name
        patch = b'\xE9' + struct.pack('<i', rel) + b'\x90' * (aob_len - 5)
        if len(patch) != aob_len:
            return '%s hook 补丁长度错误' % name
        if not self.write_code(hook_va, patch):
            return '%s hook 写入失败' % name
        return None

    def _installed_names(self):
        return {n for n, _, _ in self.installed_hooks}

    def install_named_hooks(self, names):
        """安装名单中尚未安装的 hook。返回 (ok, err)。"""
        want = set(names)
        have = self._installed_names()
        for name, hook_va_abs, aob, target_off in CORE_HOOKS:
            if name not in want or name in have:
                continue
            err = self._patch_one_hook(name, hook_va_abs, aob, target_off)
            if err:
                return False, err
            self.installed_hooks.append((name, hook_va_abs, aob))
        return True, None

    def uninstall_named_hooks(self, names):
        """还原名单中已安装的 hook。"""
        drop = set(names)
        kept = []
        for name, va, aob in self.installed_hooks:
            if name in drop:
                self.write_code(self.va_of(va), bytes.fromhex(aob))
            else:
                kept.append((name, va, aob))
        self.installed_hooks = kept

    def is_spectator(self):
        """是否处于观战/无本地势力状态。

        依据游戏自己的判定（0x877580 / 0x877010）：
          · [LOCAL_PLAYER] 为空 → 视为观战（主菜单也是，资源 hook 先不装）
          · [player+0x28] 模板为空 → 观战
          · 模板 +0x106 或 +0x123C 非 0 → 观察者标志
        """
        if not self.handle or not self.module_base:
            return True
        player = self.read_u32(self.module_base + LOCAL_PLAYER_RVA)
        if not self.is_ptr(player):
            return True
        tmpl = self.read_u32(player + 0x28)
        if not self.is_ptr(tmpl):
            return True
        b106 = self.read(tmpl + 0x106, 1)
        if b106 and b106[0] != 0:
            return True
        b123c = self.read(tmpl + 0x123C, 1)
        if b123c and b123c[0] != 0:
            return True
        return False

    def sync_player_hooks(self):
        """智能模式：观战卸掉资源 hook，有本地势力再装上。

        返回 (changed, spectator, msg)；changed=False 表示状态未变无需提示。
        """
        if not self.auto_spectator or not self.hooked:
            return False, self._spectator_cached, ''
        spec = self.is_spectator()
        have_player = bool(self._installed_names() & PLAYER_HOOK_NAMES)
        if spec and have_player:
            self.uninstall_named_hooks(PLAYER_HOOK_NAMES)
            self._spectator_cached = True
            return True, True, '检测到观战/无本地势力：已禁用资源 hook（防崩溃）'
        if (not spec) and (not have_player):
            ok, err = self.install_named_hooks(PLAYER_HOOK_NAMES)
            if not ok:
                return True, False, '启用资源 hook 失败：' + err
            self._spectator_cached = False
            return True, False, '检测到本地玩家：已启用资源 hook'
        if self._spectator_cached is None:
            self._spectator_cached = spec
        return False, spec, ''

    def inject(self, enabled_names=None, auto_spectator=False):
        """分配内存、重汇编并写入、按名单补丁 hook。

        enabled_names:
          None → 安装全部 CORE_HOOKS（或智能模式下先装非资源类）
          空集合 → 只分配 MustCode/FLAGS，不打任何补丁（观战崩溃二分用）
          名字集合 → 只安装名单内的 hook
        auto_spectator:
          True → 智能模式：始终装非资源 hook；资源 hook 按观战状态装卸
        """
        self.mc_base = self.alloc(MC_SIZE)
        self.mc2_base = self.alloc(MC2_SIZE)
        self.flags_base = self.alloc(FLAGS_SIZE)
        self.idb_base = self.alloc(IDB_SIZE)
        if not (self.mc_base and self.mc2_base and self.flags_base and self.idb_base):
            return False, '内存分配失败'

        try:
            mc, mc2 = build(ASM_TEXT, SYMBOLS,
                            self.mc_base, self.mc2_base,
                            self.flags_base, self.idb_base, self.module_base)
        except Exception as e:
            return False, '汇编失败: %s' % e

        if not self.write(self.mc_base, mc):
            return False, '写入 MustCode 失败'
        if not self.write(self.mc2_base, mc2):
            return False, '写入 MustCode2 失败'
        self.write(self.flags_base + 0x24, b'\xA0\xA5\x86\x65')
        self.write(self.flags_base + 0x20, b'\x00\x00\x00\x00')

        err = self._probe_module()
        if err:
            return False, err

        self.auto_spectator = bool(auto_spectator)
        self._spectator_cached = None
        self.installed_hooks = []

        if auto_spectator:
            # 先装安全 hook；资源 hook 交给 sync_player_hooks
            selected = [h for h in CORE_HOOKS if h[0] not in PLAYER_HOOK_NAMES]
        elif enabled_names is None:
            selected = list(CORE_HOOKS)
        else:
            allow = set(enabled_names)
            selected = [h for h in CORE_HOOKS if h[0] in allow]

        for name, hook_va_abs, aob, target_off in selected:
            err = self._patch_one_hook(name, hook_va_abs, aob, target_off)
            if err:
                return False, err
            self.installed_hooks.append((name, hook_va_abs, aob))

        self.hooked = True
        extra = ''
        if auto_spectator:
            changed, spec, msg = self.sync_player_hooks()
            extra = ' | ' + (msg or ('观战中' if spec else '本地玩家'))
        return True, '已安装 %d/%d 个 hook%s' % (
            len(self.installed_hooks), len(CORE_HOOKS), extra)

    def set_flag_byte(self, offset, value):
        if not self.handle or not self.flags_base:
            return False
        return self.write(self.flags_base + offset, bytes([value & 0xFF]))

    def get_flag_byte(self, offset):
        if not self.handle or not self.flags_base:
            return 0
        return self.read(self.flags_base + offset, 1)[0]

    def set_command(self, value):
        """写 FLAGS+20（dword），由 Money hook 分发一次性执行。"""
        if not self.handle or not self.flags_base:
            return False
        return self.write(self.flags_base + 0x20, struct.pack('<I', value))

    # ---- 选中单位指针链（与 MustCode2 里用的一致） ----
    def read_u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<I', b)[0] if len(b) == 4 else None

    def read_f32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<f', b)[0] if len(b) == 4 else None

    def write_f32(self, addr, value):
        return self.write(addr, struct.pack('<f', float(value)))

    def is_ptr(self, v):
        return v is not None and 0x10000 <= v < 0x7FFF0000 and (v & 3) == 0

    def mgr_addr(self):
        """「选中单位管理器」全局指针的绝对地址 = 基址 + RVA 0x8E08DC。"""
        return self.module_base + MGR_GLOBAL_RVA

    def selected_entities(self, limit=64):
        """遍历「选中单位管理器」链表，返回单位实体地址列表。

        管理器 = [基址+0x8E08DC]（Steam 1.12 的全局指针）
           +0x5C = 选中数量   +0x50 = 链表头   节点+0x08 = 对象   对象+0x138 = 实体
        每一步都判空，任何一个环节失效就返回已收集到的部分（不抛异常）。
        """
        out = []
        mgr = self.read_u32(self.mgr_addr())
        if not self.is_ptr(mgr):
            return out
        count = self.read_u32(mgr + 0x5C) or 0
        node = self.read_u32(mgr + 0x50)
        steps = count if 0 < count <= limit else limit
        head = node
        for i in range(steps):
            if not self.is_ptr(node):
                break
            obj = self.read_u32(node + 8)
            if self.is_ptr(obj):
                ent = self.read_u32(obj + 0x138)
                if self.is_ptr(ent):
                    out.append(ent)
            nxt = self.read_u32(node)
            if not self.is_ptr(nxt) or nxt == head or nxt == node:
                break
            node = nxt
        return out

    def _speed_nodes(self, ent):
        """取出单位速度控制器节点列表（可能 0~2 个），结构同 MustCode2。

        实体+0x374 → +0x200 → [0]=主节点 / [4]=副节点（副节点还需 +0x18==1.0）
        节点 +0x8 = 当前倍率，+0x40 = 备份的原始倍率。
        """
        vec = self.read_u32(ent + 0x374)
        if not self.is_ptr(vec):
            return []
        ctrl = self.read_u32(vec + 0x200)
        if not self.is_ptr(ctrl):
            return []
        nodes = []
        first = self.read_u32(ctrl)
        if self.is_ptr(first):
            nodes.append(first)
        second = self.read_u32(ctrl + 4)
        if self.is_ptr(second):
            flag = self.read_f32(second + 0x18)
            if flag is not None and abs(flag - 1.0) < 1e-6:
                nodes.append(second)
        return nodes

    def _set_speed_node(self, node, target):
        """按原脚本逻辑写速度倍率：特殊值之间切换不覆盖备份。"""
        specials = (500.0, 10.0, 0.0)
        cur = self.read_f32(node + 8)
        if cur is None:
            return False
        if abs(cur - target) < 1e-4:
            return True
        # 当前不是「我们设过的特殊值」时，先备份到 +0x40
        if not any(abs(cur - s) < 1e-4 for s in specials):
            self.write_f32(node + 0x40, cur)
        return self.write_f32(node + 8, target)

    def _restore_speed_node(self, node):
        """从 +0x40 恢复；若当前不是特殊值则不动。"""
        specials = (500.0, 10.0, 0.0)
        cur = self.read_f32(node + 8)
        if cur is None:
            return False
        if not any(abs(cur - s) < 1e-4 for s in specials):
            return True
        bak = self.read_f32(node + 0x40)
        if bak is None:
            return False
        return self.write_f32(node + 8, bak)

    def apply_unit_speed(self, mode):
        """对选中单位改速度。mode: max/slow/freeze/restore。不依赖 Money hook。"""
        targets = {'max': 500.0, 'slow': 10.0, 'freeze': 0.0}
        ents = self.selected_entities()
        if not ents:
            return False, '没读到选中单位（请先选中）'
        n = 0
        for ent in ents:
            nodes = self._speed_nodes(ent)
            if not nodes:
                continue
            ok = True
            for node in nodes:
                if mode == 'restore':
                    ok = self._restore_speed_node(node) and ok
                else:
                    ok = self._set_speed_node(node, targets[mode]) and ok
            if ok:
                n += 1
        if n == 0:
            return False, '选中对象没有可写的速度组件'
        names = {'max': '超速×500', 'slow': '慢速×10', 'freeze': '冻结',
                 'restore': '恢复速度'}
        return True, '已对 %d 个单位执行「%s」' % (n, names.get(mode, mode))

    def _hp_component(self, ent):
        hp = self.read_u32(ent + 0x33C)
        return hp if self.is_ptr(hp) else None

    def _write_entity_hp(self, ent, mode):
        hp = self._hp_component(ent)
        if not hp:
            return False
        if mode == 'max':
            return self.write_f32(hp + 4, 9999999.0) and self.write_f32(hp + 0xC, 9999999.0)
        if mode == 'min':
            return self.write_f32(hp + 4, 1.0)
        if mode == 'normal':
            mx = self.read_f32(hp + 0x10)
            if mx is None:
                return False
            return self.write_f32(hp + 4, mx) and self.write_f32(hp + 0xC, mx)
        return False

    def _entity_owner(self, ent):
        return self.read_u32(ent + 0x418)

    def _local_owner_for_unit_ops(self):
        """实体 +0x418 上的归属；优先本地玩家，观战时可读 PlayerID hook 写入的 IDB。"""
        owner = self._local_owner()
        if self.is_ptr(owner):
            return owner
        if self.idb_base:
            cached = self.read_u32(self.idb_base)
            if self.is_ptr(cached):
                return cached
        return None

    def _filter_entities_by_owner(self, ents, relation):
        """relation: ally=与本地同归属；enemy=非本地归属。无本地归属时仅处理当前选中。"""
        local = self._local_owner_for_unit_ops()
        out = []
        for ent in ents:
            ow = self._entity_owner(ent)
            if relation == 'ally':
                if local:
                    if self.is_ptr(ow) and ow == local:
                        out.append(ent)
                else:
                    out.append(ent)
            elif relation == 'enemy':
                if local:
                    if self.is_ptr(ow) and ow != local:
                        out.append(ent)
                else:
                    if self.is_ptr(ow):
                        out.append(ent)
        return out, local

    def apply_unit_hp(self, mode, entities=None):
        """对选中单位改血量。mode: max/min/normal。不依赖 Money hook。"""
        ents = entities if entities is not None else self.selected_entities()
        if not ents:
            return False, '没读到选中单位（请先选中）'
        n = 0
        for ent in ents:
            if self._write_entity_hp(ent, mode):
                n += 1
        if n == 0:
            return False, '选中对象没有可写的血量组件'
        names = {'max': '无敌', 'min': '残血', 'normal': '恢复血量'}
        return True, '已对 %d 个单位执行「%s」' % (n, names.get(mode, mode))

    def apply_unit_hp_relation(self, mode, relation):
        """按归属筛选后再改血（无全体单位列表，仅当前选中里符合条件的）。"""
        ents = self.selected_entities()
        if not ents:
            return False, '没读到选中单位（请先选中）'
        filtered, local = self._filter_entities_by_owner(ents, relation)
        rel_name = '友军/己方' if relation == 'ally' else '敌方'
        if not filtered:
            if local:
                return False, ('选中里没有可识别的%s单位（请框选对应阵营）'
                               % rel_name)
            return False, '选中里没有带有效归属的单位'
        ok, msg = self.apply_unit_hp(mode, entities=filtered)
        if not ok:
            return ok, msg
        scope = ('（仅当前选中中符合条件的单位，非地图全体）'
                 if len(filtered) < len(ents) or len(ents) < 64 else
                 '（仅当前选中，非地图全体）')
        return True, msg + scope

    def kill_selected(self):
        """在独立线程里调官方摧毁接口 0x7DCDF0，真正拆掉选中单位。

        签名（与原版修改器 DestroySelectUnit 一致，Steam 重定位后地址）：
          __thiscall  ecx=实体
          栈参数 push 0 / push 0x19 / push 6
          函数自身 ret 0xC 清栈，故 call_remote 必须 cleanup=False。

        只写血量为 0 只会让单位「不能选中、血条消失」，并不会走死亡/拆除流程。
        返回 (ok, 消息)。
        """
        ents = self.selected_entities()
        if not ents:
            mgr = self.read_u32(self.mgr_addr())
            return False, ('没读到选中单位。诊断：管理器指针 [0x%08X] = 0x%s'
                           '（先在游戏里选中单位）'
                           % (self.mgr_addr(), ('%08X' % mgr) if mgr else '读不到'))
        ok_n = 0
        detail = []
        for ent in ents:
            ok, err = self.call_remote(
                self.va_of(FN_DESTROY), this=ent,
                args=(6, 0x19, 0), cleanup=False, timeout=4000)
            if ok:
                ok_n += 1
                detail.append('0x%08X' % ent)
            else:
                detail.append('0x%08X 失败(%s)' % (ent, err))
        if ok_n == 0:
            return False, '摧毁失败：' + '；'.join(detail)
        return True, '已摧毁 %d 个单位：%s' % (ok_n, '、'.join(detail))

    def _local_owner(self):
        """取本地玩家的「归属对象」（供 CreateUnit 使用）。

        MustCode 的 CopyForMe 用 PlayerID hook 写入的 [ID]=[本地玩家+0x28]；
        游戏生成单位时写到实体+0x418 的是 [本地玩家+0x30]。两者都试，优先 +0x28。
        """
        player = self.read_u32(self.module_base + LOCAL_PLAYER_RVA)
        if not self.is_ptr(player):
            return None
        for off in (0x28, 0x30):
            owner = self.read_u32(player + off)
            if self.is_ptr(owner):
                # +0x10 在 CreateUnit 里原样压栈，允许为 0
                return owner
        return None

    def _game_hwnd(self):
        """找本游戏进程的主窗口句柄（用于 ScreenToClient）。"""
        if not self.pid:
            return None
        found = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def _enum(hwnd, _lparam):
            pid = wintypes.DWORD(0)
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value != self.pid:
                return True
            if not user32.IsWindowVisible(hwnd):
                return True
            if user32.GetWindow(hwnd, 4):  # GW_OWNER
                return True
            if user32.GetWindowTextLengthW(hwnd) <= 0:
                return True
            found.append(hwnd)
            return True

        user32.EnumWindows(_enum, 0)
        return found[0] if found else None

    def _mouse_client_xy(self):
        """当前鼠标在游戏客户区中的像素坐标。失败返回 None。"""
        hwnd = self._game_hwnd()
        if not hwnd:
            return None
        pt = POINT()
        if not user32.GetCursorPos(ctypes.byref(pt)):
            return None
        if not user32.ScreenToClient(hwnd, ctypes.byref(pt)):
            return None
        return int(pt.x), int(pt.y)

    def mouse_world_pos(self):
        """把当前鼠标位置转换成地图世界坐标 (x,y,z)。

        Win32 取客户区像素 → FN_GET_MOUSE_XYZ 投影到地形。
        缓冲沿用 MustCode：输入 MC+0x1110，输出 MC+0x1080。失败返回 None。
        """
        if not self.mc_base:
            return None
        xy = self._mouse_client_xy()
        if not xy:
            return None
        in_buf = self.mc_base + 0x1110
        out_buf = self.mc_base + 0x1080
        if not self.write(in_buf, struct.pack('<ii', xy[0], xy[1])):
            return None
        # 哨兵：若函数未改写输出，说明投影失败
        if not self.write(out_buf, struct.pack('<III', 0x7F7F7F7F, 0x7F7F7F7F, 0x7F7F7F7F)):
            return None
        ok, _err = self.call_remote(
            self.va_of(FN_GET_MOUSE_XYZ),
            args=(in_buf, out_buf, 0, 0),
            cleanup=True, timeout=4000)
        if not ok:
            return None
        raw = self.read(out_buf, 12)
        if len(raw) < 12:
            return None
        if raw == struct.pack('<III', 0x7F7F7F7F, 0x7F7F7F7F, 0x7F7F7F7F):
            return None
        x, y, z = struct.unpack('<fff', raw)
        if any(v != v for v in (x, y, z)):
            return None
        return float(x), float(y), float(z)

    def clone_selected(self, as_mine=True, copies=1):
        """复制选中单位（独立线程调 FN_CREATE_UNIT）。

        as_mine=True  → 归属本地玩家（正常模式）
        as_mine=False → 归属原单位阵营（观战模式，用实体+0x418）
        copies        → 每个选中模板各复制几份（批量复制）

        模板取自实体+0x4（与 MustCode GetUnitData 一致）；
        生成坐标取自当前鼠标在地图上的落点（FN_GET_MOUSE_XYZ）。
        连续复制会自动错开落点，避免多单位叠在同一坐标引发碰撞/寻路崩溃。
        返回 (ok, 消息)。
        """
        copies = max(1, int(copies))
        ents = self.selected_entities()
        if not ents:
            return False, '没读到选中单位（请先选中）'
        if not self.mc_base:
            return False, 'MustCode 未分配，无法写入坐标缓冲'
        spawn = self.mouse_world_pos()
        if not spawn:
            return False, '读不到鼠标地图坐标（请把鼠标移到战场地形上再试）'
        pos_buf = self.mc_base + 0x1080
        self._clone_seq = (self._clone_seq + 1) & 0xFFFF
        ok_n = 0
        fail_n = 0
        detail = []
        slot_base = self._clone_seq * 3
        for i, ent in enumerate(ents):
            unit_data = self.read_u32(ent + 4)
            if not self.is_ptr(unit_data):
                detail.append('0x%08X 无模板' % ent)
                fail_n += copies
                continue
            if as_mine:
                owner = self._local_owner()
                if not owner:
                    return False, '读不到本地玩家归属（观战中请改用观战模式复制）'
            else:
                owner = self.read_u32(ent + 0x418)
                if not self.is_ptr(owner):
                    detail.append('0x%08X 无归属' % ent)
                    fail_n += copies
                    continue
            # 归属对象本身再校验一次，避免野指针进 CreateUnit
            owner_vt = self.read_u32(owner)
            if not self.is_ptr(owner_vt):
                detail.append('0x%08X 归属无效' % ent)
                fail_n += copies
                continue
            owner_info = self.read_u32(owner + 0x10)
            if owner_info is None:
                detail.append('0x%08X 归属+0x10 读失败' % ent)
                fail_n += copies
                continue
            # owner+0x10 允许为 0（MustCode 原样压栈，不做空指针判断）
            for c in range(copies):
                slot = slot_base + i * copies + c
                radius = 35.0 + 18.0 * (slot % 12)
                angle = (slot * 2.399963)  # 黄金角，近似均匀散开
                x = spawn[0] + radius * math.cos(angle)
                y = spawn[1] + radius * math.sin(angle)
                z = spawn[2]
                if not self.write(pos_buf, struct.pack('<fff', x, y, z)):
                    detail.append('0x%08X 写坐标失败' % ent)
                    fail_n += 1
                    continue
                ok, err, new_ent = self.call_remote(
                    self.va_of(FN_CREATE_UNIT),
                    args=(0, unit_data, pos_buf, owner, owner_info & 0xFFFFFFFF),
                    cleanup=True, timeout=4000, capture_eax=True)
                if ok and self.is_ptr(new_ent):
                    ok_n += 1
                    if copies == 1:
                        detail.append('0x%08X→0x%08X' % (ent, new_ent))
                elif ok:
                    fail_n += 1
                    if copies == 1:
                        detail.append('0x%08X 引擎拒绝生成' % ent)
                else:
                    fail_n += 1
                    if copies == 1:
                        detail.append('0x%08X 失败(%s)' % (ent, err))
                time.sleep(0.05)
        if ok_n == 0:
            return False, '复制失败：' + ('；'.join(detail) if detail else '引擎未生成任何单位')
        who = '己方' if as_mine else '原阵营'
        extra = ('（%d 个被引擎拒绝/失败）' % fail_n) if fail_n else ''
        if copies > 1:
            return True, '已为 %d 个模板各复制约 %d 份到鼠标附近（归属%s，共成功 %d 个）%s' % (
                len(ents), copies, who, ok_n, extra)
        return True, '已复制 %d 个单位到鼠标附近（归属%s）%s：%s' % (
            ok_n, who, extra, '、'.join(detail))

    def convert_selected(self):
        """收编：把选中单位的实体+0x418 改为本地归属（同 MustCode MC2+0x800 写 [IDB]）。"""
        ents = self.selected_entities()
        if not ents:
            return False, '没读到选中单位（请先选中）'
        owner = self._local_owner_for_unit_ops()
        if not self.is_ptr(owner):
            return False, ('读不到本地玩家归属（正常对战需有己方势力；'
                           '观战需已附加且 PlayerID 曾写入缓存）')
        owner_vt = self.read_u32(owner)
        if not self.is_ptr(owner_vt):
            return False, '本地归属指针无效'
        n = 0
        skip = 0
        for ent in ents:
            cur = self._entity_owner(ent)
            if self.is_ptr(cur) and cur == owner:
                skip += 1
                continue
            if self.write(ent + 0x418, struct.pack('<I', owner)):
                n += 1
        if n == 0:
            if skip:
                return False, '选中单位已是己方归属，无需收编'
            return False, '未能写入任何单位的归属'
        extra = ('，%d 个已是己方跳过' % skip) if skip else ''
        return True, '已收编 %d 个单位到本地玩家%s' % (n, extra)

    def _pulse_ammo_hook_for_selected(self):
        """短暂打开弹药 hook，让选中单位走一遍装填逻辑（需已安装 UnitAmmo）。"""
        if not self.flags_base:
            return False
        if 'UnitAmmo' not in self._installed_names():
            return False
        prev = self.get_flag_byte(0x12)
        self.set_flag_byte(0x12, 1)
        time.sleep(0.3)
        if not prev:
            self.set_flag_byte(0x12, 0)
        return True

    def full_buff_selected(self, xp=200000, max_calls=8):
        """选中单位：满血 → 尽量拉满弹药 → 尽量满星。"""
        ents = self.selected_entities()
        if not ents:
            return False, '没读到选中单位（请先选中）'
        hp_n = sum(1 for ent in ents if self._write_entity_hp(ent, 'max'))
        ammo_ok = self._pulse_ammo_hook_for_selected()
        rank_ok, rank_msg = self.rank_up_via_engine(xp=xp, max_calls=max_calls)
        parts = []
        if hp_n:
            parts.append('满血 %d 个' % hp_n)
        else:
            parts.append('满血：无可用血量组件')
        if ammo_ok:
            parts.append('已触发弹药装填（需已开 hook）')
        else:
            parts.append('弹药：请手动开「弹药无限」或确保 UnitAmmo hook 已装')
        if rank_ok:
            parts.append('满星：' + rank_msg.split('：', 1)[-1])
        else:
            parts.append('满星：' + rank_msg)
        ok = hp_n > 0 or rank_ok
        return ok, '；'.join(parts)

    def chaos_selected(self):
        """混乱模式：对每个选中单位随机改速度档位和/或血量。"""
        ents = self.selected_entities()
        if not ents:
            return False, '没读到选中单位（请先选中）'
        speed_modes = ('max', 'slow', 'freeze', 'restore')
        hp_modes = ('max', 'min', 'normal')
        speed_names = {'max': '超速', 'slow': '慢速', 'freeze': '冻结',
                       'restore': '恢复速'}
        hp_names = {'max': '满血', 'min': '残血', 'normal': '正常血'}
        n = 0
        detail = []
        for ent in ents:
            did = False
            parts = []
            # 约 70% 改速度、70% 改血量，至少一个生效
            do_speed = random.random() < 0.7
            do_hp = random.random() < 0.7
            if not do_speed and not do_hp:
                do_speed = True
            if do_speed:
                sm = random.choice(speed_modes)
                nodes = self._speed_nodes(ent)
                if nodes:
                    ok = True
                    for node in nodes:
                        if sm == 'restore':
                            ok = self._restore_speed_node(node) and ok
                        else:
                            targets = {'max': 500.0, 'slow': 10.0, 'freeze': 0.0}
                            ok = self._set_speed_node(node, targets[sm]) and ok
                    if ok:
                        parts.append(speed_names[sm])
                        did = True
            if do_hp:
                hm = random.choice(hp_modes)
                if self._write_entity_hp(ent, hm):
                    parts.append(hp_names[hm])
                    did = True
            if did:
                n += 1
                detail.append('0x%08X(%s)' % (ent, '+'.join(parts)))
        if n == 0:
            return False, '选中对象没有可写的速度/血量组件'
        shown = '、'.join(detail[:6])
        extra = ('…共 %d 个' % n) if n > 6 else ''
        return True, '混乱已施加到 %d 个单位：%s%s' % (n, shown, extra)

    def ore_convoy(self, as_mine=True, copies=8):
        """刷矿车车队：无固定矿车类型 ID，改为对当前选中单位批量复制。

        用法：先选中矿车（或任意要刷的单位），鼠标移到落点，再点本功能。
        """
        copies = max(1, int(copies))
        ok, msg = self.clone_selected(as_mine=as_mine, copies=copies)
        if not ok:
            return ok, ('【车队】' + msg + '（请先选中矿车或其它单位）')
        return True, '【车队·选中模板×%d】%s' % (copies, msg)

    def toggle_fog(self):
        """迷雾开关：翻转全地图 flag(0x11)。开=透视（关迷雾），关=恢复迷雾。"""
        if not self.flags_base:
            return False, '尚未注入（请先附加游戏）', None
        if 'Map' not in self._installed_names():
            return False, '未安装 Map hook（观战模式未装全地图，请用正常模式）', None
        cur = self.get_flag_byte(0x11)
        new = 0 if cur else 1
        if not self.set_flag_byte(0x11, new):
            return False, '写入迷雾 flag 失败', None
        if new:
            return True, '已关闭战争迷雾（全地图开启）', True
        return True, '已恢复战争迷雾（全地图关闭）', False

    def pulse_protocol_ready(self, hold_sec=1.0):
        """协议/超武就绪：短暂打开 SuperPower flag(0x0D)，让己方冷却节点走就绪路径。

        不长期占用「超级武器」开关；若该开关本来就是开的，结束后保持开启。
        """
        if not self.flags_base:
            return False, '尚未注入（请先附加游戏）'
        if 'SuperPower' not in self._installed_names():
            return False, '未安装 SuperPower hook（请先附加游戏）'
        prev = self.get_flag_byte(0x0D)
        self.set_flag_byte(0x0D, 1)
        time.sleep(max(0.2, float(hold_sec)))
        if not prev:
            self.set_flag_byte(0x0D, 0)
        return True, '已脉冲协议/超武就绪约 %.1f 秒（己方冷却节点）' % hold_sec

    def pulse_unit_skill_ready(self, hold_sec=1.2):
        """单位技能无冷却：与协议就绪共用 SuperPower hook（己方冷却节点就绪路径）。

        Steam 版无单独的「单位技能」hook；SuperPower 对己方归属的冷却槽生效，
        单位特殊技能与顶部协议走同一类节点。不长期占用「超级武器」开关。
        """
        ok, msg = self.pulse_protocol_ready(hold_sec=hold_sec)
        if not ok:
            return ok, msg
        return True, ('已脉冲单位技能/协议就绪约 %.1f 秒'
                      '（与「协议无冷却」同 hook，己方冷却节点）' % hold_sec)

    def pulse_disable_protocol(self, hold_sec=2.0):
        """禁用敌方协议：短暂打开 DisableAllSP flag(0x0E)。

        无独立的「仅协议」偏移；效果近似「禁用超武」脉冲——拖敌方超武/协议冷却。
        不长期占用「禁用超武」开关。
        """
        if not self.flags_base:
            return False, '尚未注入（请先附加游戏）'
        if 'DisableAllSP' not in self._installed_names():
            return False, '未安装 DisableAllSP hook（请先附加游戏；观战模式可用）'
        prev = self.get_flag_byte(0x0E)
        self.set_flag_byte(0x0E, 1)
        time.sleep(max(0.2, float(hold_sec)))
        if not prev:
            self.set_flag_byte(0x0E, 0)
        return True, ('已脉冲禁用敌方超武/协议约 %.1f 秒'
                      '（近似「禁用超武」，非独立协议接口）' % hold_sec)

    def spawn_as_mine(self, copies=1):
        """按选中模板在鼠标处生成，归属强制为本地玩家（「复制到己方」）。

        与「复制选中」的差别：观战模式下复制选中跟原阵营，本功能始终尝试己方归属。
        无兵种面板 / 类型 ID 表，只能从当前选中单位取模板。
        """
        ok, msg = self.clone_selected(as_mine=True, copies=max(1, int(copies)))
        if not ok:
            return ok, '【复制到己方】' + msg
        return True, '【复制到己方】' + msg

    def apply_damage_mult(self, mult=5.0):
        """选中单位伤害/属性近似倍率：写星级加成对象 sub+0x08。

        已知偏移（README §6.2）：实体+0x3CC → tracker → +0x2C=sub，sub+0x08=当前加成倍率。
        无独立「武器伤害」字段；升星或引擎重算加成时可能被覆盖。
        """
        ents = self.selected_entities()
        if not ents:
            return False, '没读到选中单位（请先选中）'
        target = float(mult)
        if target <= 0:
            return False, '倍率必须 > 0'
        n = 0
        detail = []
        for ent in ents:
            tracker = self.read_u32(ent + 0x3CC)
            if not self.is_ptr(tracker):
                detail.append('0x%08X 无星级组件' % ent)
                continue
            sub = self.read_u32(tracker + 0x2C)
            if not self.is_ptr(sub):
                detail.append('0x%08X 无加成对象' % ent)
                continue
            if self.write_f32(sub + 0x08, target):
                n += 1
                detail.append('0x%08X×%.1f' % (ent, target))
            else:
                detail.append('0x%08X 写入失败' % ent)
        if n == 0:
            return False, '未能写入伤害倍率：' + ('；'.join(detail) if detail else '无可用组件')
        shown = '、'.join(detail[:6])
        extra = ('…共 %d 个' % n) if n > 6 else ''
        return True, ('已将 %d 个单位星级加成倍率设为 ×%.1f（近似伤害/属性；'
                      '升星后可能被重算覆盖）：%s%s' % (n, target, shown, extra))

    def read_level(self, tracker):
        """读跟踪器的 +0x24（当前等级序号）。"""
        v = self.read_u32(tracker + 0x24)
        return v

    def rank_up_via_engine(self, xp=200000, max_calls=8):
        """在游戏进程的**独立线程**里反复调用官方的「给单位加经验」接口。

        为什么不去 hook 里调：0x781EB0 会抢一把全局锁，而 hook 所在的
        「每帧玩家更新」函数是被持锁调用的，同线程再抢会自死锁 → 游戏卡死。
        独立线程抢锁最多只是等待，不会把游戏卡死（实测有效）。

        为什么只调 0x5173F0：实测「单独调用 0x71B290(tracker,1) 重算星级」
        会把游戏直接弄挂（绕过官方入口后等级链状态不一致）。
        0x5173F0 是官方入口，它会自己完成取锁 / 逐级晋升 / 播特效 / 刷新 UI。

        实测：一次调用可以跨多级（给 5000 经验就把等级从 1 直接推到 4 的顶），
        所以循环调用只是为了保证「一次点到封顶」，直到 +0x24 不再变化为止。
        返回 (ok, 消息)。
        """
        ents = self.selected_entities()
        if not ents:
            mgr = self.read_u32(self.mgr_addr())
            return False, ('没读到选中单位。诊断：管理器指针 [0x%08X] = 0x%s'
                           '（先在游戏里选中单位；若仍失败请点一次「附加游戏」）'
                           % (self.mgr_addr(), ('%08X' % mgr) if mgr else '读不到'))
        ok_cnt = 0
        rose_cnt = 0
        detail = []
        for ent in ents:
            tracker = self.read_u32(ent + 0x3CC)
            if not self.is_ptr(tracker):
                detail.append('0x%08X 无星级组件' % ent)
                continue
            start = self.read_level(tracker)
            prev = start
            calls = 0
            rose = 0
            for _ in range(max_calls):
                ok, err = self.call_remote(self.va_of(FN_ADD_XP),
                                           args=(ent, xp), timeout=4000)
                if not ok:
                    detail.append('0x%08X 调用失败(%s)' % (ent, err))
                    break
                calls += 1
                now = self.read_level(tracker)
                if now is None:
                    break
                if prev is not None and now <= prev:
                    break       # 等级不再上升 → 已到该单位等级链的顶
                rose += 1
                prev = now
            end = self.read_level(tracker)
            if calls == 0:
                detail.append('0x%08X 未能调用' % ent)
                continue
            ok_cnt += 1
            detail.append('0x%08X 等级 %s→%s（%d 次调用）'
                          % (ent, start, end, calls))
            rose_cnt += 1 if rose else 0
        if ok_cnt == 0:
            return False, '晋升失败：' + '；'.join(detail)
        if rose_cnt == 0:
            return False, ('调用成功但等级没有变化 —— 该单位可能已经是最高等级，'
                           '或它被禁止获得经验（[[跟踪器+4]+0x14]+0x245 == 0）：'
                           + '；'.join(detail))
        return True, '晋升完成：' + '；'.join(detail)

    def detach_and_restore(self):
        if self.handle and self.hooked:
            for name, va, aob in self.installed_hooks:
                self.write_code(self.va_of(va), bytes.fromhex(aob))
            self.installed_hooks = []
            self.hooked = False
        self.auto_spectator = False
        self._spectator_cached = None
        self.detach()


# ============================================================
# 功能定义
# ============================================================
# 每项: (唯一键, 中文名, 类型, 参数)
# 类型: toggle=开关(flag字节) / cmd=一次性命令(FLAGS+20) / danger=危险等级三态
FEATURES = [
    # 资源类
    ('money',    '金钱+10万',       'pulse',  dict(flag=0x08)),
    ('power',    '电力无限',        'toggle', dict(flag=0x09)),
    ('scpoint',  '科技点无限',      'toggle', dict(flag=0x0A)),
    ('haveallsc','全科技',          'toggle', dict(flag=0x0B)),
    ('fastbuild','快速建造',        'toggle', dict(flag=0x0C)),
    ('oremine',  '恢复矿场',        'pulse',  dict(flag=0x14)),
    # 超武 / 建造 / 地图
    ('superpower',   '超级武器',       'toggle', dict(flag=0x0D)),
    ('disableallsp', '禁用超武',       'toggle', dict(flag=0x0E)),
    ('map',          '全地图',         'toggle', dict(flag=0x11)),
    ('nocbuild',     '敌人无法建造',   'toggle', dict(flag=0x15)),
    # 弹药 / 危险等级
    ('ammo',     '弹药无限',     'toggle', dict(flag=0x12)),
    ('danger',   '危险等级',     'danger', dict(flag=0x13)),
    # 单位速度/血量：改由修改器直接写内存（engine），观战模式无 Money hook 也能用
    ('speed_max',    '超速 ×500',   'engine', dict(action='speed', mode='max')),
    ('speed_slow',   '慢速 ×10',    'engine', dict(action='speed', mode='slow')),
    ('speed_freeze', '冻结',        'engine', dict(action='speed', mode='freeze')),
    ('speed_restore','恢复速度',    'engine', dict(action='speed', mode='restore')),
    ('hp_max',       '无敌',        'engine', dict(action='hp', mode='max')),
    ('hp_min',       '残血(1点)',   'engine', dict(action='hp', mode='min')),
    ('hp_normal',    '恢复血量',    'engine', dict(action='hp', mode='normal')),
    ('unit_kill',    '摧毁选中',   'engine', dict(action='kill')),
    ('unit_clone',   '复制选中',   'engine', dict(action='clone')),
    ('clone_multi',  '批量复制',   'engine', dict(action='clone_multi', copies=5)),
    ('convert_unit', '收编敌方',   'engine', dict(action='convert')),
    ('full_buff',    '一键满状态', 'engine', dict(action='full_buff')),
    ('enemy_weaken', '敌方残血',   'engine', dict(action='hp_relation', mode='min', relation='enemy')),
    ('ally_god',     '友军无敌',   'engine', dict(action='hp_relation', mode='max', relation='ally')),
    ('chaos_mode',   '混乱模式',   'engine', dict(action='chaos')),
    ('ore_convoy',   '刷矿车车队', 'engine', dict(action='ore_convoy', copies=8)),
    ('fog_toggle',   '迷雾开关',   'engine', dict(action='fog_toggle')),
    ('protocol_ready', '协议无冷却', 'engine', dict(action='protocol_ready', hold_sec=1.0)),
    ('unit_skill_ready', '单位技能无冷却', 'engine',
     dict(action='unit_skill_ready', hold_sec=1.2)),
    ('disable_protocol', '禁用敌方协议', 'engine',
     dict(action='disable_protocol', hold_sec=2.0)),
    ('spawn_unit',   '复制到己方', 'engine', dict(action='spawn_as_mine')),
    ('damage_mult',  '伤害×5',     'engine', dict(action='damage_mult', mult=5.0)),
    ('spec_gift',    '观战赠送单位', 'engine', dict(action='spec_gift')),
    # 星级（满级 3 星）：
    #   实体+0x3CC 是 ExperienceTrackerObject（经验追踪器），它的 +0x24 只是
    #   「当前等级」的缓存 —— 纯粹写内存改它，游戏里一点变化都没有（实测）。
    #   实测结论（验证星级机制脚本）：
    #     · 单独调 0x71B290(tracker,1)「重算星级」→ 游戏直接挂掉，绝不能用；
    #     · 调官方入口 0x5173F0(实体, 经验) → 单位真的升级了（+0x24 上升、
    #        +0x20 特效标志置 1、等级定义键切换），而且不卡死；
    #     · 但一次调用就可能跨多级（实测 5000 经验把等级从 1 直接推到 4），
    #        所以循环调用只是为了确保一次点到该单位的等级上限（实测上限为 4）。
    #   注意：这个调用必须跑在游戏进程的**独立线程**里（修改器侧 CreateRemoteThread），
    #   在 hook 里同步调会因为抢同一把全局锁而自死锁（上一版的卡死原因）。
    ('unit_rank',    '满级(3星)',   'engine', dict(xp=200000, max_calls=8)),
]

# 规划中的功能：仅 GUI 占位（灰色不可点），逻辑未实现。
# 类型固定为 planned；实现时改类型并挪进 FEATURES，同时接上 handler。
# 无已知偏移 / 无可行近似路径的项已移除（勿再加半残按钮）。
PLANNED_FEATURES = [
]

# 热键: (功能键, 修饰键VK列表, 主键VK)
# VK_OEM_7=' / VK_OEM_1=; / VK_OEM_COMMA=, / VK_OEM_PERIOD=. / VK_OEM_2=/
HOTKEYS = {
    'money':        ([], 0x70),   # Ctrl+F1 -> 修饰键在 GUI 层处理
    'power':        ([], 0x71),
    'scpoint':      ([], 0x72),
    'haveallsc':    ([], 0x73),
    'fastbuild':    ([], 0x74),
    'superpower':   ([], 0x75),
    'disableallsp': ([], 0x76),
    'map':          ([], 0x78),   # F9
    'nocbuild':     ([], 0x79),   # F10
    'ammo':         ([], 0xBA),   # ;
    'oremine':      ([], 0xDE),   # '
    'danger_max':   ([], 0xBC),   # ,
    'danger_min':   ([], 0xBE),   # .  （原先误写成 0x2E=Delete）
    'danger_norm':  ([], 0xBF),   # /
    # 单位操作（需先选中单位）
    'speed_max':    ([], 0xBD),   # -
    'speed_slow':   ([], 0xBB),   # =
    'speed_freeze': ([], 0x21),   # PageUp
    'speed_restore':([], 0x22),   # PageDown
    'hp_max':       ([], 0xDB),   # [
    'hp_min':       ([], 0xDD),   # ]
    'hp_normal':    ([], 0xDC),   # \
    'unit_rank':    ([], 0x50),   # p
    'unit_kill':    ([], 0x2E),   # Delete
    'unit_clone':   ([], 0x49),   # I
}

CTRL_VK = 0x11

# 运行模式：正常=装全部 hook；观战=不打补丁（只分配内存，避免资源 hook 闪退）
PLAY_MODES = [
    ('normal', '正常模式'),
    ('spectate', '观战模式'),
]
PLAY_MODE_LABELS = {k: v for k, v in PLAY_MODES}

# 虚拟键码 -> 可读按键名（用于 GUI 悬停提示）
VK_NAMES = {
    0x08: 'Backspace', 0x09: 'Tab', 0x0D: 'Enter', 0x1B: 'Esc', 0x20: '空格',
    0x21: 'PageUp', 0x22: 'PageDown', 0x23: 'End', 0x24: 'Home',
    0x25: '←', 0x26: '↑', 0x27: '→', 0x28: '↓',
    0x2D: 'Insert', 0x2E: 'Delete',
    0xBA: ';', 0xBB: '=', 0xBC: ',', 0xBD: '-', 0xBE: '.', 0xBF: '/',
    0xC0: '`', 0xDB: '[', 0xDC: '\\', 0xDD: ']', 0xDE: "'",
}


def vk_name(vk):
    """虚拟键码 -> 可读按键名。"""
    if 0x70 <= vk <= 0x7B:
        return 'F%d' % (vk - 0x70 + 1)
    if 0x30 <= vk <= 0x39 or 0x41 <= vk <= 0x5A:
        return chr(vk)
    return VK_NAMES.get(vk, 'VK_%02X' % vk)


def hotkey_text(key):
    """某功能的快捷键显示文本，如 'Ctrl+F1'、'-'；无则返回 None。"""
    entry = HOTKEYS.get(key)
    if not entry:
        return None
    _mods, vk = entry
    return ('Ctrl+' if key in CTRL_REQUIRED else '') + vk_name(vk)


# 悬停提示的补充说明（第二行）
FEATURE_HINTS = {
    'money':         '每点击一次 +10 万金钱',
    'speed_max':     '选中单位速度 ×500',
    'speed_slow':    '选中单位速度 ×10',
    'speed_freeze':  '选中单位停止移动',
    'speed_restore': '恢复选中单位的原始速度',
    'hp_max':        '选中单位血量拉满，等效无敌',
    'hp_min':        '选中单位血量降到 1 点',
    'hp_normal':     '恢复选中单位的正常血量',
    'unit_rank':     '选中单位升到该单位等级的顶（3 星 / 英雄级）。'
                     '走游戏官方的「加经验」接口，所以星星图标、数值加成、'
                     'EVA 语音都会自己出来；一次调用可能跨多级，'
                     '这里会自动反复调用直到封顶',
    'unit_kill':     '调用官方摧毁接口拆掉选中单位（观战/正常都可用）',
    'unit_clone':    '复制选中单位到鼠标附近（自动错开落点）：正常模式归属自己，观战模式归属原阵营',
    'clone_multi':   '每个选中模板在鼠标附近螺旋错开各复制 5 份（串行执行，与「复制选中」相同归属规则）',
    'convert_unit':  '把选中单位的归属改为本地玩家（写实体+0x418，同原版收编逻辑）',
    'full_buff':     '选中单位满血 + 短暂触发弹药 hook 装填 + 官方接口尽量满星',
    'enemy_weaken':  '当前选中里归属非己方的单位血量设为 1（无地图全体遍历）',
    'ally_god':      '当前选中里归属己方的单位血量拉满（无地图全体遍历）',
    'disableallsp':  '禁用敌方超级武器；己方单位技能/协议不受影响',
    'chaos_mode':    '对每个选中单位随机改速度档位（超速/慢速/冻结/恢复）和/或血量',
    'ore_convoy':    '选中矿车（或任意单位）后，在鼠标落点各复制 8 份（无矿车类型 ID，批量复制变体）',
    'fog_toggle':    '翻转「全地图」flag：开=关闭战争迷雾，再点一次恢复迷雾',
    'protocol_ready': '短暂打开己方超武/协议就绪 hook 约 1 秒（不长期占用「超级武器」开关）',
    'unit_skill_ready': '短暂打开己方冷却就绪 hook 约 1.2 秒（与「协议无冷却」同路径；'
                        '无独立单位技能 hook）',
    'disable_protocol': '短暂打开「禁用超武」flag 约 2 秒（近似禁用敌方协议/超武；'
                        '无独立协议接口，不长期占用开关）',
    'spawn_unit':    '按选中模板在鼠标处生成，归属强制本地玩家（观战下「复制选中」跟原阵营，'
                     '本键始终尝试己方；无兵种面板）',
    'damage_mult':   '把选中单位星级加成倍率(sub+0x08)写成 ×5（近似伤害/属性；'
                     '升星后可能被引擎重算覆盖）',
    'spec_gift':     '按选中单位模板在鼠标处生成，归属始终为原阵营（观战刷兵 / 给电脑送礼）',
}

FEATURE_BY_KEY = {f[0]: f for f in FEATURES + PLANNED_FEATURES}

# 功能分组（用于 GUI 分区展示，顺序即显示顺序）
# 已实现与规划中可混排；planned 类型按钮自动变灰不可点。
FEATURE_GROUPS = [
    ('资源', [
        'money', 'power', 'scpoint', 'haveallsc', 'fastbuild', 'oremine',
    ]),
    ('超武 / 地图', [
        'superpower', 'disableallsp', 'map', 'nocbuild',
        'protocol_ready', 'unit_skill_ready', 'disable_protocol',
    ]),
    ('弹药 / 危险', ['ammo', 'danger']),
    ('单位操作', [
        'speed_max', 'speed_slow', 'speed_freeze', 'speed_restore',
        'hp_max', 'hp_min', 'hp_normal', 'unit_rank', 'unit_kill', 'unit_clone',
        'convert_unit', 'spawn_unit', 'clone_multi',
        'damage_mult', 'full_buff',
    ]),
    ('战场', [
        'enemy_weaken', 'ally_god',
    ]),
    ('情报', ['fog_toggle']),
    ('观战', ['spec_gift']),
    ('趣味', ['chaos_mode', 'ore_convoy']),
]

# 侧栏选中后，右侧标题下的补充说明（可选）
GROUP_SUBTITLES = {
    '单位操作': '需先在游戏里选中单位',
    '观战': '建议使用观战模式附加',
    '趣味': '娱乐向，优先级较低',
}

# F1-F12 需要 Ctrl；其它单键不需修饰
CTRL_REQUIRED = {'money', 'power', 'scpoint', 'haveallsc', 'fastbuild',
                 'superpower', 'disableallsp', 'map', 'nocbuild'}


# ============================================================
# GUI 悬停提示
# ============================================================
class ToolTip:
    """轻量悬停提示（无边框浮层）。

    - 悬停 delay_ms 毫秒后弹出，移开或点击立即消失
    - 默认显示在控件正下方，贴近屏幕边缘时自动翻转/收边，避免被裁掉
    - 不抢焦点、不进入任务栏
    """

    def __init__(self, widget, text, delay_ms=320, bg='#1f2937', fg='#f8fafc'):
        self.widget = widget
        self.text = text
        self.delay_ms = delay_ms
        self.bg = bg
        self.fg = fg
        self._after = None
        self._tip = None
        widget.bind('<Enter>', self._on_enter, add='+')
        widget.bind('<Leave>', self._on_leave, add='+')
        widget.bind('<ButtonPress>', self._on_leave, add='+')

    def _on_enter(self, _e=None):
        self._cancel()
        try:
            self._after = self.widget.after(self.delay_ms, self._show)
        except Exception:
            self._after = None

    def _on_leave(self, _e=None):
        self._cancel()
        self._hide()

    def _cancel(self):
        if self._after is not None:
            try:
                self.widget.after_cancel(self._after)
            except Exception:
                pass
            self._after = None

    def _show(self):
        self._after = None
        if self._tip is not None or not self.text:
            return
        try:
            import tkinter as tk
            tip = tk.Toplevel(self.widget)
            tip.wm_overrideredirect(True)
            try:
                tip.attributes('-topmost', True)
            except Exception:
                pass
            frame = tk.Frame(tip, bg=self.bg, highlightthickness=1,
                             highlightbackground='#94a3b8')
            frame.pack()
            tk.Label(frame, text=self.text, justify='left', anchor='w',
                     bg=self.bg, fg=self.fg, bd=0, padx=10, pady=7,
                     font=('Microsoft YaHei UI', 8)).pack()
            tip.update_idletasks()
            w, h = tip.winfo_width(), tip.winfo_height()
            sw, sh = tip.winfo_screenwidth(), tip.winfo_screenheight()
            wx = self.widget.winfo_rootx()
            wy = self.widget.winfo_rooty()
            ww = self.widget.winfo_width()
            wh = self.widget.winfo_height()
            # 居中贴在该控件下方
            x = wx + max(0, (ww - w) // 2)
            y = wy + wh + 5
            if x + w + 6 > sw:
                x = sw - w - 6
            if x < 6:
                x = 6
            if y + h + 6 > sh:      # 下方放不下 -> 翻到上方
                y = wy - h - 5
            tip.wm_geometry('+%d+%d' % (x, y))
            self._tip = tip
        except Exception:
            self._tip = None

    def _hide(self):
        if self._tip is not None:
            try:
                self._tip.destroy()
            except Exception:
                pass
            self._tip = None


def add_tooltip(widget, lines, colors=None):
    """把若干行文本组装成悬停提示；全为空时不创建。"""
    text = '\n'.join(x for x in lines if x)
    if not text:
        return None
    kw = {}
    if colors:
        kw['bg'] = colors.get('tip_bg', '#1f2937')
        kw['fg'] = colors.get('tip_fg', '#f8fafc')
    return ToolTip(widget, text, **kw)


# ============================================================
# GUI + 热键
# ============================================================
class TrainerApp:
    def __init__(self, root):
        import tkinter as tk
        from tkinter import ttk
        self.tk = tk
        self.ttk = ttk
        self.root = root
        self.gp = GameProcess()
        self.attached = False
        self.state = {}          # 功能键 -> 开关状态(bool)
        self.btn_widgets = {}    # 功能键 -> 按钮
        self.var_widgets = {}    # （保留兼容，toggle 已改用按钮式）
        self.ui_queue = queue.Queue()  # 热键线程 -> 主线程 UI 更新队列
        self._engine_busy = False     # 单位操作串行，防连按并发崩游戏

        root.title('红警3 Steam 修改器')
        root.configure(bg='#e8ecf2')

        # 配色方案（默认浅色 · 红警红点缀）
        self.colors = {
            'bg': '#e8ecf2',            # 窗口底（冷灰蓝，非奶油白）
            'panel': '#ffffff',         # 面板
            'card': '#f7f9fc',          # 功能格
            'card_hover': '#eef2f8',    # 悬停
            'fg': '#1e293b',            # 主文字
            'dim': '#64748b',           # 次级文字
            'muted': '#94a3b8',         # 规划中文字
            'planned_bg': '#eef1f6',    # 规划中底
            'primary': '#c62828',       # 红警红
            'primary_dark': '#9b1c1c',
            'primary_soft': '#fdecea',  # 浅红底（导航未选悬停等）
            'green': '#2e7d32',         # 开启 / 成功
            'orange': '#e65100',        # 警告 / 脱离
            'border': '#d0d7e2',
            'nav_idle': '#ffffff',
            'tip_bg': '#1e293b',
            'tip_fg': '#f8fafc',
            'on_fg': '#ffffff',
        }
        self.c = self.colors

        self.status_var = tk.StringVar(value='未附加：请先启动游戏，再点击「附加游戏」')
        self.play_mode = tk.StringVar(value='normal')
        self._build_ui()

        # 默认固定一屏：左侧导航 + 右侧当前分组，无需整页滚动。
        root.update_idletasks()
        self._win_w = 740
        self._win_h = 540
        root.geometry('%dx%d' % (self._win_w, self._win_h))
        root.minsize(680, 480)
        root.resizable(True, True)

        # 热键轮询线程
        self.running = True
        self.hotkey_thread = threading.Thread(target=self._hotkey_loop, daemon=True)
        self.hotkey_thread.start()

        # 定时刷新状态 + 处理 UI 队列
        self._refresh_status()
        self._poll_ui_queue()

    def _feature_tip(self, key, extra=None):
        """组装某功能的悬停提示行：快捷键 + 补充说明。"""
        hk = hotkey_text(key)
        return [
            ('快捷键  ' + hk) if hk else '无全局快捷键',
            FEATURE_HINTS.get(key),
            extra,
        ]

    def _tip(self, widget, lines):
        return add_tooltip(widget, lines, colors=self.c)

    def _panel(self, parent, **pack_kw):
        """带细边框的白色面板。"""
        tk = self.tk
        c = self.c
        fr = tk.Frame(parent, bg=c['panel'],
                      highlightthickness=1, highlightbackground=c['border'],
                      highlightcolor=c['border'])
        if pack_kw:
            fr.pack(**pack_kw)
        return fr

    def _build_ui(self):
        tk = self.tk
        root = self.root
        c = self.c
        font = ('Microsoft YaHei UI', 9)
        font_title = ('Microsoft YaHei UI', 16, 'bold')
        font_sub = ('Microsoft YaHei UI', 8)
        font_group = ('Microsoft YaHei UI', 11, 'bold')
        font_small = ('Microsoft YaHei UI', 8)
        font_badge = ('Microsoft YaHei UI', 7)
        font_nav = ('Microsoft YaHei UI', 9)

        # ===== 顶部标题 =====
        header = tk.Frame(root, bg=c['bg'])
        header.pack(fill='x', padx=16, pady=(14, 4))
        title_row = tk.Frame(header, bg=c['bg'])
        title_row.pack(anchor='w', fill='x')
        tk.Label(title_row, text='红警3 Steam 修改器', font=font_title,
                 bg=c['bg'], fg=c['primary']).pack(side='left')
        badge = tk.Label(title_row, text='[预] 规划中', font=font_badge,
                         bg=c['primary_soft'], fg=c['primary'], padx=6, pady=1)
        badge.pack(side='left', padx=(10, 0), pady=(4, 0))
        tk.Label(header, text='Command & Conquer: Red Alert 3 · 1.12 核心版',
                 font=font_sub, bg=c['bg'], fg=c['dim']).pack(anchor='w', pady=(2, 0))
        # 红色装饰线
        tk.Frame(root, bg=c['primary'], height=3).pack(fill='x', padx=16, pady=(8, 8))

        # ===== 连接栏 =====
        conn = self._panel(root, fill='x', padx=12)
        conn.configure(padx=12, pady=10)
        self.attach_btn = tk.Button(
            conn, text='附加游戏', font=font, width=11,
            bg=c['primary'], fg=c['on_fg'], activebackground=c['primary_dark'],
            activeforeground=c['on_fg'], relief='flat', bd=0, cursor='hand2',
            highlightthickness=0, padx=10, pady=6,
            disabledforeground='#f5c2c0', command=self.do_attach)
        self.attach_btn.pack(side='left')
        self._tip(self.attach_btn, ['附加到游戏进程',
                                    '目标进程：ra3_1.12.game',
                                    '需要管理员权限运行',
                                    '附加前请先选好「正常模式」或「观战模式」'])
        self.detach_btn = tk.Button(
            conn, text='脱离', font=font, width=7,
            bg=c['card'], fg=c['fg'], activebackground=c['card_hover'],
            activeforeground=c['fg'], relief='flat', bd=0, cursor='hand2',
            highlightthickness=1, highlightbackground=c['border'],
            padx=8, pady=5, state='disabled',
            disabledforeground=c['muted'], command=self.do_detach)
        self.detach_btn.pack(side='left', padx=(8, 0))
        self._tip(self.detach_btn, ['脱离游戏并还原全部 17 个 hook',
                                    '退出修改器前建议先点这里'])

        mode_wrap = tk.Frame(conn, bg=c['panel'])
        mode_wrap.pack(side='left', padx=(16, 0))
        tk.Label(mode_wrap, text='模式', font=font_small, bg=c['panel'], fg=c['dim']
                 ).pack(side='left', padx=(0, 6))
        self.mode_btns = {}
        for key, label in PLAY_MODES:
            btn = tk.Button(
                mode_wrap, text=label, font=font_small, width=8,
                relief='flat', bd=0, cursor='hand2', highlightthickness=0,
                padx=8, pady=4,
                command=lambda k=key: self._set_play_mode(k))
            btn.pack(side='left', padx=2)
            self.mode_btns[key] = btn
            tip = {
                'normal': ['正常模式：注入全部 17 个 hook',
                           '适合自己操控的遭遇战 / 战役',
                           '观战进对局会闪退，请改用观战模式',
                           '复制单位归属自己'],
                'spectate': ['观战模式：只装超武禁用等安全 hook',
                             '适合电脑互打时旁观',
                             '不装金钱/电力等资源 hook（防闪退）',
                             '速度/血量/满级/摧毁/复制仍可用',
                             '「禁用超武」= 关掉全场所有阵营超武',
                             '（复制出的单位归属原阵营）'],
            }[key]
            self._tip(btn, tip)
        self._refresh_mode_btns()

        self.status_dot = tk.Label(conn, text='●', font=('Microsoft YaHei UI', 10),
                                   bg=c['panel'], fg=c['dim'])
        self.status_dot.pack(side='left', padx=(16, 4))
        self.status_lbl = tk.Label(conn, textvariable=self.status_var, anchor='w',
                                   font=font_small, bg=c['panel'], fg=c['dim'])
        self.status_lbl.pack(side='left', fill='x', expand=True)

        # ===== 主体：左侧导航 + 右侧分页 =====
        main = tk.Frame(root, bg=c['bg'])
        main.pack(fill='both', expand=True, padx=12, pady=(10, 4))

        nav = self._panel(main)
        nav.configure(width=122, padx=6, pady=8)
        nav.pack(side='left', fill='y')
        nav.pack_propagate(False)
        tk.Label(nav, text='分类', font=font_badge, bg=c['panel'], fg=c['muted']
                 ).pack(anchor='w', padx=8, pady=(2, 8))

        content_shell = self._panel(main)
        content_shell.configure(padx=14, pady=12)
        content_shell.pack(side='left', fill='both', expand=True, padx=(8, 0))

        self._page_title = tk.Label(content_shell, text='', font=font_group,
                                    bg=c['panel'], fg=c['fg'])
        self._page_title.pack(anchor='w')
        self._page_sub = tk.Label(content_shell, text='', font=font_badge,
                                  bg=c['panel'], fg=c['dim'])
        self._page_sub.pack(anchor='w', pady=(2, 10))

        pages_host = tk.Frame(content_shell, bg=c['panel'])
        pages_host.pack(fill='both', expand=True)

        self._nav_btns = {}
        self._pages = {}
        self._group_meta = {}
        self._current_group = 0

        for gi, (gname, keys) in enumerate(FEATURE_GROUPS):
            planned_n = sum(1 for k in keys if FEATURE_BY_KEY[k][2] == 'planned')
            live_n = len(keys) - planned_n
            if planned_n and live_n:
                badge_txt = '已实现 %d · 规划 %d' % (live_n, planned_n)
            elif planned_n:
                badge_txt = '全部规划中'
            else:
                badge_txt = '已实现 · %d 项' % live_n
            sub = GROUP_SUBTITLES.get(gname, '')
            meta_line = badge_txt if not sub else ('%s  ·  %s' % (badge_txt, sub))
            self._group_meta[gi] = (gname, meta_line)

            nav_btn = tk.Button(
                nav, text=gname, font=font_nav, anchor='w',
                relief='flat', bd=0, cursor='hand2', highlightthickness=0,
                padx=12, pady=8,
                command=lambda i=gi: self._show_group(i))
            nav_btn.pack(fill='x', pady=2)
            self._nav_btns[gi] = nav_btn

            page = tk.Frame(pages_host, bg=c['panel'])
            self._pages[gi] = page
            self._fill_feature_grid(page, keys, font, font_small)

        self._show_group(0)

        foot = tk.Label(
            root, font=font_small, bg=c['bg'], fg=c['muted'], justify='left',
            text='热键  Ctrl+F1~F7 / F9~F10 资源与超武    ; , . / \' 弹药·危险·矿场    '
                 '- = PgUp/Dn [ ] \\ p I Del 单位操作')
        foot.pack(fill='x', padx=16, pady=(4, 10))

    def _fill_feature_grid(self, parent, keys, font, font_small):
        """在右侧页面里按 3 列铺功能按钮。"""
        tk = self.tk
        c = self.c
        grid = tk.Frame(parent, bg=c['panel'])
        grid.pack(fill='both', expand=True)
        cols = 3
        for i, key in enumerate(keys):
            _, name, ftype, _params = FEATURE_BY_KEY[key]
            cell = tk.Frame(grid, bg=c['card'], padx=6, pady=5,
                            highlightthickness=1, highlightbackground=c['border'],
                            highlightcolor=c['border'])
            cell.grid(row=i // cols, column=i % cols, sticky='nsew', padx=3, pady=3)
            if ftype == 'planned':
                btn = tk.Button(
                    cell, text='[预] ' + name, font=font, width=13,
                    bg=c['planned_bg'], fg=c['muted'],
                    activebackground=c['planned_bg'], activeforeground=c['muted'],
                    relief='flat', bd=0, cursor='arrow', highlightthickness=0,
                    padx=4, pady=4, state='disabled',
                    disabledforeground=c['muted'])
                btn.pack(padx=2, pady=2)
                self.btn_widgets[key] = btn
                self._tip(btn, self._feature_tip(key, '尚未实现 · 界面预留'))
            elif ftype == 'toggle':
                self.state[key] = False
                btn = tk.Button(
                    cell, text='○ ' + name, font=font, width=13,
                    bg=c['card'], fg=c['fg'], activebackground=c['card_hover'],
                    activeforeground=c['fg'], relief='flat', bd=0, cursor='hand2',
                    highlightthickness=0, padx=4, pady=4,
                    command=lambda k=key: self.toggle_feature(k, not self.state.get(k, False)))
                btn.pack(padx=2, pady=2)
                self.btn_widgets[key] = btn
                self._tip(btn, self._feature_tip(key))
            elif ftype == 'danger':
                tk.Label(cell, text=name, font=font, bg=c['card'], fg=c['fg']
                         ).pack(anchor='w', padx=4, pady=(2, 0))
                sub = tk.Frame(cell, bg=c['card'])
                sub.pack(pady=2)
                self.danger_btns = {}
                self.state['danger'] = 0
                for val, txt in [(1, '最高'), (2, '最低'), (0, '正常')]:
                    btn = tk.Button(
                        sub, text=txt, font=font_small, width=4,
                        bg=c['card'], fg=c['dim'], activebackground=c['card_hover'],
                        activeforeground=c['fg'], relief='flat', bd=0, cursor='hand2',
                        highlightthickness=1, highlightbackground=c['border'], pady=2,
                        command=lambda v=val: self.set_danger(v))
                    btn.pack(side='left', padx=2)
                    self.danger_btns[val] = btn
                    dkey = {1: 'danger_max', 2: 'danger_min', 0: 'danger_norm'}[val]
                    self._tip(btn, self._feature_tip(dkey, '危险等级 → ' + txt))
            else:  # pulse / cmd / engine
                cmd = (lambda k=key: self.do_pulse(k)) if ftype == 'pulse' \
                    else (lambda k=key: self.do_engine(k)) if ftype == 'engine' \
                    else (lambda k=key: self.do_command(k))
                btn = tk.Button(
                    cell, text=name, font=font, width=13,
                    bg=c['card'], fg=c['fg'], activebackground=c['card_hover'],
                    activeforeground=c['fg'], relief='flat', bd=0, cursor='hand2',
                    highlightthickness=0, padx=4, pady=4, command=cmd)
                btn.pack(padx=2, pady=2)
                self.btn_widgets[key] = btn
                self._tip(btn, self._feature_tip(key))
        for col in range(cols):
            grid.columnconfigure(col, weight=1, uniform='cell')

    def _show_group(self, idx):
        """切换左侧分类：只显示对应右侧页面。"""
        if idx not in self._pages:
            return
        self._current_group = idx
        for i, page in self._pages.items():
            if i == idx:
                page.pack(fill='both', expand=True)
            else:
                page.pack_forget()
        gname, meta = self._group_meta[idx]
        self._page_title.config(text=gname)
        self._page_sub.config(text=meta)
        c = self.c
        for i, btn in self._nav_btns.items():
            on = (i == idx)
            btn.config(
                bg=c['primary'] if on else c['nav_idle'],
                fg=c['on_fg'] if on else c['fg'],
                activebackground=c['primary_dark'] if on else c['primary_soft'],
                activeforeground=c['on_fg'] if on else c['primary'],
            )
    # ---- 动作 ----
    def _post(self, fn, *args):
        """跨线程安全地执行 UI 更新：主线程直接执行，否则入队由主线程轮询。"""
        if threading.current_thread() is threading.main_thread():
            fn(*args)
        else:
            self.ui_queue.put((fn, args))

    def _poll_ui_queue(self):
        try:
            while True:
                fn, args = self.ui_queue.get_nowait()
                try:
                    fn(*args)
                except Exception:
                    pass
        except queue.Empty:
            pass
        self.root.after(40, self._poll_ui_queue)

    def log_status(self, text):
        self._post(self._set_status, text)

    def _set_status(self, text):
        """设置状态文字，并根据语义联动颜色（主线程执行）。"""
        self.status_var.set(text)
        if '失败' in text:
            color = self.c['primary']
        elif '已附加' in text:
            color = self.c['green']
        elif '已脱离' in text or '已退出' in text:
            color = self.c['orange']
        elif '开启' in text or '已触发' in text or '已发送' in text:
            color = self.c['green']
        elif '关闭' in text:
            color = self.c['dim']
        else:
            color = self.c['dim']
        self.status_lbl.config(fg=color)
        self.status_dot.config(fg=color)

    # ---- 提示音 ----
    def _beep(self, kind):
        """播放提示音。

        kind:
          'on'    —— 可撤销开关：开启（上行音 do→sol）
          'off'   —— 可撤销开关：关闭（下行音 sol→do）
          'click' —— 一次性操作成功（单位操作 / 脉冲等，无法撤销）
          'error' —— 失败提示（短促低音）
        """
        try:
            import winsound
            if kind == 'on':
                winsound.Beep(523, 70)   # C5
                winsound.Beep(784, 90)   # G5（上行）
            elif kind == 'off':
                winsound.Beep(784, 70)   # G5
                winsound.Beep(523, 90)   # C5（下行）
            elif kind == 'error':
                winsound.Beep(220, 140)  # 低音
            else:  # click
                winsound.Beep(1000, 60)
        except Exception:
            pass

    # ---- 内存操作（线程安全）----
    def _mem_toggle(self, key, on):
        flag = FEATURE_BY_KEY[key][3]['flag']
        self.gp.set_flag_byte(flag, 1 if on else 0)
        self.state[key] = on

    def _mem_danger(self, value):
        self.gp.set_flag_byte(0x13, value)
        self.state['danger'] = value

    def _mem_cmd(self, key):
        self.gp.set_command(FEATURE_BY_KEY[key][3]['cmd'])

    def _mem_pulse(self, key):
        self.gp.set_flag_byte(FEATURE_BY_KEY[key][3]['flag'], 1)

    # ---- UI 更新（仅主线程）----
    def _update_toggle_btn(self, key, on):
        """更新 toggle 按钮外观：●绿色=开，○灰色=关。"""
        btn = self.btn_widgets.get(key)
        if btn is None:
            return
        name = FEATURE_BY_KEY[key][1]
        c = self.c
        if on:
            btn.config(text='● ' + name, bg=c['green'], fg=c['on_fg'],
                       activebackground=c['green'], activeforeground=c['on_fg'])
        else:
            btn.config(text='○ ' + name, bg=c['card'], fg=c['fg'],
                       activebackground=c['card_hover'], activeforeground=c['fg'])

    def _update_danger_btns(self, val):
        """更新危险等级三按钮高亮。"""
        c = self.c
        for v, btn in getattr(self, 'danger_btns', {}).items():
            if v == val:
                btn.config(bg=c['primary'], fg=c['on_fg'], activebackground=c['primary'])
            else:
                btn.config(bg=c['card'], fg=c['dim'], activebackground=c['card_hover'])

    def _ui_checkbox(self, key, val):
        self._update_toggle_btn(key, val)

    def _ui_danger(self, val):
        self._update_danger_btns(val)

    def _set_play_mode(self, key):
        if self.attached:
            return
        self.play_mode.set(key)
        self._refresh_mode_btns()

    def _refresh_mode_btns(self):
        c = self.c
        cur = self.play_mode.get()
        for key, btn in self.mode_btns.items():
            on = (key == cur)
            btn.config(
                bg=c['primary'] if on else c['card'],
                fg=c['on_fg'] if on else c['fg'],
                activebackground=c['primary_dark'] if on else c['card_hover'],
                activeforeground=c['on_fg'] if on else c['fg'],
                state='disabled' if self.attached else 'normal',
                disabledforeground='#f5c2c0' if on else c['muted'],
                highlightthickness=0 if on else 1,
                highlightbackground=c['border'],
            )

    def do_attach(self):
        ok, err = self.gp.attach()
        if not ok:
            self._beep('error')
            self.log_status('附加失败：' + err)
            return
        mode = self.play_mode.get()
        if mode == 'spectate':
            ok, err = self.gp.inject(
                enabled_names=SPECTATE_HOOK_NAMES, auto_spectator=False)
        else:
            ok, err = self.gp.inject(enabled_names=None, auto_spectator=False)
        if not ok:
            self._beep('error')
            self.gp.detach()
            self.log_status('注入失败：' + err)
            return
        self.attached = True
        self.attach_btn.config(state='disabled')
        self.detach_btn.config(state='normal')
        self._refresh_mode_btns()
        self._beep('on')
        mode_name = PLAY_MODE_LABELS.get(mode, mode)
        if mode == 'spectate':
            self.log_status(
                '已附加（%s）：超武禁用 hook + 单位操作可用（模块 0x%X）'
                % (mode_name, self.gp.module_base))
        else:
            self.log_status('已附加（%s）：模块 0x%X  MustCode 0x%X'
                            % (mode_name, self.gp.module_base, self.gp.mc_base))

    def do_detach(self):
        self.gp.detach_and_restore()
        self.attached = False
        self.attach_btn.config(state='normal')
        self.detach_btn.config(state='disabled')
        self._refresh_mode_btns()
        self._beep('off')
        self.log_status('已脱离，hook 已还原')

    def _ensure_attached(self):
        if not self.attached:
            self.log_status('请先附加游戏')
            return False
        return True

    def toggle_feature(self, key, on):
        if not self._ensure_attached():
            # 回滚界面
            self._update_toggle_btn(key, not on)
            return
        self._mem_toggle(key, on)
        self._update_toggle_btn(key, on)
        self._beep('on' if on else 'off')
        self.log_status('%s %s' % (FEATURE_BY_KEY[key][1], '开启' if on else '关闭'))

    def set_danger(self, value):
        if not self._ensure_attached():
            self._update_danger_btns(self.state.get('danger', 0))
            return
        self._mem_danger(value)
        self._update_danger_btns(value)
        self._beep('click')
        self.log_status('危险等级 -> %s' % {1: '最高', 2: '最低', 0: '正常'}[value])

    def do_pulse(self, key):
        if not self._ensure_attached():
            return
        self._mem_pulse(key)
        self._beep('click')
        self.log_status('%s 已触发' % FEATURE_BY_KEY[key][1])

    def do_command(self, key):
        if not self._ensure_attached():
            return
        self._mem_cmd(key)
        self._beep('click')
        self.log_status('%s 已发送（需选中单位）' % FEATURE_BY_KEY[key][1])

    def do_engine(self, key):
        """由修改器直接在游戏进程里执行（晋升 / 摧毁等），不依赖 Money hook。"""
        if not self._ensure_attached():
            return
        if self._engine_busy:
            self._beep('error')
            self.log_status('上一次单位操作还在执行，请稍候再按')
            return
        self._engine_busy = True
        self._post(self.log_status, '%s：执行中…' % FEATURE_BY_KEY[key][1])
        threading.Thread(target=self._engine_worker, args=(key,),
                         daemon=True).start()

    def _engine_worker(self, key):
        params = FEATURE_BY_KEY[key][3]
        action = params.get('action')
        map_on = None
        try:
            if action == 'kill':
                ok, msg = self.gp.kill_selected()
            elif action == 'clone':
                as_mine = self.play_mode.get() != 'spectate'
                ok, msg = self.gp.clone_selected(as_mine=as_mine)
            elif action == 'clone_multi':
                as_mine = self.play_mode.get() != 'spectate'
                copies = int(params.get('copies', 5))
                ok, msg = self.gp.clone_selected(as_mine=as_mine, copies=copies)
            elif action == 'ore_convoy':
                as_mine = self.play_mode.get() != 'spectate'
                copies = int(params.get('copies', 8))
                ok, msg = self.gp.ore_convoy(as_mine=as_mine, copies=copies)
            elif action == 'spec_gift':
                # 始终归属原阵营（观战刷兵 / 给对方送礼）
                ok, msg = self.gp.clone_selected(as_mine=False, copies=1)
            elif action == 'convert':
                ok, msg = self.gp.convert_selected()
            elif action == 'full_buff':
                ok, msg = self.gp.full_buff_selected(
                    xp=int(params.get('xp', 200000)),
                    max_calls=int(params.get('max_calls', 8)))
            elif action == 'hp_relation':
                ok, msg = self.gp.apply_unit_hp_relation(
                    params['mode'], params['relation'])
            elif action == 'speed':
                ok, msg = self.gp.apply_unit_speed(params['mode'])
            elif action == 'hp':
                ok, msg = self.gp.apply_unit_hp(params['mode'])
            elif action == 'chaos':
                ok, msg = self.gp.chaos_selected()
            elif action == 'fog_toggle':
                ok, msg, map_on = self.gp.toggle_fog()
            elif action == 'protocol_ready':
                ok, msg = self.gp.pulse_protocol_ready(
                    hold_sec=float(params.get('hold_sec', 1.0)))
            elif action == 'unit_skill_ready':
                ok, msg = self.gp.pulse_unit_skill_ready(
                    hold_sec=float(params.get('hold_sec', 1.2)))
            elif action == 'disable_protocol':
                ok, msg = self.gp.pulse_disable_protocol(
                    hold_sec=float(params.get('hold_sec', 2.0)))
            elif action == 'spawn_as_mine':
                ok, msg = self.gp.spawn_as_mine(
                    copies=int(params.get('copies', 1)))
            elif action == 'damage_mult':
                ok, msg = self.gp.apply_damage_mult(
                    mult=float(params.get('mult', 5.0)))
            else:
                ok, msg = self.gp.rank_up_via_engine(**{
                    k: v for k, v in params.items() if k != 'action'})
        except Exception as e:
            ok, msg = False, '异常：%s' % e
        finally:
            self._engine_busy = False
        self._post(self.log_status, ('✓ ' if ok else '✗ ') + msg)
        # 迷雾开关与「全地图」共用 flag 0x11，同步按钮外观
        if map_on is not None:
            self._post(self._sync_map_toggle, map_on)
        # 单位操作无法撤销，统一用 click / error，不用开关的 on/off 音
        self._post(self._beep, 'click' if ok else 'error')

    def _sync_map_toggle(self, on):
        """雾开关翻转后，把「全地图」按钮状态对齐到 flag。"""
        self.state['map'] = bool(on)
        self._update_toggle_btn('map', bool(on))

    # ---- 热键 ----
    def _key_down(self, vk):
        return (user32.GetAsyncKeyState(vk) & 0x8000) != 0

    def _hotkey_loop(self):
        pressed = {}
        while self.running:
            try:
                ctrl = self._key_down(CTRL_VK)
                for key, (mods, vk) in HOTKEYS.items():
                    down = self._key_down(vk)
                    was = pressed.get(key, False)
                    need_ctrl = key in CTRL_REQUIRED
                    mods_ok = need_ctrl == ctrl
                    if down and not was and mods_ok:
                        self._trigger_hotkey(key)
                    pressed[key] = down
            except Exception:
                pass
            time.sleep(0.02)

    def _trigger_hotkey(self, key):
        if not self.attached:
            return
        # 危险等级三键单独映射
        if key == 'danger_max':
            self._mem_danger(1)
            self._post(self._ui_danger, 1)
            self._beep('click')
            self._post(self.log_status, '危险等级 -> 最高')
            return
        if key == 'danger_min':
            self._mem_danger(2)
            self._post(self._ui_danger, 2)
            self._beep('click')
            self._post(self.log_status, '危险等级 -> 最低')
            return
        if key == 'danger_norm':
            self._mem_danger(0)
            self._post(self._ui_danger, 0)
            self._beep('click')
            self._post(self.log_status, '危险等级 -> 正常')
            return
        ftype = FEATURE_BY_KEY[key][2]
        if ftype == 'planned':
            return
        if ftype == 'toggle':
            new = not self.state.get(key, False)
            self._mem_toggle(key, new)
            self._post(self._ui_checkbox, key, new)
            self._beep('on' if new else 'off')
            self._post(self.log_status, '%s %s' % (FEATURE_BY_KEY[key][1], '开启' if new else '关闭'))
        elif ftype == 'pulse':
            self._mem_pulse(key)
            self._beep('click')
            self._post(self.log_status, '%s 已触发' % FEATURE_BY_KEY[key][1])
        elif ftype == 'engine':
            self.do_engine(key)
        else:
            self._mem_cmd(key)
            self._beep('click')
            self._post(self.log_status, '%s 已发送（需选中单位）' % FEATURE_BY_KEY[key][1])

    def _refresh_status(self):
        if self.attached and not self.gp.handle:
            self.attached = False
            self.attach_btn.config(state='normal')
            self.detach_btn.config(state='disabled')
            self._refresh_mode_btns()
            self.log_status('游戏已退出')
        self.root.after(2000, self._refresh_status)

    def on_close(self):
        self.running = False
        if self.attached:
            self.gp.detach_and_restore()
        self.root.destroy()


def _resource_dir():
    """返回资源（icon 等）所在目录。

    源码运行时就是脚本所在目录；PyInstaller onefile 打包后是解包临时目录
    `sys._MEIPASS`。两者都要能找到图标。
    """
    base = getattr(sys, '_MEIPASS', None)
    if not base:
        base = os.path.dirname(os.path.abspath(__file__))
    return base


def apply_icon(root):
    """给窗口和任务栏设置图标；失败一律静默忽略。

    图标不是功能，绝不能因为它缺失或平台不支持就让修改器起不来。
    优先用 .ico（`iconbitmap` 在 Windows 上同时影响标题栏和任务栏），
    再用 .png 走 `iconphoto` 兜底（Tk 8.6 支持 PNG）。
    """
    base = _resource_dir()
    ico = os.path.join(base, 'icon.ico')
    png = os.path.join(base, 'icon.png')

    if os.path.exists(ico):
        try:
            root.iconbitmap(default=ico)
        except Exception:
            pass
    if os.path.exists(png):
        try:
            from tkinter import PhotoImage
            ph = PhotoImage(file=png)
            root.iconphoto(True, ph)
            root._icon_ref = ph          # 防 GC 回收导致图标消失
        except Exception:
            pass


def main():
    # 声明 DPI 感知，避免高 DPI（缩放 >100%）下 Tk 字体被拉伸而模糊
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass

    import tkinter as tk
    root = tk.Tk()
    apply_icon(root)
    app = TrainerApp(root)
    root.protocol('WM_DELETE_WINDOW', app.on_close)
    root.mainloop()


if __name__ == '__main__':
    main()

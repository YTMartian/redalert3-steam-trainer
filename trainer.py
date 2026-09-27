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
import os
import queue
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

# 星级相关的游戏函数（VA；诊断/实验用，正常游戏流程只调 FN_ADD_XP）
FN_ADD_XP = 0x005173F0       # __cdecl(entity, int xp)  官方加经验接口（唯一安全）
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
        # 调用桩是共享的：多线程（例如连按两次 p）同时写会把机器码写坏，
        # 所以所有 call_remote 串行化。
        self._call_lock = threading.Lock()

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
    def call_remote(self, addr, this=None, args=(), cleanup=True, timeout=4000):
        """在游戏进程内调用一个 x86 函数。

        addr    目标函数地址（绝对地址）
        this    传入 ecx（__thiscall 的 this），None 表示不管 ecx
        args    参数列表，按 cdecl 顺序（本方法会反向压栈）
        cleanup True 表示按 __cdecl 由调用方清栈（__thiscall 无栈参数时无影响）

        返回 (ok, err)。注意：会短暂冻结游戏，属正常现象。
        """
        with self._call_lock:
            return self._call_remote_locked(addr, this, args, cleanup, timeout)

    def _call_remote_locked(self, addr, this=None, args=(), cleanup=True, timeout=4000):
        code = bytearray(b'\x60')                       # pushad 保存全部寄存器
        if this is not None:
            code += b'\xb9' + struct.pack('<I', this & 0xFFFFFFFF)
        for a in reversed(list(args)):
            code += b'\x68' + struct.pack('<I', a & 0xFFFFFFFF)
        code += b'\xb8' + struct.pack('<I', addr & 0xFFFFFFFF)
        code += b'\xff\xd0'                             # call eax
        if cleanup and args:
            code += b'\x81\xc4' + struct.pack('<I', 4 * len(args))
        code += b'\x61\xc3'                             # popad; ret

        if not getattr(self, '_stub', None):
            self._stub = self.alloc(0x100)
            if not self._stub:
                return False, '分配调用桩内存失败'
        stub = self._stub
        if not self.write(stub, b'\xcc' * 0x100):
            return False, '清理调用桩失败'
        if not self.write(stub, bytes(code)):
            return False, '写入调用桩失败'

        h = kernel32.CreateRemoteThread(
            ctypes.c_void_p(self.handle), None, 0,
            ctypes.c_void_p(stub), None, 0, None)
        if not h:
            return False, 'CreateRemoteThread 失败 (err=%d)' % ctypes.get_last_error()
        waited = kernel32.WaitForSingleObject(ctypes.c_void_p(h), timeout)
        kernel32.CloseHandle(ctypes.c_void_p(h))
        if waited == 0x102:                 # WAIT_TIMEOUT
            # 远端线程可能还在跑：绝不能复用这块桩内存（会被下一次写入踩坏），
            # 直接丢弃，下次调用重新分配。
            self._stub = 0
            return False, '调用超时（>%d ms），已放弃这一次' % timeout
        return True, None

    # ---- 注入 ----
    def inject(self):
        """分配内存、重汇编并写入、补丁 17 个 hook。"""
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
        # 初始化 FLAGS+0x24 数据（WeNeedBack 用，无害）
        self.write(self.flags_base + 0x24, b'\xA0\xA5\x86\x65')

        # 校验基址：读回第一个 hook 处字节，应匹配 AOB（否则说明模块基址/版本不对）
        probe_va = CORE_HOOKS[0][1]
        probe_len = len(CORE_HOOKS[0][2]) // 2
        probe = self.read(self.va_of(probe_va), probe_len)
        expect = bytes.fromhex(CORE_HOOKS[0][2])
        if probe != expect:
            if self.game_module and self.game_module.lower() != 'ra3_1.12.game':
                return False, ('检测到游戏模块为 %s，但本修改器适配 ra3_1.12.game。'
                               '请在 Steam 启动选项里添加 -runver 1.12 后重启游戏'
                               % self.game_module)
            return False, ('基址校验失败：模块 0x%X 地址 0x%X 处读到 %s，期望 %s'
                           '（游戏版本不符或基址错误）'
                           % (self.module_base, self.va_of(probe_va), probe.hex(), expect.hex()))

        # 补丁 17 个 hook
        for name, hook_va_abs, aob, target_off in CORE_HOOKS:
            hook_va = self.va_of(hook_va_abs)
            aob_len = len(aob) // 2
            tag = 'mc_' + format(target_off, 'x')
            label_off = LABELS['MC'].get(tag)
            if label_off is None:
                return False, '缺少标签 %s (%s)' % (tag, name)
            jmp_target = self.mc_base + label_off
            rel = jmp_target - (hook_va + 5)
            if not (-0x80000000 <= rel <= 0x7FFFFFFF):
                return False, '%s hook 跳转距离超出 ±2GB（MC 段分配地址过远）' % name
            patch = b'\xE9' + struct.pack('<i', rel) + b'\x90' * (aob_len - 5)
            if len(patch) != aob_len:
                return False, '%s hook 补丁长度错误' % name
            if not self.write_code(hook_va, patch):
                return False, '%s hook 写入失败' % name

        self.hooked = True
        return True, ''

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
            for name, va, aob, _ in CORE_HOOKS:
                self.write_code(self.va_of(va), bytes.fromhex(aob))
            self.hooked = False
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
    ('disableallsp', '禁用敌方超武',   'toggle', dict(flag=0x0E)),
    ('map',          '全地图',         'toggle', dict(flag=0x11)),
    ('nocbuild',     '敌人无法建造',   'toggle', dict(flag=0x15)),
    # 弹药 / 危险等级
    ('ammo',     '弹药无限',     'toggle', dict(flag=0x12)),
    ('danger',   '危险等级',     'danger', dict(flag=0x13)),
    # 单位速度/血量操作：依赖「选中单位管理器」全局指针。
    # Steam 版该指针已由 0x8DB73C 重定位为 0x8E08DC（经运行时差分扫描确认，
    # 结构不变：+0x5C=选中数量，+0x50=选中单位链表头，节点+8 -> 对象+0x138=单位实体）。
    ('speed_max',    '超速 ×500',   'cmd', dict(cmd=1)),
    ('speed_slow',   '慢速 ×10',    'cmd', dict(cmd=2)),
    ('speed_freeze', '冻结',        'cmd', dict(cmd=3)),
    ('speed_restore','恢复速度',    'cmd', dict(cmd=4)),
    ('hp_max',       '无敌',        'cmd', dict(cmd=5)),
    ('hp_min',       '残血(1点)',   'cmd', dict(cmd=6)),
    ('hp_normal',    '恢复血量',    'cmd', dict(cmd=7)),
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
    'danger_max':   ([], 0x2C),   # ,
    'danger_min':   ([], 0x2E),   # .
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
}

CTRL_VK = 0x11

# 虚拟键码 -> 可读按键名（用于 GUI 悬停提示）
VK_NAMES = {
    0x08: 'Backspace', 0x09: 'Tab', 0x0D: 'Enter', 0x1B: 'Esc', 0x20: '空格',
    0x21: 'PageUp', 0x22: 'PageDown', 0x23: 'End', 0x24: 'Home',
    0x25: '←', 0x26: '↑', 0x27: '→', 0x28: '↓',
    0x2C: ',', 0x2E: '.', 0x2F: '/',
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
}

FEATURE_BY_KEY = {f[0]: f for f in FEATURES}

# 功能分组（用于 GUI 分区展示，顺序即显示顺序）
FEATURE_GROUPS = [
    ('资源', ['money', 'power', 'scpoint', 'haveallsc', 'fastbuild', 'oremine']),
    ('超武 / 建造 / 地图', ['superpower', 'disableallsp', 'map', 'nocbuild']),
    ('弹药 / 危险等级', ['ammo', 'danger']),
    ('单位操作（需先选中单位）', ['speed_max', 'speed_slow', 'speed_freeze', 'speed_restore',
                          'hp_max', 'hp_min', 'hp_normal', 'unit_rank']),
]

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

    def __init__(self, widget, text, delay_ms=320):
        self.widget = widget
        self.text = text
        self.delay_ms = delay_ms
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
            tk.Label(tip, text=self.text, justify='left', anchor='w',
                     bg='#111116', fg='#e9e9ec', bd=0, padx=9, pady=6,
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


def add_tooltip(widget, lines):
    """把若干行文本组装成悬停提示；全为空时不创建。"""
    text = '\n'.join(x for x in lines if x)
    return ToolTip(widget, text) if text else None


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

        root.title('红警3 Steam 修改器')
        root.resizable(False, False)
        root.configure(bg='#1b1b20')

        # 配色方案（红警深色主题）
        self.colors = {
            'bg': '#1b1b20',          # 窗口背景
            'panel': '#232329',       # 面板背景
            'card': '#2b2b33',        # 卡片背景
            'card_hover': '#34343f',  # 卡片悬停
            'fg': '#e9e9ec',          # 主文字
            'dim': '#8f8f9a',         # 次级文字
            'primary': '#e04a3f',     # 红警红
            'primary_dark': '#b7332a',
            'green': '#3fae5a',       # 开启/成功
            'orange': '#e8a23d',      # 警告/脱离
            'border': '#34343e',
        }
        self.c = self.colors

        self.status_var = tk.StringVar(value='未附加：请先启动游戏，再点击「附加游戏」')
        self._build_ui()

        # 固定窗口尺寸：以构建完成后的自然尺寸为准，锁定宽高，
        # 避免状态文字/按钮文本变化导致窗口大小跳动。
        root.update_idletasks()
        self._win_w = max(root.winfo_reqwidth(), 560)
        self._win_h = root.winfo_reqheight()
        root.geometry('%dx%d' % (self._win_w, self._win_h))
        root.minsize(self._win_w, self._win_h)
        root.maxsize(self._win_w, self._win_h)

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

    def _build_ui(self):
        tk = self.tk
        root = self.root
        c = self.c
        font = ('Microsoft YaHei UI', 9)
        font_title = ('Microsoft YaHei UI', 15, 'bold')
        font_sub = ('Microsoft YaHei UI', 8)
        font_group = ('Microsoft YaHei UI', 9, 'bold')
        font_small = ('Microsoft YaHei UI', 8)

        # ===== 顶部标题 =====
        header = tk.Frame(root, bg=c['bg'])
        header.pack(fill='x', padx=16, pady=(14, 8))
        tk.Label(header, text='红警3 Steam 修改器', font=font_title,
                 bg=c['bg'], fg=c['primary']).pack(anchor='w')
        tk.Label(header, text='Command & Conquer: Red Alert 3 · 1.12 核心版',
                 font=font_sub, bg=c['bg'], fg=c['dim']).pack(anchor='w')

        # ===== 连接栏 =====
        conn = tk.Frame(root, bg=c['panel'], padx=10, pady=10)
        conn.pack(fill='x', padx=12)
        self.attach_btn = tk.Button(conn, text='⚡ 附加游戏', font=font, width=12,
                                    bg=c['primary'], fg='#ffffff', activebackground=c['primary_dark'],
                                    activeforeground='#ffffff', relief='flat', bd=0, cursor='hand2',
                                    highlightthickness=0, padx=8, pady=5,
                                    disabledforeground='#d8b9b7', command=self.do_attach)
        self.attach_btn.pack(side='left')
        add_tooltip(self.attach_btn, ['附加到游戏进程',
                                      '目标进程：ra3_1.12.game',
                                      '需要管理员权限运行'])
        self.detach_btn = tk.Button(conn, text='脱离', font=font, width=7,
                                    bg=c['card'], fg=c['fg'], activebackground=c['card_hover'],
                                    activeforeground=c['fg'], relief='flat', bd=0, cursor='hand2',
                                    highlightthickness=0, padx=8, pady=5, state='disabled',
                                    disabledforeground=c['dim'], command=self.do_detach)
        self.detach_btn.pack(side='left', padx=(8, 0))
        add_tooltip(self.detach_btn, ['脱离游戏并还原全部 17 个 hook',
                                      '退出修改器前建议先点这里'])
        self.status_dot = tk.Label(conn, text='●', font=('Microsoft YaHei UI', 10),
                                   bg=c['panel'], fg=c['dim'])
        self.status_dot.pack(side='left', padx=(14, 4))
        self.status_lbl = tk.Label(conn, textvariable=self.status_var, anchor='w',
                                   font=font_small, bg=c['panel'], fg=c['dim'])
        self.status_lbl.pack(side='left', fill='x', expand=True)

        # ===== 功能分组 =====
        body = tk.Frame(root, bg=c['bg'])
        body.pack(fill='both', expand=True, padx=12, pady=(10, 4))

        for gi, (gname, keys) in enumerate(FEATURE_GROUPS):
            gf = tk.Frame(body, bg=c['panel'], padx=8, pady=8)
            gf.pack(fill='x', pady=(0, 8))
            tk.Label(gf, text=gname, font=font_group, bg=c['panel'], fg=c['primary']
                     ).pack(anchor='w', padx=4, pady=(0, 6))
            grid = tk.Frame(gf, bg=c['panel'])
            grid.pack(fill='x')
            cols = 3
            for i, key in enumerate(keys):
                _, name, ftype, _params = FEATURE_BY_KEY[key]
                cell = tk.Frame(grid, bg=c['card'], padx=6, pady=4)
                cell.grid(row=i // cols, column=i % cols, sticky='nsew', padx=3, pady=3)
                if ftype == 'toggle':
                    self.state[key] = False
                    btn = tk.Button(cell, text='○ ' + name, font=font, width=13,
                                    bg=c['card'], fg=c['fg'], activebackground=c['card_hover'],
                                    activeforeground=c['fg'], relief='flat', bd=0, cursor='hand2',
                                    highlightthickness=0, padx=4, pady=3,
                                    command=lambda k=key: self.toggle_feature(k, not self.state.get(k, False)))
                    btn.pack(padx=2, pady=2)
                    self.btn_widgets[key] = btn
                    add_tooltip(btn, self._feature_tip(key))
                elif ftype == 'danger':
                    tk.Label(cell, text=name, font=font, bg=c['card'], fg=c['fg']
                             ).pack(anchor='w', padx=4, pady=(2, 0))
                    sub = tk.Frame(cell, bg=c['card'])
                    sub.pack(pady=2)
                    self.danger_btns = {}
                    self.state['danger'] = 0
                    for val, txt in [(1, '最高'), (2, '最低'), (0, '正常')]:
                        btn = tk.Button(sub, text=txt, font=font_small, width=4,
                                        bg=c['card'], fg=c['dim'], activebackground=c['card_hover'],
                                        activeforeground=c['fg'], relief='flat', bd=0, cursor='hand2',
                                        highlightthickness=0, pady=2,
                                        command=lambda v=val: self.set_danger(v))
                        btn.pack(side='left', padx=2)
                        self.danger_btns[val] = btn
                        dkey = {1: 'danger_max', 2: 'danger_min', 0: 'danger_norm'}[val]
                        add_tooltip(btn, self._feature_tip(dkey, '危险等级 → ' + txt))
                else:  # pulse / cmd / engine
                    cmd = (lambda k=key: self.do_pulse(k)) if ftype == 'pulse' \
                        else (lambda k=key: self.do_engine(k)) if ftype == 'engine' \
                        else (lambda k=key: self.do_command(k))
                    btn = tk.Button(cell, text=name, font=font, width=13,
                                    bg=c['card'], fg=c['fg'], activebackground=c['card_hover'],
                                    activeforeground=c['fg'], relief='flat', bd=0, cursor='hand2',
                                    highlightthickness=0, padx=4, pady=3, command=cmd)
                    btn.pack(padx=2, pady=2)
                    self.btn_widgets[key] = btn
                    add_tooltip(btn, self._feature_tip(key))
            for col in range(cols):
                grid.columnconfigure(col, weight=1, uniform='cell')

        # ===== 底部热键提示 =====
        foot = tk.Label(root, font=font_small, bg=c['bg'], fg=c['dim'], justify='left',
                        text='热键  Ctrl+F1 金钱  F2 电力  F3 科技点  F4 全科技  F5 快速建造\n'
                             'Ctrl+F6 超级武器  F7 禁用超武  F9 全图  F10 敌不可建\n'
                             '; 弹药  , 危险高  . 危险低  / 危险正常  \' 恢复矿场')
        foot.pack(fill='x', padx=16, pady=(2, 10))

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
          'on'    —— 开启 / 勾选（上行音 do→sol）
          'off'   —— 取消 / 关闭（下行音 sol→do）
          'click' —— 普通单次触发按钮
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
        if on:
            btn.config(text='● ' + name, bg=self.c['green'], fg='#ffffff',
                       activebackground=self.c['green'], activeforeground='#ffffff')
        else:
            btn.config(text='○ ' + name, bg=self.c['card'], fg=self.c['fg'],
                       activebackground=self.c['card_hover'], activeforeground=self.c['fg'])

    def _update_danger_btns(self, val):
        """更新危险等级三按钮高亮。"""
        for v, btn in getattr(self, 'danger_btns', {}).items():
            if v == val:
                btn.config(bg=self.c['primary'], fg='#ffffff', activebackground=self.c['primary'])
            else:
                btn.config(bg=self.c['card'], fg=self.c['dim'], activebackground=self.c['card_hover'])

    def _ui_checkbox(self, key, val):
        self._update_toggle_btn(key, val)

    def _ui_danger(self, val):
        self._update_danger_btns(val)

    def do_attach(self):
        ok, err = self.gp.attach()
        if not ok:
            self._beep('error')
            self.log_status('附加失败：' + err)
            return
        ok, err = self.gp.inject()
        if not ok:
            self._beep('error')
            self.gp.detach()
            self.log_status('注入失败：' + err)
            return
        self.attached = True
        self.attach_btn.config(state='disabled')
        self.detach_btn.config(state='normal')
        self._beep('on')
        self.log_status('已附加：模块 0x%X  MustCode 0x%X' % (self.gp.module_base, self.gp.mc_base))

    def do_detach(self):
        self.gp.detach_and_restore()
        self.attached = False
        self.attach_btn.config(state='normal')
        self.detach_btn.config(state='disabled')
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
        """由修改器直接在游戏进程的独立线程里调引擎函数（见 rank_up_via_engine）。

        调用可能要花上几百毫秒到几秒，所以放到工作线程里跑，
        免得卡住热键轮询线程和界面。
        """
        if not self._ensure_attached():
            return
        self._beep('click')
        self._post(self.log_status, '%s：正在调用官方晋升接口…'
                   % FEATURE_BY_KEY[key][1])
        threading.Thread(target=self._engine_worker, args=(key,),
                         daemon=True).start()

    def _engine_worker(self, key):
        try:
            ok, msg = self.gp.rank_up_via_engine(**FEATURE_BY_KEY[key][3])
        except Exception as e:
            ok, msg = False, '异常：%s' % e
        self._post(self.log_status, ('✓ ' if ok else '✗ ') + msg)
        self._post(self._beep, 'on' if ok else 'off')

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

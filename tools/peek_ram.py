"""Read (read-only) the emulated PS1 RAM out of a running psxvr.exe and save it to a file.

Locates the 2MB RAM buffer by searching for the game's executable code (loaded at 0x80010000).
usage: peek_ram.py <exe file> <out.bin>
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                ('AllocationProtect', wt.DWORD), ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_size_t),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


def find_pid(name='psxvr.exe'):
    out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower() == name:
            return int(parts[1])
    return None


def main():
    exe = open(sys.argv[1], 'rb').read()
    sig = exe[0x800 + 0x2400:0x800 + 0x2400 + 64]  # code bytes at 0x80012400 -> RAM offset 0x12400
    sig_off = 0x12400
    pid = find_pid()
    if not pid:
        print('psxvr.exe not running')
        return 1
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                      ctypes.POINTER(ctypes.c_size_t)]
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print('OpenProcess failed', ctypes.get_last_error())
        return 1
    addr = 0
    mbi = MBI()
    found = None
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        if mbi.State == MEM_COMMIT and not (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) and size >= 0x200000:
            buf = ctypes.create_string_buffer(size)
            n = ctypes.c_size_t()
            if k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, size, ctypes.byref(n)):
                data = buf.raw[:n.value]
                i = data.find(sig)
                while i >= 0:
                    ram = i - sig_off
                    if ram >= 0 and ram + 0x200000 <= len(data):
                        found = data[ram:ram + 0x200000]
                        break
                    i = data.find(sig, i + 1)
        if found:
            break
        addr = base + size
    k32.CloseHandle(h)
    if not found:
        print('RAM not found')
        return 1
    open(sys.argv[2], 'wb').write(found)
    print('RAM saved')
    return 0


if __name__ == '__main__':
    sys.exit(main())

"""List JAL call sites of a function: callers.py exe target_hex"""
import struct
import sys

exe = open(sys.argv[1], 'rb').read()
base = struct.unpack_from('<I', exe, 0x18)[0]
code = exe[0x800:]
t = int(sys.argv[2], 16)
w = (3 << 26) | ((t >> 2) & 0x3ffffff)
for i in range(0, len(code) - 3, 4):
    if struct.unpack_from('<I', code, i)[0] == w:
        print(hex(base + i))

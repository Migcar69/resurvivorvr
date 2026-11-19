"""Disassemble a range of a PS-X EXE: dis.py exe start_hex count"""
import os
import struct
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips import Ins, disasm

exe = open(sys.argv[1], 'rb').read()
base = struct.unpack_from('<I', exe, 0x18)[0]
start = int(sys.argv[2], 16)
n = int(sys.argv[3])
for k in range(n):
    a = start + 4 * k
    w = struct.unpack_from('<I', exe, 0x800 + a - base)[0]
    print('%08x %08x  %s' % (a, w, disasm(Ins(a, w))))

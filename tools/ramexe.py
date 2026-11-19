"""Wrap a RAM dump (ram_NNNNN.bin, 2 MB) as a PS-X EXE based at 0x80000000, so dis.py / callers.py also see the
overlays the game loads at run time: ramexe.py ram.bin out.exe"""
import struct
import sys

ram = open(sys.argv[1], 'rb').read()
hdr = bytearray(0x800)
hdr[0:8] = b'PS-X EXE'
struct.pack_into('<IIII', hdr, 0x10, 0x80000000, 0, 0x80000000, len(ram))
open(sys.argv[2], 'wb').write(bytes(hdr) + ram)

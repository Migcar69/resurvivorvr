"""PS-X EXE loader."""
import struct


class Exe:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        assert raw[:8] == b'PS-X EXE'
        self.pc, self.gp, self.base, self.size = struct.unpack_from('<4I', raw, 0x10)
        self.sp_base, self.sp_off = struct.unpack_from('<2I', raw, 0x30)
        self.data = raw[0x800:0x800 + self.size]
        self.end = self.base + len(self.data)

    def contains(self, addr):
        return self.base <= addr < self.end

    def word(self, addr):
        return struct.unpack_from('<I', self.data, addr - self.base)[0]

    def cstr(self, addr, maxlen=200):
        o = addr - self.base
        e = self.data.find(b'\0', o, o + maxlen)
        return self.data[o:e if e >= 0 else o + maxlen]

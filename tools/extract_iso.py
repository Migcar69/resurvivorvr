"""Extract the ISO9660 filesystem from a MODE2/2352 PS1 BIN image."""
import os
import struct
import sys

SECTOR = 2352
HDR = 24  # sync(12) + header(4) + subheader(8) for Mode2 Form1


def read_sector(f, lba):
    f.seek(lba * SECTOR)
    raw = f.read(SECTOR)
    return raw[HDR:HDR + 2048]


def read_extent(f, lba, size):
    out = bytearray()
    n = (size + 2047) // 2048
    for i in range(n):
        out += read_sector(f, lba + i)
    return bytes(out[:size])


def parse_dir(f, lba, size, path, out_root, listing):
    data = read_extent(f, lba, size)
    pos = 0
    while pos < len(data):
        ln = data[pos]
        if ln == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = data[pos:pos + ln]
        ext_lba = struct.unpack_from('<I', rec, 2)[0]
        ext_size = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        pos += ln
        if name in (b'\x00', b'\x01'):
            continue
        name = name.decode('ascii', 'replace').split(';')[0]
        full = os.path.join(path, name)
        if flags & 2:
            os.makedirs(os.path.join(out_root, full), exist_ok=True)
            parse_dir(f, ext_lba, ext_size, full, out_root, listing)
        else:
            listing.append((full, ext_lba, ext_size))
            with open(os.path.join(out_root, full), 'wb') as o:
                o.write(read_extent(f, ext_lba, ext_size))


def main():
    bin_path, out_root = sys.argv[1], sys.argv[2]
    os.makedirs(out_root, exist_ok=True)
    with open(bin_path, 'rb') as f:
        pvd = read_sector(f, 16)
        assert pvd[1:6] == b'CD001', 'not ISO9660'
        root = pvd[156:156 + 34]
        lba = struct.unpack_from('<I', root, 2)[0]
        size = struct.unpack_from('<I', root, 10)[0]
        listing = []
        parse_dir(f, lba, size, '', out_root, listing)
    for p, l, s in sorted(listing, key=lambda x: x[1]):
        print(f'{l:8d} {s:10d} {p}')


if __name__ == '__main__':
    main()

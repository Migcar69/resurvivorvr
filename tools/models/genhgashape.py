"""Handgun A for releases: its shape and texture coordinates (VR's own, from handgun_a_model.h) without the game's
picture; the picture is read at run time from the player's own disc (B\\BIGTIM.BIN, the item pictures; weapon_harvest.cpp).

usage: genhgashape.py handgun_a_model.h BIGTIM.BIN out.h
Writes the mesh (x y z, nx ny nz, u v), the picture's size, the part of the item picture it uses (86 x 54 at 14, 9 of
the 112 x 72 picture), a bit mask of the pixels it covers and a CRC32 of those pixels (15-bit words, from the disc) to
recognise the picture - no colour of the picture.
"""
import re
import struct
import sys
import zlib
import numpy as np

src, bigtim, out_h = sys.argv[1], sys.argv[2], sys.argv[3]
OX, OY = 14, 9
s = open(src, encoding='utf-8').read()
tw = int(re.search(r'HANDGUN_A_TEX_W = (\d+)', s).group(1))
th = int(re.search(r'HANDGUN_A_TEX_H = (\d+)', s).group(1))
tex = np.array([int(x) for x in re.search(r'HANDGUN_A_TEX\[\] = \{(.*?)\};', s, re.S).group(1).replace('\n', '').split(',')
                if x.strip()], np.uint8).reshape(th, tw, 4)
mesh = [x.strip() for x in re.search(r'HANDGUN_A_MESH\[\] = \{[^\n]*\n(.*?)\};', s, re.S).group(1).replace('\n', '').split(',')
        if x.strip()]
mask = tex[..., 3] > 0
# the first picture of BIGTIM.BIN (an 8-bit TIM, 112 x 72): Handgun A
d = open(bigtim, 'rb').read()
q = 12 + 8
L, cx, cy, cw, ch = struct.unpack_from('<IHHHH', d, q)
clut = np.frombuffer(d, '<u2', cw * ch, q + 12)
q += L
L, ix, iy, iw, ih = struct.unpack_from('<IHHHH', d, q)
img = np.frombuffer(d, np.uint8, iw * ih * 2, q + 12).reshape(ih, iw * 2)
pic = (clut[img] & 0x7fff)[OY:OY + th, OX:OX + tw]
crc = zlib.crc32(pic[mask].astype('<u2').tobytes())
bits = np.packbits(mask.reshape(-1), bitorder='little')
with open(out_h + '.tmp', 'w', encoding='utf-8') as fo:
    fo.write('// HGA: Handgun A\'s shape (tools/models/genhgashape.py): the mesh and texture coordinates VR built to the\n')
    fo.write('// Glock 17\'s measures, the mask of the item picture it is coloured with and a CRC to recognise that picture;\n')
    fo.write('// the picture itself is read from the player\'s disc (B\\BIGTIM.BIN, weapon_harvest.cpp).\n')
    fo.write('#pragma once\n#include <cstdint>\nnamespace game {\n')
    fo.write('constexpr int HGA_TEX_W = %d, HGA_TEX_H = %d, HGA_PIC_X = %d, HGA_PIC_Y = %d;\n' % (tw, th, OX, OY))
    fo.write('constexpr uint32_t HGA_CRC = 0x%08xu;\n' % crc)
    fo.write('static const uint8_t HGA_MASK[] = {\n')
    for i in range(0, len(bits), 32):
        fo.write('    ' + ','.join(str(x) for x in bits[i:i + 32]) + ',\n')
    fo.write('};\nconstexpr int HGA_VERTS = %d;\nstatic const float HGA_MESH[] = {  // x y z, nx ny nz, u v\n' % (len(mesh) // 8))
    for i in range(0, len(mesh), 8):
        fo.write('    ' + ', '.join(mesh[i:i + 8]) + ',\n')
    fo.write('};\n}  // namespace game\n')
import os
os.replace(out_h + '.tmp', out_h)
print('picture %dx%d at %d,%d, %d pixels, crc %08x, %d vertices' % (tw, th, OX, OY, mask.sum(), crc, len(mesh) // 8))

"""A weapon's model straight from the game's RAM (its own model format, the one turning when it is picked up) -> a header
usage: genweap.py ram.bin vram.bin out.h PREFIX obj_hex length_mm "comment" [preview.png [fist_x,fist_y]]
  obj_hex: the model's object record (?, primitives, vertices, normals); length_mm: the real weapon's length (the model
  is scaled to it, barrel to back)"""
import struct, sys
import numpy as np
from PIL import Image

ram = open(sys.argv[1], 'rb').read()
vram = np.frombuffer(open(sys.argv[2], 'rb').read(), '<u2').reshape(512, 1024)
PREFIX = sys.argv[4]
OBJ = int(sys.argv[5], 16) & 0x1fffff
LENGTH_MM = float(sys.argv[6])
COMMENT = sys.argv[7]
_, prim_top, vert_top, norm_top = struct.unpack_from('<4I', ram, OBJ)
prim_top &= 0x1fffff; vert_top &= 0x1fffff
NV = (norm_top & 0x1fffff) - vert_top
NV //= 8
verts = [struct.unpack_from('<3h', ram, vert_top + 8 * i) for i in range(NV)]

# primitives: a 2-word group header, then textured gouraud quads (GPU 0x3c, 9 words) and triangles (0x34, 7 words)
faces = []  # (indices, uvs, clut, tpage)
# GPU polygon commands, textured (0x24 bit set): 0x08 quad, 0x10 gouraud (a colour per vertex), 0x02 semi-transparent,
# 0x01 raw texture. Words: the colour(s), one word per vertex (uv; clut in the first's upper half, tpage in the
# second's, the first index in the third's, the second index in the fourth's for quads), then the remaining indices.
a = prim_top
while a < vert_top:
    w = struct.unpack_from('<I', ram, a)[0]
    code = w >> 24
    if (code & 0xe4) != 0x24:
        a += 4  # group header
        continue
    nv = 4 if code & 0x08 else 3
    ncol = nv if code & 0x10 else 1
    nw = ncol + nv + 1
    r = struct.unpack_from('<%dI' % nw, ram, a)
    uvw = r[ncol:ncol + nv]
    uv = [(x & 0xff, (x >> 8) & 0xff) for x in uvw]
    clut, tpage = uvw[0] >> 16, uvw[1] >> 16
    last = r[ncol + nv]
    if nv == 4:
        idx = [uvw[2] >> 16, uvw[3] >> 16, last & 0xffff, last >> 16]
    else:
        idx = [uvw[2] >> 16, last & 0xffff, last >> 16]
    faces.append((idx, uv, clut, tpage & 0x1f | (tpage & 0x180)))  # (semi-transparency mode dropped)
    a += 4 * nw
print('vertices', NV, 'faces', len(faces), 'cluts', sorted({hex(f[2]) for f in faces}), 'tpages', sorted({hex(f[3]) for f in faces}))
for f in faces:
    assert all(i < NV for i in f[0]), f

# texture: each CLUT's colours of the 4-bit page, rows used only; atlas of one band per CLUT
tp = faces[0][3]
px0, py0 = (tp & 0xf) * 64, ((tp >> 4) & 1) * 256
vmin = min(v for f in faces for u, v in f[1]); vmax = max(v for f in faces for u, v in f[1])
cluts = sorted({f[2] for f in faces})
BH = vmax - vmin + 1
TW, TH = 256, BH * len(cluts)
tex = np.zeros((TH, TW, 4), np.uint8)
for ci, cl in enumerate(cluts):
    cx, cy = (cl & 0x3f) * 16, cl >> 6
    pal = vram[cy, cx:cx + 16]
    for v in range(vmin, vmax + 1):
        for u in range(256):
            word = vram[py0 + v, px0 + u // 4]
            c = int(pal[(word >> ((u & 3) * 4)) & 15])
            r, g, b = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
            tex[ci * BH + v - vmin, u] = (r, g, b, 0 if c == 0 else 255)

# model frame: x along the gun (barrel at -x), y down, z across -> pistol frame X = z, Y = -y, Z = x (metres)
V = np.array(verts, float)
import os
if os.environ.get('GENWEAP_TURN'):  # the model has its barrel towards +x: turned half round (x, z -> -x, -z; not mirrored)
    V[:, 0] *= -1
    V[:, 2] *= -1
print('extent', V.min(0), V.max(0))
S = LENGTH_MM / 1000.0 / (V[:, 0].max() - V[:, 0].min())  # scaled to the real weapon's length
# fist: on the grip (the lowest, rearmost part): middle of the grip's points
grip = V[(V[:, 1] > 20) & (V[:, 0] > 40)]
FX, FY = grip[:, 0].mean(), (grip[:, 1].min() + grip[:, 1].max()) / 2
if len(sys.argv) > 9:  # the fist's place in the model's units, "x,y" (when the grip is not simply the low rear part)
    FX, FY = (float(t) for t in sys.argv[9].split(','))
def P(v): return ((v[2]) * S, -(v[1] - FY) * S, (v[0] - FX) * S)
muzzle = V[V[:, 0] <= V[:, 0].min() + 1]
bore_y = muzzle[:, 1].mean()
my, mz = -(bore_y - FY) * S, (V[:, 0].min() - FX) * S
out = []
for idx, uv, cl, _ in faces:
    ci = cluts.index(cl)
    pts = [np.array(P(V[i])) for i in idx]
    tris = [(0, 1, 2), (1, 3, 2)] if len(idx) == 4 else [(0, 1, 2)]
    for t in tris:
        n = np.cross(pts[t[1]] - pts[t[0]], pts[t[2]] - pts[t[0]])
        l = np.linalg.norm(n)
        n = n / l if l > 0 else np.array([0, 1.0, 0])
        for k in t:
            u, v = uv[k]
            out.append((*pts[k], *n, (u + 0.5) / TW, (ci * BH + v - vmin + 0.5) / TH))
Q = np.array([o[:3] for o in out])
print('verts', len(out), 'bbox', Q.min(0).round(3), Q.max(0).round(3), 'muzzle', round(my, 4), round(mz, 4))
if len(sys.argv) > 8:
    Image.fromarray(tex).resize((TW * 3, TH * 3), Image.NEAREST).save(sys.argv[8])
P_ = PREFIX
with open(sys.argv[3], 'w') as fo:
    fo.write("// %s: the game's own model, read from its RAM (%s): %d vertices, %d faces (textured quads and\n"
             % (P_, COMMENT, NV, len(faces)))
    fo.write("// triangles), texture page 0x%02x with %d CLUT(s), scaled to %.0f mm long. Pistol frame (metres: -Z barrel,\n"
             % (tp, len(cluts), LENGTH_MM))
    fo.write("// +Y up, origin = fist on the grip). Generated by tools/models/genweap.py.\n")
    fo.write('#pragma once\n#include <cstdint>\nnamespace game {\n')
    fo.write('constexpr int %s_TEX_W = %d, %s_TEX_H = %d;\n' % (P_, TW, P_, TH))
    fo.write('constexpr float %s_MUZZLE_Y = %.4ff, %s_MUZZLE_Z = %.4ff;\n' % (P_, my, P_, mz))
    bb = tex.tobytes()
    fo.write('static const uint8_t %s_TEX[] = {\n' % P_)
    for i in range(0, len(bb), 24): fo.write('    ' + ','.join(str(x) for x in bb[i:i + 24]) + ',\n')
    fo.write('};\nconstexpr int %s_VERTS = %d;\nstatic const float %s_MESH[] = {  // x y z, nx ny nz, u v\n' % (P_, len(out), P_))
    for o in out: fo.write('    ' + ', '.join('%.5ff' % x for x in o) + ',\n')
    fo.write('};\n}  // namespace game\n')

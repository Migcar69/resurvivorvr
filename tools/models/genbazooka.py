"""The Rocket Launcher's hand model: an M20A1-like bazooka made of solid low-poly shapes (PS1 style), proportions taken
from the side photo 1200px-M20A1.jpg (1.286 mm a pixel, fist on the pistol grip at pixel 655,185). Everything is closed
(a tube with flared ends, boxes for grip / trigger guard / sight / rear box / shoulder rest), so it looks full from every
side; colours are flat swatches of a tiny texture.

usage: genbazooka.py out.h PREFIX
Pistol frame (metres: -Z towards the muzzle, +Y up, origin = the fist on the pistol grip).
"""
import math
import sys
import numpy as np

out_h, PREFIX = sys.argv[1], sys.argv[2]
MM = 0.001

# a pixelly texture atlas in the PS1's style (5 bits a component, point-sampled): the tube's paint (128 x 32: along x
# the tube's length, along y around it) and a 16 x 16 tile for each part (dark-edged), with dithered noise and wear
rng = np.random.default_rng(7)
TW, TH = 128, 64
tex = np.zeros((TH, TW, 4), np.uint8)
tex[..., 3] = 255
Z0, Z1 = -835.0, 691.0


def paint(h, w, base, noise=10, wear=0.0, wear_col=(150, 150, 110)):
    t = np.array(base, float)[None, None, :] + rng.integers(-noise, noise + 1, (h, w, 1))         + rng.integers(-noise // 3 - 1, noise // 3 + 2, (h, w, 3))
    if wear:
        m = rng.random((h, w)) < wear
        t[m] = np.array(wear_col, float) + rng.integers(-12, 13, (m.sum(), 3))
    return t


# the tube: olive paint, lighter on top (row 0 = top), darker under it; darker bands at the rings; scuffed near the ends
tube = paint(32, 128, (80, 86, 52), 7)
for r in range(32):
    a = r / 32 * 2 * math.pi
    tube[r] *= 0.80 + 0.32 * (0.5 + 0.5 * math.cos(a))
def zc(z):
    return int(round((z - Z0) / (Z1 - Z0) * 127))
for za, zb in ((-100, -60), (170, 192)):
    tube[:, zc(za):zc(zb) + 1] *= 0.72
for za, zb in ((-835, -760), (580, 691)):  # flares: worn paint on the rims
    sl = tube[:, zc(za):zc(zb) + 1]
    m = rng.random(sl.shape[:2]) < 0.18
    sl[m] = np.array((120, 118, 90)) + rng.integers(-15, 16, (m.sum(), 3))
tube[:, zc(-835):zc(-830) + 1] *= 0.6
tube[:, zc(686):zc(691) + 1] *= 0.6
# stencil marks on the side (row ~8 = the left side): a few pale blocks like lettering
for k, zz in enumerate(range(zc(250), zc(420), 3)):
    if rng.random() < 0.7:
        tube[7:9, zz:zz + 2] = (176, 168, 128)
for zz in range(zc(280), zc(380), 4):
    if rng.random() < 0.6:
        tube[10, zz:zz + 3] = (164, 156, 120)
# scratches along the tube
for _ in range(14):
    r, z = rng.integers(0, 32), rng.integers(4, 120)
    tube[r, z:z + rng.integers(2, 6)] = (118, 122, 84)
tex[0:32, :, :3] = (tube.clip(0, 255).astype(np.uint8) >> 3) << 3

TILES = {}


def tile(name, col, base, noise=10, wear=0.0, wear_col=(140, 140, 110), edge=0.65, pattern=None):
    t = paint(16, 16, base, noise, wear, wear_col)
    if pattern == 'grip':  # chequering
        for y in range(1, 15):
            for x in range(1, 15):
                if (x + y) % 3 == 0:
                    t[y, x] *= 0.7
    if pattern == 'bore':
        yy, xx = np.mgrid[0:16, 0:16]
        d = np.hypot(yy - 7.5, xx - 7.5) / 8
        t *= (0.25 + 0.75 * d.clip(0, 1))[..., None]
    else:
        t[0, :] *= edge; t[-1, :] *= edge; t[:, 0] *= edge; t[:, -1] *= edge
        t[1, 1:-1] *= 1.12  # a lit upper edge
    x0, y0 = col * 16, 32 + (len(TILES) // 8) * 16
    tex[y0:y0 + 16, x0:x0 + 16, :3] = (t.clip(0, 255).astype(np.uint8) >> 3) << 3
    TILES[name] = ((x0 + 0.02) / TW, (y0 + 0.02) / TH, (x0 + 15.98) / TW, (y0 + 15.98) / TH)


tile('olive', 0, (80, 86, 52), 7, wear=0.06)
tile('olive_dk', 1, (58, 62, 38), 7, wear=0.05, wear_col=(110, 112, 80))
tile('olive_lt', 2, (98, 104, 66), 7, wear=0.05)
tile('grip', 3, (44, 44, 54), 6, pattern='grip')
tile('metal', 4, (78, 78, 74), 12, wear=0.15, wear_col=(130, 130, 124))
tile('inside', 5, (40, 40, 30), 4, pattern='bore')


def tile_uv(name):
    u0, v0, u1, v1 = TILES[name]
    return [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]


tris = []


def tri(p0, p1, p2, uv3):
    tris.append((np.array(p0, float), np.array(p1, float), np.array(p2, float), uv3))


def quad(a, b, c, d, col, uv4=None):  # a b c d around the face; col = a tile, or uv4 = the corners' uvs
    u = uv4 or tile_uv(col)
    tri(a, b, c, (u[0], u[1], u[2]))
    tri(a, c, d, (u[0], u[2], u[3]))


def box(cx, cy, cz, sx, sy, sz, col, shear_z=0.0):
    """A box centred at (cx,cy,cz) mm, sizes s* mm; shear_z moves the bottom face along z (a slanted grip)."""
    hx, hy, hz = sx / 2, sy / 2, sz / 2
    def P(x, y, z):
        dz = shear_z * (0.5 - (y + hy) / sy) if sy else 0  # bottom (y=-hy) moved by +shear_z/2, top by -shear_z/2
        return ((cx + x) * MM, (cy + y) * MM, (cz + z + dz) * MM)
    v = [P(x, y, z) for x in (-hx, hx) for y in (-hy, hy) for z in (-hz, hz)]
    # index: x*4 + y*2 + z
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    for f in faces:
        quad(*(v[i] for i in f), col)


def bar(p, q, w, h, col):
    """A square bar from p to q (mm, in the y-z plane), w across (x), h thick."""
    p, q = np.array(p, float), np.array(q, float)
    d = q - p
    n = np.array([0, -d[2], d[1]]) / np.linalg.norm(d) * h / 2
    corners = []
    for e in (p, q):
        for s in (-1, 1):
            for x in (-w / 2, w / 2):
                c = e + s * n
                corners.append(((c[0] + x) * MM, c[1] * MM, c[2] * MM))
    # corners: end*4 + side*2 + x
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    for f in faces:
        quad(*(corners[i] for i in f), col)


# the tube: profile (z mm, radius mm, colour of the band ending there); the axis is 125 mm above the fist
AY = 125.0
SIDES = 10
prof = [(-835, 66, 'olive'), (-815, 60, 'olive'), (-790, 48, 'olive'), (-760, 46, 'olive'),
        (-100, 46, 'olive'), (-100, 60, 'olive_dk'), (-60, 60, 'olive_dk'), (-60, 46, 'olive_dk'),
        (170, 46, 'olive'), (170, 52, 'olive_dk'), (192, 52, 'olive_dk'), (192, 46, 'olive_dk'),
        (580, 48, 'olive'), (630, 56, 'olive'), (665, 70, 'olive'), (691, 76, 'olive')]


def ring(z, r, k):
    t = 2 * math.pi * (k + 0.5) / SIDES
    return (r * math.sin(t) * MM, (AY + r * math.cos(t)) * MM, z * MM)


def tube_uv(z, k):
    return ((zc(z) + 0.5) / TW, min(k / SIDES * 32, 31.98) / TH)


for (z0, r0, _), (z1, r1, c) in zip(prof, prof[1:]):
    for k in range(SIDES):
        quad(ring(z0, r0, k), ring(z1, r1, k), ring(z1, r1, k + 1), ring(z0, r0, k + 1), None,
             [tube_uv(z0, k), tube_uv(z1, k), tube_uv(z1, k + 1), tube_uv(z0, k + 1)])
# the bore: a dark disc a little inside each mouth (the tube looks open but not see-through)
for z, r in ((-825, 62), (680, 72)):
    c = (0, AY * MM, z * MM)
    u0, v0, u1, v1 = TILES['inside']
    cu = ((u0 + u1) / 2, (v0 + v1) / 2)
    def duv(k):
        t = 2 * math.pi * (k + 0.5) / SIDES
        return (cu[0] + math.sin(t) * (u1 - u0) / 2, cu[1] + math.cos(t) * (v1 - v0) / 2)
    for k in range(SIDES):
        tri(c, ring(z, r, k), ring(z, r, k + 1), (cu, duv(k), duv(k + 1)))

# pistol grip (slanted back at the bottom), trigger guard, trigger
box(0, 20, 8, 30, 120, 44, 'grip', shear_z=26)
box(0, 72, -10, 34, 16, 70, 'olive_dk')            # the grip's root under the tube
box(0, 12, -48, 16, 16, 50, 'metal')                # trigger guard: bottom
box(0, 42, -70, 16, 60, 16, 'metal')                # trigger guard: front
box(0, 45, -38, 10, 30, 10, 'metal')                  # trigger
# sight housing on the left side of the tube, over the grip
box(-40, 160, 60, 26, 46, 80, 'olive_dk')
box(-40, 190, 50, 14, 22, 18, 'metal')               # sight
# rear box on top of the tube, near the back
box(0, 190, 560, 56, 44, 130, 'olive_lt')
# shoulder rest: a leg down from the ring and a slanted bar back up to the tube
bar((0, 79, 182), (0, -95, 182), 22, 18, 'olive_dk')
bar((0, -95, 182), (0, 79, 450), 22, 16, 'olive_dk')
box(0, -100, 200, 30, 14, 60, 'olive_dk')           # its foot
# small lugs under the tube (the sling rings)
box(0, 72, -620, 16, 20, 24, 'olive_dk')
box(0, 72, 400, 16, 20, 24, 'olive_dk')

out = []
for p0, p1, p2, uv3 in tris:
    n = np.cross(p1 - p0, p2 - p0)
    l = np.linalg.norm(n)
    n = n / l if l > 0 else np.array([0, 1.0, 0])
    for p, u in zip((p0, p1, p2), uv3):
        out.append((*p, *n, *u))
Q = np.array([o[:3] for o in out])
muzzle_y, muzzle_z = AY * MM, -835 * MM
print('verts', len(out), 'bbox', Q.min(0).round(3), Q.max(0).round(3))
with open(out_h + '.tmp', 'w', encoding='utf-8') as fo:
    fo.write('// %s: an M20A1-like bazooka of solid low-poly shapes (tools/models/genbazooka.py), proportions from the\n' % PREFIX)
    fo.write('// photo 1200px-M20A1.jpg; 1524 mm. Pistol frame (metres: -Z muzzle, +Y up, origin = fist on the grip).\n')
    fo.write('#pragma once\n#include <cstdint>\nnamespace game {\n')
    fo.write('constexpr int %s_TEX_W = %d, %s_TEX_H = %d;\n' % (PREFIX, TW, PREFIX, TH))
    fo.write('constexpr float %s_MUZZLE_Y = %.4ff, %s_MUZZLE_Z = %.4ff;\n' % (PREFIX, muzzle_y, PREFIX, muzzle_z))
    bb = tex.tobytes()
    fo.write('static const uint8_t %s_TEX[] = {\n' % PREFIX)
    for i in range(0, len(bb), 32):
        fo.write('    ' + ','.join(str(x) for x in bb[i:i + 32]) + ',\n')
    fo.write('};\nconstexpr int %s_VERTS = %d;\nstatic const float %s_MESH[] = {  // x y z, nx ny nz, u v\n' % (PREFIX, len(out), PREFIX))
    for o in out:
        fo.write('    ' + ', '.join('%.5ff' % x for x in o) + ',\n')
    fo.write('};\n}  // namespace game\n')
import os
os.replace(out_h + '.tmp', out_h)

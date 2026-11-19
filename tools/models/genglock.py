"""Handgun A's hand model (the game has no 3D model of it): a Glock 17 of solid low-poly shapes (PS1 style, like the
bazooka), proportions from a side photo (Glock17EarlyModel.jpg, 700 x 474: muzzle at x 12, slide top at y 20), the
real pistol's measures (202 mm long, slide 25.5 mm wide, frame 28, grip 30, trigger guard 10). The texture is ours:
dark steel slide with rear serrations, black polymer frame, stippled grip.

usage: genglock.py out.h
Pistol frame (metres: -Z barrel, +Y up, origin = the fist), placed as the earlier Handgun A: muzzle at z -0.152, slide
top at y 0.0877.
"""
import math
import os
import sys
import numpy as np

out_h = sys.argv[1]
S = 0.202 / (640 - 12)          # metres per photo pixel


def Z(px): return -0.152 + (px - 12) * S
def Y(py): return 0.0877 - (py - 20) * S


# ---- texture: 16 x 16 tiles, 5 bits a component, dark-edged
rng = np.random.default_rng(17)
TILES, NAMES = [], []


def tile(name, base, noise=6, pattern=None):
    t = np.array(base, float)[None, None, :] + rng.integers(-noise, noise + 1, (16, 16, 1))
    if pattern == 'stipple':
        t[rng.random((16, 16)) < 0.4] *= 0.68
    if pattern == 'serr':
        for x in range(1, 16, 3):
            t[:, x] *= 0.5
    if pattern == 'bore':
        t[:] = 12
    t[0, :] *= 0.75; t[-1, :] *= 0.75; t[:, 0] *= 0.75; t[:, -1] *= 0.75
    t[1, 1:-1] *= 1.15
    TILES.append((np.clip(t, 0, 255).astype(np.uint8) >> 3) << 3)
    NAMES.append(name)


tile('slide', (70, 72, 78))
tile('serr', (70, 72, 78), pattern='serr')
tile('frame', (44, 44, 48), 4)
tile('grip', (44, 44, 48), 4, 'stipple')
tile('metal', (96, 98, 104), 8)
tile('bore', (12, 12, 12))
TW, TH = 16 * len(TILES), 16
tex = np.zeros((TH, TW, 4), np.uint8)
for i, t in enumerate(TILES):
    tex[:, i * 16:(i + 1) * 16, :3] = t
tex[..., 3] = 255


def uvrect(name):
    i = NAMES.index(name)
    return ((i * 16 + 0.5) / TW, 0.5 / TH, (i * 16 + 15.5) / TW, 15.5 / TH)


tris = []


def tri(a, b, c, uv):
    tris.append((np.array(a, float), np.array(b, float), np.array(c, float), uv))


def prism(poly, x0, x1, name, side_name=None):
    """A solid: the convex outline poly [(px, py) photo pixels, in order] extruded across from x0 to x1 (metres)."""
    u0, v0, u1, v1 = uvrect(name)
    pts = [(Z(px), Y(py)) for px, py in poly]
    zs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    zmin, zmax, ymin, ymax = min(zs), max(zs), min(ys), max(ys)
    def uv_of(z, y):
        return (u0 + (u1 - u0) * (z - zmin) / max(1e-6, zmax - zmin), v0 + (v1 - v0) * (ymax - y) / max(1e-6, ymax - ymin))
    for x in (x0, x1):  # the two flat sides (a fan: the outline is convex)
        for k in range(1, len(pts) - 1):
            a, b, c = pts[0], pts[k], pts[k + 1]
            tri((x, a[1], a[0]), (x, b[1], b[0]), (x, c[1], c[0]), (uv_of(*a), uv_of(*b), uv_of(*c)))
    su0, sv0, su1, sv1 = uvrect(side_name or name)
    for k in range(len(pts)):  # the walls around
        a, b = pts[k], pts[(k + 1) % len(pts)]
        q = [(x0, a[1], a[0]), (x1, a[1], a[0]), (x1, b[1], b[0]), (x0, b[1], b[0])]
        uq = [(su0, sv0), (su1, sv0), (su1, sv1), (su0, sv1)]
        tri(q[0], q[1], q[2], (uq[0], uq[1], uq[2]))
        tri(q[0], q[2], q[3], (uq[0], uq[2], uq[3]))


def box(px0, py0, px1, py1, half_w, name, side=None):
    prism([(px0, py0), (px1, py0), (px1, py1), (px0, py1)], -half_w, half_w, name, side)


W = 0.001
# slide (25.5 mm wide): the front part, the serrated rear, a slightly narrower top band, the muzzle's bore
box(12, 20, 488, 92, 12.75 * W, 'slide')
box(488, 20, 632, 92, 12.75 * W, 'serr', 'slide')
box(14, 40, 18, 62, 4.5 * W, 'bore')                  # (just in front of the slide's face)
box(28, 8, 46, 20, 1.8 * W, 'metal')                  # front sight
box(578, 6, 602, 20, 5.0 * W, 'metal')                # rear sight
# frame (28 mm): dust cover under the slide, the middle over the trigger
box(16, 92, 250, 138, 13.0 * W, 'frame')
box(250, 92, 470, 152, 14.0 * W, 'frame')
# trigger guard (10 mm): front (slanted) and bottom, and the trigger
prism([(240, 138), (266, 138), (268, 228), (244, 244)], -5 * W, 5 * W, 'frame')
prism([(244, 222), (420, 222), (420, 244), (244, 244)], -5 * W, 5 * W, 'frame')
prism([(330, 160), (352, 160), (372, 208), (352, 214)], -3 * W, 3 * W, 'metal')
# grip (30 mm), slanted back, with the backstrap's hump; the magazine's floor plate
prism([(420, 92), (640, 92), (668, 230), (690, 432), (500, 446), (432, 248)], -15 * W, 15 * W, 'grip', 'frame')
prism([(496, 440), (692, 428), (694, 462), (500, 468)], -15.5 * W, 15.5 * W, 'frame')
# slide stop and takedown lever (small, on the left side)
box(436, 90, 476, 104, 15.0 * W, 'metal')

out = []
for p0, p1, p2, uv3 in tris:
    n = np.cross(p1 - p0, p2 - p0)
    l = np.linalg.norm(n)
    n = n / l if l > 0 else np.array([0, 1.0, 0])
    for p, u in zip((p0, p1, p2), uv3):
        out.append((*p, *n, *u))
Q = np.array([o[:3] for o in out])
muzzle_y = Y(51)
print('verts', len(out), 'bbox', Q.min(0).round(4), Q.max(0).round(4), 'muzzle y', round(muzzle_y, 4))
with open(out_h + '.tmp', 'w', encoding='utf-8') as fo:
    fo.write('// GLOCK: Handgun A (a Glock 17) of solid low-poly shapes (tools/models/genglock.py), proportions from a side\n')
    fo.write('// photo and the real pistol\'s measures, a texture of our own (nothing of the game). Pistol frame (metres:\n')
    fo.write('// -Z barrel, +Y up, origin = the fist).\n#pragma once\n#include <cstdint>\nnamespace game {\n')
    fo.write('constexpr int GLOCK_TEX_W = %d, GLOCK_TEX_H = %d;\n' % (TW, TH))
    fo.write('constexpr float GLOCK_MUZZLE_Y = %.4ff, GLOCK_MUZZLE_Z = %.4ff;\n' % (muzzle_y, Z(12)))
    b = tex.tobytes()
    fo.write('static const uint8_t GLOCK_TEX[] = {\n')
    for i in range(0, len(b), 32):
        fo.write('    ' + ','.join(str(x) for x in b[i:i + 32]) + ',\n')
    fo.write('};\nconstexpr int GLOCK_VERTS = %d;\nstatic const float GLOCK_MESH[] = {  // x y z, nx ny nz, u v\n' % len(out))
    for o in out:
        fo.write('    ' + ', '.join('%.5ff' % x for x in o) + ',\n')
    fo.write('};\n}  // namespace game\n')
os.replace(out_h + '.tmp', out_h)

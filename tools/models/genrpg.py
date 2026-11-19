"""A launcher model from a side photo on a white background (the RPG-7 for the Rocket Launcher) -> a header.

usage: genrpg.py photo out.h PREFIX launcher_mm [preview.png]   (GENRPG_FIST=x,y: the fist in the photo)
The tube (everything round: launcher, grenade, cone at the back) becomes a body of revolution: for each column of the
straightened photo, its height around the tube's axis is the diameter of a ring (12 sides), coloured with the photo's
column. What sticks out under / over the tube (grips, trigger, sights) becomes flat cut-outs 3 cm thick, textured with
the photo (transparent where the photo is white). Scale: the launcher tube (from the grenade's end to the back) is
launcher_mm long. Pistol frame (metres: -Z towards the muzzle, +Y up), origin = the fist on the rear pistol grip.
"""
import math
import os
import sys
import numpy as np
from PIL import Image

src, out_h, PREFIX, LAUNCHER_MM = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
im = Image.open(src).convert('RGB')
a = np.asarray(im).astype(int)
H, W = a.shape[:2]
mask = (a.min(2) < 225)  # not the white background


def tube_rows(col):
    ys = np.nonzero(col)[0]
    return (ys.min(), ys.max()) if len(ys) else None


# the tube's axis: the middle of the longest vertical run in each column, fitted with a line (the photo is tilted)
xs, ys = [], []
for x in range(W):
    col = mask[:, x]
    if col.sum() < 4:
        continue
    # runs
    d = np.diff(np.concatenate([[0], col.astype(int), [0]]))
    st, en = np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]
    k = np.argmax(en - st)
    xs.append(x), ys.append((st[k] + en[k] - 1) / 2)
xs, ys = np.array(xs, float), np.array(ys, float)
keep = np.ones(len(xs), bool)
for _ in range(5):  # robust: grips / sights hanging from the tube shift some columns' centres - drop them and refit
    k, b = np.polyfit(xs[keep], ys[keep], 1)
    keep = np.abs(ys - (k * xs + b)) < 2.0
ang = math.degrees(math.atan(k))
print('tilt %.2f deg' % ang)
im2 = im.rotate(ang, resample=Image.BICUBIC, fillcolor=(255, 255, 255), center=(0, b))
a = np.asarray(im2).astype(int)
mask = (a.min(2) < 225)
axis_y = b
# tube radius per column: the run containing the axis
x_cols = [x for x in range(W) if mask[int(round(axis_y)), x] or mask[int(round(axis_y)) - 1, x]]
x0, x1 = min(x_cols), max(x_cols)
rad = np.zeros(W)
top = np.zeros(W)
bot = np.zeros(W)
for x in range(x0, x1 + 1):
    col = mask[:, x]
    y = int(round(axis_y))
    if not col[y]:
        rad[x] = rad[x - 1] if x > x0 else 0
        continue
    t = y
    while t > 0 and col[t - 1]:
        t -= 1
    bt = y
    while bt < H - 1 and col[bt + 1]:
        bt += 1
    top[x], bot[x] = t, bt
    rad[x] = min(axis_y - t, bt - axis_y)  # the narrower side: grips / sights are not the tube
# smooth the radius a little and cap jumps (a grip's root counted as tube)
r2 = rad.copy()
for x in range(x0 + 2, x1 - 1):
    r2[x] = np.median(rad[x - 2:x + 3])
rad = r2
# the launcher tube (without the grenade): the grenade is the front part ahead of the first narrow neck; the tube starts
# where the long straight part begins (the first column, going back from the middle, with the tube's typical radius)
mid = (x0 + x1) // 2
typical = np.median(rad[mid - 50:mid + 50])
tube_start = x0
for x in range(mid, x0, -1):
    if abs(rad[x] - typical) > typical * 0.35:
        tube_start = x + 1
        break
if os.environ.get('GENRPG_FULL'):  # the whole object is the launcher (no grenade in front)
    tube_start = x0
launcher_px = x1 - tube_start
S = LAUNCHER_MM / 1000.0 / launcher_px  # metres per pixel
print('ends %d..%d, launcher from %d (%d px), %.3f mm/px, tube radius %.1f px' % (x0, x1, tube_start, launcher_px, S * 1000, typical))

# appendages: mask pixels outside the tube's rows
app = mask.copy()
for x in range(W):
    if x0 <= x <= x1 and rad[x] > 0:
        app[int(axis_y - rad[x]) - 1:int(axis_y + rad[x]) + 2, x] = False
app[:, :x0] = False
app[:, x1 + 1:] = False
# the rear pistol grip (with the trigger): the lowest point of the appendages in the front half of the launcher
ys_a, xs_a = np.nonzero(app)
below = ys_a > axis_y + typical * 1.5
gx = xs_a[below]
gy = ys_a[below]
# two grips: split at the biggest gap in x
order = np.sort(np.unique(gx))
gaps = np.diff(order)
split = order[np.argmax(gaps)] if len(gaps) else order[0]
front = (gx <= split)
fist_x = gx[front].mean()
fist_y = (gy[front].min() + gy[front].max()) / 2
import os
if os.environ.get('GENRPG_FIST'):  # the fist's place in the photo (pixels, after straightening): "x,y"
    fist_x, fist_y = (float(t) for t in os.environ['GENRPG_FIST'].split(','))
print('fist at %.0f, %.0f (px)' % (fist_x, fist_y))

# texture in the PS1's style: the straightened photo at 128 pixels across, 16 colours (a 4-bit CLUT), 5 bits a
# component; alpha = not background
TW = 128
scale_t = TW / W
TH = int(round(H * scale_t))
tex_img = im2.resize((TW, TH), Image.BOX)
ta = np.asarray(tex_img).astype(float)
solid = ta.min(2) < 200  # (edges mixed with the white background are left out)
alpha = solid.astype(np.uint8) * 255
# the launcher's colours spread over the background around it (the rings' edges sample there, and the 16 colours must
# not include the white): a few passes of the average of the solid neighbours
rgb = ta.copy()
known = solid.copy()
for _ in range(6):
    acc = np.zeros_like(rgb); cnt = np.zeros(known.shape)
    for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
        sh = np.roll(np.roll(known, dy, 0), dx, 1)
        acc += np.roll(np.roll(rgb * known[..., None], dy, 0), dx, 1)
        cnt += sh
    new = (~known) & (cnt > 0)
    rgb[new] = acc[new] / cnt[new][:, None]
    known = known | new
rgb[~known] = rgb[known].mean(0)
tex_img = Image.fromarray(rgb.clip(0, 255).astype(np.uint8))
q = tex_img.quantize(colors=16, method=Image.Quantize.MEDIANCUT).convert('RGB')
tm = (np.asarray(q).astype(np.uint8) >> 3) << 3
tex = np.dstack([tm, alpha])


def uv(px, py):
    return ((px + 0.5) / W, (py + 0.5) / H)


def P(px, py, z_mm=0.0):
    # photo x grows towards the back: forward is -Z
    return np.array([z_mm / 1000.0, -(py - fist_y) * S, (px - fist_x) * S])


tris = []  # (p0, p1, p2, uv0, uv1, uv2)
SIDES = 8  # (PS1-style: few polygons)
# a new ring only where the profile turns (the radius changes by more than ~1.5 px from the straight line between the
# last ring and here), at least every 60 px
cols = [x0]
for x in range(x0 + 1, x1 + 1):
    last = cols[-1]
    if x - last >= 60 or x == x1:
        cols.append(x)
        continue
    nx = min(x + 1, x1)
    # the straight line from the last ring to the next column: does it stray from the profile here?
    if any(abs(rad[xx] - (rad[last] + (rad[nx] - rad[last]) * (xx - last) / max(1, nx - last))) > 1.5
           for xx in range(last + 1, nx)):
        cols.append(x)
for i in range(len(cols) - 1):
    xa, xb = cols[i], cols[i + 1]
    ra, rb = rad[xa], rad[xb]
    if ra <= 0 or rb <= 0:
        continue
    for s in range(SIDES):
        t0, t1 = 2 * math.pi * s / SIDES, 2 * math.pi * (s + 1) / SIDES
        def ring(x, r, t):
            # a point of the ring: across (z in the photo's depth) and up
            p = P(x, axis_y - r * math.cos(t))
            p[0] = r * math.sin(t) * S
            return p, uv(x, axis_y - 0.8 * r * math.cos(t))  # (colour from a little inside the contour)
        (pa0, ua0), (pa1, ua1) = ring(xa, ra, t0), ring(xa, ra, t1)
        (pb0, ub0), (pb1, ub1) = ring(xb, rb, t0), ring(xb, rb, t1)
        tris.append((pa0, pb0, pa1, ua0, ub0, ua1))
        tris.append((pa1, pb0, pb1, ua1, ub0, ub1))
# end caps (back cone's mouth, grenade tip): discs
for x, sign in ((x0, -1), (x1, 1)):
    r = rad[x]
    c = P(x, axis_y)
    for s in range(SIDES):
        t0, t1 = 2 * math.pi * s / SIDES, 2 * math.pi * (s + 1) / SIDES
        p0 = P(x, axis_y - r * math.cos(t0)); p0[0] = r * math.sin(t0) * S
        p1 = P(x, axis_y - r * math.cos(t1)); p1[0] = r * math.sin(t1) * S
        u = uv(x - sign * 3, axis_y)
        tris.append((c, p0, p1, u, u, u))
# appendages: per run of rows in each column block, flat cut-out quads on both sides (3 cm thick), and their top edge
THICK = 30.0
ys_a, xs_a = np.nonzero(app)
if len(xs_a):
    bx0, bx1, by0, by1 = xs_a.min(), xs_a.max(), ys_a.min(), ys_a.max()
    # the bounding box of the appendages above and below the tube separately, as textured cut-out boxes
    for sel in (ys_a < axis_y, ys_a > axis_y):
        if not sel.any():
            continue
        # columns groups (each grip / sight separately): split where columns are empty
        cx = np.sort(np.unique(xs_a[sel]))
        groups = [[cx[0]]]
        for c in cx[1:]:
            if c - groups[-1][-1] > 3:
                groups.append([c])
            else:
                groups[-1].append(c)
        for g in groups:
            gx0, gx1 = g[0], g[-1]
            m = sel & (xs_a >= gx0) & (xs_a <= gx1)
            gy0, gy1 = ys_a[m].min(), ys_a[m].max()
            for zf in (-THICK / 2, THICK / 2):
                q = [P(gx0, gy0, zf), P(gx1 + 1, gy0, zf), P(gx0, gy1 + 1, zf), P(gx1 + 1, gy1 + 1, zf)]
                u = [uv(gx0, gy0), uv(gx1 + 1, gy0), uv(gx0, gy1 + 1), uv(gx1 + 1, gy1 + 1)]
                tris.append((q[0], q[1], q[2], u[0], u[1], u[2]))
                tris.append((q[1], q[3], q[2], u[1], u[3], u[2]))
            # the sides of the cut-out: thin strips along each column's run (front / back) skipped; edges at x
            for xe in (gx0, gx1 + 1):
                q = [P(xe, gy0, -THICK / 2), P(xe, gy0, THICK / 2), P(xe, gy1 + 1, -THICK / 2), P(xe, gy1 + 1, THICK / 2)]
                u0 = uv(min(xe, W - 1), (gy0 + gy1) / 2)
                tris.append((q[0], q[1], q[2], u0, u0, u0))
                tris.append((q[1], q[3], q[2], u0, u0, u0))

out = []
for p0, p1, p2, u0, u1, u2 in tris:
    n = np.cross(p1 - p0, p2 - p0)
    l = np.linalg.norm(n)
    n = n / l if l > 0 else np.array([0, 1.0, 0])
    for p, u in ((p0, u0), (p1, u1), (p2, u2)):
        out.append((*p, *n, u[0], u[1]))
Q = np.array([o[:3] for o in out])
muzzle_y = -(axis_y - fist_y) * S
muzzle_z = (x0 - fist_x) * S
print('verts', len(out), 'bbox', Q.min(0).round(3), Q.max(0).round(3), 'muzzle', round(muzzle_y, 4), round(muzzle_z, 4))
if len(sys.argv) > 5:
    Image.fromarray(tex, 'RGBA').save(sys.argv[5])
with open(out_h, 'w', encoding='utf-8') as fo:
    fo.write('// %s: built from a side photo of an RPG-7 (tools/models/genrpg.py): the tube as a body of revolution, grips /\n' % PREFIX)
    fo.write('// trigger / sights as 3 cm cut-outs; launcher %.0f mm. Pistol frame (metres: -Z muzzle, +Y up, origin = fist on\n' % LAUNCHER_MM)
    fo.write('// the rear pistol grip).\n#pragma once\n#include <cstdint>\nnamespace game {\n')
    fo.write('constexpr int %s_TEX_W = %d, %s_TEX_H = %d;\n' % (PREFIX, TW, PREFIX, TH))
    fo.write('constexpr float %s_MUZZLE_Y = %.4ff, %s_MUZZLE_Z = %.4ff;\n' % (PREFIX, muzzle_y, PREFIX, muzzle_z))
    bb = tex.astype(np.uint8).tobytes()
    fo.write('static const uint8_t %s_TEX[] = {\n' % PREFIX)
    for i in range(0, len(bb), 32):
        fo.write('    ' + ','.join(str(x) for x in bb[i:i + 32]) + ',\n')
    fo.write('};\nconstexpr int %s_VERTS = %d;\nstatic const float %s_MESH[] = {  // x y z, nx ny nz, u v\n' % (PREFIX, len(out), PREFIX))
    for o in out:
        fo.write('    ' + ', '.join('%.5ff' % x for x in o) + ',\n')
    fo.write('};\n}  // namespace game\n')

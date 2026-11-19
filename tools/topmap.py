"""topmap.py out.png capdump...: world-space top view of captured 3D vertices (uses each dump's game_cam: eye x, y,
heading/2048, pitch/65536, eye z)."""
import math, re, sys
from PIL import Image, ImageDraw
pts = []; eyes = []
for fn in sys.argv[2:]:
    lines = open(fn).read().split('\n')
    g = re.search(r'game_cam (-?\d+) (-?\d+) (-?\d+) (-?\d+) valid (\d)', lines[1])
    if not g or g.group(5) != '1': continue
    ex, ey, yaw, pitch = [int(v) for v in g.groups()[:4]]
    # eye z is game_cam[4], not printed: read from the .map? use a second regex on line 1 is not possible -> header
    m = re.search(r'eyez (-?\d+)', lines[0])
    ez = int(m.group(1)) if m else 0
    h = yaw * 2 * math.pi / 2048; p = pitch * 2 * math.pi / 65536
    sh, ch, sp, cp = math.sin(h), math.cos(h), math.sin(p), math.cos(p)
    right = (-ch, 0, sh); fwd = (-sh, 0, -ch); down = (0, 1, 0)
    f2 = [fwd[i] * cp - down[i] * sp for i in range(3)]; d2 = [down[i] * cp + fwd[i] * sp for i in range(3)]
    eyes.append((ex, ez))
    for mm in re.finditer(r'\[(-?[\d.]+) (-?[\d.]+) (-?[\d.]+) @', '\n'.join(lines[2:])):
        x, y, z = [float(v) for v in mm.groups()]
        wx = ex + right[0] * x + d2[0] * y + f2[0] * z
        wy = ey + right[1] * x + d2[1] * y + f2[1] * z
        wz = ez + right[2] * x + d2[2] * y + f2[2] * z
        pts.append((wx, wy, wz))
if not pts: print('no points'); sys.exit()
xs = [p[0] for p in pts] + [e[0] for e in eyes]; zs = [p[2] for p in pts] + [e[1] for e in eyes]
x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
S = 900 / max(x1 - x0, z1 - z0, 1)
im = Image.new('RGB', (int((x1 - x0) * S) + 20, int((z1 - z0) * S) + 20), (0, 0, 0)); d = ImageDraw.Draw(im)
ey0 = sum(p[1] for p in pts) / len(pts)
for x, y, z in pts:
    c = int(max(40, min(255, 255 - (y - (-3000)) / 20)))
    d.point((10 + (x - x0) * S, 10 + (z - z0) * S), fill=(c, c, c))
for ex, ez in eyes:
    d.ellipse((10 + (ex - x0) * S - 4, 10 + (ez - z0) * S - 4, 10 + (ex - x0) * S + 4, 10 + (ez - z0) * S + 4), fill=(255, 0, 0))
d.text((5, 5), 'x %d..%d  z %d..%d (x right, z down)' % (x0, x1, z0, z1), fill=(255, 255, 0))
im.save(sys.argv[1]); print('points', len(pts), 'eye', eyes[:1], 'range x', int(x0), int(x1), 'z', int(z0), int(z1))

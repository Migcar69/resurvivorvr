"""stampcheck.py capdump...: world positions of the captured 3D vertices of each frame (placed with its own stamp:
eye, heading, pitch); for consecutive distinct frames, the median distance from each point to the nearest point of the
previous frame. A static scene stays put (millimetres) when every frame is stamped with the view it was drawn with."""
import re, sys, math, numpy as np
from scipy.spatial import cKDTree
prev = None; seen = set()
for fn in sys.argv[1:]:
    t = open(fn).read().split('\n')
    m0 = re.search(r'frame (\d+).*eyez (-?\d+)', t[0]); g = re.search(r'game_cam (-?\d+) (-?\d+) (-?\d+) (-?\d+) valid (\d) yaw16 (-?\d+) pitch16 (-?\d+)', t[1])
    if not m0 or not g or m0.group(1) in seen: continue
    seen.add(m0.group(1))
    ex, ey = int(g.group(1)), int(g.group(2)); ez = int(m0.group(2))
    h = int(g.group(6)) * 2 * math.pi / 65536; p = int(g.group(7)) * 2 * math.pi / 65536
    sh, ch, sp, cp = math.sin(h), math.cos(h), math.sin(p), math.cos(p)
    right = np.array([-ch, 0, sh]); fwd = np.array([-sh, 0, -ch]); down = np.array([0, 1, 0])
    f2 = fwd * cp - down * sp; d2 = down * cp + fwd * sp
    P = np.array([[float(v) for v in mm.groups()] for mm in re.finditer(r'\[(-?[\d.]+) (-?[\d.]+) (-?[\d.]+) @', '\n'.join(t[2:]))])
    W = np.array([ex, ey, ez]) + P[:, :1] * right + P[:, 1:2] * d2 + P[:, 2:3] * f2
    if prev is not None:
        d, _ = cKDTree(prev).query(W)
        print(f'{fn}: frame {m0.group(1)} yaw {math.degrees(h):7.2f}  points {len(W)}  median shift {np.median(d):6.1f} mm  p90 {np.percentile(d,90):6.1f}')
    prev = W

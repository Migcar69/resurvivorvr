"""sparkgrid.py snapshot x,y ...: shoot each GunCon pixel from a VR snapshot (view, H env, DY env) and find the impact
spark nearest to the aim (frame difference against a run without the shot): prints aim -> impact offset."""
import subprocess, sys, os, numpy as np
from PIL import Image
snap = sys.argv[1]
H, DY = os.environ.get('H', '100'), os.environ.get('DY', '0')
t = {l.split()[0]: l.split()[1:] for l in open(snap + '.txt') if l.strip()}
exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'Release', 'resurvivor.exe')
view = f"{t['view_game_yaw_deg'][0]},{t['head_pitch_deg'][0]}"
def run(inp, prefix):
    subprocess.run([exe, '--frames', '18', '--port1', 'guncon', '--gte-h', H, '--gun-dy', DY, '--gun-exact', '--load-state', snap + '.st',
                    '--view-abs', view, '--input', inp, '--shot-every', '1', '--shot', prefix],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return [np.asarray(Image.open(f'{prefix}_{k:05d}.bmp').convert('RGB')).astype(int) for k in range(9, 18)]
ctl = run('0:g160/120', 'ctl')
for a in sys.argv[2:]:
    gx, gy = map(float, a.split(','))
    fr = run(f'0:g{gx:g}/{gy:g},4:g{gx:g}/{gy:g}+o,8:g{gx:g}/{gy:g}', 'sg')
    best = None
    for k, (c, b) in enumerate(zip(ctl, fr)):
        d = np.abs(b - c).sum(2); d[178:, :] = 0
        ys, xs = np.where(d > 90)
        if len(xs) == 0: continue
        dist = np.hypot(xs - gx, ys - gy)
        m = dist < 45
        if m.sum() >= 2:
            # the blob around the nearest changed pixel
            j = np.argmin(dist); cx, cy = xs[j], ys[j]
            mm = (abs(xs - cx) < 6) & (abs(ys - cy) < 6)
            best = (k + 9, xs[mm].mean(), ys[mm].mean(), int(mm.sum())); break
    if best: print(f'aim {gx:5.0f},{gy:5.0f}  impact {best[1]:6.1f},{best[2]:6.1f}  offset {best[1]-gx:+5.1f},{best[2]-gy:+5.1f}  (frame {best[0]}, {best[3]} px)')
    else: print(f'aim {gx:5.0f},{gy:5.0f}  no impact found')

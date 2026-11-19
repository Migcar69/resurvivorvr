"""hitgrid.py snapshot cx cy: shoot a grid of GunCon pixels around cx,cy from a VR snapshot (its view, H, gun_dy) and
report which ones make blood appear (red pixels vs a run without the shot) - where the game's shots really land."""
import subprocess, sys, os, numpy as np
from PIL import Image
snap, cx, cy = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
H, DY = os.environ.get('H', '100'), os.environ.get('DY', '0')
t = {l.split()[0]: l.split()[1:] for l in open(snap + '.txt') if l.strip()}
exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'Release', 'resurvivor.exe')
view = f"{t['view_game_yaw_deg'][0]},{t['head_pitch_deg'][0]}"
def run(inp, prefix):
    subprocess.run([exe, '--frames', '22', '--port1', 'guncon', '--gte-h', H, '--gun-dy', DY, '--gun-exact', '--load-state', snap + '.st',
                    '--view-abs', view, '--input', inp, '--shot-every', '1', '--shot', prefix],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return [np.asarray(Image.open(f'{prefix}_{k:05d}.bmp').convert('RGB')).astype(int) for k in range(12, 22)]
ctl = run(f'0:g{cx:g}/{cy:g}', 'ctl')
for dy in (-20, -12, -6, 0, 6, 12, 20):
    row = []
    for dx in (-12, -6, 0, 6, 12):
        g = f'{cx + dx:g}/{cy + dy:g}'
        fr = run(f'0:g{g},4:g{g}+o,8:g{g}', 'hg')
        red = 0
        for a, b in zip(ctl, fr):
            r = (b[:, :, 0] - b[:, :, 1]) - (a[:, :, 0] - a[:, :, 1])
            r[175:, :] = 0
            red = max(red, int((r > 50).sum()))
        row.append(red)
    print(f'dy {dy:+3d}:', ' '.join(f'{v:4d}' for v in row))

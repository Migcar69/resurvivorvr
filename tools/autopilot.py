"""autopilot.py start.st target_heading legs out_prefix: walk in GunCon mode (trigger off screen) in legs of 120
frames; each leg tries headings around the target and keeps the one that goes furthest along it (development aid to
reach places headless)."""
import math, subprocess, sys, os, numpy as np
exe = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'build', 'Release', 'resurvivor.exe'))
st, target, legs, prefix = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
def pos(f):
    r = np.fromfile(f, dtype=np.int32)
    return r[0xa8788 // 4], r[0xa8790 // 4]
cur = st
tx, tz = -math.sin(target * 2 * math.pi / 4096), -math.cos(target * 2 * math.pi / 4096)
for leg in range(legs):
    best = None
    for dh in (0, -150, 150, -300, 300, -500, 500, -800, 800):
        h = (target + dh) % 4096
        out = f'{prefix}_try.st'
        subprocess.run([exe, '--frames', '121', '--port1', 'guncon', '--load-state', cur, '--input', '0:goff+o',
                        '--poke', f'800a87b6={h}@0', '--ramdump', '0,120', '--save-state', f'{out}@120'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
        x0, z0 = pos('ram_00000.bin'); x1, z1 = pos('ram_00120.bin')
        prog = (x1 - x0) * tx + (z1 - z0) * tz
        if best is None or prog > best[0] + 50:
            best = (prog, h, x1, z1)
            os.replace(out, f'{prefix}_best.st')
        if prog > 2500: break
    cur = f'{prefix}_{leg}.st'
    os.replace(f'{prefix}_best.st', cur)
    print('leg', leg, 'heading', best[1], 'progress', int(best[0]), 'pos', best[2], best[3], flush=True)

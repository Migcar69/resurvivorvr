"""shotcheck.py snapshot(without .st) gx gy [out.png]: replay a GunCon shot from a VR snapshot (its view, H=120) at
pixel gx,gy and save the frames around the impact with the aim marked (development: aim accuracy)."""
import subprocess, sys, os
from PIL import Image, ImageDraw
snap, gx, gy = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
out = sys.argv[4] if len(sys.argv) > 4 else 'shot.png'
t = {l.split()[0]: l.split()[1:] for l in open(snap + '.txt') if l.strip()}
exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'Release', 'resurvivor.exe')
g = f'{gx:g}/{gy:g}'
subprocess.run([exe, '--frames', '24', '--port1', 'guncon', '--gte-h', os.environ.get('H', '100'), '--gun-dy', os.environ.get('DY', '0'), '--gun-exact', '--load-state', snap + '.st',
                '--view-abs', f"{t['view_game_yaw_deg'][0]},{t['head_pitch_deg'][0]}",
                '--input', f'0:g{g},4:g{g}+o,8:g{g}', '--shot-every', '1', '--shot', 'sc'],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
ims = []
for k in (3, 12, 14, 16, 18, 20):
    im = Image.open('sc_%05d.bmp' % k).convert('RGB')
    ImageDraw.Draw(im).ellipse((gx - 4, gy - 4, gx + 4, gy + 4), outline=(0, 255, 0))
    ims.append(im)
o = Image.new('RGB', (960, 480))
for k, i in enumerate(ims): o.paste(i, ((k % 3) * 320, (k // 3) * 240))
o.resize((1920, 960), Image.NEAREST).save(out)

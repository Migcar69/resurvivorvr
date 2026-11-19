"""A longer-barrelled variant of a weapon header (the Custom Magnum from the Magnum: 6" barrel instead of 4").

usage: stretchbarrel.py in.h OLDPREFIX out.h NEWPREFIX z_frame z_tip add_mm
Vertices between z_frame (where the barrel leaves the frame) and z_tip (the front-sight / muzzle part, kept as it is)
are stretched, those ahead of z_tip moved forward: the barrel grows by add_mm (pistol frame: -Z is forward, metres).
The texture and its coordinates stay the same (the barrel's texels get longer).
"""
import os
import re
import sys

src, old, dst, new = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
zf, zt, add = float(sys.argv[5]), float(sys.argv[6]), float(sys.argv[7]) / 1000.0
s = open(src, encoding='utf-8').read()
k = (zf - zt + add) / (zf - zt)  # the stretch between z_frame and z_tip (both negative-going)


def fz(z):
    if z >= zf:
        return z
    if z >= zt:
        return zf + (z - zf) * k
    return z - add


head, body, tail = re.match(r'(.*%s_MESH\[\] = \{[^\n]*\n)(.*?)(\};.*)' % old, s, re.S).groups()
lines = []
for ln in body.splitlines():
    v = [float(x.rstrip('f')) for x in ln.replace(' ', '').rstrip(',').split(',') if x]
    for i in range(0, len(v), 8):
        v[i + 2] = fz(v[i + 2])
    lines.append('    ' + ', '.join('%.5ff' % x for x in v) + ',')
s = head + '\n'.join(lines) + '\n' + tail
m = re.search(r'%s_MUZZLE_Z = (-?[\d.]+)f' % old, s)
s = s.replace(m.group(0), '%s_MUZZLE_Z = %.4ff' % (old, fz(float(m.group(1)))))
s = re.sub(r'\b%s_' % old, new + '_', s)
s = ('// %s: %s with the barrel %.0f mm longer (tools/models/stretchbarrel.py %s)\n' %
     (new, os.path.basename(src), add * 1000, ' '.join(sys.argv[5:]))) + s
with open(dst + '.tmp', 'w', encoding='utf-8') as fo:
    fo.write(s)
os.replace(dst + '.tmp', dst)
print('stretch %.3f, muzzle z %.4f' % (k, fz(float(m.group(1)))))

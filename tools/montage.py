"""Tile screenshots matching a glob into one contact sheet: montage.py out.png 'dir/seq_*.png' [cols]"""
import glob
import sys

from PIL import Image, ImageDraw

out, pattern = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 5
files = sorted(glob.glob(pattern))
ims = [Image.open(f).convert('RGB') for f in files]
w = max(i.width for i in ims)
h = max(i.height for i in ims)
rows = (len(ims) + cols - 1) // cols
sheet = Image.new('RGB', (cols * w, rows * (h + 12)), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for k, (f, im) in enumerate(zip(files, ims)):
    x, y = (k % cols) * w, (k // cols) * (h + 12)
    sheet.paste(im, (x, y + 12))
    d.text((x + 2, y), f.split('\\')[-1].split('/')[-1], fill=(255, 255, 0))
sheet.save(out)

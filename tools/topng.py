import sys
from PIL import Image
for p in sys.argv[1:]:
    Image.open(p).save(p[:-4]+'.png')


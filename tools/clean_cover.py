"""Remove the advert text from the Software Projects Manic Miner advert scan.

usage: clean_cover.py <ManicMiner.jpg> <out.png>
Keeps the art, the "by Matthew Smith" credit and the SOFTWARE PROJECTS logo;
inpaints the blurb paragraph, the C64 line, "Distributors contact" and the address.
"""
import sys
import cv2
import numpy as np

src, out = sys.argv[1], sys.argv[2]
img = cv2.imread(src)
h, w = img.shape[:2]
b, g, r = [img[:, :, i].astype(int) for i in range(3)]
lum = (r * 299 + g * 587 + b * 114) // 1000
mask = np.zeros((h, w), np.uint8)

def box(x0, y0, x1, y1, sel):
    m = np.zeros((h, w), bool)
    m[y0:y1, x0:x1] = True
    mask[m & sel] = 255

dark = lum < 95
red = (r > 130) & (r - g > 45) & (r - b > 45)
box(412, 320, 572, 410, dark)        # blurb paragraph (black text)
box(410, 418, 572, 445, red)         # "AVAILABLE SHORTLY FOR THE COMMODORE 64."
box(410, 686, 540, 708, red)         # "Distributors contact:"
box(405, 774, 562, 862, red)         # address + phone

mask = cv2.dilate(mask, np.ones((3, 3), np.uint8), iterations=2)
res = cv2.inpaint(img, mask, 6, cv2.INPAINT_TELEA)
cv2.imwrite(out, res)

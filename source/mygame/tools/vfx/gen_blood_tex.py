"""Procedural blood-spray sprite (higher resolution replacement for the game's 64x64 spray texture).
The game tints the sprite red (the colour comes from the particle definition), so the picture carries the SHAPE in
alpha and a light grey in RGB. Usage: python gen_blood_tex.py out.png [size] [seed]"""
import sys, math, random
from PIL import Image, ImageDraw, ImageFilter

out = sys.argv[1]
S = int(sys.argv[2]) if len(sys.argv) > 2 else 256
seed = int(sys.argv[3]) if len(sys.argv) > 3 else 7
random.seed(seed)
SS = 4                                   # supersampling for clean edges
W = S * SS
img = Image.new('L', (W, W), 0)
d = ImageDraw.Draw(img)

ox, oy = 0.50 * W, 0.86 * W             # where the spray leaves
main = -math.pi / 2                       # up
spread = math.radians(62)


def blob(cx, cy, rx, ry, ang, val=255):
    pts = []
    n = 14
    for i in range(n):
        t = 2 * math.pi * i / n
        r = 1.0 + random.uniform(-0.18, 0.18)
        x, y = math.cos(t) * rx * r, math.sin(t) * ry * r
        pts.append((cx + x * math.cos(ang) - y * math.sin(ang), cy + x * math.sin(ang) + y * math.cos(ang)))
    d.polygon(pts, fill=val)


# a dense irregular mass at the origin
for _ in range(26):
    a = main + random.uniform(-spread, spread) * 0.8
    r = random.uniform(0.0, 0.10) * W
    blob(ox + math.cos(a) * r, oy + math.sin(a) * r, random.uniform(0.025, 0.06) * W, random.uniform(0.02, 0.05) * W,
         random.uniform(0, 3.14))

# rays of elongated droplets that thin out with distance
for _ in range(95):
    a = main + random.gauss(0, spread * 0.5)
    a = max(main - spread, min(main + spread, a))
    reach = random.uniform(0.18, 0.80) * W
    x, y = ox, oy
    t = 0.0
    while t < reach:
        step = random.uniform(0.012, 0.05) * W
        t += step
        x = ox + math.cos(a) * t + random.uniform(-0.006, 0.006) * W
        y = oy + math.sin(a) * t + random.uniform(-0.006, 0.006) * W
        k = 1.0 - t / reach
        rs = (0.004 + 0.016 * k * random.random()) * W
        blob(x, y, rs * random.uniform(1.0, 2.6), rs, a + random.uniform(-0.2, 0.2))
    # a streak behind some of them
    if random.random() < 0.35:
        w = max(2, int(0.004 * W * random.uniform(0.6, 1.4)))
        d.line([(ox, oy), (ox + math.cos(a) * reach * 0.55, oy + math.sin(a) * reach * 0.55)], fill=200, width=w)

# fine mist of tiny satellite dots around the fan
for _ in range(420):
    a = main + random.gauss(0, spread * 0.65)
    r = random.uniform(0.1, 0.85) * W
    x, y = ox + math.cos(a) * r, oy + math.sin(a) * r
    rr = random.uniform(0.0012, 0.004) * W
    d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=random.randint(150, 255))

# soften the edges a touch so it reads as wet liquid, then bring the alpha back up to crisp
img = img.filter(ImageFilter.GaussianBlur(0.9 * SS))
img = img.point(lambda v: 255 if v > 150 else int(max(0, (v - 40) * 255 / 110)))
img = img.filter(ImageFilter.GaussianBlur(0.35 * SS))
alpha = img.resize((S, S), Image.LANCZOS)

# a little inner variation: thick parts a bit more opaque than the thin rims
rgb = Image.new('RGB', (S, S), (215, 215, 215))
rgba = Image.merge('RGBA', (*rgb.split(), alpha))
rgba.save(out)
print('wrote', out, S)

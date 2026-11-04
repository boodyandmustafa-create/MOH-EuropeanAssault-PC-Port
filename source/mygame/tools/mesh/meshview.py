import os, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(__file__))
from meshan import load, D

def render(m, size=220):
    pos = m['vd'][:, 0, :3].astype(float)
    lo, hi = pos.min(0), pos.max(0)
    c = (lo + hi) / 2
    s = max((hi - lo).max(), 1e-6)
    p = (pos - c) / s
    views = []
    for ax in ((0, 1), (2, 1), (0, 2)):
        im = Image.new('RGB', (size, size), (20, 20, 24))
        d = ImageDraw.Draw(im)
        q = p[:, ax] * (size * 0.45)
        q[:, 1] *= -1
        q += size / 2
        for t in m['idx'][::max(1, len(m['idx']) // 1500)]:
            pts = [tuple(q[i]) for i in t]
            d.line(pts + [pts[0]], fill=(150, 200, 255), width=1)
        views.append(im)
    out = Image.new('RGB', (size * 3, size))
    for i, v in enumerate(views):
        out.paste(v, (i * size, 0))
    return out

if __name__ == '__main__':
    names = sys.argv[2:]
    ims = []
    for n in names:
        m = load(n)
        im = render(m)
        ImageDraw.Draw(im).text((4, 4), '%s %dv %dt mask %x' % (n[:9], m['nv'], m['nt'], m['mask']), fill=(255, 255, 0))
        ims.append(im)
    sheet = Image.new('RGB', (ims[0].width, ims[0].height * len(ims)))
    for i, im in enumerate(ims):
        sheet.paste(im, (0, i * im.height))
    sheet.save(sys.argv[1])

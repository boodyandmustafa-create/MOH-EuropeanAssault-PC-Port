# python -I align_tex.py <his.png|webp> <original.png> <out_dir>
# Lays an AI-enhanced atlas back onto the original atlas: global scale + shift (edge correlation, coarse to fine),
# then a per-block shift found with FFT cross-correlation (fast). Writes the aligned image at 2x the original size.
import sys, os
import numpy as np
from PIL import Image

his = Image.open(sys.argv[1]).convert('RGB')
org = Image.open(sys.argv[2]).convert('RGB')
out = sys.argv[3]
os.makedirs(out, exist_ok=True)
W, H = org.size
S = 2
size = (W * S, H * S)
WORK = (W, H)           # the global search runs at the original size, the result is built at 2x


def edges(im):
    g = np.array(im.convert('L'), float)
    gy, gx = np.gradient(g)
    e = np.hypot(gx, gy)
    return (e - e.mean()) / (e.std() + 1e-9)


def warp(im, sx, sy, tx, ty, sz):
    return im.transform(sz, Image.AFFINE, (sx, 0, tx, 0, sy, ty), resample=Image.BICUBIC)


hw, hh = his.size
B = edges(org)


def score(p):
    return float((edges(warp(his, p[0], p[1], p[2], p[3], WORK)) * B).mean())


p = [hw / WORK[0], hh / WORK[1], 0.0, 0.0]
best = score(p)
print('start corr %.4f' % best, flush=True)
for st in [(0.02, 0.02, 10, 10), (0.01, 0.01, 5, 5), (0.004, 0.004, 2, 2), (0.0015, 0.0015, 0.7, 0.7)]:
    for _ in range(5):
        moved = False
        for i in range(4):
            for d in (-1, 1):
                q = list(p)
                q[i] += d * st[i] * (p[0] if i < 2 else 1.0)
                s = score(q)
                if s > best:
                    best, p, moved = s, q, True
        if not moved:
            break
    print('step', st, 'corr %.4f' % best, [round(v, 4) for v in p], flush=True)

# 2x version of the globally aligned image
p2 = [p[0] / S, p[1] / S, p[2], p[3]]      # output pixels are S times finer: his = out * (s / S) + t
res = warp(his, p2[0], p2[1], p2[2], p2[3], size)
res.save(os.path.join(out, 'aligned_global_%dx%d.png' % size))
Image.blend(res.resize((W, H), Image.LANCZOS), org, 0.5).save(os.path.join(out, 'aligned_global_overlay.png'))
if '--blocks' not in sys.argv:
    print('saved (global only)', flush=True)
    sys.exit(0)
A = edges(res)
Bn = edges(org.resize(size, Image.LANCZOS))
blk, R = 256, 24
for y0 in range(0, size[1], blk):
    for x0 in range(0, size[0], blk):
        y1, x1 = min(y0 + blk, size[1]), min(x0 + blk, size[0])
        a, b = A[y0:y1, x0:x1], Bn[y0:y1, x0:x1]
        F = np.fft.irfft2(np.conj(np.fft.rfft2(b)) * np.fft.rfft2(a), s=a.shape)
        F = np.fft.fftshift(F)
        cy, cx = a.shape[0] // 2, a.shape[1] // 2
        win = F[cy - R:cy + R + 1, cx - R:cx + R + 1]
        iy, ix = np.unravel_index(np.argmax(win), win.shape)
        dy, dx = iy - R, ix - R
        if (dx, dy) != (0, 0):
            q = [p2[0], p2[1], p2[2] + dx, p2[3] + dy]
            res.paste(warp(his, q[0], q[1], q[2], q[3], size).crop((x0, y0, x1, y1)), (x0, y0))
        print('block', (x0, y0), 'shift', (dx, dy), flush=True)
res.save(os.path.join(out, 'aligned_%dx%d.png' % size))
Image.blend(res.resize((W, H), Image.LANCZOS), org, 0.5).save(os.path.join(out, 'aligned_overlay.png'))
print('saved', flush=True)

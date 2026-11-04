import sys, os
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(__file__))
from meshan import load

def norm(v):
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.maximum(n, 1e-9)

def phong_subdiv(P, N, F, alpha=0.75):
    """P,N: (n,3); F: (t,3). One level 1->4 with Phong tessellation midpoints."""
    edge = {}
    newP, newN = [], []
    n0 = len(P)
    def mid(a, b):
        k = (a, b) if a < b else (b, a)
        if k in edge:
            return edge[k]
        pa, pb, na, nb = P[a], P[b], N[a], N[b]
        p = (pa + pb) / 2
        pr = lambda pi, ni: p - np.dot(p - pi, ni) * ni
        q = (pr(pa, na) + pr(pb, nb)) / 2
        pp = (1 - alpha) * p + alpha * q
        nn = na + nb
        nn = nn / max(np.linalg.norm(nn), 1e-9)
        edge[k] = n0 + len(newP)
        newP.append(pp)
        newN.append(nn)
        return edge[k]
    G = []
    for a, b, c in F:
        ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
        G += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
    return np.vstack([P, np.array(newP)]), np.vstack([N, np.array(newN)]), np.array(G)

def raster(P, N, F, W=520, H=700, flip=False):
    # front view: x -> screen x, z -> screen y (up), depth = y
    lo, hi = P.min(0), P.max(0)
    s = min(W / (hi[0] - lo[0]), H / (hi[2] - lo[2])) * 0.92
    cx, cz = (lo[0] + hi[0]) / 2, (lo[2] + hi[2]) / 2
    sx = (P[:, 0] - cx) * s + W / 2
    sy = H / 2 - (P[:, 2] - cz) * s
    dep = P[:, 1] * (-1 if flip else 1)
    img = np.zeros((H, W, 3), np.float32) + 0.08
    zb = np.full((H, W), 1e9, np.float32)
    L = norm(np.array([0.4, -0.6, 0.7]))
    for a, b, c in F:
        xs = sx[[a, b, c]]; ys = sy[[a, b, c]]
        x0, x1 = int(max(0, np.floor(xs.min()))), int(min(W - 1, np.ceil(xs.max())))
        y0, y1 = int(max(0, np.floor(ys.min()))), int(min(H - 1, np.ceil(ys.max())))
        if x1 < x0 or y1 < y0:
            continue
        d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        if abs(d) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / d
        w1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / d
        w2 = 1 - w0 - w1
        m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not m.any():
            continue
        z = w0 * dep[a] + w1 * dep[b] + w2 * dep[c]
        nn = w0[..., None] * N[a] + w1[..., None] * N[b] + w2[..., None] * N[c]
        nn = norm(nn)
        sh = np.clip((nn * L).sum(-1), 0, 1) * 0.8 + 0.2
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        upd = m & (z < sub)
        sub[upd] = z[upd]
        img[y0:y1 + 1, x0:x1 + 1][upd] = (sh[upd, None] * np.array([0.75, 0.8, 0.7]))
    return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))

def unique_pos(m):
    vd = m['vd']
    return vd[:, 0, :3].astype(float), vd[:, m['attrs'].index(3), :3].astype(float), m['idx'].astype(int)

if __name__ == '__main__':
    fn = sys.argv[1]; out = sys.argv[2]; levels = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    m = load(fn)
    P, N, F = unique_pos(m)
    N = norm(N)
    imgs = [raster(P, N, F, flip=len(sys.argv) > 4)]
    P2, N2, F2 = P, N, F
    for _ in range(levels):
        P2, N2, F2 = phong_subdiv(P2, N2, F2)
    imgs.append(raster(P2, N2, F2, flip=len(sys.argv) > 4))
    print('before', len(F), 'tris', len(P), 'verts; after', len(F2), 'tris', len(P2), 'verts')
    sheet = Image.new('RGB', (imgs[0].width * 2, imgs[0].height))
    sheet.paste(imgs[0], (0, 0)); sheet.paste(imgs[1], (imgs[0].width, 0))
    sheet.save(out)

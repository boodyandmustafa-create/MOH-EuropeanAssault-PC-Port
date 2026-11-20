import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import meshan
from meshan import load

TH = int(sys.argv[1], 16)
B = []
for fn in sorted(os.listdir(meshan.D)):
    if not fn.endswith('.bin'):
        continue
    m = load(fn)
    if m['mask'] == 0x1f and m['th'] == TH:
        B.append((fn, m))
B.sort(key=lambda b: -b[1]['nt'])


def share(a, b, tol=0.01):
    pa = a['vd'][:, 0, :3]
    pb = b['vd'][:, 0, :3]
    hit = 0
    for i in range(0, len(pa), 64):
        d = np.linalg.norm(pa[i:i + 64, None, :] - pb[None, :, :], axis=2).min(1)
        hit += (d < tol).sum()
    return hit / len(pa)


names = [b[0][:9] for b in B]
print('batches', [(b[0][:9], b[1]['nv'], b[1]['nt']) for b in B])
print('share of ROW vertices lying within 1 cm of COLUMN batch')
print(' ' * 10 + ' '.join('%9s' % n for n in names))
for i, (fa, a) in enumerate(B):
    print('%9s ' % names[i] + ' '.join('%9.2f' % (1.0 if i == j else share(a, b)) for j, (fb, b) in enumerate(B)))
for fn, m in B:
    p = m['vd'][:, 0, :3]
    print(fn[:9], 'bbox', p.min(0).round(2), p.max(0).round(2), 'bones', len(np.unique(m['vd'][:, 4, 0])))

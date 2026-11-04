import os, struct, sys
import numpy as np
D = r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\mesh_dump'

def load(fn):
    b = open(os.path.join(D, fn), 'rb').read()
    magic, nv, nt, mask, nins, nattr = struct.unpack('<6I', b[:24])
    th, tw, tH = struct.unpack('<QII', b[24:40])
    off = 40
    cst = np.frombuffer(b[off:off + 192 * 16], dtype='<f4').reshape(192, 4); off += 192 * 16
    attrs = [a for a in range(16) if mask >> a & 1]
    vd = np.frombuffer(b[off:off + nv * nattr * 16], dtype='<f4').reshape(nv, nattr, 4); off += nv * nattr * 16
    idx = np.frombuffer(b[off:off + nt * 12], dtype='<u4').reshape(nt, 3)
    return dict(nv=nv, nt=nt, mask=mask, nins=nins, th=th, tw=tw, tH=tH, cst=cst, attrs=attrs, vd=vd, idx=idx)

if __name__ == '__main__':
    rows = []
    for fn in sorted(os.listdir(D)):
        if not fn.endswith('.bin'):
            continue
        m = load(fn)
        pos = m['vd'][:, 0, :3]
        ext = pos.max(0) - pos.min(0)
        a1 = m['vd'][:, m['attrs'].index(1), :] if 1 in m['attrs'] else None
        rows.append((fn, m['nv'], m['nt'], hex(m['mask']), m['nins'], ext.round(2), None if a1 is None else (a1.min(0).round(2), a1.max(0).round(2))))
    rows.sort(key=lambda r: -r[2])
    for r in rows[:40]:
        print(*r)
    print(len(rows))

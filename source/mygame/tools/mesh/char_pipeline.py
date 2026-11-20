# Character pipeline: dump batches -> (optional) subdivision across ALL batches of the soldier -> mods\models\*.mdl
#
#   python -I char_pipeline.py gate   <texhash>                 L0 export must equal the dump (offline gate)
#   python -I char_pipeline.py export <texhash> <level> [outdir]  subdivide <level> times and write the .mdl files
#
# .mdl ("MDL1", little endian): u32 magic, h32, nins, orig_nv, orig_nt, mask, nattr, nv, nt,
#   nv * nattr * 4 floats (attribute order = ascending attribute number, like the dump), then nt * 3 u32 indices.
# The game looks a draw up by (h32, nins, orig_nv, orig_nt) -- the dump file name carries h32, orig_nv, orig_nt.
import os, re, struct, sys
import numpy as np

ROOT = r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame'
DUMP = os.path.join(ROOT, 'mesh_dump')
MAGIC = 0x314C444D
BONE = 4            # attribute number holding the (palette offset) bone value
NRM = 3
UV = 1


def load_dump(fn):
    b = open(os.path.join(DUMP, fn), 'rb').read()
    magic, nv, nt, mask, nins, nattr = struct.unpack('<6I', b[:24])
    th, tw, tH = struct.unpack('<QII', b[24:40])
    off = 40 + 192 * 16
    vd = np.frombuffer(b[off:off + nv * nattr * 16], dtype='<f4').reshape(nv, nattr, 4).astype(np.float64)
    off += nv * nattr * 16
    idx = np.frombuffer(b[off:off + nt * 12], dtype='<u4').reshape(nt, 3).astype(np.int64)
    m = re.match(r'm([0-9a-f]{8})_(\d+)v_(\d+)t\.bin', fn)
    return dict(name=fn[:-4], h32=int(m.group(1), 16), nv=nv, nt=nt, orig_nv=nv, orig_nt=nt, mask=mask, nins=nins,
                nattr=nattr, th=th, vd=vd, idx=idx)


def soldier(texhash):
    out = []
    for fn in sorted(os.listdir(DUMP)):
        if fn.endswith('.bin'):
            m = load_dump(fn)
            if m['mask'] == 0x1f and m['th'] == texhash:
                out.append(m)
    return out


def unit(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-9)


def qkey(P):
    return np.round(P * 1e4).astype(np.int64)


def boundary_set(m, grp):
    """Edges (as welded-position pairs) that belong to exactly one triangle of THIS batch."""
    F = m['idx']
    e = np.sort(np.stack([grp[F[:, [0, 1]]], grp[F[:, [1, 2]]], grp[F[:, [2, 0]]]], 1).reshape(-1, 2), axis=1)
    u, cnt = np.unique(e, axis=0, return_counts=True)
    return {(int(a), int(b)) for (a, b) in u[cnt == 1]}


def subdivide_all(batches, alpha=0.6):
    """One 1->4 level for every batch of the soldier at once.
    - smooth normals and crease flags come from ALL batches together (a vertex on a batch seam gets one answer);
    - an edge stays a plain linear midpoint when it is a hard edge, open in its own batch, or joins two bones;
    - the midpoint's bone is that of the endpoint with the smaller position (same answer for every copy of the edge)."""
    allP = np.concatenate([m['vd'][:, 0, :3] for m in batches])
    allN = np.concatenate([m['vd'][:, NRM, :3] for m in batches])
    _, inv = np.unique(qkey(allP), axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    ng = np.zeros((inv.max() + 1, 3))
    np.add.at(ng, inv, allN)
    SNall = unit(ng)[inv]
    creaseall = (allN * SNall).sum(1) < 0.8
    out, base = [], 0
    for m in batches:
        n = m['nv']
        vd, F = m['vd'], m['idx']
        P = vd[:, 0, :3]
        grp = inv[base:base + n]
        SN, crease = SNall[base:base + n], creaseall[base:base + n]
        bset = boundary_set(m, grp)
        bone = vd[:, BONE, 0]
        edge, rows = {}, []

        def mid(a, b):
            k = (a, b) if a < b else (b, a)
            if k in edge:
                return edge[k]
            pa, pb = P[a], P[b]
            p = (pa + pb) / 2
            gk = (int(grp[a]), int(grp[b]))
            gk = gk if gk[0] < gk[1] else (gk[1], gk[0])
            flat = crease[a] or crease[b] or gk in bset or bone[a] != bone[b]
            if flat:
                pp = p
            else:
                q = (p - np.dot(p - pa, SN[a]) * SN[a] + p - np.dot(p - pb, SN[b]) * SN[b]) / 2
                pp = (1 - alpha) * p + alpha * q
            row = (vd[a] + vd[b]) / 2
            row[0, :3] = pp
            row[NRM, :3] = unit(vd[a, NRM, :3] + vd[b, NRM, :3])
            src = a if tuple(P[a]) + (bone[a],) <= tuple(P[b]) + (bone[b],) else b
            row[BONE] = vd[src, BONE]
            edge[k] = n + len(rows)
            rows.append(row)
            return edge[k]

        G = []
        for a, b, c in F:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            G += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
        o = dict(m)
        o['vd'] = np.concatenate([vd, np.array(rows)])
        o['idx'] = np.array(G, dtype=np.int64)
        o['nv'], o['nt'] = len(o['vd']), len(G)
        out.append(o)
        base += n
    return out


def check(before, after):
    for a, b in zip(before, after):
        assert set(np.unique(b['vd'][:, BONE, 0])) <= set(np.unique(a['vd'][:, BONE, 0])), 'new bone value appeared'
        assert b['idx'].max() < b['nv'] and b['idx'].min() >= 0
        assert np.isfinite(b['vd']).all()


def write_mdl(m, path):
    with open(path, 'wb') as f:
        f.write(struct.pack('<9I', MAGIC, m['h32'], m['nins'], m['orig_nv'], m['orig_nt'], m['mask'], m['nattr'],
                            m['nv'], m['nt']))
        f.write(m['vd'].astype('<f4').tobytes())
        f.write(m['idx'].astype('<u4').tobytes())


def read_mdl(path):
    b = open(path, 'rb').read()
    magic, h32, nins, onv, ont, mask, nattr, nv, nt = struct.unpack('<9I', b[:36])
    assert magic == MAGIC
    vd = np.frombuffer(b[36:36 + nv * nattr * 16], dtype='<f4').reshape(nv, nattr, 4).astype(np.float64)
    idx = np.frombuffer(b[36 + nv * nattr * 16:36 + nv * nattr * 16 + nt * 12], dtype='<u4').reshape(nt, 3)
    return dict(h32=h32, nins=nins, orig_nv=onv, orig_nt=ont, mask=mask, nattr=nattr, nv=nv, nt=nt, vd=vd,
                idx=idx.astype(np.int64))


def mdl_name(m):
    return 'm%08x_%dv_%dt.mdl' % (m['h32'], m['orig_nv'], m['orig_nt'])


if __name__ == '__main__':
    cmd, th = sys.argv[1], int(sys.argv[2], 16)
    bl = soldier(th)
    print('batches:', [(m['name'][:9], m['nv'], m['nt']) for m in bl])
    if cmd == 'gate':
        tmp = os.path.join(ROOT, 'char_test', 'gate')
        os.makedirs(tmp, exist_ok=True)
        ok = True
        for m in bl:
            p = os.path.join(tmp, mdl_name(m))
            write_mdl(m, p)
            r = read_mdl(p)
            d = load_dump(m['name'] + '.bin')
            e = np.abs(r['vd'] - d['vd']).max()
            same = (r['idx'] == d['idx']).all() and r['nt'] == d['nt'] and e < 1e-5 and \
                (r['vd'][:, BONE] == d['vd'][:, BONE]).all()
            ok &= bool(same)
            print(m['name'][:9], 'max diff %.2g' % e, 'tris', r['nt'], 'OK' if same else 'MISMATCH')
        print('GATE', 'PASS' if ok else 'FAIL')
    elif cmd == 'export':
        level = int(sys.argv[3])
        out = sys.argv[4] if len(sys.argv) > 4 else os.path.join(ROOT, 'char_test', 'models_L%d' % level)
        os.makedirs(out, exist_ok=True)
        cur = bl
        for _ in range(level):
            nxt = subdivide_all(cur)
            check(cur, nxt)
            cur = nxt
        for m in cur:
            write_mdl(m, os.path.join(out, mdl_name(m)))
            print('wrote', mdl_name(m), m['nv'], 'verts', m['nt'], 'tris')
        print('total tris', sum(m['nt'] for m in cur))

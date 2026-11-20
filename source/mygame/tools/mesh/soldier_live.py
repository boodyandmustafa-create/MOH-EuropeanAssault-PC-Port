# Live pipeline for one dumped soldier.
#   GUI (you watch):  blender.exe -P soldier_live.py -- <texhash> --show
#   headless check:   blender.exe -b -P soldier_live.py -- <texhash> --check <out_dir>
# Steps: import the batches (one object per draw batch) -> weld report -> Phong subdivision level 1 and 2 (per batch,
# bone index copied from an original vertex, never interpolated) -> side by side original | L1 | L2.
import os, struct, sys, math
import numpy as np
import bpy

ROOT = r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame'
argv = sys.argv[sys.argv.index('--') + 1:]
TH = int(argv[0], 16)
SHOW = '--show' in argv
CHECK = argv[argv.index('--check') + 1] if '--check' in argv else None
SPACING = 1.7


def load(fn):
    b = open(fn, 'rb').read()
    magic, nv, nt, mask, nins, nattr = struct.unpack('<6I', b[:24])
    th, tw, tH = struct.unpack('<QII', b[24:40])
    off = 40 + 192 * 16
    vd = np.frombuffer(b[off:off + nv * nattr * 16], dtype='<f4').reshape(nv, nattr, 4)
    off += nv * nattr * 16
    idx = np.frombuffer(b[off:off + nt * 12], dtype='<u4').reshape(nt, 3)
    return dict(nv=nv, nt=nt, mask=mask, th=th, vd=vd.astype(np.float64), idx=idx.astype(np.int64))


def batches():
    D = os.path.join(ROOT, 'mesh_dump')
    out = []
    for fn in sorted(os.listdir(D)):
        if fn.endswith('.bin'):
            m = load(os.path.join(D, fn))
            if m['mask'] == 0x1f and m['th'] == TH:
                m['name'] = fn[:-4]
                out.append(m)
    return out


def unit(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-9)


def weld_groups(P):
    key = np.round(P * 1e4).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    return inv.reshape(-1)


def boundary_edges(F, grp):
    e = np.sort(np.stack([grp[F[:, [0, 1]]], grp[F[:, [1, 2]]], grp[F[:, [2, 0]]]], 1).reshape(-1, 2), axis=1)
    _, cnt = np.unique(e, axis=0, return_counts=True)
    return int((cnt == 1).sum()), int((cnt > 2).sum())


def subdivide(m, alpha=0.6):
    """One 1->4 level. Position = Phong tessellation on welded smooth normals (stays on the surface, no shrinking),
    attributes (uv, normal, colour) from the endpoints; the bone value is copied from endpoint a, never averaged."""
    vd, F = m['vd'], m['idx']
    P = vd[:, 0, :3]
    grp = weld_groups(P)
    ng = np.zeros((grp.max() + 1, 3))
    np.add.at(ng, grp, vd[:, 3, :3])
    SN = unit(ng)[grp]                       # smooth normal shared by every copy of the same position
    # a vertex sits on a hard edge when its own normal disagrees with the average of its copies: edges touching one stay
    # flat on BOTH sides of the crease (same midpoint for both copies, so no cracks and no pillow-shaped boxes)
    crease = (vd[:, 3, :3] * SN).sum(1) < 0.8
    edge, newv = {}, []
    n0 = len(P)

    def mid(a, b):
        k = (a, b) if a < b else (b, a)
        if k in edge:
            return edge[k]
        pa, pb = P[a], P[b]
        p = (pa + pb) / 2
        q = (p - np.dot(p - pa, SN[a]) * SN[a] + p - np.dot(p - pb, SN[b]) * SN[b]) / 2
        row = (vd[a] + vd[b]) / 2
        flat = crease[a] or crease[b]
        row[0, :3] = p if flat else (1 - alpha) * p + alpha * q
        row[3, :3] = unit(vd[a, 3, :3] + vd[b, 3, :3])
        row[4] = vd[k[0], 4]                 # bone: copied, not interpolated
        edge[k] = n0 + len(newv)
        newv.append(row)
        return edge[k]

    G = []
    for a, b, c in F:
        ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
        G += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
    out = dict(m)
    out['vd'] = np.concatenate([vd, np.array(newv)])
    out['idx'] = np.array(G, dtype=np.int64)
    out['nv'], out['nt'] = len(out['vd']), len(G)
    return out


def make_material():
    tex = [f for f in os.listdir(os.path.join(ROOT, 'tex_dump')) if f.startswith('%016x' % TH) and f.endswith('.png')]
    img = bpy.data.images.load(os.environ.get('SOLDIER_TEX') or os.path.join(ROOT, 'tex_dump', tex[0]))
    mat = bpy.data.materials.new('soldier')
    mat.use_nodes = True
    nt_ = mat.node_tree
    for n in list(nt_.nodes):
        nt_.nodes.remove(n)
    tn = nt_.nodes.new('ShaderNodeTexImage')
    tn.image = img
    em = nt_.nodes.new('ShaderNodeEmission')
    outn = nt_.nodes.new('ShaderNodeOutputMaterial')
    nt_.links.new(tn.outputs['Color'], em.inputs['Color'])
    nt_.links.new(em.outputs['Emission'], outn.inputs['Surface'])
    return mat


def to_object(m, mat, name, dx):
    vd = m['vd']
    me = bpy.data.meshes.new(name)
    me.from_pydata(vd[:, 0, :3].tolist(), [], m['idx'].tolist())
    me.update()
    uvl = me.uv_layers.new(name='UV')
    uv = vd[:, 1, :2]
    for poly in me.polygons:
        for li, vi in zip(poly.loop_indices, poly.vertices):
            uvl.data[li].uv = (uv[vi][0], 1.0 - uv[vi][1])
    me.materials.append(mat)
    ob = bpy.data.objects.new(name, me)
    ob.location = (dx, 0, 0)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def build_level(level, mat, dx, bl):
    objs = []
    for m in bl:
        mm = m
        for _ in range(level):
            mm = subdivide(mm)
        objs.append(to_object(mm, mat, '%s_L%d' % (m['name'][:9], level), dx))
        yield mm, objs
    return


def report(bl):
    tot = 0
    for m in bl:
        P = m['vd'][:, 0, :3]
        grp = weld_groups(P)
        b, nm = boundary_edges(m['idx'], grp)
        print('[batch %s] %dv (%d welded) %dt, boundary edges %d, non-manifold %d, bones %d' % (
            m['name'][:9], m['nv'], grp.max() + 1, m['nt'], b, nm, len(np.unique(m['vd'][:, 4, 0]))))
        tot += m['nt']
    print('[total] %d triangles' % tot)


def bones_ok(a, b):
    return set(np.unique(a['vd'][:, 4, 0])) == set(np.unique(b['vd'][:, 4, 0]))


def setup_view(frame=True):
    for area in bpy.context.screen.areas:
        if area.type == 'VIEW_3D':
            sp = area.spaces[0]
            sp.shading.type = 'SOLID'
            sp.shading.light = 'FLAT'
            sp.shading.color_type = 'TEXTURE'
            sp.overlay.show_floor = False
            sp.overlay.show_axis_x = sp.overlay.show_axis_y = False
            for reg in area.regions:
                if reg.type == 'WINDOW':
                    with bpy.context.temp_override(area=area, region=reg):
                        bpy.ops.view3d.view_axis(type='FRONT')
                        bpy.ops.view3d.view_all()


def run_show(bl):
    mat = make_material()
    steps = []
    state = {'objs': {}, 'ms': {}}

    def s_import():
        for m in bl:
            state['objs'].setdefault(0, []).append(to_object(m, mat, m['name'][:9] + '_orig', -SPACING))
        setup_view()
        print('step 1: imported %d batches' % len(bl))

    def s_weld():
        report(bl)

    def s_l1():
        cur = []
        for m in bl:
            m1 = subdivide(m)
            assert bones_ok(m, m1)
            cur.append(m1)
            to_object(m1, mat, m['name'][:9] + '_L1', 0.0)
        state['ms'][1] = cur
        setup_view()
        print('step 3: level 1 (%d tris)' % sum(c['nt'] for c in cur))

    def s_l2():
        cur = []
        for m1 in state['ms'][1]:
            m2 = subdivide(m1)
            assert bones_ok(m1, m2)
            cur.append(m2)
            to_object(m2, mat, m1['name'][:9] + '_L2', SPACING)
        print('step 4: level 2 (%d tris)' % sum(c['nt'] for c in cur))
        setup_view()
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(ROOT, 'char_test', 'soldier1', 'soldier_live.blend'))

    steps = [s_import, s_weld, s_l1, s_l2]

    def tick():
        if steps:
            steps.pop(0)()
            return 3.0
        return None

    bpy.app.timers.register(tick, first_interval=2.0)


def run_check(bl):
    os.makedirs(CHECK, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mat = make_material()
    report(bl)
    lv = {0: bl}
    for i in (1, 2):
        lv[i] = [subdivide(m) for m in lv[i - 1]]
        assert all(bones_ok(a, b) for a, b in zip(lv[i - 1], lv[i]))
        print('level %d: %d tris' % (i, sum(m['nt'] for m in lv[i])))
    allp = []
    for i, dx in ((0, -SPACING), (1, 0.0), (2, SPACING)):
        for m in lv[i]:
            to_object(m, mat, '%s_L%d' % (m['name'][:9], i), dx)
        allp.append(np.concatenate([m['vd'][:, 0, :3] for m in lv[i]]))
    for i in (1, 2):
        print('bbox L%d vs L0 (shrink check):' % i, (allp[i].min(0) - allp[0].min(0)).round(3), (allp[i].max(0) - allp[0].max(0)).round(3))
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'
    sc.display.shading.light = 'FLAT'
    sc.display.shading.color_type = 'TEXTURE'
    sc.render.resolution_x, sc.render.resolution_y = 2400, 800
    sc.world = bpy.data.worlds.new('w')
    sc.world.use_nodes = True
    sc.world.node_tree.nodes['Background'].inputs[0].default_value = (0.2, 0.2, 0.2, 1)
    cam = bpy.data.cameras.new('cam')
    cam.type = 'ORTHO'
    cam.ortho_scale = 5.6
    co = bpy.data.objects.new('cam', cam)
    sc.collection.objects.link(co)
    sc.camera = co
    for name, a in (('front', 0.0), ('side', 90.0)):
        r = math.radians(a)
        co.location = (4 * math.sin(r) if a else 0.0, -4 * math.cos(r), 0.86)
        co.rotation_euler = (math.radians(90), 0, r)
        sc.render.filepath = os.path.join(CHECK, 'levels_%s.png' % name)
        bpy.ops.render.render(write_still=True)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(CHECK, 'soldier_levels.blend'))
    print('check done')


bl = batches()
if CHECK:
    run_check(bl)
elif SHOW:
    run_show(bl)

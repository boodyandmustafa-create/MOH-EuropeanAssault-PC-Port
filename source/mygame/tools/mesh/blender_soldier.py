# blender -b -P blender_soldier.py -- <texhash> <out_dir>
# Imports every character batch (mask 0x1f) of one texture hash from mesh_dump, textures it with the dumped PNG,
# saves a .blend and renders front / right / back / left orthographic views.
import os, struct, sys, math
import numpy as np
import bpy
bpy.ops.wm.read_factory_settings(use_empty=True)
from mathutils import Vector

ROOT = r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame'
argv = sys.argv[sys.argv.index('--') + 1:]
TH = int(argv[0], 16)
OUT = os.path.abspath(argv[1]) if os.path.isabs(argv[1]) else os.path.join(ROOT, argv[1])
os.makedirs(OUT, exist_ok=True)


def load(fn):
    b = open(fn, 'rb').read()
    magic, nv, nt, mask, nins, nattr = struct.unpack('<6I', b[:24])
    th, tw, tH = struct.unpack('<QII', b[24:40])
    off = 40 + 192 * 16
    attrs = [a for a in range(16) if mask >> a & 1]
    vd = np.frombuffer(b[off:off + nv * nattr * 16], dtype='<f4').reshape(nv, nattr, 4)
    off += nv * nattr * 16
    idx = np.frombuffer(b[off:off + nt * 12], dtype='<u4').reshape(nt, 3)
    return dict(nv=nv, nt=nt, mask=mask, th=th, attrs=attrs, vd=vd, idx=idx)


tex = [f for f in os.listdir(os.path.join(ROOT, 'tex_dump')) if f.startswith('%016x' % TH) and f.endswith('.png')]
img = bpy.data.images.load(os.path.join(ROOT, 'tex_dump', tex[0]))

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

D = os.path.join(ROOT, 'mesh_dump')
objs = []
for fn in sorted(os.listdir(D)):
    if not fn.endswith('.bin'):
        continue
    m = load(os.path.join(D, fn))
    if m['mask'] != 0x1f or m['th'] != TH:
        continue
    v = m['vd']
    pos = v[:, 0, :3]
    uv = v[:, 1, :2]
    nrm = v[:, 3, :3]
    me = bpy.data.meshes.new(fn[:-4])
    me.from_pydata(pos.tolist(), [], m['idx'].tolist())
    me.update()
    uvl = me.uv_layers.new(name='UV')
    for poly in me.polygons:
        for li, vi in zip(poly.loop_indices, poly.vertices):
            u, w = uv[vi]
            uvl.data[li].uv = (u, 1.0 - w)
    me.materials.append(mat)
    ob = bpy.data.objects.new(fn[:-4], me)
    bpy.context.scene.collection.objects.link(ob)
    objs.append(ob)
    print('imported', fn, m['nv'], m['nt'])

allv = np.concatenate([np.array([ob.matrix_world @ v.co for v in ob.data.vertices]) for ob in objs])
lo, hi = allv.min(0), allv.max(0)
print('bounds', lo, hi)
c = (lo + hi) / 2
size = float((hi - lo).max()) * 1.15

sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'FLAT'
sc.display.shading.color_type = 'TEXTURE'
sc.display.shading.background_type = 'WORLD'
sc.render.resolution_x = sc.render.resolution_y = 1024
sc.render.film_transparent = False
sc.world = bpy.data.worlds.new('w')
sc.world.use_nodes = True
sc.world.node_tree.nodes['Background'].inputs[0].default_value = (0.5, 0.5, 0.5, 1)
sc.view_settings.view_transform = 'Standard'

cam = bpy.data.cameras.new('cam')
cam.type = 'ORTHO'
cam.ortho_scale = size
co = bpy.data.objects.new('cam', cam)
sc.collection.objects.link(co)
sc.camera = co
names = ['front', 'right', 'back', 'left']
for i, nme in enumerate(names):
    a = math.radians(90 * i)
    d = 5.0
    # front camera looks from -Y towards +Y
    co.location = (c[0] + d * math.sin(a), c[1] - d * math.cos(a), c[2])
    co.rotation_euler = (math.radians(90), 0, a)
    sc.render.filepath = os.path.join(OUT, nme + '.png')
    bpy.ops.render.render(write_still=True)
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, 'soldier_original.blend'))
print('done')

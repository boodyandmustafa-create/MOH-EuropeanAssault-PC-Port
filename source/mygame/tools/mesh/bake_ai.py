# blender -b -P bake_ai.py -- <game texhash> <ai.glb> <out_dir> [align|bake] [--rot180] [--rotz DEG]
# Lays an AI / third-party soldier over the dumped game soldier and bakes its colour onto the game soldier's ORIGINAL
# UV layout (so every LOD and every other user of that texture keeps working, no model replacement).
import os, struct, sys, math
import numpy as np
import bpy
from mathutils import Vector

bpy.ops.wm.read_factory_settings(use_empty=True)
ROOT = r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame'
a = sys.argv[sys.argv.index('--') + 1:]
TH, GLB = int(a[0], 16), a[1]
OUT = a[2] if os.path.isabs(a[2]) else os.path.join(ROOT, a[2])
STAGE = a[3] if len(a) > 3 and not a[3].startswith('--') else 'align'
ROT = 180.0 if '--rot180' in a else (float(a[a.index('--rotz') + 1]) if '--rotz' in a else 0.0)
os.makedirs(OUT, exist_ok=True)


def load(fn):
    b = open(fn, 'rb').read()
    magic, nv, nt, mask, nins, nattr = struct.unpack('<6I', b[:24])
    th = struct.unpack('<Q', b[24:32])[0]
    off = 40 + 192 * 16
    vd = np.frombuffer(b[off:off + nv * nattr * 16], dtype='<f4').reshape(nv, nattr, 4)
    off += nv * nattr * 16
    idx = np.frombuffer(b[off:off + nt * 12], dtype='<u4').reshape(nt, 3)
    return nv, nt, mask, th, vd.astype(np.float64), idx.astype(np.int64)


# ---- the game soldier (low): every batch of this texture in ONE object, original UVs
P, UVs, F = [], [], []
base = 0
D = os.path.join(ROOT, 'mesh_dump')
for fn in sorted(os.listdir(D)):
    if fn.endswith('.bin'):
        nv, nt, mask, th, vd, idx = load(os.path.join(D, fn))
        if mask == 0x1f and th == TH:
            P.append(vd[:, 0, :3]); UVs.append(vd[:, 1, :2]); F.append(idx + base); base += nv
P, UVs, F = np.concatenate(P), np.concatenate(UVs), np.concatenate(F)
me = bpy.data.meshes.new('low')
me.from_pydata(P.tolist(), [], F.tolist())
me.update()
uvl = me.uv_layers.new(name='UV')
for poly in me.polygons:
    for li, vi in zip(poly.loop_indices, poly.vertices):
        uvl.data[li].uv = (UVs[vi][0], 1.0 - UVs[vi][1])
low = bpy.data.objects.new('low', me)
bpy.context.scene.collection.objects.link(low)

# ---- the AI soldier (high)
bpy.ops.import_scene.gltf(filepath=GLB)
meshes = [o for o in bpy.data.objects if o.type == 'MESH' and o is not low]
bpy.ops.object.select_all(action='DESELECT')
for o in meshes:
    o.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')
bpy.ops.object.join()
hi = bpy.context.view_layer.objects.active
hi.name = 'hi'
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
for o in [o for o in bpy.data.objects if o.type == 'EMPTY']:
    bpy.data.objects.remove(o)

# ---- align hi to low: same height, feet on z=0, centred on x/y, optional rotation about Z
if ROT:
    hi.rotation_euler[2] = math.radians(ROT)
    bpy.ops.object.select_all(action='DESELECT')
    hi.select_set(True)
    bpy.context.view_layer.objects.active = hi
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)
bb = np.array([hi.matrix_world @ Vector(c) for c in hi.bound_box])
hlo, hhi = bb.min(0), bb.max(0)
lo_, hi_ = P.min(0), P.max(0)
s = (hi_[2] - lo_[2]) / (hhi[2] - hlo[2])
hi.scale = (s, s, s)
bpy.context.view_layer.update()
bb = np.array([hi.matrix_world @ Vector(c) for c in hi.bound_box])
hlo, hhi = bb.min(0), bb.max(0)
hi.location = (hi.location[0] + ((lo_[0] + hi_[0]) - (hlo[0] + hhi[0])) / 2,
               hi.location[1] + ((lo_[1] + hi_[1]) - (hlo[1] + hhi[1])) / 2,
               hi.location[2] + lo_[2] - hlo[2])
bpy.context.view_layer.update()
print('scale %.5f' % s, 'low z', lo_[2], hi_[2], 'low width', lo_[0], hi_[0])

sc = bpy.context.scene


def render_views(name, objs, ortho=2.2):
    sc.render.engine = 'BLENDER_WORKBENCH'
    sc.display.shading.light = 'FLAT'
    sc.display.shading.color_type = 'TEXTURE'
    sc.render.resolution_x, sc.render.resolution_y = 1600, 900
    sc.world = bpy.data.worlds.new('w' + name)
    sc.world.use_nodes = True
    sc.world.node_tree.nodes['Background'].inputs[0].default_value = (0.25, 0.25, 0.25, 1)
    cam = bpy.data.cameras.new('cam' + name)
    cam.type = 'ORTHO'
    cam.ortho_scale = ortho * 16 / 9
    co = bpy.data.objects.new('cam' + name, cam)
    sc.collection.objects.link(co)
    sc.camera = co
    for view, ang in (('front', 0), ('side', 90)):
        r = math.radians(ang)
        co.location = (4 * math.sin(r), -4 * math.cos(r), 0.86)
        co.rotation_euler = (math.radians(90), 0, r)
        sc.render.filepath = os.path.join(OUT, '%s_%s.png' % (name, view))
        bpy.ops.render.render(write_still=True)


if STAGE == 'align':
    # low in its own material-less grey, hi textured; show them side by side (low shifted to the right) and overlaid
    gm = bpy.data.materials.new('grey')
    gm.use_nodes = False
    gm.diffuse_color = (0.8, 0.2, 0.2, 1)
    me.materials.append(gm)
    sc.display.shading.color_type = 'MATERIAL'
    low.location = (1.5, 0, 0)
    render_views('align_side', None, 2.2)
    low.location = (0, 0, 0)
    low.display_type = 'WIRE'
    render_views('align_overlay', None, 2.2)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, 'align.blend'))
    print('align done')
else:
    sc.render.engine = 'CYCLES'
    sc.cycles.device = 'CPU'
    sc.cycles.samples = 16
    W, H = 2048, 1024
    img = bpy.data.images.new('baked', W, H, alpha=False)
    img.generated_color = (0, 0, 0, 1)
    mat = bpy.data.materials.new('bake_target')
    mat.use_nodes = True
    node = mat.node_tree.nodes.new('ShaderNodeTexImage')
    node.image = img
    mat.node_tree.nodes.active = node
    me.materials.clear()
    me.materials.append(mat)
    bpy.ops.object.select_all(action='DESELECT')
    hi.select_set(True)
    low.select_set(True)
    bpy.context.view_layer.objects.active = low
    sc.render.bake.use_selected_to_active = True
    sc.render.bake.cage_extrusion = 0.08
    sc.render.bake.max_ray_distance = 0.25
    sc.render.bake.margin = 8
    sc.render.bake.use_pass_direct = False
    sc.render.bake.use_pass_indirect = False
    sc.render.bake.use_pass_color = True
    bpy.ops.object.bake(type='DIFFUSE')
    img.filepath_raw = os.path.join(OUT, 'baked_%dx%d.png' % (W, H))
    img.file_format = 'PNG'
    img.save()
    hi.hide_render = True
    hi.hide_viewport = True
    render_views('baked_on_game_soldier', None, 2.2)
    sc.render.engine = 'BLENDER_WORKBENCH'
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, 'bake.blend'))
    print('bake done')

# blender -b -P inspect_glb.py -- <file.glb>   : prints what the AI model contains
import sys
import bpy
bpy.ops.wm.read_factory_settings(use_empty=True)
p = sys.argv[sys.argv.index('--') + 1]
bpy.ops.import_scene.gltf(filepath=p)
tot_f = tot_v = 0
for o in bpy.data.objects:
    print('OBJ', o.type, o.name, 'parent', o.parent.name if o.parent else None)
    if o.type == 'MESH':
        me = o.data
        tot_f += len(me.polygons)
        tot_v += len(me.vertices)
        bb = [o.matrix_world @ __import__('mathutils').Vector(c) for c in o.bound_box]
        lo = [min(v[i] for v in bb) for i in range(3)]
        hi = [max(v[i] for v in bb) for i in range(3)]
        print('  faces', len(me.polygons), 'verts', len(me.vertices), 'uv layers', len(me.uv_layers),
              'materials', [m.name for m in me.materials], 'vgroups', len(o.vertex_groups))
        print('  bbox lo', [round(x, 3) for x in lo], 'hi', [round(x, 3) for x in hi])
    if o.type == 'ARMATURE':
        print('  bones', len(o.data.bones))
print('TOTAL faces', tot_f, 'verts', tot_v)
for im in bpy.data.images:
    print('IMAGE', im.name, tuple(im.size))

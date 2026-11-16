"""Render a reference Cryptomatte with Blender for the jd_EXRCompiler round-trip test.

Run headless (it never saves the .blend or the preferences):
    blender -b --factory-startup any_scene.blend --python blender_crypto_scene.py -- OUT.exr

Scene: every mesh in the opened file + two rotated copies that overlap it in camera view + a ground plane,
so the Cryptomatte has several objects, shared antialiased edges and occlusion.
Output: one multilayer EXR, 32-bit float, with the Cryptomatte Object pass (6 ranks).
Then: crypto_roundtrip.py OUT.exr WORKDIR path/to/jd_exrc  (rebuilds it from per-object mattes and compares).
"""
import math
import sys

import bpy
from mathutils import Vector

out_path = sys.argv[sys.argv.index("--") + 1]
sc = bpy.context.scene

# --- the subject: all mesh objects already in the file------------------------------------------------------------
subject = [o for o in sc.objects if o.type == "MESH"]
if not subject:
    raise SystemExit("no mesh objects in the file")
pts = [o.matrix_world @ Vector(c) for o in subject for c in o.bound_box]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
center, size = (lo + hi) / 2, max((hi - lo).length, 1e-3)

# --- two copies (linked data, own object names) that overlap the original in camera view ---------------------------
for k, (dx, dy, rz) in enumerate([(0.55, 0.45, 35.0), (-0.5, 0.8, -50.0)], start=1):
    for o in subject:
        c = o.copy()  # shares mesh data; new object name
        c.name = f"{o.name}_copy{k}"
        c.matrix_world = o.matrix_world.copy()
        c.location += Vector((dx * size, dy * size, 0.0))
        c.rotation_euler.z += math.radians(rz)
        sc.collection.objects.link(c)

# --- ground, camera, light -----------------------------------------------------------------------------------------
bpy.ops.mesh.primitive_plane_add(size=size * 8, location=(center.x, center.y, lo.z))
bpy.context.active_object.name = "ground"

cam_data = bpy.data.cameras.new("rt_cam")
cam = bpy.data.objects.new("rt_cam", cam_data)
sc.collection.objects.link(cam)
cam.location = center + Vector((0.0, -3.2 * size, 1.4 * size))
track = cam.constraints.new("TRACK_TO")
target = bpy.data.objects.new("rt_target", None)
sc.collection.objects.link(target)
target.location = center + Vector((0.0, 0.3 * size, 0.0))
track.target = target
sc.camera = cam

sun = bpy.data.objects.new("rt_sun", bpy.data.lights.new("rt_sun", "SUN"))
sun.rotation_euler = (math.radians(50), 0.0, math.radians(30))
sc.collection.objects.link(sun)

# --- render settings ------------------------------------------------------------------------------------------------
r = sc.render
r.engine = "CYCLES"
sc.cycles.device = "CPU"
sc.cycles.samples = 32
r.resolution_x, r.resolution_y, r.resolution_percentage = 320, 240, 100
r.use_compositing = False
r.use_sequencer = False

vl = bpy.context.view_layer
vl.use_pass_cryptomatte_object = True
vl.pass_cryptomatte_depth = 6

im = r.image_settings
try:
    im.file_format = "OPEN_EXR_MULTILAYER"
except TypeError:
    # Blender 5.x: multilayer is a media type, not a file format. Read the enums instead of guessing.
    media = [i.identifier for i in type(im).bl_rna.properties["media_type"].enum_items]
    print("media types:", media)
    im.media_type = "MULTI_LAYER_IMAGE"
    fmts = [i.identifier for i in type(im).bl_rna.properties["file_format"].enum_items_static]
    im.file_format = "OPEN_EXR_MULTILAYER" if "OPEN_EXR_MULTILAYER" in fmts else "OPEN_EXR"
im.color_depth = "32"
im.exr_codec = "ZIP"

r.filepath = out_path
r.use_file_extension = False
bpy.ops.render.render(write_still=True)
print("WROTE", out_path, "objects:", len([o for o in sc.objects if o.type == "MESH"]))

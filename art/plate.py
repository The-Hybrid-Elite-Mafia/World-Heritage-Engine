"""Blender scene: the World Heritage Engine obsidian faceplate.

    blender -b --python plate.py -- <out.png> <scale> <samples>

scale 2 -> 3840x2160 (4K). Camera is orthographic, straight down; one key light
from the top-left. Pockets, knob wells, glass recesses and neon grooves are cut
with exact booleans so walls, floors and fillets are real geometry.
"""
import math
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import layout as L  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = argv[0] if argv else "/tmp/plate.png"
SCALE = float(argv[1]) if len(argv) > 1 else 0.5
SAMPLES = int(argv[2]) if len(argv) > 2 else 32

PX = 0.01  # 1 layout px = 0.01 blender units


def X(x):
    return (x - L.W / 2) * PX


def Y(y):
    return (L.H / 2 - y) * PX


# --------------------------------------------------------------------------- scene
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = SAMPLES
scene.cycles.use_denoising = False
scene.cycles.max_bounces = 6
scene.cycles.glossy_bounces = 3
scene.cycles.transparent_max_bounces = 4
scene.render.resolution_x = int(L.W * SCALE)
scene.render.resolution_y = int(L.H * SCALE)
scene.render.film_transparent = False
scene.view_settings.view_transform = "AgX"
scene.view_settings.look = "AgX - Medium High Contrast"
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_depth = "8"

world = bpy.data.worlds.new("World")
scene.world = world
world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.006, 0.0065, 0.0075, 1)
world.node_tree.nodes["Background"].inputs["Strength"].default_value = 1.0

cam_data = bpy.data.cameras.new("Cam")
cam_data.type = "ORTHO"
cam_data.ortho_scale = L.W * PX
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
cam.location = (0, 0, 30)
scene.camera = cam


# --------------------------------------------------------------------------- materials
def principled(name):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    return m, nt, nt.nodes["Principled BSDF"]


def mat_obsidian():
    m, nt, b = principled("Obsidian")
    n1 = nt.nodes.new("ShaderNodeTexNoise")
    n1.inputs["Scale"].default_value = 1.6
    n1.inputs["Detail"].default_value = 10
    n1.inputs["Roughness"].default_value = 0.62
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.38
    ramp.color_ramp.elements[0].color = (0.0045, 0.005, 0.006, 1)
    ramp.color_ramp.elements[1].position = 0.72
    ramp.color_ramp.elements[1].color = (0.020, 0.022, 0.026, 1)
    nt.links.new(n1.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], b.inputs["Base Color"])
    n2 = nt.nodes.new("ShaderNodeTexNoise")
    n2.inputs["Scale"].default_value = 7.0
    n2.inputs["Detail"].default_value = 6
    rr = nt.nodes.new("ShaderNodeMapRange")
    rr.inputs["To Min"].default_value = 0.16
    rr.inputs["To Max"].default_value = 0.30
    nt.links.new(n2.outputs["Fac"], rr.inputs["Value"])
    nt.links.new(rr.outputs["Result"], b.inputs["Roughness"])
    b.inputs["Specular IOR Level"].default_value = 0.55
    b.inputs["Coat Weight"].default_value = 0.85
    b.inputs["Coat Roughness"].default_value = 0.035
    return m


def mat_brushed(name, color, rough, aniso, stretch_axis="X"):
    m, nt, b = principled(name)
    b.inputs["Base Color"].default_value = color
    b.inputs["Metallic"].default_value = 1.0
    b.inputs["Roughness"].default_value = rough
    b.inputs["Anisotropic"].default_value = aniso
    tc = nt.nodes.new("ShaderNodeTexCoord")
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (1.0, 260.0, 1.0) if stretch_axis == "X" else (260.0, 1.0, 1.0)
    nz = nt.nodes.new("ShaderNodeTexNoise")
    nz.inputs["Scale"].default_value = 18.0
    nz.inputs["Detail"].default_value = 4
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.06
    bump.inputs["Distance"].default_value = 0.002
    nt.links.new(tc.outputs["Object"], mp.inputs["Vector"])
    nt.links.new(mp.outputs["Vector"], nz.inputs["Vector"])
    nt.links.new(nz.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], b.inputs["Normal"])
    return m


def mat_pocket():
    m, nt, b = principled("PocketFloor")
    b.inputs["Base Color"].default_value = (0.010, 0.011, 0.013, 1)
    b.inputs["Metallic"].default_value = 0.65
    b.inputs["Roughness"].default_value = 0.46
    nz = nt.nodes.new("ShaderNodeTexNoise")
    nz.inputs["Scale"].default_value = 220.0
    nz.inputs["Detail"].default_value = 2
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.03
    nt.links.new(nz.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], b.inputs["Normal"])
    return m


def mat_glass():
    m, nt, b = principled("Glass")
    b.inputs["Base Color"].default_value = (0.0006, 0.0008, 0.0010, 1)
    b.inputs["Roughness"].default_value = 0.12
    b.inputs["Specular IOR Level"].default_value = 0.25
    b.inputs["Coat Weight"].default_value = 0.45
    b.inputs["Coat Roughness"].default_value = 0.0
    return m


def mat_emission(name, color, strength):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        if n.type != "OUTPUT_MATERIAL":
            nt.nodes.remove(n)
    e = nt.nodes.new("ShaderNodeEmission")
    e.inputs["Color"].default_value = color
    e.inputs["Strength"].default_value = strength
    nt.links.new(e.outputs["Emission"], nt.nodes["Material Output"].inputs["Surface"])
    return m


M_OBS = mat_obsidian()
M_BEZEL = mat_brushed("Bezel", (0.20, 0.21, 0.22, 1), 0.30, 0.85)
M_POCKET = mat_pocket()
M_WELL = mat_brushed("Well", (0.05, 0.052, 0.056, 1), 0.38, 0.6)
M_GLASS = mat_glass()
M_NEON = mat_emission("Neon", (0.18, 0.86, 0.95, 1), 10.0)
M_SCREW = mat_brushed("Screw", (0.62, 0.64, 0.67, 1), 0.24, 0.5)
M_SLOT = mat_pocket()


# --------------------------------------------------------------------------- geometry helpers
def apply_all(obj):
    bpy.context.view_layer.objects.active = obj
    for mod in list(obj.modifiers):
        bpy.ops.object.modifier_apply(modifier=mod.name)


def box(name, x, y, w, h, z_top, z_bot, radius_px=0.0, mat=None, segments=8):
    bpy.ops.mesh.primitive_cube_add(size=1)
    o = bpy.context.active_object
    o.name = name
    o.scale = (w * PX, h * PX, z_top - z_bot)
    o.location = (X(x + w / 2), Y(y + h / 2), (z_top + z_bot) / 2)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if radius_px > 0:
        bv = o.modifiers.new("bv", "BEVEL")
        bv.width = min(radius_px * PX, (z_top - z_bot) * 0.45, w * PX * 0.49, h * PX * 0.49)
        bv.segments = segments
        bv.limit_method = "NONE"
        apply_all(o)
    if mat:
        o.data.materials.append(mat)
    return o


def cylinder(name, cx, cy, r_px, z_top, z_bot, mat=None, fillet_px=0.0, verts=128):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=r_px * PX, depth=z_top - z_bot,
                                        location=(X(cx), Y(cy), (z_top + z_bot) / 2))
    o = bpy.context.active_object
    o.name = name
    if fillet_px > 0:
        bv = o.modifiers.new("bv", "BEVEL")
        bv.width = min(fillet_px * PX, (z_top - z_bot) * 0.45)
        bv.segments = 6
        bv.limit_method = "ANGLE"
        apply_all(o)
    if mat:
        o.data.materials.append(mat)
    return o


def cut(target, cutter):
    m = target.modifiers.new("cut", "BOOLEAN")
    m.operation = "DIFFERENCE"
    m.solver = "EXACT"
    m.material_mode = "TRANSFER"
    m.object = cutter
    bpy.context.view_layer.objects.active = target
    bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.data.objects.remove(cutter, do_unlink=True)


def smooth(o, angle=35):
    o.data.use_auto_smooth = True
    o.data.auto_smooth_angle = math.radians(angle)
    for p in o.data.polygons:
        p.use_smooth = True


# --------------------------------------------------------------------------- bezel + plate
B = L.BEZEL
bezel = box("Bezel", 0, 0, L.W, L.H, 0.03, -0.6, radius_px=18, mat=M_BEZEL)
cut(bezel, box("BezelHole", B, B, L.W - 2 * B, L.H - 2 * B, 1.0, -1.0, radius_px=10))
smooth(bezel)

plate = box("Plate", B - 2, B - 2, L.W - 2 * B + 4, L.H - 2 * B + 4, 0.0, -0.5, mat=M_OBS)

for name, (x, y, w, h, r, d) in L.POCKETS.items():
    cut(plate, box("pk_" + name, x, y, w, h, 0.5, -d * PX, radius_px=r, mat=M_POCKET))

pocket_floor = {}
for name, (x, y, w, h, r, d) in L.POCKETS.items():
    pocket_floor[name] = -d * PX


def floor_at(px, py):
    for name, (x, y, w, h, r, d) in L.POCKETS.items():
        if x <= px <= x + w and y <= py <= y + h:
            return -d * PX
    return 0.0


# knob wells: brushed circular recess under every knob (LED ring sits on its floor)
for name, (cx, cy, dia, ring) in L.KNOBS.items():
    f = floor_at(cx, cy)
    cut(plate, cylinder("well_" + name, cx, cy, ring + 11, 0.5, f - 0.05, mat=M_WELL, fillet_px=4))

# round push-button well
for name, (cx, cy, dia) in L.ROUND_BUTTONS.items():
    f = floor_at(cx, cy)
    cut(plate, cylinder("rb_" + name, cx, cy, dia / 2 + 10, 0.5, f - 0.05, mat=M_WELL, fillet_px=4))

# logo badges
for name, (cx, cy, r) in L.BADGES.items():
    cut(plate, cylinder("badge_" + name, cx, cy, r, 0.5, -0.06, mat=M_WELL, fillet_px=4))

# fader slot
fx, fy, fw, fh = L.FADER
cut(plate, box("fader_slot", fx, fy + fh / 2 - 4, fw, 8, 0.5, floor_at(fx + 1, fy + 1) - 0.06, radius_px=4, mat=M_SLOT))

# glass recesses + glass slabs
glass_objs = []
for name, (x, y, w, h, r) in L.GLASS.items():
    f = floor_at(x + w / 2, y + h / 2)
    cut(plate, box("gr_" + name, x - 3, y - 3, w + 6, h + 6, 0.5, f - 0.05, radius_px=r + 3, mat=M_POCKET))
    g = box("glass_" + name, x, y, w, h, f - 0.012, f - 0.05, radius_px=r, mat=M_GLASS)
    smooth(g, 40)
    glass_objs.append(g)

# neon grooves + emissive inlays
for i, (x0, y0, x1, y1, wpx) in enumerate(L.NEON):
    cut(plate, box("ng%d" % i, x0, y0 - wpx / 2 - 1, x1 - x0, wpx + 2, 0.5, -0.04, radius_px=2, mat=M_POCKET))
    box("neon%d" % i, x0 + 1, y0 - wpx / 2, x1 - x0 - 2, wpx, -0.006, -0.03, radius_px=1.5, mat=M_NEON)

smooth(plate, 32)

# screws in the bezel corners
for i, (sx, sy) in enumerate(L.SCREWS):
    s = cylinder("screw%d" % i, sx, sy, 12, 0.07, -0.05, mat=M_SCREW, fillet_px=5, verts=96)
    slot = box("slot%d" % i, sx - 10, sy - 1.6, 20, 3.2, 0.2, 0.035, mat=M_SLOT)
    slot.rotation_euler[2] = math.radians(35 + 25 * i)
    cut(s, slot)
    smooth(s, 40)

# --------------------------------------------------------------------------- lighting
key = bpy.data.lights.new("Key", "AREA")
key.energy = 2600
key.size = 3.0
key.color = (1.0, 0.98, 0.95)
ko = bpy.data.objects.new("Key", key)
scene.collection.objects.link(ko)
ko.location = (-9.0, 7.0, 9.0)
direction = -ko.location.normalized()
ko.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()

fill = bpy.data.lights.new("Fill", "AREA")
fill.energy = 90
fill.size = 3.0
fill.color = (0.75, 0.9, 1.0)
fo = bpy.data.objects.new("Fill", fill)
scene.collection.objects.link(fo)
fo.location = (14.0, -9.0, 8.0)
fo.rotation_euler = (-fo.location.normalized()).to_track_quat("-Z", "Y").to_euler()

# soft "studio window" overhead, invisible to the camera: gives the glossy obsidian
# and the glass a diagonal reflection that fades from top-left to bottom-right
bpy.ops.mesh.primitive_plane_add(size=1, location=(0, 0, 6))
refl = bpy.context.active_object
refl.name = "Reflector"
refl.scale = (L.W * PX * 1.4, L.H * PX * 1.4, 1)
refl.rotation_euler = (math.pi, 0, 0)
rm = bpy.data.materials.new("ReflGrad")
rm.use_nodes = True
nt = rm.node_tree
for n in list(nt.nodes):
    if n.type != "OUTPUT_MATERIAL":
        nt.nodes.remove(n)
tc = nt.nodes.new("ShaderNodeTexCoord")
mp = nt.nodes.new("ShaderNodeMapping")
mp.inputs["Rotation"].default_value = (0, 0, math.radians(-35))
gr = nt.nodes.new("ShaderNodeTexGradient")
ramp = nt.nodes.new("ShaderNodeValToRGB")
ramp.color_ramp.elements[0].position = 0.25
ramp.color_ramp.elements[0].color = (0.0, 0.0, 0.0, 1)
ramp.color_ramp.elements[1].position = 0.95
ramp.color_ramp.elements[1].color = (0.55, 0.62, 0.68, 1)
em = nt.nodes.new("ShaderNodeEmission")
em.inputs["Strength"].default_value = 1.1
nt.links.new(tc.outputs["UV"], mp.inputs["Vector"])
nt.links.new(mp.outputs["Vector"], gr.inputs["Vector"])
nt.links.new(gr.outputs["Fac"], ramp.inputs["Fac"])
nt.links.new(ramp.outputs["Color"], em.inputs["Color"])
nt.links.new(em.outputs["Emission"], nt.nodes["Material Output"].inputs["Surface"])
refl.data.materials.append(rm)
refl.visible_camera = False
refl.visible_shadow = False
refl.visible_diffuse = False

# --------------------------------------------------------------------------- bloom for the neon
scene.use_nodes = True
ct = scene.node_tree
for n in list(ct.nodes):
    ct.nodes.remove(n)
rl = ct.nodes.new("CompositorNodeRLayers")
gl = ct.nodes.new("CompositorNodeGlare")
gl.glare_type = "FOG_GLOW"
gl.quality = "HIGH"
gl.threshold = 1.4
gl.size = 7
comp = ct.nodes.new("CompositorNodeComposite")
ct.links.new(rl.outputs["Image"], gl.inputs["Image"])
ct.links.new(gl.outputs["Image"], comp.inputs["Image"])

scene.render.filepath = OUT
bpy.ops.render.render(write_still=True)
print("WROTE", OUT)

"""Blender scene: every moving / switching part of the World Heritage Engine front panel.

    blender -b --python parts.py -- <out_dir> <samples>

Same orthographic top-down camera and the same top-left key light direction as
the faceplate, so parts sit in the plate with matching light. Every part is
rendered on a transparent background; contact shadows come from a shadow catcher.

Layers:
  knob_body.png, knob_pointer.png (pointer only; the app rotates this layer)
  knob_big_body.png, knob_big_pointer.png
  led_off.png, led_on.png
  button_off.png, button_on.png (9-slice: corners 96 px at 512 px)
  round_off.png, round_on.png
  fader_cap.png
  key_white.png, key_white_down.png, key_black.png, key_black_down.png
"""
import math
import os
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = argv[0] if argv else "/tmp/parts"
SAMPLES = int(argv[1]) if len(argv) > 1 else 64
os.makedirs(OUT, exist_ok=True)

CYAN = (0.18, 0.86, 0.95, 1)
KEY_DIR = Vector((-9.0, 7.0, 9.0)).normalized()  # identical to the faceplate key light


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    s = bpy.context.scene
    s.render.engine = "CYCLES"
    s.cycles.device = "CPU"
    s.cycles.samples = SAMPLES
    s.cycles.use_denoising = False
    s.render.film_transparent = True
    s.view_settings.view_transform = "AgX"
    s.view_settings.look = "AgX - Medium High Contrast"
    s.render.image_settings.file_format = "PNG"
    s.render.image_settings.color_mode = "RGBA"
    w = bpy.data.worlds.new("W")
    s.world = w
    w.use_nodes = True
    w.node_tree.nodes["Background"].inputs["Color"].default_value = (0.006, 0.0065, 0.0075, 1)
    return s


def camera(s, ortho, res_x, res_y):
    cd = bpy.data.cameras.new("Cam")
    cd.type = "ORTHO"
    cd.ortho_scale = ortho
    c = bpy.data.objects.new("Cam", cd)
    s.collection.objects.link(c)
    c.location = (0, 0, 20)
    s.camera = c
    s.render.resolution_x = res_x
    s.render.resolution_y = res_y


def lights(s, scale=1.0):
    k = bpy.data.lights.new("Key", "AREA")
    k.energy = 260 * scale * scale
    k.size = 0.6 * scale
    k.color = (1.0, 0.98, 0.95)
    ko = bpy.data.objects.new("Key", k)
    s.collection.objects.link(ko)
    ko.location = KEY_DIR * 6.0 * scale
    ko.rotation_euler = (-KEY_DIR).to_track_quat("-Z", "Y").to_euler()
    f = bpy.data.lights.new("Fill", "AREA")
    f.energy = 10 * scale * scale
    f.size = 1.0 * scale
    f.color = (0.75, 0.9, 1.0)
    fo = bpy.data.objects.new("Fill", f)
    s.collection.objects.link(fo)
    fo.location = Vector((9.0, -6.0, 6.0)).normalized() * 6.0 * scale
    fo.rotation_euler = (-fo.location.normalized()).to_track_quat("-Z", "Y").to_euler()
    # overhead soft reflector (camera-invisible), same idea as the faceplate
    bpy.ops.mesh.primitive_plane_add(size=6 * scale, location=(-1.5 * scale, 1.5 * scale, 4 * scale))
    r = bpy.context.active_object
    r.rotation_euler = (math.pi, 0, 0)
    r.data.materials.append(emission("Refl", (0.6, 0.66, 0.72, 1), 0.6))
    r.visible_camera = False
    r.visible_shadow = False
    r.visible_diffuse = False


def bloom(s, threshold=1.2, size=7):
    s.use_nodes = True
    ct = s.node_tree
    for n in list(ct.nodes):
        ct.nodes.remove(n)
    rl = ct.nodes.new("CompositorNodeRLayers")
    gl = ct.nodes.new("CompositorNodeGlare")
    gl.glare_type = "FOG_GLOW"
    gl.quality = "HIGH"
    gl.threshold = threshold
    gl.size = size
    comp = ct.nodes.new("CompositorNodeComposite")
    ct.links.new(rl.outputs["Image"], gl.inputs["Image"])
    ct.links.new(gl.outputs["Image"], comp.inputs["Image"])
    ct.links.new(rl.outputs["Alpha"], comp.inputs["Alpha"]) if "Alpha" in comp.inputs else None
    # keep the glow visible on a transparent background: alpha = max(alpha, glow luminance)
    if "Alpha" in comp.inputs:
        sep = ct.nodes.new("CompositorNodeRGBToBW")
        mx = ct.nodes.new("CompositorNodeMath")
        mx.operation = "MAXIMUM"
        ct.links.new(gl.outputs["Image"], sep.inputs["Image"])
        ct.links.new(rl.outputs["Alpha"], mx.inputs[0])
        ct.links.new(sep.outputs["Val"], mx.inputs[1])
        ct.links.new(mx.outputs["Value"], comp.inputs["Alpha"])


def render(s, name):
    s.render.filepath = os.path.join(OUT, name)
    if os.environ.get("WHE_SKIP_EXISTING") and os.path.exists(s.render.filepath):
        print("SKIP", name)
        return
    bpy.ops.render.render(write_still=True)
    print("WROTE", name)


def principled(name, base, metallic=0.0, rough=0.4, coat=0.0, aniso=0.0, spec=0.5):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = base
    b.inputs["Metallic"].default_value = metallic
    b.inputs["Roughness"].default_value = rough
    b.inputs["Coat Weight"].default_value = coat
    b.inputs["Coat Roughness"].default_value = 0.03
    b.inputs["Anisotropic"].default_value = aniso
    b.inputs["Specular IOR Level"].default_value = spec
    return m, m.node_tree, b


def emission(name, color, strength):
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


def spun_aluminum():
    m, nt, b = principled("Spun", (0.72, 0.74, 0.77, 1), metallic=1.0, rough=0.22, aniso=0.9)
    tg = nt.nodes.new("ShaderNodeTangent")
    tg.direction_type = "RADIAL"
    tg.axis = "Z"
    nt.links.new(tg.outputs["Tangent"], b.inputs["Tangent"])
    # concentric spin marks
    tc = nt.nodes.new("ShaderNodeTexCoord")
    grad = nt.nodes.new("ShaderNodeTexGradient")
    grad.gradient_type = "SPHERICAL"
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (1.0, 1.0, 1.0)
    wave = nt.nodes.new("ShaderNodeTexWave")
    wave.wave_type = "RINGS"
    wave.inputs["Scale"].default_value = 60.0
    wave.inputs["Distortion"].default_value = 2.0
    wave.inputs["Detail"].default_value = 3.0
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.05
    bump.inputs["Distance"].default_value = 0.001
    nt.links.new(tc.outputs["Object"], wave.inputs["Vector"])
    nt.links.new(wave.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], b.inputs["Normal"])
    return m


def apply(o):
    bpy.context.view_layer.objects.active = o
    for mod in list(o.modifiers):
        bpy.ops.object.modifier_apply(modifier=mod.name)


def smooth(o, angle=40):
    o.data.use_auto_smooth = True
    o.data.auto_smooth_angle = math.radians(angle)
    for p in o.data.polygons:
        p.use_smooth = True


def cyl(r, z0, z1, mat, bevel=0.0, verts=160, segs=6, loc=(0, 0)):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=r, depth=z1 - z0, location=(loc[0], loc[1], (z0 + z1) / 2))
    o = bpy.context.active_object
    if bevel > 0:
        b = o.modifiers.new("b", "BEVEL")
        b.width = bevel
        b.segments = segs
        b.limit_method = "ANGLE"
        apply(o)
    smooth(o)
    o.data.materials.append(mat)
    return o


def rbox(w, h, z0, z1, r, mat, bevel_top=0.0, loc=(0, 0)):
    bpy.ops.mesh.primitive_cube_add(size=1, location=(loc[0], loc[1], (z0 + z1) / 2))
    o = bpy.context.active_object
    o.scale = (w, h, z1 - z0)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    b = o.modifiers.new("b", "BEVEL")
    b.width = min(r, (z1 - z0) * 0.48, w * 0.49, h * 0.49)
    b.segments = 8
    b.limit_method = "NONE"
    apply(o)
    smooth(o, 50)
    o.data.materials.append(mat)
    return o


def shadow_catcher(size=8.0):
    bpy.ops.mesh.primitive_plane_add(size=size, location=(0, 0, 0))
    p = bpy.context.active_object
    p.is_shadow_catcher = True
    p.data.materials.append(principled("Catcher", (0.02, 0.02, 0.02, 1))[0])
    return p


# =========================================================================== knobs
def knob(prefix, res):
    for layer in ("body", "pointer"):
        s = reset()
        camera(s, 3.0, res, res)
        lights(s)
        shadow_catcher()
        black = principled("Rubber", (0.012, 0.012, 0.014, 1), rough=0.55)[0]
        satin = principled("Satin", (0.03, 0.031, 0.034, 1), metallic=0.8, rough=0.35, aniso=0.5)[0]
        skirt = cyl(1.0, 0.0, 0.12, satin, bevel=0.04)
        body = cyl(0.86, 0.0, 0.44, black, bevel=0.1)
        # knurled grip: shallow vertical flutes around the body edge
        for i in range(48):
            a = 2 * math.pi * i / 48
            bpy.ops.mesh.primitive_cube_add(size=1, location=(0.86 * math.cos(a), 0.86 * math.sin(a), 0.22))
            fl = bpy.context.active_object
            fl.scale = (0.05, 0.035, 0.36)
            fl.rotation_euler[2] = a
            m = body.modifiers.new("f%d" % i, "BOOLEAN")
            m.operation = "DIFFERENCE"
            m.solver = "FAST"
            m.object = fl
            apply(body)
            bpy.data.objects.remove(fl, do_unlink=True)
        smooth(body, 30)
        cap = cyl(0.64, 0.36, 0.5, spun_aluminum(), bevel=0.045)
        if layer == "pointer":
            # everything but the pointer is a shadow catcher: the pointer's own
            # shadow on the cap is kept, the knob itself is transparent
            for o in (skirt, body, cap):
                o.is_shadow_catcher = True
            dark = principled("Pointer", (0.008, 0.008, 0.01, 1), rough=0.3, coat=0.6)[0]
            ptr = rbox(0.09, 0.42, 0.47, 0.535, 0.03, dark, loc=(0, 0.37))
            dot = cyl(0.045, 0.5, 0.545, emission("PtrDot", CYAN, 6.0), loc=(0, 0.53), verts=48)
            bloom(s, threshold=1.5, size=5)
        render(s, prefix + "_" + layer + ".png")


# =========================================================================== LEDs
def led(res):
    for state in ("off", "on"):
        s = reset()
        camera(s, 1.0, res, res)
        lights(s, 0.4)
        shadow_catcher()
        bezel = principled("LedBezel", (0.03, 0.032, 0.035, 1), metallic=0.9, rough=0.3)[0]
        cyl(0.2, 0.0, 0.05, bezel, bevel=0.02, verts=64)
        if state == "on":
            lens_m = emission("LedOn", CYAN, 9.0)
        else:
            lens_m = principled("LedOff", (0.02, 0.06, 0.07, 1), rough=0.12, coat=1.0, spec=0.8)[0]
        bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=0.15, location=(0, 0, 0.02))
        lens = bpy.context.active_object
        lens.scale[2] = 0.55
        smooth(lens)
        lens.data.materials.append(lens_m)
        if state == "on":
            bloom(s, threshold=1.0, size=7)
        render(s, "led_" + state + ".png")


# =========================================================================== buttons
def button(res):
    for state in ("off", "on"):
        s = reset()
        camera(s, 1.5, res, res)
        lights(s, 0.5)
        shadow_catcher()
        frame = principled("BtnFrame", (0.02, 0.021, 0.024, 1), metallic=0.85, rough=0.32)[0]
        rbox(1.04, 1.04, 0.0, 0.055, 0.13, frame)
        if state == "on":
            m, nt, b = principled("BtnOn", (0.02, 0.42, 0.52, 1), rough=0.25, coat=0.8)
            b.inputs["Emission Color"].default_value = (0.05, 0.72, 0.86, 1)
            b.inputs["Emission Strength"].default_value = 1.15
        else:
            m = principled("BtnOff", (0.012, 0.013, 0.015, 1), rough=0.2, coat=0.9, spec=0.6)[0]
        rbox(0.96, 0.96, 0.03, 0.11, 0.08, m)
        if state == "on":
            bloom(s, threshold=1.4, size=7)
        render(s, "button_" + state + ".png")


def round_button(res):
    for state in ("off", "on"):
        s = reset()
        camera(s, 1.4, res, res)
        lights(s, 0.5)
        shadow_catcher()
        frame = principled("RFrame", (0.25, 0.26, 0.28, 1), metallic=1.0, rough=0.25, aniso=0.7)[0]
        cyl(0.5, 0.0, 0.08, frame, bevel=0.03, verts=128)
        if state == "on":
            m, nt, b = principled("RBOn", (0.02, 0.45, 0.55, 1), rough=0.2, coat=1.0)
            b.inputs["Emission Color"].default_value = (0.05, 0.72, 0.86, 1)
            b.inputs["Emission Strength"].default_value = 1.5
        else:
            m = principled("RBOff", (0.015, 0.03, 0.035, 1), rough=0.15, coat=1.0, spec=0.7)[0]
        bpy.ops.mesh.primitive_uv_sphere_add(segments=96, ring_count=48, radius=0.42, location=(0, 0, 0.0))
        dome = bpy.context.active_object
        dome.scale[2] = 0.38
        smooth(dome)
        dome.data.materials.append(m)
        if state == "on":
            bloom(s, threshold=1.3, size=8)
        render(s, "round_" + state + ".png")


def fader_cap(rx, ry):
    s = reset()
    camera(s, 1.6, rx, ry)
    lights(s, 0.5)
    shadow_catcher()
    metal = principled("Fader", (0.55, 0.57, 0.6, 1), metallic=1.0, rough=0.28, aniso=0.8)[0]
    black = principled("FaderBody", (0.015, 0.015, 0.017, 1), rough=0.4)[0]
    rbox(0.62, 0.96, 0.0, 0.3, 0.08, black)
    rbox(0.54, 0.88, 0.2, 0.36, 0.06, metal)
    for i in range(-3, 4):
        if i == 0:
            continue
        rbox(0.5, 0.025, 0.34, 0.37, 0.01, black, loc=(0, i * 0.1))
    rbox(0.52, 0.04, 0.35, 0.385, 0.015, emission("FaderLine", CYAN, 6.0))
    bloom(s, threshold=1.5, size=5)
    render(s, "fader_cap.png")


def keys():
    specs = [
        ("key_white.png", 1.0, 6.4, (0.86, 0.85, 0.82, 1), False, False),
        ("key_white_down.png", 1.0, 6.4, (0.86, 0.85, 0.82, 1), True, False),
        ("key_black.png", 0.62, 4.0, (0.012, 0.012, 0.014, 1), False, True),
        ("key_black_down.png", 0.62, 4.0, (0.012, 0.012, 0.014, 1), True, True),
    ]
    for name, w, h, col, down, is_black in specs:
        s = reset()
        aspect = h / w
        camera(s, h * 1.02, int(round(160 * w * 1.02)), int(round(160 * h * 1.02)))
        cam = s.camera
        cam.data.sensor_fit = "VERTICAL"
        lights(s, 3.0)
        shadow_catcher(20)
        if down:
            m, nt, b = principled("KeyDown", (0.03, 0.45, 0.55, 1) if not is_black else (0.01, 0.2, 0.25, 1),
                                  rough=0.25, coat=0.7)
            b.inputs["Emission Color"].default_value = (0.05, 0.72, 0.86, 1)
            b.inputs["Emission Strength"].default_value = 0.9 if not is_black else 0.7
        else:
            m = principled("Key", col, rough=0.22 if not is_black else 0.15, coat=0.8, spec=0.6)[0]
        top = 0.5 if not is_black else 0.9
        if down:
            top -= 0.12
        rbox(w * 0.96, h, 0.0, top, 0.06 if not is_black else 0.1, m)
        if down:
            bloom(s, threshold=1.3, size=6)
        render(s, name)


knob("knob", 512)
knob("knob_big", 640)
led(128)
button(512)
round_button(320)
fader_cap(256, 384)
keys()
print("ALL DONE")

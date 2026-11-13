"""Lacquer: renders the faceplate with Blender (Cycles). Forked from Kontur.

    python3 render.py panel [--draft]     the panel background (JPEG)
    python3 render.py sprites [--draft]   knob filmstrips, toggles, lit buttons, lamps, meter glass

World units are points (1 unit = 1 pt at 100%); canvas x runs right, canvas y
runs down, so Blender Y = -canvas y. Lights are suns plus a studio world, so a
part lights the same wherever it sits and sprites can be rendered at the origin.
"""
import math
import os
import sys

import bpy  # noqa: I001  (bpy first: it puts bmesh and mathutils on the path)
import bmesh
from mathutils import Vector
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import layout as L  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
ASSETS = BUILD
DRAFT = "--draft" in sys.argv
PREVIEW = "--preview" in sys.argv


def _arg(name):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return None


STATE = _arg("--state")        # None: the mockup. "bare": the plugin's background (no knobs, switches or
                               # needles, nothing lit). "amber" / "green": everything lit, for the lit patches.
FLIP = _arg("--flip")          # a toggle id to render in its other position
BORDER = _arg("--border")      # "x,y,w,h" in points: render only this region, to a PNG

Z_FRAME = 3.0
Z_CASS = 4.5

# Knob geometry per kind: skirt radius = layout radius.
KNOB_H = {"large": 17.0, "med": 15.0, "small": 13.0, "sel": 13.0}


# ---------------------------------------------------------------------------
# Scene
def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _mats.clear()   # the old scene's materials are gone
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.device = "CPU"
    sc.cycles.samples = 24 if DRAFT else (56 if PREVIEW else 160)
    sc.cycles.use_adaptive_sampling = True
    sc.cycles.adaptive_threshold = 0.02 if DRAFT else 0.006
    sc.cycles.max_bounces = 8
    sc.cycles.glossy_bounces = 4
    sc.cycles.transmission_bounces = 8
    sc.cycles.use_denoising = True
    try:
        sc.cycles.denoiser = "OPENIMAGEDENOISE"
    except Exception:
        pass
    sc.view_settings.view_transform = "AgX"
    try:
        sc.view_settings.look = "AgX - Base Contrast"
    except Exception:
        pass
    sc.view_settings.exposure = 0.0
    sc.render.image_settings.color_depth = "8"
    sc.cycles.seed = 7
    return sc


def look_dir(obj, direction):
    obj.rotation_euler = Vector(direction).normalized().to_track_quat("-Z", "Y").to_euler()


def lights():
    # Key: high from the upper left, soft. Fill: low and weak from the right.
    key = bpy.data.lights.new("key", "SUN")
    key.energy = 3.2
    key.angle = math.radians(9)
    key.color = (1.0, 0.97, 0.92)
    ko = bpy.data.objects.new("key", key)
    bpy.context.scene.collection.objects.link(ko)
    look_dir(ko, (0.55, -0.62, -1.25))
    fill = bpy.data.lights.new("fill", "SUN")
    fill.energy = 0.55
    fill.angle = math.radians(25)
    fill.color = (0.92, 0.95, 1.0)
    fo = bpy.data.objects.new("fill", fill)
    bpy.context.scene.collection.objects.link(fo)
    look_dir(fo, (-0.9, 0.2, -1.0))

    # Studio world: a dim room with a large softbox up and to the left (for reflections).
    w = bpy.data.worlds.new("studio")
    bpy.context.scene.world = w
    w.use_nodes = True
    nt = w.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputWorld")
    bg = nt.nodes.new("ShaderNodeBackground")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    dot = nt.nodes.new("ShaderNodeVectorMath")
    dot.operation = "DOT_PRODUCT"
    kd = Vector((-0.45, 0.55, 1.0)).normalized()
    dot.inputs[1].default_value = kd
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value = 0.86
    mr.inputs["From Max"].default_value = 0.97
    mr.inputs["To Min"].default_value = 0.0
    mr.inputs["To Max"].default_value = 3.2
    up = nt.nodes.new("ShaderNodeSeparateXYZ")
    amb = nt.nodes.new("ShaderNodeMapRange")
    amb.inputs["From Min"].default_value = -1.0
    amb.inputs["From Max"].default_value = 1.0
    amb.inputs["To Min"].default_value = 0.03
    amb.inputs["To Max"].default_value = 0.30
    add = nt.nodes.new("ShaderNodeMath")
    add.operation = "ADD"
    nt.links.new(tc.outputs["Generated"], dot.inputs[0])
    nt.links.new(dot.outputs["Value"], mr.inputs["Value"])
    nt.links.new(tc.outputs["Generated"], up.inputs[0])
    nt.links.new(up.outputs["Z"], amb.inputs["Value"])
    nt.links.new(mr.outputs["Result"], add.inputs[0])
    nt.links.new(amb.outputs["Result"], add.inputs[1])
    bg.inputs["Color"].default_value = (1.0, 0.96, 0.9, 1)
    nt.links.new(add.outputs["Value"], bg.inputs["Strength"])
    nt.links.new(bg.outputs["Background"], out.inputs["Surface"])


def camera(cx, cy, width, height, px_per_pt):
    cam = bpy.data.cameras.new("cam")
    cam.type = "ORTHO"
    cam.ortho_scale = max(width, height)
    cam.clip_start = 1
    cam.clip_end = 3000
    co = bpy.data.objects.new("cam", cam)
    bpy.context.scene.collection.objects.link(co)
    co.location = (cx, -cy, 1500)
    co.rotation_euler = (0, 0, 0)
    sc = bpy.context.scene
    sc.camera = co
    sc.render.resolution_x = int(round(width * px_per_pt))
    sc.render.resolution_y = int(round(height * px_per_pt))
    sc.render.resolution_percentage = 100
    sc.render.pixel_aspect_x = sc.render.pixel_aspect_y = 1
    return co


# ---------------------------------------------------------------------------
# Materials
def new_mat(name):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    return m, nt, bsdf


def srgb(c):
    def f(v):
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    return (f(c[0]), f(c[1]), f(c[2]), 1.0)


def img(path, noncolor=True):
    im = bpy.data.images.load(path, check_existing=True)
    if noncolor:
        im.colorspace_settings.name = "Non-Color"
    return im


CANVAS = [L.W, L.H]


def canvas_uv(nt):
    """World position -> canvas UV (0..1 across the whole faceplate, or the tray)."""
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (1.0 / CANVAS[0], 1.0 / CANVAS[1], 1.0)
    mp.inputs["Location"].default_value = (0.0, 1.0, 0.0)
    nt.links.new(geo.outputs["Position"], mp.inputs["Vector"])
    return mp.outputs["Vector"]


def tex(nt, image, vec, interp="Cubic"):
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = image
    t.interpolation = interp
    t.extension = "CLIP"
    nt.links.new(vec, t.inputs["Vector"])
    return t


def math_node(nt, op, a, b=None, clamp=False):
    n = nt.nodes.new("ShaderNodeMath")
    n.operation = op
    n.use_clamp = clamp
    for i, v in enumerate((a, b)):
        if v is None:
            continue
        if isinstance(v, (int, float)):
            n.inputs[i].default_value = v
        else:
            nt.links.new(v, n.inputs[i])
    return n.outputs[0]


def mix_rgb(nt, fac, a, b, blend="MIX"):
    n = nt.nodes.new("ShaderNodeMix")
    n.data_type = "RGBA"
    n.blend_type = blend
    if isinstance(fac, (int, float)):
        n.inputs[0].default_value = fac
    else:
        nt.links.new(fac, n.inputs[0])
    for idx, v in ((6, a), (7, b)):
        if isinstance(v, tuple):
            n.inputs[idx].default_value = v
        else:
            nt.links.new(v, n.inputs[idx])
    return n.outputs[2]


def hammertone(name, base, metal, ink_layers, wear=True):
    """Hammer-finish paint with printed ink layers [(mask image, ink colour, roughness)]."""
    m, nt, bsdf = new_mat(name)
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    vor = nt.nodes.new("ShaderNodeTexVoronoi")
    vor.voronoi_dimensions = "3D"
    vor.feature = "F1"
    vor.inputs["Scale"].default_value = 0.17
    vor.inputs["Randomness"].default_value = 1.0
    nt.links.new(geo.outputs["Position"], vor.inputs["Vector"])
    noi = nt.nodes.new("ShaderNodeTexNoise")
    noi.inputs["Scale"].default_value = 0.9
    noi.inputs["Detail"].default_value = 6
    nt.links.new(geo.outputs["Position"], noi.inputs["Vector"])
    big = nt.nodes.new("ShaderNodeTexNoise")
    big.inputs["Scale"].default_value = 0.012
    big.inputs["Detail"].default_value = 3
    nt.links.new(geo.outputs["Position"], big.inputs["Vector"])

    dist = math_node(nt, "POWER", vor.outputs["Distance"], 0.8)
    bump1 = nt.nodes.new("ShaderNodeBump")
    bump1.inputs["Strength"].default_value = 0.22
    bump1.inputs["Distance"].default_value = 0.6
    nt.links.new(dist, bump1.inputs["Height"])
    bump2 = nt.nodes.new("ShaderNodeBump")
    bump2.inputs["Strength"].default_value = 0.05
    bump2.inputs["Distance"].default_value = 0.2
    nt.links.new(noi.outputs["Fac"], bump2.inputs["Height"])
    nt.links.new(bump1.outputs["Normal"], bump2.inputs["Normal"])

    uv = canvas_uv(nt)
    col = mix_rgb(nt, math_node(nt, "MULTIPLY", vor.outputs["Distance"], 0.25), base, (base[0] * 0.86, base[1] * 0.86, base[2] * 0.86, 1))
    col = mix_rgb(nt, math_node(nt, "MULTIPLY", big.outputs["Fac"], 0.35), col, (base[0] * 1.08, base[1] * 1.06, base[2] * 1.0, 1))
    rough = math_node(nt, "ADD", math_node(nt, "MULTIPLY", noi.outputs["Fac"], 0.12), 0.40)
    wear_fac = None
    if wear:
        wt = tex(nt, img(os.path.join(BUILD, "wear.png")), uv)
        gt = tex(nt, img(os.path.join(BUILD, "grime.png")), uv)
        wear_fac = wt.outputs["Color"]
        col = mix_rgb(nt, math_node(nt, "MULTIPLY", wt.outputs["Color"], 0.35), col, (base[0] * 1.25, base[1] * 1.25, base[2] * 1.22, 1))
        rough = math_node(nt, "SUBTRACT", rough, math_node(nt, "MULTIPLY", wt.outputs["Color"], 0.18))
        col = mix_rgb(nt, math_node(nt, "MULTIPLY", gt.outputs["Color"], 0.55), col, (0.10, 0.085, 0.06, 1))
        rough = math_node(nt, "ADD", rough, math_node(nt, "MULTIPLY", gt.outputs["Color"], 0.2))
    for path, ink, ink_rough in ink_layers:
        it = tex(nt, img(path), uv)
        fac = it.outputs["Color"]
        if wear_fac is not None:
            fac = math_node(nt, "MULTIPLY", fac, math_node(nt, "SUBTRACT", 1.0, math_node(nt, "MULTIPLY", wear_fac, 0.8)), clamp=True)
        col = mix_rgb(nt, fac, col, ink)
        rough = mix_rgb(nt, fac, rough, (ink_rough, ink_rough, ink_rough, 1))
    nt.links.new(col, bsdf.inputs["Base Color"])
    nt.links.new(rough, bsdf.inputs["Roughness"])
    bsdf.inputs["Metallic"].default_value = metal
    nt.links.new(bump2.outputs["Normal"], bsdf.inputs["Normal"])
    return m


def simple(name, color, metal=0.0, rough=0.4, **extra):
    m, nt, bsdf = new_mat(name)
    bsdf.inputs["Base Color"].default_value = color
    bsdf.inputs["Metallic"].default_value = metal
    bsdf.inputs["Roughness"].default_value = rough
    for k, v in extra.items():
        bsdf.inputs[k].default_value = v
    return m


def phenolic():
    """Black bakelite: glossy, with faint micro-scratches."""
    m, nt, bsdf = new_mat("phenolic")
    bsdf.inputs["Base Color"].default_value = (0.012, 0.011, 0.010, 1)
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    noi = nt.nodes.new("ShaderNodeTexNoise")
    noi.inputs["Scale"].default_value = 2.5
    noi.inputs["Detail"].default_value = 8
    nt.links.new(geo.outputs["Position"], noi.inputs["Vector"])
    nt.links.new(math_node(nt, "ADD", math_node(nt, "MULTIPLY", noi.outputs["Fac"], 0.18), 0.17), bsdf.inputs["Roughness"])
    bsdf.inputs["Coat Weight"].default_value = 0.35
    bsdf.inputs["Coat Roughness"].default_value = 0.12
    return m


def brushed_alu():
    m, nt, bsdf = new_mat("alu")
    bsdf.inputs["Base Color"].default_value = (0.80, 0.80, 0.79, 1)
    bsdf.inputs["Metallic"].default_value = 1.0
    bsdf.inputs["Roughness"].default_value = 0.26
    bsdf.inputs["Anisotropic"].default_value = 0.85
    # Radial (spun) brushing: tangent around the knob axis.
    tg = nt.nodes.new("ShaderNodeTangent")
    tg.direction_type = "RADIAL"
    tg.axis = "Z"
    nt.links.new(tg.outputs["Tangent"], bsdf.inputs["Tangent"])
    return m


def paint_white():
    return simple("paint_white", srgb((0.93, 0.92, 0.87)), rough=0.45)


def glass(name, color=(1, 1, 1, 1), rough=0.02):
    m, nt, bsdf = new_mat(name)
    bsdf.inputs["Base Color"].default_value = color
    bsdf.inputs["Transmission Weight"].default_value = 1.0
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["IOR"].default_value = 1.5
    return m


# ---------------------------------------------------------------------------
# Geometry helpers
def link(obj):
    bpy.context.scene.collection.objects.link(obj)
    return obj


def mesh_obj(name, bm, mat=None):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    o = link(bpy.data.objects.new(name, me))
    if mat:
        o.data.materials.append(mat)
    return o


def box(name, x0, y0, x1, y1, z0, z1, mat, bevel=1.0):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    o = mesh_obj(name, bm, mat)
    o.scale = (x1 - x0, y1 - y0, z1 - z0)
    o.location = ((x0 + x1) / 2, -(y0 + y1) / 2, (z0 + z1) / 2)
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    o.select_set(False)
    if bevel > 0:
        b = o.modifiers.new("bevel", "BEVEL")
        b.width = bevel
        b.segments = 4
        b.limit_method = "ANGLE"
    return o


def prism(name, outline, z0, z1, mat, bevel=0.0, taper=1.0, smooth=True):
    """Extrude a closed 2D outline (points in local xy) from z0 to z1."""
    bm = bmesh.new()
    vs = [bm.verts.new((x, y, z0)) for x, y in outline]
    f = bm.faces.new(vs)
    ext = bmesh.ops.extrude_face_region(bm, geom=[f])
    top = [e for e in ext["geom"] if isinstance(e, bmesh.types.BMVert)]
    for v in top:
        v.co.z = z1
        v.co.x *= taper
        v.co.y *= taper
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    o = mesh_obj(name, bm, mat)
    if smooth:
        for p in o.data.polygons:
            p.use_smooth = True
    if bevel > 0:
        b = o.modifiers.new("bevel", "BEVEL")
        b.width = bevel
        b.segments = 5
        b.limit_method = "ANGLE"
        b.angle_limit = math.radians(40)
        o.modifiers.new("wn", "WEIGHTED_NORMAL")
    return o


def circle_pts(r, n=96, flutes=0, depth=0.0, phase=0.0):
    pts = []
    for i in range(n):
        a = 2 * math.pi * i / n + phase
        rr = r
        if flutes:
            rr = r - depth * (0.5 - 0.5 * math.cos(flutes * a)) ** 3
        pts.append((rr * math.cos(a), rr * math.sin(a)))
    return pts


def rounded_rect_pts(w, h, rad, n=8):
    pts = []
    corners = [(w / 2 - rad, h / 2 - rad, 0), (-w / 2 + rad, h / 2 - rad, 90), (-w / 2 + rad, -h / 2 + rad, 180), (w / 2 - rad, -h / 2 + rad, 270)]
    for cx_, cy_, a0 in corners:
        for i in range(n + 1):
            a = math.radians(a0 + 90 * i / n)
            pts.append((cx_ + rad * math.cos(a), cy_ + rad * math.sin(a)))
    return pts


def place(objs, x, y, z=0.0, rot_deg=0.0):
    for o in objs:
        o.location.x += x
        o.location.y += -y
        o.location.z += z
        if rot_deg:
            # rotate about the part's axis at (x, -y)
            o.rotation_euler.z += math.radians(-rot_deg)
    return objs


def parent_to(objs, name="part"):
    root = link(bpy.data.objects.new(name, None))
    for o in objs:
        o.parent = root
    return root


# ---------------------------------------------------------------------------
# Parts. Each builder returns objects centred on (0, 0) sitting on z = 0.
_mats = {}


def mats():
    if not _mats:
        _mats["phen"] = phenolic()
        _mats["alu"] = brushed_alu()
        _mats["white"] = paint_white()
        _mats["nickel"] = simple("nickel", srgb((0.78, 0.76, 0.72)), metal=1.0, rough=0.24)
        _mats["chrome"] = simple("chrome", srgb((0.86, 0.86, 0.86)), metal=1.0, rough=0.10)
        _mats["dark"] = simple("slot", (0.01, 0.01, 0.01, 1), rough=0.8)
        _mats["black_plastic"] = simple("black_plastic", (0.02, 0.02, 0.022, 1), rough=0.35)
    return _mats


def pointer_mat(colour):
    key = "ptr_" + (colour or "white")
    M = mats()
    if key not in M:
        if colour is None:
            M[key] = M["white"]
        else:
            mul = L.POINTER_COLOURS[colour]
            base = (0.93, 0.92, 0.87)
            M[key] = simple(key, srgb((base[0] * mul[0], base[1] * mul[1], base[2] * mul[2])), rough=0.45)
    return M[key]


def knob(kind, pointer=None):
    """Knob pointing at 12 o'clock (canvas up = +Y)."""
    M = mats()
    PM = pointer_mat(pointer)
    r = L.KNOBS[kind]
    h = KNOB_H[kind]
    objs = []
    if kind == "sel":
        # A bar ("chicken-head") knob on a round boss.
        objs.append(prism("boss", circle_pts(r * 0.62, 64), 0, 4.0, M["phen"], bevel=0.8))
        tip, tail, wid = r * 0.98, r * 0.80, r * 0.56
        out = []
        n = 10
        for i in range(n + 1):           # rounded tail
            a = math.pi + math.pi * i / n
            out.append((wid / 2 * math.cos(a) * 1.0, -tail + wid / 2 * 0.9 + wid / 2 * math.sin(a) * 0.9))
        out = [(x, y) for x, y in out]
        out = [(wid / 2, -tail + wid * 0.45)] + out[::-1] if False else out
        body = [(-wid / 2, -tail + wid * 0.45)] + [(p[0], p[1]) for p in out] + [(wid / 2, -tail + wid * 0.45), (wid * 0.32, tip - 4), (wid * 0.12, tip), (-wid * 0.12, tip), (-wid * 0.32, tip - 4)]
        body = sorted_ccw(body)
        objs.append(prism("bar", body, 3.5, h, M["phen"], bevel=1.6, taper=0.94))
        objs.append(prism("line", [(-0.85, 2.0), (0.85, 2.0), (0.85, tip - 2.5), (-0.85, tip - 2.5)], h - 0.6, h + 0.12, M["white"], smooth=False))
        return objs
    # Round knobs: skirt, fluted body, spun aluminium cap, white index.
    objs.append(prism("skirt", circle_pts(r, 128), 0, 3.6, M["phen"], bevel=0.9, taper=0.98))
    objs.append(prism("index_skirt", [(-0.95, r * 0.77), (0.95, r * 0.77), (0.95, r * 0.975), (-0.95, r * 0.975)], 3.3, 3.62, PM, smooth=False))
    body_r = r * 0.74
    objs.append(prism("body", circle_pts(body_r, 240, flutes=24, depth=r * 0.06), 3.4, h, M["phen"], bevel=1.5, taper=0.95))
    cap_r = body_r * 0.66
    if kind != "small":
        objs.append(prism("cap", circle_pts(cap_r, 96), h - 0.6, h + 0.25, M["alu"], bevel=0.35))
        objs.append(prism("index_top", [(-0.8, cap_r + 0.6), (0.8, cap_r + 0.6), (0.8, body_r * 0.95 - 1.2), (-0.8, body_r * 0.95 - 1.2)], h - 0.3, h + 0.05, PM, smooth=False))
    else:
        objs.append(prism("index_top", [(-0.8, 1.5), (0.8, 1.5), (0.8, body_r * 0.95 - 1.2), (-0.8, body_r * 0.95 - 1.2)], h - 0.3, h + 0.05, PM, smooth=False))
    return objs


def sorted_ccw(pts):
    cxp = sum(p[0] for p in pts) / len(pts)
    cyp = sum(p[1] for p in pts) / len(pts)
    return sorted(pts, key=lambda p: math.atan2(p[1] - cyp, p[0] - cxp))


def screw(rot):
    M = mats()
    objs = [prism("screw_head", circle_pts(4.0, 64), 0, 1.4, M["nickel"], bevel=0.7)]
    s = prism("slot", [(-3.6, -0.45), (3.6, -0.45), (3.6, 0.45), (-3.6, 0.45)], 0.9, 1.5, M["dark"], smooth=False)
    s.rotation_euler.z = math.radians(rot)
    objs.append(s)
    return objs


def toggle(up):
    M = mats()
    hexpts = [(6.2 * math.cos(math.radians(30 + 60 * i)), 6.2 * math.sin(math.radians(30 + 60 * i))) for i in range(6)]
    objs = [prism("washer", circle_pts(7.6, 64), 0, 0.6, M["nickel"], bevel=0.2),
            prism("nut", hexpts, 0.6, 2.8, M["nickel"], bevel=0.4, smooth=False),
            prism("bush", circle_pts(3.9, 48), 2.8, 5.5, M["nickel"], bevel=0.3)]
    # The bat lever, tilted toward its label.
    lev = prism("lever", circle_pts(1.5, 32), 0, 15.5, M["chrome"], bevel=0.4, taper=1.55)
    tipb = prism("lever_tip", circle_pts(2.3, 32), 15.0, 16.6, M["chrome"], bevel=0.8)
    for o in (lev, tipb):
        o.location.z = 4.6
        o.rotation_euler.x = math.radians(-27 if up else 27)
    objs += [lev, tipb]
    return objs


def label_texture(text, w, h, path, dark=True):
    S = 24
    im = Image.new("L", (int(w * S), int(h * S)), 0)
    d = ImageDraw.Draw(im)
    if text in ("◂", "▸"):     # arrows are drawn: the legend font has no triangles
        cx_, cy_, a = im.size[0] / 2, im.size[1] / 2, min(w, h) * S * 0.26
        sgn = -1 if text == "◂" else 1
        d.polygon([(cx_ + sgn * a, cy_), (cx_ - sgn * a * 0.7, cy_ - a), (cx_ - sgn * a * 0.7, cy_ + a)], fill=255)
    else:
        f = ImageFont.truetype(os.path.join(HERE, "fonts", "BarlowSemiCondensed-SemiBold.ttf"), int(min(h * 0.62, 9) * S))
        d.text((im.size[0] / 2, im.size[1] / 2), text, font=f, fill=255, anchor="mm")
    im.save(path)
    return path


def lens_mat(name, text_path, lit, small=False, tint="amber"):
    m, nt, bsdf = new_mat(name)
    uvn = nt.nodes.new("ShaderNodeTexCoord")
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = img(text_path)
    t.extension = "CLIP"
    nt.links.new(uvn.outputs["UV"], t.inputs["Vector"])
    if small and not lit:
        base = (0.03, 0.03, 0.03, 1)
        ink = srgb((0.9, 0.9, 0.86))
    else:
        # An opal lens: cream when dark, warm amber with the lamp behind it.
        lit_base = srgb((0.80, 0.97, 0.80)) if tint == "green" else srgb((0.99, 0.79, 0.50))
        base = lit_base if lit else srgb((0.88, 0.84, 0.72))
        ink = srgb((0.12, 0.10, 0.08))
    col = mix_rgb(nt, t.outputs["Color"], base, ink)
    nt.links.new(col, bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.32
    bsdf.inputs["Subsurface Weight"].default_value = 0.0 if (small and not lit) else 0.4
    bsdf.inputs["Subsurface Radius"].default_value = (2.0, 1.2, 0.6)
    if lit:
        glow = srgb((0.35, 1.0, 0.55)) if tint == "green" else srgb((1.0, 0.50, 0.12))
        em = mix_rgb(nt, t.outputs["Color"], glow, (0.02, 0.012, 0.004, 1))
        nt.links.new(em, bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = 1.4
    return m


def uv_plane_box(o, w, h):
    """UVs from local xy over a w x h rectangle (top faces)."""
    me = o.data
    uvl = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        for li in poly.loop_indices:
            v = me.vertices[me.loops[li].vertex_index].co
            uvl.data[li].uv = (v.x / w + 0.5, v.y / h + 0.5)


def button(w, h, text, lit, small=False, tint="amber"):
    M = mats()
    objs = [prism("bezel", rounded_rect_pts(w, h, 2.5), 0, 2.2, M["chrome"] if not small else M["black_plastic"], bevel=0.8)]
    lw, lh = w - 6, h - 6
    tp = label_texture(text, lw, lh, os.path.join(BUILD, f"lab_{abs(hash((text, w, h)))}.png"))
    lens = prism("lens", rounded_rect_pts(lw, lh, 1.6), 1.5, 4.2, lens_mat(f"lens_{text}_{lit}_{small}_{tint}_{w}", tp, lit, small, tint), bevel=0.7)
    uv_plane_box(lens, lw, lh)
    objs.append(lens)
    return objs


def jewel(color, lit, r=6.0):
    M = mats()
    objs = [prism("lamp_ring", circle_pts(r + 1.6, 64), 0, 2.0, M["chrome"], bevel=0.9)]
    m, nt, bsdf = new_mat(f"jewel_{color}_{lit}")
    c = {"red": (1.0, 0.05, 0.02), "amber": (1.0, 0.28, 0.02)}[color]
    bsdf.inputs["Base Color"].default_value = (*c, 1)
    bsdf.inputs["Transmission Weight"].default_value = 0.6
    bsdf.inputs["Roughness"].default_value = 0.05
    if lit:
        bsdf.inputs["Emission Color"].default_value = (*c, 1)
        bsdf.inputs["Emission Strength"].default_value = 0.9
    else:
        bsdf.inputs["Base Color"].default_value = (c[0] * 0.35, c[1] * 0.3, c[2] * 0.3, 1)
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=2, radius=r)
    j = mesh_obj("jewel", bm, m)
    j.scale = (1, 1, 0.55)
    j.location.z = 1.0
    objs.append(j)
    return objs


def meter(kind, w, h, face_path, with_glass):
    M = mats()
    inset = 8.0
    fw, fh = w - 2 * inset, h - 2 * inset
    # Bezel: a rounded frame (2D curve with a hole), extruded.
    cu = bpy.data.curves.new("bezel", "CURVE")
    cu.dimensions = "2D"
    cu.fill_mode = "BOTH"
    cu.extrude = 2.2
    cu.bevel_depth = 1.0
    cu.bevel_resolution = 4
    for pts in (rounded_rect_pts(w, h, 6.0), rounded_rect_pts(fw, fh, 3.0)):
        sp = cu.splines.new("POLY")
        sp.points.add(len(pts) - 1)
        for i, (x, y) in enumerate(pts):
            sp.points[i].co = (x, y, 0, 1)
        sp.use_cyclic_u = True
    bez = link(bpy.data.objects.new("bezel", cu))
    bez.data.materials.append(M["phen"])
    bez.location.z = 3.2
    objs = [bez]
    # Face, recessed, faintly backlit.
    m, nt, bsdf = new_mat(f"face_{kind}_{os.path.basename(face_path)}")
    uvn = nt.nodes.new("ShaderNodeTexCoord")
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = img(face_path, noncolor=False)
    t.interpolation = "Cubic"
    nt.links.new(uvn.outputs["UV"], t.inputs["Vector"])
    nt.links.new(t.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(t.outputs["Color"], bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = 0.10
    bsdf.inputs["Roughness"].default_value = 0.7
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=0.5)
    face = mesh_obj("face", bm, m)
    face.scale = (fw + 0.5, fh + 0.5, 1)
    face.location.z = 0.6
    face.data.uv_layers.new(name="UVMap")
    uvl = face.data.uv_layers[0]
    for poly in face.data.polygons:
        for li in poly.loop_indices:
            v = face.data.vertices[face.data.loops[li].vertex_index].co
            uvl.data[li].uv = (v.x + 0.5, v.y + 0.5)
    objs.append(face)
    # A shadow lip inside the bezel (the face sits in a box).
    lip = prism("lip", rounded_rect_pts(fw + 1.6, fh + 1.6, 3.0), 0.0, 0.3, M["dark"])
    objs.append(lip)
    if with_glass:
        bm = bmesh.new()
        bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=0.5)
        g = mesh_obj("glass", bm, glass("meter_glass", rough=0.015))
        g.scale = (fw + 2, fh + 2, 1)
        g.location.z = 4.6
        # A slight curve so the reflection reads as glass, not a decal.
        g.rotation_euler.x = math.radians(1.2)
        objs.append(g)
    return objs


def tape(spec, alpha_name, crepe_name, ink_name, lift=1.6, z=None):
    pad = 6
    w, h = spec["w"] + 2 * pad, spec["h"] + 2 * pad
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=90, y_segments=16, size=0.5)
    for v in bm.verts:
        x, y = v.co.x, v.co.y
        v.co.x, v.co.y = x * w, y * h
        # Right end lifts a little; small undulations elsewhere.
        t = max(0.0, (x + 0.5 - 0.84) / 0.16)
        v.co.z = 0.22 + lift * t * t + 0.05 * math.sin(x * 9.0 + y * 2.0)
    me = bpy.data.meshes.new("tape")
    bm.to_mesh(me)
    bm.free()
    o = link(bpy.data.objects.new("tape", me))
    for p in me.polygons:
        p.use_smooth = True
    uvl = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        for li in poly.loop_indices:
            v = me.vertices[me.loops[li].vertex_index].co
            uvl.data[li].uv = (v.x / w + 0.5, v.y / h + 0.5)
    m, nt, bsdf = new_mat("tape_" + alpha_name)
    uvn = nt.nodes.new("ShaderNodeTexCoord")
    ta = tex(nt, img(os.path.join(BUILD, alpha_name)), uvn.outputs["UV"], "Linear")
    tc = tex(nt, img(os.path.join(BUILD, crepe_name)), uvn.outputs["UV"], "Linear")
    paper = srgb((0.86, 0.80, 0.63))
    col = mix_rgb(nt, math_node(nt, "MULTIPLY", tc.outputs["Color"], 0.25), paper, srgb((0.95, 0.90, 0.74)))
    rough = 0.82
    if ink_name:
        ti = tex(nt, img(os.path.join(BUILD, ink_name)), uvn.outputs["UV"], "Linear")
        col = mix_rgb(nt, ti.outputs["Color"], col, srgb((0.07, 0.07, 0.10)))
        rough = mix_rgb(nt, ti.outputs["Color"], (0.82, 0.82, 0.82, 1), (0.5, 0.5, 0.5, 1))
    nt.links.new(col, bsdf.inputs["Base Color"])
    if isinstance(rough, float):
        bsdf.inputs["Roughness"].default_value = rough
    else:
        nt.links.new(rough, bsdf.inputs["Roughness"])
    nt.links.new(ta.outputs["Color"], bsdf.inputs["Alpha"])
    bsdf.inputs["Subsurface Weight"].default_value = 0.15
    bsdf.inputs["Subsurface Radius"].default_value = (1.0, 0.8, 0.5)
    bp = nt.nodes.new("ShaderNodeBump")
    bp.inputs["Strength"].default_value = 0.35
    bp.inputs["Distance"].default_value = 0.15
    nt.links.new(tc.outputs["Color"], bp.inputs["Height"])
    nt.links.new(bp.outputs["Normal"], bsdf.inputs["Normal"])
    o.data.materials.append(m)
    o.location = (spec["x"], -spec["y"], (Z_FRAME if z is None else z) + 0.05)
    o.rotation_euler.z = math.radians(-spec["angle"])
    return o


def latch():
    """A quarter-turn fastener: a knurled nickel ring round a domed, slotted head."""
    M = mats()
    r = L.SPACE_LATCH["r"]
    objs = [prism("latch_ring", circle_pts(r, 160, flutes=40, depth=0.35), 0, 1.6, M["nickel"], bevel=0.35),
            prism("latch_head", circle_pts(r * 0.68, 96), 1.3, 2.6, M["chrome"], bevel=0.8)]
    sl = prism("latch_slot", [(-r * 0.62, -0.5), (r * 0.62, -0.5), (r * 0.62, 0.5), (-r * 0.62, 0.5)], 2.0, 2.7, M["dark"], smooth=False)
    sl.rotation_euler.z = math.radians(-35)
    objs.append(sl)
    return objs


def sticker_obj():
    """The inspection sticker: paper on the cassette, its edges just lifting."""
    st = L.STICKER
    w, h = st["w"], st["h"]
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=40, y_segments=20, size=0.5)
    for v in bm.verts:
        x, y = v.co.x, v.co.y
        v.co.x, v.co.y = x * w, y * h
        edge = max(abs(x), abs(y)) * 2           # 0 centre .. 1 edge
        lift = 0.25 * max(0.0, (x + 0.5 - 0.88) / 0.12) ** 2 if y > 0.3 else 0.0   # one corner coming up
        v.co.z = 0.12 + 0.05 * edge ** 6 + lift
    me = bpy.data.meshes.new("sticker")
    bm.to_mesh(me)
    bm.free()
    o = link(bpy.data.objects.new("sticker", me))
    for poly in me.polygons:
        poly.use_smooth = True
    uvl = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        for li in poly.loop_indices:
            v = me.vertices[me.loops[li].vertex_index].co
            uvl.data[li].uv = (v.x / w + 0.5, v.y / h + 0.5)
    m, nt, bsdf = new_mat("sticker")
    uvn = nt.nodes.new("ShaderNodeTexCoord")
    tc = tex(nt, img(os.path.join(BUILD, "sticker_color.png"), noncolor=False), uvn.outputs["UV"], "Linear")
    ta = tex(nt, img(os.path.join(BUILD, "sticker_alpha.png")), uvn.outputs["UV"], "Linear")
    nt.links.new(tc.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(ta.outputs["Color"], bsdf.inputs["Alpha"])
    bsdf.inputs["Roughness"].default_value = 0.42
    bsdf.inputs["Coat Weight"].default_value = 0.15   # a varnished paper label
    o.data.materials.append(m)
    o.location = (st["x"], -st["y"], Z_CASS)
    o.rotation_euler.z = math.radians(-st["angle"])
    return o


def serial_plate():
    """The maker's plate: satin aluminium, engraved and filled, two rivets."""
    p = L.SERIAL_PLATE
    w, h = p["w"], p["h"]
    M = mats()
    body = box("plate", p["x"] - w / 2, p["y"] - h / 2, p["x"] + w / 2, p["y"] + h / 2, Z_FRAME, Z_FRAME + 0.9,
               simple("plate_edge", srgb((0.74, 0.73, 0.70)), metal=1.0, rough=0.35), bevel=0.5)
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=0.5)
    face = mesh_obj("plate_face", bm)
    face.scale = (w - 1.0, h - 1.0, 1)
    face.location = (p["x"], -p["y"], Z_FRAME + 0.92)
    face.data.uv_layers.new(name="UVMap")
    uvl = face.data.uv_layers[0]
    for poly in face.data.polygons:
        for li in poly.loop_indices:
            v = face.data.vertices[face.data.loops[li].vertex_index].co
            uvl.data[li].uv = (v.x + 0.5, v.y + 0.5)
    m, nt, bsdf = new_mat("plate_face")
    uvn = nt.nodes.new("ShaderNodeTexCoord")
    ti = tex(nt, img(os.path.join(BUILD, "plate_ink.png")), uvn.outputs["UV"], "Linear")
    noi = nt.nodes.new("ShaderNodeTexNoise")
    noi.inputs["Scale"].default_value = 140.0
    noi.inputs["Detail"].default_value = 4
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (0.04, 1.0, 1.0)   # stretched along x: brushed
    nt.links.new(uvn.outputs["UV"], mp.inputs["Vector"])
    nt.links.new(mp.outputs["Vector"], noi.inputs["Vector"])
    metal_col = mix_rgb(nt, math_node(nt, "MULTIPLY", noi.outputs["Fac"], 0.25), srgb((0.80, 0.79, 0.76)), srgb((0.62, 0.61, 0.58)))
    col = mix_rgb(nt, ti.outputs["Color"], metal_col, srgb((0.05, 0.045, 0.04)))
    nt.links.new(col, bsdf.inputs["Base Color"])
    nt.links.new(math_node(nt, "SUBTRACT", 1.0, ti.outputs["Color"]), bsdf.inputs["Metallic"])
    bsdf.inputs["Roughness"].default_value = 0.34
    face.data.materials.append(m)
    for sx in (-1, 1):
        bm = bmesh.new()
        bmesh.ops.create_uvsphere(bm, u_segments=24, v_segments=12, radius=2.0)
        r = mesh_obj("rivet", bm, M["nickel"])
        r.scale = (1, 1, 0.45)
        r.location = (p["x"] + sx * (w / 2 - 5.0), -p["y"], Z_FRAME + 0.9)
        for poly in r.data.polygons:
            poly.use_smooth = True
    return body


def nameplate(p=None, ink="nameplate_ink.png"):
    """The model plate: black anodised aluminium, the name engraved through to bright metal,
    polished bevel, two domed rivets."""
    p = p or L.NAMEPLATE
    w, h = p["w"], p["h"]
    M = mats()
    body = box("nameplate", p["x"] - w / 2, p["y"] - h / 2, p["x"] + w / 2, p["y"] + h / 2, Z_FRAME, Z_FRAME + 1.1,
               simple("nameplate_edge", srgb((0.70, 0.69, 0.66)), metal=1.0, rough=0.22), bevel=0.7)
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=0.5)
    face = mesh_obj("nameplate_face", bm)
    face.scale = (w - 1.6, h - 1.6, 1)
    face.location = (p["x"], -p["y"], Z_FRAME + 1.12)
    face.data.uv_layers.new(name="UVMap")
    uvl = face.data.uv_layers[0]
    for poly in face.data.polygons:
        for li in poly.loop_indices:
            v = face.data.vertices[face.data.loops[li].vertex_index].co
            uvl.data[li].uv = (v.x + 0.5, v.y + 0.5)
    m, nt, bsdf = new_mat("nameplate_face_" + ink)
    uvn = nt.nodes.new("ShaderNodeTexCoord")
    ti = tex(nt, img(os.path.join(BUILD, ink)), uvn.outputs["UV"], "Linear")
    noi = nt.nodes.new("ShaderNodeTexNoise")
    noi.inputs["Scale"].default_value = 160.0
    noi.inputs["Detail"].default_value = 4
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (0.04, 1.0, 1.0)   # brushed along x
    nt.links.new(uvn.outputs["UV"], mp.inputs["Vector"])
    nt.links.new(mp.outputs["Vector"], noi.inputs["Vector"])
    anod = mix_rgb(nt, math_node(nt, "MULTIPLY", noi.outputs["Fac"], 0.35), srgb((0.030, 0.030, 0.032)), srgb((0.060, 0.059, 0.062)))
    bright = mix_rgb(nt, math_node(nt, "MULTIPLY", noi.outputs["Fac"], 0.3), srgb((0.86, 0.85, 0.82)), srgb((0.70, 0.69, 0.66)))
    col = mix_rgb(nt, ti.outputs["Color"], anod, bright)
    nt.links.new(col, bsdf.inputs["Base Color"])
    nt.links.new(math_node(nt, "ADD", math_node(nt, "MULTIPLY", ti.outputs["Color"], 0.55), 0.45), bsdf.inputs["Metallic"])
    nt.links.new(math_node(nt, "SUBTRACT", 0.42, math_node(nt, "MULTIPLY", ti.outputs["Color"], 0.14)), bsdf.inputs["Roughness"])
    face.data.materials.append(m)
    for sx in (-1, 1):
        bm = bmesh.new()
        bmesh.ops.create_uvsphere(bm, u_segments=24, v_segments=12, radius=2.1)
        rv = mesh_obj("nameplate_rivet", bm, M["nickel"])
        rv.scale = (1, 1, 0.45)
        rv.location = (p["x"] + sx * (w / 2 - 6.5), -p["y"], Z_FRAME + 1.1)
        for poly in rv.data.polygons:
            poly.use_smooth = True
    return body


# ---------------------------------------------------------------------------
def frame_curve(name, outer, inner, extrude, bevel, mat):
    """A flat frame (outer outline with an inner hole), extruded and bevelled."""
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "2D"
    cu.fill_mode = "BOTH"
    cu.extrude = extrude
    cu.bevel_depth = bevel
    cu.bevel_resolution = 4
    for pts in (outer, inner):
        sp = cu.splines.new("POLY")
        sp.points.add(len(pts) - 1)
        for i, (x, y) in enumerate(pts):
            sp.points[i].co = (x, y, 0, 1)
        sp.use_cyclic_u = True
    o = link(bpy.data.objects.new(name, cu))
    o.data.materials.append(mat)
    return o


def crt():
    """The tube: a thick phenolic bezel, a nickel mask, and a domed dark-green face
    with the graticule etched in it, lit faintly from the edge."""
    M = mats()
    w, h = L.CRT["w"], L.CRT["h"]
    F = L.FACE
    fw, fh = F["x1"] - F["x0"], F["y1"] - F["y0"]
    objs = []
    bez = frame_curve("crt_bezel", rounded_rect_pts(w, h, 16.0), rounded_rect_pts(fw + 3.0, fh + 3.0, 11.0), 3.0, 1.5, M["phen"])
    bez.location.z = 3.7
    objs.append(bez)
    mask = frame_curve("crt_mask", rounded_rect_pts(fw + 4.0, fh + 4.0, 11.5), rounded_rect_pts(fw - 1.0, fh - 1.0, 9.0), 0.35, 0.25, M["nickel"])
    mask.location.z = 1.6
    objs.append(mask)
    # The face: a gently domed sheet, glossy, near black-green, with the graticule glowing in it.
    m, nt, bsdf = new_mat("crt_face")
    uvn = nt.nodes.new("ShaderNodeTexCoord")
    g = tex(nt, img(os.path.join(BUILD, "crt_graticule.png")), uvn.outputs["UV"], "Linear")
    grain = nt.nodes.new("ShaderNodeTexNoise")
    grain.inputs["Scale"].default_value = 900.0
    grain.inputs["Detail"].default_value = 2
    nt.links.new(uvn.outputs["UV"], grain.inputs["Vector"])
    base = mix_rgb(nt, math_node(nt, "MULTIPLY", grain.outputs["Fac"], 0.5), (0.010, 0.024, 0.017, 1), (0.016, 0.034, 0.024, 1))
    nt.links.new(base, bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.06
    bsdf.inputs["Coat Weight"].default_value = 0.6
    bsdf.inputs["Coat Roughness"].default_value = 0.03
    glow = math_node(nt, "ADD", math_node(nt, "MULTIPLY", g.outputs["Color"], 1.0), 0.05)
    em = mix_rgb(nt, 1.0, (0, 0, 0, 1), srgb((0.55, 1.0, 0.72)))
    nt.links.new(em, bsdf.inputs["Emission Color"])
    nt.links.new(math_node(nt, "MULTIPLY", glow, 0.17), bsdf.inputs["Emission Strength"])
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=60, y_segments=36, size=0.5)
    for v in bm.verts:
        x, y = v.co.x, v.co.y
        v.co.x, v.co.y = x * (fw + 2), y * (fh + 2)
        v.co.z = 0.8 + 1.6 * (1 - (2 * x) ** 2 * 0.55 - (2 * y) ** 2 * 0.45)
    face = mesh_obj("crt_face", bm, m)
    for p in face.data.polygons:
        p.use_smooth = True
    uvl = face.data.uv_layers.new(name="UVMap")
    for poly in face.data.polygons:
        for li in poly.loop_indices:
            v = face.data.vertices[face.data.loops[li].vertex_index].co
            uvl.data[li].uv = ((v.x + 1) / (fw + 2) + 0.5 - 1 / (fw + 2), (v.y + 1) / (fh + 2) + 0.5 - 1 / (fh + 2))
    objs.append(face)
    lip = prism("crt_lip", rounded_rect_pts(fw + 3.5, fh + 3.5, 11.0), 0.0, 0.5, M["dark"])
    objs.append(lip)
    return objs


def drum(spec):
    """A readout: drum wheels behind a small glazed window."""
    M = mats()
    w, h = spec["w"], spec["h"]
    objs = [frame_curve("drum_bezel", rounded_rect_pts(w, h, 3.0), rounded_rect_pts(w - 6, h - 6, 1.5), 1.4, 0.6, M["phen"])]
    objs[0].location.z = 1.6
    m, nt, bsdf = new_mat("drum_face_" + spec["id"])
    uvn = nt.nodes.new("ShaderNodeTexCoord")
    drum_png = os.path.join(BUILD, f"drum_{spec['id']}{'_blank' if STATE else ''}.png")
    t = tex(nt, img(drum_png, noncolor=False), uvn.outputs["UV"], "Linear")
    nt.links.new(t.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.5
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=0.5)
    face = mesh_obj("drum_face", bm, m)
    face.scale = (w - 6, h - 6, 1)
    face.location.z = 0.3
    face.data.uv_layers.new(name="UVMap")
    uvl = face.data.uv_layers[0]
    for poly in face.data.polygons:
        for li in poly.loop_indices:
            v = face.data.vertices[face.data.loops[li].vertex_index].co
            uvl.data[li].uv = (v.x + 0.5, v.y + 0.5)
    objs.append(face)
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=0.5)
    gl = mesh_obj("drum_glass", bm, glass("drum_glass", rough=0.02))
    gl.scale = (w - 5, h - 5, 1)
    gl.location.z = 2.4
    objs.append(gl)
    return objs


def vu_angle(vu):
    f = 10 ** (vu / 20) / 10 ** (3 / 20)
    return -42.0 + 2 * 42.0 * f


def needle(kind, ctl, db):
    """The meter needle, pivoting just below the window."""
    import json
    geo = json.load(open(os.path.join(BUILD, "meter_geometry.json")))[kind]
    fw, fh = geo["fw"], geo["fh"]
    px, py = geo["pivot"]
    R = geo["radius"]
    if kind == "vu":
        ang = vu_angle(db)
    elif kind == "lufs":
        ang = -42.0 + 84.0 * (min(max(db, -24.0), -6.0) + 24.0) / 18.0
    elif kind == "plr":
        ang = -42.0 + 84.0 * (min(max(db, 3.0), 18.0) - 3.0) / 15.0
    else:
        ang = 42.0 * max(-12.0, min(12.0, db)) / 12.0
    nd = prism("needle", [(-0.42, 0.0), (0.42, 0.0), (0.22, R + 5.0), (-0.22, R + 5.0)], 0.0, 0.25,
               simple("needle", (0.015, 0.014, 0.012, 1), rough=0.5), smooth=False)
    nd.rotation_euler.z = math.radians(-ang)
    # pivot in canvas coordinates
    cx_ = ctl["x"] - fw / 2 + px
    cy_ = ctl["y"] - fh / 2 + py
    nd.location = (cx_, -cy_, Z_CASS + 1.25)
    return nd


def build_panel(regions=None):
    """The whole faceplate as the mockup shows it: every knob, switch and lamp in place."""
    sc = reset()
    lights()
    camera(L.W / 2, L.H / 2, L.W, L.H, 1 if DRAFT else L.SCALE)
    box("floor", -50, -50, L.W + 50, L.H + 50, -4, -2, simple("floor", (0.004, 0.004, 0.004, 1), rough=0.9), bevel=0)

    rail = hammertone("rail", srgb((0.30, 0.315, 0.33)), 0.35,
                      [(os.path.join(BUILD, "brand_ink.png"), srgb((0.86, 0.82, 0.70)), 0.5),
                       (os.path.join(BUILD, "rail_ink.png"), srgb((0.86, 0.83, 0.74)), 0.5),
                       (os.path.join(BUILD, "coffee.png"), srgb((0.42, 0.29, 0.16)), 0.3)], wear=False)
    ct, cb = L.CASS_Y
    box("rail_top", 0, 0, L.W, ct, 0, Z_FRAME, rail, bevel=1.2)
    box("rail_bot", 0, cb, L.W, L.H, 0, Z_FRAME, rail, bevel=1.2)
    box("side_l", 0, ct - 1, 24, cb + 1, 0, Z_FRAME - 0.4, rail, bevel=0.8)
    box("side_r", 1176, ct - 1, L.W, cb + 1, 0, Z_FRAME - 0.4, rail, bevel=0.8)

    cass = hammertone("cassette", srgb((0.72, 0.705, 0.655)), 0.22,
                      [(os.path.join(BUILD, "legend_ink.png"), srgb((0.045, 0.042, 0.038)), 0.32),
                       (os.path.join(BUILD, "grease_ink.png"), srgb((0.66, 0.11, 0.07)), 0.55)])
    for c in L.CASSETTES:
        box("cass_" + c["id"], c["x0"] + 1.0, ct + 1.0, c["x1"] - 1.0, cb - 1.0, 0, Z_CASS, cass, bevel=1.4)

    sticker_obj()
    serial_plate()

    for i, (x, y) in enumerate(L.SCREWS):
        z = Z_FRAME if (y < ct or y > cb or x < 24 or x > 1176) else Z_CASS
        place(screw((i * 47) % 180), x, y, z)
    for i, (x, y) in enumerate(L.CRT_SCREWS):
        place(screw((i * 61 + 20) % 180), x, y, Z_CASS + 6.6)

    place(crt(), L.CRT["x"], L.CRT["y"], Z_CASS)
    for r in L.READOUTS:
        place(drum(r), r["x"], r["y"], Z_CASS)

    for ctl in L.CONTROLS:
        t = ctl["type"]
        if STATE and t in ("knob", "toggle"):
            continue
        if t == "knob":
            ang = L.set_angle(ctl)
            place(knob(ctl["kind"], L.POINTERS.get(ctl["id"])), ctl["x"], ctl["y"], Z_CASS, rot_deg=ang)
        elif t == "button":
            small = ctl.get("small", False)
            z = Z_FRAME if small else Z_CASS
            lit = bool(ctl.get("set"))
            tint = "amber"
            if STATE:
                lit = STATE != "bare"
                if ctl.get("key") and STATE == "green":
                    tint = "green"
                    z -= 0.9
            elif ctl.get("key"):
                if ctl.get("selected"):
                    tint = "green"
                    z -= 0.9          # pressed in
            place(button(ctl["w"], ctl["h"], ctl["text"], lit=lit, small=small and ctl["id"] not in ("ab_a", "ab_b"), tint=tint),
                  ctl["x"], ctl["y"], z)
        elif t == "lamp":
            zl = Z_FRAME if ctl["y"] > L.CASS_Y[1] else Z_CASS
            place(jewel(ctl["color"], lit=(STATE in ("amber", "green")) if STATE else bool(ctl.get("set")), r=ctl["r"]), ctl["x"], ctl["y"], zl)
        elif t == "meter":
            face = os.path.join(BUILD, f"face_{ctl['kind']}.png")
            place(meter(ctl["kind"], ctl["w"], ctl["h"], face, with_glass=not STATE), ctl["x"], ctl["y"], Z_CASS)
            if not STATE:
                needle(ctl["kind"], ctl, L.METER_SET[ctl["id"]])
        elif t == "toggle":
            up = ctl["set"] == "up"
            if FLIP == ctl["id"]:
                up = not up
            place(toggle(up), ctl["x"], ctl["y"], Z_CASS)

    nameplate()
    ink = "tape_preset_ink.png"
    if STATE:   # the plugin writes the preset's name
        Image.new("L", Image.open(os.path.join(BUILD, ink)).size, 0).save(os.path.join(BUILD, "tape_blank_ink.png"))
        ink = "tape_blank_ink.png"
    tape(L.PRESET_TAPE, "tape_preset_alpha.png", "tape_preset_crepe.png", ink, lift=0.6)
    sect_ink = "tape_sect_ink.png"
    if STATE:   # the plugin writes the section names
        Image.new("L", Image.open(os.path.join(BUILD, sect_ink)).size, 0).save(os.path.join(BUILD, "tape_sect_blank.png"))
        sect_ink = "tape_sect_blank.png"
    tape(L.SECT_TAPE, "tape_sect_alpha.png", "tape_sect_crepe.png", sect_ink, lift=0.5, z=Z_CASS)
    place(latch(), L.SPACE_LATCH["x"], L.SPACE_LATCH["y"], Z_FRAME)

    if BORDER:
        # Only a small region (for a switch's other position), cropped, as a PNG.
        x, y, w, h = [float(v) for v in BORDER.split(",")]
        sc.render.use_border = True
        sc.render.use_crop_to_border = True
        sc.render.border_min_x, sc.render.border_max_x = x / L.W, (x + w) / L.W
        sc.render.border_min_y, sc.render.border_max_y = 1.0 - (y + h) / L.H, 1.0 - y / L.H
        sc.render.image_settings.file_format = "PNG"
        sc.render.image_settings.color_mode = "RGB"
        sc.render.filepath = os.path.join(BUILD, f"flip_{FLIP}.png")
        bpy.ops.render.render(write_still=True)
        print("wrote", sc.render.filepath)
        return
    if STATE in ("amber", "green"):
        # The lit patches: one crop per group of controls.
        os.makedirs(os.path.join(BUILD, "lit"), exist_ok=True)
        sc.render.use_border = True
        sc.render.use_crop_to_border = True
        sc.render.image_settings.file_format = "PNG"
        sc.render.image_settings.color_mode = "RGB"
        for name, ids in L.LIT_GROUPS.items():
            if (STATE == "green") != name.startswith("key_"):
                continue
            rects = [L.lit_rect(L.control(i)) for i in ids]
            x0, y0 = min(r[0] for r in rects), min(r[1] for r in rects)
            x1, y1 = max(r[0] + r[2] for r in rects), max(r[1] + r[3] for r in rects)
            sc.render.border_min_x, sc.render.border_max_x = x0 / L.W, x1 / L.W
            sc.render.border_min_y, sc.render.border_max_y = 1.0 - y1 / L.H, 1.0 - y0 / L.H
            sc.render.filepath = os.path.join(BUILD, "lit", f"{STATE}_{name}.png")
            bpy.ops.render.render(write_still=True)
            print("wrote", sc.render.filepath, (x0, y0, x1 - x0, y1 - y0))
        return
    sc.render.image_settings.file_format = "JPEG"
    sc.render.image_settings.quality = 92
    name = "panel_draft.jpg" if DRAFT else ("panel_preview.jpg" if PREVIEW else "panel.jpg")
    if STATE == "bare":
        name = "bare_" + name
    sc.render.filepath = os.path.join(BUILD, name)
    bpy.ops.render.render(write_still=True)
    print("wrote", sc.render.filepath)


def build_tray():
    """The SPACE tray that slides out from under the desk: dark hammertone, cream legends."""
    CANVAS[0], CANVAS[1] = L.TRAY_W, L.TRAY_H
    sc = reset()
    lights()
    camera(L.TRAY_W / 2, L.TRAY_H / 2, L.TRAY_W, L.TRAY_H, 1 if DRAFT else L.SCALE)
    box("floor", -50, -50, L.TRAY_W + 50, L.TRAY_H + 50, -4, -2, simple("floor", (0.004, 0.004, 0.004, 1), rough=0.9), bevel=0)
    tray = hammertone("tray", srgb((0.25, 0.262, 0.275)), 0.35,
                      [(os.path.join(BUILD, "tray_ink.png"), srgb((0.88, 0.85, 0.76)), 0.5),
                       (os.path.join(BUILD, "tray_shadow.png"), srgb((0.02, 0.02, 0.022)), 0.9)], wear=False)
    box("tray", 0, 0, L.TRAY_W, L.TRAY_H, 0, Z_FRAME, tray, bevel=1.2)
    for i, (x, y) in enumerate(L.TRAY_SCREWS):
        place(screw((i * 53) % 180), x, y, Z_FRAME)
    for ctl in L.TRAY_CONTROLS:
        if ctl["type"] == "knob" and not STATE:
            place(knob(ctl["kind"], L.POINTERS.get(ctl["id"])), ctl["x"], ctl["y"], Z_FRAME, rot_deg=L.set_angle(ctl))
        elif ctl["type"] == "button":
            lit = (STATE == "amber") if STATE else bool(ctl.get("set"))
            place(button(ctl["w"], ctl["h"], ctl["text"], lit=lit), ctl["x"], ctl["y"], Z_FRAME)
    nameplate(L.TRAY_PLATE, "tray_plate_ink.png")
    if STATE == "amber":   # the IN button's lit patch
        x, y, w, h = L.lit_rect(next(c for c in L.TRAY_CONTROLS if c["id"] == "sp_in"))
        sc.render.use_border = True
        sc.render.use_crop_to_border = True
        sc.render.border_min_x, sc.render.border_max_x = x / L.TRAY_W, (x + w) / L.TRAY_W
        sc.render.border_min_y, sc.render.border_max_y = 1.0 - (y + h) / L.TRAY_H, 1.0 - y / L.TRAY_H
        sc.render.image_settings.file_format = "PNG"
        sc.render.image_settings.color_mode = "RGB"
        os.makedirs(os.path.join(BUILD, "lit"), exist_ok=True)
        sc.render.filepath = os.path.join(BUILD, "lit", "amber_tray.png")
        bpy.ops.render.render(write_still=True)
        print("wrote", sc.render.filepath, (x, y, w, h))
        return
    sc.render.image_settings.file_format = "JPEG"
    sc.render.image_settings.quality = 92
    name = "tray_draft.jpg" if DRAFT else "tray.jpg"
    if STATE == "bare":
        name = "bare_" + name
    sc.render.filepath = os.path.join(BUILD, name)
    bpy.ops.render.render(write_still=True)
    print("wrote", sc.render.filepath)


def patch_regions(sc, out, regions):
    import numpy as np
    from PIL import ImageFilter
    base = Image.open(out).convert("RGB")
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGBA"
    sc.render.use_border = True
    sc.render.use_crop_to_border = False
    tmp = os.path.join(BUILD, "patch.png")
    for (x, y, w, h) in regions:
        sc.render.border_min_x = max(0.0, x / L.W)
        sc.render.border_max_x = min(1.0, (x + w) / L.W)
        sc.render.border_min_y = max(0.0, 1.0 - (y + h) / L.H)
        sc.render.border_max_y = min(1.0, 1.0 - y / L.H)
        sc.render.filepath = tmp
        bpy.ops.render.render(write_still=True)
        patch = Image.open(tmp).convert("RGBA")
        # Feathered edges, so the patch blends into the render around it.
        S = L.SCALE
        box = (int(x * S), int(y * S), int((x + w) * S), int((y + h) * S))
        m = Image.new("L", base.size, 0)
        ImageDraw.Draw(m).rectangle((box[0] + 10, box[1] + 10, box[2] - 10, box[3] - 10), fill=255)
        m = m.filter(ImageFilter.GaussianBlur(4))
        a = np.asarray(patch)[..., 3:4] > 0
        mm = (np.asarray(m)[..., None] / 255.0) * a
        blended = np.asarray(base) * (1 - mm) + np.asarray(patch.convert("RGB")) * mm
        base = Image.fromarray(np.clip(blended + 0.5, 0, 255).astype("uint8"), "RGB")
        print("patched", (x, y, w, h))
    base.save(out, quality=90)


# ---------------------------------------------------------------------------
def sprite_scene(box_w, box_h, catcher=False):
    sc = reset()
    lights()
    camera(0, 0, box_w, box_h, L.SCALE)
    sc.render.film_transparent = True
    sc.cycles.film_transparent_glass = True
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGBA"
    sc.render.image_settings.compression = 90
    if catcher:
        bm = bmesh.new()
        bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=box_w * 2)
        c = mesh_obj("catcher", bm, simple("catch", (0.5, 0.5, 0.5, 1)))
        c.is_shadow_catcher = True
    return sc


def render_to(path):
    sc = bpy.context.scene
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)


def knob_box(kind):
    """Sprite size in points (selectors have room for their own shadow)."""
    return 2 * L.KNOBS[kind] + (20 if kind == "sel" else 12)


def knob_strip(kind):
    box_pt = knob_box(kind)
    sc = sprite_scene(box_pt, box_pt, catcher=(kind == "sel"))
    if DRAFT:
        sc.cycles.samples = 16
    else:
        sc.cycles.samples = 96
    parts = knob(kind)
    root = parent_to(parts, "knob")
    frames = []
    tmp = os.path.join(BUILD, "frames_" + kind)
    os.makedirs(tmp, exist_ok=True)
    n = 9 if DRAFT else L.STRIP_FRAMES
    angles = [-L.SWEEP + 2 * L.SWEEP * i / (n - 1) for i in range(n)]
    if kind == "sel":
        angles = list(L.SEL_ANGLES)   # a selector only ever rests at these
    for i, ang in enumerate(angles):
        root.rotation_euler.z = math.radians(-ang)
        p = os.path.join(tmp, f"{i:03d}.png")
        render_to(p)
        frames.append(p)
    # Stack the frames into one vertical strip.
    ims = [Image.open(p) for p in frames]
    fw, fh = ims[0].size
    strip = Image.new("RGBA", (fw, fh * len(ims)))
    for i, im in enumerate(ims):
        strip.paste(im, (0, i * fh))
    os.makedirs(os.path.join(BUILD, "strips"), exist_ok=True)
    out = os.path.join(BUILD, "strips", f"knob_{kind}.png")
    strip.save(out, optimize=True)
    print("wrote", out, strip.size)


def sprites():
    for kind in ("large", "med", "small", "sel"):
        knob_strip(kind)
    small_sprites()


def small_sprites():
    for up in (True, False):
        # Rendered on the catcher (z = 0), so the shadow falls where the cassette face would be.
        sprite_scene(44, 44, catcher=True)
        place(toggle(up), 0, 0, 0)
        render_to(os.path.join(ASSETS, f"toggle_{'up' if up else 'down'}.png"))
    sprite_scene(44, 28)
    place(button(36, 20, "IN", lit=True), 0, 0, 0)
    render_to(os.path.join(ASSETS, "button_in_on.png"))
    for t in ("A", "B"):
        sprite_scene(34, 30)
        place(button(26, 22, t, lit=True, small=True), 0, 0, 0)
        render_to(os.path.join(ASSETS, f"button_{t.lower()}_on.png"))
    sprite_scene(22, 22)
    place(jewel("red", lit=True, r=6.0), 0, 0, 0)
    render_to(os.path.join(ASSETS, "lamp_red_on.png"))
    sprite_scene(24, 24)
    place(jewel("amber", lit=True, r=7.0), 0, 0, 0)
    render_to(os.path.join(ASSETS, "lamp_amber_on.png"))
    vu, mv = L.control("vu_l"), L.control("d_meter")
    for kind, (w, h) in (("vu", (vu["w"], vu["h"])), ("move", (mv["w"], mv["h"]))):
        sprite_scene(w + 4, h + 4)
        objs = meter(kind, w, h, os.path.join(BUILD, "face_move.png" if kind == "move" else "face_vu_l.png"), with_glass=True)
        # Only the glass itself: anything behind it would show through as part of the sprite
        # and hide the needle, which the window draws between the face and the glass.
        for o in objs:
            if not o.name.startswith("glass"):
                bpy.data.objects.remove(o, do_unlink=True)
        render_to(os.path.join(ASSETS, f"glass_{kind}.png"))


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "panel"
    if what == "panel":
        build_panel()
    elif what == "tray":
        build_tray()
    elif what == "glass":
        ASSETS = os.path.join(BUILD, "sprites")
        os.makedirs(ASSETS, exist_ok=True)
        small_sprites()
    elif what.startswith("knob_"):
        knob_strip(what[5:])

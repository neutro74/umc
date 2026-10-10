"""Renders the UMC art: blender -b -P render.py -- <textures dir> <out.png> <width> <samples> [logo|banner]"""
import math
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

argv = sys.argv[sys.argv.index("--") + 1:]
TEX, OUT = argv[0], argv[1]
SIZE = int(argv[2]) if len(argv) > 2 else 1600
SAMPLES = int(argv[3]) if len(argv) > 3 else 192
MODE = argv[4] if len(argv) > 4 else "logo"

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene


def material(name, color, rough=0.5, emission=None, strength=0.0, image=None, metallic=0.0, pillow=False):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nodes, links = m.node_tree.nodes, m.node_tree.links
    bsdf = nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*color, 1)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metallic
    if image:
        tex = nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.load(f"{TEX}/{image}")
        tex.interpolation = "Closest"  # keep the pixel art sharp
        tex.extension = "EXTEND"  # clamp, so UVs at exactly 1.0 don't wrap to the opposite row
        links.new(tex.outputs["Color"], bsdf.inputs["Emission Color" if emission == "image" else "Base Color"])
        if emission == "image":
            bsdf.inputs["Base Color"].default_value = (0.01, 0.01, 0.01, 1)
    elif emission:
        bsdf.inputs["Emission Color"].default_value = (*emission, 1)
    if pillow:
        # Rounded tiles generated from object position on a 1/8-unit lattice that matches the 16px
        # texture. Each axis only contributes along the surface, so tiles continue over the curved edges.
        def node(kind, **props):
            n_ = nodes.new(kind)
            for k_, v_ in props.items():
                setattr(n_, k_, v_)
            return n_

        def op(operation, a, b=None):
            n_ = node("ShaderNodeMath", operation=operation)
            for slot, value in ((0, a), (1, b)):
                if value is None:
                    continue
                if isinstance(value, (int, float)):
                    n_.inputs[slot].default_value = value
                else:
                    links.new(value, n_.inputs[slot])
            return n_.outputs[0]

        def smooth(value, width):
            r = node("ShaderNodeMapRange", interpolation_type="SMOOTHSTEP")
            r.inputs["From Max"].default_value = width
            links.new(value, r.inputs["Value"])
            return r.outputs["Result"]

        pos = node("ShaderNodeSeparateXYZ")
        links.new(node("ShaderNodeTexCoord").outputs["Object"], pos.inputs["Vector"])
        normal = node("ShaderNodeSeparateXYZ")
        links.new(node("ShaderNodeNewGeometry").outputs["True Normal"], normal.inputs["Vector"])
        height = None
        for axis in "XYZ":
            f = op("FRACT", op("DIVIDE", pos.outputs[axis], 0.125))
            d = op("MINIMUM", f, op("SUBTRACT", 1.0, f))  # distance to the nearest texel edge
            tile = op("ADD", op("MULTIPLY", smooth(d, 0.5), 0.6), op("MULTIPLY", smooth(d, 0.2), 0.4))
            # Weight 0 when this axis is the surface normal, 1 when it runs along the surface.
            along = smooth(op("SUBTRACT", 1.0, op("ABSOLUTE", normal.outputs[axis])), 0.3)
            h = op("ADD", op("MULTIPLY", op("SUBTRACT", tile, 1.0), along), 1.0)
            height = h if height is None else op("MULTIPLY", height, h)
        disp = node("ShaderNodeDisplacement")
        disp.inputs["Midlevel"].default_value = 1.0
        disp.inputs["Scale"].default_value = 0.016
        links.new(height, disp.inputs["Height"])
        links.new(disp.outputs["Displacement"], nodes["Material Output"].inputs["Displacement"])
        m.displacement_method = "DISPLACEMENT"
    bsdf.inputs["Emission Strength"].default_value = strength
    return m


def mesh_object(name, bm, mats, parent=None):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    for m in mats:
        me.materials.append(m)
    scene.collection.objects.link(obj)
    if parent:
        obj.parent = parent
    return obj


def bevel(obj, width, segments=3, harden=True):
    mod = obj.modifiers.new("bevel", "BEVEL")
    mod.width = width
    mod.segments = segments
    mod.limit_method = "ANGLE"
    mod.harden_normals = harden
    for p in obj.data.polygons:
        p.use_smooth = True
    return obj


def box(name, size, loc, mat, parent=None, round_=0.0, taper=None, taper_axis="y", rot=None):
    """Box centered at loc. taper=(a, b) shrinks the +taper_axis face, e.g. CRT backs or keycaps."""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        if taper and getattr(v.co, taper_axis) > 0:
            other = [a for a in "xyz" if a != taper_axis]
            setattr(v.co, other[0], getattr(v.co, other[0]) * taper[0])
            setattr(v.co, other[1], getattr(v.co, other[1]) * taper[1])
        v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    obj = mesh_object(name, bm, [mat], parent)
    obj.location = loc
    if rot:
        obj.rotation_euler = rot
    return bevel(obj, round_) if round_ else obj


def cylinder(name, r1, r2, depth, loc, rot, mat, parent=None, round_=0.0):
    """Cylinder or cone along its local Z axis."""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=48, radius1=r1, radius2=r2, depth=depth)
    obj = mesh_object(name, bm, [mat], parent)
    obj.location = loc
    obj.rotation_euler = rot
    if round_:
        bevel(obj, round_, 3)
    for p in obj.data.polygons:
        p.use_smooth = True
    return obj


def aim(obj, direction):
    obj.rotation_euler = Vector(direction).to_track_quat("-Z", "Y").to_euler()


# Grass block, 2 units wide, standing on the ground.
grass_top = material("grass_top", (1, 1, 1), 0.8, image="grass_top.png", pillow=True)
grass_side = material("grass_side", (1, 1, 1), 0.85, image="grass_side.png", pillow=True)
dirt = material("dirt", (1, 1, 1), 0.9, image="dirt.png", pillow=True)

bm = bmesh.new()
bmesh.ops.create_cube(bm, size=2.0)
bmesh.ops.translate(bm, verts=bm.verts, vec=(0, 0, 1))
uv = bm.loops.layers.uv.new()
for f in bm.faces:
    n = f.normal
    f.material_index = 0 if n.z > 0.5 else 2 if n.z < -0.5 else 1
    for loop in f.loops:
        x, y, z = loop.vert.co
        if abs(n.z) > 0.5:
            loop[uv].uv = ((x + 1) / 2, (y + 1) / 2)
        elif abs(n.x) > 0.5:
            loop[uv].uv = ((y + 1) / 2 * (1 if n.x > 0 else -1) % 1.0001, z / 2)
        else:
            loop[uv].uv = ((x + 1) / 2 * (-1 if n.y > 0 else 1) % 1.0001, z / 2)
block = bevel(mesh_object("block", bm, [grass_top, grass_side, dirt]), 0.1875, 24, harden=False)  # 1.5 texels keeps grooves off the bend
# Adaptive subdivision dices the block into pixel-sized micropolygons for the tile displacement,
# fine enough along the curved edges to follow the grooves without folding.
sub = block.modifiers.new("dense", "SUBSURF")
sub.subdivision_type = "SIMPLE"
sub.use_adaptive_subdivision = True
sub.adaptive_space = "PIXEL"
sub.adaptive_pixel_size = 1.0
scene.cycles.max_subdivisions = 14

# Retro computer, grouped under an empty so it can be placed and turned as one.
plastic = material("plastic", (0.62, 0.6, 0.55), 0.45)
plastic_dark = material("plastic_dark", (0.36, 0.35, 0.33), 0.5)
plastic_light = material("plastic_light", (0.7, 0.68, 0.62), 0.45)
recess = material("recess", (0.035, 0.035, 0.04), 0.4)
screen = material("screen", (0, 0, 0), 0.12, emission="image", strength=1.1, image="screen.png")
screen.node_tree.nodes["Principled BSDF"].inputs["Coat Weight"].default_value = 1.0
screen.node_tree.nodes["Principled BSDF"].inputs["Coat Roughness"].default_value = 0.08
led = material("led", (0.1, 0.9, 0.3), 0.3, emission=(0.2, 1.0, 0.35), strength=10)
rubber = material("rubber", (0.03, 0.03, 0.034), 0.45)
metal = material("metal", (0.75, 0.75, 0.77), 0.22, metallic=1.0)

pc = bpy.data.objects.new("computer", None)
scene.collection.objects.link(pc)
pc.location = (2.95, 0.35, 0)
pc.rotation_euler = (0, 0, math.radians(28))

# Retro CRT: rounded bezel, tapered tube behind it, low stand
box("stand_base", (0.95, 0.75, 0.08), (0, 0.15, 0.04), plastic_dark, pc, 0.035)
box("stand_neck", (0.42, 0.36, 0.2), (0, 0.15, 0.17), plastic_dark, pc, 0.02)
box("bezel", (1.6, 0.26, 1.28), (0, -0.12, 0.92), plastic, pc, 0.11)
box("crt_back", (1.5, 0.9, 1.18), (0, 0.45, 0.92), plastic, pc, 0.14, taper=(0.62, 0.6))
box("screen_edge", (1.3, 0.04, 0.98), (0, -0.255, 0.95), recess, pc, 0.012)
bm = bmesh.new()
uv = bm.loops.layers.uv.new()
corners = [(-0.6, -0.45), (0.6, -0.45), (0.6, 0.45), (-0.6, 0.45)]
face = bm.faces.new([bm.verts.new((x, -0.277, z + 0.95)) for x, z in corners])
for loop, (x, z) in zip(face.loops, corners):
    loop[uv].uv = (x / 1.2 + 0.5, z / 0.9 + 0.5)
mesh_object("screen", bm, [screen], pc)
# Power light set flush into the bezel
cylinder("led", 0.022, 0.022, 0.02, (0.66, -0.24, 0.38), (math.radians(90), 0, 0), led, pc)

# Keyboard: tilted base with staggered rows of tapered keycaps and a spacebar
kb = bpy.data.objects.new("keyboard", None)
kb.parent = pc
kb.location = (0, -0.85, 0)
kb.rotation_euler = (math.radians(-4), 0, 0)
scene.collection.objects.link(kb)
box("kb_base", (1.5, 0.56, 0.08), (0, 0, 0.05), plastic, kb, 0.035)
key = (0.088, 0.088, 0.05)
for row in range(4):
    offset = (0, 0.03, 0.06, 0.1)[row]
    for col in range(12):
        x = -0.62 + offset + col * 0.105
        if x > 0.62:
            continue
        mat = plastic_dark if (row, col) in {(0, 0), (1, 0), (2, 0), (0, 11)} else plastic_light
        box(f"key{row}_{col}", key, (x, 0.17 - row * 0.105, 0.11), mat, kb, 0.018, taper=(0.82, 0.82), taper_axis="z")
box("spacebar", (0.62, 0.088, 0.05), (0, -0.25, 0.11), plastic_light, kb, 0.018, taper=(0.95, 0.82), taper_axis="z")


def connector(name, at, direction):
    """Port, metal collar, rounded plug and tapered strain relief, pointing along `direction`."""
    d = Vector(direction).normalized()
    rot = d.to_track_quat("X", "Z").to_euler()
    box(f"{name}_port", (0.04, 0.3, 0.24), at, recess, None, 0.05, rot=rot)
    box(f"{name}_collar", (0.04, 0.22, 0.17), at + d * 0.035, metal, None, 0.025, rot=rot)
    box(f"{name}_body", (0.18, 0.2, 0.15), at + d * 0.14, rubber, None, 0.06, rot=rot)
    relief = d.to_track_quat("Z", "Y").to_euler()
    cylinder(f"{name}_relief", 0.055, 0.042, 0.12, at + d * 0.29, relief, rubber, None, 0.008)
    return at + d * 0.35


# Cable: from the block, down onto the ground, around into the back of the monitor
bpy.context.view_layer.update()
start = connector("block", Vector((1.0, 0.45, 0.45)), (1, 0, 0))
back = pc.matrix_world.to_3x3() @ Vector((0, 1, 0))
end = connector("pc", pc.matrix_world @ Vector((-0.25, 0.9, 0.75)), back)

curve = bpy.data.curves.new("cable", "CURVE")
curve.dimensions = "3D"
curve.bevel_depth = 0.042
curve.bevel_resolution = 8
curve.resolution_u = 48
spline = curve.splines.new("BEZIER")
# Drops into the gap in front, rests on the ground, then curves round to the monitor's back port.
path = [
    (start, Vector((1, 0, 0)), 0.22),
    (Vector((1.75, 0.0, 0.042)), Vector((0.55, 0.85, 0)), 0.3),
    (Vector((2.3, 1.05, 0.042)), Vector((0.75, 0.65, 0)), 0.3),
    (end, -back, 0.3),
]
spline.bezier_points.add(len(path) - 1)
for bp, (co, tangent, reach) in zip(spline.bezier_points, path):
    t = tangent.normalized() * reach
    bp.co = co
    bp.handle_left_type = bp.handle_right_type = "ALIGNED"
    bp.handle_left, bp.handle_right = co - t, co + t
cable = bpy.data.objects.new("cable", curve)
curve.materials.append(rubber)
scene.collection.objects.link(cable)

# Banner: 3D type standing on the ground, turned to face the isometric camera.
def text(body, font, size, extrude, bevel_, loc, mat, spacing=1.0):
    curve = bpy.data.curves.new(body, "FONT")
    curve.body = body
    curve.font = bpy.data.fonts.load(font)
    curve.size = size
    curve.extrude = extrude
    curve.bevel_depth = bevel_
    curve.bevel_resolution = 4
    curve.space_character = spacing
    obj = bpy.data.objects.new(body, curve)
    obj.location = loc
    obj.rotation_euler = (math.radians(90), 0, math.radians(45))
    obj.data.materials.append(mat)
    scene.collection.objects.link(obj)
    return obj


if MODE == "banner":
    mono = "/usr/share/fonts/jetbrains-mono-fonts"
    letters = material("letters", (0.95, 0.94, 0.9), 0.35)
    subtitle = material("subtitle", (0.92, 0.93, 0.86), 0.45)
    along = Vector((1, 1, 0)).normalized()     # reads left to right on screen
    toward = Vector((1, -1, 0)).normalized()   # toward the camera, lower on screen
    origin = Vector((3.6, 2.6, 0))
    text("UMC", f"{mono}/JetBrainsMono-ExtraBold.otf", 2.3, 0.12, 0.03, origin, letters)
    text("Universal Minecraft Connector", f"{mono}/JetBrainsMono-SemiBold.otf", 0.4, 0.045, 0.006,
         origin + toward * 1.1 + along * 0.05, subtitle)
    # Soft fill from the camera side so the letter faces aren't left in the sun's shade.
    fill = bpy.data.objects.new("fill", bpy.data.lights.new("fill", "AREA"))
    fill.data.energy = 1200
    fill.data.size = 8
    fill.location = origin + toward * 9 + Vector((0, 0, 5))
    aim(fill, origin + Vector((2, 2, 1)) - fill.location)
    scene.collection.objects.link(fill)

# Ground that only receives shadows, so the render has a transparent background.
bm = bmesh.new()
bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=30)
ground = mesh_object("ground", bm, [material("ground", (0.5, 0.5, 0.5), 0.9)])
ground.is_shadow_catcher = True

# Lighting: a soft key sun from the front-left, cool sky fill, warm rim from behind.

sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
sun.data.energy = 1.7
sun.data.angle = math.radians(12)
aim(sun, (0.35, 0.75, -1.0))
scene.collection.objects.link(sun)

rim = bpy.data.objects.new("rim", bpy.data.lights.new("rim", "AREA"))
rim.data.energy = 400
rim.data.size = 6
rim.data.color = (1.0, 0.85, 0.65)
rim.location = (-2, 7, 6)
aim(rim, Vector((1.4, 0, 0.8)) - rim.location)
scene.collection.objects.link(rim)

world = bpy.data.worlds.new("world")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.8, 0.85, 0.9, 1)
world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.3
scene.world = world

# True isometric orthographic camera.
target = Vector((1.45, 0.05, 0.85)) if MODE == "logo" else Vector((3.5, 1.35, 1.35))
cam = bpy.data.objects.new("camera", bpy.data.cameras.new("camera"))
cam.data.type = "ORTHO"
cam.data.ortho_scale = 6.1 if MODE == "logo" else 15.0
cam.location = target + Vector((1, -1, math.sqrt(2) * math.tan(math.radians(35.264)))).normalized() * 30
aim(cam, target - cam.location)
cam.data.clip_end = 100
scene.collection.objects.link(cam)
scene.camera = cam

scene.render.engine = "CYCLES"
scene.cycles.samples = SAMPLES
scene.cycles.use_denoising = True
scene.cycles.device = "CPU"
scene.render.film_transparent = True
scene.render.resolution_x = SIZE
scene.render.resolution_y = SIZE if MODE == "logo" else SIZE * 5 // 16
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGBA"
scene.view_settings.view_transform = "AgX"
scene.view_settings.look = "None"
scene.render.filepath = OUT
bpy.ops.render.render(write_still=True)

"""Moto's motorcycle and its rider: a stylized superbike with a superbike's published proportions (a wheelbase of 1,485
mm, a rake of 24 degrees and a trail of 98 mm, on a 120/70 ZR17 front tyre and a 200/55 ZR17 rear one), and a rigid rider
in the saddle, his hands on the bars and his feet on the pegs. GenerateAssets.py writes their meshes (Meshes/Bike) and
their texture (Textures/Bike/Livery.png); Author.py's bike part stands them on the grid.

The bike is five meshes, so that what turns on a motorcycle can turn: Body (the frame, the engine, the bodywork, the
swingarm and the exhaust), Fork (the forks, the clamps, the bars and the front mudguard: what the rider steers), FrontWheel
and RearWheel (each a tyre, a rim and its brake discs), and Rider. ENTITIES lists them as the bike part makes them, each
with where it stands in its parent:

- the bike's own frame (the Bike entity's): its origin on the ground midway between the tyres' contact patches, +Y up,
  its front along -Z (as the circuit's structures stand: Author.place turns -Z along the travel), +X to its right;
- Fork: its origin where the steering axis meets the ground, TRAIL ahead of the front tyre's contact patch, turned RAKE
  degrees about X, so that its +Y runs up the steering axis: turning the entity about its own Y steers;
- Front Wheel, under Fork: its origin on the front axle, turned back by the rake, so that its axes are the bike's while
  the fork is straight: turning it about its X rolls the wheel;
- Rear Wheel: its origin on the rear axle;
- Rider: its origin at his hips on the saddle (HIP), where he leans and tucks from.

The meshes draw with four materials (Materials/Bike, all with the livery texture), each mesh with those it uses, in this
order: Paint, glossy (the bodywork's livery, the rims, the rider's leathers and helmet), Dark (tyres, seat, engine,
chain, grips, gloves and boots), Metal (frame, forks, discs, exhaust) and Glass (the screen, the lights and the visor).
That is 16 primitives, so the bike costs 16 draws in each pass that sees all of it.

    python Projects/Moto/Tools/Bike.py

prints the proportions as check() measures them from the meshes made: the axles' distance, the steering axis' angle and
where it meets the ground, the contact patches, and the bike's and the rider's sizes.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Circuit  # noqa: E402  (the layout)
import GenerateAssets  # noqa: E402  (meshes and textures)
import Tour  # noqa: E402  (the camera tour)
from GenerateAssets import MeshBuilder, add, cross, dot, normalize, scale, subtract  # noqa: E402

# The published proportions (meters, degrees) and the tyres: a 17 inch rim (431.8 mm) with a 120/70 tyre in front (84 mm
# of sidewall) and a 200/55 at the rear (110 mm).
WHEELBASE = 1.485
RAKE = 24.0
TRAIL = 0.098
RIM_RADIUS = 0.2159
FRONT_RADIUS = RIM_RADIUS + 0.084
REAR_RADIUS = RIM_RADIUS + 0.110
FRONT_WIDTH = 0.120
REAR_WIDTH = 0.200
# The fork's offset: how far the front axle lies ahead of the steering axis, square to it. It follows from the rest:
# trail = (front radius x sin(rake) - offset) / cos(rake).
FORK_OFFSET = FRONT_RADIUS * math.sin(math.radians(RAKE)) - TRAIL * math.cos(math.radians(RAKE))

# Where things are in the bike's frame (x right, y up, z backwards: the front is -Z).
FRONT_AXLE = (0.0, FRONT_RADIUS, -WHEELBASE / 2.0)
REAR_AXLE = (0.0, REAR_RADIUS, WHEELBASE / 2.0)
# Where the steering axis meets the ground, and its direction up the axis (up and backwards by the rake).
STEERING_GROUND = (0.0, 0.0, -(WHEELBASE / 2.0 + TRAIL))
STEERING_AXIS = (0.0, math.cos(math.radians(RAKE)), math.sin(math.radians(RAKE)))
# The rider's hips on the saddle: his origin.
HIP = (0.0, 0.86, 0.30)

# The meshes' materials, in the order a mesh's primitives use them (Materials/Bike/<name>.smat).
MATERIALS = ("Paint", "Dark", "Metal", "Glass")
MESHES = ("Body", "Fork", "FrontWheel", "RearWheel", "Rider")

# --- The livery texture -----------------------------------------------------------------------------------------------
#
# Textures/Bike/Livery.png: cells of 64 pixels, 4 to a row. Most are a flat color (a face drawn in one samples its middle);
# Livery and Helmet are bands from top to bottom that the bodywork and the helmet map by height; Number is the race
# number on a white plate.

LIVERY_SIZE = 256
LIVERY_CELL = 64
SWATCHES = (
    ("Red", (0.78, 0.05, 0.04)), ("White", (0.92, 0.92, 0.9)), ("Black", (0.035, 0.035, 0.04)), ("Livery", None),
    ("Tyre", (0.045, 0.045, 0.05)), ("Trim", (0.09, 0.09, 0.1)), ("Engine", (0.16, 0.16, 0.17)), ("Silver", (0.8, 0.8, 0.82)),
    ("Gunmetal", (0.3, 0.31, 0.33)), ("Gold", (0.86, 0.62, 0.24)), ("Smoke", (0.025, 0.03, 0.045)), ("Lens", (0.82, 0.86, 0.92)),
    ("TailLight", (0.72, 0.03, 0.02)), ("Number", None), ("Helmet", None), ("Leathers", (0.12, 0.12, 0.13)),
)
CELL = {name: index for index, (name, _) in enumerate(SWATCHES)}
# The bands of the bodywork's livery and of the helmet, from the top: (where the band ends, 0 to 1 down the cell, color).
LIVERY_BANDS = ((0.6, "Red"), (0.68, "White"), (1.0, "Black"))
HELMET_BANDS = ((0.42, "White"), (0.55, "Red"), (0.62, "White"), (1.0, "Black"))
NUMBER = "23"


def cell_origin(cell):
    return (cell % 4) * LIVERY_CELL, (cell // 4) * LIVERY_CELL


def swatch(name):
    """The texture coordinate in the middle of a cell: a face drawn in its color."""
    x, y = cell_origin(CELL[name])
    return ((x + LIVERY_CELL / 2.0) / LIVERY_SIZE, (y + LIVERY_CELL / 2.0) / LIVERY_SIZE)


def cell_uv(name, u, v, inset=4.0):
    """A texture coordinate in a cell from u and v in 0..1 across it, kept inset pixels from its edges (smaller images
    blend neighboring cells at their borders)."""
    x, y = cell_origin(CELL[name])
    u, v = min(1.0, max(0.0, u)), min(1.0, max(0.0, v))
    return ((x + inset + u * (LIVERY_CELL - 2.0 * inset)) / LIVERY_SIZE, (y + inset + v * (LIVERY_CELL - 2.0 * inset)) / LIVERY_SIZE)


def livery_pixel(seed):
    """The livery's pixel(x, y) for GenerateAssets.write_png."""
    left, top = cell_origin(CELL["Number"])
    digits = [shape for k, digit in enumerate(NUMBER)
              for shape in GenerateAssets.segment_rectangles(digit, left + 12 + k * 22, top + 14, 16, 36, 5)]
    grain = GenerateAssets.fractal(seed + 50, 2, 8)
    colors = dict(SWATCHES)

    def banded(bands, v):
        return next(colors[name] for end, name in bands if v < end)

    def pixel(x, y):
        name = SWATCHES[(y // LIVERY_CELL) * 4 + x // LIVERY_CELL][0]
        _, cell_y = cell_origin(CELL[name])
        # Down the cell's inset part, as cell_uv maps it.
        v = min(0.999, max(0.0, (y - cell_y - 4.0 + 0.5) / (LIVERY_CELL - 8.0)))
        if name == "Livery":
            color = banded(LIVERY_BANDS, v)
        elif name == "Helmet":
            color = banded(HELMET_BANDS, v)
        elif name == "Number":
            inside = any(l <= x + 0.5 < r and t <= y + 0.5 < b for l, t, r, b in digits)
            color = colors["Black"] if inside else colors["White"]
        else:
            color = colors[name]
        # The rubber and the leathers have a little grain; paint, metal and glass none.
        amount = 0.12 if name in ("Tyre", "Trim", "Leathers", "Engine") else 0.0
        shade = 1.0 + amount * (grain((x + 0.5) / LIVERY_SIZE, (y + 0.5) / LIVERY_SIZE) - 0.5) * 2.0
        return [GenerateAssets.quantize(min(1.0, value * shade)) for value in color]

    return pixel


# --- Shapes ----------------------------------------------------------------------------------------------------------------

def P(x, y, f):
    """A point of the bike's frame from x (right), y (up) and f (forward: the frame's -Z)."""
    return (x, y, -f)


class Parts:
    """A mesh's parts, a MeshBuilder per material (MATERIALS), written in that order."""

    def __init__(self):
        self.builders = {name: MeshBuilder() for name in MATERIALS}

    def __getitem__(self, material):
        return self.builders[material]

    def transformed(self, function):
        """The same parts with every position and normal mapped: function(point, is_direction) -> point."""
        moved = Parts()
        for name, builder in self.builders.items():
            target = moved.builders[name]
            target.positions = [tuple(round(v, 4) + 0.0 for v in function(p, False)) for p in builder.positions]
            target.normals = [tuple(round(v, 6) + 0.0 for v in normalize(function(n, True))) for n in builder.normals]
            target.uvs = list(builder.uvs)
            target.indices = list(builder.indices)
        return moved

    def gltf(self):
        """(parts, materials) for GenerateAssets.write_gltf: the materials the mesh uses, in MATERIALS' order."""
        used = [name for name in MATERIALS if self.builders[name].indices]
        return [(self.builders[name], index) for index, name in enumerate(used)], tuple(used)


def grid(mesh, rings, normals, uvs, closed=True):
    """Quads between consecutive rings of points (each ring closed round when closed), with the normals and texture
    coordinates given per point; each quad is wound to face where its normals point."""
    first = len(mesh.positions)
    count = len(rings[0])
    for ring, ring_normals, ring_uvs in zip(rings, normals, uvs):
        for point, normal, uv in zip(ring, ring_normals, ring_uvs):
            mesh.vertex(point, normal, uv)
    columns = count if closed else count - 1
    for i in range(len(rings) - 1):
        for j in range(columns):
            k = (j + 1) % count
            a, b, c, d = (first + i * count + j, first + i * count + k, first + (i + 1) * count + k, first + (i + 1) * count + j)
            up = normalize(add(add(mesh.normals[a], mesh.normals[b]), add(mesh.normals[c], mesh.normals[d])))
            mesh.quad(a, b, c, d, up)


def grid_normals(rings, centers, closed=True):
    """Smooth normals of a grid of rings from the differences between neighbors, each turned away from its ring's
    center."""
    normals = []
    count = len(rings[0])
    for i, ring in enumerate(rings):
        before, after = rings[max(0, i - 1)], rings[min(len(rings) - 1, i + 1)]
        row = []
        for j, point in enumerate(ring):
            if closed:
                left, right = ring[(j - 1) % count], ring[(j + 1) % count]
            else:
                left, right = ring[max(0, j - 1)], ring[min(count - 1, j + 1)]
            normal = cross(subtract(after[j], before[j]), subtract(right, left))
            if dot(normal, normal) < 1e-14:
                normal = subtract(point, centers[i])
            normal = normalize(normal)
            if dot(normal, subtract(point, centers[i])) < 0.0:
                normal = scale(normal, -1.0)
            row.append(normal)
        normals.append(row)
    return normals


def ellipsoid(mesh, center, radii, uv, rows=8, columns=14):
    """A smooth ellipsoid; uv(point) gives each vertex's texture coordinate."""
    rings, normals = [], []
    for i in range(rows + 1):
        polar = math.pi * i / rows
        ring, ring_normals = [], []
        for j in range(columns):
            azimuth = 2.0 * math.pi * j / columns
            unit = (math.sin(polar) * math.cos(azimuth), math.cos(polar), math.sin(polar) * math.sin(azimuth))
            ring.append(tuple(center[k] + unit[k] * radii[k] for k in range(3)))
            ring_normals.append(normalize(tuple(unit[k] / radii[k] for k in range(3))))
        rings.append(ring)
        normals.append(ring_normals)
    grid(mesh, rings, normals, [[uv(point) for point in ring] for ring in rings])


def frame_at(tangent):
    """Two unit directions square to a tangent and to each other: the first as near the bike's +X as can be."""
    side = (1.0, 0.0, 0.0)
    if abs(dot(side, tangent)) > 0.9:
        side = (0.0, 1.0, 0.0)
    side = normalize(subtract(side, scale(tangent, dot(side, tangent))))
    return side, normalize(cross(tangent, side))


def tube(mesh, points, radii, uv, sides=12, caps=True):
    """A smooth tube through points with an elliptical section at each ((across, up) radii, the first along the side
    frame_at gives), capped at both ends when asked."""
    rings, normals, centers = [], [], []
    for index, point in enumerate(points):
        tangent = normalize(subtract(points[min(index + 1, len(points) - 1)], points[max(index - 1, 0)]))
        side, other = frame_at(tangent)
        rx, ry = radii[index]
        ring, ring_normals = [], []
        for j in range(sides):
            angle = 2.0 * math.pi * j / sides
            c, s = math.cos(angle), math.sin(angle)
            ring.append(add(point, add(scale(side, c * rx), scale(other, s * ry))))
            ring_normals.append(normalize(add(scale(side, c / rx), scale(other, s / ry))))
        rings.append(ring)
        normals.append(ring_normals)
        centers.append(point)
    grid(mesh, rings, normals, [[uv(p) for p in ring] for ring in rings])
    if caps:
        for ring, center, direction in ((rings[0], points[0], subtract(points[0], points[1])),
                                         (rings[-1], points[-1], subtract(points[-1], points[-2]))):
            texture = uv(center)
            mesh.face(ring, normalize(direction), [texture] * len(ring))


def beam(mesh, a, b, width, height, texture, up=(0.0, 1.0, 0.0)):
    """A box from a to b, width across and height square to it (towards up), flat shaded in one texture coordinate."""
    axis = subtract(b, a)
    side = normalize(cross(axis, up))
    if dot(side, side) < 0.5:
        side = (1.0, 0.0, 0.0)
    rise = normalize(cross(side, axis))
    hw, hh = width / 2.0, height / 2.0
    corners = {}
    for end, base in ((0, a), (1, b)):
        for sx in (-1, 1):
            for sy in (-1, 1):
                corners[(end, sx, sy)] = add(base, add(scale(side, sx * hw), scale(rise, sy * hh)))
    direction = normalize(axis)
    faces = (
        (side, [(0, 1, -1), (1, 1, -1), (1, 1, 1), (0, 1, 1)]),
        (scale(side, -1.0), [(0, -1, -1), (0, -1, 1), (1, -1, 1), (1, -1, -1)]),
        (rise, [(0, -1, 1), (0, 1, 1), (1, 1, 1), (1, -1, 1)]),
        (scale(rise, -1.0), [(0, -1, -1), (1, -1, -1), (1, 1, -1), (0, 1, -1)]),
        (scale(direction, -1.0), [(0, -1, -1), (0, 1, -1), (0, 1, 1), (0, -1, 1)]),
        (direction, [(1, -1, -1), (1, -1, 1), (1, 1, 1), (1, 1, -1)]),
    )
    for normal, keys in faces:
        mesh.face([corners[key] for key in keys], normal, [texture] * 4)


def revolve(mesh, profile, center, texture, segments=40):
    """A smooth surface of revolution about the X axis: the profile's (x, r) points swept round, each normal square to
    the profile and turned away from its center (x, r); texture(x, r) gives each ring's texture coordinate."""
    rings, normals, uvs = [], [], []
    for index, (x, r) in enumerate(profile):
        before, after = profile[max(0, index - 1)], profile[min(len(profile) - 1, index + 1)]
        tx, tr = after[0] - before[0], after[1] - before[1]
        nx, nr = -tr, tx
        if nx * (x - center[0]) + nr * (r - center[1]) < 0.0:
            nx, nr = -nx, -nr
        length = math.hypot(nx, nr) or 1.0
        nx, nr = nx / length, nr / length
        ring, ring_normals = [], []
        for j in range(segments):
            angle = 2.0 * math.pi * j / segments
            ring.append((x, r * math.cos(angle), r * math.sin(angle)))
            ring_normals.append((nx, nr * math.cos(angle), nr * math.sin(angle)))
        rings.append(ring)
        normals.append(ring_normals)
        uvs.append([texture(x, r)] * segments)
    grid(mesh, rings, normals, uvs)


def annulus(mesh, x, inner, outer, facing, texture, segments=40):
    """A flat ring square to the X axis at x, facing +X (facing 1) or -X (-1)."""
    normal = (float(facing), 0.0, 0.0)
    for j in range(segments):
        a0, a1 = 2.0 * math.pi * j / segments, 2.0 * math.pi * (j + 1) / segments
        points = [(x, inner * math.cos(a0), inner * math.sin(a0)), (x, outer * math.cos(a0), outer * math.sin(a0)),
                  (x, outer * math.cos(a1), outer * math.sin(a1)), (x, inner * math.cos(a1), inner * math.sin(a1))]
        mesh.face(points, normal, [texture] * 4)


def disc(mesh, x0, x1, inner, outer, texture, segments=40):
    """A flat disc (a ring when inner is above 0) from x0 to x1: both faces and its rims."""
    annulus(mesh, x0, inner, outer, -1, texture, segments)
    annulus(mesh, x1, inner, outer, 1, texture, segments)
    revolve(mesh, [(x0, outer), (x1, outer)], ((x0 + x1) / 2.0, 0.0), lambda x, r: texture, segments)
    if inner > 0.0:
        revolve(mesh, [(x0, inner), (x1, inner)], ((x0 + x1) / 2.0, 2.0 * inner), lambda x, r: texture, segments)


def loft(mesh, stations, uv, sides=24, power=2.6, caps=True):
    """Bodywork: a smooth surface through cross-sections square to the bike's length, each (f, bottom, top, half width,
    narrowing): a rounded rectangle (a superellipse of the power) from bottom to top, its top narrower by the narrowing
    (a fraction of its half width). uv(point, height) gives each vertex's texture coordinate from its height in its
    section (0 at the bottom, 1 at the top)."""
    rings, centers, heights = [], [], []
    for f, bottom, top, half, narrowing in stations:
        middle, reach = (bottom + top) / 2.0, (top - bottom) / 2.0
        ring, ring_heights = [], []
        for j in range(sides):
            angle = 2.0 * math.pi * j / sides
            c, s = math.cos(angle), math.sin(angle)
            sx = math.copysign(abs(c) ** (2.0 / power), c)
            sy = math.copysign(abs(s) ** (2.0 / power), s)
            width = half * (1.0 - narrowing * max(0.0, sy))
            ring.append(P(sx * width, middle + sy * reach, f))
            ring_heights.append((sy + 1.0) / 2.0)
        rings.append(ring)
        centers.append(P(0.0, middle, f))
        heights.append(ring_heights)
    normals = grid_normals(rings, centers)
    grid(mesh, rings, normals, [[uv(point, height) for point, height in zip(ring, ring_heights)]
                                 for ring, ring_heights in zip(rings, heights)])
    if caps:
        for ring, center, direction, ring_heights in ((rings[0], centers[0], (0.0, 0.0, -1.0), heights[0]),
                                                      (rings[-1], centers[-1], (0.0, 0.0, 1.0), heights[-1])):
            if stations[0][0] < stations[-1][0]:
                direction = scale(direction, -1.0)
            mesh.face(ring, direction, [uv(center, 0.5)] * len(ring))


def banded_uv(cell):
    """Texture coordinates down a banded cell by height (1 at the top)."""
    return lambda point, height: cell_uv(cell, 0.5, 1.0 - height)


def flat(name):
    texture = swatch(name)
    return lambda *arguments: texture


# --- The bike ----------------------------------------------------------------------------------------------------------------

def build_body():
    parts = Parts()
    paint, dark, metal, glass = (parts[name] for name in MATERIALS)
    # The bodywork in one piece, nose to tail: the nose over the front tyre, the side panels behind it down to the belly,
    # open behind their trailing edge where the tank sits over the engine, the seat and the tail rising behind it. Each
    # station: f, bottom, top, half width, narrowing at the top.
    loft(paint, (
        (1.07, 0.735, 0.775, 0.03, 0.2),
        (1.0, 0.68, 0.85, 0.11, 0.3),
        (0.88, 0.645, 0.92, 0.19, 0.35),
        (0.72, 0.64, 0.965, 0.24, 0.4),
        (0.57, 0.55, 0.985, 0.27, 0.4),
        (0.45, 0.27, 0.99, 0.28, 0.4),
        (0.31, 0.23, 1.0, 0.265, 0.4),
        (0.2, 0.62, 1.02, 0.225, 0.35),
        (0.02, 0.69, 0.985, 0.2, 0.35),
        (-0.12, 0.73, 0.89, 0.165, 0.2),
        (-0.42, 0.75, 0.875, 0.14, 0.3),
        (-0.6, 0.8, 0.97, 0.11, 0.5),
        (-0.78, 0.88, 1.05, 0.08, 0.55),
        (-0.9, 0.94, 1.075, 0.035, 0.55),
    ), banded_uv("Livery"))
    # The belly pan under the engine, black.
    loft(paint, ((0.44, 0.2, 0.34, 0.19, 0.0), (0.3, 0.14, 0.33, 0.2, 0.0), (0.0, 0.16, 0.33, 0.17, 0.0),
                 (-0.12, 0.24, 0.34, 0.12, 0.0)), flat("Black"))
    # The seat, a pad on the bodywork where the rider sits.
    loft(dark, ((-0.1, 0.82, 0.885, 0.15, 0.25), (-0.3, 0.82, 0.875, 0.145, 0.25), (-0.52, 0.82, 0.872, 0.13, 0.25)),
         flat("Trim"))
    # The engine under the tank, and its cylinders leaning forward.
    beam(dark, P(0.0, 0.42, 0.40), P(0.0, 0.42, -0.14), 0.34, 0.42, swatch("Engine"))
    beam(dark, P(0.0, 0.60, 0.32), P(0.0, 0.72, 0.12), 0.30, 0.20, swatch("Engine"))
    # The frame's spars from the steering head to the swingarm's pivot, and the swingarm to the rear axle.
    for x in (-1.0, 1.0):
        beam(metal, P(x * 0.155, 0.86, 0.44), P(x * 0.175, 0.47, -0.15), 0.05, 0.11, swatch("Silver"))
        beam(metal, P(x * 0.135, 0.43, -0.14), P(x * 0.135, REAR_RADIUS, -WHEELBASE / 2.0 - 0.03), 0.04, 0.09,
             swatch("Gunmetal"))
        # The footpegs.
        beam(metal, P(x * 0.15, 0.37, -0.23), P(x * 0.26, 0.37, -0.23), 0.025, 0.025, swatch("Silver"), (0.0, 0.0, 1.0))
    beam(metal, P(-0.135, 0.40, -0.45), P(0.135, 0.40, -0.45), 0.03, 0.05, swatch("Gunmetal"), (0.0, 0.0, 1.0))
    # The chain, on the left.
    beam(dark, P(-0.105, 0.41, -0.06), P(-0.105, REAR_RADIUS + 0.05, -WHEELBASE / 2.0), 0.02, 0.025, swatch("Trim"))
    # The exhaust: a header under the engine, the silencer up the right.
    tube(metal, [P(0.0, 0.2, 0.35), P(0.09, 0.2, 0.05), P(0.165, 0.36, -0.36)], [(0.032, 0.032)] * 3, flat("Silver"), 10, False)
    tube(metal, [P(0.17, 0.4, -0.36), P(0.18, 0.47, -0.6), P(0.185, 0.54, -0.82)], [(0.042, 0.042), (0.05, 0.05),
                                                                                       (0.048, 0.048)], flat("Silver"))
    # The screen over the fairing's top, smoked.
    # Its sections from the fairing's top up and back: f, the height on the middle, half its width.
    screen = ((0.86, 0.905, 0.17), (0.74, 1.0, 0.165), (0.62, 1.07, 0.14), (0.55, 1.095, 0.12))
    for thickness, facing in ((0.0, 1.0), (-0.006, -1.0)):
        rings = [[P(half * math.sin(t), y + thickness - 0.05 * (1.0 - math.cos(t)), f) for t in
                  (-1.2, -0.9, -0.6, -0.3, 0.0, 0.3, 0.6, 0.9, 1.2)] for f, y, half in screen]
        centers = [P(0.0, y - 1.0 * facing, f) for f, y, half in screen]
        grid(glass, rings, grid_normals(rings, centers, closed=False), [[swatch("Smoke")] * 9 for _ in rings], closed=False)
    # The headlights in the nose (a racing bike has no mirrors), and the tail light.
    for x in (-1.0, 1.0):
        beam(glass, P(x * 0.06, 0.765, 0.94), P(x * 0.075, 0.745, 1.02), 0.075, 0.04, swatch("Lens"))
    beam(glass, P(0.0, 0.985, -0.86), P(0.0, 0.98, -0.905), 0.06, 0.035, swatch("TailLight"))
    # The race number on both sides of the fairing, read from either side.
    for x, flip in ((1.0, False), (-1.0, True)):
        corners = [P(x * 0.284, 0.45, 0.3), P(x * 0.284, 0.45, 0.46), P(x * 0.284, 0.59, 0.46), P(x * 0.284, 0.59, 0.3)]
        uvs = [cell_uv("Number", 0.0, 1.0), cell_uv("Number", 1.0, 1.0), cell_uv("Number", 1.0, 0.0), cell_uv("Number", 0.0, 0.0)]
        if flip:
            uvs = [uvs[1], uvs[0], uvs[3], uvs[2]]
        paint.face(corners, (x, 0.0, 0.0), uvs)
    return parts


def axis_point(height, ahead=0.0):
    """The point of the steering axis at a height over the ground, moved ahead square to it by ahead (meters)."""
    rake = math.radians(RAKE)
    along = (height - ahead * math.sin(rake)) / math.cos(rake)
    forward = (0.0, math.sin(rake), -math.cos(rake))
    return add(add(STEERING_GROUND, scale(STEERING_AXIS, along)), scale(forward, ahead))


def build_fork():
    """The steered parts, in the bike's frame (fork_local moves them into the fork's)."""
    parts = Parts()
    paint, dark, metal = parts["Paint"], parts["Dark"], parts["Metal"]
    bottom = axis_point(FRONT_RADIUS - 0.03, FORK_OFFSET)
    split = axis_point(FRONT_RADIUS + 0.24, FORK_OFFSET)
    top = axis_point(0.95, FORK_OFFSET)
    for x in (-0.105, 0.105):
        # Upside down: the gold sliders below, the outer tubes above, the axle's lug at the bottom.
        tube(metal, [add(bottom, (x, 0.0, 0.0)), add(split, (x, 0.0, 0.0))], [(0.027, 0.027)] * 2, flat("Gold"), 10)
        tube(metal, [add(split, (x, 0.0, 0.0)), add(top, (x, 0.0, 0.0))], [(0.034, 0.034)] * 2, flat("Gunmetal"), 10)
        beam(metal, add(axis_point(FRONT_RADIUS - 0.05, FORK_OFFSET), (x, 0.0, 0.0)),
             add(axis_point(FRONT_RADIUS + 0.06, FORK_OFFSET), (x, 0.0, 0.0)), 0.05, 0.06, swatch("Gunmetal"))
        # The caliper behind the leg, over the disc.
        caliper = add(FRONT_AXLE, (math.copysign(0.085, x), 0.11, 0.09))
        beam(metal, add(caliper, (0.0, 0.05, -0.03)), add(caliper, (0.0, -0.05, 0.03)), 0.035, 0.05, swatch("Gold"))
    # The clamps, and the clip-on bars from the top clamp out to the grips.
    for height in (0.73, 0.93):
        middle = axis_point(height, FORK_OFFSET / 2.0)
        beam(metal, add(middle, (-0.15, 0.0, 0.0)), add(middle, (0.15, 0.0, 0.0)), 0.05, 0.09, swatch("Gunmetal"),
             STEERING_AXIS)
    clamp = axis_point(0.89, FORK_OFFSET)
    for x in (-1.0, 1.0):
        start = add(clamp, (x * 0.105, 0.0, 0.0))
        end = P(x * 0.25, 0.875, 0.42)
        tube(metal, [start, end], [(0.014, 0.014)] * 2, flat("Gunmetal"), 8)
        tube(dark, [end, P(x * 0.34, 0.868, 0.405)], [(0.02, 0.02)] * 2, flat("Trim"), 10)
    # The front mudguard over the tyre.
    rings = []
    for angle in range(-35, 66, 10):
        a = math.radians(angle)
        r = FRONT_RADIUS + 0.025
        center = add(FRONT_AXLE, (0.0, r * math.cos(a), -r * math.sin(a)))
        rings.append([add(center, (x, 0.012 * math.cos(a) * (1.0 - (x / 0.075) ** 2), -0.012 * math.sin(a) * (1.0 - (x / 0.075) ** 2)))
                      for x in (-0.075, -0.05, -0.025, 0.0, 0.025, 0.05, 0.075)])
    for side in (1.0, -1.0):
        layer = [[add(point, scale(subtract(point, FRONT_AXLE), 0.0 if side > 0 else -0.012)) for point in ring] for ring in rings]
        # The outside faces away from the axle, the inside towards it.
        centers = [FRONT_AXLE if side > 0 else add(ring[3], subtract(ring[3], FRONT_AXLE)) for ring in layer]
        grid(paint, layer, grid_normals(layer, centers, closed=False), [[swatch("Red")] * 7 for _ in layer], closed=False)
    return parts


def build_wheel(radius, width, front):
    """A wheel round its axle (X): the tyre, the rim with five pairs of spokes, the hub and the discs."""
    parts = Parts()
    paint, dark, metal = parts["Paint"], parts["Dark"], parts["Metal"]
    sidewall = radius - RIM_RADIUS
    half = width / 2.0
    # The tyre: its section rounded, widest at mid height, its tread a curve across.
    middle = RIM_RADIUS + sidewall * 0.45
    profile = []
    for step in range(-9, 10):
        angle = math.radians(step * 15.0)
        x = half * math.copysign(abs(math.sin(angle)) ** 0.8, math.sin(angle))
        reach = (radius - middle) if abs(angle) <= math.pi / 2.0 else (middle - RIM_RADIUS + 0.004)
        profile.append((x, middle + reach * math.cos(angle)))
    revolve(dark, profile, (0.0, middle), lambda x, r: swatch("Tyre"), 48)
    # The rim: its flanges, its barrel, and the spokes to the hub.
    rim = half * 0.8
    for side in (-1.0, 1.0):
        annulus(paint, side * rim, RIM_RADIUS - 0.022, RIM_RADIUS + 0.008, side, swatch("Black"), 48)
    revolve(paint, [(-rim, RIM_RADIUS - 0.022), (rim, RIM_RADIUS - 0.022)], (0.0, RIM_RADIUS), lambda x, r: swatch("Black"), 48)
    for k in range(5):
        angle = 2.0 * math.pi * k / 5.0
        for twist in (-0.16, 0.16):
            inner = (0.0, 0.05 * math.cos(angle), 0.05 * math.sin(angle))
            outer = (0.0, (RIM_RADIUS - 0.02) * math.cos(angle + twist), (RIM_RADIUS - 0.02) * math.sin(angle + twist))
            beam(paint, inner, outer, 0.022, 0.03, swatch("Black"), (1.0, 0.0, 0.0))
    disc(metal, -half * 0.6, half * 0.6, 0.0, 0.055, swatch("Gunmetal"), 20)
    if front:
        for side in (-1.0, 1.0):
            disc(metal, side * 0.076 - 0.0025, side * 0.076 + 0.0025, 0.105, 0.165, swatch("Silver"), 48)
    else:
        # The disc on the right, the sprocket on the left where the chain runs.
        disc(metal, 0.077, 0.082, 0.075, 0.11, swatch("Silver"), 40)
        disc(dark, -0.091, -0.085, 0.05, 0.1, swatch("Trim"), 40)
    return parts


# --- The rider ---------------------------------------------------------------------------------------------------------------

def build_rider():
    """The rider in the bike's frame (rider_local moves him into his own)."""
    parts = Parts()
    paint, dark, glass = parts["Paint"], parts["Dark"], parts["Glass"]
    white, leathers = flat("White"), flat("Leathers")
    ellipsoid(paint, P(0.0, 0.90, -0.28), (0.17, 0.11, 0.15), leathers)
    tube(paint, [P(0.0, 0.92, -0.27), P(0.0, 1.06, -0.15), P(0.0, 1.17, -0.01)], [(0.165, 0.12), (0.185, 0.12), (0.2, 0.11)], white)
    ellipsoid(paint, P(0.0, 1.18, 0.02), (0.21, 0.085, 0.11), white)
    # The hump on his back.
    ellipsoid(paint, P(0.0, 1.2, -0.12), (0.085, 0.05, 0.1), white)
    tube(paint, [P(0.0, 1.22, 0.06), P(0.0, 1.3, 0.12)], [(0.05, 0.05)] * 2, leathers, 10)
    # The helmet, its colors by height, and its visor.
    helmet_center, helmet_radii = P(0.0, 1.36, 0.16), (0.135, 0.14, 0.165)
    ellipsoid(paint, helmet_center, helmet_radii,
              lambda point: cell_uv("Helmet", 0.5, 0.5 - (point[1] - helmet_center[1]) / (2.0 * helmet_radii[1])), 10, 18)
    ellipsoid(glass, P(0.0, 1.366, 0.255), (0.108, 0.055, 0.07), flat("Smoke"))
    for x in (-1.0, 1.0):
        shoulder, elbow, wrist = P(x * 0.2, 1.18, 0.04), P(x * 0.27, 1.02, 0.22), P(x * 0.29, 0.925, 0.37)
        tube(paint, [shoulder, elbow], [(0.062, 0.062), (0.054, 0.054)], white, 12)
        ellipsoid(paint, elbow, (0.055, 0.055, 0.055), white, 6, 10)
        tube(paint, [elbow, wrist], [(0.052, 0.052), (0.042, 0.042)], white, 12)
        ellipsoid(dark, P(x * 0.3, 0.9, 0.41), (0.045, 0.042, 0.068), flat("Trim"), 6, 10)
        hip, knee, ankle = P(x * 0.12, 0.9, -0.27), P(x * 0.255, 0.7, 0.03), P(x * 0.215, 0.42, -0.16)
        tube(paint, [hip, knee], [(0.088, 0.088), (0.066, 0.066)], leathers, 12)
        ellipsoid(paint, knee, (0.066, 0.066, 0.066), leathers, 6, 10)
        # The knee slider.
        beam(dark, add(knee, (x * 0.055, -0.02, -0.03)), add(knee, (x * 0.055, 0.02, 0.05)), 0.02, 0.07, swatch("Trim"),
             (x, 0.0, 0.0))
        tube(paint, [knee, ankle], [(0.062, 0.062), (0.048, 0.048)], leathers, 12)
        ellipsoid(dark, P(x * 0.205, 0.37, -0.13), (0.05, 0.065, 0.135), flat("Trim"), 6, 10)
    return parts


# --- Frames ----------------------------------------------------------------------------------------------------------------

def rotate_x(point, degrees):
    a = math.radians(degrees)
    c, s = math.cos(a), math.sin(a)
    return (point[0], point[1] * c - point[2] * s, point[1] * s + point[2] * c)


def fork_local(point, direction):
    """A point (or direction) of the bike's frame in the fork's: from the steering axis' foot, turned back by the rake."""
    return rotate_x(point if direction else subtract(point, STEERING_GROUND), -RAKE)


def rider_local(point, direction):
    return point if direction else subtract(point, HIP)


# The bike part's entities: (name, mesh, parent, translation, rotation about X in degrees), each in its parent's frame
# (the Bike entity's is the bike's).
ENTITIES = (
    ("Body", "Body", "Bike", (0.0, 0.0, 0.0), 0.0),
    ("Fork", "Fork", "Bike", STEERING_GROUND, RAKE),
    ("Front Wheel", "FrontWheel", "Fork", fork_local(FRONT_AXLE, False), -RAKE),
    ("Rear Wheel", "RearWheel", "Bike", REAR_AXLE, 0.0),
    ("Rider", "Rider", "Bike", HIP, 0.0),
)


def meshes():
    """The bike's and the rider's meshes by name (Meshes/Bike/<name>.gltf), each in its entity's frame."""
    if not _MESHES:
        _MESHES.update(_build_meshes())
    return _MESHES


_MESHES = {}


def _build_meshes():
    return {
        "Body": build_body(),
        "Fork": build_fork().transformed(fork_local),
        "FrontWheel": build_wheel(FRONT_RADIUS, FRONT_WIDTH, True),
        "RearWheel": build_wheel(REAR_RADIUS, REAR_WIDTH, False),
        "Rider": build_rider().transformed(rider_local),
    }


def generate(assets, seed, log=print):
    """Writes the meshes (Meshes/Bike) and the livery (Textures/Bike/Livery.png)."""
    for name, parts in meshes().items():
        gltf_parts, materials = parts.gltf()
        GenerateAssets.write_gltf(assets, f"Bike/{name}", gltf_parts, materials, log)
    GenerateAssets.write_png(assets, "Bike/Livery", LIVERY_SIZE, LIVERY_SIZE, livery_pixel(seed), log)


OUTPUTS = tuple(f"Meshes/Bike/{name}.gltf" for name in MESHES) + ("Textures/Bike/Livery.png",)


def mesh_materials(name):
    """The materials a mesh's primitives use, in order (MATERIALS' names)."""
    return meshes()[name].gltf()[1]


# --- Checks -------------------------------------------------------------------------------------------------------------------

def to_bike(entity, point):
    """A point of an entity's frame (ENTITIES) in the bike's."""
    by_name = {name: (parent, translation, angle) for name, _, parent, translation, angle in ENTITIES}
    while entity != "Bike":
        parent, translation, angle = by_name[entity]
        point = add(rotate_x(point, angle), translation)
        entity = parent
    return point


def positions_in_bike(name, mesh):
    entity = next(entity for entity, mesh_name, _, _, _ in ENTITIES if mesh_name == name)
    return [to_bike(entity, point) for builder in mesh.builders.values() for point in builder.positions]


def check():
    """Measures the proportions from the meshes and their entities as made, and asserts them: the axles' distance, the
    steering axis' angle and where it meets the ground against the front tyre's contact patch, both tyres on the ground,
    and nothing below it. Returns a summary."""
    made = meshes()
    axis_foot = to_bike("Fork", (0.0, 0.0, 0.0))
    axis_top = to_bike("Fork", (0.0, 1.0, 0.0))
    rake = math.degrees(math.atan2(axis_top[2] - axis_foot[2], axis_top[1] - axis_foot[1]))
    problems = []
    contacts = {}
    for name in ("FrontWheel", "RearWheel"):
        points = positions_in_bike(name, made[name])
        lowest = min(point[1] for point in points)
        # The contact patch: the tyre's points within a millimeter of the lowest.
        patch = [point for point in points if point[1] < lowest + 0.001]
        contacts[name] = (lowest, sum(point[2] for point in patch) / len(patch))
        if abs(lowest) > 0.001:
            problems.append(f"{name} stands {lowest * 1000.0:.1f} mm off the ground")
    # The wheelbase is the distance between the contact patches (the axles' along the ground: the tyres differ in size).
    wheelbase = contacts["RearWheel"][1] - contacts["FrontWheel"][1]
    trail = contacts["FrontWheel"][1] - axis_foot[2]
    if abs(axis_foot[1]) > 1e-6:
        problems.append("the steering axis' foot is off the ground")
    if abs(wheelbase - WHEELBASE) > 0.001:
        problems.append(f"wheelbase {wheelbase:.4f} m")
    if abs(rake - RAKE) > 0.1:
        problems.append(f"rake {rake:.2f} degrees")
    if abs(trail - TRAIL) > 0.002:
        problems.append(f"trail {trail * 1000.0:.1f} mm")
    everything = [point for name, mesh in made.items() for point in positions_in_bike(name, mesh)]
    if min(point[1] for point in everything) < -0.001:
        problems.append("something lies below the ground")
    bike = [point for name, mesh in made.items() if name != "Rider" for point in positions_in_bike(name, mesh)]
    rider = positions_in_bike("Rider", made["Rider"])
    size = [max(p[k] for p in bike) - min(p[k] for p in bike) for k in range(3)]
    assert not problems, "; ".join(problems)
    triangles = sum(len(builder.indices) // 3 for parts in made.values() for builder in parts.builders.values())
    return (f"bike: wheelbase {wheelbase * 1000.0:.1f} mm, rake {rake:.2f} degrees, trail {trail * 1000.0:.1f} mm "
            f"(fork offset {FORK_OFFSET * 1000.0:.1f} mm); {size[2]:.2f} m long, {size[0]:.2f} m wide, {size[1]:.2f} m high; "
            f"the rider's helmet tops out at {max(p[1] for p in rider):.2f} m; {triangles} triangles")


def bounds():
    """The bike and rider's box in the bike's frame: (minimum, maximum) corners."""
    points = [point for name, mesh in meshes().items() for point in positions_in_bike(name, mesh)]
    return tuple(min(p[k] for p in points) for k in range(3)), tuple(max(p[k] for p in points) for k in range(3))


# --- On the grid --------------------------------------------------------------------------------------------------------------

# Where the bike waits: the pole position's slot, this far before the start and finish line and this far to the left of the
# center line, its front along the travel.
GRID_BEHIND = 9.0
GRID_LATERAL = -3.0


def grid_place():
    """The Bike entity's translation (on the road's crowned surface) and yaw on the grid."""
    s = Circuit.LENGTH - GRID_BEHIND
    return Circuit.offset_point(s, GRID_LATERAL, Circuit.road_height(GRID_LATERAL)), Circuit.yaw(s)


def check_grid(step=0.01):
    """The camera tour keeps Tour.CLEARANCE from the bike and its rider on the grid the whole way (the camera has no
    collider); returns how close it comes."""
    (x0, y0, z0), (x1, y1, z1) = bounds()
    origin, yaw = grid_place()
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    closest, t = math.inf, 0.0
    while t <= Tour.LENGTH:
        eye, _ = Tour.camera(t)
        # The eye in the bike's frame: back from the world by the yaw about +Y.
        dx, dy, dz = eye[0] - origin[0], eye[1] - origin[1], eye[2] - origin[2]
        local = (c * dx - s * dz, dy, s * dx + c * dz)
        outside = [max(low - value, 0.0, value - high) for value, low, high in zip(local, (x0, y0, z0), (x1, y1, z1))]
        closest = min(closest, math.hypot(*outside))
        t += step
    assert closest >= Tour.CLEARANCE, f"the tour's camera comes {closest:.2f} m from the bike on the grid"
    return f"on the grid {GRID_BEHIND:.0f} m before the line, the tour's camera {closest:.1f} m from it at the closest"


if __name__ == "__main__":
    print(check())
    print(check_grid())

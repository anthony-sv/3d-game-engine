"""Generates the Showcase project's meshes (Assets/Meshes/*.gltf) and textures (Assets/Textures/*.png).

Everything is made here, so the files have no license of their own and regenerate identically:

    python Projects/Showcase/Tools/GenerateAssets.py

Meshes (glTF 2.0 with positions, normals and texture coordinates, Y up, meters):
- TorusKnot: the glass sculpture, a (2, 3) torus knot standing in the XY plane, 2 m across.
- Hoop: the goal's ring, a torus of radius 1 lying in the XZ plane.
- Net: the goal's net, an open tube narrowing downwards from its top ring (radius 0.95 at the origin, 1.2 m long) that
  catches balls and drops them straight down; a net texture makes its mesh (Lattice, alpha-tested).
- Fountain: the fountain's basin with its column and upper bowl (a lathe), 4 m across.
- MirrorBall: a faceted sphere of radius 0.5 whose flat facets scatter reflections.
- Crystal: a hexagonal crystal with a pointed tip standing on the origin, 1 m tall.
- NeonStrada and NeonFrame: neon tubes spelling STRADA, and the frame around it (centered, in the XY plane).
- Beam: a light shaft, an open cone from its tip at the origin along -Z to a radius of 1 at a length of 1, its texture
  coordinate V running from the tip (0) to the end (1), so a texture can fade it out.
- TreeA, TreeB, TreeC: bare autumn trees standing on the origin, 14 to 19 m tall: a leaning trunk with a root flare that
  forks into limbs and twigs, all tapered tubes whose texture runs along them (bark grain follows the wood).
- Ground: the clearing's floor, texture coordinates in meters: a disc of 42 m radius (its first material), and around it a
  band out to 70 m in 12 rings, one material each from the inside out, which the scene fades out ring by ring into the sky's
  forest floor, so the ground ends without an edge. The band is cut into 12 sectors: each piece is a submesh of its own,
  sorted by its own distance among the other transparent surfaces.

Textures (RGBA PNG):
- Lattice: a diagonal wooden trellis with transparent gaps, for alpha-tested (Mask) materials; tiles seamlessly.
- Net: the same pattern in thin white cord, for the goal's net.
- Banner: a white banner (materials tint it) with a dark emblem, a border and a swallowtail cut out of its foot.
- Crosshair: the aiming ring shown at the center of the screen.
- BeamFade: white fading to transparent from top to bottom, which fades the light shaft along its length.
- Leaf: a pale leaf with veins on a transparent background, which particles tint into falling autumn leaves.
- SoftBand: white whose opacity fades out towards every edge, a soft backdrop that UI images tint (behind titles).
- Icons/*: the HUD buttons' white icons on transparent backgrounds: Reset (a circular arrow), Swing (a wrecking ball),
  Dominoes (a domino tipping into two more), Launch (a ball on its arc), Sun and Moon (day and night), Walk (footprints) and
  Fly (a paper plane).
"""
import base64
import json
import math
import os
import random
import struct
import zlib

ASSETS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Assets")


# --- glTF ---------------------------------------------------------------------------------------------------------------

class MeshBuilder:
    """Collects vertices (position, normal, texture coordinate) and triangles."""

    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.indices = []

    def vertex(self, position, normal, uv):
        self.positions.append(tuple(position))
        self.normals.append(tuple(normalize(normal)))
        self.uvs.append(tuple(uv))
        return len(self.positions) - 1

    def triangle(self, a, b, c):
        self.indices.extend((a, b, c))

    def quad(self, a, b, c, d):
        """Two triangles of the quad a b c d, counter-clockwise seen from the front."""
        self.indices.extend((a, b, c, a, c, d))

    def flat_polygon(self, points, uvs=None):
        """A planar convex polygon with its own vertices (flat shading), counter-clockwise seen from the front."""
        normal = normalize(cross(subtract(points[1], points[0]), subtract(points[2], points[0])))
        uvs = uvs or [(0.5 + 0.5 * math.cos(2.0 * math.pi * i / len(points)), 0.5 + 0.5 * math.sin(2.0 * math.pi * i / len(points)))
                      for i in range(len(points))]
        first = len(self.positions)
        for point, uv in zip(points, uvs):
            self.vertex(point, normal, uv)
        for k in range(1, len(points) - 1):
            self.triangle(first, first + k, first + k + 1)


def subtract(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def scale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(a):
    length = math.sqrt(dot(a, a))
    return (a[0] / length, a[1] / length, a[2] / length) if length > 1e-12 else (0.0, 1.0, 0.0)


def rotate_about(vector, axis, angle):
    """Rodrigues' rotation of the vector about the unit axis."""
    c = math.cos(angle)
    s = math.sin(angle)
    return add(add(scale(vector, c), scale(cross(axis, vector), s)), scale(axis, dot(axis, vector) * (1.0 - c)))


def write_gltf(name, mesh, materials=()):
    """Writes Meshes/<name>.gltf with the buffer embedded (base64). The mesh is a MeshBuilder, or a list of (MeshBuilder,
    material index) pairs, each a primitive of its own (a submesh) drawn with that one of the named materials."""
    parts = mesh if isinstance(mesh, list) else [(mesh, None)]
    buffer = b""
    views = []
    accessors = []
    primitives = []
    for part, material in parts:
        count = len(part.positions)
        wide = count > 65535
        first = len(accessors)
        for data, kind in ((b"".join(struct.pack("<3f", *p) for p in part.positions), "VEC3"),
                           (b"".join(struct.pack("<3f", *n) for n in part.normals), "VEC3"),
                           (b"".join(struct.pack("<2f", *uv) for uv in part.uvs), "VEC2")):
            accessors.append({"bufferView": len(views), "componentType": 5126, "count": count, "type": kind})
            views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data)})
            buffer += data
        accessors[first]["min"] = [min(p[k] for p in part.positions) for k in range(3)]
        accessors[first]["max"] = [max(p[k] for p in part.positions) for k in range(3)]
        indices = b"".join(struct.pack("<I" if wide else "<H", index) for index in part.indices)
        accessors.append({"bufferView": len(views), "componentType": 5125 if wide else 5123, "count": len(part.indices), "type": "SCALAR"})
        views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(indices)})
        buffer += indices
        # The next part's floats start on a four-byte boundary.
        while len(buffer) % 4:
            buffer += b"\0"
        primitive = {"attributes": {"POSITION": first, "NORMAL": first + 1, "TEXCOORD_0": first + 2}, "indices": first + 3}
        if material is not None:
            primitive["material"] = material
        primitives.append(primitive)
    document = {
        "asset": {"version": "2.0", "generator": "Showcase/Tools/GenerateAssets.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": primitives}],
        "buffers": [{"byteLength": len(buffer), "uri": "data:application/octet-stream;base64," + base64.b64encode(buffer).decode()}],
        "bufferViews": views,
        "accessors": accessors,
    }
    if materials:
        document["materials"] = [{"name": material} for material in materials]
    path = os.path.join(ASSETS, "Meshes", f"{name}.gltf")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as file:
        json.dump(document, file)
    vertices = sum(len(part.positions) for part, _ in parts)
    triangles = sum(len(part.indices) for part, _ in parts) // 3
    print("wrote", path, f"({vertices} vertices, {triangles} triangles, {len(parts)} primitives)")


# --- Meshes -------------------------------------------------------------------------------------------------------------

def transport(curve, closed):
    """The curve's tangents and normals, the normals carried along by parallel transport (so tubes around the curve do not
    twist), and for a closed curve the angle by which the transported frame misses the first one at the seam."""
    count = len(curve)
    tangents = []
    for i in range(count):
        before = curve[(i - 1) % count] if closed or i > 0 else curve[i]
        after = curve[(i + 1) % count] if closed or i < count - 1 else curve[i]
        tangents.append(normalize(subtract(after, before)))
    # A starting normal that is not parallel to the first tangent.
    reference = (0.0, 0.0, 1.0) if abs(tangents[0][2]) < 0.9 else (1.0, 0.0, 0.0)
    normals = [normalize(cross(cross(tangents[0], reference), tangents[0]))]
    for i in range(1, count + (1 if closed else 0)):
        previous, current = tangents[i - 1], tangents[i % count]
        axis = cross(previous, current)
        length = math.sqrt(dot(axis, axis))
        normal = normals[-1]
        if length > 1e-9:
            normal = rotate_about(normal, scale(axis, 1.0 / length), math.asin(min(1.0, length)) if dot(previous, current) > 0.0
                                  else math.pi - math.asin(min(1.0, length)))
        normals.append(normalize(subtract(normal, scale(current, dot(normal, current)))))
    twist = 0.0
    if closed:
        # The angle between the transported frame back at the start and the frame the start began with.
        end = normals[-1]
        twist = math.atan2(dot(cross(end, normals[0]), tangents[0]), dot(end, normals[0]))
    return tangents, normals, twist


def tube(mesh, curve, radius, sides, closed, u_scale=1.0):
    """A tube of the radius around the curve (a list of points). Frames are carried along by parallel transport; a closed
    curve spreads the twist the transport leaves at the seam over its length, so the surface meets itself."""
    count = len(curve)
    segments = count if closed else count - 1
    tangents, normals, twist = transport(curve, closed)
    distance = [0.0]
    for i in range(1, segments + 1):
        distance.append(distance[-1] + math.sqrt(dot(subtract(curve[i % count], curve[i - 1]), subtract(curve[i % count], curve[i - 1]))))
    rings = []
    for i in range(segments + 1):
        center = curve[i % count]
        tangent = tangents[i % count]
        normal = rotate_about(normals[i], tangent, twist * i / segments) if closed else normals[i]
        binormal = cross(tangent, normal)
        ring = []
        for j in range(sides + 1):
            angle = 2.0 * math.pi * j / sides
            direction = add(scale(normal, math.cos(angle)), scale(binormal, math.sin(angle)))
            ring.append(mesh.vertex(add(center, scale(direction, radius)), direction, (distance[i] * u_scale, j / sides)))
        rings.append(ring)
    for i in range(segments):
        for j in range(sides):
            mesh.quad(rings[i][j], rings[i][j + 1], rings[i + 1][j + 1], rings[i + 1][j])


def limb(mesh, spine, radii, sides, along):
    """A tapered tube along the spine (open at both ends) with the radius radii[i] at spine[i]. The texture's U goes once
    around it and V along it, `along` per meter, so the grain of a bark texture follows the limb."""
    tangents, normals, _ = transport(spine, False)
    distance = 0.0
    rings = []
    for i, center in enumerate(spine):
        if i > 0:
            step = subtract(center, spine[i - 1])
            distance += math.sqrt(dot(step, step))
        binormal = cross(tangents[i], normals[i])
        ring = []
        for j in range(sides + 1):
            angle = 2.0 * math.pi * j / sides
            direction = add(scale(normals[i], math.cos(angle)), scale(binormal, math.sin(angle)))
            ring.append(mesh.vertex(add(center, scale(direction, radii[i])), direction, (j / sides, distance * along)))
        rings.append(ring)
    for i in range(len(spine) - 1):
        for j in range(sides):
            mesh.quad(rings[i][j], rings[i][j + 1], rings[i + 1][j + 1], rings[i + 1][j])


def grow(generator, start, direction, length, step, rise, wander):
    """The spine of a limb from the start along the direction: every step it bends up by rise (wood grows towards the light)
    and wanders sideways by up to wander, both per meter."""
    steps = max(2, int(math.ceil(length / step)))
    step = length / steps
    points = [start]
    heading = normalize(direction)
    for _ in range(steps):
        heading = normalize(add(heading, (generator.uniform(-wander, wander) * step, rise * step, generator.uniform(-wander, wander) * step)))
        points.append(add(points[-1], scale(heading, step)))
    return points


def tree(seed, height, radius):
    """A bare tree of the height (meters) whose trunk has the radius above its root flare: the trunk leans a little and
    tapers to the top, limbs leave it in a spiral over its upper part and bend upwards, and each limb carries twigs."""
    generator = random.Random(seed)
    mesh = MeshBuilder()
    # The trunk: its spine wanders gently; its radius tapers to the top and flares at the roots.
    lean = (generator.uniform(-0.04, 0.04), 1.0, generator.uniform(-0.04, 0.04))
    trunk = grow(generator, (0.0, -0.3, 0.0), lean, height + 0.3, 0.5, 0.0, 0.025)

    def trunk_radius(t):
        flare = 1.0 + 0.7 * max(0.0, 1.0 - t / 0.07) ** 2
        return radius * flare * max(0.06, 1.0 - 0.9 * t) ** 0.9

    count = len(trunk)
    limb(mesh, trunk, [trunk_radius(i / (count - 1)) for i in range(count)], 12, 1.0 / (2.0 * math.pi * radius))
    # Limbs in a spiral (the golden angle apart) from 40% of the height up, shorter higher up.
    limbs = 7
    for k in range(limbs):
        t = 0.4 + 0.5 * k / (limbs - 1) + generator.uniform(-0.03, 0.03)
        base = trunk[min(count - 1, int(round(t * (count - 1))))]
        azimuth = 2.39996 * k + generator.uniform(-0.3, 0.3)
        elevation = math.radians(generator.uniform(28.0, 52.0))
        direction = (math.cos(elevation) * math.cos(azimuth), math.sin(elevation), math.cos(elevation) * math.sin(azimuth))
        length = height * generator.uniform(0.26, 0.36) * (1.15 - 0.6 * t)
        start_radius = trunk_radius(t) * 0.6
        spine = grow(generator, base, direction, length, 0.45, 0.35, 0.18)
        points = len(spine)
        radii = [max(0.012, start_radius * (1.0 - 0.92 * i / (points - 1))) for i in range(points)]
        limb(mesh, spine, radii, 8, 1.0 / (2.0 * math.pi * start_radius))
        # Twigs leave the limb on alternate sides along its outer two thirds.
        for twig in range(3):
            s = 0.35 + 0.22 * twig + generator.uniform(-0.05, 0.05)
            index = min(points - 2, int(round(s * (points - 1))))
            heading = normalize(subtract(spine[index + 1], spine[index]))
            side = normalize(cross(heading, (0.0, 1.0, 0.0)))
            side = scale(side, 1.0 if twig % 2 == 0 else -1.0)
            twig_direction = add(add(scale(heading, 0.6), scale(side, 0.65)), (0.0, 0.45, 0.0))
            twig_length = length * generator.uniform(0.32, 0.45) * (1.0 - 0.3 * s)
            twig_radius = radii[index] * 0.55
            twig_spine = grow(generator, spine[index], twig_direction, twig_length, 0.35, 0.5, 0.3)
            twig_points = len(twig_spine)
            limb(mesh, twig_spine, [max(0.008, twig_radius * (1.0 - 0.9 * i / (twig_points - 1))) for i in range(twig_points)], 6,
                 1.0 / (2.0 * math.pi * max(0.02, twig_radius)))
    return mesh


def ground(inner=42.0, outer=70.0, rings=12, sectors=12, segments=72, rows=7):
    """The clearing's floor, lying in the XZ plane facing up: a disc of the inner radius (material 0) in rows of quads around a
    fan, and a band out to the outer radius in rings (materials 1 to rings, from the inside out), each ring cut into sectors.
    Texture coordinates are the position in meters (X, Z). Returns the parts and the names of their materials."""
    up = (0.0, 1.0, 0.0)

    def point(radius, step):
        # Counter-clockwise seen from above: from +X towards -Z.
        angle = 2.0 * math.pi * step / segments
        return (radius * math.cos(angle), 0.0, -radius * math.sin(angle))

    def vertex(mesh, radius, step):
        position = point(radius, step)
        return mesh.vertex(position, up, (position[0], position[2]))

    disc = MeshBuilder()
    center = disc.vertex((0.0, 0.0, 0.0), up, (0.0, 0.0))
    circles = [[vertex(disc, inner * row / rows, step) for step in range(segments)] for row in range(1, rows + 1)]
    for step in range(segments):
        disc.triangle(center, circles[0][step], circles[0][(step + 1) % segments])
    for row in range(rows - 1):
        for step in range(segments):
            after = (step + 1) % segments
            disc.quad(circles[row][step], circles[row + 1][step], circles[row + 1][after], circles[row][after])
    parts = [(disc, 0)]
    width = (outer - inner) / rings
    steps = segments // sectors
    for sector in range(sectors):
        for ring in range(rings):
            piece = MeshBuilder()
            near = inner + ring * width
            far = near + width
            inside = [vertex(piece, near, sector * steps + step) for step in range(steps + 1)]
            outside = [vertex(piece, far, sector * steps + step) for step in range(steps + 1)]
            for step in range(steps):
                piece.quad(inside[step], outside[step], outside[step + 1], inside[step + 1])
            parts.append((piece, ring + 1))
    return parts, ["Ground"] + [f"Fade {ring + 1}" for ring in range(rings)]


def sphere_cap(mesh, center, radius, rings=6, sides=12):
    """A small smooth sphere: the rounded joints and ends of neon tubes."""
    grid = []
    for i in range(rings + 1):
        latitude = math.pi * i / rings
        row = []
        for j in range(sides + 1):
            longitude = 2.0 * math.pi * j / sides
            direction = (math.sin(latitude) * math.cos(longitude), math.cos(latitude), math.sin(latitude) * math.sin(longitude))
            row.append(mesh.vertex(add(center, scale(direction, radius)), direction, (j / sides, i / rings)))
        grid.append(row)
    for i in range(rings):
        for j in range(sides):
            mesh.quad(grid[i][j], grid[i][j + 1], grid[i + 1][j + 1], grid[i + 1][j])


def torus_knot():
    """A (2, 3) torus knot standing in the XY plane, about 2 m across."""
    mesh = MeshBuilder()
    points = []
    steps = 256
    for i in range(steps):
        t = 2.0 * math.pi * i / steps
        r = 2.0 + math.cos(3.0 * t)
        points.append((r * math.cos(2.0 * t) / 3.0, r * math.sin(2.0 * t) / 3.0, math.sin(3.0 * t) / 3.0))
    tube(mesh, points, 0.12, 20, closed=True, u_scale=2.0)
    return mesh


def torus(major=1.0, minor=0.07):
    """A torus of the major radius lying in the XZ plane."""
    mesh = MeshBuilder()
    points = [(major * math.cos(2.0 * math.pi * i / 96), 0.0, major * math.sin(2.0 * math.pi * i / 96)) for i in range(96)]
    tube(mesh, points, minor, 16, closed=True, u_scale=1.0)
    return mesh


def net():
    """An open tube from a ring of radius 0.95 at the origin down to one of 0.62, 1.2 m below."""
    mesh = MeshBuilder()
    profile = [(0.95 - 0.33 * (i / 6.0) ** 1.3, -1.2 * i / 6.0) for i in range(7)]
    # The lathe's normals face away from the profile's left side: listed from the bottom up they face outwards.
    lathe(mesh, list(reversed(profile)), 24, smooth=set(range(1, 6)))
    return mesh


def beam(sides=48):
    """An open cone along -Z: the tip at the origin, radius 1 at z = -1. Normals face outwards (materials show both sides)."""
    mesh = MeshBuilder()
    slope = 1.0 / math.sqrt(2.0)
    def vertex(angle, v):
        return mesh.vertex((v * math.cos(angle), v * math.sin(angle), -v), (math.cos(angle) * slope, math.sin(angle) * slope, slope),
                           (angle / (2.0 * math.pi), v))

    for j in range(sides):
        a0 = 2.0 * math.pi * j / sides
        a1 = 2.0 * math.pi * (j + 1) / sides
        # The tip, then the far edge: counter-clockwise seen from outside.
        mesh.triangle(vertex((a0 + a1) / 2.0, 0.0), vertex(a0, 1.0), vertex(a1, 1.0))
    return mesh


def lathe(mesh, profile, sides, smooth):
    """Turns the profile, a list of (radius, height) points from the outside in, around the Y axis. Normals face away from
    the profile's left side (the outside); profile points listed in smooth are shared by their neighbors' normals, the
    others make hard edges."""
    segments = []
    for k in range(len(profile) - 1):
        (r0, y0), (r1, y1) = profile[k], profile[k + 1]
        dr, dy = r1 - r0, y1 - y0
        length = math.hypot(dr, dy)
        segments.append((dy / length, -dr / length))
    # The texture's V follows the profile's length.
    lengths = [0.0]
    for k in range(len(profile) - 1):
        lengths.append(lengths[-1] + math.hypot(profile[k + 1][0] - profile[k][0], profile[k + 1][1] - profile[k][1]))
    for k in range(len(profile) - 1):
        columns = []
        for end in (k, k + 1):
            normal = segments[k]
            neighbor = end - 1 if end == k else end
            if end in smooth and 0 <= neighbor < len(segments) and neighbor != k:
                normal = ((segments[k][0] + segments[neighbor][0]) / 2.0, (segments[k][1] + segments[neighbor][1]) / 2.0)
            radius, height = profile[end]
            column = []
            for j in range(sides + 1):
                angle = 2.0 * math.pi * j / sides
                c, s = math.cos(angle), math.sin(angle)
                column.append(mesh.vertex((radius * c, height, radius * s), (normal[0] * c, normal[1], normal[0] * s),
                                          (4.0 * j / sides, lengths[end])))
            columns.append(column)
        for j in range(sides):
            mesh.quad(columns[0][j], columns[1][j], columns[1][j + 1], columns[0][j + 1])


def fountain():
    """A basin 4 m across with a rounded rim, its floor 0.15 m up, a column and an upper bowl; one mesh, so it takes one
    stone material and one mesh collider."""
    mesh = MeshBuilder()
    # The wall rises to a rounded rim (a half circle from the outside over to the inside) and falls to the floor.
    rim = [(1.94 + 0.08 * math.cos(math.pi * i / 8.0), 0.56 + 0.08 * math.sin(math.pi * i / 8.0)) for i in range(9)]
    basin = [(2.02, 0.0)] + rim + [(1.86, 0.15), (0.0, 0.15)]
    lathe(mesh, basin, 72, smooth=set(range(1, 10)))
    # The column (from the floor up) and the upper bowl, whose inside holds the water of the jets.
    column = [(0.34, 0.15), (0.34, 0.24), (0.22, 0.32), (0.18, 0.9), (0.24, 1.0), (0.3, 1.04), (0.78, 1.24), (0.82, 1.3), (0.8, 1.34),
              (0.72, 1.32), (0.2, 1.22), (0.14, 1.4), (0.18, 1.5), (0.1, 1.62), (0.0, 1.66)]
    lathe(mesh, column, 48, smooth={4, 5, 7, 8, 12, 13})
    return mesh


def mirror_ball(rings=16, sides=32):
    """A flat-faceted sphere of radius 0.5: every facet is a little mirror."""
    mesh = MeshBuilder()
    for i in range(rings):
        for j in range(sides):
            corners = []
            for di, dj in ((0, 0), (0, 1), (1, 1), (1, 0)):
                latitude = math.pi * (i + di) / rings
                longitude = 2.0 * math.pi * (j + dj) / sides
                corners.append((0.5 * math.sin(latitude) * math.cos(longitude), 0.5 * math.cos(latitude),
                                0.5 * math.sin(latitude) * math.sin(longitude)))
            # The poles' facets are triangles.
            unique = [corner for k, corner in enumerate(corners) if corner != corners[k - 1]]
            if len(unique) >= 3:
                # Counter-clockwise from outside: reverse when the polygon faces inward.
                normal = cross(subtract(unique[1], unique[0]), subtract(unique[2], unique[0]))
                center = scale(add(add(unique[0], unique[1]), unique[2]), 1.0 / 3.0)
                if dot(normal, center) < 0.0:
                    unique.reverse()
                mesh.flat_polygon(unique, [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)][:len(unique)])
    return mesh


def crystal():
    """A hexagonal quartz crystal: a slightly tapered prism with a pointed tip, 1 m tall, flat-shaded."""
    mesh = MeshBuilder()
    sides = 6
    bottom = [(0.13 * math.cos(2.0 * math.pi * k / sides), 0.0, 0.13 * math.sin(2.0 * math.pi * k / sides)) for k in range(sides)]
    top = [(0.16 * math.cos(2.0 * math.pi * k / sides), 0.72, 0.16 * math.sin(2.0 * math.pi * k / sides)) for k in range(sides)]
    tip = (0.0, 1.0, 0.0)
    for k in range(sides):
        n = (k + 1) % sides
        mesh.flat_polygon([bottom[k], top[k], top[n], bottom[n]], [(0.0, 0.0), (0.0, 1.0), (1.0, 1.0), (1.0, 0.0)])
        mesh.flat_polygon([top[k], tip, top[n]], [(0.0, 0.0), (0.5, 1.0), (1.0, 0.0)])
    mesh.flat_polygon(list(bottom))
    return mesh


# --- Neon: letters as strokes of tubes ------------------------------------------------------------------------------------

def arc(center, radius_x, radius_y, start, end, steps):
    """Points along an elliptical arc from the start to the end angle (degrees, counter-clockwise from +X)."""
    return [(center[0] + radius_x * math.cos(math.radians(start + (end - start) * i / steps)),
             center[1] + radius_y * math.sin(math.radians(start + (end - start) * i / steps))) for i in range(steps + 1)]


# Strokes of each letter in a cell 1 wide and 1.6 tall (x right, y up), as polylines.
LETTERS = {
    "S": [arc((0.5, 1.2), 0.46, 0.4, 25.0, 270.0, 20)[:-1] + arc((0.5, 0.4), 0.5, 0.4, 90.0, -155.0, 22)],
    "T": [[(0.0, 1.6), (1.0, 1.6)], [(0.5, 1.6), (0.5, 0.0)]],
    "R": [[(0.0, 0.0), (0.0, 1.6), (0.55, 1.6)] + arc((0.55, 1.2), 0.42, 0.4, 90.0, -90.0, 16)[1:] + [(0.0, 0.8)],
          [(0.5, 0.8), (1.0, 0.0)]],
    "A": [[(0.0, 0.0), (0.5, 1.6), (1.0, 0.0)], [(0.18, 0.58), (0.82, 0.58)]],
    "D": [[(0.0, 0.0), (0.0, 1.6), (0.3, 1.6)] + arc((0.3, 0.8), 0.7, 0.8, 90.0, -90.0, 24)[1:] + [(0.0, 0.0)]],
}


def neon(strokes, radius):
    """Tubes along the strokes (lists of (x, y) points) with rounded joints and ends."""
    mesh = MeshBuilder()
    for stroke in strokes:
        points = [(x, y, 0.0) for x, y in stroke]
        closed = len(points) > 2 and points[0] == points[-1]
        if closed:
            points = points[:-1]
        tube(mesh, points, radius, 14, closed=closed)
        # The ends and the joints where the stroke turns sharply get a sphere, so the tube looks bent rather than broken.
        count = len(points)
        for k, point in enumerate(points):
            if closed or 0 < k < count - 1:
                before = normalize(subtract(point, points[(k - 1) % count]))
                after = normalize(subtract(points[(k + 1) % count], point))
                if dot(before, after) > 0.95:
                    continue
            sphere_cap(mesh, point, radius)
    return mesh


def neon_word(word, height=1.0, spacing=0.42, radius=0.045):
    """The word in neon letters of the height, centered on the origin."""
    unit = height / 1.6
    strokes = []
    x = 0.0
    for letter in word:
        for stroke in LETTERS[letter]:
            strokes.append([(x + px * unit, py * unit) for px, py in stroke])
        x += (1.0 + spacing) * unit
    width = x - spacing * unit
    centered = [[(px - width / 2.0, py - height / 2.0) for px, py in stroke] for stroke in strokes]
    return neon(centered, radius), width


def neon_frame(width, height, corner=0.18, radius=0.035):
    """A rectangle with rounded corners, centered on the origin."""
    w, h = width / 2.0 - corner, height / 2.0 - corner
    outline = []
    for cx, cy, start in ((w, h, 0.0), (-w, h, 90.0), (-w, -h, 180.0), (w, -h, 270.0)):
        outline += arc((cx, cy), corner, corner, start, start + 90.0, 6)
    outline.append(outline[0])
    return neon([outline], radius)


# --- Textures -------------------------------------------------------------------------------------------------------------

def write_png(name, width, height, pixel):
    """Writes Textures/<name>.png, 8-bit RGBA, from pixel(x, y) -> (r, g, b, a) in 0..1."""
    def channel(value):
        return max(0, min(255, int(round(value * 255.0))))

    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(channel(value) for value in pixel(x, y))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    path = os.path.join(ASSETS, "Textures", f"{name}.png")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                   chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))
    print("wrote", path)


def value_noise(seed, size):
    """A tileable grid of random values, smoothly interpolated."""
    generator = random.Random(seed)
    grid = [[generator.random() for _ in range(size)] for _ in range(size)]

    def sample(u, v):
        x, y = u * size, v * size
        x0, y0 = int(math.floor(x)) % size, int(math.floor(y)) % size
        fx, fy = x - math.floor(x), y - math.floor(y)
        fx, fy = fx * fx * (3.0 - 2.0 * fx), fy * fy * (3.0 - 2.0 * fy)
        x1, y1 = (x0 + 1) % size, (y0 + 1) % size
        top = grid[y0][x0] * (1.0 - fx) + grid[y0][x1] * fx
        bottom = grid[y1][x0] * (1.0 - fx) + grid[y1][x1] * fx
        return top * (1.0 - fy) + bottom * fy

    return sample


def lattice(name, half_width, color, grain_amount, seed):
    """Strands crossing diagonally (64 pixel period on a 256 pixel tile): opaque where they are, transparent between them. The
    strands rising to the right lie over the others, which darken where they pass under."""
    size = 256
    period = 64.0
    grain = value_noise(seed, 16)
    fine = value_noise(seed + 1, 64)

    def distance_from_center(distance):
        # Distance from the strand's center line, wrapped to the period.
        return abs((distance % period) - period / 2.0)

    def pixel(x, y):
        rising = distance_from_center(x + y + 0.5)
        falling = distance_from_center(x - y + 0.5)
        on_rising = rising < half_width
        on_falling = falling < half_width
        if not on_rising and not on_falling:
            return (0.0, 0.0, 0.0, 0.0)
        across = rising if on_rising else falling
        # Rounded strands: darker towards their edges; the lower strand is shaded where the upper one crosses it.
        roundness = 1.0 - 0.35 * (across / half_width) ** 2
        shade = 1.0 if on_rising else (0.62 if rising < half_width + 3.0 else 0.86)
        along = (x - y) if on_rising else (x + y)
        texture = 1.0 - grain_amount + grain_amount * grain((along % size) / size, ((across + (0 if on_rising else 31)) % 16) / 16.0)
        texture *= 0.9 + 0.1 * fine(x / size, y / size)
        light = roundness * shade * texture
        return (color[0] * light, color[1] * light, color[2] * light, 1.0)

    write_png(name, size, size, pixel)


def banner():
    """A banner 256 x 512: white cloth (the material's color tints it) with a woven texture, a border, a dark S-shaped
    emblem, and a swallowtail notch cut out of the foot (transparent)."""
    width, height = 256, 512
    weave = value_noise(21, 128)
    emblem_strokes = []
    for stroke in LETTERS["S"]:
        emblem_strokes.append([(64.0 + px * 128.0, 140.0 + (1.6 - py) * 128.0) for px, py in stroke])

    def distance_to_strokes(x, y):
        best = 1e9
        for stroke in emblem_strokes:
            for (ax, ay), (bx, by) in zip(stroke, stroke[1:]):
                dx, dy = bx - ax, by - ay
                t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / max(1e-9, dx * dx + dy * dy)))
                best = min(best, math.hypot(x - ax - t * dx, y - ay - t * dy))
        return best

    notch_top = height - 110

    def pixel(x, y):
        center = x + 0.5 - width / 2.0
        # The swallowtail: a V cut upwards into the foot.
        if y >= notch_top and abs(center) < (y - notch_top) * 1.1:
            return (0.0, 0.0, 0.0, 0.0)
        cloth = 0.9 + 0.1 * weave(x / width, y / height) * (0.85 + 0.15 * ((x + y) % 2))
        color = cloth
        border = min(x, width - 1 - x, y)
        if border < 14:
            color *= 0.82
        elif border < 18:
            color *= 0.2
        # The emblem: a thick S in a dark ring.
        from_center = math.hypot(x + 0.5 - 128.0, y + 0.5 - 242.0)
        ring = abs(from_center - 98.0)
        stroke = distance_to_strokes(x + 0.5, y + 0.5) if from_center < 100.0 else 1e9
        ink = max(min(1.0, max(0.0, 9.0 - ring)), min(1.0, max(0.0, 15.0 - stroke)))
        color = color * (1.0 - 0.85 * ink)
        return (color, color, color, 1.0)

    write_png("Banner", width, height, pixel)


def beam_fade():
    """Opaque white at the top, fading to transparent at the bottom (quadratically, so most of the light stays near the
    source); the first rows fade in, so the shaft does not start with a hard edge at the lamp."""
    def pixel(x, y):
        v = (y + 0.5) / 256.0
        fade = (1.0 - v) ** 2 * min(1.0, v / 0.04)
        return (fade, fade, fade, fade)

    write_png("BeamFade", 4, 256, pixel)


def leaf():
    """A leaf for falling-leaf particles, 64 x 64: a pale blade (the particles' colors tint it) on a diagonal, pointed at the
    tip, with a darker midrib and side veins and a short stalk; transparent around it, with soft edges."""
    size = 64
    mottle = value_noise(31, 8)
    # The blade's axis runs from the stalk (lower left) to the tip (upper right).
    base = (14.0, 50.0)
    tip = (52.0, 12.0)
    axis = (tip[0] - base[0], tip[1] - base[1])
    length = math.hypot(*axis)
    along_unit = (axis[0] / length, axis[1] / length)
    across_unit = (-along_unit[1], along_unit[0])

    def pixel(x, y):
        px, py = x + 0.5 - base[0], y + 0.5 - base[1]
        u = (px * along_unit[0] + py * along_unit[1]) / length
        v = (px * across_unit[0] + py * across_unit[1]) / length
        # The stalk: a thin line before the blade.
        if -0.16 < u <= 0.02:
            coverage = max(0.0, min(1.0, 1.6 - abs(v) * length))
            return (0.55, 0.5, 0.42, coverage)
        if not 0.0 < u < 1.0:
            return (0.0, 0.0, 0.0, 0.0)
        half_width = 0.36 * math.sin(math.pi * u ** 0.75) ** 0.85
        edge = (half_width - abs(v)) * length
        coverage = max(0.0, min(1.0, edge + 0.5))
        if coverage <= 0.0:
            return (0.0, 0.0, 0.0, 0.0)
        shade = 0.88 + 0.12 * mottle(x / size, y / size)
        # The midrib and the side veins that leave it towards the tip.
        midrib = max(0.0, 1.0 - abs(v) * length / 1.1)
        vein = max(0.0, 1.0 - abs(((u - abs(v) * 1.3) * 7.0) % 1.0 - 0.5) * 10.0) * (1.0 if abs(v) < half_width * 0.85 else 0.0)
        shade *= 1.0 - 0.28 * max(midrib, 0.6 * vein)
        # Lighter towards the middle of the blade, darker at its rim.
        shade *= 0.85 + 0.15 * min(1.0, edge / 4.0)
        return (shade, shade * 0.97, shade * 0.92, coverage)

    write_png("Leaf", size, size, pixel)


def crosshair():
    """A thin ring with four gaps and a dot at the center, white with soft edges."""
    size = 64

    def pixel(x, y):
        dx, dy = x + 0.5 - size / 2.0, y + 0.5 - size / 2.0
        distance = math.hypot(dx, dy)
        ring = max(0.0, 1.0 - abs(distance - 13.0) / 1.6)
        # Gaps on the axes.
        angle = math.degrees(math.atan2(dy, dx)) % 90.0
        if angle < 14.0 or angle > 76.0:
            ring = 0.0
        dot_alpha = max(0.0, min(1.0, 2.6 - distance))
        alpha = max(ring, dot_alpha)
        return (1.0, 1.0, 1.0, alpha)

    write_png("Crosshair", size, size, pixel)


def soft_band():
    """White, opaque in the middle and fading out towards every edge (more slowly towards the ends), 256 x 64: a soft
    backdrop that UI images tint behind titles and captions."""
    width, height = 256, 64

    def pixel(x, y):
        u, v = (x + 0.5) / width, (y + 0.5) / height
        across = min(u, 1.0 - u) / 0.36
        down = min(v, 1.0 - v) / 0.5
        alpha = smoothstep(min(1.0, across)) * smoothstep(min(1.0, down))
        return (1.0, 1.0, 1.0, alpha)

    write_png("SoftBand", width, height, pixel)


def smoothstep(t):
    return t * t * (3.0 - 2.0 * t)


# --- HUD icons ---------------------------------------------------------------------------------------------------------------

def icon(name, inside, size=96, samples=4):
    """Writes Textures/Icons/<name>.png: a white icon on a transparent background, size x size pixels. inside(u, v) says
    whether a point (0 to 1 across, v downwards) belongs to the icon; samples x samples points per pixel smooth its edges."""
    def pixel(x, y):
        covered = 0
        for sy in range(samples):
            for sx in range(samples):
                if inside((x + (sx + 0.5) / samples) / size, (y + (sy + 0.5) / samples) / size):
                    covered += 1
        return (1.0, 1.0, 1.0, covered / (samples * samples))

    write_png(f"Icons/{name}", size, size, pixel)


def near_segment(u, v, a, b, width):
    """Whether the point lies within width / 2 of the segment a-b (a stroke with round ends)."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    t = max(0.0, min(1.0, ((u - a[0]) * dx + (v - a[1]) * dy) / (dx * dx + dy * dy)))
    return math.hypot(u - a[0] - t * dx, v - a[1] - t * dy) <= width / 2.0


def in_triangle(u, v, a, b, c):
    def side(p, q):
        return (q[0] - p[0]) * (v - p[1]) - (q[1] - p[1]) * (u - p[0])
    sides = (side(a, b), side(b, c), side(c, a))
    return all(s >= 0.0 for s in sides) or all(s <= 0.0 for s in sides)


def in_ellipse(u, v, center, radii, angle=0.0):
    """Whether the point lies in the ellipse with the radii, turned by the angle (radians, clockwise on the screen)."""
    du, dv = u - center[0], v - center[1]
    cosine, sine = math.cos(angle), math.sin(angle)
    x, y = du * cosine + dv * sine, -du * sine + dv * cosine
    return (x / radii[0]) ** 2 + (y / radii[1]) ** 2 <= 1.0


def reset_icon():
    """A circular arrow turning clockwise, its head at the top pointing into the gap at the top right."""
    center, radius, width = (0.5, 0.53), 0.27, 0.095
    gap_start, gap_end = math.radians(-100.0), math.radians(-38.0)
    end = (center[0] + radius * math.cos(gap_start), center[1] + radius * math.sin(gap_start))
    # On the screen (v down), increasing angles turn clockwise: the head points along the circle's tangent there.
    tangent = (-math.sin(gap_start), math.cos(gap_start))
    normal = (math.cos(gap_start), math.sin(gap_start))
    head = ((end[0] + tangent[0] * 0.15, end[1] + tangent[1] * 0.15),
            (end[0] + normal[0] * 0.12, end[1] + normal[1] * 0.12),
            (end[0] - normal[0] * 0.12, end[1] - normal[1] * 0.12))

    def inside(u, v):
        if in_triangle(u, v, *head):
            return True
        distance = math.hypot(u - center[0], v - center[1])
        angle = math.atan2(v - center[1], u - center[0])
        return abs(distance - radius) <= width / 2.0 and not gap_start < angle < gap_end

    icon("Reset", inside)


def swing_icon():
    """A wrecking ball swung out to the left from a beam, with motion lines trailing it."""
    pivot, ball, radius = (0.56, 0.15), (0.31, 0.62), 0.165
    along = (ball[0] - pivot[0], ball[1] - pivot[1])
    length = math.hypot(*along)
    # The ball swings to the left: the lines trail on its right, along its path.
    trail = (along[1] / length, -along[0] / length)
    side = (along[0] / length, along[1] / length)
    lines = []
    for offset, start, stop in ((-0.07, 0.24, 0.38), (0.07, 0.27, 0.36)):
        base = (ball[0] + side[0] * offset, ball[1] + side[1] * offset)
        lines.append(((base[0] + trail[0] * start, base[1] + trail[1] * start), (base[0] + trail[0] * stop, base[1] + trail[1] * stop)))

    def inside(u, v):
        return (near_segment(u, v, (0.2, 0.15), (0.84, 0.15), 0.08) or near_segment(u, v, pivot, ball, 0.045) or
                math.hypot(u - ball[0], v - ball[1]) <= radius or any(near_segment(u, v, a, b, 0.05) for a, b in lines))

    icon("Swing", inside)


def in_box(u, v, center, half, angle):
    """Whether the point lies in the rectangle of the half extents around the center, turned by the angle (radians, clockwise
    on screen)."""
    du, dv = u - center[0], v - center[1]
    cosine, sine = math.cos(angle), math.sin(angle)
    return abs(du * cosine + dv * sine) <= half[0] and abs(-du * sine + dv * cosine) <= half[1]


def dominoes_icon():
    """Three dominoes on the ground, the first tipping into the second."""
    half = (0.065, 0.22)
    ground_line = 0.82
    tilt = math.radians(26.0)
    # The first domino turns about the foot of its leading edge.
    pivot = (0.33, ground_line)
    offset = (-half[0], -half[1])
    tipped = (pivot[0] + offset[0] * math.cos(tilt) - offset[1] * math.sin(tilt),
              pivot[1] + offset[0] * math.sin(tilt) + offset[1] * math.cos(tilt))
    tiles = ((tipped, tilt), ((0.6, ground_line - half[1]), 0.0), ((0.82, ground_line - half[1]), 0.0))

    def inside(u, v):
        if 0.1 <= u <= 0.92 and ground_line + 0.03 <= v <= ground_line + 0.07:
            return True
        return any(in_box(u, v, center, half, angle) for center, angle in tiles)

    icon("Dominoes", inside)


def launch_icon():
    """A ball flying up and away on a dotted arc from the lower left."""
    start, control, end = (0.14, 0.88), (0.34, 0.1), (0.72, 0.3)

    def bezier(t):
        return tuple((1.0 - t) ** 2 * start[k] + 2.0 * (1.0 - t) * t * control[k] + t * t * end[k] for k in range(2))

    dots = [(bezier(t), 0.032 + 0.018 * t) for t in (0.04, 0.24, 0.44, 0.62)]

    def inside(u, v):
        return math.hypot(u - end[0], v - end[1]) <= 0.15 or any(math.hypot(u - c[0], v - c[1]) <= r for c, r in dots)

    icon("Launch", inside)


def sun_icon():
    """A disc with eight rays."""
    rays = [((0.5 + 0.27 * math.cos(a), 0.5 + 0.27 * math.sin(a)), (0.5 + 0.39 * math.cos(a), 0.5 + 0.39 * math.sin(a)))
            for a in (math.radians(45.0 * index) for index in range(8))]

    def inside(u, v):
        return math.hypot(u - 0.5, v - 0.5) <= 0.17 or any(near_segment(u, v, a, b, 0.075) for a, b in rays)

    icon("Sun", inside)


def moon_icon():
    """A crescent moon with a small star."""
    def inside(u, v):
        crescent = math.hypot(u - 0.46, v - 0.54) <= 0.33 and math.hypot(u - 0.62, v - 0.42) > 0.28
        # A four-pointed star at the top right: two thin diamonds crossing.
        du, dv = abs(u - 0.76), abs(v - 0.2)
        star = du / 0.1 + dv / 0.03 <= 1.0 or du / 0.03 + dv / 0.1 <= 1.0
        return crescent or star

    icon("Moon", inside)


def walk_icon():
    """Two footprints, the left one ahead: a sole with a heel and five toes each."""
    feet = []
    for center, angle, mirror in (((0.32, 0.36), -0.18, 1.0), ((0.68, 0.62), 0.18, -1.0)):
        cosine, sine = math.cos(angle), math.sin(angle)

        def place(x, y, center=center, cosine=cosine, sine=sine):
            # The foot's own frame (x across, y down the foot) turned by the angle and moved to the center.
            return (center[0] + x * cosine - y * sine, center[1] + x * sine + y * cosine)

        toes = [(place(mirror * x * 1.25, y * 1.25), r * 1.25) for x, y, r in ((-0.07, -0.19, 0.035), (-0.02, -0.215, 0.03),
                                                                               (0.025, -0.21, 0.027), (0.063, -0.19, 0.024),
                                                                               (0.092, -0.16, 0.021))]
        feet.append((place(0.0, -0.05), place(0.0, 0.15), angle, toes))

    def inside(u, v):
        for sole, heel, angle, toes in feet:
            if in_ellipse(u, v, sole, (0.11, 0.14), angle) or in_ellipse(u, v, heel, (0.08, 0.09), angle):
                return True
            if any(math.hypot(u - c[0], v - c[1]) <= r for c, r in toes):
                return True
        return False

    icon("Walk", inside)


def fly_icon():
    """A paper plane flying up to the right: its wing and its keel, with a fold between them."""
    nose, wing, fold, keel = (0.88, 0.14), (0.1, 0.46), (0.42, 0.6), (0.52, 0.88)

    def inside(u, v):
        if near_segment(u, v, fold, nose, 0.04):
            return False
        return in_triangle(u, v, wing, nose, fold) or in_triangle(u, v, fold, nose, keel)

    icon("Fly", inside)


if __name__ == "__main__":
    write_gltf("TorusKnot", torus_knot())
    write_gltf("Hoop", torus())
    write_gltf("Net", net())
    write_gltf("Fountain", fountain())
    write_gltf("MirrorBall", mirror_ball())
    write_gltf("Crystal", crystal())
    write_gltf("Beam", beam())
    word, word_width = neon_word("STRADA")
    write_gltf("NeonStrada", word)
    write_gltf("NeonFrame", neon_frame(word_width + 0.7, 1.0 + 0.6))
    for name, seed, height, radius in (("TreeA", 41, 17.0, 0.34), ("TreeB", 52, 14.0, 0.26), ("TreeC", 63, 19.0, 0.4)):
        write_gltf(name, tree(seed, height, radius))
    floor_parts, floor_materials = ground()
    write_gltf("Ground", floor_parts, floor_materials)
    lattice("Lattice", 7.5, (0.52, 0.36, 0.21), 0.25, 11)
    lattice("Net", 2.2, (0.96, 0.95, 0.92), 0.08, 13)
    banner()
    crosshair()
    beam_fade()
    leaf()
    soft_band()
    for make in (reset_icon, swing_icon, dominoes_icon, launch_icon, sun_icon, moon_icon, walk_icon, fly_icon):
        make()

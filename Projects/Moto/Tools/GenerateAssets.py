"""Generates the Moto project's circuit: its meshes (Assets/Meshes/*.gltf), textures (Assets/Textures/*.png) and sky
(Assets/Environments/Sky.hdr), from the layout in Circuit.py. Standard library only; everything is made here, so the
files have no license of their own.

    python Projects/Moto/Tools/GenerateAssets.py [--seed N] [--output DIRECTORY]
    python Projects/Moto/Tools/GenerateAssets.py --check

The seed varies the textures' noise and the hills' skyline; the circuit itself is Circuit.py's. The same seed gives the
same bytes: --check generates everything twice into temporary folders, compares the two byte for byte, and compares
them with the files in Assets (a PNG whose bytes differ there but whose pixels are equal passes, naming the file: zlib
builds may compress differently). It fails when the two runs differ or when Assets holds something else.

Meshes (glTF 2.0 with positions, normals and texture coordinates in meters, Y up; materials named per slot):
- Track: the road (crowned asphalt; around each timing line the asphalt with the line painted on it), the kerbs, the
  verges, the gravel traps and the skirts sloping down to the ground, in world space, one primitive per material and
  200 m of the lap (culled one by one).
- TrackCollision: the same surface, one primitive per surface the physics tells apart (SURFACES: asphalt, kerb, gravel,
  grass; the scene gives each its physics material), each welded (positions and normals, no texture coordinates): the
  physics builds the shape over the mesh's distinct positions, so the edges between triangles, also where two surfaces
  meet, are shared and not bumps.
- Barriers and BarrierCollision: the concrete walls along both sides, drawn and welded likewise.
- Ground: the flat meadow the circuit lies on, out to where the hills begin, in rings whose materials (GROUND_RINGS) the
  haze of the air between fades towards the horizon's color, the farther the more.
- Hills: the ring of wooded hills around it that makes the skyline, in sectors; their foot is meadow as hazy as the
  ground's last ring, so that the ground runs into the hills without an edge.
- Gantry: the start gantry over the line (legs, beam, the lights' panel and five lamps); local to its entity, which stands
  on the center line with its forward (-Z) along the direction of travel.
- Gate: a sector gate's arch with its board (local likewise).
- Footbridge: the bridge over the back straight with its piers, and at each end a flight of stairs: treads and risers on
  two stringers, under a railing (local likewise).

Textures (RGB PNG, tiling): Asphalt (12 m square, with the edge lines), TimingLines (the same asphalt with the timing
lines painted where the road around each line maps it: the sector lines and the finish line's checks), Grass (8 m, mown
in stripes, beside the track), Meadow (16 m, fine blades without stripes or patches that would repeat as a grid when
seen from the air), Gravel (3 m), Kerb (red and white blocks, 2 m along), Concrete (4 m), Forest (the hills' canopy, 60
m) and SectorBoards (the boards' "S1", "S2" and "S3" side by side).

Sky.hdr (Radiance RGBE, run-length encoded): a clear afternoon sky over hazy land, with the sun's glow where
sun_direction() points; it is the same for every seed.

The props' meshes and textures (Meshes/Props, Textures/Props; Props.py places them): a tyre stack, a cone, braking
markers (300, 200, 100), a fence post and a chain-link panel, three trees and a bush, a grandstand block, a marshals'
post, a floodlight and advertising boards with invented names, each one primitive, local to its entity and standing on
the ground at its origin. Every one but the panel is drawn with an atlas (Props.png: flat colors, the markers' figures,
the boards' names); the panel with Fence.png, a chain-link with alpha whose holes the Fence material cuts out. The
circuit and the props are generate()'s two groups (GROUPS): Author.py's circuit part writes the first, its props part
the second; the command line writes both.
"""
import argparse
import base64
import filecmp
import json
import math
import os
import random
import shutil
import struct
import sys
import tempfile
import zlib

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import Circuit  # noqa: E402  (the layout)

PROJECT = os.path.dirname(TOOLS)
SEED = 2026
# The direction towards the sun (world space, unit): from the south-west, 38 degrees up. Author.py points the sun light
# along its opposite.
SUN_ELEVATION = 38.0
SUN_AZIMUTH = 215.0
# The lap is cut into pieces of this length (meters) for culling.
CHUNK = 200.0
# The grass disc's radius and the hills' ring (meters from the circuit's center).
GROUND_RADIUS = 2200.0
HILLS_OUTER = 3600.0


def sun_direction():
    """The unit vector towards the sun: azimuth clockwise from north (-Z), elevation above the horizon."""
    elevation = math.radians(SUN_ELEVATION)
    azimuth = math.radians(SUN_AZIMUTH)
    return (math.cos(elevation) * math.sin(azimuth), math.sin(elevation), -math.cos(elevation) * math.cos(azimuth))


def circuit_center():
    """The middle of the circuit's bounds (world x and z), where the ground and the hills are centered."""
    xs, zs = [], []
    s = 0.0
    while s < Circuit.LENGTH:
        point, _, _ = Circuit.frame(s)
        xs.append(point[0])
        zs.append(point[2])
        s += 10.0
    return (round((min(xs) + max(xs)) / 2.0, 1), round((min(zs) + max(zs)) / 2.0, 1))


# --- Vectors -------------------------------------------------------------------------------------------------------------

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


def lerp(a, b, t):
    return a + (b - a) * t


# --- glTF ----------------------------------------------------------------------------------------------------------------

class MeshBuilder:
    """Vertices (position, normal, texture coordinate) and triangles. Values are rounded (positions to 0.1 mm, the rest to
    1e-6), so that the same input gives the same bytes wherever the maths library rounds its last digit differently."""

    def __init__(self, uvs=True):
        self.positions = []
        self.normals = []
        self.uvs = [] if uvs else None
        self.indices = []
        self.keys = {}

    def vertex(self, position, normal, uv=(0.0, 0.0), key=None):
        """Adds a vertex, or returns the one added before under the same key."""
        if key is not None and key in self.keys:
            return self.keys[key]
        self.positions.append(tuple(round(v, 4) + 0.0 for v in position))
        self.normals.append(tuple(round(v, 6) + 0.0 for v in normalize(normal)))
        if self.uvs is not None:
            self.uvs.append(tuple(round(v, 6) + 0.0 for v in uv))
        index = len(self.positions) - 1
        if key is not None:
            self.keys[key] = index
        return index

    def triangle(self, a, b, c):
        self.indices.extend((a, b, c))

    def quad(self, a, b, c, d, up):
        """The quad a b c d (in order round it) as two triangles, wound counter-clockwise seen from the side the up vector
        points to. A corner given twice (a quad that narrows to a triangle) leaves out the triangle it would make empty."""
        p = self.positions
        normal = cross(subtract(p[c], p[a]), subtract(p[d], p[b]))
        if dot(normal, up) < 0.0:
            a, b, c, d = a, d, c, b
        for triangle in ((a, b, c), (a, c, d)):
            if len(set(triangle)) == 3:
                self.indices.extend(triangle)

    def box(self, center, size):
        """An axis-aligned box: six faces with their own vertices (flat shading), texture coordinates in meters across each
        face."""
        hx, hy, hz = size[0] / 2.0, size[1] / 2.0, size[2] / 2.0
        cx, cy, cz = center
        faces = (((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)), ((0, 1, 0), (1, 0, 0), (0, 0, -1)),
                 ((0, -1, 0), (1, 0, 0), (0, 0, 1)), ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0)))
        half = (hx, hy, hz)
        for normal, across, up in faces:
            middle = (cx + normal[0] * hx, cy + normal[1] * hy, cz + normal[2] * hz)
            ea = sum(abs(across[k]) * half[k] for k in range(3))
            eu = sum(abs(up[k]) * half[k] for k in range(3))
            corners = []
            for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                point = add(middle, add(scale(across, su * ea), scale(up, sv * eu)))
                uv = (su * ea + ea, eu - sv * eu)
                corners.append(self.vertex(point, normal, uv))
            self.quad(*corners, normal)

    def face(self, points, normal, uvs):
        """A planar convex polygon with its own vertices."""
        first = len(self.positions)
        for point, uv in zip(points, uvs):
            self.vertex(point, normal, uv)
        count = len(points)
        for k in range(1, count - 1):
            a, b, c = first, first + k, first + k + 1
            n = cross(subtract(self.positions[b], self.positions[a]), subtract(self.positions[c], self.positions[a]))
            self.indices.extend((a, b, c) if dot(n, normal) >= 0.0 else (a, c, b))


def write_gltf(assets, name, parts, materials, log):
    """Writes Meshes/<name>.gltf with the buffer embedded (base64): one primitive per (MeshBuilder, material index) part,
    in order. The importer gives material slots in the order primitives first use them, so parts come sorted by material,
    and every material is used."""
    used = []
    for _, material in parts:
        if material not in used:
            used.append(material)
    assert used == list(range(len(materials))), f"{name}: parts must use the materials in order ({used})"
    buffer = bytearray()
    views = []
    accessors = []
    primitives = []
    for part, material in parts:
        if not part.indices:
            continue
        count = len(part.positions)
        wide = count > 65535
        attributes = {}
        streams = [("POSITION", part.positions, "VEC3", "<3f"), ("NORMAL", part.normals, "VEC3", "<3f")]
        if part.uvs is not None:
            streams.append(("TEXCOORD_0", part.uvs, "VEC2", "<2f"))
        for semantic, values, kind, form in streams:
            data = b"".join(struct.pack(form, *value) for value in values)
            accessor = {"bufferView": len(views), "componentType": 5126, "count": count, "type": kind}
            if semantic == "POSITION":
                accessor["min"] = [min(p[k] for p in values) for k in range(3)]
                accessor["max"] = [max(p[k] for p in values) for k in range(3)]
            attributes[semantic] = len(accessors)
            accessors.append(accessor)
            views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data)})
            buffer += data
        indices = b"".join(struct.pack("<I" if wide else "<H", index) for index in part.indices)
        accessors.append({"bufferView": len(views), "componentType": 5125 if wide else 5123, "count": len(part.indices),
                          "type": "SCALAR"})
        views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(indices)})
        buffer += indices
        while len(buffer) % 4:
            buffer += b"\0"
        primitives.append({"attributes": attributes, "indices": len(accessors) - 1, "material": material})
    document = {
        "asset": {"version": "2.0", "generator": "Moto/Tools/GenerateAssets.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": primitives}],
        "materials": [{"name": material} for material in materials],
        "buffers": [{"byteLength": len(buffer), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
        "bufferViews": views,
        "accessors": accessors,
    }
    path = os.path.join(assets, "Meshes", f"{name}.gltf")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as file:
        json.dump(document, file, separators=(",", ":"))
    vertices = sum(len(part.positions) for part, _ in parts)
    triangles = sum(len(part.indices) for part, _ in parts) // 3
    log(f"Meshes/{name}.gltf: {vertices} vertices, {triangles} triangles, {len(primitives)} primitives")


# --- The track ------------------------------------------------------------------------------------------------------------

TRACK_MATERIALS = ("Asphalt", "Kerb", "Grass", "Gravel", "TimingLines")
ASPHALT, KERB, GRASS, GRAVEL, TIMING_LINES = range(len(TRACK_MATERIALS))
# The surfaces the physics tells apart, in the order of TrackCollision's primitives (its submeshes): the scene gives each
# its physics material (Assets/Surfaces/<name>.sphysmat), which raycasts and contacts name.
SURFACES = ("Asphalt", "Kerb", "Gravel", "Grass")
# The surface of each drawn material: the timing lines are paint on the asphalt.
SURFACE_OF = {ASPHALT: 0, TIMING_LINES: 0, KERB: 1, GRAVEL: 2, GRASS: 3}
# The asphalt texture's size along the lap and across it (meters): Asphalt and TimingLines map the road alike.
ASPHALT_TILE = 12.0


def side_profile(s, side):
    """The cross-section's points on one side, from the road's edge outwards: (lateral distance, height)."""
    k = Circuit.kerb(s, side)
    g = Circuit.gravel(s, side)
    edge = Circuit.HALF_WIDTH
    kerb_out = edge + Circuit.KERB_WIDTH
    verge_out = kerb_out + Circuit.VERGE_WIDTH
    gravel_out = verge_out + g
    skirt_out = gravel_out + Circuit.SKIRT_WIDTH
    road = Circuit.ROAD_HEIGHT
    top = road + Circuit.KERB_HEIGHT
    # Without a kerb the strip is grass a centimeter below the road's edge.
    grass = road - 0.01
    return [(edge, road), (edge + Circuit.KERB_RISE, lerp(road, top, k)), (kerb_out, lerp(grass, top, k)),
            (verge_out, Circuit.VERGE_HEIGHT), (gravel_out, lerp(Circuit.VERGE_HEIGHT, Circuit.GRAVEL_HEIGHT, min(1.0, g))),
            (skirt_out, 0.0)]


def cross_section(s):
    """The cross-section's points across the circuit at the lap distance, from the left skirt to the right: (lateral
    distance, positive to the right, height)."""
    left = [(-lateral, height) for lateral, height in reversed(side_profile(s, "left"))]
    right = side_profile(s, "right")
    return left + [(0.0, Circuit.road_height(0.0))] + right


# The strips between the cross-section's points (13 points, 12 strips), from the left: skirt, gravel, verge, kerb outer,
# kerb rise, road, road, kerb rise, kerb outer, verge, gravel, skirt.
STRIPS = ("skirt", "gravel", "verge", "kerb", "kerb", "road", "road", "kerb", "kerb", "verge", "gravel", "skirt")


def band_at(s):
    """The timing line whose band of painted road (Circuit.timing_band) holds the lap distance; None elsewhere."""
    for line in Circuit.SECTOR_GATES:
        start, end = Circuit.timing_band(line)
        if (s - start) % Circuit.LENGTH < end - start:
            return line
    return None


def strip_material(kind, s0, s1, side):
    middle = (s0 + s1) / 2.0
    if kind == "road":
        return TIMING_LINES if band_at(middle) is not None else ASPHALT
    if kind == "kerb":
        return KERB if Circuit.kerb(middle, side) > 0.02 else GRASS
    if kind == "gravel":
        return GRAVEL if Circuit.gravel(middle, side) > 0.05 else GRASS
    return GRASS


def strip_uv(material, s, lateral, point):
    """Texture coordinates in meters; the materials' UVTiling sets how often their textures repeat. The timing lines' road
    maps the texture as the asphalt does, so the two meet without a seam and each line lies where painted_lines() puts
    it."""
    if material in (ASPHALT, TIMING_LINES):
        return (lateral + Circuit.HALF_WIDTH, s)
    if material == KERB:
        return (abs(lateral) - Circuit.HALF_WIDTH, s)
    return (point[0], point[2])


def tile_span(start, end):
    """A span of lap distances as the road maps it into the asphalt texture: (start, end) meters along a tile, start in
    [0, ASPHALT_TILE). Spans before the line (negative) map as the rows at the lap's end do (LENGTH on)."""
    if start < 0.0:
        start, end = start + Circuit.LENGTH, end + Circuit.LENGTH
    return start % ASPHALT_TILE, start % ASPHALT_TILE + (end - start)


def painted_lines():
    """Where the timing lines lie in the asphalt texture: (start, end, is the finish line) per line, in meters along the
    tile. Asserts that each band of painted road shows its own line alone."""
    lines = [tile_span(*Circuit.timing_line(line)) + (line == 0.0,) for line in Circuit.SECTOR_GATES]
    for line in Circuit.SECTOR_GATES:
        low, high = tile_span(*Circuit.timing_band(line))
        for other, (paint_start, paint_end, _) in zip(Circuit.SECTOR_GATES, lines):
            if other == line:
                continue
            for shift in (-ASPHALT_TILE, 0.0, ASPHALT_TILE):
                assert paint_end + shift <= low or paint_start + shift >= high, \
                    f"the band of the line at {line:.1f} m would show the line at {other:.1f} m"
    return lines


def build_track():
    """The drawn surface (one MeshBuilder per material and chunk) and the welded collision surface."""
    sections = Circuit.sections()
    count = len(sections)
    rows = []
    for s in sections:
        (px, _, pz), _, (rx, _, rz) = Circuit.frame(s)
        rows.append([(px + lateral * rx, height, pz + lateral * rz) for lateral, height in cross_section(s)])
    laterals = [[lateral for lateral, _ in cross_section(s)] for s in sections]

    def face_normal(i, j):
        """The normal of strip j between sections i and i + 1 (wrapping)."""
        a, b = rows[i][j], rows[i][j + 1]
        c = rows[(i + 1) % count][j]
        n = cross(subtract(c, a), subtract(b, a))
        return normalize(n if n[1] >= 0.0 else scale(n, -1.0))

    faces = [[face_normal(i, j) for j in range(len(STRIPS))] for i in range(count)]
    chunks = int(math.ceil(Circuit.LENGTH / CHUNK))
    parts = {}
    side_of = ["left"] * 6 + ["right"] * 6
    for i in range(count):
        s0 = sections[i]
        s1 = sections[i + 1] if i + 1 < count else Circuit.LENGTH
        chunk = min(chunks - 1, int(s0 // CHUNK))
        for j, kind in enumerate(STRIPS):
            material = strip_material(kind, s0, s1, side_of[j])
            if kind == "gravel" and abs(laterals[i][j + 1] - laterals[i][j]) < 1e-3 and \
                    abs(laterals[(i + 1) % count][j + 1] - laterals[(i + 1) % count][j]) < 1e-3:
                continue
            mesh = parts.setdefault((material, chunk), MeshBuilder())
            corners = []
            for (row, s), column in (((i, s0), j), ((i, s0), j + 1), (((i + 1) % count, s1), j + 1), (((i + 1) % count, s1), j)):
                point = rows[row][column]
                # Smooth along the lap within the strip: the normal of the strip's two faces meeting at the row.
                normal = normalize(add(faces[row][j], faces[row - 1][j]))
                uv = strip_uv(material, s, laterals[row][column], point)
                corners.append(mesh.vertex(point, normal, uv, key=(row, column, j, s == Circuit.LENGTH)))
            mesh.quad(*corners, (0.0, 1.0, 0.0))
    drawn = [(parts[key], key[0]) for key in sorted(parts)]

    # One primitive per surface, each welded: points at the same place (a gravel trap of no width) are one vertex.
    surfaces = [MeshBuilder(uvs=False) for _ in SURFACES]
    for i in range(count):
        s0 = sections[i]
        s1 = sections[i + 1] if i + 1 < count else Circuit.LENGTH
        for j, kind in enumerate(STRIPS):
            collision = surfaces[SURFACE_OF[strip_material(kind, s0, s1, side_of[j])]]
            a, b = welded(collision, rows[i][j]), welded(collision, rows[i][j + 1])
            c, d = welded(collision, rows[(i + 1) % count][j + 1]), welded(collision, rows[(i + 1) % count][j])
            if len({a, b, c, d}) >= 3:
                collision.quad(a, b, c, d, (0.0, 1.0, 0.0))
    return drawn, [(surface, index) for index, surface in enumerate(surfaces)]


def welded(mesh, point):
    """The vertex of a collision mesh at the point (to 0.1 mm), added when there is none. Collision vertices all point up
    and have no texture coordinates, so the importer keeps them welded."""
    return mesh.vertex(point, (0.0, 1.0, 0.0), key=tuple(round(v, 4) for v in point))


# --- The barriers ----------------------------------------------------------------------------------------------------------

# The wall's profile across it, from its face (towards the track) to its back: (distance from the face, height).
BARRIER_PROFILE = ((0.0, 0.0), (0.2, Circuit.BARRIER_HEIGHT), (0.2 + Circuit.BARRIER_TOP, Circuit.BARRIER_HEIGHT),
                   (Circuit.BARRIER_BASE, 0.0))


def build_barriers(sections):
    count = len(sections)
    rows = {}
    for side, sign in (("left", -1.0), ("right", 1.0)):
        side_rows = []
        for s in sections:
            face = Circuit.barrier_offset(s, side)
            side_rows.append([Circuit.offset_point(s, sign * (face + across), height) for across, height in BARRIER_PROFILE])
        rows[side] = side_rows
    chunks = int(math.ceil(Circuit.LENGTH / CHUNK))
    parts = [MeshBuilder() for _ in range(chunks)]
    collision = MeshBuilder(uvs=False)

    for side, sign in (("left", -1.0), ("right", 1.0)):
        side_rows = rows[side]
        for i in range(count):
            s0 = sections[i]
            s1 = sections[i + 1] if i + 1 < count else Circuit.LENGTH
            mesh = parts[min(chunks - 1, int(s0 // CHUNK))]
            row0, row1 = side_rows[i], side_rows[(i + 1) % count]
            across = 0.0
            for j in range(len(BARRIER_PROFILE) - 1):
                width = math.dist(row0[j], row0[j + 1])
                # Each face of the profile is flat across; its outward side faces away from the wall's middle.
                middle = Circuit.offset_point(s0, sign * (Circuit.barrier_offset(s0, side) + Circuit.BARRIER_BASE / 2.0), 0.3)
                outward = subtract(scale(add(row0[j], row0[j + 1]), 0.5), middle)
                along = subtract(row1[j], row0[j])
                normal = normalize(cross(along, subtract(row0[j + 1], row0[j])))
                if dot(normal, outward) < 0.0:
                    normal = scale(normal, -1.0)
                # Vertices are shared along the lap within a face (but at the lap's end, where v starts again).
                key0, key1 = (side, i, j), (side, (i + 1) % count if s1 < Circuit.LENGTH else count, j)
                corners = [mesh.vertex(row0[j], normal, (s0, across), key0 + (0,)),
                           mesh.vertex(row0[j + 1], normal, (s0, across + width), key0 + (1,)),
                           mesh.vertex(row1[j + 1], normal, (s1, across + width), key1 + (1,)),
                           mesh.vertex(row1[j], normal, (s1, across), key1 + (0,))]
                mesh.quad(*corners, normal)
                collision.quad(welded(collision, row0[j]), welded(collision, row0[j + 1]), welded(collision, row1[j + 1]),
                               welded(collision, row1[j]), normal)
                across += width
    return [(part, 0) for part in parts], collision


# --- The ground and the hills ---------------------------------------------------------------------------------------------

SEGMENTS_AROUND = 128


def skyline(seed):
    """The hills' height (meters) by angle: a few long swells with seeded phases."""
    generator = random.Random(seed * 7 + 3)
    waves = [(frequency, generator.uniform(0.0, 2.0 * math.pi), amplitude)
             for frequency, amplitude in ((2, 38.0), (3, 26.0), (5, 18.0), (8, 10.0), (13, 6.0))]

    def height(angle):
        return 110.0 + sum(amplitude * math.sin(frequency * angle + phase) for frequency, phase, amplitude in waves)

    return height


# The ground's rings, from the middle outwards: (outer radius in meters, material). The meadow is the same everywhere; the
# farther rings' materials lighten and blue it towards the horizon's color, as the air between does (there is no fog), up
# to the haze of the hills' foot, so that the ground meets the hills without an edge. Author.py makes the materials.
GROUND_RINGS = ((600.0, "Meadow"),) + tuple((600.0 + 200.0 * ring, f"MeadowHaze{ring}") for ring in range(1, 9))
assert GROUND_RINGS[-1][0] == GROUND_RADIUS
GROUND_MATERIALS = tuple(material for _, material in GROUND_RINGS)


def build_ground():
    """The flat meadow out to the hills: a disc and rings around it, one primitive per ring (GROUND_RINGS)."""
    cx, cz = circuit_center()
    parts = []
    inner = 0.0
    for index, (outer, _) in enumerate(GROUND_RINGS):
        mesh = MeshBuilder()

        def ring(radius):
            vertices = []
            for k in range(SEGMENTS_AROUND):
                angle = 2.0 * math.pi * k / SEGMENTS_AROUND
                point = (cx + radius * math.cos(angle), 0.0, cz + radius * math.sin(angle))
                vertices.append(mesh.vertex(point, (0.0, 1.0, 0.0), (point[0], point[2])))
            return vertices

        outside = ring(outer)
        if inner == 0.0:
            middle = mesh.vertex((cx, 0.0, cz), (0.0, 1.0, 0.0), (cx, cz))
            for k in range(SEGMENTS_AROUND):
                a, b = outside[k], outside[(k + 1) % SEGMENTS_AROUND]
                n = cross(subtract(mesh.positions[a], mesh.positions[middle]), subtract(mesh.positions[b], mesh.positions[middle]))
                mesh.indices.extend((middle, a, b) if n[1] > 0.0 else (middle, b, a))
        else:
            inside = ring(inner)
            for k in range(SEGMENTS_AROUND):
                after = (k + 1) % SEGMENTS_AROUND
                mesh.quad(inside[k], inside[after], outside[after], outside[k], (0.0, 1.0, 0.0))
        parts.append((mesh, index))
        inner = outer
    return parts


def build_hills(seed):
    """The ring of hills from the ground's edge outwards: rising to a ridge, then falling a little behind it, in 16
    sectors."""
    cx, cz = circuit_center()
    height = skyline(seed)
    radii = (0.0, 0.08, 0.2, 0.35, 0.5, 0.65, 0.8, 1.0)
    profile = (0.0, 0.05, 0.22, 0.5, 0.8, 0.97, 1.0, 0.82)

    def point(k, r):
        angle = 2.0 * math.pi * (k % SEGMENTS_AROUND) / SEGMENTS_AROUND
        radius = lerp(GROUND_RADIUS, HILLS_OUTER, radii[r])
        y = height(angle) * profile[r]
        return (cx + radius * math.cos(angle), y, cz + radius * math.sin(angle))

    def normal(k, r):
        across = subtract(point(k + 1, r), point(k - 1, r))
        outward = subtract(point(k, min(r + 1, len(radii) - 1)), point(k, max(r - 1, 0)))
        n = cross(outward, across)
        n = n if n[1] > 0.0 else scale(n, -1.0)
        return normalize(n)

    # The forest covers the slopes from the second row up; the foot of the hills, below it, is meadow in the ground's last
    # ring's haze, one primitive round the whole ring.
    sectors = 16
    per = SEGMENTS_AROUND // sectors
    parts = []
    foot = MeshBuilder()
    for sector in range(sectors):
        mesh = MeshBuilder()
        for k in range(sector * per, (sector + 1) * per):
            for r in range(len(radii) - 1):
                target = foot if r == 0 else mesh
                corners = []
                for kk, rr in ((k, r), (k + 1, r), (k + 1, r + 1), (k, r + 1)):
                    p = point(kk, rr)
                    corners.append(target.vertex(p, normal(kk, rr), (p[0], p[2]), key=(kk, rr)))
                target.quad(*corners, (0.0, 1.0, 0.0))
        parts.append((mesh, 0))
    parts.append((foot, 1))
    return parts


# The hills' materials: the forest, and at their foot the meadow of the ground's last ring.
HILLS_MATERIALS = ("Forest", GROUND_MATERIALS[-1])


# --- The structures (local to their entities: x right, y up, -z the direction of travel) ----------------------------------

def build_gantry():
    steel, panel, lamp, accent = MeshBuilder(), MeshBuilder(), MeshBuilder(), MeshBuilder()
    leg = Circuit.LEG_OFFSET
    for x in (-leg, leg):
        steel.box((x, 3.8, 0.0), (0.6, 7.6, 0.6))
        steel.box((x, 0.15, 0.0), (1.2, 0.3, 1.2))
    steel.box((0.0, 7.0, 0.0), (2.0 * leg + 0.8, 0.5, 1.0))
    steel.box((0.0, 7.75, 0.0), (2.0 * leg + 0.8, 0.2, 0.6))
    # The accent band faces the riders coming towards it (+Z), proud of the beam's front.
    accent.box((0.0, 7.0, 0.53), (2.0 * leg + 0.4, 0.42, 0.06))
    panel.box((0.0, 5.75, 0.0), (6.4, 1.5, 0.4))
    for k in range(5):
        lamp.box((-2.4 + 1.2 * k, 5.75, 0.24), (0.7, 0.7, 0.1))
    return [(steel, 0), (panel, 1), (lamp, 2), (accent, 3)], ("GantrySteel", "GantryPanel", "StartLamp", "GantryAccent")


def build_gate():
    steel, board = MeshBuilder(), MeshBuilder()
    leg = Circuit.LEG_OFFSET
    for x in (-leg, leg):
        steel.box((x, 3.2, 0.0), (0.4, 6.4, 0.4))
        steel.box((x, 0.12, 0.0), (0.9, 0.24, 0.9))
    steel.box((0.0, 6.2, 0.0), (2.0 * leg + 0.4, 0.4, 0.4))
    # The board: its front (+Z, towards the riders) shows the whole texture, the other faces its edge.
    width, height, depth, y = 4.2, 1.4, 0.2, 5.1
    hw, hh, hd = width / 2.0, height / 2.0, depth / 2.0
    board.face([(-hw, y - hh, hd), (hw, y - hh, hd), (hw, y + hh, hd), (-hw, y + hh, hd)], (0.0, 0.0, 1.0),
               [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)])
    board.face([(hw, y - hh, -hd), (-hw, y - hh, -hd), (-hw, y + hh, -hd), (hw, y + hh, -hd)], (0.0, 0.0, -1.0),
               [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)])
    edge = (0.01, 0.01)
    for normal, points in (((1.0, 0.0, 0.0), [(hw, y - hh, hd), (hw, y - hh, -hd), (hw, y + hh, -hd), (hw, y + hh, hd)]),
                           ((-1.0, 0.0, 0.0), [(-hw, y - hh, -hd), (-hw, y - hh, hd), (-hw, y + hh, hd), (-hw, y + hh, -hd)]),
                           ((0.0, 1.0, 0.0), [(-hw, y + hh, hd), (hw, y + hh, hd), (hw, y + hh, -hd), (-hw, y + hh, -hd)]),
                           ((0.0, -1.0, 0.0), [(-hw, y - hh, -hd), (hw, y - hh, -hd), (hw, y - hh, hd), (-hw, y - hh, hd)])):
        board.face(points, normal, [edge] * 4)
    return [(steel, 0), (board, 1)], ("GateSteel", "SectorBoard")


def slab(mesh, outline, z0, z1):
    """A prism standing on a convex outline in the x-y plane (meters), from z0 to z1: its two faces and its sides, each
    with vertices of its own (flat shading), texture coordinates in meters."""
    area = sum(outline[k][0] * outline[(k + 1) % len(outline)][1] - outline[(k + 1) % len(outline)][0] * outline[k][1]
               for k in range(len(outline)))
    if area < 0.0:
        outline = list(reversed(outline))
    mesh.face([(x, y, z1) for x, y in outline], (0.0, 0.0, 1.0), [(x, -y) for x, y in outline])
    mesh.face([(x, y, z0) for x, y in reversed(outline)], (0.0, 0.0, -1.0), [(-x, -y) for x, y in reversed(outline)])
    for k, (x0, y0) in enumerate(outline):
        x1, y1 = outline[(k + 1) % len(outline)]
        length = math.hypot(x1 - x0, y1 - y0)
        normal = normalize((y1 - y0, x0 - x1, 0.0))
        mesh.face([(x0, y0, z0), (x1, y1, z0), (x1, y1, z1), (x0, y0, z1)], normal,
                  [(0.0, 0.0), (length, 0.0), (length, z1 - z0), (0.0, z1 - z0)])


def build_footbridge():
    concrete, panel = MeshBuilder(), MeshBuilder()
    deck_bottom = Circuit.BRIDGE_CLEARANCE
    deck_top = deck_bottom + 0.5
    span = Circuit.PIER_OFFSET + 1.4
    width = 3.2
    concrete.box((0.0, (deck_bottom + deck_top) / 2.0, 0.0), (2.0 * span, deck_top - deck_bottom, width))
    for x in (-Circuit.PIER_OFFSET, Circuit.PIER_OFFSET):
        for z in (-1.1, 1.1):
            concrete.box((x, deck_bottom / 2.0, z), (0.5, deck_bottom, 0.5))
    for z in (-width / 2.0 + 0.06, width / 2.0 - 0.06):
        panel.box((0.0, deck_top + 0.6, z), (2.0 * span, 1.2, 0.12))
    # The stairs go down from each end of the deck, outwards: a tread and a riser per step, open beneath, resting on a
    # stringer each side, with a railing above the stringers. Seen from the side, the steps' edges show above the
    # stringers.
    steps = 38
    rise = deck_top / steps
    run = 0.28
    tread = 0.05
    inside = width / 2.0 - 0.2
    count = 0
    while deck_top - (count + 1) * rise > 1e-6:
        count += 1
    for sign in (-1.0, 1.0):
        previous = deck_top
        for k in range(count):
            top = deck_top - (k + 1) * rise
            inner = span + k * run
            concrete.box((sign * (inner + run / 2.0), top - tread / 2.0, 0.0), (run, tread, 2.0 * inside))
            concrete.box((sign * (inner + 0.015), (top - tread + previous) / 2.0, 0.0), (0.03, previous - top + tread, 2.0 * inside))
            previous = top
        # The stringers' tops run below the steps' inner corners (the line of the nosings less a rise and a tread), their
        # undersides 0.35 m lower, both down to the ground.
        slope = rise / run
        top0 = deck_top - rise - tread - 0.02
        bottom0 = top0 - 0.35
        outline = [(sign * span, bottom0), (sign * (span + bottom0 / slope), 0.0), (sign * (span + top0 / slope), 0.0),
                   (sign * span, top0)]
        for z0, z1 in ((inside, width / 2.0), (-width / 2.0, -inside)):
            slab(concrete, outline, z0, z1)
        # The railing: posts every fourth step, and a hand rail and a middle rail along the slope, 1 m and 0.5 m over the
        # nosings.
        last = span + count * run
        for z in (-width / 2.0 + 0.1, width / 2.0 - 0.1):
            for k in range(0, count, 4):
                top = deck_top - (k + 1) * rise
                panel.box((sign * (span + k * run + run / 2.0), top + 0.5, z), (0.06, 1.0, 0.06))
            for height in (1.0, 0.5):
                start, end = deck_top + height, deck_top - count * rise + height
                rail = [(sign * span, start - 0.03), (sign * last, end - 0.03), (sign * last, end + 0.03), (sign * span, start + 0.03)]
                slab(panel, rail, z - 0.03, z + 0.03)
    return [(concrete, 0), (panel, 1)], ("Concrete", "BridgePanel")


# --- Textures ------------------------------------------------------------------------------------------------------------

def write_png(assets, name, width, height, pixel, log, alpha=False):
    """Writes Textures/<name>.png, 8-bit RGB, from pixel(x, y) -> (r, g, b) in 0..1 (sRGB); with alpha, RGBA from (r, g, b,
    a)."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            values = pixel(x, y)
            assert len(values) == (4 if alpha else 3), f"{name}: {len(values)} channels"
            for value in values:
                rows.append(max(0, min(255, int(round(value * 255.0)))))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    path = os.path.join(assets, "Textures", f"{name}.png")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2, 0, 0, 0)) +
                   chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))
    log(f"Textures/{name}.png: {width}x{height}")


def value_noise(seed, size):
    """A tileable grid of random values in 0..1, smoothly interpolated; sample(u, v) takes coordinates in tiles."""
    generator = random.Random(seed)
    grid = [[generator.random() for _ in range(size)] for _ in range(size)]

    def sample(u, v):
        x, y = u * size, v * size
        x0, y0 = int(math.floor(x)), int(math.floor(y))
        fx, fy = x - x0, y - y0
        fx, fy = fx * fx * (3.0 - 2.0 * fx), fy * fy * (3.0 - 2.0 * fy)
        x0, y0 = x0 % size, y0 % size
        x1, y1 = (x0 + 1) % size, (y0 + 1) % size
        top = grid[y0][x0] * (1.0 - fx) + grid[y0][x1] * fx
        bottom = grid[y1][x0] * (1.0 - fx) + grid[y1][x1] * fx
        return top * (1.0 - fy) + bottom * fy

    return sample


def fractal(seed, octaves, base=4):
    """Tileable fractal noise in about 0..1: octaves of value noise, each twice as fine and half as strong."""
    layers = [value_noise(seed + k, base << k) for k in range(octaves)]

    def sample(u, v):
        total, weight, amplitude = 0.0, 0.0, 1.0
        for layer in layers:
            total += layer(u, v) * amplitude
            weight += amplitude
            amplitude *= 0.5
        return total / weight

    return sample


def quantize(value, levels=96):
    """Rounds a channel to fewer levels: the noise compresses better and looks the same."""
    return round(value * levels) / levels


def make_textures(assets, seed, log):
    size = 512
    # Asphalt: 12 m square; u runs across the road (0 at the left edge), v along it. Fine aggregate on a darker binder,
    # broad patches, the racing line's rubber darker in the middle of the road, and the white edge lines (25 cm wide,
    # 20 cm in from each edge), worn.
    grit = value_noise(seed + 1, 512)
    patches = fractal(seed + 2, 4, 4)
    wear = fractal(seed + 3, 3, 8)
    meters = 12.0

    def asphalt(x, y):
        u, v = (x + 0.5) / size, (y + 0.5) / size
        lateral = u * meters
        g = grit(u, v)
        speck = 0.035 if g > 0.85 else (-0.03 if g < 0.15 else 0.0)
        tone = 0.3 + 0.04 * (patches(u, v) - 0.5) + speck + 0.02 * (grit(u * 0.5 + 0.25, v * 0.5) - 0.5)
        rubber = math.exp(-((lateral - 6.0) / 2.6) ** 2)
        tone -= 0.04 * rubber
        color = [tone, tone, tone * 1.01]
        line = min(abs(lateral - 0.325), abs(lateral - (meters - 0.325)))
        if line < 0.125:
            paint = 0.82 - 0.18 * wear(u, v) - 0.06 * max(0.0, g - 0.7)
            color = [paint, paint, paint * 0.98]
        return [quantize(c) for c in color]

    write_png(assets, "Asphalt", size, size, asphalt, log)

    # TimingLines: the asphalt with the timing lines painted where the road around each line maps them (painted_lines()):
    # the sector lines in the edge lines' worn paint, the finish line's two rows of 0.5 m checks across the road. A pixel
    # the paint touches takes its share of it (4 x 4 samples), so the lines' edges are smooth.
    lines = painted_lines()
    pixel_meters = meters / size

    def painted(u_meters, v_meters, base):
        for start, end, finish in lines:
            for shift in (-meters, 0.0, meters):
                if start + shift <= v_meters < end + shift:
                    if not finish:
                        tone = 0.82 - 0.18 * wear(u_meters / meters, v_meters / meters)
                        return [tone, tone, tone * 0.98]
                    dark = (int(u_meters // 0.5) + int((v_meters - start - shift) // 0.5)) % 2 == 1
                    tone = 0.06 if dark else 0.85
                    return [tone, tone, tone]
        return base

    def timing_lines(x, y):
        base = asphalt(x, y)
        v0, v1 = y * pixel_meters, (y + 1) * pixel_meters
        if not any(start + shift < v1 and v0 < end + shift for start, end, _ in lines for shift in (-meters, 0.0, meters)):
            return base
        total = [0.0, 0.0, 0.0]
        for sy in range(4):
            for sx in range(4):
                color = painted((x + (sx + 0.5) / 4.0) * pixel_meters, (y + (sy + 0.5) / 4.0) * pixel_meters, base)
                total = [total[k] + color[k] / 16.0 for k in range(3)]
        return [quantize(c) for c in total]

    write_png(assets, "TimingLines", size, size, timing_lines, log)

    # Grass: 8 m square, mown in two stripes (4 m each, along v) of lighter and darker green, with clumps and blades.
    blades = value_noise(seed + 11, 256)
    clumps = fractal(seed + 12, 4, 4)
    tint = fractal(seed + 13, 3, 2)

    def grass(x, y):
        u, v = (x + 0.5) / size, (y + 0.5) / size
        stripe = 0.5 + 0.5 * math.cos(2.0 * math.pi * u)
        stripe = 0.5 + 0.5 * math.tanh(4.0 * (stripe - 0.5))
        light = 0.9 + 0.07 * stripe + 0.2 * (clumps(u, v) - 0.5) + 0.18 * (blades(u, v * 0.35) - 0.5)
        warm = tint(u, v)
        color = (0.22 + 0.07 * warm, 0.37 + 0.04 * warm, 0.14)
        return [quantize(c * light) for c in color]

    write_png(assets, "Grass", size, size, grass, log)

    # Gravel: 3 m square, rounded pebbles in sand and stone colors.
    small = 256
    pebbles = value_noise(seed + 21, 128)
    coarse = fractal(seed + 22, 3, 6)
    hue = value_noise(seed + 23, 96)

    def gravel(x, y):
        u, v = (x + 0.5) / small, (y + 0.5) / small
        p = pebbles(u, v)
        shade = 0.8 + 0.3 * (p - 0.5) + 0.15 * (coarse(u, v) - 0.5)
        if p < 0.25:
            shade *= 0.85
        h = hue(u + 0.13, v + 0.37)
        color = (0.62 + 0.08 * h, 0.55 + 0.05 * h, 0.43 - 0.03 * h)
        return [quantize(c * shade) for c in color]

    write_png(assets, "Gravel", small, small, gravel, log)

    # Kerb: across it (u, 1.2 m) and along it (v, 2 m): a red block then a white one, with rounded shading at the block
    # joints and grime.
    grime = fractal(seed + 31, 3, 4)

    def kerb(x, y):
        u, v = (x + 0.5) / 64, (y + 0.5) / 128
        red = v < 0.5
        joint = min(v % 0.5, 0.5 - v % 0.5) * 2.0
        bevel = 0.82 + 0.18 * min(1.0, joint / 0.03)
        dirt = 0.9 + 0.1 * grime(u, v)
        color = (0.62, 0.05, 0.04) if red else (0.86, 0.86, 0.84)
        return [quantize(c * bevel * dirt) for c in color]

    write_png(assets, "Kerb", 64, 128, kerb, log)

    # Meadow: 16 m square of uncut grass, blades and small tufts only. A stripe or a patch the size of meters would repeat
    # every tile and show as a grid from the air; detail this fine averages out at a distance instead.
    fine = value_noise(seed + 61, 512)
    tufts = value_noise(seed + 62, 128)
    hues = fractal(seed + 63, 2, 32)

    def meadow(x, y):
        u, v = (x + 0.5) / size, (y + 0.5) / size
        light = 0.92 + 0.16 * (fine(u, v) - 0.5) + 0.12 * (tufts(u, v * 0.6) - 0.5)
        warm = hues(u, v)
        color = (0.23 + 0.06 * warm, 0.37 + 0.03 * warm, 0.14 + 0.02 * warm)
        return [quantize(c * light) for c in color]

    write_png(assets, "Meadow", size, size, meadow, log)

    # Concrete: 4 m square, pale and mottled.
    mottle = fractal(seed + 41, 4, 4)
    pores = value_noise(seed + 42, 128)

    def concrete(x, y):
        u, v = (x + 0.5) / small, (y + 0.5) / small
        tone = 0.6 + 0.12 * (mottle(u, v) - 0.5) + (-0.08 if pores(u, v) > 0.86 else 0.0)
        return [quantize(tone * 1.0), quantize(tone * 0.99), quantize(tone * 0.96)]

    write_png(assets, "Concrete", small, small, concrete, log)

    # Forest: 60 m square of tree crowns seen from afar, dark greens with lighter tops.
    crowns = value_noise(seed + 51, 48)
    canopy = fractal(seed + 52, 3, 4)

    def forest(x, y):
        u, v = (x + 0.5) / small, (y + 0.5) / small
        c = crowns(u, v)
        light = 0.65 + 0.5 * c * c + 0.25 * (canopy(u, v) - 0.5)
        return [quantize(0.13 * light), quantize(0.24 * light), quantize(0.13 * light)]

    write_png(assets, "Forest", small, small, forest, log)

    # Sector boards: "S1", "S2" and "S3" in three cells side by side, white seven-segment figures on dark blue.
    write_png(assets, "SectorBoards", 384, 128, sector_boards(), log)


# Seven segments: a top, b upper right, c lower right, d bottom, e lower left, f upper left, g middle.
FIGURES = {"S": "afgcd", "0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd"}


def segment_rectangles(figure, left, top, width, height, thickness):
    rectangles = []
    middle = top + height / 2.0
    for segment in FIGURES[figure]:
        if segment == "a":
            rectangles.append((left, top, left + width, top + thickness))
        elif segment == "d":
            rectangles.append((left, top + height - thickness, left + width, top + height))
        elif segment == "g":
            rectangles.append((left, middle - thickness / 2.0, left + width, middle + thickness / 2.0))
        elif segment == "b":
            rectangles.append((left + width - thickness, top, left + width, middle + thickness / 2.0))
        elif segment == "c":
            rectangles.append((left + width - thickness, middle - thickness / 2.0, left + width, top + height))
        elif segment == "e":
            rectangles.append((left, middle - thickness / 2.0, left + thickness, top + height))
        elif segment == "f":
            rectangles.append((left, top, left + thickness, middle + thickness / 2.0))
    return rectangles


def sector_boards():
    shapes = []
    for cell, digit in enumerate("123"):
        origin = cell * 128
        shapes += segment_rectangles("S", origin + 22, 22, 36, 84, 11)
        # A one is its right-hand segments only: it stands closer, as wide as its stroke.
        shapes += segment_rectangles(digit, origin + (45 if digit == "1" else 70), 22, 36, 84, 11)

    def pixel(x, y):
        # Four samples per pixel smooth the figures' edges.
        inside = 0
        for dx, dy in ((0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75)):
            px, py = x + dx, y + dy
            if any(l <= px < r and t <= py < b for l, t, r, b in shapes):
                inside += 1
        border = min(x % 128, 127 - x % 128, y, 127 - y) < 5
        background = (0.85, 0.85, 0.85) if border else (0.02, 0.05, 0.16)
        t = inside / 4.0
        return [lerp(background[k], 0.95, t) for k in range(3)]

    return pixel


# --- Props ---------------------------------------------------------------------------------------------------------------
#
# The trackside props Props.py places by the thousand: few meshes and two materials, so that their instances draw in few
# batches. Every mesh but the fence panel is drawn with Props, whose texture (Textures/Props/Props.png) is an atlas of
# flat colors, the marker boards' figures and the advertising boards' names; the fence panel with Fence, a chain-link
# whose holes are cut out (an alpha mask) and which shows both sides. Each mesh stands on the ground at its origin, local
# to its entity; its front (+Z) is what faces the track or the riders coming (Props.py turns it so).

ATLAS_SIZE = 512
CELL = 64
# The atlas's cells of color (8 to a row from the top left, 64 pixels each), with how much the color varies across a
# cell (foliage and bark more than paint), in sRGB.
SWATCHES = (
    ("Rubber", (0.055, 0.055, 0.06), 0.08), ("TyreWhite", (0.86, 0.86, 0.84), 0.04), ("TyreRed", (0.72, 0.09, 0.07), 0.04),
    ("ConeOrange", (0.96, 0.36, 0.05), 0.03), ("White", (0.9, 0.9, 0.89), 0.03), ("Steel", (0.56, 0.58, 0.61), 0.05),
    ("DarkSteel", (0.17, 0.18, 0.2), 0.05), ("Concrete", (0.62, 0.61, 0.58), 0.1), ("Bark", (0.27, 0.19, 0.12), 0.25),
    ("LeafLight", (0.25, 0.4, 0.12), 0.3), ("LeafDeep", (0.14, 0.29, 0.09), 0.3), ("Needles", (0.08, 0.2, 0.11), 0.3),
    ("Shrub", (0.2, 0.33, 0.1), 0.3), ("SeatBlue", (0.07, 0.2, 0.55), 0.04), ("SeatRed", (0.62, 0.08, 0.07), 0.04),
    ("Roof", (0.8, 0.81, 0.82), 0.04), ("Marshal", (0.94, 0.46, 0.07), 0.04), ("Window", (0.05, 0.07, 0.09), 0.02),
    ("Lamp", (1.0, 0.97, 0.86), 0.0), ("BoardBlue", (0.04, 0.11, 0.33), 0.03),
)
SWATCH = {name: index for index, (name, _, _) in enumerate(SWATCHES)}
# The marker boards' figures (meters to the corner), each in a cell after the swatches: white on blue, framed white.
MARKERS = ("100", "200", "300")
# The advertising boards' names, invented: the text, its color and the board's, each a strip as wide as the atlas below
# its cells (64 pixels high: a board 4 m by 0.5 m).
BRANDS = (("VOLTA", (0.96, 0.8, 0.12), (0.05, 0.05, 0.06)), ("KESTREL OIL", (0.95, 0.95, 0.93), (0.74, 0.08, 0.06)),
          ("NORDA TYRES", (0.95, 0.95, 0.93), (0.04, 0.13, 0.42)), ("AURUM", (0.1, 0.34, 0.19), (0.92, 0.91, 0.87)))
BRAND_TOP = 256
# The letters the names use, five pixels by seven.
LETTERS = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "I": ("01110", "00100", "00100", "00100", "00100", "00100", "01110"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    " ": ("00000",) * 7,
}
# The fence's chain-link: its texture repeats every meter; the wires run diagonally every DIAMOND pixels, WIRE wide.
FENCE_SIZE = 128
DIAMOND = 16
WIRE = 2
FENCE_WIDTH = 3.0
FENCE_HEIGHT = 2.6


def cell_origin(cell):
    """The top left pixel of an atlas cell (swatches first, then the markers)."""
    return (cell % 8) * CELL, (cell // 8) * CELL


def swatch_uv(name):
    """The texture coordinate at the middle of a swatch's cell: a face drawn in one color."""
    x, y = cell_origin(SWATCH[name])
    return ((x + CELL / 2.0) / ATLAS_SIZE, (y + CELL / 2.0) / ATLAS_SIZE)


def cell_uv(cell, u, v, inset=4.0):
    """A texture coordinate inside an atlas cell, from u and v in 0..1 across it, kept inset pixels from its edges (the
    smaller images of the atlas blend neighboring cells at their borders)."""
    x, y = cell_origin(cell)
    return ((x + inset + u * (CELL - 2.0 * inset)) / ATLAS_SIZE, (y + inset + v * (CELL - 2.0 * inset)) / ATLAS_SIZE)


def brand_uv(brand, u, v):
    """A texture coordinate on an advertising board's strip, u along the board and v down it, both 0..1."""
    return ((1.0 + u * (ATLAS_SIZE - 2.0)) / ATLAS_SIZE, (BRAND_TOP + brand * CELL + 1.0 + v * (CELL - 2.0)) / ATLAS_SIZE)


def painted_box(mesh, center, size, name, front=None):
    """A box drawn in one swatch's color, flat shaded; front, when given, is the texture coordinates of its +Z face's
    corners (bottom left, bottom right, top right, top left), which shows them instead."""
    hx, hy, hz = (value / 2.0 for value in size)
    cx, cy, cz = center
    uv = swatch_uv(name)
    for normal, points in (
            ((1.0, 0.0, 0.0), [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),
            ((-1.0, 0.0, 0.0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
            ((0.0, 1.0, 0.0), [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),
            ((0.0, -1.0, 0.0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),
            ((0.0, 0.0, 1.0), [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
            ((0.0, 0.0, -1.0), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)])):
        uvs = front if front is not None and normal == (0.0, 0.0, 1.0) else [uv] * 4
        mesh.face([(cx + x, cy + y, cz + z) for x, y, z in points], normal, uvs)


def ring_point(radius, angle, y):
    return (radius * math.cos(angle), y, radius * math.sin(angle))


def frustum(mesh, y0, y1, r0, r1, sides, name, top=False, phase=0.0):
    """The flat-shaded sides of a vertical frustum round the Y axis (a cylinder when the radii are equal, a cone when the
    top's is 0), in one swatch's color, with its top cap when asked."""
    uv = swatch_uv(name)
    for k in range(sides):
        a0 = phase + 2.0 * math.pi * k / sides
        a1 = phase + 2.0 * math.pi * (k + 1) / sides
        middle = (a0 + a1) / 2.0
        slope = (r0 - r1) / (y1 - y0)
        normal = normalize((math.cos(middle), slope, math.sin(middle)))
        points = [ring_point(r0, a0, y0), ring_point(r0, a1, y0), ring_point(r1, a1, y1), ring_point(r1, a0, y1)]
        if r1 <= 1e-6:
            points = points[:3]
        mesh.face(points, normal, [uv] * len(points))
    if top and r1 > 1e-6:
        mesh.face([ring_point(r1, phase + 2.0 * math.pi * k / sides, y1) for k in range(sides)], (0.0, 1.0, 0.0), [uv] * sides)


def disc(mesh, y, inner, outer, sides, name, down=False):
    """A flat ring (a disc when inner is 0) facing up, or down, in one swatch's color."""
    uv = swatch_uv(name)
    normal = (0.0, -1.0 if down else 1.0, 0.0)
    for k in range(sides):
        a0, a1 = 2.0 * math.pi * k / sides, 2.0 * math.pi * (k + 1) / sides
        if inner <= 1e-6:
            mesh.face([(0.0, y, 0.0), ring_point(outer, a0, y), ring_point(outer, a1, y)], normal, [uv] * 3)
        else:
            mesh.face([ring_point(inner, a0, y), ring_point(outer, a0, y), ring_point(outer, a1, y), ring_point(inner, a1, y)],
                      normal, [uv] * 4)


def lump(mesh, center, radii, name, seed):
    """A rough ball of foliage: an icosahedron split once, its corners pushed in and out a little, flat shaded, its
    swatch's cell mapped across it from the side so that the leaves' shades vary over it."""
    generator = random.Random(seed)
    t = (1.0 + math.sqrt(5.0)) / 2.0
    corners = [normalize(p) for p in ((-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t),
                                       (0, 1, -t), (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1))]
    faces = ((0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6),
             (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7),
             (9, 8, 1))
    points = list(corners)
    middles = {}

    def middle(a, b):
        key = (min(a, b), max(a, b))
        if key not in middles:
            middles[key] = len(points)
            points.append(normalize(scale(add(points[a], points[b]), 0.5)))
        return middles[key]

    split = []
    for a, b, c in faces:
        ab, bc, ca = middle(a, b), middle(b, c), middle(c, a)
        split += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
    pushed = [scale(p, 0.86 + 0.28 * generator.random()) for p in points]
    world = [(center[0] + p[0] * radii[0], center[1] + p[1] * radii[1], center[2] + p[2] * radii[2]) for p in pushed]
    cell = SWATCH[name]
    for a, b, c in split:
        triangle = [world[a], world[b], world[c]]
        normal = normalize(cross(subtract(triangle[1], triangle[0]), subtract(triangle[2], triangle[0])))
        if dot(normal, subtract(triangle[0], center)) < 0.0:
            normal = scale(normal, -1.0)
        uvs = [cell_uv(cell, min(1.0, max(0.0, 0.5 + 0.45 * (p[0] - center[0]) / radii[0])),
                       min(1.0, max(0.0, 0.5 - 0.45 * (p[1] - center[1]) / radii[1]))) for p in triangle]
        mesh.face(triangle, normal, uvs)


def build_tyre_stack():
    """Four tyres stacked (60 cm across, 96 cm high), the top one painted white, as they line the walls of a run-off."""
    mesh = MeshBuilder()
    outer, inner, height, sides = 0.3, 0.17, 0.24, 12
    for k in range(4):
        y0 = k * height
        frustum(mesh, y0, y0 + height * 0.5, outer * 0.94, outer, sides, "TyreWhite" if k == 3 else "Rubber")
        frustum(mesh, y0 + height * 0.5, y0 + height, outer, outer * 0.94, sides, "TyreWhite" if k == 3 else "Rubber")
    disc(mesh, 4 * height, inner, outer * 0.94, sides, "TyreWhite")
    # The hole down the middle, dark.
    disc(mesh, 4 * height - 0.06, 0.0, inner, sides, "Rubber")
    return mesh


def build_cone():
    """A traffic cone, 53 cm high: orange with a white band, on a black base."""
    mesh = MeshBuilder()
    painted_box(mesh, (0.0, 0.015, 0.0), (0.36, 0.03, 0.36), "Rubber")
    bands = ((0.03, 0.26, 0.15, 0.085, "ConeOrange"), (0.26, 0.36, 0.085, 0.057, "White"), (0.36, 0.53, 0.057, 0.012, "ConeOrange"))
    for y0, y1, r0, r1, name in bands:
        frustum(mesh, y0, y1, r0, r1, 10, name, top=y1 == 0.53)
    return mesh


def build_marker(cell):
    """A braking marker: a square board (90 cm) on two posts, its figures facing the riders coming (+Z)."""
    mesh = MeshBuilder()
    for x in (-0.3, 0.3):
        painted_box(mesh, (x, 0.5, 0.0), (0.06, 1.0, 0.06), "DarkSteel")
    front = [cell_uv(cell, 0.0, 1.0, 1.0), cell_uv(cell, 1.0, 1.0, 1.0), cell_uv(cell, 1.0, 0.0, 1.0), cell_uv(cell, 0.0, 0.0, 1.0)]
    painted_box(mesh, (0.0, 1.45, 0.05), (0.9, 0.9, 0.04), "DarkSteel", front)
    return mesh


def build_fence_post():
    mesh = MeshBuilder()
    painted_box(mesh, (0.0, FENCE_HEIGHT / 2.0 + 0.1, 0.0), (0.07, FENCE_HEIGHT + 0.2, 0.07), "Steel")
    painted_box(mesh, (0.0, 0.05, 0.0), (0.2, 0.1, 0.2), "Concrete")
    return mesh


def build_fence_panel():
    """A catch fence's panel between two posts: one quad of chain-link, 3 m wide and 2.6 m high, texture coordinates in
    meters (the Fence material shows both sides and cuts out the holes)."""
    mesh = MeshBuilder()
    w = FENCE_WIDTH / 2.0
    mesh.face([(-w, 0.0, 0.0), (w, 0.0, 0.0), (w, FENCE_HEIGHT, 0.0), (-w, FENCE_HEIGHT, 0.0)], (0.0, 0.0, 1.0),
              [(0.0, FENCE_HEIGHT), (FENCE_WIDTH, FENCE_HEIGHT), (FENCE_WIDTH, 0.0), (0.0, 0.0)])
    return mesh


def build_broadleaf(seed):
    """A broadleaf tree about 7 m high: a trunk and a crown of three lumps."""
    mesh = MeshBuilder()
    frustum(mesh, 0.0, 3.4, 0.2, 0.13, 6, "Bark")
    lump(mesh, (0.0, 4.6, 0.0), (2.3, 1.9, 2.3), "LeafLight", seed)
    lump(mesh, (0.9, 5.6, -0.5), (1.6, 1.4, 1.6), "LeafLight", seed + 1)
    lump(mesh, (-0.8, 5.3, 0.7), (1.5, 1.3, 1.5), "LeafDeep", seed + 2)
    return mesh


def build_round_tree(seed):
    """A tree with a round crown, about 6.5 m high."""
    mesh = MeshBuilder()
    frustum(mesh, 0.0, 2.6, 0.17, 0.11, 6, "Bark")
    lump(mesh, (0.0, 4.2, 0.0), (2.4, 2.3, 2.4), "LeafDeep", seed)
    return mesh


def build_conifer():
    """A conifer about 9 m high: a short trunk under three tiers of needles."""
    mesh = MeshBuilder()
    frustum(mesh, 0.0, 1.6, 0.16, 0.12, 6, "Bark")
    for y0, y1, radius, phase in ((1.0, 4.6, 2.1, 0.0), (3.2, 6.8, 1.6, 0.4), (5.4, 9.0, 1.05, 0.8)):
        frustum(mesh, y0, y1, radius, 0.0, 9, "Needles", phase=phase)
        disc(mesh, y0, 0.0, radius, 9, "Needles", down=True)
    return mesh


def build_bush(seed):
    """A low bush, 2 m across and 1.2 m high."""
    mesh = MeshBuilder()
    lump(mesh, (0.0, 0.55, 0.0), (1.0, 0.65, 1.0), "Shrub", seed)
    return mesh


def build_grandstand():
    """A block of a grandstand, 10 m wide: six rows of seats on concrete steps rising from the front (+Z, towards the
    track), a back wall, and a roof on two posts."""
    mesh = MeshBuilder()
    width, rows, depth, rise, first = 10.0, 6, 1.0, 0.5, 0.6
    front = 3.0
    for k in range(rows):
        top = first + rise * k
        z = front - depth * (k + 0.5)
        painted_box(mesh, (0.0, top / 2.0, z), (width, top, depth), "Concrete")
        painted_box(mesh, (0.0, top + 0.15, z - 0.2), (width - 0.4, 0.3, 0.45), "SeatBlue")
    back = front - depth * rows
    wall = first + rise * rows + 2.2
    painted_box(mesh, (0.0, wall / 2.0, back - 0.15), (width, wall, 0.3), "Concrete")
    for x in (-width / 2.0 + 0.2, width / 2.0 - 0.2):
        painted_box(mesh, (x, wall / 2.0, front - 0.3), (0.2, wall, 0.2), "Steel")
    painted_box(mesh, (0.0, wall + 0.08, (front + back) / 2.0 - 0.2), (width + 0.3, 0.16, front - back + 0.9), "Roof")
    return mesh


def build_marshal_post():
    """A marshals' post: an orange hut with a dark window band on a platform 1.2 m up, under a white roof."""
    mesh = MeshBuilder()
    for x in (-0.8, 0.8):
        for z in (-0.8, 0.8):
            painted_box(mesh, (x, 0.6, z), (0.12, 1.2, 0.12), "Steel")
    painted_box(mesh, (0.0, 1.25, 0.0), (2.0, 0.1, 2.0), "Concrete")
    painted_box(mesh, (0.0, 1.75, 0.0), (1.8, 0.9, 1.8), "Marshal")
    painted_box(mesh, (0.0, 2.45, 0.0), (1.82, 0.5, 1.82), "Window")
    painted_box(mesh, (0.0, 2.8, 0.0), (1.8, 0.2, 1.8), "Marshal")
    painted_box(mesh, (0.0, 2.96, 0.0), (2.3, 0.12, 2.3), "Roof")
    return mesh


def build_light_pole():
    """A floodlight pole 11 m high whose lamp leans out over its front (+Z)."""
    mesh = MeshBuilder()
    frustum(mesh, 0.0, 11.0, 0.13, 0.08, 6, "Steel")
    painted_box(mesh, (0.0, 0.1, 0.0), (0.45, 0.2, 0.45), "Concrete")
    painted_box(mesh, (0.0, 10.9, 0.75), (0.1, 0.1, 1.5), "Steel")
    painted_box(mesh, (0.0, 10.88, 1.55), (0.9, 0.2, 0.5), "DarkSteel")
    # The lamp's glass, under its housing.
    painted_box(mesh, (0.0, 10.75, 1.55), (0.84, 0.06, 0.44), "Lamp")
    return mesh


def build_board(brand):
    """An advertising board, 4 m by 0.5 m, on two legs: its name faces the track (+Z), its back is dark."""
    mesh = MeshBuilder()
    bottom, top = 0.9, 1.4
    for x in (-1.5, 1.5):
        painted_box(mesh, (x, bottom / 2.0, -0.06), (0.06, bottom, 0.06), "DarkSteel")
    front = [brand_uv(brand, 0.0, 1.0), brand_uv(brand, 1.0, 1.0), brand_uv(brand, 1.0, 0.0), brand_uv(brand, 0.0, 0.0)]
    painted_box(mesh, (0.0, (bottom + top) / 2.0, 0.0), (4.0, top - bottom, 0.04), "DarkSteel", front)
    return mesh


def prop_meshes(seed):
    """The props' meshes by name (Meshes/Props/<name>.gltf), each one primitive: the fence panel's of Fence, every other's
    of Props."""
    meshes = {
        "TyreStack": build_tyre_stack(), "Cone": build_cone(), "FencePost": build_fence_post(), "FencePanel": build_fence_panel(),
        "Broadleaf": build_broadleaf(seed + 10), "RoundTree": build_round_tree(seed + 20), "Conifer": build_conifer(),
        "Bush": build_bush(seed + 30), "Grandstand": build_grandstand(), "MarshalPost": build_marshal_post(),
        "LightPole": build_light_pole(),
    }
    for index, figures in enumerate(MARKERS):
        meshes[f"Marker{figures}"] = build_marker(len(SWATCHES) + index)
    for index in range(len(BRANDS)):
        meshes[f"Board{index + 1}"] = build_board(index)
    return meshes


# The props' meshes, in the order they are written (prop_meshes).
PROP_MESH_NAMES = ("TyreStack", "Cone", "FencePost", "FencePanel", "Broadleaf", "RoundTree", "Conifer", "Bush", "Grandstand",
                   "MarshalPost", "LightPole") + tuple(f"Marker{figures}" for figures in MARKERS) +     tuple(f"Board{index + 1}" for index in range(len(BRANDS)))
# Each prop mesh's material: the fence panel's Fence, every other's Props (Materials/Props/<name>.smat).
PROP_MATERIALS = ("Props", "Fence")


def prop_material(name):
    return "Fence" if name == "FencePanel" else "Props"


def draw_text(text, left, top, size):
    """The pixels (x, y) the letters of the text cover, each letter's dots size pixels square, a dot apart."""
    covered = set()
    for index, letter in enumerate(text):
        for row, bits in enumerate(LETTERS[letter]):
            for column, bit in enumerate(bits):
                if bit == "1":
                    x0, y0 = left + (index * 6 + column) * size, top + row * size
                    covered.update((x, y) for x in range(x0, x0 + size) for y in range(y0, y0 + size))
    return covered


def make_prop_textures(assets, seed, log):
    # The atlas: noise varies each swatch's color by its amount, the markers' figures and the boards' names.
    grain = fractal(seed + 40, 3, 8)
    figures = []
    for index, text in enumerate(MARKERS):
        left, top = cell_origin(len(SWATCHES) + index)
        figures.append([shape for k, digit in enumerate(text) for shape in segment_rectangles(digit, left + 8 + k * 17, top + 16, 13, 32, 4)])
    names = []
    for index, (text, _, _) in enumerate(BRANDS):
        dot = 6
        width = (len(text) * 6 - 1) * dot
        names.append(draw_text(text, (ATLAS_SIZE - width) // 2, BRAND_TOP + index * CELL + (CELL - 7 * dot) // 2 - 3, dot))

    def atlas(x, y):
        if y >= BRAND_TOP:
            index = (y - BRAND_TOP) // CELL
            _, ink, paper = BRANDS[index]
            local = (y - BRAND_TOP) % CELL
            if (x, y) in names[index]:
                return list(ink)
            # A band of the name's color along the board's foot.
            return list(ink) if local >= CELL - 6 else list(paper)
        cell = (y // CELL) * 8 + x // CELL
        if cell < len(SWATCHES):
            _, color, amount = SWATCHES[cell]
            shade = 1.0 + amount * (grain((x + 0.5) / ATLAS_SIZE, (y + 0.5) / ATLAS_SIZE) - 0.5) * 2.0
            return [quantize(min(1.0, value * shade)) for value in color]
        marker = cell - len(SWATCHES)
        if marker < len(MARKERS):
            left, top = cell_origin(cell)
            frame = min(x - left, left + CELL - 1 - x, y - top, top + CELL - 1 - y) < 4
            inside = any(l <= x + 0.5 < r and t <= y + 0.5 < b for l, t, r, b in figures[marker])
            return [0.92, 0.92, 0.9] if frame or inside else list(SWATCHES[SWATCH["BoardBlue"]][1])
        return [0.5, 0.5, 0.5]

    write_png(assets, "Props/Props", ATLAS_SIZE, ATLAS_SIZE, atlas, log)

    # The chain-link: galvanized wires on the diagonals, the holes between them transparent (their color the wires', so
    # that the smaller images keep the wires' shade as the holes blend in).
    wire = (0.62, 0.64, 0.66)

    def fence(x, y):
        a, b = (x + y) % DIAMOND, (x - y) % DIAMOND
        covered = a < WIRE or b < WIRE
        # Where two wires cross, a little brighter.
        tone = 1.0 if a < WIRE and b < WIRE else 0.9
        return [quantize(value * tone) for value in wire] + [1.0 if covered else 0.0]

    write_png(assets, "Props/Fence", FENCE_SIZE, FENCE_SIZE, fence, log, alpha=True)


def generate_props(assets, seed, log=print):
    """The props' meshes and textures (Meshes/Props, Textures/Props)."""
    meshes = prop_meshes(seed)
    assert tuple(meshes) == PROP_MESH_NAMES, tuple(meshes)
    for name, mesh in meshes.items():
        write_gltf(assets, f"Props/{name}", [(mesh, 0)], (prop_material(name),), log)
    make_prop_textures(assets, seed, log)


# --- The sky -------------------------------------------------------------------------------------------------------------

def sky_radiance(direction, sun):
    """Linear radiance of the sky in a direction: a blue zenith over a pale, hazy horizon, the sun's glow, and hazy land
    below the horizon."""
    up = direction[1]
    zenith = (0.11, 0.29, 0.85)
    horizon = (0.7, 0.8, 0.93)
    cosine = max(-1.0, min(1.0, dot(direction, sun)))
    angle = math.acos(cosine)
    glow = 4.0 * math.exp(-angle / 0.05) + 0.55 * math.exp(-angle / 0.35)
    if up >= 0.0:
        t = up ** 0.45
        color = [lerp(horizon[k], zenith[k], t) for k in range(3)]
        haze = math.exp(-up / 0.08)
        color = [c + 0.12 * haze for c in color]
        warm = (1.0, 0.9, 0.72)
        return [color[k] + glow * warm[k] * (0.4 + 0.6 * (1.0 - t)) for k in range(3)]
    land = (0.13, 0.15, 0.1)
    t = min(1.0, -up / 0.06)
    return [lerp(horizon[k] * 0.85, land[k], t) for k in range(3)]


def write_hdr(assets, name, width, height, log):
    """Writes Environments/<name>.hdr: an equirectangular sky (u = 0.5 looks down -Z, v = 0 straight up), RGBE with
    run-length encoded scanlines."""
    sun = sun_direction()
    lines = []
    for y in range(height):
        elevation = math.pi * (y + 0.5) / height
        dy = math.cos(elevation)
        ring = math.sin(elevation)
        channels = ([], [], [], [])
        for x in range(width):
            phi = ((x + 0.5) / width - 0.5) * 2.0 * math.pi
            direction = (ring * math.sin(phi), dy, -ring * math.cos(phi))
            rgbe = to_rgbe(sky_radiance(direction, sun))
            for k in range(4):
                channels[k].append(rgbe[k])
        line = bytearray((2, 2, width >> 8, width & 0xFF))
        for channel in channels:
            line += run_length(channel)
        lines.append(bytes(line))
    path = os.path.join(assets, "Environments", f"{name}.hdr")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as file:
        file.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n" + f"-Y {height} +X {width}\n".encode() + b"".join(lines))
    log(f"Environments/{name}.hdr: {width}x{height}")


def to_rgbe(color):
    largest = max(color)
    if largest < 1e-32:
        return (0, 0, 0, 0)
    mantissa, exponent = math.frexp(largest)
    factor = mantissa * 256.0 / largest
    return tuple(max(0, min(255, int(c * factor))) for c in color) + (exponent + 128,)


def run_length(values):
    """Radiance's run-length encoding of one channel of a scanline: runs of 3 to 127 equal bytes, literals of up to 128."""
    encoded = bytearray()
    index = 0
    count = len(values)
    while index < count:
        run = 1
        while index + run < count and run < 127 and values[index + run] == values[index]:
            run += 1
        if run >= 3:
            encoded += bytes((128 + run, values[index]))
            index += run
            continue
        start = index
        while index < count and index - start < 128:
            if index + 2 < count and values[index] == values[index + 1] == values[index + 2]:
                break
            index += 1
        encoded += bytes((index - start,)) + bytes(values[start:index])
    return bytes(encoded)


# --- Everything ----------------------------------------------------------------------------------------------------------

def generate_circuit(assets, seed, log=print):
    """The circuit's meshes, textures and sky."""
    log(Circuit.check())
    drawn, collision = build_track()
    write_gltf(assets, "Track", drawn, TRACK_MATERIALS, log)
    write_gltf(assets, "TrackCollision", collision, SURFACES, log)
    barriers, barrier_collision = build_barriers(Circuit.sections(spacing=2.5))
    write_gltf(assets, "Barriers", barriers, ("Concrete",), log)
    write_gltf(assets, "BarrierCollision", [(barrier_collision, 0)], ("Collision",), log)
    write_gltf(assets, "Ground", build_ground(), GROUND_MATERIALS, log)
    write_gltf(assets, "Hills", build_hills(seed), HILLS_MATERIALS, log)
    for name, (parts, materials) in (("Gantry", build_gantry()), ("Gate", build_gate()), ("Footbridge", build_footbridge())):
        write_gltf(assets, name, parts, materials, log)
    make_textures(assets, seed, log)
    write_hdr(assets, "Sky", 1024, 512, log)


# What each group of generate() writes (Author.py's circuit and props parts each write theirs), relative to Assets.
GROUPS = {"circuit": generate_circuit, "props": generate_props}
OUTPUTS = {
    "circuit": tuple([f"Meshes/{name}.gltf" for name in ("Track", "TrackCollision", "Barriers", "BarrierCollision", "Ground",
                                                         "Hills", "Gantry", "Gate", "Footbridge")] +
                     [f"Textures/{name}.png" for name in ("Asphalt", "TimingLines", "Grass", "Meadow", "Gravel", "Kerb",
                                                          "Concrete", "Forest", "SectorBoards")] +
                     ["Environments/Sky.hdr"]),
    "props": tuple([f"Meshes/Props/{name}.gltf" for name in PROP_MESH_NAMES] + ["Textures/Props/Props.png", "Textures/Props/Fence.png"]),
}
ALL_OUTPUTS = OUTPUTS["circuit"] + OUTPUTS["props"]


def generate(assets, seed, log=print, groups=tuple(GROUPS)):
    """Writes the files of the groups named (by default all of them) into the assets folder."""
    for group in groups:
        GROUPS[group](assets, seed, log)


def png_pixels(path):
    """The decompressed image data of a PNG (its IDAT chunks inflated), to compare pixels whatever the compression."""
    with open(path, "rb") as file:
        data = file.read()
    position, compressed = 8, b""
    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        if kind in (b"IHDR", b"IDAT"):
            compressed += data[position + 8:position + 8 + length] if kind == b"IDAT" else b""
        position += 12 + length
    return data[8:33], zlib.decompress(compressed)


def check(seed):
    """Generates twice into temporary folders, then compares the runs with each other and with Assets."""
    folders = [tempfile.mkdtemp(prefix="moto-assets-") for _ in range(2)]
    try:
        for folder in folders:
            generate(folder, seed, log=lambda line: None)
        problems = []
        for path in ALL_OUTPUTS:
            first, second = (os.path.join(folder, *path.split("/")) for folder in folders)
            if not filecmp.cmp(first, second, shallow=False):
                problems.append(f"{path}: two runs with seed {seed} differ")
        print(f"two runs with seed {seed}: {'the same bytes' if not problems else 'different'} ({len(ALL_OUTPUTS)} files)")
        if seed == SEED:
            assets = os.path.join(PROJECT, "Assets")
            for path in ALL_OUTPUTS:
                fresh, committed = os.path.join(folders[0], *path.split("/")), os.path.join(assets, *path.split("/"))
                if not os.path.exists(committed):
                    problems.append(f"{path}: missing from Assets")
                elif not filecmp.cmp(fresh, committed, shallow=False):
                    if path.endswith(".png") and png_pixels(fresh) == png_pixels(committed):
                        print(f"{path}: the same pixels, compressed differently by this zlib")
                    else:
                        problems.append(f"{path}: differs from Assets")
            if not any("Assets" in problem for problem in problems):
                print("Assets holds what the generator makes")
        if problems:
            raise SystemExit("\n".join(problems))
    finally:
        for folder in folders:
            shutil.rmtree(folder, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description="Generates the Moto circuit's meshes, textures and sky.")
    parser.add_argument("--seed", type=int, default=SEED, help=f"the seed of the textures' noise and the hills (default {SEED})")
    parser.add_argument("--output", help="the folder to write into instead of the project's Assets")
    parser.add_argument("--check", action="store_true", help="generate twice and compare the runs and Assets, writing nothing")
    arguments = parser.parse_args()
    if arguments.check:
        check(arguments.seed)
    else:
        generate(os.path.abspath(arguments.output) if arguments.output else os.path.join(PROJECT, "Assets"), arguments.seed)


if __name__ == "__main__":
    main()

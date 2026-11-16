"""Moto's props: about 10,000 static props placed by seed from the layout in Circuit.py, on the meshes GenerateAssets.py
makes (Meshes/Props), so that few meshes and two materials draw them all as instances in few batches.

Along the circuit: tyre stacks lining the walls round the corners' outsides and cones at their insides (the only props on
the circuit's side of a wall, in the strip of grass at its foot, where nothing runs wide); braking markers 300, 200 and
100 m before the corners that follow a straight; catch fences of posts and chain-link panels behind the walls (all round
the outside of the lap, and on the inside of the main and back straights); advertising boards with invented names
between the walls and the fences along the straights; grandstands along the main straight and round Turns 1 and 6;
marshals' posts at the corners' exits; floodlight poles along the main straight. Beyond them: bushes, and clumps of
trees across the infield and the land round the circuit.

    python Projects/Moto/Tools/Props.py

prints the figures and runs check(): no prop on the road, a kerb, a verge, a gravel trap or a skirt of any part of the
circuit (only the stacks and cones stand at a wall's foot, in the last strip of the skirt), none on a structure's legs,
piers or stairs, none where the camera tour flies, and none in another prop's way.
"""
import bisect
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Circuit  # noqa: E402  (the layout)
import GenerateAssets  # noqa: E402  (the props' meshes)
import Tour  # noqa: E402  (the camera tour)

SEED = GenerateAssets.SEED
# How many props there are: the trees fill up to it.
COUNT = 10000
# Values are rounded: positions and scales to 1/64 m, so the scene file holds short numbers that every machine writes
# alike; angles to a hundredth of a degree.
GRID = 64.0

# The groups the props are made in (Author.py's props part: one entity per group under Props, one per prop under it),
# each with its props' name and meshes.
GROUPS = (
    ("Tyre Stacks", "Tyre Stack", ("TyreStack",)),
    ("Cones", "Cone", ("Cone",)),
    ("Braking Markers", "Marker", tuple(f"Marker{figures}" for figures in GenerateAssets.MARKERS)),
    ("Fence Posts", "Fence Post", ("FencePost",)),
    ("Fence Panels", "Fence Panel", ("FencePanel",)),
    ("Advertising Boards", "Board", tuple(f"Board{index + 1}" for index in range(len(GenerateAssets.BRANDS)))),
    ("Grandstands", "Grandstand", ("Grandstand",)),
    ("Marshal Posts", "Marshal Post", ("MarshalPost",)),
    ("Light Poles", "Light Pole", ("LightPole",)),
    ("Bushes", "Bush", ("Bush",)),
    ("Trees", "Tree", ("Broadleaf", "RoundTree", "Conifer")),
)
GROUP_OF = {mesh: group for group, _, meshes in GROUPS for mesh in meshes}

# Where the props stand from the walls (meters from a wall's face, outwards): the fence's posts, the boards in front of
# them, the floodlights and the marshals' posts behind them, and how far beyond the fence the grandstands' fronts, the
# bushes and the trees keep.
BEHIND_WALL = Circuit.BARRIER_BASE
FENCE = BEHIND_WALL + 1.4
BOARDS = BEHIND_WALL + 0.45
POLES = FENCE + 2.1
MARSHALS = FENCE + 2.6
STANDS = FENCE + 1.5
BUSHES = (FENCE + 2.5, FENCE + 9.0)
TREES = FENCE + 9.0
# The strip at a wall's foot where the stacks and cones stand: the gap between the skirt and the wall, and the last half
# meter of the skirt (a few centimeters high there).
FOOT = Circuit.BARRIER_GAP + 0.5
# Along the lap, how far from each structure no prop stands by the circuit (meters): the start gantry and the gates
# straddle the road on legs 14.6 m out, the footbridge's piers and stairs reach 28 m out on both sides.
STRUCTURES = ((0.0, 7.0),) + tuple((gate, 6.0) for gate in Circuit.SECTOR_GATES if gate != 0.0) + ((Circuit.FOOTBRIDGE, 9.0),)
# The structures' footprints for check() (lap distance, half their length along the lap, lateral extent from and to on
# both sides): the gantry's and the gates' legs and feet, the footbridge's piers and stairs.
FOOTPRINTS = ((0.0, 0.6, Circuit.LEG_OFFSET - 0.6, Circuit.LEG_OFFSET + 0.6),) + \
    tuple((gate, 0.45, Circuit.LEG_OFFSET - 0.45, Circuit.LEG_OFFSET + 0.45) for gate in Circuit.SECTOR_GATES if gate != 0.0) + \
    ((Circuit.FOOTBRIDGE, 1.6, Circuit.PIER_OFFSET - 0.3, 28.0),)
# Every prop keeps this far from the tour's camera (Tour.CLEARANCE: the camera has no collider).
TOUR_CLEARANCE = Tour.CLEARANCE


class Prop:
    """A prop: its mesh, where it stands (world), its yaw (degrees about +Y; 0 turns its front, +Z, to +Z) and scale."""

    def __init__(self, mesh, position, yaw, size=(1.0, 1.0, 1.0)):
        self.mesh = mesh
        self.position = tuple(round(value * GRID) / GRID + 0.0 for value in position)
        self.yaw = round(math.remainder(yaw, 360.0), 2) + 0.0
        self.scale = tuple(round(value * GRID) / GRID for value in size)

    def footprint(self):
        """What it covers seen from above, a rectangle turned with it, and its top."""
        (x0, x1, z0, z1), top = BOUNDS[self.mesh]
        sx, sy, sz = self.scale
        yaw = math.radians(self.yaw)
        u = (math.cos(yaw), -math.sin(yaw))
        v = (math.sin(yaw), math.cos(yaw))
        mx, mz = (x0 + x1) / 2.0 * sx, (z0 + z1) / 2.0 * sz
        center = (self.position[0] + mx * u[0] + mz * v[0], self.position[2] + mx * u[1] + mz * v[1])
        return Footprint(center, u, v, (x1 - x0) / 2.0 * sx, (z1 - z0) / 2.0 * sz), top * sy


class Footprint:
    """A rectangle on the ground: its center (x, z), its two directions (unit, at right angles) and half its size along
    each."""

    def __init__(self, center, u, v, half_u, half_v):
        self.center, self.u, self.v, self.half_u, self.half_v = center, u, v, half_u, half_v

    def reach(self, axis):
        """Half its extent along a unit direction."""
        return self.half_u * abs(self.u[0] * axis[0] + self.u[1] * axis[1]) + self.half_v * abs(self.v[0] * axis[0] + self.v[1] * axis[1])

    def radius(self):
        return math.hypot(self.half_u, self.half_v)

    def shrunk(self, amount):
        return Footprint(self.center, self.u, self.v, max(0.0, self.half_u - amount), max(0.0, self.half_v - amount))

    def overlaps(self, other, margin=0.0):
        """Whether the two rectangles come closer than the margin (separating axes)."""
        dx, dz = other.center[0] - self.center[0], other.center[1] - self.center[1]
        for axis in (self.u, self.v, other.u, other.v):
            if abs(dx * axis[0] + dz * axis[1]) >= self.reach(axis) + other.reach(axis) + margin:
                return False
        return True

    def distance(self, x, z):
        """How far a point lies from the rectangle (0 inside it)."""
        dx, dz = x - self.center[0], z - self.center[1]
        a = max(0.0, abs(dx * self.u[0] + dz * self.u[1]) - self.half_u)
        b = max(0.0, abs(dx * self.v[0] + dz * self.v[1]) - self.half_v)
        return math.hypot(a, b)


def mesh_bounds(mesh):
    """A mesh's extent seen from above (x from and to, z from and to) and its top."""
    xs = [p[0] for p in mesh.positions]
    zs = [p[2] for p in mesh.positions]
    return (min(xs), max(xs), min(zs), max(zs)), max(p[1] for p in mesh.positions)


BOUNDS = {name: mesh_bounds(mesh) for name, mesh in GenerateAssets.prop_meshes(SEED).items()}


def facing(direction):
    """The yaw that turns a prop's front (+Z) towards a horizontal direction."""
    return math.degrees(math.atan2(direction[0], direction[2]))


def along_yaw(s):
    """The yaw that turns a prop's +Z against the travel at the lap distance (its -Z along it): its front faces the riders
    coming."""
    return Circuit.yaw(s)


def side_sign(side):
    return 1.0 if side == "right" else -1.0


def face(s, side):
    """The lateral distance of the wall's face from the center line on the side."""
    return Circuit.barrier_offset(s % Circuit.LENGTH, side)


def envelope(s, side):
    """The farthest the wall's face reaches on the side within ENVELOPE meters along the lap: where a gravel trap widens
    or narrows the wall swings out or in steeply, and a line beyond it keeps clear of it seen from the whole stretch."""
    return max(face(s + k, side) for k in range(-ENVELOPE, ENVELOPE + 1))


# How far along the lap a line beyond the wall looks for the wall's widest (meters).
ENVELOPE = 12


def outward(s, side, beyond):
    """The world point beyond the wall's face on the side (meters outwards from the face; negative is in front of it, at
    the wall's own face there, otherwise beyond its envelope)."""
    wall = face(s, side) if beyond < 0.0 else envelope(s, side)
    return Circuit.offset_point(s % Circuit.LENGTH, side_sign(side) * (wall + beyond))


def towards_track(s, side):
    """The horizontal direction from the side towards the center line."""
    _, _, right = Circuit.frame(s % Circuit.LENGTH)
    sign = -side_sign(side)
    return (sign * right[0], 0.0, sign * right[2])


def near_structure(s, margin=0.0):
    return any(abs(math.remainder(s - at, Circuit.LENGTH)) < reach + margin for at, reach in STRUCTURES)


def segments_of(kind):
    """The (start, end, segment) of the lap's straights or corners."""
    found = []
    for index, segment in enumerate(Circuit.SEGMENTS):
        if isinstance(segment, kind):
            found.append((Circuit.STARTS[index], Circuit.STARTS[index] + segment.length, segment))
    return found


def straights():
    """The straights as (start, end, name), the main straight's two pieces (each side of the line) joined."""
    spans = [(start, end, segment.name) for start, end, segment in segments_of(Circuit.Straight)]
    first, last = spans[0], spans[-1]
    joined = (last[0] - Circuit.LENGTH, first[1], first[2])
    return [joined] + spans[1:-1]


def corners():
    """The corners as (start, end, corner, inside, outside), the sides named."""
    found = []
    for corner, start, end, turn in Circuit.corner_ranges():
        inside, outside = ("right", "left") if turn > 0 else ("left", "right")
        found.append((start, end, corner, inside, outside))
    return found


def line_along(start, end, side, beyond, spacing):
    """Points along a line a distance beyond the wall's face on the side, every spacing meters of the line itself (round a
    corner's outside it is longer than the center line), from the lap distance start to end; each as (s, point). No point
    lies near a structure: a line that meets one goes on after it."""
    points = []
    step = 0.25
    s = start
    last = None
    travelled = 0.0
    while s <= end:
        point = outward(s, side, beyond)
        if near_structure(s):
            last = None
        elif last is None:
            points.append((s, point))
            last, travelled = point, 0.0
        else:
            travelled += math.dist(point, last)
            last = point
            if travelled >= spacing:
                points.append((s, point))
                travelled = 0.0
        s += step
    return points


# --- Placement -------------------------------------------------------------------------------------------------------------

def place_tyres(props):
    """A row of tyre stacks at the wall's foot round each corner's outside, from before its gravel trap to its end."""
    for start, end, corner, inside, outside in corners():
        for s, point in line_along(start - 12.0, end + 30.0, outside, -0.33, 0.62):
            props.append(Prop("TyreStack", point, facing(towards_track(s, outside))))


def place_cones(props):
    """Cones at the wall's foot inside each corner round its apex, and a few where its tyres end."""
    for start, end, corner, inside, outside in corners():
        apex = (start + end) / 2.0
        for s, point in line_along(apex - 7.0, apex + 7.0, inside, -0.3, 1.75):
            props.append(Prop("Cone", point, along_yaw(s)))
        for s, point in line_along(end + 31.0, end + 37.0, outside, -0.3, 1.5):
            props.append(Prop("Cone", point, along_yaw(s)))


def place_markers(props):
    """Braking markers behind the wall on a corner's outside, 300, 200 and 100 m before it, where a straight comes before
    it; their figures face the riders coming."""
    spans = straights()
    for start, end, corner, inside, outside in corners():
        before = next(span for span in spans if abs(math.remainder(span[1] - start, Circuit.LENGTH)) < 1e-6)
        for figures in GenerateAssets.MARKERS:
            s = start - float(figures)
            if s >= before[0] + 10.0 and not near_structure(s, 2.0):
                props.append(Prop(f"Marker{figures}", outward(s, outside, BEHIND_WALL + 0.5), along_yaw(s)))


def fence_sides(start, end):
    """The sides a straight's or a corner's catch fence stands on: the lap's outside everywhere, its inside along the main
    and back straights too."""
    return ("left", "right") if Circuit.segment_at((start + end) / 2.0)[0] in FENCED_BOTH else ("left",)


FENCED_BOTH = {index for index, segment in enumerate(Circuit.SEGMENTS) if segment.name in ("Main straight", "Back straight")}


def place_fences(props):
    """The catch fences: a post every 3 m along a line behind the wall, a panel between each two, turned along their chord
    and stretched to it."""
    spans = [(start, end) for start, end, _ in straights()] + [(start, end) for start, end, *_ in corners()]
    for start, end in spans:
        for side in fence_sides(start, end):
            posts = line_along(start, end - 0.25, side, FENCE, GenerateAssets.FENCE_WIDTH)
            for (s0, p0), (s1, p1) in zip(posts, posts[1:]):
                chord = math.dist(p0, p1)
                if chord > GenerateAssets.FENCE_WIDTH * 1.1:
                    continue
                middle = ((p0[0] + p1[0]) / 2.0, 0.0, (p0[2] + p1[2]) / 2.0)
                direction = ((p1[0] - p0[0]) / chord, 0.0, (p1[2] - p0[2]) / chord)
                props.append(Prop("FencePanel", middle, facing((-direction[2], 0.0, direction[0])),
                                  (chord / GenerateAssets.FENCE_WIDTH, 1.0, 1.0)))
            for s, point in posts:
                props.append(Prop("FencePost", point, along_yaw(s)))


def place_boards(props, generator):
    """Advertising boards between the wall and the fence along the straights, facing the track, in runs of one name."""
    for start, end, name in straights():
        sides = ("left", "right") if name in ("Main straight", "Back straight") else ("left",)
        for side in sides:
            points = line_along(start + 15.0, end - 15.0, side, BOARDS, 4.3)
            brand = generator.randrange(len(GenerateAssets.BRANDS))
            for index, (s, point) in enumerate(points):
                if index % 5 == 0:
                    brand = (brand + 1 + generator.randrange(len(GenerateAssets.BRANDS) - 1)) % len(GenerateAssets.BRANDS)
                props.append(Prop(f"Board{brand + 1}", point, facing(towards_track(s, side))))


def place_stands(props):
    """Grandstands of a few blocks each behind the fence, facing the track: two along the main straight's inside, before
    and after the line, one round the outside of Turn 1 and one round the middle of Turn 6's (a stand longer than a few
    blocks repeats itself)."""
    rows = [(-150.0, -45.0, "right"), (20.0, 125.0, "right")]
    for start, end, corner, inside, outside in corners():
        middle = (start + end) / 2.0
        if corner.name in ("Turn 1", "Turn 6"):
            rows.append((middle - 32.0, middle + 32.0, outside))
    for start, end, side in rows:
        for s, point in line_along(start, end, side, STANDS + 3.0, 10.6):
            if not near_structure(s, 8.0):
                props.append(Prop("Grandstand", point, facing(towards_track(s, side))))


def place_marshals(props):
    """A marshals' post behind the fence at each corner's exit, on its outside."""
    for start, end, corner, inside, outside in corners():
        s = end + 12.0
        if not near_structure(s, 4.0):
            props.append(Prop("MarshalPost", outward(s, outside, MARSHALS), facing(towards_track(s, outside))))


def place_poles(props):
    """Floodlights behind the fence every 30 m along the main straight, both sides, leaning out over the track."""
    for start, end, name in straights():
        if name == "Main straight":
            for side in ("left", "right"):
                for s, point in line_along(start + 20.0, end - 20.0, side, POLES, 30.0):
                    props.append(Prop("LightPole", point, facing(towards_track(s, side))))


def place_bushes(props, generator, count):
    """Bushes scattered behind the fences round the lap, both sides."""
    for _ in range(count):
        s = generator.uniform(0.0, Circuit.LENGTH)
        side = generator.choice(("left", "right"))
        beyond = generator.uniform(*BUSHES)
        size = generator.uniform(0.7, 1.5)
        props.append(Prop("Bush", outward(s, side, beyond), generator.uniform(0.0, 360.0), (size, generator.uniform(0.7, 1.3) * size, size)))


def place_trees(props, generator, count, checker):
    """Clumps of trees, each mostly of one kind, until there are count: most of them near the circuit (from just beyond
    its fences to a few hundred meters out, the infield included), the rest across the land round it. The checker says
    where a tree may stand, and takes each tree placed."""
    cx, cz = GenerateAssets.circuit_center()
    trees = []
    attempts = 0
    while len(trees) < count:
        attempts += 1
        assert attempts < 20000, f"only {len(trees)} of {count} trees found room"
        if generator.random() < 0.6:
            s = generator.uniform(0.0, Circuit.LENGTH)
            center = outward(s, generator.choice(("left", "right")), generator.uniform(PLANTS_BEYOND + 15.0, 260.0))
        else:
            center = (cx + generator.uniform(-750.0, 750.0), 0.0, cz + generator.uniform(-650.0, 650.0))
        if checker.problems(Prop("Broadleaf", center, 0.0, (2.0, 1.0, 2.0))):
            continue
        spread = generator.uniform(10.0, 40.0)
        kinds = ("Broadleaf", "RoundTree", "Conifer")
        main = generator.choice(kinds)
        for _ in range(generator.randint(15, 90)):
            if len(trees) >= count:
                break
            point = (center[0] + generator.gauss(0.0, spread), 0.0, center[2] + generator.gauss(0.0, spread))
            kind = main if generator.random() < 0.75 else generator.choice(kinds)
            size = generator.uniform(0.75, 1.35)
            tree = Prop(kind, point, generator.uniform(0.0, 360.0), (size, size * generator.uniform(0.85, 1.2), size))
            if not checker.problems(tree):
                checker.add(tree)
                trees.append(tree)
                props.append(tree)


def place(seed=SEED):
    """Every prop, in its groups' order: [Prop]. The same seed gives the same props."""
    generator = random.Random(seed)
    checker = Checker()
    props = []
    for step in (place_tyres, place_cones, place_markers, place_fences):
        step(props)
    place_boards(props, generator)
    for step in (place_stands, place_marshals, place_poles):
        step(props)
    # What a wall, a structure, the tour or a prop before it leaves no room for is left out.
    kept = []
    for prop in props:
        if not checker.problems(prop):
            kept.append(prop)
            checker.add(prop)
    bushes = []
    place_bushes(bushes, generator, 900)
    for prop in bushes:
        if not checker.problems(prop):
            kept.append(prop)
            checker.add(prop)
    place_trees(kept, generator, COUNT - len(kept), checker)
    order = {mesh: index for index, (_, _, meshes) in enumerate(GROUPS) for mesh in meshes}
    kept.sort(key=lambda prop: order[prop.mesh])
    return kept


# --- Checks ----------------------------------------------------------------------------------------------------------------

# Props that stand at a wall's foot (the strip FOOT); props in rows whose members may touch (a fence's posts and panels,
# the stacks of a row, the boards of a run, the cones of a line); and the plants, whose crowns may reach into each other's
# a little.
AT_WALL = ("TyreStack", "Cone")
IN_ROWS = ("TyreStack", "Cone", "FencePost", "FencePanel") + GROUPS[5][2]
PLANTS = ("Broadleaf", "RoundTree", "Conifer", "Bush")
# How far beyond a wall's face the plants keep (meters): clear of the fence.
PLANTS_BEYOND = FENCE + 0.5


class Checker:
    """Finds what keeps a prop from where it stands: the circuit's surfaces (seen from every part of the lap it lies
    beside, not only the one it was placed by), the structures, the tour's camera and the props already added."""

    CELL = 16.0

    def __init__(self):
        self.step = 1.0
        self.samples = []
        s = 0.0
        while s < Circuit.LENGTH:
            point, forward, right = Circuit.frame(s)
            self.samples.append((s, point, (forward[0], forward[2]), (right[0], right[2]), face(s, "left"), face(s, "right")))
            s += self.step
        self.sample_cells = {}
        for index, sample in enumerate(self.samples):
            self.sample_cells.setdefault(self.cell(sample[1][0], sample[1][2]), []).append(index)
        self.reach = max(max(sample[4], sample[5]) for sample in self.samples) + Circuit.BARRIER_BASE
        self.structures = []
        for at, half, inner, outer in FOOTPRINTS:
            point, forward, right = Circuit.frame(at % Circuit.LENGTH)
            for sign in (-1.0, 1.0):
                middle = sign * (inner + outer) / 2.0
                center = (point[0] + middle * right[0], point[2] + middle * right[2])
                self.structures.append((at, Footprint(center, (right[0], right[2]), (forward[0], forward[2]), (outer - inner) / 2.0,
                                                      half)))
        self.camera = []
        t = 0.0
        while t <= Tour.LENGTH:
            self.camera.append(Tour.camera(t)[0])
            t += 0.05
        self.camera_cells = {}
        for index, eye in enumerate(self.camera):
            self.camera_cells.setdefault(self.cell(eye[0], eye[2]), []).append(index)
        self.props = []
        self.footprints = []
        self.prop_cells = {}

    def cell(self, x, z):
        return (int(math.floor(x / self.CELL)), int(math.floor(z / self.CELL)))

    def around(self, cells, x, z, reach):
        cx, cz = self.cell(x, z)
        span = int(math.ceil(reach / self.CELL))
        for dx in range(-span, span + 1):
            for dz in range(-span, span + 1):
                yield from cells.get((cx + dx, cz + dz), ())

    def add(self, prop):
        footprint, _ = prop.footprint()
        self.props.append(prop)
        self.footprints.append(footprint)
        self.prop_cells.setdefault(self.cell(*footprint.center), []).append(len(self.props) - 1)

    def problems(self, prop):
        """What keeps the prop from where it stands (an empty list when nothing does)."""
        footprint, height = prop.footprint()
        x, z = footprint.center
        radius = footprint.radius()
        found = []
        cx, cz = GenerateAssets.circuit_center()
        if math.hypot(x - cx, z - cz) + radius > GenerateAssets.GROUND_RADIUS - 100.0:
            found.append("beyond the meadow")
        keep = PLANTS_BEYOND if prop.mesh in PLANTS else Circuit.BARRIER_BASE
        for index in self.around(self.sample_cells, x, z, self.reach + keep + radius + self.step):
            s, point, forward, right, left_wall, right_wall = self.samples[index]
            dx, dz = x - point[0], z - point[2]
            if abs(dx * forward[0] + dz * forward[1]) > self.step / 2.0 + footprint.reach(forward):
                continue
            lateral = dx * right[0] + dz * right[1]
            across = footprint.reach(right)
            near, far = abs(lateral) - across, abs(lateral) + across
            if near > self.reach + keep:
                continue
            wall = right_wall if lateral > 0.0 else left_wall
            beyond = near >= wall + keep - 1e-6
            at_foot = prop.mesh in AT_WALL and near >= wall - FOOT - 1e-6 and far <= wall + 1e-6
            if not beyond and not at_foot:
                found.append(f"on the circuit at {s:.0f} m ({near:.2f} m out, the wall at {wall:.2f} m)")
                break
        for at, structure in self.structures:
            if footprint.overlaps(structure, 1.0):
                found.append(f"on the structure at {at:.0f} m")
        for index in self.around(self.camera_cells, x, z, radius + TOUR_CLEARANCE):
            eye = self.camera[index]
            if eye[1] < height + TOUR_CLEARANCE and footprint.distance(eye[0], eye[2]) < TOUR_CLEARANCE:
                found.append(f"in the tour's way at ({eye[0]:.1f}, {eye[1]:.1f}, {eye[2]:.1f})")
                break
        for index in self.around(self.prop_cells, x, z, radius + 20.0):
            other = self.props[index]
            if prop.mesh in IN_ROWS and other.mesh in IN_ROWS:
                continue
            mine, theirs = footprint, self.footprints[index]
            if prop.mesh in PLANTS and other.mesh in PLANTS:
                mine = mine.shrunk(0.4 * min(mine.half_u, mine.half_v))
                theirs = theirs.shrunk(0.4 * min(theirs.half_u, theirs.half_v))
            if mine.overlaps(theirs):
                found.append(f"in the way of a {other.mesh} at ({other.position[0]:.1f}, {other.position[2]:.1f})")
                break
        return found


def check(props=None):
    """Asserts that every prop stands clear (Checker) and that there are COUNT; returns the figures."""
    props = place() if props is None else props
    assert len(props) == COUNT, f"{len(props)} props"
    checker = Checker()
    for prop in props:
        problems = checker.problems(prop)
        assert not problems, f"{prop.mesh} at {prop.position}: {problems}"
        checker.add(prop)
    counts = {}
    for prop in props:
        group = GROUP_OF[prop.mesh]
        counts[group] = counts.get(group, 0) + 1
    return f"{len(props)} props: " + ", ".join(f"{count} {group.lower()}" for group, count in counts.items())


if __name__ == "__main__":
    print(check())

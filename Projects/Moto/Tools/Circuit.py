"""The Moto circuit's layout, the one source of its geometry: GenerateAssets.py builds the meshes from it and Author.py
places the scene's structures and cameras with it.

The circuit is a closed path of straights and arcs on flat ground, raced clockwise seen from above. The lap distance s
runs from 0 at the start and finish line (Sector Gate 3, under the start gantry) to LENGTH. World axes: +X east, +Y up,
-Z north; meters.

Across the path, from the center line outwards on each side (GenerateAssets.cross_section builds it):
- the road (asphalt, HALF_WIDTH each side), crowned for drainage: it falls CROWN in a straight line from the center line
  to each edge (road_height());
- the kerb strip (KERB_WIDTH): a raised red and white kerb along the corners (kerb()), grass elsewhere;
- the verge (VERGE_WIDTH of grass), then the gravel trap where one lies on the outside of a corner (its width ramps up
  and down along the lap), then the skirt, grass sloping down to the ground, and the barrier, a concrete wall.

`check()` asserts what the rest relies on: the length, ten corners, a closed and smooth seam, corners wide enough for
the barrier on their inside, and no two parts of the circuit (barriers included) closer than CLEARANCE.
"""
import bisect
import math

# --- The cross-section (meters) ------------------------------------------------------------------------------------------

HALF_WIDTH = 6.0
KERB_WIDTH = 1.2
VERGE_WIDTH = 3.0
SKIRT_WIDTH = 2.5
# The gap between the skirt's foot and the barrier's face, the barrier's base and top widths and its height. Its face
# leans back, rising over 0.2 m to its top.
BARRIER_GAP = 0.5
BARRIER_BASE = 0.6
BARRIER_TOP = 0.2
BARRIER_HEIGHT = 0.85
# Heights above the ground (y = 0): the road's edge and its crown at the center line, the kerb's top, the verge's outer
# edge and the gravel. The skirt slopes from the verge (or gravel) down to the ground.
ROAD_HEIGHT = 0.12
CROWN = 0.05
KERB_HEIGHT = 0.05
VERGE_HEIGHT = 0.09
GRAVEL_HEIGHT = 0.07
# The kerb rises from the road over its first part, then stays at its height.
KERB_RISE = 0.3
# Over these lengths a kerb rises from nothing and a gravel trap widens from nothing (meters along the lap).
KERB_RAMP = 3.0
GRAVEL_RAMP = 25.0
# The structures stand on the ground this far from the center line: the gantry's and the gates' legs, the footbridge's
# piers (both outside the barrier), and the footbridge's clearance under its deck.
LEG_OFFSET = 14.6
PIER_OFFSET = 15.6
BRIDGE_CLEARANCE = 6.0
# No two parts of the circuit come closer than this (meters, barrier to barrier).
CLEARANCE = 8.0


def road_height(lateral):
    """The road's surface height at a lateral offset from the center line (|lateral| <= HALF_WIDTH): the crown falls in a
    straight line from the center line to each edge, as the track's meshes and its collider have it
    (GenerateAssets.cross_section puts the road's points at the edges and the center line only)."""
    return ROAD_HEIGHT + CROWN * (1.0 - abs(lateral) / HALF_WIDTH)


# --- The layout ----------------------------------------------------------------------------------------------------------

class Straight:
    def __init__(self, name, length):
        self.name = name
        self.length = length


class Corner:
    """An arc of the radius (meters) turning by the angle (degrees: positive turns left, negative right)."""

    def __init__(self, name, radius, angle, gravel):
        self.name = name
        self.radius = radius
        self.angle = angle
        # The widest the gravel trap on its outside gets (meters; 0 for none).
        self.gravel = gravel

    @property
    def length(self):
        return self.radius * math.radians(abs(self.angle))


# The circuit as a closed polygon, raced in this order (clockwise): each vertex is a corner, rounded by an arc of its
# radius tangent to both edges. Plane coordinates (x east, y north, meters), the radius, and the widest gravel trap on
# the corner's outside. The main straight runs from Turn 10 to Turn 1.
VERTICES = [
    ("Turn 1", (650.0, 0.0), 28.0, 24.0),
    ("Turn 2", (490.0, -136.0), 70.0, 16.0),
    ("Turn 3", (522.0, -345.0), 45.0, 20.0),
    ("Turn 4", (208.0, -397.0), 25.0, 18.0),
    ("Turn 5", (156.0, -350.0), 25.0, 12.0),
    ("Turn 6", (-115.0, -366.0), 130.0, 22.0),
    ("Turn 7", (-240.0, -84.0), 50.0, 20.0),
    ("Turn 8", (-104.0, 63.0), 40.0, 18.0),
    ("Turn 9", (-272.0, 188.0), 55.0, 20.0),
    ("Turn 10", (-125.0, 293.0), 60.0, 20.0),
]
# The names of the straights after each corner (the one after Turn 10 is the main straight).
STRAIGHTS = ("Turn 1 exit", "Turn 2 exit", "Back straight", "Chicane", "Turn 5 exit", "Turn 6 exit", "Turn 7 exit",
             "Turn 8 exit", "Turn 9 exit", "Main straight")
# The start and finish line lies this far along the main straight after Turn 10's exit.
LINE_AFTER_TURN_10 = 180.0


def _heading(a, b):
    return math.atan2(b[1] - a[1], b[0] - a[0])


def _build():
    """The segments from the start and finish line round the lap, the line's point and the heading there."""
    count = len(VERTICES)
    corners = []
    tangents = []
    for index, (name, point, radius, widest) in enumerate(VERTICES):
        before = VERTICES[index - 1][1]
        after = VERTICES[(index + 1) % count][1]
        turn = math.remainder(_heading(point, after) - _heading(before, point), 2.0 * math.pi)
        corners.append(Corner(name, radius, math.degrees(turn), widest))
        tangents.append(radius * math.tan(abs(turn) / 2.0))
    straights = []
    for index in range(count):
        point = VERTICES[index][1]
        after = VERTICES[(index + 1) % count][1]
        edge = math.hypot(after[0] - point[0], after[1] - point[1])
        straights.append(Straight(STRAIGHTS[index], edge - tangents[index] - tangents[(index + 1) % count]))
    # The lap starts on the main straight (after the last corner), LINE_AFTER_TURN_10 past Turn 10's exit.
    main = straights[-1]
    segments = [Straight(main.name, main.length - LINE_AFTER_TURN_10)]
    for index in range(count - 1):
        segments += [corners[index], straights[index]]
    segments += [corners[-1], Straight(main.name, LINE_AFTER_TURN_10)]
    last, first = VERTICES[-1][1], VERTICES[0][1]
    heading = _heading(last, first)
    along = tangents[-1] + LINE_AFTER_TURN_10
    origin = (last[0] + along * math.cos(heading), last[1] + along * math.sin(heading))
    return segments, origin, heading


SEGMENTS, ORIGIN, HEADING = _build()


def walk(segments):
    """The end point and heading (radians, counter-clockwise from east) of the path from the line."""
    x, y, heading = ORIGIN[0], ORIGIN[1], HEADING
    for segment in segments:
        x, y, heading = advance(x, y, heading, segment, segment.length)
    return (x, y), heading


def advance(x, y, heading, segment, distance):
    """The point and heading a distance into a segment that starts at (x, y) with the heading. Plane coordinates: x east,
    y north."""
    if isinstance(segment, Straight):
        return x + distance * math.cos(heading), y + distance * math.sin(heading), heading
    turn = math.copysign(1.0, segment.angle)
    # The arc's center lies to the left (left turns) or the right (right turns) of the heading.
    cx = x - turn * segment.radius * math.sin(heading)
    cy = y + turn * segment.radius * math.cos(heading)
    swept = turn * distance / segment.radius
    angle = heading + swept
    return cx + turn * segment.radius * math.sin(angle), cy - turn * segment.radius * math.cos(angle), angle


LENGTH = sum(segment.length for segment in SEGMENTS)
STARTS = []
_start = 0.0
for _segment in SEGMENTS:
    STARTS.append(_start)
    _start += _segment.length
del _start, _segment


def segment_at(s):
    """The index of the segment the lap distance falls in, and the distance into it."""
    s %= LENGTH
    for index in range(len(SEGMENTS) - 1, -1, -1):
        if s >= STARTS[index]:
            return index, s - STARTS[index]
    return 0, s


_SEGMENT_STATES = []


def _segment_states():
    if not _SEGMENT_STATES:
        x, y, heading = ORIGIN[0], ORIGIN[1], HEADING
        for segment in SEGMENTS:
            _SEGMENT_STATES.append((x, y, heading))
            x, y, heading = advance(x, y, heading, segment, segment.length)
    return _SEGMENT_STATES


def plane_point(s):
    """The center line's point (x east, y north) and heading (radians, counter-clockwise from east) at the lap distance."""
    index, into = segment_at(s)
    x, y, heading = _segment_states()[index]
    return advance(x, y, heading, SEGMENTS[index], into)


def curvature(s):
    """The center line's signed curvature at the lap distance (1/m, positive turning left)."""
    index, _ = segment_at(s)
    segment = SEGMENTS[index]
    return 0.0 if isinstance(segment, Straight) else math.copysign(1.0, segment.angle) / segment.radius


def world(x, y, height=0.0):
    """A plane point at a height, in world coordinates (+X east, +Y up, -Z north)."""
    return (x, height, -y)


def frame(s):
    """The center line's frame at the lap distance in world coordinates: the point on the ground, the direction of travel
    and the right-hand direction (both horizontal unit vectors)."""
    x, y, heading = plane_point(s)
    forward = (math.cos(heading), 0.0, -math.sin(heading))
    right = (math.sin(heading), 0.0, math.cos(heading))
    return world(x, y), forward, right


def offset_point(s, lateral, height=0.0):
    """The world point at a lateral offset (meters, positive to the right of travel) from the center line, at a height."""
    (px, _, pz), _, (rx, _, rz) = frame(s)
    return (px + lateral * rx, height, pz + lateral * rz)


def yaw(s):
    """The yaw (degrees about +Y) that turns an entity's forward (-Z) along the direction of travel at the lap distance and
    its +X to the right."""
    _, _, heading = plane_point(s)
    return math.degrees(heading) - 90.0


# --- What lies along the lap ---------------------------------------------------------------------------------------------

def corner_ranges():
    """The corners as (name, start, end, side) with lap distances; side is +1 when the corner turns right (its outside on
    the left) and -1 when it turns left."""
    ranges = []
    for index, segment in enumerate(SEGMENTS):
        if isinstance(segment, Corner):
            ranges.append((segment, STARTS[index], STARTS[index] + segment.length, 1 if segment.angle < 0 else -1))
    return ranges


def _union(ranges):
    merged = []
    for start, end in sorted(ranges):
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [tuple(span) for span in merged]


def _kerb_spans():
    """The kerbs' lap distance spans per side ('left', 'right'): along the inside of each corner and on its outside at the
    turn-in and the exit."""
    spans = {"left": [], "right": []}
    for corner, start, end, side in corner_ranges():
        inside = "right" if side > 0 else "left"
        outside = "left" if side > 0 else "right"
        spans[inside].append((start - 8.0, end + 8.0))
        spans[outside].append((start - 25.0, start + 6.0))
        spans[outside].append((end - 10.0, end + 30.0))
    return {side: _union(values) for side, values in spans.items()}


KERBS = _kerb_spans()


def _ramp(s, spans, ramp):
    """1 inside a span, 0 well outside, rising smoothly over the ramp's length before it and falling after it (with the lap
    distance wrapped)."""
    best = 0.0
    for start, end in spans:
        for shift in (-LENGTH, 0.0, LENGTH):
            a, b = start + shift, end + shift
            if a <= s <= b:
                return 1.0
            if a - ramp < s < a:
                best = max(best, smoothstep((s - (a - ramp)) / ramp))
            elif b < s < b + ramp:
                best = max(best, smoothstep(1.0 - (s - b) / ramp))
    return best


def smoothstep(t):
    t = min(1.0, max(0.0, t))
    return t * t * (3.0 - 2.0 * t)


def kerb(s, side):
    """How much of the kerb stands on the side at the lap distance (0 none, 1 its full height)."""
    return _ramp(s, KERBS[side], KERB_RAMP)


def _gravel_spans():
    spans = {"left": [], "right": []}
    for corner, start, end, side in corner_ranges():
        if corner.gravel > 0.0:
            outside = "left" if side > 0 else "right"
            spans[outside].append((start - 15.0, end + 35.0, corner.gravel))
    return spans


GRAVEL = _gravel_spans()


def gravel(s, side):
    """The gravel trap's width on the side at the lap distance (meters)."""
    width = 0.0
    for start, end, widest in GRAVEL[side]:
        width = max(width, widest * _ramp(s, [(start, end)], GRAVEL_RAMP))
    return width


def barrier_offset(s, side):
    """The lateral distance of the barrier's face from the center line on the side."""
    return HALF_WIDTH + KERB_WIDTH + VERGE_WIDTH + gravel(s, side) + SKIRT_WIDTH + BARRIER_GAP


def _straight(name):
    index = next(i for i, segment in enumerate(SEGMENTS) if segment.name == name)
    return STARTS[index], STARTS[index] + SEGMENTS[index].length


def _place():
    first = _straight("Turn 2 exit")
    second = _straight("Turn 6 exit")
    back = _straight("Back straight")
    gates = (first[0] + 0.58 * (first[1] - first[0]), second[0] + 0.5 * (second[1] - second[0]), 0.0)
    bridge = back[0] + 0.55 * (back[1] - back[0])
    return gates, bridge


# The timing lines (lap distances): each sector ends at a gate, the third at the start and finish line under the start
# gantry. The footbridge crosses the back straight.
SECTOR_GATES, FOOTBRIDGE = _place()


# --- Sampling ------------------------------------------------------------------------------------------------------------

def sections(spacing=0.25):
    """The lap distances at which the meshes have a cross-section: at most 8 m apart on straights and 2 degrees of turn (and
    3 m) on arcs, 1 m apart where a kerb rises and 2.5 m where a gravel trap widens, and at both edges of each timing
    line's band; no two closer than the spacing. Sorted, from 0 up to (but without) LENGTH."""
    points = []
    for index, segment in enumerate(SEGMENTS):
        start = STARTS[index]
        step = 8.0 if isinstance(segment, Straight) else min(3.0, segment.radius * math.radians(2.0))
        count = max(1, math.ceil(segment.length / step))
        points += [start + segment.length * k / count for k in range(count)]
    for side in ("left", "right"):
        for spans, ramp, step in ((KERBS[side], KERB_RAMP, 1.0), ([span[:2] for span in GRAVEL[side]], GRAVEL_RAMP, 2.5)):
            for start, end in spans:
                for first in (start - ramp, end):
                    count = int(math.ceil(ramp / step))
                    points += [(first + ramp * k / count) % LENGTH for k in range(count + 1)]
    lines = sorted({round(edge % LENGTH, 4) for line in SECTOR_GATES for edge in timing_band(line)})
    kept = list(lines)
    for point in sorted(round(point, 4) for point in points):
        index = bisect.bisect_left(kept, point)
        before = kept[index - 1] if index > 0 else kept[-1] - LENGTH
        after = kept[index] if index < len(kept) else kept[0] + LENGTH
        if point - before >= spacing and after - point >= spacing:
            kept.insert(index, point)
    return kept


def timing_line(s):
    """The lap distances of a timing line's painted band: the finish line is 1 m wide (two rows of checks), the sector lines
    0.4 m."""
    width = 1.0 if s == 0.0 else 0.4
    return (s - width, s) if s == 0.0 else (s - width / 2.0, s + width / 2.0)


# The road around a timing line is drawn with the line painted into its texture (GenerateAssets.py): geometry thinner than
# a pixel breaks into dashes at a distance, while a texture's smaller images blend the line into the asphalt.
TIMING_MARGIN = 1.0


def timing_band(s):
    """The lap distances of the road drawn with a timing line's paint: the line and TIMING_MARGIN either side of it."""
    start, end = timing_line(s)
    return start - TIMING_MARGIN, end + TIMING_MARGIN


# --- Checks --------------------------------------------------------------------------------------------------------------

def check():
    """Asserts the layout's promises; returns a short description of the circuit."""
    corners = corner_ranges()
    assert len(corners) == 10, f"{len(corners)} corners"
    assert 2450.0 <= LENGTH <= 2750.0, f"the lap is {LENGTH:.1f} m"
    short = [segment.name for segment in SEGMENTS if isinstance(segment, Straight) and segment.length < 20.0]
    assert not short, f"straights too short to ride: {short}"
    end, heading = walk(SEGMENTS)
    assert math.hypot(end[0] - ORIGIN[0], end[1] - ORIGIN[1]) < 1e-6, end
    assert abs(math.remainder(heading - HEADING, 2.0 * math.pi)) < 1e-9 and abs(heading - HEADING + 2.0 * math.pi) < 1e-9, heading
    for corner, _, _, _ in corners:
        inside = HALF_WIDTH + KERB_WIDTH + VERGE_WIDTH + SKIRT_WIDTH + BARRIER_GAP + BARRIER_BASE
        assert corner.radius > inside + 5.0, f"{corner.name}: radius {corner.radius} m, barrier {inside} m inside"
    # No two parts of the circuit come closer than the clearance: any two center line points farther apart along the lap
    # than their envelopes' reach must lie farther apart than both envelopes and the clearance.
    step = 4.0
    samples = []
    s = 0.0
    while s < LENGTH:
        x, y, _ = plane_point(s)
        reach = max(barrier_offset(s, "left"), barrier_offset(s, "right")) + BARRIER_BASE
        samples.append((s, x, y, reach))
        s += step
    closest = None
    for i, (si, xi, yi, ri) in enumerate(samples):
        for sj, xj, yj, rj in samples[i + 1:]:
            along = min(sj - si, LENGTH - (sj - si))
            limit = ri + rj + CLEARANCE
            if along < 1.6 * limit:
                continue
            distance = math.hypot(xi - xj, yi - yj)
            if closest is None or distance - limit < closest[0]:
                closest = (distance - limit, si, sj)
            assert distance >= limit, f"the circuit comes within {distance:.1f} m of itself at {si:.0f} m and {sj:.0f} m"
    # The structures' legs and the footbridge's piers and stairs stand behind the barrier, on grass.
    for s, reach, around in [(FOOTBRIDGE, PIER_OFFSET, 10.0)] + [(gate, LEG_OFFSET, 5.0) for gate in SECTOR_GATES]:
        for side in ("left", "right"):
            for probe in (-around, 0.0, around):
                face = barrier_offset(s + probe, side)
                assert face + BARRIER_BASE + 0.3 < reach, f"the barrier reaches {face:.1f} m at {s + probe:.0f} m ({side})"
    return (f"{LENGTH:.1f} m, {len(corners)} corners, gates at {', '.join(f'{gate:.0f}' for gate in SECTOR_GATES)} m, "
            f"footbridge at {FOOTBRIDGE:.0f} m, closest approach {closest[0]:.1f} m beyond the clearance "
            f"({closest[1]:.0f} m and {closest[2]:.0f} m)")


if __name__ == "__main__":
    for segment, start in zip(SEGMENTS, STARTS):
        print(f"{start:8.1f} {segment.name:16} {segment.length:8.1f}" +
              (f"  R{segment.radius:.0f} {segment.angle:+.0f} deg" if isinstance(segment, Corner) else ""))
    print(check())

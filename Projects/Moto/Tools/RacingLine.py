"""Moto's racing line: where a fast lap rides, how fast, how far leaned, in which gear and at what engine speed, from
the layout in Circuit.py and the figures below. The ghost bike rides it (Author.py's ghost part writes its keys into the
timeline of Scripts/Ghost.sgraph), and the test scene's checks compare the ghost with it.

The line, in four steps:

1. Out, in and out: every corner is taken from the road's outside edge at its turn-in, to its inside edge at its apex, and
   back out to the outside edge at its exit (EDGE from the center line). The turn-in and the exit lie where a single arc
   of the largest radius the road allows would leave the edge (_corner_points); where the next corner turns the
   other way too soon to reach the outside between them (the chicane), the line runs from one apex straight to the next.
   Between those points the lateral offset moves with a smoothstep, so the line's direction is continuous.
2. Smoothed: the line's points are averaged along the lap with a Gaussian of SMOOTHING meters, so that its curvature is
   continuous too (the circuit's arcs start at full curvature): no lean and no speed jumps where a corner begins. The
   offsets the smoothing gives are measured again from the center line; check() asserts they stay on the asphalt.
3. The speed: at each point the fastest the line's curvature allows at GRIP (the sideways acceleration of the lean
   limit), then a pass forwards at the acceleration the engine gives (POWER at the rear wheel against the drag of
   DRAG_AREA and the rolling resistance, at most WHEELIE, which keeps the front down) and one backwards at BRAKING; both
   share the tyres' grip with cornering (a friction ellipse). The passes go round the lap three times, so the speed where
   the lap closes is the lap's own.
4. What the rider and the engine do: the lean is the angle at which gravity and the cornering force balance, atan(v^2
   kappa / g), positive to the right; the gear follows the gearbox (PRIMARY, GEARS, FINAL on the rear tyre's radius):
   up at SHIFT_UP while accelerating, down while braking whenever the engine falls under DOWNSHIFT and the lower gear
   stays under SHIFT_UP; the engine speed is the rear wheel's turned by the gear's ratio; the load is the share of POWER the engine gives (1 at full throttle,
   0 braking or coasting).

timeline() writes the ghost's keys: the bike's position (its origin on the road, raised as a leaned tyre's round section
lifts it), heading (degrees about +Y, unwrapped: a lap turns it by -360), lean, engine speed and load, each a cubic track
whose keys carry the line's own slopes, as few keys as keep each track within its tolerance of the line (KEY_TOLERANCES);
the engine speed jumps at each shift (two keys at one time). evaluate() is the engine's evaluation of such a track
(Strada-ScriptCore's CurveMath), so check() asserts what the ghost does, not what the design meant.

    python Projects/Moto/Tools/RacingLine.py      (prints the line's figures and runs check())
"""
import bisect
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Bike  # noqa: E402  (the motorcycle: its tyres)
import Circuit  # noqa: E402  (the layout)

# --- The figures ---------------------------------------------------------------------------------------------------------

# How far from the center line the line goes at most (meters): the tyres' contact patch stays this far inside the road's
# edge (Circuit.HALF_WIDTH), on the asphalt even where the smoothing moves it.
EDGE = 4.6
EDGE_LIMIT = Circuit.HALF_WIDTH - 0.8
# The Gaussian the line's points are averaged with (its standard deviation, meters along the lap).
SMOOTHING = 6.0
# The points the line is computed at: every STEP meters of the center line.
STEP = 1.0
# The bike and its rider: mass (kilograms), power at the rear wheel (watts), the drag area (Cd times frontal area, m^2,
# tucked in), the air's density (kg/m^3) and the asphalt's rolling resistance (Surfaces/Asphalt.sphysmat).
MASS = 262.0
POWER = 150000.0
DRAG_AREA = 0.30
AIR_DENSITY = 1.2
ROLLING = 0.015
GRAVITY = 9.81
# Accelerations (m/s^2): the most the rear tyre drives before the front lifts, and the hardest braking before the rear
# lifts; GRIP, the sideways acceleration at the lean limit (in g).
WHEELIE = 9.0
BRAKING = 11.5
GRIP = 1.2
# The lean the line never passes (degrees): atan(GRIP) is 50.2. Every corner's deepest lean is at least MIN_CORNER_LEAN, to
# its inside.
MAX_LEAN = 52.0
MIN_CORNER_LEAN = 10.0
# The gearbox: the primary drive, the six gears and the final drive (chain), on the rear tyre's radius.
PRIMARY = 1.80
GEARS = (2.714, 2.118, 1.833, 1.600, 1.429, 1.304)
FINAL = 2.5625
WHEEL_RADIUS = Bike.REAR_RADIUS
# Engine speeds (revolutions a minute): the shift up while accelerating, the speed under which the rider shifts down while
# braking (when the lower gear stays under SHIFT_UP), the red line and idle.
SHIFT_UP = 15500.0
DOWNSHIFT = 10500.0
RED_LINE = 16000.0
IDLE = 1700.0
# What each of the ghost's tracks may differ from the line between its keys: position (meters), heading and lean
# (degrees), engine speed (rpm) and load.
KEY_TOLERANCES = {"Position": 0.01, "Heading": 0.1, "Lean": 0.2, "EngineSpeed": 15.0, "Load": 0.02}
# The shortest stretch over which the line crosses the road from one edge to the other (meters).
CROSSING = 60.0
# The least room between the line and the bike waiting on the grid (meters, box to box).
GRID_CLEARANCE = 0.5
# The longest a span between two keys may be (seconds).
MAX_KEY_SPAN = 2.0


# --- Step 1: out, in and out ---------------------------------------------------------------------------------------------

def _corner_points():
    """The lateral offset's control points (lap distance, offset), in lap order, before resolving conflicts."""
    points = []
    for corner, start, end, side in Circuit.corner_ranges():
        theta = math.radians(abs(corner.angle))
        half = math.cos(theta / 2.0)
        # The largest arc inside the corner: tangent to the outside edges of the straights either side and through the
        # inside edge at the bisector (R' below), leaves the edge (R' - R) tan(theta / 2) before and after the center
        # line's own arc does.
        widest = ((corner.radius + EDGE) - (corner.radius - EDGE) * half) / (1.0 - half)
        lead = (widest - corner.radius) * math.tan(theta / 2.0)
        inside, outside = side * EDGE, -side * EDGE
        points.append(("in", start - lead, outside))
        points.append(("apex", 0.5 * (start + end), inside))
        points.append(("out", end + lead, outside))
    return points


def control_points():
    """The lateral offset's control points (lap distance, offset), sorted, conflicts resolved: where a corner's exit comes
    after the next corner's turn-in, or the two lie on opposite sides less than CROSSING meters apart, both are dropped and
    the line runs from apex to apex."""
    points = _corner_points()
    kept = list(points)
    count = len(points) // 3
    for index in range(count):
        exit_point = points[3 * index + 2]
        turn_in = points[(3 * index + 3) % len(points)]
        exit_s, turn_in_s = exit_point[1], turn_in[1] + (Circuit.LENGTH if index == count - 1 else 0.0)
        opposite = exit_point[2] != turn_in[2]
        if turn_in_s <= exit_s or (opposite and turn_in_s - exit_s < CROSSING):
            kept.remove(exit_point)
            kept.remove(turn_in)
    return sorted(((s % Circuit.LENGTH, offset) for _, s, offset in kept))



def offset(s, points):
    """The lateral offset at a lap distance: smoothstep between the control points around it."""
    s %= Circuit.LENGTH
    times = [point[0] for point in points]
    k = bisect.bisect_right(times, s) - 1
    a = points[k] if k >= 0 else (points[-1][0] - Circuit.LENGTH, points[-1][1])
    b = points[k + 1] if k + 1 < len(points) else (points[0][0] + Circuit.LENGTH, points[0][1])
    return a[1] + (b[1] - a[1]) * Circuit.smoothstep((s - a[0]) / (b[0] - a[0]))


# --- Step 2: smoothed ----------------------------------------------------------------------------------------------------

def _raw_points(count, step, points):
    raw = []
    for i in range(count):
        s = i * step
        (cx, _, cz), _, (rx, _, rz) = Circuit.frame(s)
        n = offset(s, points)
        raw.append((cx + n * rx, cz + n * rz))
    return raw


def _smooth(raw, step):
    count = len(raw)
    reach = int(math.ceil(3.0 * SMOOTHING / step))
    weights = [math.exp(-0.5 * (k * step / SMOOTHING) ** 2) for k in range(-reach, reach + 1)]
    total = sum(weights)
    smoothed = []
    for i in range(count):
        x = z = 0.0
        for k, weight in zip(range(-reach, reach + 1), weights):
            px, pz = raw[(i + k) % count]
            x += weight * px
            z += weight * pz
        smoothed.append((x / total, z / total))
    return smoothed


def _lateral(point, s):
    """The point's lateral offset from the center line (right of travel) and the lap distance of its foot (the center
    line's point square to it), found from s by a few steps along the center line's direction."""
    t = s
    for _ in range(4):
        center, forward, _ = Circuit.frame(t % Circuit.LENGTH)
        t += (point[0] - center[0]) * forward[0] + (point[1] - center[2]) * forward[2]
    center, _, right = Circuit.frame(t % Circuit.LENGTH)
    return (point[0] - center[0]) * right[0] + (point[1] - center[2]) * right[2], t % Circuit.LENGTH


# --- Steps 3 and 4: the speed, the lean, the gear and the engine ---------------------------------------------------------

def resistance(v):
    """The deceleration of the air's drag and the rolling resistance at a speed (m/s^2)."""
    return 0.5 * AIR_DENSITY * DRAG_AREA * v * v / MASS + ROLLING * GRAVITY


def ellipse(v, curvature):
    """The share of the tyres' grip left for driving or braking while cornering at a speed and curvature."""
    used = v * v * abs(curvature) / (GRIP * GRAVITY)
    return math.sqrt(max(0.0, 1.0 - used * used))


def ratio(gear):
    """The engine's turns for each of the rear wheel's in a gear (1 to 6)."""
    return PRIMARY * GEARS[gear - 1] * FINAL


def engine_speed(v, gear):
    """The engine speed (rpm) at a road speed in a gear, at least IDLE (the clutch slips below it)."""
    return max(IDLE, v / (2.0 * math.pi * WHEEL_RADIUS) * 60.0 * ratio(gear))


class Point:
    """The line at one of its points: where it is (world x, z, and its height y over the road), its lateral offset and
    the center line's lap distance at its foot, the line's own distance from the start and its curvature (1/m, positive
    turning left), the speed (m/s), the longitudinal acceleration (m/s^2), the time from the line, the heading (degrees
    about +Y, unwrapped), the lean (degrees, positive right), the gear, the engine speed (rpm) and the load (0 to 1)."""

    __slots__ = ("x", "y", "z", "lateral", "s", "distance", "curvature", "speed", "acceleration", "time", "heading", "lean",
                 "gear", "rpm", "load")


def _signed_curvature(a, b, c):
    """The signed curvature of the circle through three points of the plane's x and world z (positive turning left, as
    Circuit.curvature: the plane's y is -z)."""
    abx, aby = b[0] - a[0], -(b[1] - a[1])
    bcx, bcy = c[0] - b[0], -(c[1] - b[1])
    cross = abx * bcy - aby * bcx
    product = math.hypot(abx, aby) * math.hypot(bcx, bcy) * math.hypot(c[0] - a[0], c[1] - a[1])
    return 2.0 * cross / product if product > 0.0 else 0.0


def _build():
    count = int(round(Circuit.LENGTH / STEP))
    step = Circuit.LENGTH / count
    points = _smooth(_raw_points(count, step, control_points()), step)
    line = []
    for i, (x, z) in enumerate(points):
        point = Point()
        point.x, point.z = x, z
        point.lateral, point.s = _lateral((x, z), i * step)
        point.curvature = _signed_curvature(points[i - 1], points[i], points[(i + 1) % count])
        line.append(point)
    # The line's own distances, from the start and finish line's point (index 0) on.
    distance = 0.0
    lengths = []
    for i in range(count):
        a, b = points[i], points[(i + 1) % count]
        lengths.append(math.hypot(b[0] - a[0], b[1] - a[1]))
    for i, point in enumerate(line):
        point.distance = distance
        distance += lengths[i]
    # The speed: the cornering limit, then a forward pass and a backward pass, each round the lap three times from the
    # slowest point so that where the lap closes is settled.
    limits = [math.sqrt(GRIP * GRAVITY / abs(point.curvature)) if point.curvature != 0.0 else math.inf for point in line]
    speeds = list(limits)
    slowest = min(range(count), key=lambda index: limits[index])
    for lap in range(3):
        for k in range(count):
            i = (slowest + k) % count
            j = (i + 1) % count
            v = speeds[i]
            drive = min(WHEELIE * ellipse(v, line[i].curvature), POWER / (MASS * max(v, 1.0))) - resistance(v)
            speeds[j] = min(speeds[j], math.sqrt(max(0.0, v * v + 2.0 * drive * lengths[i])))
    for lap in range(3):
        for k in range(count):
            j = (slowest - k) % count
            i = (j - 1) % count
            v = speeds[j]
            brake = BRAKING * ellipse(v, line[j].curvature) + resistance(v)
            speeds[i] = min(speeds[i], math.sqrt(v * v + 2.0 * brake * lengths[i]))
    elapsed = 0.0
    for i, point in enumerate(line):
        point.speed = speeds[i]
        point.time = elapsed
        elapsed += 2.0 * lengths[i] / (speeds[i] + speeds[(i + 1) % count])
    lap_time = elapsed
    for i, point in enumerate(line):
        before, after = line[i - 1], line[(i + 1) % count]
        span = lengths[i - 1] + lengths[i]
        point.acceleration = (after.speed ** 2 - before.speed ** 2) / (2.0 * span)
        drive = MASS * (point.acceleration + resistance(point.speed)) * point.speed
        point.load = min(1.0, max(0.0, drive / POWER))
        point.lean = -math.degrees(math.atan(point.speed ** 2 * point.curvature / GRAVITY))
        # A leaned tyre touches the road off its middle: the bike's origin rises by what its section's roundness takes.
        point.y = Circuit.road_height(max(-Circuit.HALF_WIDTH, min(Circuit.HALF_WIDTH, point.lateral))) + \
            Bike.REAR_WIDTH / 2.0 * (1.0 - math.cos(math.radians(point.lean)))
    # The heading: the direction from the point before to the point after, unwrapped from the start's.
    previous = None
    for i, point in enumerate(line):
        before, after = points[i - 1], points[(i + 1) % count]
        heading = math.degrees(math.atan2(-(after[0] - before[0]), -(after[1] - before[1])))
        if previous is not None:
            heading = previous + math.remainder(heading - previous, 360.0)
        point.heading = previous = heading
    # The gears: walked twice round the lap from the start's, so that the lap's end shifts into the gear its start has.
    gear = next(g for g in range(1, len(GEARS) + 1) if engine_speed(line[0].speed, g) <= SHIFT_UP or g == len(GEARS))
    for lap in range(2):
        for point in line:
            if point.acceleration >= 0.0:
                while gear < len(GEARS) and engine_speed(point.speed, gear) > SHIFT_UP:
                    gear += 1
            else:
                while gear > 1 and engine_speed(point.speed, gear) < DOWNSHIFT and engine_speed(point.speed, gear - 1) <= SHIFT_UP:
                    gear -= 1
            point.gear = gear
            point.rpm = engine_speed(point.speed, gear)
    return line, lap_time, lengths


LINE, LAP_TIME, _LENGTHS = _build()


# --- The ghost's keys ----------------------------------------------------------------------------------------------------

def _slope(values, times, i, first, last):
    """A track's slope at sample i from its neighbours within [first, last] (one-sided at the ends)."""
    lo, hi = max(first, i - 1), min(last, i + 1)
    return [(values[hi][c] - values[lo][c]) / (times[hi] - times[lo]) for c in range(len(values[i]))]


def hermite(t0, v0, m0, t1, v1, m1, t):
    """A cubic segment's value at t, from its keys' values and slopes, as the engine evaluates it (CurveMath)."""
    span = t1 - t0
    s = (t - t0) / span
    h00, h10, h01, h11 = 2 * s ** 3 - 3 * s ** 2 + 1, s ** 3 - 2 * s ** 2 + s, -2 * s ** 3 + 3 * s ** 2, s ** 3 - s ** 2
    return [h00 * v0[c] + h10 * span * m0[c] + h01 * v1[c] + h11 * span * m1[c] for c in range(len(v0))]


def _fits(times, values, slopes, i, j, tolerance, linear):
    for k in range(i + 1, j):
        if linear:
            s = (times[k] - times[i]) / (times[j] - times[i])
            guess = [values[i][c] + (values[j][c] - values[i][c]) * s for c in range(len(values[i]))]
        else:
            guess = hermite(times[i], values[i], slopes[i], times[j], values[j], slopes[j], times[k])
        if math.dist(guess, values[k]) > tolerance:
            return False
    return True


def _choose_keys(times, values, slopes, first, last, tolerance, linear):
    """The samples a track keeps as keys between two (both kept): as few as keep every sample within the tolerance of
    the segment through them, no span longer than MAX_KEY_SPAN."""
    keys = [first]
    i = first
    while i < last:
        # The farthest sample whose segment from i fits: doubling, then halving.
        good, step = i + 1, 1
        while good + step <= last and times[good + step] - times[i] <= MAX_KEY_SPAN and \
                _fits(times, values, slopes, i, good + step, tolerance, linear):
            good += step
            step *= 2
        while step > 1:
            step //= 2
            if good + step <= last and times[good + step] - times[i] <= MAX_KEY_SPAN and \
                    _fits(times, values, slopes, i, good + step, tolerance, linear):
                good += step
        keys.append(good)
        i = good
    return keys


def _samples():
    """The lap's samples, the start's again at the lap's end: times, and per track the values (lists)."""
    points = LINE + [LINE[0]]
    times = [point.time for point in LINE] + [LAP_TIME]
    turned = LINE[-1].heading + math.remainder(LINE[0].heading - LINE[-1].heading, 360.0)
    tracks = {
        "Position": [[point.x, point.y, point.z] for point in points],
        "Heading": [[point.heading] for point in LINE] + [[turned]],
        "Lean": [[point.lean] for point in points],
        "Load": [[point.load] for point in points],
    }
    return times, tracks


def _rounded(values, digits):
    # Adding 0.0 turns a negative zero into zero, which JSON writes without its sign.
    return [round(value, digits) + 0.0 for value in values]


# Decimal places the keys are written with: times, then each track's values and slopes.
TIME_DIGITS = 5
DIGITS = {"Position": 4, "Heading": 3, "Lean": 3, "EngineSpeed": 1, "Load": 3}


def keys():
    """The ghost's tracks: {name: [(time, value, in slope, out slope, linear)]}, values and slopes as lists (a float
    track's of one), the engine speed's keys in pairs at one time where it shifts."""
    times, tracks = _samples()
    last = len(times) - 1
    out = {}
    for name, values in tracks.items():
        linear = name == "Load"
        slopes = [_slope(values, times, i, 0, last) for i in range(len(values))]
        # The lap closes: its first and last keys take the slope across the seam.
        seam = [(values[1][c] - values[last - 1][c] + (values[last][c] - values[0][c])) /
                (times[1] + LAP_TIME - times[last - 1]) for c in range(len(values[0]))]
        slopes[0] = slopes[last] = seam
        chosen = _choose_keys(times, values, slopes, 0, last, KEY_TOLERANCES[name], linear)
        out[name] = [(times[i], values[i], slopes[i], slopes[i], linear) for i in chosen]
    # The engine speed: a stretch per gear, its keys at both ends, two keys at one time where it shifts.
    rpm = [[point.rpm] for point in LINE] + [[LINE[0].rpm]]
    gears = [point.gear for point in LINE] + [LINE[0].gear]
    starts = [0] + [i for i in range(1, last + 1) if gears[i] != gears[i - 1]]
    engine = []
    for index, first in enumerate(starts):
        end = starts[index + 1] if index + 1 < len(starts) else last
        # The stretch runs from its first sample to the sample where the next gear begins, at the old gear's speed.
        values = rpm[first:end] + [[engine_speed((LINE + [LINE[0]])[end].speed, gears[first])]]
        stretch_times = times[first:end + 1]
        slopes = [_slope(values, stretch_times, i, 0, len(values) - 1) for i in range(len(values))]
        if first == 0 or end == last:
            # Where the lap closes inside a gear, that end takes the slope across the seam.
            closing = (rpm[1][0] - rpm[last - 1][0]) / (times[1] + LAP_TIME - times[last - 1])
            slopes[0 if first == 0 else -1] = [closing]
        chosen = _choose_keys(stretch_times, values, slopes, 0, len(values) - 1, KEY_TOLERANCES["EngineSpeed"], False)
        engine += [(stretch_times[i], values[i], slopes[i], slopes[i], False) for i in chosen]
    out["EngineSpeed"] = engine
    # Rounded as written.
    return {name: [(round(time, TIME_DIGITS), _rounded(value, DIGITS[name]), _rounded(slope_in, DIGITS[name] + 1),
                    _rounded(slope_out, DIGITS[name] + 1), linear) for time, value, slope_in, slope_out, linear in track]
            for name, track in out.items()}


KEYS = keys()


def evaluate(track, time):
    """A track's value at a time, as the engine evaluates it (CurveMath.Evaluate): the last key at or before the time
    starts the segment (of keys at one time, the later), cubic from the keys' slopes or linear."""
    times = [key[0] for key in track]
    if time < times[0]:
        return list(track[0][1])
    index = bisect.bisect_right(times, time) - 1
    if index >= len(track) - 1:
        return list(track[-1][1])
    (t0, v0, _, m0, linear), (t1, v1, m1, _, _) = track[index], track[index + 1]
    if linear:
        s = (time - t0) / (t1 - t0)
        return [v0[c] + (v1[c] - v0[c]) * s for c in range(len(v0))]
    return hermite(t0, v0, m0, t1, v1, m1, time)


def length():
    """The ghost's lap: the timeline's length (seconds), its last key's time."""
    return KEYS["Position"][-1][0]


def pose(time):
    """Where the ghost is at a time of its lap (wrapped): its position, heading and lean (degrees), engine speed and load,
    as its timeline plays them."""
    time %= length()
    return tuple(evaluate(KEYS[name], time) for name in TRACKS)


def velocity(time, step=1e-3):
    """The ghost's velocity at a time of its lap (m/s, world), from its position track."""
    after = evaluate(KEYS["Position"], (time + step) % length())
    before = evaluate(KEYS["Position"], (time - step) % length())
    return [(after[c] - before[c]) / (2.0 * step) for c in range(3)]


# The ghost's tracks and their types.
TRACKS = {"Position": "Strada.Vector3", "Heading": "float", "Lean": "float", "EngineSpeed": "float", "Load": "float"}


def timeline():
    """The ghost's Flow.Timeline setting: its five tracks over the lap, looping."""
    tracks = []
    for name, type_id in TRACKS.items():
        scalar = type_id == "float"
        written = []
        for time, value, slope_in, slope_out, linear in KEYS[name]:
            key = {"Time": time, "Value": value[0] if scalar else value}
            if linear:
                key["Interpolation"] = "Linear"
            else:
                key["In"] = slope_in[0] if scalar else slope_in
                key["Out"] = slope_out[0] if scalar else slope_out
            written.append(key)
        tracks.append({"Name": name, "Type": type_id, "Keys": written})
    return {"Length": length(), "Loop": True, "Tracks": tracks}


# --- Checks --------------------------------------------------------------------------------------------------------------

def check():
    """Asserts the line's promises: on the asphalt everywhere (its lateral offset within EDGE_LIMIT), leaned at most
    MAX_LEAN, speeds continuous (no step between two points larger than the hardest acceleration allows, and none in the
    speed the ghost's keys give), the engine within its red line, GRID_CLEARANCE from the bike waiting on the grid, and
    keys that reproduce the line within their tolerances, at most 1024 a track, closing the lap; returns the line's
    figures."""
    widest = max(LINE, key=lambda point: abs(point.lateral))
    assert abs(widest.lateral) <= EDGE_LIMIT, f"the line leaves the asphalt: {widest.lateral:.2f} m at {widest.s:.0f} m"
    leaned = max(LINE, key=lambda point: abs(point.lean))
    assert abs(leaned.lean) <= MAX_LEAN, f"the line leans {leaned.lean:.1f} degrees at {leaned.s:.0f} m"
    # Into every corner, not out of it: the deepest lean between a corner's turn-in and exit (on the center line's arc) is to
    # its inside, which the circuit's layout gives (positive lean is to the right, a corner's side +1 when it turns right),
    # not the curvature the lean itself comes from.
    for corner, start, end, side in Circuit.corner_ranges():
        deepest = max((point for point in LINE if start <= point.s <= end), key=lambda point: abs(point.lean))
        assert deepest.lean * side >= MIN_CORNER_LEAN, \
            f"{corner.name}: the line leans {deepest.lean:.1f} degrees at {deepest.s:.0f} m, not into the corner"
    hardest = max(WHEELIE, BRAKING + resistance(max(point.speed for point in LINE)))
    for i, point in enumerate(LINE):
        after = LINE[(i + 1) % len(LINE)]
        elapsed = (after.time if i + 1 < len(LINE) else LAP_TIME) - point.time
        assert abs(after.speed - point.speed) <= 1.01 * hardest * elapsed, f"the speed jumps at {point.s:.0f} m"
        assert point.rpm <= RED_LINE and 1 <= point.gear <= len(GEARS), f"the engine at {point.rpm:.0f} rpm at {point.s:.0f} m"
    for name, track in KEYS.items():
        assert len(track) <= 1024, f"{name}: {len(track)} keys"
        assert all(a[0] <= b[0] for a, b in zip(track, track[1:])), f"{name}: keys out of time order"
    # The line passes the bike waiting on the grid with room between them (both upright there, side by side).
    (low, _, front), (high, top, back) = Bike.bounds()
    grid = Circuit.LENGTH - Bike.GRID_BEHIND
    for point in LINE:
        along = math.remainder(point.s - grid, Circuit.LENGTH)
        if front - back <= along <= back - front:
            # A leaned bike's top reaches out by its height times the lean's sine.
            reach = high + top * math.sin(math.radians(abs(point.lean)))
            gap = abs(point.lateral - Bike.GRID_LATERAL) - (high - low) / 2.0 - reach
            assert gap >= GRID_CLEARANCE, f"the line passes {gap:.2f} m from the bike on the grid"
    assert KEYS["Position"][0][1] == KEYS["Position"][-1][1], "the lap does not close"
    assert abs(KEYS["Heading"][-1][1][0] - KEYS["Heading"][0][1][0] + 360.0) < 1e-6, "the lap does not turn once round"
    # The keys against the line at every point (the engine speed away from its shifts, where its keys jump).
    worst = {name: 0.0 for name in KEYS}
    shifts = [key[0] for key in KEYS["EngineSpeed"]]
    for point in LINE:
        position, heading, lean, rpm, load = pose(point.time)
        worst["Position"] = max(worst["Position"], math.dist(position, (point.x, point.y, point.z)))
        worst["Heading"] = max(worst["Heading"], abs(math.remainder(heading[0] - point.heading, 360.0)))
        worst["Lean"] = max(worst["Lean"], abs(lean[0] - point.lean))
        worst["Load"] = max(worst["Load"], abs(load[0] - point.load))
        if all(abs(point.time - time) > 1e-3 for time in shifts):
            worst["EngineSpeed"] = max(worst["EngineSpeed"], abs(rpm[0] - point.rpm))
    for name, tolerance in KEY_TOLERANCES.items():
        allowed = tolerance * 1.05 + 10.0 ** -DIGITS[name]
        assert worst[name] <= allowed, f"{name}: the keys stray {worst[name]:.4f} from the line"
    # The ghost's own speed, from its position keys every 10 ms: no step.
    step = 0.01
    previous = None
    steepest = 0.0
    time = 0.0
    while time < length():
        speed = math.sqrt(sum(c * c for c in velocity(time)))
        if previous is not None:
            steepest = max(steepest, abs(speed - previous) / step)
        previous = speed
        time += step
    assert steepest <= 1.2 * hardest, f"the ghost's speed changes at {steepest:.1f} m/s^2"
    slowest = min(LINE, key=lambda point: point.speed)
    fastest = max(LINE, key=lambda point: point.speed)
    bridge = min(LINE, key=lambda point: abs(math.remainder(point.s - Circuit.FOOTBRIDGE, Circuit.LENGTH)))
    gear_changes = sum(1 for a, b in zip(LINE, LINE[1:] + LINE[:1]) if a.gear != b.gear)
    figures = ", ".join(f"{name} {len(track)}" for name, track in KEYS.items())
    errors = ", ".join(f"{name} {worst[name]:.3g}" for name in KEYS)
    return (f"a lap of {LAP_TIME:.2f} s ({3.6 * LINE[-1].distance / LAP_TIME:.0f} km/h on average), {3.6 * fastest.speed:.0f} "
            f"km/h at the fastest, {3.6 * slowest.speed:.0f} km/h at the slowest ({slowest.s:.0f} m); at most "
            f"{abs(leaned.lean):.1f} degrees of lean and {abs(widest.lateral):.2f} m off the center line; gears "
            f"{min(p.gear for p in LINE)} to {max(p.gear for p in LINE)}, {gear_changes} shifts a lap, "
            f"{min(p.rpm for p in LINE):.0f} to {max(p.rpm for p in LINE):.0f} rpm; under the footbridge at {bridge.time:.2f} s, "
            f"{3.6 * bridge.speed:.0f} km/h in gear {bridge.gear}; keys {figures}, within {errors} of the line")


if __name__ == "__main__":
    print(check())

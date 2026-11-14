"""Moto's camera tour: the path a camera flies once round the circuit, as the keys of a Flow.Timeline. Author.py writes
the keys into the tour's graph (Scripts/Tour.sgraph, which the main camera runs), and the test scene's checks compare the
camera with points of the path.

The tour starts and ends at the home view (the main camera's own transform, over the inside of the main straight before
the line). A lead point runs one lap along the center line, from just ahead of the home view to just before it, at a pace
the corners set (it brakes for them and accelerates out of them, faster the higher the camera flies), and the camera
chases it at a height and a distance that change along the lap: over the start gantry, then low down the main straight,
past the sector gates' boards and under the footbridge, and higher over the corners, so that their shape shows. In its
first and last BLEND seconds the camera glides between the home view and the chase.

The timeline's two curves (Eye, where the camera is, and LookAt, where it looks) are cubic through keys every KEY_STEP
seconds, with automatic slopes: Catmull-Rom on the keys' times, flat at the first and last key. evaluate() is the
engine's evaluation of such a curve (Strada-ScriptCore's CurveMath), so check() asserts what the camera does, not what the
design meant: it keeps CLEARANCE from every surface of the circuit's layout, the whole way.

The graph (declare() and write()): when the scene starts and PlayOnStart holds, the camera sends itself PlayTour, which
plays the timeline from its start; every update places the camera at Eye and turns it towards LookAt, and when the
timeline finishes, the Tours read-out counts the tour and, while Repeat holds, the timeline plays again. Other graphs
start a tour by sending the camera PlayTour.

    python Projects/Moto/Tools/Tour.py      (prints the tour's figures and runs check())
"""
import bisect
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Circuit  # noqa: E402  (the layout)
import Graphs  # noqa: E402  (writing graphs)
from Graphs import EFFECTS, EVENTS, public  # noqa: E402

# The home view: over the inside of the main straight 48 m before the line, looking past the gantry down the straight.
HOME_EYE = Circuit.offset_point(Circuit.LENGTH - 48.0, -24.0, 7.5)
HOME_TARGET = Circuit.offset_point(25.0, 3.0, 3.0)

# The lead's pace (meters and seconds): its top speed and the sideways acceleration it takes the corners at with the
# camera LOW, both growing with the camera's height (a view from higher up turns slower), and how hard it accelerates and
# brakes. It starts and ends at rest.
TOP_SPEED = 80.0
TOP_SPEED_PER_METER = 4.0
CORNERING = 20.0
CORNERING_PER_METER = 5.0
ACCELERATION = 10.0
BRAKING = 14.0
# The lap the lead runs: from this far before the line (unwrapped lap distances: negative is before the line) to this far
# before the line one lap on.
LEAD_START = -36.0
LEAD_END = Circuit.LENGTH - 30.0
# The camera's height over the center line by the lead's lap distance: LOW, except over the stretches listed (start, end,
# height), which it rises to and comes down from over RAMP meters before and after them. It leaves the home view over the
# start gantry, whose beam tops out at 7.85 m.
LOW = 2.8
RAMP = 110.0
HEIGHTS = (
    (LEAD_START - RAMP, 20.0, 11.0),  # over the start gantry
    (470.0, 690.0, 17.0),      # Turns 1 and 2
    (1300.0, 1690.0, 24.0),    # the chicane and Turn 6
    (1910.0, 2300.0, 15.0),    # Turns 7, 8 and 9
)
# Past each sector gate's board the camera keeps to the side, this far right of the center line (the board hangs over the
# middle of the road, 4.4 m up); it moves over within SIDE_RAMP meters.
GATE_SIDE = 5.0
SIDE_RAMP = 60.0
# How far behind the lead the camera flies and how far ahead of it it looks, both growing with the height.
BEHIND = 12.0
BEHIND_PER_METER = 0.6
AHEAD = 24.0
AHEAD_PER_METER = 1.3
# The glide between the home view and the chase at each end (seconds), and the keys' spacing.
BLEND = 4.5
KEY_STEP = 0.5
# Every point of the path keeps this distance from the circuit's surfaces (meters), a margin so that no wall, kerb or
# beam passes closer to the lens than about a frame's flight at the tour's top speed (105 m/s is 1.75 m a frame at
# 1/60 s). The camera has no collider: nothing else keeps it off them.
CLEARANCE = 2.0


# --- The design ----------------------------------------------------------------------------------------------------------

def _pace():
    """The lead's unwrapped lap distances every meter and the time it reaches each."""
    count = int(round(LEAD_END - LEAD_START))
    distances = [LEAD_START + (LEAD_END - LEAD_START) * k / count for k in range(count + 1)]
    step = (LEAD_END - LEAD_START) / count
    speeds = []
    for u in distances:
        raised = height(u) - LOW
        top = TOP_SPEED + TOP_SPEED_PER_METER * raised
        curvature = abs(Circuit.curvature(u % Circuit.LENGTH))
        speeds.append(min(top, math.sqrt((CORNERING + CORNERING_PER_METER * raised) / curvature)) if curvature > 0.0 else top)
    speeds[0] = speeds[-1] = 0.0
    for k in range(1, len(speeds)):
        speeds[k] = min(speeds[k], math.sqrt(speeds[k - 1] ** 2 + 2.0 * ACCELERATION * step))
    for k in range(len(speeds) - 2, -1, -1):
        speeds[k] = min(speeds[k], math.sqrt(speeds[k + 1] ** 2 + 2.0 * BRAKING * step))
    times = [0.0]
    for k in range(1, len(speeds)):
        times.append(times[-1] + 2.0 * step / (speeds[k - 1] + speeds[k]))
    return distances, times


def lead(t):
    """The lead's unwrapped lap distance at a time of the tour."""
    t = min(max(t, 0.0), LENGTH)
    k = min(max(bisect.bisect_right(TIMES, t) - 1, 0), len(TIMES) - 2)
    span = TIMES[k + 1] - TIMES[k]
    return DISTANCES[k] + (DISTANCES[k + 1] - DISTANCES[k]) * ((t - TIMES[k]) / span if span > 0.0 else 0.0)


def height(u):
    """The camera's height at the lead's lap distance: LOW, raised over the stretches of HEIGHTS."""
    raised = LOW
    for start, end, high in HEIGHTS:
        if start <= u <= end:
            weight = 1.0
        elif start - RAMP < u < start:
            weight = Circuit.smoothstep((u - (start - RAMP)) / RAMP)
        elif end < u < end + RAMP:
            weight = Circuit.smoothstep(1.0 - (u - end) / RAMP)
        else:
            weight = 0.0
        raised = max(raised, LOW + (high - LOW) * weight)
    return raised


def side(e):
    """The camera's lateral offset (right of travel) at its own lap distance: GATE_SIDE past the sector gates' boards."""
    offset = 0.0
    for gate in Circuit.SECTOR_GATES[:2]:
        distance = abs(math.remainder(e - gate, Circuit.LENGTH))
        offset = max(offset, GATE_SIDE * Circuit.smoothstep(1.0 - (distance - 2.0 * BEHIND) / SIDE_RAMP))
    return offset


def chase(t):
    """Where the chasing camera is and where it looks at a time of the tour (world points)."""
    u = lead(t)
    h = height(u)
    e = u - (BEHIND + BEHIND_PER_METER * h)
    eye = Circuit.offset_point(e % Circuit.LENGTH, side(e), h)
    target = Circuit.offset_point((u + AHEAD + AHEAD_PER_METER * h) % Circuit.LENGTH, 0.0, 1.0 + 0.25 * h)
    return eye, target


DISTANCES, TIMES = _pace()
# The tour's length: the lead's lap time.
LENGTH = TIMES[-1]


def design(t):
    """The tour's eye and target at a time: the home view glides into the chase and back."""
    eye, target = chase(t)
    weight = min(Circuit.smoothstep(t / BLEND), Circuit.smoothstep((LENGTH - t) / BLEND))
    return tuple(_mix(HOME_EYE, eye, weight)), tuple(_mix(HOME_TARGET, target, weight))


def _mix(a, b, weight):
    return [a[k] + (b[k] - a[k]) * weight for k in range(3)]


def key_times():
    """The keys' times: every KEY_STEP seconds from 0, and the tour's end."""
    count = int(math.floor(LENGTH / KEY_STEP + 1e-9))
    times = [round(KEY_STEP * k, 3) for k in range(count + 1)]
    if LENGTH - times[-1] > 0.25 * KEY_STEP:
        times.append(round(LENGTH, 3))
    else:
        times[-1] = round(LENGTH, 3)
    return times


def keys():
    """The Eye and LookAt keys: (time, [x, y, z]) pairs, millimeter values, the first and last at the home view."""
    eyes, targets = [], []
    for time in key_times():
        eye, target = design(time)
        eyes.append((time, [round(value, 3) for value in eye]))
        targets.append((time, [round(value, 3) for value in target]))
    return eyes, targets


def timeline():
    """The tour's Flow.Timeline setting: the Eye and LookAt curves over the tour's length, not looping."""
    eyes, targets = keys()
    return {"Length": key_times()[-1], "Loop": False, "Tracks": [
        {"Name": "Eye", "Type": "Strada.Vector3", "Keys": [{"Time": time, "Value": value} for time, value in eyes]},
        {"Name": "LookAt", "Type": "Strada.Vector3", "Keys": [{"Time": time, "Value": value} for time, value in targets]},
    ]}


# --- What the camera does ------------------------------------------------------------------------------------------------

def evaluate(curve, t):
    """A curve of (time, value) keys at a time as the engine evaluates a timeline's cubic curve with automatic slopes:
    Catmull-Rom on the keys' times, flat at the first and last key, the end values outside."""
    times = [time for time, _ in curve]
    if t <= times[0]:
        return list(curve[0][1])
    if t >= times[-1]:
        return list(curve[-1][1])
    k = bisect.bisect_right(times, t) - 1

    def slope(index, component):
        if index == 0 or index == len(curve) - 1:
            return 0.0
        return (curve[index + 1][1][component] - curve[index - 1][1][component]) / (times[index + 1] - times[index - 1])

    span = times[k + 1] - times[k]
    s = (t - times[k]) / span
    h00, h10, h01, h11 = 2 * s ** 3 - 3 * s ** 2 + 1, s ** 3 - 2 * s ** 2 + s, -2 * s ** 3 + 3 * s ** 2, s ** 3 - s ** 2
    return [h00 * curve[k][1][c] + h10 * span * slope(k, c) + h01 * curve[k + 1][1][c] + h11 * span * slope(k + 1, c)
            for c in range(3)]


def camera(t):
    """Where the camera is at a time of the tour and the unit direction it looks in."""
    eyes, targets = _KEYS
    eye, target = evaluate(eyes, t), evaluate(targets, t)
    return eye, _normalize([target[k] - eye[k] for k in range(3)])


def _normalize(v):
    length = math.sqrt(sum(c * c for c in v))
    return [c / length for c in v]


_KEYS = keys()


def under_footbridge(step=0.01):
    """When the camera passes under the footbridge: the time, where it is and the direction it looks, and how fast it flies
    (meters a second). The test scene checks the camera there."""
    best = None
    t = 0.0
    while t <= LENGTH:
        eye, _ = camera(t)
        e, _ = _lap_position(eye, lead(t))
        along = abs(math.remainder(e - Circuit.FOOTBRIDGE, Circuit.LENGTH))
        if best is None or along < best[0]:
            best = (along, t)
        t += step
    time = round(best[1], 2)
    eye, look = camera(time)
    later, _ = camera(time + step)
    return time, eye, look, math.dist(eye, later) / step


# --- Checks --------------------------------------------------------------------------------------------------------------

def _lap_position(point, guess):
    """The lap distance of the center line point nearest a world point, searched within 150 m of a guess, and the point's
    lateral offset from it (right of travel)."""
    best = None
    s = guess - 150.0
    while s <= guess + 150.0:
        center, _, right = Circuit.frame(s % Circuit.LENGTH)
        dx, dz = point[0] - center[0], point[2] - center[2]
        distance = dx * dx + dz * dz
        if best is None or distance < best[0]:
            best = (distance, s, dx * right[0] + dz * right[2])
        s += 0.5
    return best[1] % Circuit.LENGTH, best[2]


def _structure_clearance(e, lateral, y):
    """How far the point lies from the structures over the track (the start gantry, the sector gates' arches and boards,
    the footbridge) when it is level with one of them; None when it is not."""
    def gap(low, high):
        return low - y if y < low else (y - high if y > high else -1.0)

    found = []
    for gate in Circuit.SECTOR_GATES:
        along = abs(math.remainder(e - gate, Circuit.LENGTH))
        if gate == 0.0:
            # The gantry: a beam 1 m deep from 6.75 m to 7.85 m between legs 14.6 m out, the lights' panel under its middle
            # down to 5 m.
            if along <= 0.5 + CLEARANCE and abs(lateral) <= Circuit.LEG_OFFSET + 0.6 + CLEARANCE:
                found.append(gap(5.0 if abs(lateral) <= 3.2 + CLEARANCE else 6.75, 7.85))
        elif along <= 0.2 + CLEARANCE and abs(lateral) <= Circuit.LEG_OFFSET + 0.45 + CLEARANCE:
            # A sector gate: a beam 0.4 m deep from 6 m to 6.4 m, its board under its middle down to 4.4 m.
            found.append(gap(4.4 if abs(lateral) <= 2.1 + CLEARANCE else 6.0, 6.4))
    along = abs(math.remainder(e - Circuit.FOOTBRIDGE, Circuit.LENGTH))
    if along <= 1.6 + CLEARANCE and abs(lateral) <= Circuit.PIER_OFFSET + 1.4 + CLEARANCE:
        # The footbridge: its deck from 6 m to 6.5 m and the side panels up to 7.7 m.
        found.append(gap(Circuit.BRIDGE_CLEARANCE, Circuit.BRIDGE_CLEARANCE + 1.7))
    return min(found) if found else None


def clearance(point, guess):
    """How far a point lies from the circuit's surfaces under and around it, by the layout (meters)."""
    e, lateral = _lap_position(point, guess)
    y = point[1]
    face = Circuit.barrier_offset(e, "right" if lateral > 0.0 else "left")
    if abs(lateral) < face - 0.3:
        # The road, the kerbs, the verges and the gravel: nothing stands higher than the kerbs' tops.
        free = y - (Circuit.ROAD_HEIGHT + Circuit.KERB_HEIGHT + Circuit.CROWN)
    elif abs(lateral) <= face + Circuit.BARRIER_BASE + 0.3:
        free = y - Circuit.BARRIER_HEIGHT
    else:
        free = y
    structure = _structure_clearance(e, lateral, y)
    return free if structure is None else min(free, structure), e


def check(step=0.01):
    """Asserts that the camera keeps CLEARANCE from the circuit's surfaces at every step of the tour and starts and ends at
    the home view; returns the tour's figures."""
    eyes, _ = _KEYS
    assert eyes[0][1] == [round(v, 3) for v in HOME_EYE] and eyes[-1][1] == eyes[0][1], "the tour does not start and end home"
    assert len(eyes) <= 1024, f"{len(eyes)} keys: a track holds at most 1024"
    closest = None
    deviation = 0.0
    fastest = 0.0
    turned = 1.0
    guess = Circuit.LENGTH - 48.0
    previous = None
    t = 0.0
    while t <= LENGTH:
        eye, look = camera(t)
        free, guess = clearance(eye, guess)
        if closest is None or free < closest[0]:
            closest = (free, t, eye)
        assert free >= CLEARANCE, f"the camera comes within {free:.2f} m of the circuit at {t:.2f} s ({eye})"
        planned, _ = design(t)
        deviation = max(deviation, math.dist(eye, planned))
        if previous is not None:
            moved = [eye[k] - previous[k] for k in range(3)]
            speed = math.sqrt(sum(c * c for c in moved)) / step
            fastest = max(fastest, speed)
            if speed > 5.0:
                turned = min(turned, sum(look[k] * moved[k] for k in range(3)) / (speed * step))
        previous = eye
        t += step
    return (f"{LENGTH:.2f} s, {len(eyes)} keys a curve, at most {fastest:.1f} m/s and {max(h for _, _, h in HEIGHTS):.0f} m up, "
            f"{deviation:.2f} m at most from the design; {closest[0]:.2f} m from the circuit at its closest ({closest[1]:.2f} s); "
            f"the view at most {math.degrees(math.acos(max(-1.0, min(1.0, turned)))):.0f} degrees off the way it flies")


# --- The graph ---------------------------------------------------------------------------------------------------------

TOUR = Graphs.script("Tour")
# The camera's Euler angles (degrees: pitch, then yaw, no roll) that turn its forward (-Z) from the eye towards the target.
LOOK = ("vec3(degrees(atan2(target.Y - eye.Y, length(vec2(target.X - eye.X, target.Z - eye.Z)))), "
        "degrees(atan2(eye.X - target.X, eye.Z - target.Z)), 0)")


def declare(author):
    author.declare(TOUR, "The camera tour: a timeline flies the camera that runs this graph once round the circuit, from the "
                   "home view and back to it, and again while Repeat holds. Tools/Tour.py makes its keys.", variables=(
        public("PlayOnStart", "bool", True, "Whether the tour takes off when the scene starts"),
        public("Repeat", "bool", True, "Whether the tour takes off again each time it is back at the home view"),
        public("Tours", "int", 0, "How many tours have come back to the home view (a read-out)"),
    ), events=(("PlayTour", ()),))


def write(author):
    with author.region(TOUR, "Start: the tour takes off with the scene when PlayOnStart holds", EVENTS) as b:
        start = b.event("Event.Create")
        play = b.branch(b.var("PlayOnStart"))
        b.chain(start, play)
        b.flow(play["True"], b.send("PlayTour", b.self_()))
    with author.region(TOUR, "Tour: a timeline flies the camera round the lap and home", EFFECTS) as b:
        fly = b.custom("PlayTour")
        tour = b.timeline(timeline(), comment="Eye is where the camera flies and LookAt where it looks (Tools/Tour.py)")
        place = b.call("Set:Strada.TransformComponent.Translation", Value=tour["Eye"])
        aim = b.call("Set:Strada.TransformComponent.EulerAngles", Value=b.expr(LOOK, target=tour["LookAt"], eye=tour["Eye"]))
        home = b.set("Tours", b.expr("tours + 1", tours=b.var("Tours")))
        again = b.branch(b.var("Repeat"))
        b.flow(fly, tour["PlayFromStart"])
        b.chain(tour["Update"], place, aim)
        b.flow(tour["Finished"], home)
        b.chain(home, again)
        b.flow(again["True"], tour["PlayFromStart"])


if __name__ == "__main__":
    print(check())

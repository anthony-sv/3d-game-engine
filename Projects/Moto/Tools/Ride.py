"""Moto's ride part: the Ride entity and its graph (Scripts/Ride.sgraph), which keeps the ridden bike's place on the lap and
its telemetry, and rides it round the racing line when its autopilot is on.

The racing line (RacingLine.py) is given to the graph as arrays, private to it (no other graph reads them, so the inspector,
which reads a selected entity's public lists back whole on every draw while a game plays, never copies them): Line, its
points every SPACING meters of the line's own length (where the bike's origin rides: on the road, the line's height over
it), LineDistance, the center line's lap distance at each (meters from the start and finish line, as Circuit.py measures the
lap and its gates), LineSpeed, the speed the autopilot rides each at (AUTO_GRIP and AUTO_BRAKING: the line's own speeds
are a point mass's at the tyres' peak, which a rider's bike does not reach; profile()), and LineCurvature, the line's
curvature (1/m, positive to the right).

Each fixed step, whoever rides:

- the graph finds the line's point nearest the bike: from the last one on, up to TRACK_STEPS points further while each is
  nearer (the bike rides at most one point a step), and the whole line again when the bike moved more than JUMP meters
  since the last step (placed elsewhere: no ride covers that in a step), never merely for riding far from the line (a gravel
  trap is 20 m from it);
- Distance (a read-out) is the bike's foot on the line between that point and the next, along the lap (MotoLib's
  LineDistance), and Offset (a read-out) how far right of the line it rides (meters, negative to the left);
- while the editor records telemetry, it records Ride.Distance (meters along the lap, which the Telemetry panel plots laps
  against), Ride.Speed (km/h) and Ride.Offset (meters).

The autopilot (its switch Autopilot is off, so that Moto starts as it does without one: the player rides; StartAutopilot
and StopAutopilot turn it on and off, and a scene may start with it on) rides by the simulation alone, from the same fixed
step (Docs/Architecture.md section 15, rule of the rides compared across frame rates): it sets the riding graph's Driver to
1 and writes its public Throttle, FrontBrake, RearBrake and Lean, which the riding graph gives the vehicle through the rider
assists (Vehicle.py), never SetDriverInput itself, and the assists shift for it. It steers onto a curvature: the line's own
CURVE_LEAD seconds ahead (LineCurvature) times UNDERSTEER (the bike's turn builds behind its steering, so it asks for more
than the line's curvature: found by probing the ride), less its heading's error from the line's over HEADING_TIME seconds of its speed, where the heading it aims for
turns it back onto the line from its offset over OFFSET_TIME seconds (at most OFFSET_HEADING radians); and it asks the
assist for the lean that turns on that curvature (MotoLib's CurvatureLean), whose steering the assist gives. It holds the
speed of the line LEAD_TIME ahead (LineSpeed times Pace): the throttle opens by THROTTLE_GAIN
for each m/s below it, closed exactly (0) at or above it, and the front brake goes on by BRAKE_GAIN for each m/s above it
past BRAKE_MARGIN, at most BRAKE_MAX (the linked brakes add the rear). Which of the two graphs runs first in a step is the
scene's order of its graphs, the same in every run, so the ride is the same at every frame rate.

    python Projects/Moto/Tools/Ride.py      (prints the line's arrays' figures and the autopilot's predicted ride)
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Bike  # noqa: E402  (the grid)
import Circuit  # noqa: E402  (the layout)
import Graphs  # noqa: E402  (writing graphs)
import MotoLib  # noqa: E402  (the library the ride calls)
import RacingLine  # noqa: E402  (the line)
import Vehicle  # noqa: E402  (the ridden bike and its riding graph)
from Graphs import EFFECTS, ENTITY, EVENTS, RULES, private, public  # noqa: E402

RIDE = Graphs.script("Ride")
ROOT = "Ride"
VECTOR = "Strada.Vector3"
BIKE = Vehicle.BIKE

# The line's arrays: a point every SPACING meters of the line's own length.
SPACING = 2.0
# The autopilot's speeds: the most sideways grip it rides a corner at (in g) and the braking it slows for one at (m/s^2,
# besides the air's and the road's drag), within what the bike's tyres give (Vehicle.py): a hard front brake at speed sets
# the bike weaving, so it rides the line at a rider's careful pace rather than the point mass's, and its brake goes on
# gently (BRAKE_GAIN, BRAKE_MAX) for the same reason. The feel pass (Docs/Architecture.md section 15) raised the grip from
# 0.7 by probing whole laps: at 0.8 the laps stay on the line and the road, leaned 11 degrees at most; at 0.85 and 0.9 the
# exits run onto the grass, at 1.0 every exit does, and braking at 4.5 overshoots Turn 1.
AUTO_GRIP = 0.8
AUTO_BRAKING = 4.0
# How it steers (above): the line's curvature it takes from CURVE_LEAD seconds ahead, and how fast it turns its heading back
# onto the line's (HEADING_TIME) and the heading it takes back onto the line from an offset (OFFSET_TIME, at most
# OFFSET_HEADING radians), at a speed of STEER_SPEED at least. How it holds its speed: the lead of the speed it holds
# (seconds), the throttle's and the brake's gains (per m/s) and the brake's margin (m/s) and limit.
CURVE_LEAD = 0.3
UNDERSTEER = 1.3
HEADING_TIME = 0.4
OFFSET_TIME = 1.2
OFFSET_HEADING = 0.3
STEER_SPEED = 5.0
LEAD_TIME = 0.5
THROTTLE_GAIN = 0.5
BRAKE_GAIN = 0.4
BRAKE_MARGIN = 0.5
BRAKE_MAX = 0.45
# The pace: the share of LineSpeed it rides at.
PACE = 1.0
# Following the bike: points it may move on in a step, and how far the bike may move in a step before the whole line is
# searched (at 80 m/s it rides 1.3 m a step).
TRACK_STEPS = 3
JUMP = 5.0

# The telemetry the graph records while the editor records.
CHANNELS = ("Ride.Distance", "Ride.Speed", "Ride.Offset")


def _resampled():
    """The line every SPACING meters of its own length: (x, y, z, lap distance, curvature, the line's speed) at each point,
    from the start and finish line's point on."""
    line = RacingLine.LINE
    lengths = RacingLine._LENGTHS
    total = sum(lengths)
    count = int(round(total / SPACING))
    step = total / count
    points = []
    i = 0
    for k in range(count):
        target = k * step
        while i + 1 < len(line) and line[i + 1].distance <= target:
            i += 1
        a, b = line[i], line[(i + 1) % len(line)]
        span = lengths[i]
        t = (target - a.distance) / span if span > 0.0 else 0.0
        s_b = b.s if b.s >= a.s else b.s + Circuit.LENGTH
        points.append((a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.s + (s_b - a.s) * t,
                       a.curvature + (b.curvature - a.curvature) * t, a.speed + (b.speed - a.speed) * t))
    return points, step


POINTS, STEP = _resampled()
COUNT = len(POINTS)


def profile(grip=AUTO_GRIP, braking=AUTO_BRAKING):
    """The autopilot's speed at each point (m/s): the line's corners at the grip, never faster than the line's own speed
    there (the point mass's, whose drive is the engine's), slowed for at the braking, round the lap three times so the lap
    closes (the line's own drag and rolling resistance, RacingLine.resistance)."""
    limits = [min(math.sqrt(grip * RacingLine.GRAVITY / abs(p[4])) if p[4] != 0.0 else math.inf, p[5]) for p in POINTS]
    speeds = list(limits)
    slowest = min(range(COUNT), key=lambda index: limits[index])
    for _ in range(3):
        for k in range(COUNT):
            j = (slowest - k) % COUNT
            i = (j - 1) % COUNT
            v = speeds[j]
            speeds[i] = min(speeds[i], math.sqrt(v * v + 2.0 * (braking + RacingLine.resistance(v)) * STEP))
    return speeds


SPEEDS = profile()


def arrays():
    """The graph's arrays: the line's points, their lap distances, the autopilot's speeds and the line's curvature (1/m,
    positive to the right, as the riding graph's lean), rounded as the graph holds them."""
    line = [[round(x, 3), round(y, 3), round(z, 3)] for x, y, z, _, _, _ in POINTS]
    distances = [round(s % Circuit.LENGTH, 3) for _, _, _, s, _, _ in POINTS]
    curvatures = [round(-kappa, 6) + 0.0 for _, _, _, _, kappa, _ in POINTS]
    return line, distances, [round(v, 3) for v in SPEEDS], curvatures


def predict(seconds, start=None, step=1.0 / 60.0):
    """The autopilot's ride from rest as a point mass on the line (the speed each point gives, reached at the bike's drive
    in first to sixth: Vehicle.py's engine and gearbox at the rear tyre's sliding grip, and slowed for at AUTO_BRAKING):
    the lap distance covered after the seconds (meters along the center line). start: the line's index it sets out from
    (the grid's nearest by default)."""
    index = nearest(Bike.grid_place()[0]) if start is None else start
    position = 0.0
    speed = 0.0
    line_length = STEP * COUNT
    time = 0.0
    while time < seconds - 1e-9:
        k = int(position / STEP)
        i = (index + k) % COUNT
        lead = (i + max(1, int(round(speed * LEAD_TIME / STEP)) + 1)) % COUNT
        target = SPEEDS[lead] * PACE
        if speed < target:
            gear = max(1, next((g for g in range(1, 7) if Vehicle.road_speed(Vehicle.SHIFT_UP, g) > speed), 6))
            engine = max(RacingLine.IDLE, speed / Bike.REAR_RADIUS * Vehicle.overall(gear) * 30.0 / math.pi)
            turning = Vehicle.MASS + (Vehicle.ENGINE_INERTIA * Vehicle.overall(gear) ** 2 + Vehicle.REAR_INERTIA) / \
                Bike.REAR_RADIUS ** 2 + Vehicle.FRONT_INERTIA / Bike.FRONT_RADIUS ** 2
            drive = Vehicle.engine_torque(min(engine, RacingLine.RED_LINE)) * Vehicle.overall(gear) / Bike.REAR_RADIUS / turning
            accel = min(drive, Vehicle.drive_limit(Vehicle.SLIDING_FRICTION)) - Vehicle.resistance(speed, Vehicle.ASPHALT_ROLLING)
            speed = min(target, speed + accel * step)
        else:
            speed = max(target, speed - AUTO_BRAKING * step)
        position += speed * step
        time += step
    # The line's meters as the center line's: the lap distances of the points it covered.
    k = int(position / STEP)
    first, last = POINTS[index][3], POINTS[(index + k) % COUNT][3]
    covered = (last - first) % Circuit.LENGTH
    if position >= line_length:
        covered += Circuit.LENGTH * int(position // line_length)
    return covered


def nearest(point):
    """The line's index nearest a world point, on the ground."""
    return min(range(COUNT), key=lambda k: (POINTS[k][0] - point[0]) ** 2 + (POINTS[k][2] - point[2]) ** 2)


def check():
    """Asserts the arrays: the points SPACING apart and on the road, the distances along the lap rising but once, within
    the graph's limit of elements, the speeds within the line's own and above a walk; returns their figures."""
    problems = []
    line, distances, speeds, _ = arrays()
    if COUNT > 4096:
        problems.append(f"the line has {COUNT} points, more than a graph's array holds")
    gaps = [math.dist(POINTS[k][::2], POINTS[(k + 1) % COUNT][::2]) for k in range(COUNT)]
    if max(gaps) > 1.05 * STEP or min(gaps) < 0.95 * STEP:
        problems.append(f"the points lie {min(gaps):.3f} to {max(gaps):.3f} m apart, not about {STEP:.3f}")
    falls = sum(1 for k in range(COUNT - 1) if distances[k + 1] < distances[k])
    if falls != 0:
        problems.append(f"the lap distances fall {falls} times before the line's end")
    line_speeds = [p.speed for p in RacingLine.LINE]
    if max(speeds) > max(line_speeds) + 1e-6 or min(speeds) < 10.0:
        problems.append("the autopilot's speeds are not within the line's and above 10 m/s")
    assert not problems, "; ".join(problems)
    seconds = sum(STEP / v for v in SPEEDS)
    return (f"{COUNT} points {STEP:.3f} m apart over the line's {STEP * COUNT:.1f} m; the autopilot's speeds "
            f"{min(SPEEDS):.1f} to {max(SPEEDS):.1f} m/s, a lap at them of {seconds:.1f} s (the line's {RacingLine.LAP_TIME:.1f} s); "
            f"from the grid it covers {predict(20.0):.0f} m in 20 s and {predict(35.0):.0f} m in 35 s")


# --- The graph ---------------------------------------------------------------------------------------------------------------

def declare(author):
    line, distances, speeds, curvatures = arrays()
    author.declare(RIDE, "The ride: each fixed step, the bike's place on the lap and its telemetry, and with the autopilot on, "
                   "the racing line ridden through the riding graph's inputs. Tools/Ride.py writes it.", variables=(
        public("Bike", ENTITY, None, "The ridden bike, whose riding graph the autopilot writes"),
        public("Autopilot", "bool", False, "Whether the autopilot rides the bike from the start (StartAutopilot and "
               "StopAutopilot turn it on and off)"),
        public("Pace", "float", PACE, "The share of the autopilot's speeds it rides at", (0.5, 1.2)),
        public("Distance", "float", 0.0, "Where the bike is on the lap: meters along the center line from the start and finish "
               "line (a read-out)"),
        public("Offset", "float", 0.0, "How far right of the racing line the bike rides, in meters, negative to the left "
               "(a read-out)"),
        private("Line", "Strada.Vector3[]", line),
        private("LineDistance", "float[]", distances),
        private("LineSpeed", "float[]", speeds),
        private("LineCurvature", "float[]", curvatures),
        private("Index", "int", 0),
        private("LastAt", VECTOR),
    ), events=(("StartAutopilot", ()), ("StopAutopilot", ())), functions=(
        ("Find", False, (), (), (("Best", "float", 1.0e9),)),
        ("Track", False, (), (), ()),
    ))


def bike_at(b):
    return b.once("bike at", lambda: b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=b.var("Bike")))


def point(b, index, key):
    return b.once(("point", key), lambda: b.add("Array.Get", None, "Element", Array=b.var("Line"), Index=index).out)


def ground_distance(b, a, c):
    return b.expr("distance(vec2(a.X, a.Z), vec2(c.X, c.Z))", a=a, c=c)


def write_find(b):
    best = b.set("Best", 1.0e9)
    each = b.add("Flow.ForLoop", First=0, Last=COUNT - 1)
    at = b.add("Array.Get", None, "Element", Array=b.var("Line"), Index=each["Index"]).out
    near = b.expr("distance(vec2(p.X, p.Z), vec2(q.X, q.Z))", p=bike_at(b), q=at)
    closer = b.branch(b.expr("d < best", d=near, best=b.var("Best")))
    b.chain(b.entry, best, each)
    b.flow(each["Body"], closer)
    b.chain(closer["True"], b.set("Best", near), b.set("Index", each["Index"]))
    b.chain(each["Completed"], b.set("LastAt", bike_at(b), comment="Where the bike was searched from"), b.ret)


def write_track(b):
    each = b.add("Flow.ForLoop", First=1, Last=TRACK_STEPS)
    following = b.expr(f"(index + 1) % {COUNT}", index=b.var("Index"))
    ahead = b.add("Array.Get", None, "Element", Array=b.var("Line"), Index=following).out
    here = b.add("Array.Get", None, "Element", Array=b.var("Line"), Index=b.var("Index")).out
    nearer = b.branch(b.expr("distance(vec2(p.X, p.Z), vec2(n.X, n.Z)) < distance(vec2(p.X, p.Z), vec2(h.X, h.Z))", p=bike_at(b),
                             n=ahead, h=here), comment="The next point is nearer: the bike has passed this one")
    jumped = b.branch(b.expr(f"distance(vec2(p.X, p.Z), vec2(last.X, last.Z)) > {JUMP}", p=bike_at(b), last=b.var("LastAt")),
                      comment="Placed elsewhere since the last step: the whole line")
    moved = b.set("LastAt", bike_at(b))
    b.chain(b.entry, jumped)
    b.chain(jumped["True"], b.own("Find"), b.ret)
    b.chain(jumped["False"], moved, each)
    b.flow(each["Body"], nearer)
    b.flow(nearer["True"], b.set("Index", following))
    b.flow(each["Completed"], b.ret)


def write(author):
    with author.region(RIDE, "Find: the line's point nearest the bike, searched whole", RULES, "Find") as b:
        write_find(b)
    with author.region(RIDE, "Track: the nearest point, followed on from the last", RULES, "Track") as b:
        write_track(b)
    with author.region(RIDE, "Start: the bike's place on the line; the autopilot if it starts on", EVENTS) as b:
        on = b.branch(b.var("Autopilot"), comment="Off: the player rides, as Moto starts")
        b.chain(b.event("Event.Create"), b.own("Find"), on)
        b.flow(on["True"], b.set("Driver", 1, graph=BIKE, target=b.var("Bike")))
    with author.region(RIDE, "Autopilot: StartAutopilot and StopAutopilot turn it on and off", EFFECTS) as b:
        start = b.custom("StartAutopilot")
        b.chain(start, b.set("Autopilot", True), b.set("Driver", Vehicle.AUTOPILOT, graph=BIKE, target=b.var("Bike")), b.own("Find"))
        stop = b.custom("StopAutopilot")
        released = [b.set(name, 0.0, graph=BIKE, target=b.var("Bike")) for name in ("Throttle", "FrontBrake", "RearBrake", "Lean")]
        b.chain(stop, b.set("Autopilot", False), *released, b.set("Driver", Vehicle.PLAYER, graph=BIKE, target=b.var("Bike")))
    with author.region(RIDE, "Ride: each fixed step, the bike's place, its telemetry and the autopilot", EVENTS) as b:
        steps = b.sequence(3)
        index = b.var("Index")
        following = b.expr(f"(index + 1) % {COUNT}", index=index)
        here = b.add("Array.Get", None, "Element", Array=b.var("Line"), Index=index).out
        ahead = b.add("Array.Get", None, "Element", Array=b.var("Line"), Index=following).out
        start = b.add("Array.Get", None, "Element", Array=b.var("LineDistance"), Index=index).out
        end = b.expr(f"next == 0 ? {round(Circuit.LENGTH, 3)} : distance", next=following,
                     distance=b.add("Array.Get", None, "Element", Array=b.var("LineDistance"), Index=following).out)
        foot = b.library(MotoLib.LIB, "LineDistance", output="Distance", Position=bike_at(b), From=here, To=ahead, FromDistance=start,
                         ToDistance=end).out
        placed = b.set("Distance", foot)
        offset = b.set("Offset", b.expr("dot(vec2(p.X - a.X, p.Z - a.Z), normalize(vec2(a.Z - c.Z, c.X - a.X)))", p=bike_at(b),
                                        a=here, c=ahead))
        b.chain(b.event("Event.FixedUpdate"), b.own("Track"), steps)
        b.chain(steps["Then0"], placed, offset)
        recording = b.branch(b.pure("Get:Strada.Telemetry.IsRecording", "Value"), comment="Only while the editor records")
        speed = b.pure("Get:Strada.VehicleComponent.ForwardSpeed", "Value", Target=b.var("Bike"))
        records = [b.call("Call:Strada.Telemetry.Record(string,float)", Channel=channel, Value=value) for channel, value in (
            ("Ride.Distance", b.var("Distance")), ("Ride.Speed", b.library(MotoLib.LIB, "Kmh", output="Kmh", Speed=speed).out),
            ("Ride.Offset", b.var("Offset")))]
        b.flow(steps["Then1"], recording)
        b.chain(recording["True"], *records)
        riding = b.branch(b.var("Autopilot"), comment="The autopilot rides by the simulation alone")
        ahead_curve = b.expr(f"(index + clamp(int(round(v * {CURVE_LEAD} / {round(STEP, 6)})), 0, 40)) % {COUNT}", index=index, v=speed)
        line_curve = b.add("Array.Get", None, "Element", Array=b.var("LineCurvature"), Index=ahead_curve).out
        forward = b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=b.var("Bike"))
        # The heading's error from the line's (radians, positive to its right) and the heading the offset asks for.
        curvature = b.expr(
            f"curve * {UNDERSTEER} - (atan2(dot(vec2(f.X, f.Z), normalize(vec2(a.Z - c.Z, c.X - a.X))), dot(vec2(f.X, f.Z), normalize(vec2(c.X - a.X, "
            f"c.Z - a.Z)))) + clamp(offset / (max(v, {STEER_SPEED}) * {OFFSET_TIME}), -{OFFSET_HEADING}, {OFFSET_HEADING})) / "
            f"(max(v, {STEER_SPEED}) * {HEADING_TIME})", curve=line_curve, f=forward, a=here, c=ahead, offset=b.var("Offset"), v=speed)
        lean = b.library(MotoLib.LIB, "CurvatureLean", output="Request", Curvature=curvature, Speed=speed).out
        lead = b.expr(f"(index + clamp(int(round(v * {LEAD_TIME} / {round(STEP, 6)})) + 1, 1, 40)) % {COUNT}", index=index, v=speed)
        wanted = b.expr("speed * pace", speed=b.add("Array.Get", None, "Element", Array=b.var("LineSpeed"), Index=lead).out,
                        pace=b.var("Pace"))
        bike = b.var("Bike")
        sets = [
            b.set("Lean", lean, graph=BIKE, target=bike, comment="The lean that turns on the arc to the point ahead"),
            b.set("Throttle", b.expr(f"clamp((wanted - v) * {THROTTLE_GAIN}, 0, 1)", wanted=wanted, v=speed), graph=BIKE, target=bike,
                  comment="Closed exactly at the speed or above it"),
            b.set("FrontBrake", b.expr(f"clamp((v - wanted - {BRAKE_MARGIN}) * {BRAKE_GAIN}, 0, {BRAKE_MAX})", wanted=wanted, v=speed),
                  graph=BIKE, target=bike),
            b.set("RearBrake", 0.0, graph=BIKE, target=bike, comment="The linked brakes give the rear its share"),
        ]
        b.flow(steps["Then2"], riding)
        b.chain(riding["True"], *sets)


if __name__ == "__main__":
    print(check())

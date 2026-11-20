"""Moto's ghost bike: a second motorcycle and rider, of the bike part's meshes and materials (so that the renderer draws
both in the same batches), that rides the racing line (RacingLine.py) round the lap without end while the scene plays, and
the best lap the ridden bike made once there is one. Author.py's ghost part makes it: the Ghost entity at the line's pose
where it starts, with the bike's parts under it as Bike.ENTITIES has them, and its graph (Scripts/Ghost.sgraph). It has no
rigid body and no collider: nothing simulates it and nothing touches it; its graph places it every update (kinematic, in
the plain sense).

The graph (declare() and write()): when the scene starts, the ghost finds its engine's sound (the child that runs
Scripts/EngineSound.sgraph, EngineSound.py) and sends itself Ride, which moves its timeline to StartAt (seconds after the
start and finish line) and plays it. The timeline loops over one lap of the line's keys: every update places the ghost at
Position and turns it to Heading (about +Y), leaned by Lean (positive to the right: a turn about its forward axis),
counts a lap when the timeline's time comes round past the line, keeps the read-outs other graphs follow (LapTime, and
the engine's EngineSpeed and Load) and sets the engine sound's EngineSpeed and Load, which it plays from.

The best lap (declare_best_lap() and write_best_lap(), regions of their own, so that the graph without them is the racing
line's ghost alone). The race director (slice 4's race part) broadcasts LapStarted(Lap) when the ridden bike (Bike) begins a
lap and LapFinished(Lap, Time, Best) when it ends one, Best when the lap is the race's best. From LapStarted the ghost
records the bike every SAMPLE_PERIOD seconds of fixed steps (SAMPLE_STEPS steps of Moto's 1/60 s: ten samples a second, so
that a lap of up to MAX_SAMPLES samples, 409.5 s, fits one list): its position, its rotation, its engine's speed and its
applied throttle (the load), the first sample in its first fixed step at or after LapStarted (the step LapStarted arrives in
when the ghost's graph runs after the race director's within a step, the next when before), so that its samples lie
SAMPLE_STEPS steps apart from the first whatever the order, and the lap's time the ghost measures starts there. At LapFinished of that lap with Best, the
ghost keeps the samples as its best lap, public arrays (BestLapPositions, BestLapRotations, BestLapEngineSpeeds,
BestLapLoads, a value per sample) with BestLapTime, and from then on rides it: every update it stands where the samples put
it at its time on the lap (linear between two samples, the rotation spherical, and from the last sample back to the first
over the rest of the lap), sets its read-outs and its engine sound from them, sets out on the lap again at each LapStarted
(beside the bike) and goes round it without end otherwise (the attract mode). A lap of more samples than MaxSamples (a
public bound, MAX_SAMPLES by default) is timed but not kept: a best lap the ghost cannot hold clears the one it had, so that
nothing compares the race with a lap that is not its best, and the ghost rides the racing line again. While a lap is
recorded and a best lap is held, each fixed step sets Delta: the lap's time so far less the best lap's time at the same
place (the bike's foot on the polyline of the best lap's positions, followed from the sample nearest it, up to TRACK_SAMPLES
samples on a step), negative when ahead. A game that keeps the best lap (GF's save slot) writes the four arrays and
BestLapTime back and sends BestLapLoaded: the ghost rides them when they hold one lap (as many values in each, two to
MAX_SAMPLES, and a time their count fits), and otherwise clears them and rides the line. A scene that starts with them set
is ridden the same way.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import EngineSound  # noqa: E402  (the engine's sound)
import Graphs  # noqa: E402  (writing graphs)
import RacingLine  # noqa: E402  (the line the ghost rides)
from Graphs import EFFECTS, ENTITY as ENTITY_TYPE, EVENTS, RULES, private, public  # noqa: E402

GHOST = Graphs.script("Ghost")
# The Ghost entity's name, and where the main scene's ghost sets out (seconds after the start and finish line): at the
# line itself.
ENTITY = "Ghost"
START_AT = 0.0
# The test scene's ghost sets out this far into its lap, so that it leans through Turn 6 early in the test run and
# crosses the line well before the camera tour ends (Checks.py).
TEST_START_AT = 30.0
ANGLE_AXIS = "Call:Strada.Quaternion.AngleAxis(float,Strada.Vector3)"
SLERP = "Call:Strada.Quaternion.Slerp(Strada.Quaternion,Strada.Quaternion,float)"
VECTOR = "Strada.Vector3"

# --- The best lap -----------------------------------------------------------------------------------------------------------

# The samples' rate (a second) and the most a list holds (FR-R54.7): a lap of up to (MAX_SAMPLES - 1) / SAMPLE_RATE seconds
# is kept. The graph takes a sample every SAMPLE_STEPS fixed steps, the steps nearest SAMPLE_RATE a second (6 at Moto's
# fixed step of 1/60 s, Moto.sproj), and its period is those steps' time.
SAMPLE_RATE = 10.0
MAX_SAMPLES = 4096
FIXED_STEP = 1.0 / 60.0
SAMPLE_STEPS = round(1.0 / (SAMPLE_RATE * FIXED_STEP))
SAMPLE_PERIOD = SAMPLE_STEPS * FIXED_STEP
# How many samples Delta's place may move on in a fixed step (at the bike's top speed of 80 m/s it rides 1.3 m a step, and
# samples are 0.1 s apart).
TRACK_SAMPLES = 3
# The best lap's public arrays (name, type, what each value is) and the recording's private ones, in that order.
BEST_LAP = (("BestLapPositions", "Strada.Vector3[]", "positions in the world"),
            ("BestLapRotations", "Strada.Quaternion[]", "rotations in the world"),
            ("BestLapEngineSpeeds", "float[]", "engine speeds (revolutions a minute)"),
            ("BestLapLoads", "float[]", "loads (the applied throttle, 0 to 1)"))
RECORDING = (("Positions", "Strada.Vector3[]"), ("Rotations", "Strada.Quaternion[]"), ("EngineSpeeds", "float[]"), ("Loads", "float[]"))
# The race director's broadcasts the ghost handles (slice 4's spec section 12) and the event a game sends it.
LAP_STARTED = ("LapStarted", (("Lap", "int"),))
LAP_FINISHED = ("LapFinished", (("Lap", "int"), ("Time", "float"), ("Best", "bool")))
LOADED = ("BestLapLoaded", ())


def samples(seconds):
    """How many samples a lap of the seconds takes (the first at its start)."""
    return int(seconds / SAMPLE_PERIOD + 1e-9) + 1


def holds(count, seconds):
    """Whether samples of a count hold a lap of the seconds, as the graph's BestLapLoaded requires: from two to MAX_SAMPLES,
    the last within the lap, and no whole period left after it."""
    return 2 <= count <= MAX_SAMPLES and (count - 1.5) * SAMPLE_PERIOD <= seconds < (count + 1) * SAMPLE_PERIOD


def _about(axis, degrees):
    half = math.radians(degrees) / 2.0
    return tuple(component * math.sin(half) for component in axis) + (math.cos(half),)


def _multiply(a, b):
    """The product of two quaternions (x, y, z, w): b's rotation, then a's."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def rotation(heading, lean):
    """The ghost's rotation (x, y, z, w) for a heading about +Y and a lean to the right (degrees), as its graph makes it:
    the heading's turn after the lean's, which is a turn of -lean about its forward axis' opposite (+Z)."""
    return _multiply(_about((0.0, 1.0, 0.0), heading), _about((0.0, 0.0, -1.0), lean))


def transform(start_at=START_AT):
    """The Ghost entity's transform where it sets out."""
    position, heading, lean, _, _ = RacingLine.pose(start_at)
    return {"Translation": [round(value, 4) for value in position],
            "Rotation": [round(value, 7) + 0.0 for value in rotation(heading[0], lean[0])], "Scale": [1.0, 1.0, 1.0]}


def declare_best_lap():
    """The best lap's part of the graph's interface: its variables, events and functions."""
    variables = (
        public("Bike", ENTITY_TYPE, None, "The ridden bike, whose laps the ghost records from the race director's LapStarted and "
               "keeps at its LapFinished when it is the best"),
    ) + tuple(public(name, type_id, [], f"The best lap's {about}, a value per sample, ten a second (empty while there is none; "
                     "a game that loads them sends BestLapLoaded)") for name, type_id, about in BEST_LAP) + (
        public("BestLapTime", "float", 0.0, "The best lap's time in seconds, 0 while there is none (no Delta then); a game that "
               "loads the samples sets it with them", (0.0, (MAX_SAMPLES + 1) * SAMPLE_PERIOD)),
        public("Delta", "float", 0.0, "The lap in progress against the best lap at the same place, in seconds, negative when "
               "ahead (a read-out)"),
        public("MaxSamples", "int", MAX_SAMPLES, "The most samples (ten a second) a lap may take and still be kept as the ghost: "
               "a slower lap is timed but not kept", (2, MAX_SAMPLES)),
        public("RidesBestLap", "bool", False, "Whether the ghost rides its best lap rather than the racing line (a read-out)"),
    ) + tuple(private(name, type_id, []) for name, type_id in RECORDING) + (
        private("Recording", "bool", False), private("PendingLap", "int", 0), private("RecordingLap", "int", 0),
        private("LapSteps", "int", 0),
        private("Overflowed", "bool", False), private("SampleSteps", "int", SAMPLE_STEPS), private("Nearest", "int", 0),
        private("ReplayTime", "float", 0.0),
    )
    events = (("RideBestLap", ()), LAP_STARTED, LAP_FINISHED, LOADED)
    functions = (
        ("Sample", False, (), (), ()),
        ("ClearBestLap", False, (), (), ()),
        ("Measure", False, (), (), ()),
    )
    return variables, events, functions


def declare(author):
    lap = RacingLine.length()
    best_variables, best_events, best_functions = declare_best_lap()
    author.declare(GHOST, "The ghost bike: it rides the racing line round the lap without end, from StartAt seconds after "
                   "the start and finish line (Tools/RacingLine.py makes its keys), and the ridden bike's best lap once it "
                   "has one, which it records and keeps at the race director's events.", variables=(
        public("StartAt", "float", START_AT, "Where on its lap the ghost sets out, in seconds after the start and finish line",
               (0.0, lap)),
        public("Laps", "int", 0, "How many times the ghost has crossed the start and finish line (a read-out)"),
        public("LapTime", "float", 0.0, "How far into its lap the ghost is, in seconds after the start and finish line (a read-out)"),
        public("EngineSpeed", "float", 0.0, "The ghost's engine speed in revolutions a minute, for its engine's sound (a read-out)"),
        public("Load", "float", 0.0, "How far open the ghost's throttle is, from 0 to 1, for its engine's sound (a read-out)"),
        private("Engine", ENTITY_TYPE),
    ) + best_variables, events=(("Ride", ()),) + best_events, functions=best_functions)


def write(author):
    with author.region(GHOST, "Start: the ghost finds its engine's sound and sets out on its lap", EVENTS) as b:
        each = b.add("Flow.ForEach", Array=b.pure("Get:Strada.Entity.Children", "Value", Target=b.self_()))
        sounds = b.branch(b.runs(EngineSound.ENGINE_SOUND, each["Element"]))
        steps = b.set("SampleSteps", b.expr(f"max(1, int(round(1 / ({SAMPLE_RATE} * dt))))",
                                            dt=b.pure("Get:Strada.Time.FixedDeltaTime", "Value")),
                      comment="The fixed steps between two samples of a lap: ten a second")
        held = b.branch(b.expr("time > 0", time=b.var("BestLapTime")), comment="A scene that starts with a best lap rides it")
        b.chain(b.event("Event.Create"), each)
        b.chain(each["Body"], sounds)
        b.flow(sounds["True"], b.set("Engine", each["Element"]))
        b.chain(each["Completed"], steps, b.send("Ride", b.self_()), held)
        b.flow(held["True"], b.send(LOADED[0], b.self_()))
    with author.region(GHOST, "Ride: the racing line's timeline, stopped while a best lap is ridden", EFFECTS) as b:
        ride = b.custom("Ride")
        steps = b.sequence(2)
        line = b.timeline(RacingLine.timeline(), comment="The racing line's keys (Tools/RacingLine.py): where the ghost is, "
                          "its heading and lean, its engine speed and load")
        b.link(b.var("StartAt"), line["NewTime"])
        b.flow(ride, steps)
        b.flow(steps["Then0"], line["SetTime"])
        b.flow(steps["Then1"], line["Play"])
        place = b.call("Set:Strada.TransformComponent.Translation", Value=line["Position"])
        heading = b.pure(ANGLE_AXIS, Degrees=line["Heading"], Axis=[0.0, 1.0, 0.0])
        lean = b.pure(ANGLE_AXIS, Degrees=line["Lean"], Axis=[0.0, 0.0, -1.0])
        turn = b.call("Set:Strada.TransformComponent.Rotation", comment="The heading's turn after the lean's",
                      Value=b.pure("Operator.Multiply", A=heading, B=lean))
        lapped = b.branch(b.expr("time < last", time=line["Time"], last=b.var("LapTime")),
                          comment="The time came round past the start and finish line")
        count = b.set("Laps", b.expr("laps + 1", laps=b.var("Laps")))
        mark = b.set("LapTime", line["Time"])
        engine = b.set("EngineSpeed", line["EngineSpeed"])
        load = b.set("Load", line["Load"])
        sound = b.var("Engine")
        heard = b.branch(b.pure("IsValid", Value=sound), comment="A ghost without an engine's sound plays none")
        sound_speed = b.set("EngineSpeed", line["EngineSpeed"], graph=EngineSound.ENGINE_SOUND, target=sound)
        sound_load = b.set("Load", line["Load"], graph=EngineSound.ENGINE_SOUND, target=sound)
        b.chain(line["Update"], place, turn, lapped)
        b.flow(lapped["True"], count)
        b.flow(lapped["False"], mark)
        b.chain(count, mark, engine, load, heard)
        b.chain(heard["True"], sound_speed, sound_load)
        b.flow(b.custom("RideBestLap"), line["Stop"])
    write_best_lap(author)


# --- The best lap's regions ----------------------------------------------------------------------------------------------------

def element(b, array, index):
    return b.add("Array.Get", None, "Element", Array=b.var(array), Index=index).out


def bike_at(b):
    return b.once("bike at", lambda: b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=b.var("Bike")))


def count(b):
    return b.once("count", lambda: b.pure("Array.Length", "Length", Array=b.var(BEST_LAP[0][0])))


def period(b):
    return b.once("period", lambda: b.expr("float(steps) * dt", steps=b.var("SampleSteps"),
                                            dt=b.pure("Get:Strada.Time.FixedDeltaTime", "Value")))


def write_sample(b):
    """Sample: the bike's position, rotation, engine speed and load added to the lap's recording, or the recording marked
    too long for the ghost once it holds MaxSamples."""
    full = b.branch(b.expr("length >= most", length=b.pure("Array.Length", "Length", Array=b.var(RECORDING[0][0])),
                           most=b.var("MaxSamples")), comment="A lap longer than the ghost keeps is timed, not kept")
    bike = b.var("Bike")
    values = (bike_at(b), b.pure("Get:Strada.TransformComponent.Rotation", "Value", Target=bike),
              b.pure("Get:Strada.VehicleComponent.EngineSpeed", "Value", Target=bike),
              b.pure("Get:Strada.VehicleComponent.Throttle", "Value", Target=bike))
    added = [b.set(name, b.pure("Array.Add", Array=b.var(name), Value=value)) for (name, _), value in zip(RECORDING, values)]
    b.chain(b.entry, full)
    b.chain(full["True"], b.set("Overflowed", True), b.ret)
    b.chain(full["False"], *added, b.ret)


def write_clear(b):
    """Clear best lap: no best lap any more (no Delta), and the racing line ridden again."""
    emptied = [b.set(name, b.lit(type_id, [])) for name, type_id, _ in BEST_LAP]
    riding = b.branch(b.var("RidesBestLap"))
    back = [b.set("RidesBestLap", False), b.set("LapTime", 0.0, comment="The line's lap starts its count again")]
    b.chain(b.entry, *emptied, b.set("BestLapTime", 0.0), b.set("Delta", 0.0), riding)
    b.chain(riding["True"], *back, b.send("Ride", b.self_()), b.ret)
    b.flow(riding["False"], b.ret)


def write_measure(b):
    """Measure: the best lap's sample nearest the bike, followed on from the last (up to TRACK_SAMPLES a step), the bike's
    foot on the best lap's positions there (the segment from that sample, or to it when the bike is still before it), and
    Delta: the lap's time so far less the best lap's time at that foot."""
    each = b.add("Flow.ForLoop", First=1, Last=TRACK_SAMPLES)
    nearest = b.var("Nearest")
    following = b.expr("min(nearest + 1, count - 1)", nearest=nearest, count=count(b))
    here = element(b, BEST_LAP[0][0], nearest)
    ahead = element(b, BEST_LAP[0][0], following)
    nearer = b.branch(b.expr("distance(p, n) < distance(p, h)", p=bike_at(b), n=ahead, h=here),
                      comment="The next sample is nearer: the bike has passed this one")
    b.chain(b.entry, each)
    b.flow(each["Body"], nearer)
    b.flow(nearer["True"], b.set("Nearest", following))
    # After the loop: the segment the foot lies on, and the time there.
    last = b.var("Nearest")
    at = element(b, BEST_LAP[0][0], last)
    after = element(b, BEST_LAP[0][0], b.expr("min(index + 1, count - 1)", index=last, count=count(b)))
    base = b.expr("index > 0 && dot(p - a, c - a) < 0 ? index - 1 : index", index=last, p=bike_at(b), a=at, c=after)
    start = element(b, BEST_LAP[0][0], base)
    end = element(b, BEST_LAP[0][0], b.expr("min(base + 1, count - 1)", base=base, count=count(b)))
    along = b.expr("clamp(dot(p - a, c - a) / max(dot(c - a, c - a), 0.000001), 0, 1)", p=bike_at(b), a=start, c=end)
    delta = b.set("Delta", b.expr("float(steps) * dt - (float(base) + along) * period", steps=b.var("LapSteps"),
                                  dt=b.pure("Get:Strada.Time.FixedDeltaTime", "Value"), base=base, along=along, period=period(b)),
                  comment="Negative when ahead of the best lap")
    b.chain(each["Completed"], delta, b.ret)


def write_best_lap(author):
    with author.region(GHOST, "Sample: the bike's pose, engine and load added to the lap's samples", RULES, "Sample") as b:
        write_sample(b)
    with author.region(GHOST, "Clear best lap: none held, no Delta, the racing line ridden again", RULES, "ClearBestLap") as b:
        write_clear(b)
    with author.region(GHOST, "Measure: Delta, the lap so far against the best lap at the same place", RULES, "Measure") as b:
        write_measure(b)
    with author.region(GHOST, "Lap started: the ridden bike's lap recorded from the next fixed step", EVENTS) as b:
        started = b.custom(LAP_STARTED[0])
        known = b.branch(b.pure("IsValid", Value=b.var("Bike")), comment="A ghost without a bike records nothing")
        pending = [b.set("Recording", False), b.set("PendingLap", started["Lap"],
                                                    comment="The recording starts in this graph's next fixed step")]
        riding = b.branch(b.var("RidesBestLap"))
        b.chain(started, known)
        b.chain(known["True"], *pending, riding)
        b.flow(riding["True"], b.set("ReplayTime", 0.0, comment="The ghost sets out on its best lap beside the bike"))
    with author.region(GHOST, "Lap finished: the best lap kept, or cleared when too long to keep", RULES) as b:
        finished = b.custom(LAP_FINISHED[0])
        mine = b.branch(b.expr("recording && lap == recorded", recording=b.var("Recording"), lap=finished["Lap"],
                               recorded=b.var("RecordingLap")), comment="The lap the ghost recorded")
        best = b.branch(finished["Best"], comment="The race's best lap")
        whole = b.branch(b.expr("!over && n >= 2", over=b.var("Overflowed"),
                                n=b.pure("Array.Length", "Length", Array=b.var(RECORDING[0][0]))),
                         comment="It fits the ghost's samples")
        kept = [b.set(name, b.var(recorded)) for (name, _, _), (recorded, _) in zip(BEST_LAP, RECORDING)]
        timed = [b.set("BestLapTime", finished["Time"]), b.set("Nearest", 0)]
        riding = b.branch(b.var("RidesBestLap"))
        ride = [b.set("RidesBestLap", True), b.set("ReplayTime", 0.0)]
        stopped = b.set("Recording", False)
        clear = b.own("ClearBestLap")
        b.chain(finished, mine)
        b.chain(mine["True"], stopped, best)
        b.chain(best["True"], whole)
        b.chain(whole["True"], *kept, *timed, riding)
        b.chain(riding["False"], *ride, b.send("RideBestLap", b.self_()))
        b.flow(whole["False"], clear)
        unrecorded = b.branch(finished["Best"], comment="A best lap the ghost did not record cannot be ridden")
        b.flow(mine["False"], unrecorded)
        b.flow(unrecorded["True"], clear)
    with author.region(GHOST, "Loaded: samples a game set are ridden when they hold one lap", RULES) as b:
        loaded = b.custom(LOADED[0])
        lengths = [b.pure("Array.Length", "Length", Array=b.var(name)) for name, _, _ in BEST_LAP]
        valid = b.branch(b.expr(f"n >= 2 && n <= {MAX_SAMPLES} && r == n && e == n && l == n && time >= (float(n) - 1.5) * period "
                                "&& time < (float(n) + 1) * period", n=lengths[0], r=lengths[1], e=lengths[2], l=lengths[3],
                                time=b.var("BestLapTime"), period=period(b)),
                         comment="As many values in each array, and a time their count fits")
        ride = [b.set("RidesBestLap", True), b.set("ReplayTime", 0.0), b.set("Nearest", 0)]
        b.chain(loaded, valid)
        b.chain(valid["True"], *ride, b.send("RideBestLap", b.self_()))
        b.flow(valid["False"], b.own("ClearBestLap"))
    with author.region(GHOST, "Record: each fixed step of a lap, a sample every sixth and its Delta", EVENTS) as b:
        starting = b.branch(b.expr("pending > 0", pending=b.var("PendingLap")),
                            comment="The first fixed step at or after LapStarted: the lap's first sample, whichever graph runs "
                                    "first in a step")
        resets = [b.set("Recording", True), b.set("RecordingLap", b.var("PendingLap")), b.set("PendingLap", 0), b.set("LapSteps", 0),
                  b.set("Overflowed", False), b.set("Nearest", 0), b.set("Delta", 0.0)] + \
            [b.set(name, b.lit(type_id, [])) for name, type_id in RECORDING]
        recording = b.branch(b.var("Recording"))
        counted = b.set("LapSteps", b.expr("steps + 1", steps=b.var("LapSteps")))
        due = b.branch(b.expr("steps % every == 0", steps=b.var("LapSteps"), every=b.var("SampleSteps")))
        held = b.branch(b.expr("time > 0 && n >= 2", time=b.var("BestLapTime"), n=count(b)), comment="Against a best lap held")
        sampled = b.own("Sample")
        b.chain(b.event("Event.FixedUpdate"), starting)
        b.chain(starting["True"], *resets, b.own("Sample"))
        b.flow(starting["False"], recording)
        b.chain(recording["True"], counted, due)
        b.chain(due["True"], sampled, held)
        b.flow(due["False"], held)
        b.flow(held["True"], b.own("Measure"))
    with author.region(GHOST, "Replay: each update, the ghost where its best lap's samples put it", EFFECTS) as b:
        riding = b.branch(b.var("RidesBestLap"))
        time = b.var("ReplayTime")
        best = b.var("BestLapTime")
        moved = b.set("ReplayTime", b.expr("time + dt", time=time, dt=b.pure("Get:Strada.Time.DeltaTime", "Value")))
        lapped = b.branch(b.expr("time >= best", time=b.var("ReplayTime"), best=best), comment="Round past the end of its lap")
        wrapped = b.set("ReplayTime", b.expr("time - best", time=b.var("ReplayTime"), best=best))
        laps = b.set("Laps", b.expr("laps + 1", laps=b.var("Laps")))
        now = b.var("ReplayTime")
        index = b.expr("min(int(now / period), n - 1)", now=now, period=period(b), n=count(b))
        following = b.expr("index + 1 < n ? index + 1 : 0", index=index, n=count(b))
        share = b.expr("clamp(index + 1 < n ? (now - float(index) * period) / period : (now - float(index) * period) / "
                       "max(best - float(index) * period, 0.0001), 0, 1)", index=index, n=count(b), now=now, period=period(b),
                       best=best)

        def between(name, kind):
            a, c = element(b, name, index), element(b, name, following)
            if kind == "rotation":
                return b.pure(SLERP, A=a, B=c, T=share)
            return b.expr("a + (c - a) * t", a=a, c=c, t=share)

        position = between(BEST_LAP[0][0], "vector")
        rotation = between(BEST_LAP[1][0], "rotation")
        speed = between(BEST_LAP[2][0], "float")
        load = between(BEST_LAP[3][0], "float")
        place = b.call("Set:Strada.TransformComponent.Translation", Value=position)
        turn = b.call("Set:Strada.TransformComponent.Rotation", Value=rotation)
        readouts = [b.set("LapTime", now), b.set("EngineSpeed", speed), b.set("Load", load)]
        sound = b.var("Engine")
        heard = b.branch(b.pure("IsValid", Value=sound), comment="A ghost without an engine's sound plays none")
        voiced = [b.set("EngineSpeed", speed, graph=EngineSound.ENGINE_SOUND, target=sound),
                  b.set("Load", load, graph=EngineSound.ENGINE_SOUND, target=sound)]
        b.chain(b.event("Event.Update"), riding)
        b.chain(riding["True"], moved, lapped)
        b.chain(lapped["True"], wrapped, laps, place)
        b.flow(lapped["False"], place)
        b.chain(place, turn, *readouts, heard)
        b.chain(heard["True"], *voiced)

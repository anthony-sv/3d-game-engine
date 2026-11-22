"""Whole laps: the module of the lap test scene (Scenes/Lap.sscene, a line of Checks.SCENES), which the player runs behind
STRADA_LONG_RIDES=1 (MotoRideTests.cpp): the main scene without its props, the autopilot riding the bike from the grid as
the scene starts (prepare: the Ride graph's Autopilot on), LAPS laps of it round the whole circuit, and the ghost keeping
the best of them.

It stands in for the race director (Race.py), which it does not run: each fixed step it reads where the bike is on the
lap (the Ride graph's Distance) and, when the bike crossed the start and finish line since the step before (its distance
came round from the lap's last CROSSING meters to its first), broadcasts LapFinished(Lap, Time, Best) for the lap it ends
(Best when it is the faster) and LapStarted(Lap) for the next, as the race director does, the lap's time in fixed steps
since the crossing that began it. It is driven by its own count of steps and by what the bike and the Ride graph read, never
by a frame, and logs every reading in one line ("Moto's laps: ..."). Meanwhile it notes, every step of the laps, how far the
bike rode from the racing line (the Ride graph's Offset), the steps a wheel touched gravel or grass, and the deepest lean,
and ten times a second the lean's share of the lean that balances the turn.

The checks: each lap's time within LAP_BAND of the racing line's own (RacingLine.LAP_TIME, a point mass at the tyres'
grip, which the autopilot's careful pace stays behind: Docs/Architecture.md section 15, the feel pass); the laps on the line
and on the road; the ghost keeping the faster lap, with its Delta at the end of the second the laps' difference; and the
bike leaning into its corners, its lean at least LEAN_SHARE of the balancing lean over the laps.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import Circuit  # noqa: E402  (the lap's length)
import Ghost  # noqa: E402  (the ghost and its best lap)
import GhostChecks  # noqa: E402  (the ghost's Delta tolerance)
import RacingLine  # noqa: E402  (the line's lap time and lean limit)
import Ride  # noqa: E402  (the Ride graph: the autopilot and the bike's place on the lap)
import RideChecks  # noqa: E402  (the autopilot's bands on the line)
from Checks import Suite, run  # noqa: E402
from Graphs import private  # noqa: E402

GHOST = Ghost.GHOST
RIDE = Ride.RIDE

# The laps ridden, and how near the line a crossing must lie on both sides (meters of the lap: the bike rides at most 1.4 m a
# step).
LAPS = 2
CROSSING = 50.0
# Each lap's time against the racing line's (shares of it): the autopilot rides the line's corners at Ride.AUTO_GRIP of the
# tyres' grip and slows for them at Ride.AUTO_BRAKING, where the line's point mass takes the grip whole (its profile's lap is
# Ride.check()'s); the first lap starts 9 m behind the line, the second is flying. The feel pass read 82.53 and 79.28 s
# (1.16 and 1.12 times the line's 71.1 s); the band leaves room for the rides other compilers and configurations give.
LAP_BAND = (1.0, 1.25)
# The laps on the line: as the autopilot's segments of the ride test scene (RideChecks).
MOST_OFFSET = RideChecks.MOST_OFFSET
MOST_LEAN = RideChecks.MOST_LEAN
# Delta at the second lap's end against the laps' difference (seconds), as the ghost's checks of the ride test scene.
DELTA_TOLERANCE = GhostChecks.DELTA_TOLERANCE
# The lean over the lean that balances the turn: every LEAN_EVERY steps of the laps (ten times a second) the turn's rate is
# the heading's change since the last sample, the balancing lean atan(v x rate / g), and where that passes LEAN_FROM degrees
# the bike's lean over it is a sample; the laps' reading is the samples' mean. The bike as drawn (its contacts as wide as its
# tyres) read 0.07, the feel pass's contacts about two thirds (Docs/Architecture.md section 15); the bound is 85 % of the
# weaker of the Debug and the Release readings (with the yaw damper 0.643 and 0.6415; 0.671 and 0.676 before it), so that a
# bike that rides its corners upright again fails.
LEAN_EVERY = 6
LEAN_FROM = 10.0
LEAN_SHARE = 0.545
# How long a check waits for its reading (seconds of the game, from when it starts); the scene's timeout stays above it.
WAIT = 200

ENTITIES = (
    ("Bike", "Bike", "The ridden bike, which the autopilot rides"),
    ("Ride", Ride.ROOT, "The Ride entity, whose graph keeps the bike's place on the lap and rides the autopilot"),
    ("GhostBike", Ghost.ENTITY, "The ghost, which keeps the best lap"),
)
VARIABLES = (
    private("StepStart", "double", 0.0),
    private("LapStep", "int", 0), private("LapNumber", "int", 0), private("LapStartStep", "int", 0),
    private("LapLast", "float", -1.0), private("LapBest", "float", 0.0), private("LapReading", "float", 0.0),
    private("LapTimes", "float[]", [0.0] * LAPS), private("LapDeltas", "float[]", [0.0] * LAPS),
    private("LapKept", "float[]", [-1.0] * LAPS), private("LapSamples", "int[]", [-1] * LAPS),
    private("LapOffset", "float", 0.0), private("LapRough", "int", 0), private("LapLean", "float", 0.0),
    private("LapHeading", "float", 0.0), private("LapHeaded", "bool", False), private("LapShareSum", "float", 0.0),
    private("LapShareCount", "int", 0),
    private("LapsDone", "bool", False),
)


def lap_timeout():
    return next(scene.timeout for scene in Checks.SCENES if scene.name == "Lap")


def ghost_value(b, name):
    return b.get(name, GHOST, b.var("GhostBike"))


def broadcast(b, event, **arguments):
    name, parameters = event
    return b.add("Event.Broadcast", {"Name": name, "Parameters": [{"Name": p, "Type": t} for p, t in parameters]}, "Count",
                 comment="The race director's event, which this test stands in for", **arguments)


def lean_share(b):
    """The laps' lean over the balancing lean: the mean of the samples' shares."""
    return b.expr("count > 0 ? sum / float(count) : 0.0", sum=b.var("LapShareSum"), count=b.var("LapShareCount"))


def put(b, array, index, value):
    return b.set(array, b.pure("Array.Set", Array=b.var(array), Index=index, Value=value))


def write_finish(b):
    """Finish lap: the lap's time and Delta, LapFinished (Best when it is the faster), and what the ghost then keeps."""
    lap = b.var("LapNumber")
    index = b.expr("lap - 1", lap=lap)
    time = b.set("LapReading", b.expr("float(step - start) * dt", step=b.var("LapStep"), start=b.var("LapStartStep"),
                                      dt=b.pure("Get:Strada.Time.FixedDeltaTime", "Value")))
    best = b.expr("best <= 0 || time < best", best=b.var("LapBest"), time=b.var("LapReading"))
    delta = put(b, "LapDeltas", index, ghost_value(b, "Delta"))
    timed = put(b, "LapTimes", index, b.var("LapReading"))
    finished = broadcast(b, Ghost.LAP_FINISHED, Lap=lap, Time=b.var("LapReading"), Best=best)
    beats = b.branch(b.expr("best <= 0 || time < best", best=b.var("LapBest"), time=b.var("LapReading")))
    record = b.set("LapBest", b.var("LapReading"))
    kept = put(b, "LapKept", index, ghost_value(b, "BestLapTime"))
    count = put(b, "LapSamples", index, b.pure("Array.Length", "Length", Array=ghost_value(b, Ghost.BEST_LAP[0][0])))
    b.chain(b.entry, time, delta, timed, finished, beats)
    b.chain(beats["True"], record, kept)
    b.flow(beats["False"], kept)
    b.chain(kept, count, b.ret)


def write_laps(b):
    """The laps: each fixed step, the crossings of the line, the lap's readings, and the step's count."""
    length = round(Circuit.LENGTH, 3)
    here = b.get("Distance", RIDE, b.var("Ride"))
    crossed = b.branch(b.expr(f"last > {round(Circuit.LENGTH - CROSSING, 3)} && here < {CROSSING}", last=b.var("LapLast"), here=here),
                       comment="Across the start and finish line since the step before")
    riding = b.branch(b.expr(f"lap >= 1 && lap <= {LAPS}", lap=b.var("LapNumber")), comment="A lap in progress ends")
    finish = b.own("FinishLap")
    more = b.branch(b.expr(f"lap < {LAPS}", lap=b.var("LapNumber")), comment="Another lap begins")
    begin = [b.set("LapNumber", b.expr("lap + 1", lap=b.var("LapNumber"))), b.set("LapStartStep", b.var("LapStep"))]
    started = broadcast(b, Ghost.LAP_STARTED, Lap=b.var("LapNumber"))
    stop = b.send("StopAutopilot", b.var("Ride"), graph=RIDE)
    readings = b.fmt(
        "Moto's laps: {Times} s, Delta at their ends {Deltas} s, the ghost keeping {Kept} s of {Samples} samples after each; at "
        "most {Offset} m off the line, {Rough} steps off the road, leaned {Lean} degrees at most and {Share} of the balancing "
        "lean over {Count} samples; done at step {Step}",
        Times=b.var("LapTimes"), Deltas=b.var("LapDeltas"), Kept=b.var("LapKept"), Samples=b.var("LapSamples"),
        Offset=b.var("LapOffset"), Rough=b.var("LapRough"), Lean=b.var("LapLean"), Share=lean_share(b),
        Count=b.var("LapShareCount"), Step=b.var("LapStep"))
    done = [b.set("LapNumber", LAPS + 1), stop, b.call("Call:Strada.Log.Info(string)", Message=readings), b.set("LapsDone", True)]
    # Every step of the laps: how far off the line, on what, how leaned.
    timing = b.branch(b.expr(f"lap >= 1 && lap <= {LAPS}", lap=b.var("LapNumber")), comment="On a lap: its readings")
    wheels = []
    for index in (0, 1):
        state = b.pure("Call:Strada.VehicleComponent.GetWheelState(int)", Target=b.var("Bike"), Index=index)
        surface = b.pure("Get:Strada.VehicleWheelState.Surface", "Value", Target=state)
        wheels.append(b.both(b.pure("Get:Strada.VehicleWheelState.HasContact", "Value", Target=state),
                             b.expr("gravel || grass", gravel=b.equal(surface, b.lit("string", "Gravel")),
                                    grass=b.equal(surface, b.lit("string", "Grass")))))
    away = b.set("LapOffset", b.expr("max(most, abs(offset))", most=b.var("LapOffset"), offset=b.get("Offset", RIDE, b.var("Ride"))))
    rough = b.set("LapRough", b.expr("count + (front || rear ? 1 : 0)", count=b.var("LapRough"), front=wheels[0], rear=wheels[1]))
    lean = b.pure("Get:Strada.VehicleComponent.LeanAngle", "Value", Target=b.var("Bike"))
    leaned = b.set("LapLean", b.expr("max(most, abs(lean))", most=b.var("LapLean"), lean=lean))
    # Ten times a second: the turn's rate since the last sample, the lean that balances it, and the lean's share of it (the
    # lean is positive to the right, where the heading turns the other way).
    sampled = b.branch(b.expr(f"step % {LEAN_EVERY} == 0", step=b.var("LapStep")), comment="Ten times a second: the lean's share")
    heading = b.expr("atan2(f.X, f.Z)", f=b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=b.var("Bike")))
    balance = b.expr(f"degrees(atan(v * atan2(sin(h - last), cos(h - last)) / ({LEAN_EVERY} * dt) / {RacingLine.GRAVITY}))",
                     v=b.pure("Get:Strada.VehicleComponent.ForwardSpeed", "Value", Target=b.var("Bike")), h=heading,
                     last=b.var("LapHeading"), dt=b.pure("Get:Strada.Time.FixedDeltaTime", "Value"))
    turning = b.branch(b.expr(f"headed && abs(balance) > {LEAN_FROM}", headed=b.var("LapHeaded"), balance=balance),
                       comment="A turn whose balancing lean passes LEAN_FROM")
    share = b.set("LapShareSum", b.expr("sum - lean / balance", sum=b.var("LapShareSum"), lean=lean, balance=balance))
    shared = b.set("LapShareCount", b.expr("count + 1", count=b.var("LapShareCount")))
    headed = [b.set("LapHeading", heading), b.set("LapHeaded", True)]
    last = b.set("LapLast", b.get("Distance", RIDE, b.var("Ride")))
    counted = b.set("LapStep", b.expr("step + 1", step=b.var("LapStep")))
    b.chain(b.entry, crossed)
    b.flow(crossed["True"], riding)
    b.chain(riding["True"], finish, more)
    b.flow(riding["False"], more)
    b.chain(more["True"], *begin, started, timing)
    b.flow(more["False"], done[0])
    b.chain(*done, timing)
    b.flow(crossed["False"], timing)
    b.chain(timing["True"], away, rough, leaned, sampled)
    b.chain(sampled["True"], turning)
    b.chain(turning["True"], share, shared, headed[0])
    b.flow(turning["False"], headed[0])
    b.chain(*headed, last)
    b.flow(sampled["False"], last)
    b.flow(timing["False"], last)
    b.chain(last, counted, b.ret)


HELPERS = (
    (("FinishLap", False, (), (), ()), "Finish lap: its time and Delta, LapFinished, what the ghost keeps", write_finish),
)
STEPS = (("Laps", "Laps: each fixed step, the line's crossings and the lap's readings", write_laps),)


def checks():
    if WAIT >= lap_timeout():
        raise ValueError(f"a lap check waits {WAIT} s, not less than the lap test scene's timeout of {lap_timeout()} s")
    return (
        ("CheckLapTimes", "Lap times: within the band of the racing line's lap",
         "the autopilot rides whole laps within the band of the racing line's lap time"),
        ("CheckLapLine", "Line: whole laps on the racing line and the road",
         "the autopilot rides whole laps on the racing line, never off the road"),
        ("CheckLapGhost", "Ghost: the faster lap kept, Delta the laps' difference",
         "the ghost keeps the faster of two whole laps, and its Delta at the second's end is their difference"),
        ("CheckLapLean", "Lean: into the corners, near the lean that balances them",
         "the bike leans into the corners of whole laps by at least its share of the lean that balances each turn"),
    )


def array_value(b, array, index):
    return b.add("Array.Get", None, "Element", Array=b.var(array), Index=index).out


def check_times(b, name):
    line = round(RacingLine.LAP_TIME, 2)
    low, high = round(LAP_BAND[0] * line, 2), round(LAP_BAND[1] * line, 2)
    ready = b.var("LapsDone")
    t1, t2 = array_value(b, "LapTimes", 0), array_value(b, "LapTimes", 1)
    passed = b.expr(f"ready && a >= {low} && a <= {high} && c >= {low} && c <= {high}", ready=ready, a=t1, c=t2)
    text = b.fmt(f"laps of {{A}} and {{C}} s against the racing line's {line} s (the band {low} to {high})", A=t1, C=t2)
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_line(b, name):
    ready = b.var("LapsDone")
    passed = b.expr(f"ready && offset <= {MOST_OFFSET} && rough == 0 && lean <= {MOST_LEAN}", ready=ready,
                    offset=b.var("LapOffset"), rough=b.var("LapRough"), lean=b.var("LapLean"))
    text = b.fmt(f"{{Offset}} m off the line at most (at most {MOST_OFFSET}), {{Rough}} steps with a wheel on gravel or grass, "
                 f"leaned {{Lean}} degrees at most (at most {MOST_LEAN})", Offset=b.var("LapOffset"), Rough=b.var("LapRough"),
                 Lean=b.var("LapLean"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_ghost(b, name):
    ready = b.var("LapsDone")
    t1, t2 = array_value(b, "LapTimes", 0), array_value(b, "LapTimes", 1)
    k2, n2, d2 = array_value(b, "LapKept", 1), array_value(b, "LapSamples", 1), array_value(b, "LapDeltas", 1)
    # The ghost's samples start in its first fixed step at or after LapStarted (Ghost.py), so its count is its time's or one
    # fewer.
    passed = b.expr(f"ready && k2 == min(a, c) && abs(n2 - (int((k2 + 0.000001) / {round(Ghost.SAMPLE_PERIOD, 9)}) + 1)) <= 1 && "
                    f"abs(d2 - (c - a)) <= {DELTA_TOLERANCE}", ready=ready, a=t1, c=t2, k2=k2, n2=n2, d2=d2)
    text = b.fmt(f"after laps of {{A}} and {{C}} s the ghost kept {{K}} s of {{N}} samples; its Delta at the second's end {{D}} s "
                 f"(within {DELTA_TOLERANCE} of their difference)", A=t1, C=t2, K=k2, N=n2, D=d2)
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_lean(b, name):
    ready = b.var("LapsDone")
    share = lean_share(b)
    passed = b.expr(f"ready && share >= {LEAN_SHARE}", ready=ready, share=share)
    text = b.fmt(f"leaned {{Share}} of the lean that balances each turn over {{Count}} samples (at least {LEAN_SHARE})", Share=share,
                 Count=b.var("LapShareCount"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


WRITERS = {"CheckLapTimes": check_times, "CheckLapLine": check_line, "CheckLapGhost": check_ghost, "CheckLapLean": check_lean}


def write_check(b, function, name):
    WRITERS[function](b, name)


def prepare_scene(session, found):
    """The autopilot rides from the scene's start, and the ghost records the ridden bike's laps."""
    Checks.set_variables(session, found[Ride.ROOT], {"Autopilot": {"Type": "Bool", "Value": True}})
    Checks.set_variables(session, found[Ghost.ENTITY], {"Bike": Checks.entity_value(found["Bike"])})


SUITE = Suite(checks, write_check, ENTITIES, VARIABLES, HELPERS, prepare=prepare_scene, steps=STEPS)

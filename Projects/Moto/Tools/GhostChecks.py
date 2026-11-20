"""The best lap's checks, a module of Checks.MODULES that joins the ride test scene (Scenes/Ride.sscene) after the directors'
(DirectorChecks.py): its ride starts where the assists' and the autopilot's ends (RideChecks.py: AssistPhase Done, the bike
on the grid at rest in neutral, the riding graph on its test driver with the assists off), and its checks run after theirs.

It stands in for the race director (slice 4's GF), which it does not run: it broadcasts LapStarted(Lap) and
LapFinished(Lap, Time, Best) as the race director does, in the fixed step a lap starts or ends, Best when the lap is shorter
than every lap before it. Its laps are short stretches, each from the grid at rest with the autopilot riding to
LAP_DISTANCE meters along the lap (through Turn 1), timed in fixed steps, so that whole laps stay behind STRADA_LONG_RIDES
(Checks.SCENES' Lap scene). It is driven from the runner graph's Event.FixedUpdate by its own count of steps and by what the
bike and the Ride graph read, never by a frame (the rule of the rides compared across frame rates), and logs every reading
of its steps in one line ("Moto's ghost: ..."), the same at every frame rate (what the ghost's update sets, such as its
LapTime, is read only by the checks, never in that line). The phases (PHASES):

1. Wait: until AssistPhase is Done; then the assists on.
2. Laps 1, 2 and 3 at the autopilot's PACES: the first is kept, the second (faster) replaces it, the third (slower) does
   not; Delta is read in the step each ends, before its LapFinished, and the second's every sample is read from the bike
   (every Ghost.SAMPLE_STEPS steps from the step it started in) to be compared with what the ghost keeps.
3. Short (lap 4): the ghost's best lap saved by this module and its MaxSamples lowered to SHORT_SAMPLES; SHORT_STEPS steps
   on the autopilot, the shortest lap of all and so the race's best: longer than the ghost keeps, it is timed but not
   kept, and the ghost clears the best lap it had and rides the racing line; then MaxSamples back to Ghost.MAX_SAMPLES.
4. Reload: the four arrays and the time saved written back and BestLapLoaded sent: the ghost rides them.
   Unload: its rotations emptied and BestLapLoaded sent: refused (not as many values in each array), it clears its best lap
   and rides the line.
5. Stand: the autopilot stopped, the riding graph back on its test driver with the assists off, the bike on the grid, braked
   into neutral until its wheels stop; then Done (GhostPhase), for modules that join the scene after this one.

Each lap begins as the assists' ride ends (RideChecks' Stand): the riding graph on its test driver, the bike placed on the
grid at rest (BikeChecks' Place), both brakes on and shifted down into neutral until its wheels stop; then, in one step, the
autopilot starts at the lap's pace, the Ride graph's Distance (which has followed the bike onto the grid by then) is noted
as where the lap starts, and LapStarted is broadcast. The lap's clock is its steps since that step times the fixed step. In every frame of lap 3 (Checks.Suite.frames)
the ghost rides lap 2, which it keeps: it stands where the bike was at its time on the lap (on the segment between the two
samples of the bike this module took round that time, at the share of the segment the time gives) and its LapTime is the
race's lap clock within a frame's time. The ghost's samples start in its first fixed step at or after LapStarted, which is
the step the lap starts in or the next, as the scene orders the graphs within a step, so this module takes the bike's
samples on both grids (from the start step and from the next) and compares the ghost's with the one they lie on.

The checks (latent functions of the update) wait for the readings and report them; bands: Docs/Architecture.md section 15,
the ride in graphs.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import BikeChecks  # noqa: E402  (the grid, the bike's helpers)
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import Circuit  # noqa: E402  (the lap's length)
import Ghost  # noqa: E402  (the ghost and its best lap)
import Ride  # noqa: E402  (the Ride graph: the autopilot and the bike's place on the lap)
import RideChecks  # noqa: E402  (the assists' ride, whose end this one starts from)
import Vehicle  # noqa: E402  (the riding graph)
from Checks import Suite, run  # noqa: E402
from Graphs import private  # noqa: E402

VECTOR = "Strada.Vector3"
GHOST = Ghost.GHOST
RIDE = Ride.RIDE
BIKE = Vehicle.BIKE
STEP = BikeChecks.STEP

# --- The ride ------------------------------------------------------------------------------------------------------------

# Lap 4: the steps it lasts and the most samples the ghost keeps meanwhile (fewer than the lap takes).
SHORT_STEPS = 180
SHORT_SAMPLES = 20
# Laps 1 to 3: how far along the lap from the grid each rides (meters of the center line), and the autopilot's pace in each.
LAP_DISTANCE = 660.0
PACES = (0.8, 1.0, 0.9)
# The phases in the order they ride: (name, the function its case calls).
PHASES = (
    ("Wait", "GhostWait"),
    ("Lap1", "GhostLap1"),
    ("Lap2", "GhostLap2"),
    ("Lap3", "GhostLap3"),
    ("Short", "GhostShort"),
    ("Reload", "GhostReload"),
    ("Unload", "GhostUnload"),
    ("Stand", "GhostStand"),
)

# --- The bands -------------------------------------------------------------------------------------------------------------

# Delta at a lap's end against the difference of the two laps' times (seconds): the bike at the end of the lap stands up to
# a step past where the best lap's samples put the same distance, and its foot is taken on segments between samples 0.1 s
# apart.
DELTA_TOLERANCE = 0.1
# The ghost's place in a frame against where the samples put it at its time (meters), and its lap clock against the race's
# (seconds: the ghost's time runs by frames, the race's by fixed steps, a frame's time apart at most at 1/30 s a frame).
REPLAY_TOLERANCE = 0.01
CLOCK_TOLERANCE = 0.04
# The frames of lap 3 the replay is read in at least (lap 2 lasts about 20 s: 600 frames at 1/30 s a frame).
REPLAY_FRAMES = 300
# The samples the ghost keeps against the bike's own, read at the same steps (meters).
SAMPLE_TOLERANCE = 1e-5
# How long a check waits for its reading (seconds of the game, from when it starts); the scene's timeout stays above it.
WAIT = 150


def ride_timeout():
    return BikeChecks.ride_timeout()


# --- The runner graph's interface -----------------------------------------------------------------------------------------

VARIABLES = (
    private("GhostPhase", "string", "Wait"), private("GhostStep", "int", 0), private("GhostPhaseStep", "int", 0),
    private("LapStartAt", "float", 0.0), private("LapStartStep", "int", -1), private("BestSoFar", "float", 0.0),
    private("LapTimes", "float[]", [0.0, 0.0, 0.0, 0.0]), private("Deltas", "float[]", [0.0, 0.0, 0.0, 0.0]),
    private("KeptTimes", "float[]", [-1.0, -1.0, -1.0, -1.0]), private("KeptSamples", "int[]", [-1, -1, -1, -1]),
    private("ShortRides", "bool", True), private("BikeSamples", "Strada.Vector3[]", []),
    private("BikeSamplesLater", "Strada.Vector3[]", []), private("SampleError", "float", -1.0), private("SampleOffset", "int", -1),
    private("SavedPositions", "Strada.Vector3[]", []), private("SavedRotations", "Strada.Quaternion[]", []),
    private("SavedEngineSpeeds", "float[]", []), private("SavedLoads", "float[]", []), private("SavedTime", "float", 0.0),
    private("UnloadRides", "bool", True), private("UnloadTime", "float", -1.0), private("ReloadRides", "bool", False),
    private("ReloadTime", "float", -1.0),
    private("ReplayFrames", "int", 0), private("ReplayWorst", "float", 0.0), private("ClockWorst", "float", 0.0),
    private("GhostDone", "bool", False),
)


def bike(b):
    return b.var("Bike")


def ghost(b):
    return b.var("GhostBike")


def elapsed(b):
    return b.once("ghost elapsed", lambda: b.expr("step - start", step=b.var("GhostStep"), start=b.var("GhostPhaseStep")))


def enter(b, phase):
    return b.own("GhostEnter", Next=phase)


def broadcast(b, event, comment=None, **arguments):
    """The race director's event, as it broadcasts it: to every graph with an event of that name and these parameters."""
    name, parameters = event
    return b.add("Event.Broadcast", {"Name": name, "Parameters": [{"Name": p, "Type": t} for p, t in parameters]}, "Count",
                 comment=comment or "The race director's event, which this test stands in for", **arguments)


def ghost_value(b, name):
    return b.get(name, GHOST, ghost(b))


def covered(b):
    """The lap distance the bike covered since its lap started (meters of the center line)."""
    length = round(Circuit.LENGTH, 3)
    return b.once("covered", lambda: b.expr(f"(here - start + {length}) % {length}", here=b.get("Distance", RIDE, b.var("Ride")),
                                             start=b.var("LapStartAt")))


def lap_steps(b):
    """The lap's steps since the step it started in (negative before it starts)."""
    return b.once("lap steps", lambda: b.expr("start < 0 ? -1 : step - start", step=b.var("GhostStep"),
                                               start=b.var("LapStartStep")))


def lap_clock(b):
    """The lap's time so far: its steps since LapStarted times the fixed step."""
    return b.once("lap clock", lambda: b.expr("float(steps) * dt", steps=lap_steps(b),
                                               dt=b.pure("Get:Strada.Time.FixedDeltaTime", "Value")))


def put(b, array, index, value):
    return b.set(array, b.pure("Array.Set", Array=b.var(array), Index=index, Value=value))


# --- The helpers and the phases ----------------------------------------------------------------------------------------------

def write_enter(b):
    begins = b.set("GhostPhaseStep", b.expr("step + 1", step=b.var("GhostStep")), comment="Its first step is the next")
    b.chain(b.entry, begins, b.set("GhostPhase", b.entry["Next"]), b.ret)


def write_begin(b):
    """Begin lap: in the lap's first step the riding graph on its test driver and the bike placed on the grid at rest; then
    both brakes on and one gear down every BikeChecks.SHIFT_DOWN_EVERY steps until it stands in neutral, its wheels
    stopped; then the lap started (the autopilot at the pace, where along the lap it starts, LapStarted). Returns whether
    the lap is under way (from the step it started in on)."""
    first = b.branch(b.expr("elapsed == 0", elapsed=elapsed(b)), comment="The lap's first step: the bike onto the grid")
    tested = b.set("Driver", Vehicle.TEST, graph=BIKE, target=bike(b), comment="On the test driver until the lap starts")
    unstarted = b.set("LapStartStep", -1)
    at, yaw, along = BikeChecks.grid_spot()
    place = b.own("Place", At=at, Yaw=yaw, Along=along, Speed=0.0, comment="Onto the grid, at rest")
    settling = b.branch(b.expr("start < 0", start=b.var("LapStartStep")), comment="Not started yet: braked into neutral")
    held = b.own("Drive", Throttle=0.0, FrontBrake=1.0, RearBrake=1.0, Lean=0.0)
    down = b.branch(b.expr(f"elapsed % {BikeChecks.SHIFT_DOWN_EVERY} == 0 && gear > 0", elapsed=elapsed(b), gear=BikeChecks.gear(b)),
                    comment="One gear down at a time, until neutral")
    shift = b.send("ShiftDown", bike(b), graph=BIKE)
    ready = b.branch(b.expr(f"elapsed >= {BikeChecks.STAND_STEPS} && speed < {BikeChecks.STOPPED} && gear == 0",
                            elapsed=elapsed(b), speed=BikeChecks.speed(b), gear=BikeChecks.gear(b)),
                     comment="At rest in neutral on the grid: the lap starts")
    released = b.own("Drive", Throttle=0.0, FrontBrake=0.0, RearBrake=0.0, Lean=0.0)
    pace = b.set("Pace", b.entry["Pace"], graph=RIDE, target=b.var("Ride"))
    autopilot = b.send("StartAutopilot", b.var("Ride"), graph=RIDE)
    noted = b.set("LapStartAt", b.get("Distance", RIDE, b.var("Ride")))
    step = b.set("LapStartStep", b.var("GhostStep"))
    started = broadcast(b, Ghost.LAP_STARTED, Lap=b.entry["Lap"])
    b.link(b.expr("start >= 0", start=b.var("LapStartStep")), b.ret["Riding"])
    b.chain(b.entry, first)
    b.chain(first["True"], tested, unstarted, place, settling)
    b.flow(first["False"], settling)
    b.chain(settling["True"], held, down)
    b.flow(down["True"], shift)
    b.chain(shift, ready)
    b.flow(down["False"], ready)
    b.chain(ready["True"], released, pace, autopilot, noted, step, started, b.ret)
    b.flow(ready["False"], b.ret)
    b.flow(settling["False"], b.ret)


def write_end(b):
    """End lap: Delta read before LapFinished (Best when the lap is shorter than all before it), what the ghost then keeps,
    and the autopilot stopped."""
    lap = b.entry["Lap"]
    index = b.expr("lap - 1", lap=lap)
    time = b.set("LapTime", lap_clock(b))
    best = b.set("Best", b.expr("best <= 0 || time < best", best=b.var("BestSoFar"), time=b.var("LapTime")))
    delta = put(b, "Deltas", index, ghost_value(b, "Delta"))
    timed = put(b, "LapTimes", index, b.var("LapTime"))
    finished = broadcast(b, Ghost.LAP_FINISHED, Lap=lap, Time=b.var("LapTime"), Best=b.var("Best"))
    beats = b.branch(b.var("Best"))
    record = b.set("BestSoFar", b.var("LapTime"))
    kept = put(b, "KeptTimes", index, ghost_value(b, "BestLapTime"))
    count = put(b, "KeptSamples", index, b.pure("Array.Length", "Length", Array=ghost_value(b, Ghost.BEST_LAP[0][0])))
    stop = b.send("StopAutopilot", b.var("Ride"), graph=RIDE)
    b.chain(b.entry, time, best, delta, timed, finished, beats)
    b.chain(beats["True"], record, kept)
    b.flow(beats["False"], kept)
    b.chain(kept, count, stop, b.ret)


def write_wait(b):
    done = b.branch(b.equal(b.var("AssistPhase"), b.lit("string", "Done")), comment="The assists' ride is done: on the grid")
    on = [RideChecks.assist(b, name, True) for name in ("LeanAssist", "LinkedBrakes", "AutoShift")]
    b.chain(b.entry, done)
    b.chain(done["True"], *on, enter(b, "Lap1"), b.ret)
    b.flow(done["False"], b.ret)


def write_short(b):
    """Lap 4: the ghost's best lap saved and its samples bounded in the lap's first step, then SHORT_STEPS steps."""
    first = b.branch(b.expr("elapsed == 0", elapsed=elapsed(b)), comment="Before the lap: the best lap saved, samples bounded")
    saved = [b.set(f"Saved{name[len('BestLap'):]}", ghost_value(b, name)) for name, _, _ in Ghost.BEST_LAP]
    time = b.set("SavedTime", ghost_value(b, "BestLapTime"))
    short = b.set("MaxSamples", SHORT_SAMPLES, graph=GHOST, target=ghost(b))
    begun = b.own("GhostBegin", "Riding", Lap=4, Pace=Ride.PACE)
    over = b.branch(b.expr(f"riding && steps >= {SHORT_STEPS}", riding=begun["Riding"], steps=lap_steps(b)),
                    comment="The lap ends after its steps, longer than the ghost keeps")
    ended = b.own("GhostEnd", Lap=4)
    rides = b.set("ShortRides", ghost_value(b, "RidesBestLap"))
    restored = b.set("MaxSamples", Ghost.MAX_SAMPLES, graph=GHOST, target=ghost(b))
    b.chain(b.entry, first)
    b.chain(first["True"], *saved, time, short, begun)
    b.flow(first["False"], begun)
    b.chain(begun, over)
    b.chain(over["True"], ended, rides, restored, enter(b, "Reload"), b.ret)
    b.flow(over["False"], b.ret)


def write_lap(b, number, pace, after, samples=False):
    """A lap from the grid to LAP_DISTANCE at a pace; with samples, the bike's every Ghost.SAMPLE_STEPS steps from the step
    the lap started in, and from the step after it, to be compared with what the ghost keeps."""
    begun = b.own("GhostBegin", "Riding", Lap=number, Pace=pace)
    reached = b.branch(b.expr(f"riding && steps > 0 && along >= {LAP_DISTANCE} && along < {round(Circuit.LENGTH / 2, 3)}",
                              riding=begun["Riding"], steps=lap_steps(b), along=covered(b)),
                       comment="LAP_DISTANCE along the lap from the grid: the lap ends")
    ended = b.own("GhostEnd", Lap=number)
    b.chain(b.entry, begun)
    if samples:
        fresh = b.branch(b.expr("steps == 0", steps=lap_steps(b)), comment="The lap starts: its samples from none")
        emptied = [b.set(name, b.lit("Strada.Vector3[]", [])) for name in ("BikeSamples", "BikeSamplesLater")]
        at = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=bike(b))
        grids = []
        for name, phase in (("BikeSamples", 0), ("BikeSamplesLater", 1)):
            due = b.branch(b.expr(f"riding && steps >= {phase} && (steps - {phase}) % {Ghost.SAMPLE_STEPS} == 0",
                                  riding=begun["Riding"], steps=lap_steps(b)),
                           comment="A step of this grid of samples")
            noted = b.set(name, b.pure("Array.Add", Array=b.var(name), Value=at))
            grids.append((due, noted))
        b.chain(begun, fresh)
        b.chain(fresh["True"], *emptied, grids[0][0])
        b.flow(fresh["False"], grids[0][0])
        b.chain(grids[0][0]["True"], grids[0][1], grids[1][0])
        b.flow(grids[0][0]["False"], grids[1][0])
        b.chain(grids[1][0]["True"], grids[1][1], reached)
        b.flow(grids[1][0]["False"], reached)
    else:
        b.chain(begun, reached)
    b.chain(reached["True"], ended, *after, b.ret)
    b.flow(reached["False"], b.ret)


def write_error(b):
    """Sample error: the largest distance between the first samples of the bike's (Samples) and the positions the ghost kept,
    as many as the fewer of the two; -1 when their counts differ by more than one (the lap's last step may hold a sample of
    one and not the other)."""
    kept = ghost_value(b, Ghost.BEST_LAP[0][0])
    count = b.expr("min(a, c)", a=b.pure("Array.Length", "Length", Array=kept),
                   c=b.pure("Array.Length", "Length", Array=b.entry["Samples"]))
    close = b.branch(b.expr("abs(a - c) <= 1 && n >= 2", a=b.pure("Array.Length", "Length", Array=kept),
                            c=b.pure("Array.Length", "Length", Array=b.entry["Samples"]), n=count), comment="About as many samples")
    zero = b.set("Worst", 0.0)
    each = b.add("Flow.ForLoop", First=0, Last=b.expr("n - 1", n=count))
    worst = b.set("Worst", b.expr("max(worst, distance(p, q))", worst=b.var("Worst"),
                                  p=b.add("Array.Get", None, "Element", Array=b.entry["Samples"], Index=each["Index"]).out,
                                  q=b.add("Array.Get", None, "Element", Array=kept, Index=each["Index"]).out))
    b.link(b.var("Worst"), b.ret["Error"])
    b.chain(b.entry, close)
    b.chain(close["True"], zero, each)
    b.flow(each["Body"], worst)
    b.flow(each["Completed"], b.ret)
    b.chain(close["False"], b.set("Worst", -1.0), b.ret)


def write_compare(b):
    """Compare samples: the ghost's against the bike's on each grid; the grid they lie on (the smaller error) is kept, with
    its offset in steps from the lap's start, as the samples the replay is read against."""
    on_start = b.own("GhostSampleError", "Error", Samples=b.var("BikeSamples"))
    later = b.own("GhostSampleError", "Error", Samples=b.var("BikeSamplesLater"))
    first = b.set("SampleError", on_start["Error"])
    first_offset = b.set("SampleOffset", 0)
    better = b.branch(b.expr("later >= 0 && (first < 0 || later < first)", later=later["Error"], first=b.var("SampleError")),
                      comment="The ghost's samples lie on the later grid")
    chosen = [b.set("SampleError", later["Error"]), b.set("SampleOffset", 1), b.set("BikeSamples", b.var("BikeSamplesLater"))]
    b.chain(b.entry, on_start, first, first_offset, later, better)
    b.chain(better["True"], *chosen, b.ret)
    b.flow(better["False"], b.ret)


def write_unload(b):
    """Unload: the ghost's rotations emptied and BestLapLoaded sent (refused: the arrays differ in length), what it then rides
    read."""
    broken = b.set(Ghost.BEST_LAP[1][0], b.lit(Ghost.BEST_LAP[1][1], []), graph=GHOST, target=ghost(b))
    loaded = b.send(Ghost.LOADED[0], ghost(b), graph=GHOST)
    rides = b.set("UnloadRides", ghost_value(b, "RidesBestLap"))
    cleared = b.set("UnloadTime", ghost_value(b, "BestLapTime"))
    b.chain(b.entry, broken, loaded, rides, cleared, enter(b, "Stand"), b.ret)


def write_reload(b):
    """Reload: the four arrays and the time written back, BestLapLoaded sent, and what the ghost rides then read."""
    back = [b.set(name, b.var(f"Saved{name[len('BestLap'):]}"), graph=GHOST, target=ghost(b)) for name, _, _ in Ghost.BEST_LAP]
    time = b.set("BestLapTime", b.var("SavedTime"), graph=GHOST, target=ghost(b))
    loaded = b.send(Ghost.LOADED[0], ghost(b), graph=GHOST)
    rides = b.set("ReloadRides", ghost_value(b, "RidesBestLap"))
    kept = b.set("ReloadTime", ghost_value(b, "BestLapTime"))
    b.chain(b.entry, *back, time, loaded, rides, kept, enter(b, "Unload"), b.ret)


def write_stand(b):
    first = b.branch(b.expr("elapsed == 0", elapsed=elapsed(b)), comment="Back on the test driver, the assists off, on the grid")
    back = [b.set("Driver", Vehicle.TEST, graph=BIKE, target=bike(b))]
    back += [RideChecks.assist(b, name, False) for name in ("LeanAssist", "LinkedBrakes", "AutoShift")]
    at, yaw, along = BikeChecks.grid_spot()
    place = b.own("Place", At=at, Yaw=yaw, Along=along, Speed=0.0, comment="Back onto the grid, at rest")
    held = b.own("Drive", Throttle=0.0, FrontBrake=1.0, RearBrake=1.0, Lean=0.0)
    down = b.branch(b.expr(f"elapsed % {BikeChecks.SHIFT_DOWN_EVERY} == 0 && gear > 0", elapsed=elapsed(b), gear=BikeChecks.gear(b)),
                    comment="One gear down at a time, until neutral")
    shift = b.send("ShiftDown", bike(b), graph=BIKE)
    done = b.branch(b.expr(f"elapsed >= {BikeChecks.STAND_STEPS} && speed < {BikeChecks.STOPPED} && gear == 0", elapsed=elapsed(b),
                           speed=BikeChecks.speed(b), gear=BikeChecks.gear(b)), comment="Its wheels have stopped turning too, in neutral")
    readings = b.fmt(
        "Moto's ghost: lap times {Times}; Delta at their ends {Deltas}; best lap kept {Kept} s of {Samples} samples after each; "
        "the bike's samples {Error} m from the kept ones from {Offset} steps after the lap's start; a lap too long to keep left "
        "it riding the best lap {Short}; refused "
        "samples left it riding {Unload} with {UnloadTime} s, reloaded ones {Reload} with {ReloadTime} s; done at step {Step}", Times=b.var("LapTimes"), Deltas=b.var("Deltas"), Kept=b.var("KeptTimes"),
        Samples=b.var("KeptSamples"), Error=b.var("SampleError"), Offset=b.var("SampleOffset"), Short=b.var("ShortRides"), Unload=b.var("UnloadRides"),
        UnloadTime=b.var("UnloadTime"), Reload=b.var("ReloadRides"), ReloadTime=b.var("ReloadTime"), Step=b.var("GhostStep"))
    log = b.call("Call:Strada.Log.Info(string)", Message=readings, comment="Every reading of the steps, the same at every frame rate")
    b.chain(b.entry, first)
    b.chain(first["True"], *back, place, held)
    b.flow(first["False"], held)
    b.chain(held, down)
    b.flow(down["True"], shift)
    b.chain(shift, done)
    b.flow(down["False"], done)
    b.chain(done["True"], b.own("Drive", Throttle=0.0, FrontBrake=0.0, RearBrake=0.0, Lean=0.0), log, b.set("GhostDone", True),
            enter(b, "Done"), b.ret)
    b.flow(done["False"], b.ret)


FLOAT = "float"
HELPERS = (
    (("GhostEnter", False, (("Next", "string"),), (), ()), "Ghost enter: the next phase, which begins at the next step",
     write_enter),
    (("GhostBegin", False, (("Lap", "int"), ("Pace", FLOAT)), (("Riding", "bool"),), ()),
     "Begin lap: onto the grid, then LapStarted and the autopilot", write_begin),
    (("GhostEnd", False, (("Lap", "int"),), (), (("LapTime", FLOAT, 0.0), ("Best", "bool", False))),
     "End lap: Delta read, LapFinished, what the ghost keeps", write_end),
    (("GhostSampleError", False, (("Samples", "Strada.Vector3[]"),), (("Error", FLOAT),), (("Worst", FLOAT, 0.0),)),
     "Sample error: the bike's samples against those the ghost kept", write_error),
    (("GhostCompare", False, (), (), ()), "Compare samples: the grid the ghost's samples lie on", write_compare),
    (("GhostWait", False, (), (), ()), "Wait: the assists' ride done, the assists on", write_wait),
    (("GhostLap1", False, (), (), ()), "Lap 1: the first kept, at the slowest pace",
     lambda b: write_lap(b, 1, PACES[0], [enter(b, "Lap2")])),
    (("GhostLap2", False, (), (), ()), "Lap 2: faster, kept in its place, its samples read",
     lambda b: write_lap(b, 2, PACES[1], [b.own("GhostCompare"), enter(b, "Lap3")], samples=True)),
    (("GhostLap3", False, (), (), ()), "Lap 3: slower, not kept; the ghost rides lap 2 beside it",
     lambda b: write_lap(b, 3, PACES[2], [enter(b, "Short")])),
    (("GhostShort", False, (), (), ()), "Lap 4: the shortest, too long for the ghost's samples", write_short),
    (("GhostReload", False, (), (), ()), "Reload: the samples written back are ridden again", write_reload),
    (("GhostUnload", False, (), (), ()), "Unload: samples of unequal counts refused", write_unload),
    (("GhostStand", False, (), (), ()), "Stand: on the grid, braked into neutral, the readings logged", write_stand),
)


def write_ride(b):
    """The ride: each fixed step after the assists' ride (the runner graph's Event.FixedUpdate calls it), the phase's
    function, then the step's count."""
    steps = b.sequence(2)
    phases = b.add("Flow.Switch", {"Type": "string", "Cases": [name for name, _ in PHASES]}, Selection=b.var("GhostPhase"))
    b.chain(b.entry, steps)
    b.flow(steps["Then0"], phases)
    for name, function in PHASES:
        b.flow(phases[f"Case:{name}"], b.own(function))
    b.chain(steps["Then1"], b.set("GhostStep", b.expr("step + 1", step=b.var("GhostStep"))), b.ret)


def write_replay(b):
    """The replay: in every frame of lap 3 once the ghost rides its best lap, where it stands against where the bike's
    samples of lap 2 put it at its time on the lap, and its time against the race's lap clock."""
    period = round(Ghost.SAMPLE_PERIOD, 9)
    riding = b.branch(b.expr("lap3 && rides && steps >= 4 && n >= 2", lap3=b.equal(b.var("GhostPhase"), b.lit("string", "Lap3")),
                             rides=ghost_value(b, "RidesBestLap"), steps=lap_steps(b),
                             n=b.pure("Array.Length", "Length", Array=b.var("BikeSamples"))),
                      comment="Lap 3 under way a frame or more (the ghost's update has followed LapStarted), on lap 2")
    time = ghost_value(b, "LapTime")
    count = b.pure("Array.Length", "Length", Array=b.var("BikeSamples"))
    index = b.expr(f"min(int(time / {period}), n - 2)", time=time, n=count)
    a = b.add("Array.Get", None, "Element", Array=b.var("BikeSamples"), Index=index).out
    c = b.add("Array.Get", None, "Element", Array=b.var("BikeSamples"), Index=b.expr("index + 1", index=index)).out
    at = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=ghost(b))
    expected = b.expr(f"a + (c - a) * clamp((time - float(index) * {period}) / {period}, 0, 1)", a=a, c=c, time=time, index=index)
    within = b.expr(f"time < float(n - 1) * {period} && clock < float(n - 1) * {period} && offset >= 0", time=time, n=count,
                    clock=lap_clock(b), offset=b.var("SampleOffset"))
    worst = b.set("ReplayWorst", b.expr("max(worst, distance(at, expected))", worst=b.var("ReplayWorst"), at=at, expected=expected))
    clock = b.set("ClockWorst", b.expr(f"max(worst, abs(time - (clock - float(offset) * {round(Ghost.FIXED_STEP, 9)})))",
                                       worst=b.var("ClockWorst"), time=time, clock=lap_clock(b), offset=b.var("SampleOffset")))
    frames = b.set("ReplayFrames", b.expr("frames + 1", frames=b.var("ReplayFrames")))
    inside = b.branch(within, comment="Within the samples this module took")
    b.chain(b.entry, riding)
    b.chain(riding["True"], inside)
    b.chain(inside["True"], worst, clock, frames, b.ret)
    b.flow(inside["False"], b.ret)
    b.flow(riding["False"], b.ret)


STEPS = (("GhostRide", "Ghost: each fixed step, the lap stand-in's phase", write_ride),)
FRAMES = (("GhostReplay", "Ghost replay: each frame of lap 3, the ghost against lap 2", write_replay),)

# --- The checks ------------------------------------------------------------------------------------------------------------


def checks():
    if WAIT >= ride_timeout():
        raise ValueError(f"a ghost check waits {WAIT} s, not less than the ride test scene's timeout of {ride_timeout()} s")
    return (
        ("CheckGhostTooLong", "Too long: a best lap past the ghost's samples is timed, not kept",
         "a best lap longer than the ghost's samples hold is timed but not kept, and the ghost rides the racing line"),
        ("CheckGhostKeeps", "Keeps: the faster of two laps, its samples the bike's",
         "the ghost keeps a best lap at LapFinished, the faster of two, its samples the bike's at every sixth step"),
        ("CheckGhostDelta", "Delta: the lap against the best at the same place",
         "the ghost's Delta is the lap's time against the best lap's at the same place, negative when ahead"),
        ("CheckGhostReplays", "Replay: the ghost rides its best lap beside the race's",
         "the ghost replays its best lap where the bike rode it, on the race's lap clock"),
        ("CheckGhostLoads", "Loads: samples of one lap ridden, others refused",
         "the ghost rides loaded samples of one lap and refuses others, riding the racing line"),
    )


def array_value(b, array, index):
    return b.add("Array.Get", None, "Element", Array=b.var(array), Index=index).out


def check_too_long(b, name):
    ready = b.var("GhostDone")
    time = array_value(b, "LapTimes", 3)
    kept = array_value(b, "KeptTimes", 3)
    count = array_value(b, "KeptSamples", 3)
    passed = b.expr("ready && kept == 0 && count == 0 && !rides", ready=ready, kept=kept, count=count, rides=b.var("ShortRides"))
    text = b.fmt(f"lap 4, the race's best in {{Time}} s while the ghost kept {SHORT_SAMPLES} samples at most, left it keeping "
                 "{Kept} s of {Count} samples, riding its best lap {Rides}", Time=time, Kept=kept, Count=count,
                 Rides=b.var("ShortRides"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_keeps(b, name):
    ready = b.var("GhostDone")
    t1, t2, t3 = (array_value(b, "LapTimes", index) for index in (0, 1, 2))
    k1, k2, k3 = (array_value(b, "KeptTimes", index) for index in (0, 1, 2))
    n2 = array_value(b, "KeptSamples", 1)
    passed = b.expr(f"ready && t2 < t1 && t3 > t2 && k1 == t1 && k2 == t2 && k3 == t2 && abs(n2 - (int((t2 + 0.000001) / "
                    f"{round(Ghost.SAMPLE_PERIOD, 9)}) + 1)) <= 1 && error >= 0 && error <= {SAMPLE_TOLERANCE}", ready=ready, t1=t1,
                    t2=t2, t3=t3, k1=k1, k2=k2, k3=k3, n2=n2, error=b.var("SampleError"))
    text = b.fmt("laps of {T1}, {T2} and {T3} s left the ghost keeping {K1}, {K2} and {K3} s; lap 2's {N2} samples {Error} m from "
                 f"the bike's every {Ghost.SAMPLE_STEPS} steps from {{Offset}} steps after the lap's start (at most "
                 f"{SAMPLE_TOLERANCE})", T1=t1, T2=t2, T3=t3, K1=k1, K2=k2, K3=k3, N2=n2, Error=b.var("SampleError"),
                 Offset=b.var("SampleOffset"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_delta(b, name):
    ready = b.var("GhostDone")
    t1, t2, t3 = (array_value(b, "LapTimes", index) for index in (0, 1, 2))
    d1, d2, d3 = (array_value(b, "Deltas", index) for index in (0, 1, 2))
    passed = b.expr(f"ready && d1 == 0 && d2 < 0 && d3 > 0 && abs(d2 - (t2 - t1)) <= {DELTA_TOLERANCE} && abs(d3 - (t3 - t2)) <= "
                    f"{DELTA_TOLERANCE}", ready=ready, t1=t1, t2=t2, t3=t3, d1=d1, d2=d2, d3=d3)
    text = b.fmt("Delta at the ends of laps 1, 2 and 3: {D1}, {D2} and {D3} s, against the laps' differences (none before the first "
                 f"kept) of {{A}} and {{C}} s (within {DELTA_TOLERANCE})", D1=d1, D2=d2, D3=d3, A=b.expr("t2 - t1", t2=t2, t1=t1),
                 C=b.expr("t3 - t2", t3=t3, t2=t2))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_replays(b, name):
    ready = b.var("GhostDone")
    passed = b.expr(f"ready && frames >= {REPLAY_FRAMES} && worst <= {REPLAY_TOLERANCE} && clock <= {CLOCK_TOLERANCE}", ready=ready,
                    frames=b.var("ReplayFrames"), worst=b.var("ReplayWorst"), clock=b.var("ClockWorst"))
    text = b.fmt(f"in {{Frames}} frames of lap 3 the ghost stood {{Worst}} m at most from where the bike was on lap 2 at its time "
                 f"(at most {REPLAY_TOLERANCE}), its clock {{Clock}} s at most from the race's (at most {CLOCK_TOLERANCE})",
                 Frames=b.var("ReplayFrames"), Worst=b.var("ReplayWorst"), Clock=b.var("ClockWorst"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_loads(b, name):
    ready = b.var("GhostDone")
    passed = b.expr("ready && !unload && unloaded == 0 && reload && reloaded == saved && saved > 0", ready=ready,
                    unload=b.var("UnloadRides"), unloaded=b.var("UnloadTime"), reload=b.var("ReloadRides"),
                    reloaded=b.var("ReloadTime"), saved=b.var("SavedTime"))
    text = b.fmt("refused samples left the ghost riding its best lap {Unload} with {UnloadTime} s; the samples of {Saved} s "
                 "written back left it riding {Reload} with {ReloadTime} s", Unload=b.var("UnloadRides"),
                 UnloadTime=b.var("UnloadTime"), Saved=b.var("SavedTime"), Reload=b.var("ReloadRides"), ReloadTime=b.var("ReloadTime"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


WRITERS = {
    "CheckGhostTooLong": check_too_long,
    "CheckGhostKeeps": check_keeps,
    "CheckGhostDelta": check_delta,
    "CheckGhostReplays": check_replays,
    "CheckGhostLoads": check_loads,
}


def write_check(b, function, name):
    WRITERS[function](b, name)


def prepare_scene(session, found):
    """The ghost records the ridden bike's laps: its Bike set to the scene's, whatever the main scene's ghost has."""
    Checks.set_variables(session, found[Ghost.ENTITY], {"Bike": Checks.entity_value(found["Bike"])})


SUITE = Suite(checks, write_check, variables=VARIABLES, helpers=HELPERS, prepare=prepare_scene, steps=STEPS, frames=FRAMES)

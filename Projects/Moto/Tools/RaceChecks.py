"""The race's checks, a module of Checks.MODULES: they join the flow test scene (Scenes/Flow.sscene) after the ridden bike's.
The scene's race director starts idle (Checks.SWITCHES), and the entities of this module drive what the checks read, from
the scene's first step, beside the ride (the director never times the bike there), with the scene's own ghost:

- the Gate Probe, a box with no gravity and no damping that the gates time in place of the bike;
- the Ghost, the main scene's ghost (Scripts/Ghost.sgraph, Race.GHOST_GRAPH), whose best lap's samples the director
  keeps and loads, as the director's Ghost is in the main scene; its Bike is cleared (prepare), since the scripted races
  are no laps of the bike's: it records none, and what it holds is what the director and this module give it;
- the Race Tests entity, whose graph (Scripts/RaceTests.sgraph) drives the probe and a scripted race from its
  Event.FixedUpdate, as a function of its own count of fixed steps and of what the director reads, never of a frame, and
  keeps what it read for the checks;
- the Race Marker, a child of the race that runs no gate's graph, put first among the race's children, with the line
  moved last: the director numbers its gates by their Gate, so neither changes what it times.

What it drives, one phase after another (each read in the fixed step it belongs to, so the readings are the same at every
frame rate, step for step):

1. Passes (PASSES): the director's Subject set to the probe for good; the probe is sent through each gate at a stated
   speed, the bike's top speed among them (the red line in sixth gear), from LEAD m before its plane along the lap, 1.2 m
   over the road, then parked high over the grid once out of the gate. Each pass is read as the director times it: the
   crossing it interpolated against where the probe's speed puts the plane from a position the director read a few
   steps earlier (the analytic crossing), and how many times the probe entered the gate's trigger and crossed its plane.
2. The race: StartRace sent (the subject is the parked probe, so the ridden bike crosses nothing the race counts); a
   crossing of the line sent in the countdown's first step, which counts for nothing; once the countdown has run, the
   crossings of a script are sent to the director one a step (GateCrossed, as its gates send them), three laps in a few
   steps, with a crossing out of the gates' order and one after the race's end; the director's broadcasts are counted
   as they come, and its count of saves read in the step the race finished (a save waits for the screen's frame). When a
   lap is the best, the ghost's arrays are filled with that lap's samples, ten a second, as the ghost keeps a lap, once the
   crossing's Send has returned: the ghost, which recorded no lap, has cleared its best lap by then, at the broadcast
   (Ghost.py), whichever of the two graphs the broadcast reached first. Two races (SCRIPTS): the first while the
   director's ghost is the probe, which runs no graph of a ghost, so its best lap is saved without samples; the second,
   faster, while it is the scene's ghost again, so its best lap is saved with them.
3. The slot: the save slot opened again REOPEN_STEPS steps after each race's finish (past the frame that saves it at
   every frame rate), its best lap read back.
4. A real crossing: a third race started, the probe sent through the line at the top speed once it runs, which begins
   lap 1 at the crossing's interpolated time; then the race abandoned, back to the title and its attract mode.

The graph logs every reading in one line ("Moto's race: ..."), about 7 s into the scene, which StradaRuntimeTests compares
between frame rates and between two short runs that keep their data (the second loads the best lap the first saved, and
the ghost rides it: what the loader gave the ghost is read before the first race, its BestLapTime and whether it rides
the best lap); the checks, which run after the ride's, find the
readings taken.
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Bike  # noqa: E402  (the grid)
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import Circuit  # noqa: E402  (the layout)
import Ghost  # noqa: E402  (the ghost, whose best lap the director keeps)
import Graphs  # noqa: E402  (writing graphs)
import Race  # noqa: E402  (the gates and the race director)
from Checks import Suite, run  # noqa: E402
from Graphs import EFFECTS, ENTITY, EVENTS, RULES, private, public  # noqa: E402

RACE_TESTS = Graphs.script("RaceTests")
DIRECTOR = Race.DIRECTOR
GHOST = Race.GHOST_GRAPH
VECTOR = "Strada.Vector3"
TELEPORT = "Call:Strada.RigidBodyComponent.Teleport(Strada.Vector3,Strada.Quaternion)"
ANGLE_AXIS = "Call:Strada.Quaternion.AngleAxis(float,Strada.Vector3)"
PROBE = "Gate Probe"
DRIVER = "Race Tests"

# --- The passes -----------------------------------------------------------------------------------------------------------

# The probe: half its size (a box, meters), and how far before a gate's plane and how high over the gate's origin (on the
# road's center line) it sets off.
PROBE_HALF = 0.25
LEAD = 12.0
HEIGHT = 1.2
# Where it waits between passes: high over the grid, far from every gate.
PARK_HEIGHT = 60.0
# The passes: (gate, speed in m/s), the bike's top speed through each gate.
PASSES = ((0, 10.0), (1, 30.0), (2, 60.0), (0, Race.TOP_SPEED), (1, Race.TOP_SPEED), (2, Race.TOP_SPEED))
# How many steps after a launch the director's reading of the probe is taken for the analytic crossing (one after the
# launch's step at least, whichever of the two graphs runs first in a step), how long a pass may take before it gives
# up, and how many steps the probe waits parked before the next (its trigger events of the pass have arrived by then).
PAIR_STEPS = 2
PASS_LIMIT = 300
SETTLE_STEPS = 10
# The pass sent through the line in a third race, for a real crossing that begins its lap 1: the line at top speed.
REAL_PASS = 3
# How close a timed crossing must come to the analytic one (seconds): far inside a step (1/60 s), wide of the float
# precision of positions hundreds of meters from the origin.
CROSSING_TOLERANCE = 0.0005


def park():
    grid, _ = Bike.grid_place()
    return [round(grid[0], 3), PARK_HEIGHT, round(grid[2], 3)]


def pass_rows():
    """Each pass: (gate, speed, its start, its velocity, the plane's point and normal, the steps it flies on once it has
    crossed, so that it is out of the gate before it is parked)."""
    rows = []
    for gate, speed in PASSES:
        origin, forward = Race.gate_frame(gate)
        start = [round(origin[k] - forward[k] * LEAD, 4) for k in range(3)]
        start[1] = HEIGHT
        velocity = [round(forward[k] * speed, 4) for k in range(3)]
        exit_steps = math.ceil((Race.HALF_DEPTH + PROBE_HALF + 0.5) / (speed * Race.STEP)) + 1
        rows.append((gate, round(speed, 4), start, velocity, [round(value, 4) for value in origin],
                     [round(value, 6) for value in forward], exit_steps))
    return rows


# --- The race -------------------------------------------------------------------------------------------------------------

# How many steps after a race's finish its slot is opened again: the director saves it in the update of the frame after
# the finish's, which at 1/30 s a frame (two steps) comes at most two steps after it; one more for a frame's rounding.
REOPEN_STEPS = 5
assert REOPEN_STEPS >= round((1.0 / 30.0) / Race.STEP) + 1

# The crossings sent to the director, one a step once the race has started: (gate, steps after the start signal, fraction
# of the step). The second is out of the gates' order and the last comes after the race's end: both are ignored. Lap 2 is
# the best.
SCRIPT = (
    (0, 30, 0.25),
    (2, 40, 0.5),
    (1, 2100, 0.5),
    (2, 4200, 0.0),
    (0, 5430, 0.75),
    (1, 7400, 0.25),
    (2, 9300, 0.5),
    (0, 10530, 0.0),
    (1, 12600, 0.0),
    (2, 14700, 0.5),
    (0, 15780, 0.5),
    (1, 15800, 0.0),
)
TIME_TOLERANCE = 0.001
# Each script's finish is its last crossing but one (Reopen counts its steps from there).
assert sum(1 for gate, _, _ in SCRIPT if gate == 0) == Race.LAPS + 1 and SCRIPT[-2][0] == 0 and SCRIPT[-1][0] != 0
# The countdown the scene's director runs (seconds): shorter than the game's, so that a run reaches its readings sooner.
COUNTDOWN = 1.0


def _float(value):
    return struct.unpack("f", struct.pack("f", value))[0]


# The races: a first one, slower, ridden while the director's ghost is the probe, which runs no graph of a ghost, so its best
# lap is kept without samples; then SCRIPT, whose best lap beats it and is kept with the ghost's samples.
SLOW = tuple((gate, int(steps * 1.1), fraction) for gate, steps, fraction in SCRIPT)
SCRIPTS = (SLOW, SCRIPT)
# The races the director starts: the two scripted ones and the one a real crossing begins.
RACES = len(SCRIPTS) + 1


def expected_races():
    """What the director makes of SCRIPTS, one race after the other, its best lap carried from the first to the second, as
    it computes them in floats: every lap's time and every split in order, the best flags as bits (the first lap highest),
    and for each race its total, the best lap after it and that lap's splits."""
    dt = _float(Race.STEP)
    laps, sectors, mask, best, best_splits, races = [], [], 0, 0.0, [], []
    for script in SCRIPTS:
        expected, lap_start, first, splits, count, total = 0, None, 0.0, [], 0, None
        for gate, steps, fraction in script:
            time = _float((steps + fraction) * dt)
            if total is not None or gate != expected:
                continue
            if gate == 0:
                if lap_start is not None:
                    lap = _float(time - lap_start)
                    laps.append(lap)
                    count += 1
                    beat = best <= 0 or lap < best
                    mask = mask * 2 + (1 if beat else 0)
                    if beat:
                        best, best_splits = lap, list(splits)
                    if count == Race.LAPS:
                        total = _float(time - first)
                        continue
                else:
                    first = time
                lap_start, splits, expected = time, [], 1
            else:
                split = _float(time - lap_start)
                splits.append(split)
                sectors.append(split)
                expected = (gate + 1) % len(Race.GATES)
        races.append({"total": total, "best": best, "best_splits": list(best_splits)})
    return {"laps": laps, "sectors": sectors, "mask": mask, "races": races}


def samples(time):
    """How many samples the ghost keeps of a lap of that time."""
    return int(round(time * Race.SAMPLE_RATE))


# --- The driver's graph ------------------------------------------------------------------------------------------------------


READOUTS = (
    ("Phase", "string", "Launch", "Where the drive is: Launch, Fly, Settle, Start, Countdown, Inject, Reopen or Done"),
    ("Passes", "int", 0, "How many passes of the gates the probe made"),
    ("Once", "int", 0, "How many of them entered their gate's trigger once and crossed its plane once"),
    ("Worst", "float", 0.0, "The worst distance of a timed crossing from the analytic one (seconds; 1 for a pass not timed)"),
    ("Round", "int", 0, "Which of the races is run: 0 the first, 1 the second"),
    ("CountdownSteps", "int", -1, "How many steps after StartRace the race started"),
    ("EarlyLap", "int", -1, "The director's lap after a crossing of the line sent in a countdown (0: it counted for nothing)"),
    ("RaceStarts", "int", 0, "How many RaceStarted broadcasts came"),
    ("LapStarts", "int", 0, "How many LapStarted broadcasts came"),
    ("Sectors", "float[]", [], "The splits of the SectorCrossed broadcasts (seconds)"),
    ("SectorError", "float", 0.0, "The worst distance of a split from the script's (seconds; 1 for one too many)"),
    ("Laps", "float[]", [], "The times of the LapFinished broadcasts (seconds)"),
    ("LapError", "float", 0.0, "The worst distance of a lap's time from the script's (seconds; 1 for one too many)"),
    ("BestMask", "int", 0, "The LapFinished broadcasts' Best, as bits, the first lap highest"),
    ("Finishes", "int", 0, "How many RaceFinished broadcasts came"),
    ("Total", "float", 0.0, "The RaceFinished broadcast's total (seconds)"),
    ("Kept", "int", 0, "How many samples the ghost keeps of the best lap"),
    ("SavedTime", "float", -1.0, "The best lap's time the save slot holds, opened again after the results (seconds)"),
    ("SavedSplits", "float[]", [], "The best lap's splits the save slot holds (seconds)"),
    ("SavedError", "float", 1.0, "The worst distance of the slot's best lap and splits from the script's best (seconds)"),
    ("SavedSamples", "int", -1, "How many positions of the ghost's samples the save slot holds"),
    ("FirstSaved", "float", -1.0, "The best lap's time the save slot held after the first race (seconds)"),
    ("FirstSamples", "int", -1, "How many positions of the ghost's samples the save slot held after the first race"),
    ("ResultsState", "string", "", "The director's state at the second race's results"),
    ("RealLap", "int", -1, "The lap a real crossing of the line began, while racing"),
    ("RealStep", "int", -1, "The step of that crossing after the start signal, its fraction aside"),
    ("RealFraction", "float", -1.0, "How far into the step after RealStep the crossing came"),
    ("RealError", "float", 1.0, "How far the lap's start lies from that crossing (seconds)"),
    ("AfterState", "string", "", "The director's state once the race was abandoned"),
    ("Attracts", "int", 0, "How many AttractStarted broadcasts came"),
    ("Saves", "int", -1, "How many times the director had saved its best lap when the slot was last opened again"),
    ("SavedInStep", "bool", False, "Whether a race's best lap was saved in the fixed step of its finish"),
    ("Rides", "bool", False, "Whether the ghost rides the best lap the director loaded as the scene started (its RidesBestLap), "
     "read before the first race"),
    ("GhostBest", "float", -1.0, "The ghost's BestLapTime before the first race: what the director loaded gave it (seconds)"),
    ("Done", "bool", False, "Whether every reading has been taken"),
)
ENTITY_VARIABLES = (
    ("Race", "The race director's entity, whose children are the gates"),
    ("Probe", "The box the gates time in place of the bike"),
    ("Ghost", "The scene's ghost, which keeps the best lap's samples"),
)
PHASES = ("Launch", "Fly", "Settle", "Start", "Countdown", "Inject", "Reopen", "RealStart", "RealWait", "RealFly")


def declare_graphs(author):
    rows = pass_rows()
    race = expected_races()
    best = race["races"][-1]
    author.declare(RACE_TESTS, "Drives the race's checks in the flow test scene from its fixed steps: the gate probe's "
                   "passes, a scripted race and the save slot read back; keeps every reading for the checks and logs them.",
                   variables=(
        tuple(public(name, ENTITY, None, tooltip) for name, tooltip in ENTITY_VARIABLES) +
        tuple(public(name, type_id, value, f"{tooltip} (a read-out)") for name, type_id, value, tooltip in READOUTS) + (
            private("Step", "int", 0), private("PhaseStep", "int", 0), private("Pass", "int", 0),
            private("EntriesBefore", "int", 0), private("CrossingsBefore", "int", 0), private("PairStep", "int", 0),
            private("PairAt", VECTOR), private("Crossed", "bool", False), private("ExitStep", "int", 0),
            private("Error", "float", 0.0), private("Script", "int", 0), private("CountFrom", "int", 0),
            private("Reopened", Race.SAVE_SLOT), private("SavesAtStart", "int", 0), private("FinishSeen", "bool", False),
            private("KeepPending", "bool", False), private("KeepTime", "float", 0.0)) +
        tuple(private(name, type_id) for name, type_id in Race.GHOST_SAMPLES) + (
            private("PassGates", "int[]", [row[0] for row in rows]),
            private("PassSpeeds", "float[]", [row[1] for row in rows]),
            private("PassStarts", "Strada.Vector3[]", [row[2] for row in rows]),
            private("PassVelocities", "Strada.Vector3[]", [row[3] for row in rows]),
            private("PassOrigins", "Strada.Vector3[]", [row[4] for row in rows]),
            private("PassForwards", "Strada.Vector3[]", [row[5] for row in rows]),
            private("PassExits", "int[]", [row[6] for row in rows]),
            private("ScriptGates", "int[]", [gate for script in SCRIPTS for gate, _, _ in script]),
            private("ScriptSteps", "int[]", [steps for script in SCRIPTS for _, steps, _ in script]),
            private("ScriptFractions", "float[]", [fraction for script in SCRIPTS for _, _, fraction in script]),
            private("ExpectedLaps", "float[]", [round(value, 5) for value in race["laps"]]),
            private("ExpectedSectors", "float[]", [round(value, 5) for value in race["sectors"]]),
            private("ExpectedBest", "float[]", [round(value, 5) for value in [best["best"]] + best["best_splits"]]),
        )),
        events=tuple(Race.BROADCASTS.items()),
        functions=(
            ("Enter", False, (("Next", "string"),), (), ()),
            ("Place", False, (("At", VECTOR), ("Velocity", VECTOR)), (), ()),
            ("Launch", False, (), (), ()),
            ("Fly", False, (), (), ()),
            ("Settle", False, (), (), ()),
            ("Start", False, (), (), ()),
            ("Countdown", False, (), (), ()),
            ("Inject", False, (), (), ()),
            ("Keep", False, (("Time", "float"),), (), (("Count", "int", 0),)),
            ("Reopen", False, (), (), ()),
            ("Report", False, (), (), ()),
            ("RealStart", False, (), (), ()),
            ("RealWait", False, (), (), ()),
            ("RealFly", False, (), (), ()),
        ))


def director_get(b, name):
    return b.get(name, DIRECTOR, b.var("Race"))


def element(b, variable, index):
    return b.add("Array.Get", None, "Element", Array=b.var(variable), Index=index).out


def elapsed(b):
    return b.once("elapsed", lambda: b.expr("step - start", step=b.var("Step"), start=b.var("PhaseStep")))


def fixed_dt(b):
    return b.once("dt", lambda: b.pure("Get:Strada.Time.FixedDeltaTime", "Value"))


def worst_against(b, received, expected, error, value):
    """The worst distance so far of the values received against the expected ones, in order: the next value is the element
    of the expected ones at the received ones' length (1 for one more than expected)."""
    count = b.pure("Array.Length", "Length", Array=b.var(received))
    known = b.branch(b.expr("count < expected", count=count, expected=b.pure("Array.Length", "Length", Array=b.var(expected))))
    near = b.set(error, b.expr("max(worst, abs(value - expected))", worst=b.var(error), value=value,
                               expected=element(b, expected, count)))
    far = b.set(error, 1.0)
    return known, near, far


def write_functions(author):
    with author.region(RACE_TESTS, "Enter: the next phase, which begins at the next step", RULES, "Enter") as b:
        b.chain(b.entry, b.set("PhaseStep", b.expr("step + 1", step=b.var("Step"))), b.set("Phase", b.entry["Next"]), b.ret)
    with author.region(RACE_TESTS, "Place: the probe teleported, moving at a velocity", RULES, "Place") as b:
        probe = b.var("Probe")
        moved = b.call(TELEPORT, Target=probe, Position=b.entry["At"], Rotation=b.pure(ANGLE_AXIS, Degrees=0.0, Axis=[0.0, 1.0, 0.0]))
        along = b.call("Set:Strada.RigidBodyComponent.LinearVelocity", Target=probe, Value=b.entry["Velocity"])
        still = b.call("Set:Strada.RigidBodyComponent.AngularVelocity", Target=probe, Value=[0.0, 0.0, 0.0])
        b.chain(b.entry, moved, along, still, b.ret)
    with author.region(RACE_TESTS, "Launch: the probe sent through its pass's gate", EVENTS, "Launch") as b:
        index = b.var("Pass")
        subject = b.set("Subject", b.var("Probe"), graph=DIRECTOR, target=b.var("Race"), comment="The gates time the probe")
        entries = b.set("EntriesBefore", director_get(b, "Entries"))
        crossings = b.set("CrossingsBefore", director_get(b, "Crossings"))
        placed = b.own("Place", At=element(b, "PassStarts", index), Velocity=element(b, "PassVelocities", index))
        b.chain(b.entry, subject, entries, crossings, b.set("Crossed", False), placed, b.own("Enter", Next="Fly"), b.ret)
    with author.region(RACE_TESTS, "Fly: the director's reading, then its crossing against the analytic one", EVENTS, "Fly") as b:
        index = b.var("Pass")
        pair = b.branch(b.expr(f"elapsed == {PAIR_STEPS}", elapsed=elapsed(b)), comment="A reading after the launch")
        pair_step = b.set("PairStep", director_get(b, "Steps"))
        pair_at = b.set("PairAt", director_get(b, "SubjectAt"))
        seen = b.branch(b.expr("!crossed && crossings > before", crossed=b.var("Crossed"), crossings=director_get(b, "Crossings"),
                               before=b.var("CrossingsBefore")), comment="The director timed a crossing")
        analytic = b.expr("float(pair) + dot(origin - at, forward) / (speed * dt)", pair=b.var("PairStep"),
                          origin=element(b, "PassOrigins", index), at=b.var("PairAt"), forward=element(b, "PassForwards", index),
                          speed=element(b, "PassSpeeds", index), dt=fixed_dt(b))
        error = b.set("Error", b.expr("abs((float(step) + fraction - analytic) * dt)", step=director_get(b, "LastStep"),
                                      fraction=director_get(b, "LastFraction"), analytic=analytic, dt=fixed_dt(b)))
        worst = b.set("Worst", b.expr("max(worst, error)", worst=b.var("Worst"), error=b.var("Error")))
        noted = b.set("Crossed", True)
        exit_at = b.set("ExitStep", b.expr("step + exits", step=b.var("Step"), exits=element(b, "PassExits", index)))
        out = b.branch(b.expr(f"(crossed && step >= exit) || elapsed >= {PASS_LIMIT}", crossed=b.var("Crossed"), step=b.var("Step"),
                              exit=b.var("ExitStep"), elapsed=elapsed(b)), comment="Out of the gate, or given up")
        given_up = b.set("Worst", b.expr("crossed ? worst : 1.0", crossed=b.var("Crossed"), worst=b.var("Worst")))
        parked = b.own("Place", At=park(), Velocity=[0.0, 0.0, 0.0])
        b.chain(b.entry, pair)
        b.chain(pair["True"], pair_step, pair_at, seen)
        b.flow(pair["False"], seen)
        b.chain(seen["True"], error, worst, noted, exit_at, out)
        b.flow(seen["False"], out)
        b.chain(out["True"], given_up, parked, b.own("Enter", Next="Settle"), b.ret)
        b.flow(out["False"], b.ret)
    with author.region(RACE_TESTS, "Settle: the pass counted once its trigger events are in, then the next", EVENTS, "Settle") as b:
        index = b.var("Pass")
        settled = b.branch(b.expr(f"elapsed >= {SETTLE_STEPS}", elapsed=elapsed(b)))
        once = b.expr("entries - before == 1 && crossings - crossed == 1 && gate == expected", entries=director_get(b, "Entries"),
                      before=b.var("EntriesBefore"), crossings=director_get(b, "Crossings"), crossed=b.var("CrossingsBefore"),
                      gate=director_get(b, "LastGate"), expected=element(b, "PassGates", index))
        counted = b.set("Once", b.expr("count + (once ? 1 : 0)", count=b.var("Once"), once=once))
        passes = b.set("Passes", b.expr("passes + 1", passes=b.var("Passes")))
        following = b.set("Pass", b.expr("index + 1", index=index))
        more = b.branch(b.expr("passes < count", passes=b.var("Passes"),
                               count=b.pure("Array.Length", "Length", Array=b.var("PassGates"))))
        b.chain(b.entry, settled)
        b.chain(settled["True"], counted, passes, following, more)
        b.chain(more["True"], b.own("Enter", Next="Launch"), b.ret)
        b.chain(more["False"], b.own("Enter", Next="Start"), b.ret)
        b.flow(settled["False"], b.ret)
    with author.region(RACE_TESTS, "Start: the race started, its ghost the probe or the scene's ghost", EVENTS, "Start") as b:
        first = b.branch(b.expr("round == 0", round=b.var("Round")))
        loaded = [b.set("GhostBest", b.get("BestLapTime", GHOST, b.var("Ghost")),
                        comment="What the loader gave the ghost as the scene started, before a race changes it"),
                  b.set("Rides", b.get("RidesBestLap", GHOST, b.var("Ghost")),
                        comment="Whether the ghost took the samples and rides them")]
        ghost = b.set("Ghost", b.var("Probe"), graph=DIRECTOR, target=b.var("Race"),
                      comment="The first race: a ghost that keeps no samples")
        again = b.set("Ghost", b.var("Ghost"), graph=DIRECTOR, target=b.var("Race"), comment="The second: the scene's ghost")
        counted = b.set("CountFrom", director_get(b, "Steps"))
        saves = [b.set("SavesAtStart", director_get(b, "Saves")), b.set("FinishSeen", False)]
        started = b.send("StartRace", b.var("Race"), graph=DIRECTOR, comment="The parked probe its subject: the bike crosses nothing")
        b.chain(b.entry, first)
        b.chain(first["True"], *loaded, ghost, counted)
        b.chain(first["False"], again, counted)
        b.chain(counted, *saves, started, b.own("Enter", Next="Countdown"), b.ret)
    with author.region(RACE_TESTS, "Countdown: a line crossing ignored, the steps until the race started", EVENTS,
                       "Countdown") as b:
        early = b.branch(b.expr("elapsed == 0", elapsed=elapsed(b)), comment="The countdown's first step")
        jumped = b.send("GateCrossed", b.var("Race"), graph=DIRECTOR, Gate=0, Step=director_get(b, "Steps"), Fraction=0.5,
                        comment="As the line sends a jump start")
        noted = b.set("EarlyLap", director_get(b, "Lap"))
        racing = b.branch(b.equal(director_get(b, "State"), b.lit("string", "Racing")))
        took = b.set("CountdownSteps", b.expr("started - counted", started=director_get(b, "StartStep"), counted=b.var("CountFrom")))
        b.chain(b.entry, early)
        b.chain(early["True"], jumped, noted, racing)
        b.flow(early["False"], racing)
        b.chain(racing["True"], took, b.own("Enter", Next="Inject"), b.ret)
        b.flow(racing["False"], b.ret)
    with author.region(RACE_TESTS, "Inject: one scripted crossing a step, as the gates send them", EVENTS, "Inject") as b:
        index = b.var("Script")
        more = b.branch(b.expr(f"index < (round + 1) * {len(SCRIPT)}", index=index, round=b.var("Round")),
                        comment="This race's crossings")
        sent = b.send("GateCrossed", b.var("Race"), graph=DIRECTOR, Gate=element(b, "ScriptGates", index),
                      Step=b.expr("start + steps", start=director_get(b, "StartStep"), steps=element(b, "ScriptSteps", index)),
                      Fraction=element(b, "ScriptFractions", index))
        pending = b.branch(b.var("KeepPending"), comment="A best lap finished in the Send: the broadcast has reached every graph")
        kept = [b.set("KeepPending", False), b.own("Keep", Time=b.var("KeepTime"))]
        following = b.set("Script", b.expr("index + 1", index=index))
        finished = b.branch(b.both(b.pure("Operator.Not", A=b.var("FinishSeen")), b.equal(director_get(b, "State"), b.lit("string", "Results"))),
                            comment="The step the race finished in")
        seen = b.set("FinishSeen", True)
        in_step = b.set("SavedInStep", b.expr("before || saves != start", before=b.var("SavedInStep"), saves=director_get(b, "Saves"),
                                              start=b.var("SavesAtStart")))
        b.chain(b.entry, more)
        b.chain(more["True"], sent, pending)
        b.chain(pending["True"], *kept, following)
        b.flow(pending["False"], following)
        b.chain(following, finished)
        b.chain(finished["True"], seen, in_step, b.ret)
        b.flow(finished["False"], b.ret)
        b.chain(more["False"], b.own("Enter", Next="Reopen"), b.ret)
    with author.region(RACE_TESTS, "Keep: a best lap's samples, ten a second, as the ghost keeps a lap", EFFECTS, "Keep") as b:
        count = b.set("Count", b.expr(f"int(round(time * {Race.SAMPLE_RATE}))", time=b.entry["Time"]))
        cleared = [b.set(name, b.lit(type_id, [])) for name, type_id in Race.GHOST_SAMPLES]
        loop = b.add("Flow.ForLoop", First=0, Last=b.expr("count - 1", count=b.var("Count")))
        at = loop["Index"]
        (positions, _), (rotations, _), (speeds, _), (loads, _) = Race.GHOST_SAMPLES
        adds = [
            b.set(positions, b.pure("Array.Add", Array=b.var(positions), Value=b.expr("vec3(float(i), 0, 0)", i=at))),
            b.set(rotations, b.pure("Array.Add", Array=b.var(rotations),
                                    Value=b.pure(ANGLE_AXIS, Degrees=b.expr("float(i)", i=at), Axis=[0.0, 1.0, 0.0]))),
            b.set(speeds, b.pure("Array.Add", Array=b.var(speeds), Value=b.expr("float(i) * 10", i=at))),
            b.set(loads, b.pure("Array.Add", Array=b.var(loads), Value=0.5)),
        ]
        given = [b.set(name, b.var(name), graph=GHOST, target=b.var("Ghost")) for name, _ in Race.GHOST_SAMPLES]
        b.chain(b.entry, count, *cleared, loop)
        b.chain(loop["Body"], *adds)
        b.chain(loop["Completed"], *given, b.set("Kept", b.var("Count")), b.ret)
    with author.region(RACE_TESTS, "Reopen: past the frame that saves it, the slot's best lap read back", EVENTS, "Reopen") as b:
        # The phase begins three steps after the finish: the script's last crossing but one, the last sent a step after it.
        waited = b.branch(b.expr(f"elapsed >= {REOPEN_STEPS - 3}", elapsed=elapsed(b)),
                          comment=f"{REOPEN_STEPS} steps after the finish: the frame after it has saved, at every frame rate")
        opened = b.call("Call:Strada.SaveSlot.Open(string)", "Result", Name=Race.SLOT)
        slot = b.var("Reopened")

        def read(name, type_id, fallback):
            return b.pure("Call:Strada.SaveSlot.Get<T>(string,T)", Target=slot, Name=name, Fallback=b.lit(type_id, fallback))

        time = b.set("SavedTime", read("LapTime", "float", -1.0))
        splits = b.set("SavedSplits", read("Splits", "float[]", []))
        first = Race.GHOST_SAMPLES[0]
        positions = b.set("SavedSamples", b.pure("Array.Length", "Length", Array=read(first[0], first[1], [])))
        two = b.branch(b.expr("count == 2", count=b.pure("Array.Length", "Length", Array=b.var("SavedSplits"))))
        saved_error = b.set("SavedError", b.expr("max(abs(time - best), max(abs(first - one), abs(second - two)))",
                                                 time=b.var("SavedTime"), best=element(b, "ExpectedBest", 0),
                                                 first=element(b, "SavedSplits", 0), one=element(b, "ExpectedBest", 1),
                                                 second=element(b, "SavedSplits", 1), two=element(b, "ExpectedBest", 2)))
        first_race = b.branch(b.expr("round == 0", round=b.var("Round")), comment="After the first race: the second")
        kept_first = [b.set("FirstSaved", b.var("SavedTime")), b.set("FirstSamples", b.var("SavedSamples")),
                      b.set("Round", 1)]
        results = b.set("ResultsState", director_get(b, "State"), comment="The director at the second race's results")
        real = b.own("Enter", Next="RealStart")
        b.chain(b.entry, waited)
        b.flow(waited["False"], b.ret)
        b.chain(waited["True"], opened, b.set("Reopened", opened.out), time, splits, positions, b.set("Saves", director_get(b, "Saves")),
                first_race)
        b.chain(first_race["True"], *kept_first, b.own("Enter", Next="Start"), b.ret)
        b.flow(first_race["False"], two)
        b.chain(two["True"], saved_error, results)
        b.flow(two["False"], results)
        b.chain(results, real, b.ret)
    with author.region(RACE_TESTS, "Real start: a third race, for a real crossing of the line", EVENTS, "RealStart") as b:
        counted = b.set("CountFrom", director_get(b, "Steps"))
        started = b.send("StartRace", b.var("Race"), graph=DIRECTOR)
        b.chain(b.entry, counted, started, b.own("Enter", Next="RealWait"), b.ret)
    with author.region(RACE_TESTS, "Real wait: once racing, the probe sent through the line", EVENTS, "RealWait") as b:
        racing = b.branch(b.equal(director_get(b, "State"), b.lit("string", "Racing")))
        crossings = b.set("CrossingsBefore", director_get(b, "Crossings"))
        placed = b.own("Place", At=element(b, "PassStarts", REAL_PASS), Velocity=element(b, "PassVelocities", REAL_PASS))
        b.chain(b.entry, racing)
        b.chain(racing["True"], crossings, b.set("Crossed", False), placed, b.own("Enter", Next="RealFly"), b.ret)
        b.flow(racing["False"], b.ret)
    with author.region(RACE_TESTS, "Real fly: lap 1 begun at the crossing, then the race abandoned", EVENTS, "RealFly") as b:
        seen = b.branch(b.expr("!crossed && crossings > before", crossed=b.var("Crossed"), crossings=director_get(b, "Crossings"),
                               before=b.var("CrossingsBefore")), comment="The director timed the crossing")
        lap = b.set("RealLap", director_get(b, "Lap"))
        step = b.set("RealStep", b.expr("last - start", last=director_get(b, "LastStep"), start=director_get(b, "StartStep")))
        fraction = b.set("RealFraction", director_get(b, "LastFraction"))
        error = b.set("RealError", b.expr("abs((float(steps - start) * dt - lap) - (float(crossing) + fraction) * dt)",
                                          steps=director_get(b, "Steps"), start=director_get(b, "StartStep"),
                                          lap=director_get(b, "LapTime"), crossing=b.var("RealStep"),
                                          fraction=b.var("RealFraction"), dt=fixed_dt(b)),
                      comment="The lap's start, from its time so far, against the crossing")
        exit_at = b.set("ExitStep", b.expr("step + exits", step=b.var("Step"), exits=element(b, "PassExits", REAL_PASS)))
        out = b.branch(b.expr(f"(crossed && step >= exit) || elapsed >= {PASS_LIMIT}", crossed=b.var("Crossed"), step=b.var("Step"),
                              exit=b.var("ExitStep"), elapsed=elapsed(b)), comment="Out of the gate, or given up")
        parked = b.own("Place", At=park(), Velocity=[0.0, 0.0, 0.0])
        abandoned = b.send("AbandonRace", b.var("Race"), graph=DIRECTOR)
        after = b.set("AfterState", director_get(b, "State"))
        b.chain(b.entry, seen)
        b.chain(seen["True"], lap, step, fraction, error, b.set("Crossed", True), exit_at, out)
        b.flow(seen["False"], out)
        b.chain(out["True"], parked, abandoned, after, b.own("Report"), b.ret)
        b.flow(out["False"], b.ret)
    with author.region(RACE_TESTS, "Report: every reading logged, the same at every frame rate", EVENTS, "Report") as b:
        shown = b.expr("round(worst * 10000) / 10000", worst=b.var("Worst"))
        real = b.expr("round(error * 10000) / 10000", error=b.var("RealError"))
        readings = b.fmt("Moto's race: {Passes} passes of the gates, {Once} entered and timed once, the worst {Worst} s from the "
                         "analytic crossing; the race started {Countdown} steps after StartRace ({Starts} signals); laps {Laps} "
                         "(best {Mask}), splits {Sectors}, total {Total} ({Finishes} finishes, the director then {Results}); "
                         "after the first race, its ghost keeping no samples, the slot kept {First} s and {FirstSamples} samples; "
                         "the ghost kept {Kept} samples; the slot keeps {Saved} s, splits {SavedSplits}, {Samples} samples; "
                         "a real crossing began lap {RealLap} at step {RealStep} and {RealFraction} of the race, {RealError} s "
                         "from its lap's start; abandoned, the director {After} with {Attracts} attract signals; "
                         "loaded at start: {Loaded} s, the ghost riding it {Rides} with a best lap of {GhostBest} s; the best "
                         "lap saved {Saves} times, in a finish's step {InStep}",
                         Passes=b.var("Passes"), Once=b.var("Once"), Worst=shown, Countdown=b.var("CountdownSteps"),
                         Starts=b.var("RaceStarts"), Laps=b.var("Laps"), Mask=b.var("BestMask"), Sectors=b.var("Sectors"),
                         Total=b.var("Total"), Finishes=b.var("Finishes"), Results=b.var("ResultsState"), First=b.var("FirstSaved"),
                         FirstSamples=b.var("FirstSamples"), Kept=b.var("Kept"), Saved=b.var("SavedTime"),
                         SavedSplits=b.var("SavedSplits"), Samples=b.var("SavedSamples"), RealLap=b.var("RealLap"),
                         RealStep=b.var("RealStep"), RealFraction=b.var("RealFraction"), RealError=real, After=b.var("AfterState"),
                         Attracts=b.var("Attracts"), Loaded=director_get(b, "LoadedLapTime"),
                         Rides=b.var("Rides"), GhostBest=b.var("GhostBest"), Saves=b.var("Saves"),
                         InStep=b.var("SavedInStep"))
        log = b.call("Call:Strada.Log.Info(string)", Message=readings, comment="Every reading, the same at every frame rate")
        b.chain(b.entry, log, b.set("Done", True), b.own("Enter", Next="Done"), b.ret)


def write_graphs(author):
    write_functions(author)
    with author.region(RACE_TESTS, "Drive: each fixed step, the phase's work, then the step counted", EVENTS) as b:
        fixed = b.event("Event.FixedUpdate")
        steps = b.sequence(2)
        phases = b.add("Flow.Switch", {"Type": "string", "Cases": list(PHASES)}, Selection=b.var("Phase"))
        b.chain(fixed, steps)
        b.flow(steps["Then0"], phases)
        for phase in PHASES:
            b.flow(phases[f"Case:{phase}"], b.own(phase))
        b.flow(steps["Then1"], b.set("Step", b.expr("step + 1", step=b.var("Step"))))
    with author.region(RACE_TESTS, "Signals: the race's start, its laps begun and its end, counted", EVENTS) as b:
        started = b.custom("RaceStarted")
        b.chain(started, b.set("RaceStarts", b.expr("count + 1", count=b.var("RaceStarts"))))
        lap = b.custom("LapStarted")
        b.chain(lap, b.set("LapStarts", b.expr("count + 1", count=b.var("LapStarts"))))
        finished = b.custom("RaceFinished")
        b.chain(finished, b.set("Finishes", b.expr("count + 1", count=b.var("Finishes"))), b.set("Total", finished["Total"]))
        attract = b.custom("AttractStarted")
        b.chain(attract, b.set("Attracts", b.expr("count + 1", count=b.var("Attracts"))))
    with author.region(RACE_TESTS, "Sectors: each split against the script's", EVENTS) as b:
        crossed = b.custom("SectorCrossed")
        known, near, far = worst_against(b, "Sectors", "ExpectedSectors", "SectorError", crossed["Time"])
        added = b.set("Sectors", b.pure("Array.Add", Array=b.var("Sectors"), Value=crossed["Time"]))
        b.chain(crossed, known)
        b.chain(known["True"], near, added)
        b.chain(known["False"], far, added)
    with author.region(RACE_TESTS, "Laps: each lap's time against the script's, and a best lap kept", EVENTS) as b:
        finished = b.custom("LapFinished")
        known, near, far = worst_against(b, "Laps", "ExpectedLaps", "LapError", finished["Time"])
        added = b.set("Laps", b.pure("Array.Add", Array=b.var("Laps"), Value=finished["Time"]))
        mask = b.set("BestMask", b.expr("mask * 2 + (best ? 1 : 0)", mask=b.var("BestMask"), best=finished["Best"]))
        best = b.branch(finished["Best"], comment="The ghost keeps the best lap, once every graph has had the broadcast (Inject)")
        b.chain(finished, known)
        b.chain(known["True"], near, added)
        b.chain(known["False"], far, added)
        b.chain(added, mask, best)
        b.chain(best["True"], b.set("KeepTime", finished["Time"]), b.set("KeepPending", True))


def write_module_graphs(author):
    write_graphs(author)
    author.finish(RACE_TESTS)


# --- The checks -------------------------------------------------------------------------------------------------------------

# How long a check waits for its reading (seconds of the game, from when it starts): the readings are taken about 7 s into
# the scene, and the race's checks start after the ride's, which end with the ride, about 32 s into it.
RACE_WAIT = 20


def driver(b, name):
    return b.get(name, RACE_TESTS, b.var("RaceTests"))


def checks():
    flow = next(scene.timeout for scene in Checks.SCENES if scene.name == "Flow")
    if 32 + RACE_WAIT >= flow:
        raise ValueError(f"the race's checks wait {RACE_WAIT} s after the ride, past the flow test scene's timeout of {flow} s")
    top = f"{Race.TOP_SPEED:.1f}"
    return (
        ("CheckGateTimes", "Gates: crossings timed within 0.5 ms of where the speed puts them",
         f"the gates time a body's crossings within {CROSSING_TOLERANCE * 1000:g} ms of where its speed puts them, from 10 to "
         f"{top} m/s"),
        ("CheckGateOnce", "Gates: each pass enters its gate's trigger once and is timed once",
         "each pass of a gate, the bike's top speed included, enters its trigger once and is timed once"),
        ("CheckCountdown", "Countdown: each race starts once, its countdown after StartRace",
         f"sent StartRace, the director counts down its {COUNTDOWN:g} s, takes no crossing of the line meanwhile, and starts "
         "the race once"),
        ("CheckLaps", "Laps: three laps a race and their sectors, timed and in order",
         "three laps begin, cross their sectors and finish in the gates' order, timed from the crossings, the best flagged"),
        ("CheckResults", "Results: a race ends after its third lap with its total",
         "a race ends in the results after its third lap, with the total of its laps, and takes no crossing after"),
        ("CheckRealLap", "Real crossing: the line crossed while racing begins lap 1 there",
         "a body crossing the line while the race runs begins lap 1 at the crossing's interpolated time"),
        ("CheckAbandon", "Abandon: back to the title and its attract mode",
         "an abandoned race goes back to the title, and the attract mode starts once"),
        ("CheckBestAlone", "Best lap: kept without samples when the ghost kept none",
         "a best lap is kept in the save slot without samples when the ghost runs no graph that keeps them"),
        ("CheckBestKept", "Best lap: kept in the save slot with its splits and samples",
         "the best lap is kept in the save slot with its splits and the ghost's samples, ten a second, saved once the results "
         "show and never in a fixed step"),
    )


def write_gate_times(b, name):
    done = b.expr(f"passes >= {len(PASSES)}", passes=driver(b, "Passes"))
    passed = b.both(done, b.expr(f"worst <= {CROSSING_TOLERANCE}", worst=driver(b, "Worst")))
    detail = b.fmt("{Passes} passes; the worst crossing {Worst} s from where the speed puts it", Passes=driver(b, "Passes"),
                   Worst=driver(b, "Worst"))
    run(b, Checks.check(b, name, done, RACE_WAIT, detail, passed))


def write_gate_once(b, name):
    done = b.expr(f"passes >= {len(PASSES)}", passes=driver(b, "Passes"))
    passed = b.expr(f"once == {len(PASSES)}", once=driver(b, "Once"))
    detail = b.fmt(f"{{Once}} of {len(PASSES)} passes entered their gate once and were timed once", Once=driver(b, "Once"))
    run(b, Checks.check(b, name, done, RACE_WAIT, detail, passed))


def write_countdown(b, name):
    steps = round(COUNTDOWN / Race.STEP)
    done = driver(b, "Done")
    passed = b.expr(f"abs(steps - {steps}) <= 1 && starts == {RACES} && early == 0", steps=driver(b, "CountdownSteps"),
                    starts=driver(b, "RaceStarts"), early=driver(b, "EarlyLap"))
    detail = b.fmt(f"the race started {{Steps}} steps after StartRace (the countdown's {steps}), {{Starts}} signals for "
                   f"{RACES} races; a crossing of the line in the countdown left lap {{Early}}", Steps=driver(b, "CountdownSteps"),
                   Starts=driver(b, "RaceStarts"), Early=driver(b, "EarlyLap"))
    run(b, Checks.check(b, name, done, RACE_WAIT, detail, passed))


def write_laps(b, name):
    race = expected_races()
    laps = Race.LAPS * len(SCRIPTS)
    done = driver(b, "Done")
    passed = b.expr(f"starts == {laps + 1} && laps == {laps} && sectors == {len(race['sectors'])} && lap <= {TIME_TOLERANCE} && "
                    f"sector <= {TIME_TOLERANCE} && mask == {race['mask']}", starts=driver(b, "LapStarts"),
                    laps=b.pure("Array.Length", "Length", Array=driver(b, "Laps")),
                    sectors=b.pure("Array.Length", "Length", Array=driver(b, "Sectors")), lap=driver(b, "LapError"),
                    sector=driver(b, "SectorError"), mask=driver(b, "BestMask"))
    times = ", ".join(f"{value:.3f}" for value in race["laps"])
    detail = b.fmt(f"laps {{Laps}} (the scripts' {times}), splits {{Sectors}}, best flags {{Mask}} (the scripts' {race['mask']})",
                   Laps=driver(b, "Laps"), Sectors=driver(b, "Sectors"), Mask=driver(b, "BestMask"))
    run(b, Checks.check(b, name, done, RACE_WAIT, detail, passed))


def write_results(b, name):
    total = expected_races()["races"][-1]["total"]
    done = driver(b, "Done")
    state = driver(b, "ResultsState")
    passed = b.both(b.equal(state, b.lit("string", "Results")),
                    b.expr(f"finishes == {len(SCRIPTS)} && abs(total - {round(total, 4)}) <= {TIME_TOLERANCE} && "
                           f"laps == {Race.LAPS * len(SCRIPTS)}", finishes=driver(b, "Finishes"), total=driver(b, "Total"),
                           laps=b.pure("Array.Length", "Length", Array=driver(b, "Laps"))))
    detail = b.fmt(f"{{Finishes}} finishes, the last with a total of {{Total}} s (the script's {total:.3f}), the director "
                   "{State}", Finishes=driver(b, "Finishes"), Total=driver(b, "Total"), State=state)
    run(b, Checks.check(b, name, done, RACE_WAIT, detail, passed))


def write_real_lap(b, name):
    done = driver(b, "Done")
    passed = b.expr(f"lap == 1 && error <= {CROSSING_TOLERANCE}", lap=driver(b, "RealLap"), error=driver(b, "RealError"))
    detail = b.fmt("the crossing began lap {Lap} at step {Step} and {Fraction} of the race, {Error} s from the lap's start",
                   Lap=driver(b, "RealLap"), Step=driver(b, "RealStep"), Fraction=driver(b, "RealFraction"),
                   Error=driver(b, "RealError"))
    run(b, Checks.check(b, name, done, RACE_WAIT, detail, passed))


def write_abandon(b, name):
    done = driver(b, "Done")
    passed = b.both(b.equal(driver(b, "AfterState"), b.lit("string", "Title")), b.expr("count == 1", count=driver(b, "Attracts")))
    detail = b.fmt("abandoned, the director is {State}, with {Count} attract signals", State=driver(b, "AfterState"),
                   Count=driver(b, "Attracts"))
    run(b, Checks.check(b, name, done, RACE_WAIT, detail, passed))


def write_best_alone(b, name):
    best = expected_races()["races"][0]["best"]
    done = driver(b, "Done")
    passed = b.expr(f"abs(saved - {round(best, 5)}) <= {TIME_TOLERANCE} && samples == 0", saved=driver(b, "FirstSaved"),
                    samples=driver(b, "FirstSamples"))
    detail = b.fmt(f"after the first race the slot kept {{Time}} s and {{Samples}} samples (its best lap's {best:.3f} s and none)",
                   Time=driver(b, "FirstSaved"), Samples=driver(b, "FirstSamples"))
    run(b, Checks.check(b, name, done, RACE_WAIT, detail, passed))


def write_best_kept(b, name):
    best = expected_races()["races"][-1]["best"]
    count = samples(best)
    done = driver(b, "Done")
    passed = b.expr(f"error <= {TIME_TOLERANCE} && saved == {count} && kept == {count} && saves == {len(SCRIPTS)} && !in_step",
                    error=driver(b, "SavedError"), saved=driver(b, "SavedSamples"), kept=driver(b, "Kept"), saves=driver(b, "Saves"),
                    in_step=driver(b, "SavedInStep"))
    detail = b.fmt(f"the slot keeps {{Time}} s, splits {{Splits}} and {{Samples}} samples (the best lap's {best:.3f} s "
                   f"and {count} samples); saved {{Saves}} times ({len(SCRIPTS)}), in a finish's step {{InStep}}",
                   Time=driver(b, "SavedTime"), Splits=driver(b, "SavedSplits"), Samples=driver(b, "SavedSamples"),
                   Saves=driver(b, "Saves"), InStep=driver(b, "SavedInStep"))
    run(b, Checks.check(b, name, done, RACE_WAIT, detail, passed))


WRITERS = {
    "CheckGateTimes": write_gate_times,
    "CheckGateOnce": write_gate_once,
    "CheckCountdown": write_countdown,
    "CheckLaps": write_laps,
    "CheckResults": write_results,
    "CheckRealLap": write_real_lap,
    "CheckAbandon": write_abandon,
    "CheckBestAlone": write_best_alone,
    "CheckBestKept": write_best_kept,
}


def write_check(b, function, name):
    WRITERS[function](b, name)


# --- The scene ---------------------------------------------------------------------------------------------------------------

ENTITIES = (("RaceTests", DRIVER, "Drives the race's checks: the gate probe's passes and a scripted race"),)
# A child of the race that is no gate.
MARKER = "Race Marker"


def make(create, handles):
    """The gate probe, parked high over the grid; the driver of the race's checks; the marker, made here and put under the
    race by prepare."""
    create(PROBE, {
        "Transform": {"Translation": park()},
        "RigidBody": {"Type": "Dynamic", "Mass": 1.0, "LinearDamping": 0.0, "AngularDamping": 0.0, "GravityFactor": 0.0,
                      "LockRotation": [True, True, True], "AllowSleep": False},
        "BoxCollider": {"HalfExtents": [PROBE_HALF] * 3}})

    create(DRIVER, {"VisualScript": {"Graph": handles.get(RACE_TESTS, RACE_TESTS)}})
    create(MARKER, {"Transform": {"Translation": [0.0, 0.0, 0.0]}})


def prepare(session, found):
    """The race's children reordered: the marker first, the line last. The director keeps the scene's ghost's samples and
    counts down COUNTDOWN; the ghost records no lap of the bike (its Bike cleared: the scripted races are none of the
    bike's); the driver knows the director, the probe and the ghost."""
    session.call("entity_reparent", {"entity": found[MARKER], "parent": found[Race.ROOT], "siblingIndex": 0})
    session.call("entity_reparent", {"entity": found[Race.GATES[0][0]], "parent": found[Race.ROOT]})
    Checks.set_variables(session, found[Race.ROOT], {"Ghost": Checks.entity_value(found[Ghost.ENTITY]),
                                                     "Countdown": {"Type": "Float", "Value": COUNTDOWN}})
    ghost = session.call("component_get", {"entity": found[Ghost.ENTITY], "component": "VisualScript"})["fields"]
    variables = {name: value for name, value in (ghost.get("Variables") or {}).items() if name != "Bike"}
    session.call("component_set", {"entity": found[Ghost.ENTITY], "component": "VisualScript", "fields": {"Variables": variables}})
    Checks.set_variables(session, found[DRIVER], {
        "Race": Checks.entity_value(found[Race.ROOT]), "Probe": Checks.entity_value(found[PROBE]),
        "Ghost": Checks.entity_value(found[Ghost.ENTITY])})


SUITE = Suite(checks, write_check, ENTITIES, own=(PROBE, DRIVER, MARKER), make=make, prepare=prepare)

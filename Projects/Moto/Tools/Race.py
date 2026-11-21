"""Moto's race: the trigger gates at the start and finish line and at the two sector arches, and the race director, which
runs the time trial (Docs/Architecture.md section 15, "The race"). Author.py's race part makes the Race entity, which runs
the director (Scripts/RaceDirector.sgraph), and its children Line, Gate 1 and Gate 2, each a static body whose box collider
is a trigger, running Scripts/Gate.sgraph, whose Gate numbers it (0 the line, 1 and 2 the sector gates); the circuit part's
solid arches and its Sector Gate entities stay as they are.

The gates. Each stands on the center line where Circuit.SECTOR_GATES puts its timing line, turned with the road, its box
reaching to the barriers on either side, from under the road to 3.5 m over it, and DEPTH deep along the lap, its timing
plane through its middle. A trigger is a contact the physics engine finds between a moving body and the box, so the bike's
chassis box (never its wheels, which pass through triggers) raises the gate's trigger events: DEPTH holds the bike at its
top speed (the red line in sixth gear) inside the box for at least two fixed steps (check()).

The timing does not wait for those events, which reach graphs once a frame, after the frame's steps: the director's
Event.FixedUpdate reads its subject's position (where the step before left it) at every step, and when the position has
passed a gate's plane forward since the step before, inside the gate's box and from no farther behind it than a step at top
speed (a teleport jumps farther), it interpolates the crossing between the two positions and sends itself GateCrossed. A
crossing is a fixed step and a fraction of one, the same at every frame rate; the race's times are steps since the start
signal times the fixed step. The trigger events count the subject's entries into the gates (Entries).

The director takes its gates from its children that run Gate.sgraph, in the order of their Gate numbers, whatever the
order of the children: another child is no gate, and numbers that are not 0 to n - 1, each once, are logged as an error
and leave the director timing nothing.

The director. Its states: Idle (as the scene starts while TitleOnStart is off, as Moto started before it had a race),
Title (the title over the attract mode: AttractStarted is broadcast), Countdown (Countdown seconds, counted in fixed steps;
Holding reads true), Racing (RaceStarted at the signal; lap 1 begins at the first crossing of the line after it), Results
(after the crossing that finishes lap Laps: RaceFinished). A crossing counts only in Racing and
in the gates' order: the line, Gate 1, Gate 2, the line. StartRace starts it from any state but the countdown (a race in
progress is started again), AbandonRace goes back to the title from a countdown, a race or its results: the screen's menus
(Menu.py) send them, from their buttons, which the Confirm action presses. It broadcasts RaceStarted, LapStarted(Lap),
SectorCrossed(Lap, Sector, Time), LapFinished(Lap, Time, Best), RaceFinished(Total) and AttractStarted, publishes State,
Lap, LapTime and BestLapTime with the read-outs below, and marks telemetry at each crossing it counts (Lap <n>,
Sector <lap>.<n>, Lap end).

The best lap is kept in the save slot SLOT: its time, its splits and the ghost's samples (GHOST_SAMPLES, at SAMPLE_RATE a
second, as public arrays of the graph GHOST_GRAPH on the director's Ghost). The director opens the slot once the scene has
started, takes the best lap from it and, when the ghost runs GHOST_GRAPH, writes the samples and the lap's time
(BestLapTime) into the ghost and sends it GHOST_LOADED. A lap that beats the best is the best at once (the title, the HUD
and the ghost use it) and is kept in memory until a screen: the director saves the slot in its update of the frame after
the results show (never in a fixed step: a save waits for the disk), or when a race that made one ends without results
(AbandonRace: the pause screen's QUIT TO TITLE) or is started again (StartRace). GHOST_GRAPH is the ghost's own graph
(Ghost.py: the arrays, BestLapTime, Delta and GHOST_LOADED, which it takes when the samples hold one lap and rides), so
the main scene's ghost rides the best lap the slot gave it, and the director saves the samples the ghost kept.

The pause. The director owns the Pause action, in every state: it stops the game (Time.TimeScale 0: from the next frame,
whose time step is scaled as it begins, no fixed step runs and the clocks, the countdown and the bike stand still) or
lets it go on (1), publishes Paused, which the menus' pause
screen follows; SetPaused does the same for any other graph (the pause screen's RESUME). The mix
follows the pause: in the frame's update after the director paused or resumed (every graph has seen the frame's presses
by then), it sends the audio director (AudioDirector) TogglePause when that director's Paused read-out is not the game's.
So a mix already muffled by another sender (a StartPaused director) is left as it is, and nothing toggles it twice.

    python Projects/Moto/Tools/Race.py      (prints the figures check() asserts)
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Bike  # noqa: E402  (the grid)
import Circuit  # noqa: E402  (the layout)
import Graphs  # noqa: E402  (writing graphs)
import Ghost  # noqa: E402  (the ghost, whose best lap's samples the director keeps)
import RacingLine  # noqa: E402  (the engine's red line)
import Sound  # noqa: E402  (the audio director the pause tells)
import Vehicle  # noqa: E402  (the ridden bike: its gearing and its chassis)
from Graphs import EFFECTS, ENTITY, EVENTS, RULES, private, public  # noqa: E402

ROOT = "Race"
DIRECTOR = Graphs.script("RaceDirector")
GATE = Graphs.script("Gate")

# --- The gates ------------------------------------------------------------------------------------------------------------

# The gates in the order of their numbers (the director's Gate): the line, then the two sector gates.
GATES = (("Line", 0.0), ("Gate 1", Circuit.SECTOR_GATES[0]), ("Gate 2", Circuit.SECTOR_GATES[1]))
# The project's fixed step (Moto.sproj) and the bike's top speed: the red line in sixth gear (m/s).
STEP = Vehicle.COLLISION_STEP
TOP_SPEED = Vehicle.road_speed(RacingLine.RED_LINE, 6)
# Each gate's box: half its depth along the lap (its timing plane through the middle), how far below the road it reaches,
# and its height above that (meters).
HALF_DEPTH = 2.0
DEPTH = 2.0 * HALF_DEPTH
BELOW = 0.5
HEIGHT = 4.0
# How far behind a gate's plane the position before a crossing may lie: a step and a half at top speed. A teleport across
# a plane jumps farther and is not a crossing.
REACH = round(1.5 * TOP_SPEED * STEP, 3)


def half_width(s):
    """Half a gate's width: to the farther barrier's face."""
    return round(max(Circuit.barrier_offset(s, "left"), Circuit.barrier_offset(s, "right")), 3)


def gate_transform(s):
    """A gate's transform: on the center line at the lap distance, its forward (-Z) along the travel."""
    return {"Translation": [round(value, 4) for value in Circuit.offset_point(s, 0.0)], "RotationEuler": [0.0, round(Circuit.yaw(s), 4), 0.0],
            "Scale": [1.0, 1.0, 1.0]}


def gate_box(s):
    """A gate's trigger box: its half extents and its center (the entity's frame)."""
    return [half_width(s), HEIGHT / 2.0, HALF_DEPTH], [0.0, HEIGHT / 2.0 - BELOW, 0.0]


def gate_components(number, s, graph):
    extents, center = gate_box(s)
    return {"Transform": gate_transform(s), "RigidBody": {"Type": "Static"},
            "BoxCollider": {"HalfExtents": extents, "Offset": center, "IsTrigger": True},
            "VisualScript": {"Graph": graph, "Variables": {"Gate": {"Type": "Int32", "Value": number}}}}


def gate_frame(number):
    """A gate's timing plane: a point on it and its normal along the travel (world)."""
    origin, forward, _ = Circuit.frame(GATES[number][1])
    return origin, forward


# --- The race -------------------------------------------------------------------------------------------------------------

LAPS = 3
COUNTDOWN = 3.0
STATES = ("Idle", "Title", "Countdown", "Racing", "Results")
PAUSE = "Pause"
# The audio director's event that blends the mix to its Paused snapshot and back, and its read-out of where the mix is.
TOGGLE_PAUSE = "TogglePause"
MIX_PAUSED = "Paused"
# The save slot of the best lap, and its values: the lap's time, its splits at the two sector gates and, with the ghost's
# samples, how many a second they are.
SLOT = "BestLap"
SAMPLE_RATE = 10.0
MAX_SAMPLES = 4096
# What the director reads and writes of the ghost: the graph, the public arrays of its best lap's samples (each saved under
# its own name) and the event that tells it they were loaded.
GHOST_GRAPH = Ghost.GHOST
GHOST_SAMPLES = (("BestLapPositions", "Strada.Vector3[]"), ("BestLapRotations", "Strada.Quaternion[]"),
                 ("BestLapEngineSpeeds", "float[]"), ("BestLapLoads", "float[]"))
GHOST_LOADED = "BestLapLoaded"
assert GHOST_SAMPLES == tuple((name, type_id) for name, type_id, _ in Ghost.BEST_LAP) and GHOST_LOADED == Ghost.LOADED[0], \
    "the director reads and writes the ghost's arrays and event by the ghost's names"
# The events the director broadcasts, with their parameters.
BROADCASTS = {
    "AttractStarted": (),
    "RaceStarted": (),
    "LapStarted": (("Lap", "int"),),
    "SectorCrossed": (("Lap", "int"), ("Sector", "int"), ("Time", "float")),
    "LapFinished": (("Lap", "int"), ("Time", "float"), ("Best", "bool")),
    "RaceFinished": (("Total", "float"),),
}
assert all(BROADCASTS[name] == parameters for name, parameters in (Ghost.LAP_STARTED, Ghost.LAP_FINISHED)), \
    "the director broadcasts the lap events the ghost receives, with their parameters"
FLOAT_ARRAY = "float[]"
VECTOR = "Strada.Vector3"
SAVE_SLOT = "Strada.SaveSlot"


def check():
    """Asserts what the gates promise; returns a summary."""
    problems = []
    top_step = TOP_SPEED * STEP
    chassis_length = 2.0 * Vehicle.CHASSIS_HALF[2]
    # The chassis box overlaps a box DEPTH deep over DEPTH plus its own length of travel: at top speed, at least two steps.
    steps_inside = math.floor((DEPTH + chassis_length) / top_step)
    if steps_inside < 2:
        problems.append(f"the bike at top speed is inside a gate for {steps_inside} steps, not two")
    if REACH >= HALF_DEPTH + chassis_length:
        problems.append("a crossing may start from outside the gate")
    # The grid bike's chassis stands clear of the line's box, so the race begins with it outside.
    grid, _ = Bike.grid_place()
    origin, forward = gate_frame(0)
    behind = -sum((grid[k] - origin[k]) * forward[k] for k in range(3))
    clear = behind - Vehicle.CHASSIS_HALF[2] - HALF_DEPTH
    if clear < 1.0:
        problems.append(f"the grid bike's chassis is {clear:.2f} m from the line's gate")
    # The gates stand on straights, far apart, and reach to the barriers.
    for name, s in GATES:
        if Circuit.curvature(s) != 0.0:
            problems.append(f"{name} does not stand on a straight")
    assert not problems, "; ".join(problems)
    widths = ", ".join(f"{name} {2.0 * half_width(s):.1f} m wide at {s:.1f} m" for name, s in GATES)
    return (f"{widths}; {DEPTH:.0f} m deep: the bike at its top speed ({TOP_SPEED:.2f} m/s, {top_step:.3f} m a step) is inside "
            f"for {steps_inside} steps or more; the grid bike's chassis stands {clear:.2f} m clear of the line's gate")


# --- The graphs ---------------------------------------------------------------------------------------------------------------

def declare(author):
    author.declare(GATE, "A timing gate: its trigger tells the race director when a body meets it.", variables=(
        public("Gate", "int", 0, "Which gate this is: 0 the start and finish line, 1 and 2 the sector gates", (0, 2)),
    ))
    author.declare(DIRECTOR, "The race director: the time trial's states from the title to the results, the laps and sectors "
                   "timed from the gates' interpolated crossings in fixed steps, and the best lap kept in a save slot.",
                   variables=(
        public("Subject", ENTITY, None, "What the gates time: the ridden bike"),
        public("Ghost", ENTITY, None, "The ghost whose best lap's samples are saved with the best lap and loaded back"),
        public("AudioDirector", ENTITY, None, "The audio director, sent TogglePause so that the mix follows the game's pause"),
        public("TitleOnStart", "bool", True, "On: the race opens on its title over the attract mode. Off: it starts idle, as "
               "Moto did before it had a race, until it is sent StartRace"),
        public("Laps", "int", LAPS, "How many laps the time trial has", (1, 9)),
        public("Countdown", "float", COUNTDOWN, "How long the countdown lasts (seconds)", (0.0, 10.0)),
        public("State", "string", "Idle", "Where the race is: Idle, Title, Countdown, Racing or Results (a read-out)"),
        public("Lap", "int", 0, "The lap being ridden, from 1; 0 before the first crossing of the line (a read-out)"),
        public("LapTime", "float", 0.0, "The lap's time so far, or the last lap's at the results (seconds, a read-out)"),
        public("BestLapTime", "float", 0.0, "The best lap's time, 0 when there is none (seconds, a read-out)"),
        public("LastLapTime", "float", 0.0, "The last lap finished (seconds, a read-out)"),
        public("RaceTime", "float", 0.0, "The race's time since lap 1 began (seconds, a read-out)"),
        public("CountdownLeft", "float", 0.0, "What is left of the countdown (seconds, a read-out)"),
        public("Holding", "bool", False, "Whether the bike should stand on the grid: through the countdown, for the riding "
               "graph to hold it (a read-out)"),
        public("Paused", "bool", False, "Whether the game is paused: time stands still and the pause screen shows (a read-out)"),
        public("Splits", FLOAT_ARRAY, [], "The lap's times at the sector gates crossed so far (seconds, a read-out)"),
        public("BestSplits", FLOAT_ARRAY, [], "The best lap's times at the sector gates (seconds, a read-out)"),
        public("LoadedLapTime", "float", 0.0, "The best lap the save slot gave as the scene started, 0 for none (a read-out)"),
        public("Saves", "int", 0, "How many times the best lap was saved into the slot (a read-out)"),
        public("Steps", "int", 0, "The fixed steps the director has counted (a read-out)"),
        public("StartStep", "int", 0, "The step of the start signal (a read-out)"),
        public("SubjectAt", VECTOR, [0.0, 0.0, 0.0], "Where the subject was at the step Steps counts (a read-out)"),
        public("Crossings", "int", 0, "How many crossings of a gate's plane the subject made (a read-out)"),
        public("Entries", "int", 0, "How many times the subject entered a gate's trigger (a read-out)"),
        public("LastGate", "int", -1, "The gate of the last crossing (a read-out)"),
        public("LastStep", "int", 0, "The step before the last crossing (a read-out)"),
        public("LastFraction", "float", 0.0, "How far into the step after LastStep the last crossing came (a read-out)"),
        private("Slot", SAVE_SLOT),
        private("Gates", "Strada.Entity[]"),
        private("GateCount", "int", 0),
        private("Misnumbered", "bool", False),
        private("Origins", "Strada.Vector3[]"),
        private("Forwards", "Strada.Vector3[]"),
        private("Rights", "Strada.Vector3[]"),
        private("Extents", "Strada.Vector3[]"),
        private("Centers", FLOAT_ARRAY),
        private("Distances", FLOAT_ARRAY),
        private("Tracked", ENTITY),
        private("Seen", "bool", False),
        private("Expected", "int", 0),
        private("LapStart", "float", 0.0),
        private("FirstLapStart", "float", 0.0),
        private("CountdownSteps", "int", 0),
        private("NewBest", "bool", False),
        private("MixPending", "bool", False),
    ), events=(
        ("GateCrossed", (("Gate", "int"), ("Step", "int"), ("Fraction", "float"))),
        ("GateEntered", (("Gate", "int"), ("Other", ENTITY))),
        ("StartRace", ()),
        ("AbandonRace", ()),
        ("SetPaused", (("Paused", "bool"),)),
    ), functions=(
        ("Title", False, (), (), ()),
        ("LoadBest", False, (), (), ()),
        ("SaveBest", False, (), (), ()),
        ("Track", False, (), (), (("At", VECTOR, [0.0, 0.0, 0.0]), ("Distance", "float", 0.0), ("Previous", "float", 0.0))),
        ("CountDown", False, (), (), ()),
        ("Clocks", False, (), (), ()),
        ("BeginLap", False, (("Number", "int"), ("Time", "float")), (), ()),
        ("FinishLap", False, (("Time", "float"),), (), (("LapTimed", "float", 0.0), ("Best", "bool", False))),
        ("FinishRace", False, (("Time", "float"),), (), ()),
        ("Line", False, (("Time", "float"),), (), ()),
        ("Sector", False, (("Gate", "int"), ("Time", "float")), (), (("Split", "float", 0.0),)),
    ))


def broadcast(b, name, comment=None, **arguments):
    settings = {"Name": name, "Parameters": [{"Name": key, "Type": value_type} for key, value_type in BROADCASTS[name]]}
    return b.add("Event.Broadcast", settings, "Count", comment=comment, **arguments)


def mark(b, label, comment=None):
    return b.call("Call:Strada.Telemetry.Mark(string)", Label=label, comment=comment)


def dt(b):
    return b.once("dt", lambda: b.pure("Get:Strada.Time.FixedDeltaTime", "Value"))


def is_state(b, state):
    """Whether the director is in a state (strings compare through Equal: an Expression has no strings)."""
    return b.once(("state", state), lambda: b.equal(b.var("State"), b.lit("string", state)))



def write_gate(author):
    with author.region(GATE, "Entered: a body met the gate, which tells the race director", EVENTS) as b:
        enter = b.event("Event.TriggerEnter")
        director = b.pure("Get:Strada.Entity.Parent", "Value")
        b.chain(enter, b.send("GateEntered", director, graph=DIRECTOR, Gate=b.var("Gate"), Other=enter["Other"]))


def write_director(author):
    write_functions(author)
    with author.region(DIRECTOR, "Start: the gates by number, the best lap from the slot, then the title", EVENTS) as b:
        create = b.event("Event.Create")
        settle = b.add("Flow.WaitFrames", Frames=1, comment="Every entity has run its Create first")
        children = b.once("children", lambda: b.pure("Get:Strada.Entity.Children", "Value", Target=b.self_()))
        counting = b.add("Flow.ForEach", Array=children)
        counts = b.branch(b.runs(GATE, counting["Element"]), comment="A child that runs Gate.sgraph is a gate; another is not")
        counted = b.set("GateCount", b.expr("count + 1", count=b.var("GateCount")))
        numbers = b.add("Flow.ForLoop", First=0, Last=b.expr("count - 1", count=b.var("GateCount")))
        number = numbers["Index"]
        each = b.add("Flow.ForEach", Array=children)
        gate = each["Element"]
        is_gate = b.branch(b.runs(GATE, gate))
        numbered = b.branch(b.equal(b.get("Gate", GATE, gate), number), comment="The gate of this number, wherever it stands")

        def append(variable, value):
            return b.set(variable, b.pure("Array.Add", Array=b.var(variable), Value=value))

        adds = [
            append("Gates", gate),
            append("Origins", b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=gate)),
            append("Forwards", b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=gate)),
            append("Rights", b.pure("Get:Strada.TransformComponent.Right", "Value", Target=gate)),
            append("Extents", b.pure("Get:Strada.BoxColliderComponent.HalfExtents", "Value", Target=gate)),
            append("Centers", b.expr("offset.Y", offset=b.pure("Get:Strada.BoxColliderComponent.Offset", "Value", Target=gate))),
            append("Distances", 0.0),
        ]
        once = b.branch(b.expr("length != number + 1", length=b.pure("Array.Length", "Length", Array=b.var("Gates")), number=number),
                        comment="No gate or two of this number")
        wrong = b.set("Misnumbered", True)
        misnumbered = b.branch(b.var("Misnumbered"))
        logged = b.call("Call:Strada.Log.Error(string)", Message="Race: the gates (the children that run Gate.sgraph) are not "
                        "numbered 0 to n - 1, each once; the race times no gate",
                        comment="An error of the scene: the race times nothing rather than the wrong gates")
        cleared = [b.set(name, b.lit(type_id, [])) for name, type_id in (
            ("Gates", "Strada.Entity[]"), ("Origins", "Strada.Vector3[]"), ("Forwards", "Strada.Vector3[]"),
            ("Rights", "Strada.Vector3[]"), ("Extents", "Strada.Vector3[]"), ("Centers", FLOAT_ARRAY), ("Distances", FLOAT_ARRAY))]
        load = b.own("LoadBest")
        titled = b.branch(b.var("TitleOnStart"))
        b.chain(create, settle)
        b.flow(settle["Completed"], counting)
        b.chain(counting["Body"], counts)
        b.flow(counts["True"], counted)
        b.flow(counting["Completed"], numbers)
        b.chain(numbers["Body"], each)
        b.chain(each["Body"], is_gate)
        b.flow(is_gate["True"], numbered)
        b.chain(numbered["True"], *adds)
        b.chain(each["Completed"], once)
        b.flow(once["True"], wrong)
        b.chain(numbers["Completed"], misnumbered)
        b.chain(misnumbered["True"], logged, *cleared, load)
        b.flow(misnumbered["False"], load)
        b.chain(load, titled)
        b.flow(titled["True"], b.own("Title"))
    with author.region(DIRECTOR, "Steps: each fixed step, the crossings, the countdown and the clocks", EVENTS) as b:
        fixed = b.event("Event.FixedUpdate")
        counted = b.set("Steps", b.expr("steps + 1", steps=b.var("Steps")))
        b.chain(fixed, counted, b.own("Track"), b.own("CountDown"), b.own("Clocks"))
    with author.region(DIRECTOR, "Crossed: a gate crossed in order while racing times a lap or a sector", RULES) as b:
        crossed = b.custom("GateCrossed")
        counts = b.branch(b.expr("racing && gate == expected", racing=is_state(b, "Racing"), gate=crossed["Gate"],
                                 expected=b.var("Expected")), comment="Any other crossing is ignored")
        time = b.expr("(float(step - start) + fraction) * dt", step=crossed["Step"], start=b.var("StartStep"),
                      fraction=crossed["Fraction"], dt=dt(b))
        line = b.branch(b.expr("gate == 0", gate=crossed["Gate"]))
        b.chain(crossed, counts)
        b.flow(counts["True"], line)
        b.flow(line["True"], b.own("Line", Time=time))
        b.flow(line["False"], b.own("Sector", Gate=crossed["Gate"], Time=time))
    with author.region(DIRECTOR, "Entered: the subject met a gate's trigger", RULES) as b:
        entered = b.custom("GateEntered")
        subject = b.branch(b.equal(entered["Other"], b.var("Subject")))
        b.chain(entered, subject)
        b.flow(subject["True"], b.set("Entries", b.expr("entries + 1", entries=b.var("Entries"))))
    with author.region(DIRECTOR, "Start race: an unsaved best lap saved, the countdown (a race ends)", RULES) as b:
        start = b.custom("StartRace")
        allowed = b.branch(b.pure("Operator.Not", A=is_state(b, "Countdown")))
        ended = b.branch(b.expr("racing && lap >= 1", racing=is_state(b, "Racing"), lap=b.var("Lap")), comment="A lap in progress ends")
        steps = b.set("CountdownSteps", b.expr("int(round(seconds / dt))", seconds=b.var("Countdown"), dt=dt(b)))
        resets = [b.set("State", "Countdown"), b.set("Holding", True), b.set("Lap", 0), b.set("LapTime", 0.0),
                  b.set("RaceTime", 0.0), b.set("Splits", b.lit(FLOAT_ARRAY, [])), b.set("CountdownLeft", b.var("Countdown"))]
        ending = mark(b, "Lap end")
        saved = b.own("SaveBest")
        b.chain(start, allowed)
        b.chain(allowed["True"], saved, ended)
        b.flow(ended["True"], ending)
        b.flow(ending, steps)
        b.flow(ended["False"], steps)
        b.chain(steps, *resets)
    with author.region(DIRECTOR, "Abandon: a race or its results end, its best lap saved, to the title", RULES) as b:
        abandon = b.custom("AbandonRace")
        running = b.branch(b.expr("counting || racing || results", counting=is_state(b, "Countdown"), racing=is_state(b, "Racing"),
                                  results=is_state(b, "Results")))
        lap = b.branch(b.expr("racing && lap >= 1", racing=is_state(b, "Racing"), lap=b.var("Lap")))
        ending = mark(b, "Lap end")
        title = b.own("Title")
        saved = b.own("SaveBest")
        b.chain(abandon, running)
        b.chain(running["True"], saved, lap)
        b.flow(lap["True"], ending)
        b.flow(ending, title)
        b.flow(lap["False"], title)
    with author.region(DIRECTOR, "Pause: the Pause action stops the game, or lets it go on", EVENTS) as b:
        pressed = b.event("Event.ActionPressed", {"Action": PAUSE})
        b.chain(pressed, b.send("SetPaused", b.self_(), graph=DIRECTOR, Paused=b.pure("Operator.Not", A=b.var("Paused"))))
    with author.region(DIRECTOR, "Set paused: time stands still or runs, and the mix is told", EFFECTS) as b:
        asked = b.custom("SetPaused")
        changes = b.branch(b.pure("Operator.NotEqual", A=asked["Paused"], B=b.var("Paused")), comment="Only a change")
        scale = b.call("Set:Strada.Time.TimeScale", Value=b.expr("paused ? 0.0 : 1.0", paused=asked["Paused"]),
                       comment="0: no fixed step runs, so the clocks, the countdown and the bike stand still")
        b.chain(asked, changes)
        b.chain(changes["True"], scale, b.set("Paused", asked["Paused"]), b.set("MixPending", True))
    with author.region(DIRECTOR, "Update: the mix follows the pause; a new best lap saved at the results", RULES) as b:
        update = b.event("Event.Update")
        tasks = b.sequence(2)
        shown = b.branch(b.both(b.var("NewBest"), is_state(b, "Results")),
                         comment="The frame after the results showed (a frame updates before its steps): never in a step")
        b.chain(update, tasks)
        b.flow(tasks["Then1"], shown)
        b.flow(shown["True"], b.own("SaveBest"))
        pending = b.branch(b.var("MixPending"), comment="Every graph has seen the frame's presses by its update")
        done = b.set("MixPending", False)
        audio = b.var("AudioDirector")
        known = b.branch(b.runs(Sound.AUDIO_DIRECTOR, audio))
        differs = b.branch(b.pure("Operator.NotEqual", A=b.get(MIX_PAUSED, Sound.AUDIO_DIRECTOR, audio), B=b.var("Paused")),
                           comment="A mix already where the pause puts it is left as it is")
        told = b.send(TOGGLE_PAUSE, audio, graph=Sound.AUDIO_DIRECTOR)
        b.flow(tasks["Then0"], pending)
        b.chain(pending["True"], done, known)
        b.flow(known["True"], differs)
        b.flow(differs["True"], told)


def slot_call(b, method, output=None, comment=None, **inputs):
    """A call of a method of the director's save slot."""
    return b.call(f"Call:Strada.SaveSlot.{method}", output, comment=comment, Target=b.var("Slot"), **inputs)


def slot_get(b, name, type_id, fallback):
    return b.pure("Call:Strada.SaveSlot.Get<T>(string,T)", Target=b.var("Slot"), Name=name, Fallback=b.lit(type_id, fallback))


def slot_has(b, name):
    return b.pure("Call:Strada.SaveSlot.Has(string)", Target=b.var("Slot"), Name=name)


def runs_ghost(b):
    return b.once("runs ghost", lambda: b.runs(GHOST_GRAPH, b.var("Ghost")))


def write_functions(author):
    with author.region(DIRECTOR, "Title: the title over the attract mode", EFFECTS, "Title") as b:
        b.chain(b.entry, b.set("State", "Title"), b.set("Holding", False), b.set("Lap", 0), b.set("LapTime", 0.0),
                b.set("CountdownLeft", 0.0), broadcast(b, "AttractStarted"), b.ret)
    with author.region(DIRECTOR, "Load best: the slot's best lap, and the ghost's samples", EFFECTS, "LoadBest") as b:
        opened = b.call("Call:Strada.SaveSlot.Open(string)", "Result", Name=SLOT, comment="Once: the slot is kept")
        kept = b.set("Slot", opened.out)
        held = b.branch(slot_has(b, "LapTime"), comment="A best lap was saved")
        time = slot_get(b, "LapTime", "float", 0.0)
        best = b.set("BestLapTime", time)
        loaded = b.set("LoadedLapTime", time)
        splits = b.set("BestSplits", slot_get(b, "Splits", FLOAT_ARRAY, []))
        ghost = b.branch(b.both(runs_ghost(b), slot_has(b, GHOST_SAMPLES[0][0])), comment="The ghost takes its samples")
        samples = [b.set(name, slot_get(b, name, type_id, []), graph=GHOST_GRAPH, target=b.var("Ghost")) for name, type_id in GHOST_SAMPLES]
        lap = b.set("BestLapTime", b.var("BestLapTime"), graph=GHOST_GRAPH, target=b.var("Ghost"),
                    comment="The lap's time, which the ghost holds its samples against")
        told = b.send(GHOST_LOADED, b.var("Ghost"), graph=GHOST_GRAPH)
        b.chain(b.entry, opened, kept, held)
        b.chain(held["True"], best, loaded, splits, ghost)
        b.chain(ghost["True"], *samples, lap, told, b.ret)
        b.flow(ghost["False"], b.ret)
        b.flow(held["False"], b.ret)
    with author.region(DIRECTOR, "Save best: a new best lap, its splits and the ghost's samples", EFFECTS, "SaveBest") as b:
        new = b.branch(b.var("NewBest"), comment="Saved only when the race beat the best lap")
        time = slot_call(b, "Set<T>(string,T)", Name="LapTime", Value=b.var("BestLapTime"))
        splits = slot_call(b, "Set<T>(string,T)", Name="Splits", Value=b.var("BestSplits"))
        rate = slot_call(b, "Set<T>(string,T)", Name="SampleRate", Value=b.lit("float", SAMPLE_RATE))
        ghost = b.branch(runs_ghost(b), comment="Only a ghost that runs the graph has its arrays read")
        count = b.pure("Array.Length", "Length", Array=b.get(GHOST_SAMPLES[0][0], GHOST_GRAPH, b.var("Ghost")))
        fits = b.branch(b.expr(f"count > 0 && count <= {MAX_SAMPLES}", count=count),
                        comment="A lap the ghost kept, within a list's 4,096 samples")
        stored = [slot_call(b, "Set<T>(string,T)", Name=name, Value=b.get(name, GHOST_GRAPH, b.var("Ghost"))) for name, _ in GHOST_SAMPLES]
        removed = [slot_call(b, "Remove(string)", "Result", Name=name) for name, _ in GHOST_SAMPLES]
        saved = slot_call(b, "Save()", "Result", comment="Waits for the disk: at the results or a menu, never in a step")
        counted = b.set("Saves", b.expr("saves + (saved ? 1 : 0)", saves=b.var("Saves"), saved=saved.out))
        done = b.set("NewBest", False)
        b.chain(b.entry, new)
        b.chain(new["True"], time, splits, rate, ghost)
        b.flow(ghost["True"], fits)
        b.flow(ghost["False"], removed[0])
        b.chain(fits["True"], *stored, saved)
        b.chain(fits["False"], *removed, saved)
        b.chain(saved, counted, done, b.ret)
        b.flow(new["False"], b.ret)
    with author.region(DIRECTOR, "Track: each gate's plane, crossed forward from within a step", RULES, "Track") as b:
        subject = b.var("Subject")
        known = b.branch(b.pure("IsValid", Value=subject))
        changed = b.branch(b.pure("Operator.NotEqual", A=subject, B=b.var("Tracked")), comment="Another subject: no step before")
        forget = b.set("Seen", False)
        track = b.set("Tracked", subject)
        at = b.set("At", b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=subject))
        shown = b.set("SubjectAt", b.var("At"))
        last = b.expr("length - 1", length=b.pure("Array.Length", "Length", Array=b.var("Gates")))
        loop = b.add("Flow.ForLoop", First=0, Last=last)
        index = loop["Index"]

        def element(variable):
            return b.add("Array.Get", None, "Element", Array=b.var(variable), Index=index).out

        origin, forward, right = element("Origins"), element("Forwards"), element("Rights")
        extent, center = element("Extents"), element("Centers")
        distance = b.set("Distance", b.expr("dot(at - origin, forward)", at=b.var("At"), origin=origin, forward=forward))
        previous = b.set("Previous", element("Distances"))
        crossed = b.branch(b.expr(f"seen && previous < 0 && distance >= 0 && previous > -{REACH} && "
                                  "abs(dot(at - origin, right)) <= extent.X && abs(at.Y - origin.Y - center) <= extent.Y",
                                  seen=b.var("Seen"), previous=b.var("Previous"), distance=b.var("Distance"), at=b.var("At"),
                                  origin=origin, right=right, extent=extent, center=center),
                           comment="Forward across the plane, inside the gate, from no farther than a step at top speed")
        step = b.expr("steps - 1", steps=b.var("Steps"))
        fraction = b.expr("previous / (previous - distance)", previous=b.var("Previous"), distance=b.var("Distance"))
        noted = [b.set("Crossings", b.expr("crossings + 1", crossings=b.var("Crossings"))), b.set("LastGate", index),
                 b.set("LastStep", step), b.set("LastFraction", fraction)]
        sent = b.send("GateCrossed", b.self_(), graph=DIRECTOR, Gate=index, Step=step, Fraction=fraction)
        kept = b.set("Distances", b.pure("Array.Set", Array=b.var("Distances"), Index=index, Value=b.var("Distance")))
        seen = b.set("Seen", True)
        b.chain(b.entry, known)
        b.flow(known["True"], changed)
        b.chain(changed["True"], forget, track, at)
        b.flow(changed["False"], at)
        b.chain(at, shown, loop)
        b.chain(loop["Body"], distance, previous, crossed)
        b.chain(crossed["True"], *noted, sent, kept)
        b.flow(crossed["False"], kept)
        b.flow(loop["Completed"], seen)
        b.chain(seen, b.ret)
        b.flow(known["False"], b.ret)
    with author.region(DIRECTOR, "Count down: the signal after the countdown's steps", RULES, "CountDown") as b:
        counting = b.branch(is_state(b, "Countdown"))
        left = b.set("CountdownSteps", b.expr("steps - 1", steps=b.var("CountdownSteps")))
        shown = b.set("CountdownLeft", b.expr("float(max(steps, 0)) * dt", steps=b.var("CountdownSteps"), dt=dt(b)))
        go = b.branch(b.expr("steps <= 0", steps=b.var("CountdownSteps")))
        started = [b.set("State", "Racing"), b.set("Holding", False), b.set("StartStep", b.var("Steps")), b.set("Lap", 0),
                   b.set("Expected", 0), b.set("CountdownLeft", 0.0)]
        signal = broadcast(b, "RaceStarted")
        b.chain(b.entry, counting)
        b.chain(counting["True"], left, shown, go)
        b.chain(go["True"], *started, signal, b.ret)
        b.flow(go["False"], b.ret)
        b.flow(counting["False"], b.ret)
    with author.region(DIRECTOR, "Clocks: the lap's and the race's time, in steps since the signal", RULES, "Clocks") as b:
        timing = b.branch(b.expr("racing && lap >= 1", racing=is_state(b, "Racing"), lap=b.var("Lap")))
        now = b.expr("float(steps - start) * dt", steps=b.var("Steps"), start=b.var("StartStep"), dt=dt(b))
        lap = b.set("LapTime", b.expr("now - start", now=now, start=b.var("LapStart")))
        race = b.set("RaceTime", b.expr("now - start", now=now, start=b.var("FirstLapStart")))
        b.chain(b.entry, timing)
        b.chain(timing["True"], lap, race, b.ret)
        b.flow(timing["False"], b.ret)
    with author.region(DIRECTOR, "Begin lap: its number, its start and its sectors to come", EFFECTS, "BeginLap") as b:
        b.chain(b.entry, b.set("Lap", b.entry["Number"]), b.set("LapStart", b.entry["Time"]), b.set("Splits", b.lit(FLOAT_ARRAY, [])),
                b.set("Expected", 1), b.set("LapTime", 0.0), broadcast(b, "LapStarted", Lap=b.entry["Number"]), b.ret)
    with author.region(DIRECTOR, "Finish lap: its time, and the best lap when it beats it", EFFECTS, "FinishLap") as b:
        timed = b.set("LapTimed", b.expr("time - start", time=b.entry["Time"], start=b.var("LapStart")))
        best = b.set("Best", b.expr("best <= 0 || timed < best", best=b.var("BestLapTime"), timed=b.var("LapTimed")))
        shown = [b.set("LastLapTime", b.var("LapTimed")), b.set("LapTime", b.var("LapTimed"))]
        beats = b.branch(b.var("Best"))
        kept = [b.set("BestLapTime", b.var("LapTimed")), b.set("BestSplits", b.var("Splits")), b.set("NewBest", True)]
        finished = broadcast(b, "LapFinished", Lap=b.var("Lap"), Time=b.var("LapTimed"), Best=b.var("Best"))
        b.chain(b.entry, timed, best, *shown, beats)
        b.chain(beats["True"], *kept, finished)
        b.flow(beats["False"], finished)
        b.chain(finished, b.ret)
    with author.region(DIRECTOR, "Finish race: the results (a new best lap is saved once they show)", EFFECTS, "FinishRace") as b:
        total = b.expr("time - start", time=b.entry["Time"], start=b.var("FirstLapStart"))
        b.chain(b.entry, mark(b, "Lap end", comment="On the row of the last lap's end"), b.set("State", "Results"),
                b.set("RaceTime", total), b.set("Expected", -1), b.set("Holding", False),
                broadcast(b, "RaceFinished", Total=b.var("RaceTime")), b.ret)
    with author.region(DIRECTOR, "Line: a lap begins, or ends and the next begins, or the race ends", RULES, "Line") as b:
        lap = b.var("Lap")
        marked = mark(b, b.fmt("Lap {Next}", Next=b.expr("lap + 1", lap=lap)), comment="Lap n begins; the race's end is laps + 1")
        riding = b.branch(b.expr("lap >= 1", lap=lap))
        finish = b.own("FinishLap", Time=b.entry["Time"])
        last = b.branch(b.expr("lap >= laps", lap=lap, laps=b.var("Laps")))
        first = b.set("FirstLapStart", b.entry["Time"])
        b.chain(b.entry, marked, riding)
        b.chain(riding["True"], finish, last)
        b.chain(last["True"], b.own("FinishRace", Time=b.entry["Time"]), b.ret)
        b.chain(last["False"], b.own("BeginLap", Number=b.expr("lap + 1", lap=lap), Time=b.entry["Time"]), b.ret)
        b.chain(riding["False"], first, b.own("BeginLap", Number=1, Time=b.entry["Time"]), b.ret)
    with author.region(DIRECTOR, "Sector: its split, marked and broadcast, and the next gate", RULES, "Sector") as b:
        split = b.set("Split", b.expr("time - start", time=b.entry["Time"], start=b.var("LapStart")))
        kept = b.set("Splits", b.pure("Array.Add", Array=b.var("Splits"), Value=b.var("Split")))
        marked = mark(b, b.fmt("Sector {Lap}.{Gate}", Lap=b.var("Lap"), Gate=b.entry["Gate"]))
        following = b.set("Expected", b.expr("(gate + 1) % count", gate=b.entry["Gate"],
                                             count=b.pure("Array.Length", "Length", Array=b.var("Gates"))))
        crossed = broadcast(b, "SectorCrossed", Lap=b.var("Lap"), Sector=b.entry["Gate"], Time=b.var("Split"))
        b.chain(b.entry, split, kept, marked, following, crossed, b.ret)


def write(author):
    write_gate(author)
    write_director(author)

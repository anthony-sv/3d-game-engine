"""The race's starts, a module of Checks.MODULES that has a test scene of its own (Scenes/Start.sscene, the main scene
without its props): the race director, the bike, the ghost and the menus wired as the main scene wires them, nothing
standing in for any of them, the race on its title as the game opens (Docs/Architecture.md section 15, "The race's
start"). Its runner rides the bike on its riding graph's test driver with the rider assists a new player has, and sends the
director StartRace from its own fixed steps, as the menus' buttons send it from theirs; every race must start from the
grid: the bike put back on the grid's place and heading and its vehicle reset (at rest in neutral, the engine at idle), then
held there through the countdown whatever its driver asks. Every start here comes TITLE_STEPS or more after the scene's
first step: a reset in the step after the vehicle was made launches it otherwise (Docs/Architecture.md section 15).

It is driven from the runner graph's Event.FixedUpdate by its own count of fixed steps and by what the bike and the
director read, never by a frame (the rule of the rides compared across frame rates), and logs every reading in one line
("Moto's starts: ..."), the same at every frame rate. The starts (STARTS), one after another:

1. From the title, the bike on the grid since the scene started (TITLE_STEPS): StartRace with the driver asking full
   throttle through the countdown and ShiftUp sent in it (a jump start), and a second StartRace in the countdown, which
   the director refuses.
2. From the results, in the middle of an automatic shift: the first race's lap 1 begins at the bike's real crossing of the
   line, the sector gates and the line are then sent as crossed (the scene's race has one lap), and with the results shown
   the bike rides on at full throttle until its automatic shift takes second gear, StartRace in that step. In this
   countdown the game is paused for PAUSE_FRAMES frames and resumed (the runner's Event.Update): no fixed step runs
   meanwhile, so the countdown and the bike stand still.
3. From a fall: the bike rolled onto its side at speed by a teleport, FALL_STEPS later.
4. From the gravel: the bike in the gravel trap outside Turn 6 at full throttle, its rear wheel spinning, GRAVEL_STEPS.
5. QUIT TO TITLE in a countdown (AbandonRace, as the pause screen sends it): the director at the title, the hold ended and
   the bike ridden off again.
6. A director that stops holding: a last StartRace, and FREE_AT steps into its countdown the director's Subject set to the
   runner (which runs no riding graph), so that it no longer holds the bike: the hold lapses on the bike's side in the next
   step, the countdown going on; then the Subject given back and QUIT TO TITLE.

After each signal the bike launches at full throttle for LAUNCH_STEPS: the launch's speed then, which every start must
give as the first does. The checks (latent functions of the update) wait for the line (StartsDone) and report what it
holds; the bands (BANDS) are stated beside them.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import BikeChecks  # noqa: E402  (the grid, the gravel trap)
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import Race  # noqa: E402  (the race director)
import RacingLine  # noqa: E402  (the engine's idle)
import Vehicle  # noqa: E402  (the ridden bike: its riding graph)
from Checks import Suite, run  # noqa: E402
from Graphs import private  # noqa: E402

BIKE = Vehicle.BIKE
DIRECTOR = Race.DIRECTOR
VECTOR = "Strada.Vector3"
ANGLE_AXIS = "Call:Strada.Quaternion.AngleAxis(float,Strada.Vector3)"

# --- The ride ------------------------------------------------------------------------------------------------------------

# The scene's countdown (seconds, the director's Countdown) and laps: short, so that the starts follow one another.
COUNTDOWN = Checks.START_COUNTDOWN
LAPS = Checks.START_LAPS
COUNTDOWN_STEPS = round(COUNTDOWN / BikeChecks.STEP)
# The launch after each signal (fixed steps at full throttle), and the starts it is read for.
LAUNCH_STEPS = 120
STARTS = ("title", "results", "fall", "gravel")
# In the first countdown, the step ShiftUp is sent and the step a second StartRace is; in the second, the step the game is
# paused and how many frames it stays paused; in the last, the step AbandonRace is sent.
SHIFT_AT = 10
AGAIN_AT = 20
PAUSE_AT = 20
PAUSE_FRAMES = 10
QUIT_AT = 10
FREE_AT = 10
# How many steps after the director let go of the bike it is read: the director's step, the riding graph's and the runner's
# fall in the scene's order of its graphs, and the hold lapses in the riding graph's first step without the director's.
FREE_STEPS = 3
# The fall: the bike teleported where it is, rolled ROLL degrees to its right at the speed it has, then FALL_STEPS; the
# gravel: GRAVEL_STEPS at full throttle in the trap from rest; after QUIT TO TITLE, QUIT_STEPS at full throttle.
ROLL = 75.0
FALL_STEPS = 45
GRAVEL_STEPS = 90
QUIT_STEPS = 60
# How many steps after StartRace the countdown's readings start: the return comes in the director's first fixed step at or
# after the request, and the hold the riding graph reads in the step after that at the latest.
SETTLED = 2
# How long the title shows before RACE (fixed steps): the bike has stood on the grid since the scene started.
TITLE_STEPS = 30
# The phases in the order they ride: (name, the function its case calls).
PHASES = (
    ("Title", "StartsTitle"),
    ("Count", "StartsCount"),
    ("Launch", "StartsLaunch"),
    ("Race", "StartsRace"),
    ("Results", "StartsResults"),
    ("Fall", "StartsFall"),
    ("Gravel", "StartsGravel"),
    ("Quit", "StartsQuit"),
    ("Free", "StartsFree"),
    ("Stand", "StartsStand"),
)

# --- The bands -------------------------------------------------------------------------------------------------------------

# At the start signal, each start's bike as the reset made it on the grid: within GRID_ROOM meters of the grid's place (it
# settles on its springs from the place the scene gives it, about 15 mm) and HEADING_ROOM degrees of its heading, each
# wheel's spin within SPIN_ROOM radians a second of still, the engine within IDLE_SHARE of its idle speed, in neutral, on
# lap 0; and its launch within LAUNCH_SHARE of the first start's.
GRID_ROOM = 0.02
HEADING_ROOM = 0.01
SPIN_ROOM = 0.01
IDLE_SHARE = 1.01
LAUNCH_SHARE = 0.005
# How long a check waits for its reading (seconds of the game, from when it starts); the scene's timeout stays above it.
WAIT = 60


def scene_timeout():
    return next(scene.timeout for scene in Checks.SCENES if scene.name == "Start")


# --- The runner graph's interface -----------------------------------------------------------------------------------------

ENTITIES = (
    ("Bike", "Bike", "The ridden bike, whose riding graph rides it on the values this graph writes"),
    ("Race", Race.ROOT, "The race director, which starts the races and puts the bike back on the grid"),
)
COUNT = len(STARTS)


def per_start(name, type_id, value):
    return private(name, type_id, [value] * COUNT)


VARIABLES = (
    private("StepStart", "double", 0.0),
    private("SPhase", "string", "Title"), private("SStep", "int", 0), private("SPhaseStep", "int", 0), private("SIndex", "int", 0),
    private("SAsked", "int", 0), private("SLastGear", "int", 0), private("SSent", "int", 0),
    per_start("FromSpeed", "float[]", -1.0), per_start("FromLean", "float[]", 0.0), per_start("FromGear", "int[]", -1),
    per_start("FromSpin", "float[]", 0.0),
    per_start("GridOff", "float[]", -1.0), per_start("HeadingOff", "float[]", -1.0), per_start("SpinOff", "float[]", -1.0),
    per_start("SignalEngine", "float[]", -1.0), per_start("SignalGear", "int[]", -1), per_start("SignalLap", "int[]", -1),
    per_start("CountSteps", "int[]", -1), per_start("HeldThrottle", "float[]", 0.0), per_start("HeldGear", "int[]", 0),
    per_start("Launch", "float[]", -1.0),
    private("ReturnsAfterAgain", "int", -1), private("AgainState", "string", ""),
    private("PausedFrames", "int", 0), private("PauseSteps", "int", -1), private("PauseAt", VECTOR), private("PauseMoved", "float", -1.0),
    private("PauseStepsMoved", "int", -1), private("PauseSent", "bool", False), private("PauseDone", "bool", False),
    private("QuitState", "string", ""), private("QuitHeld", "bool", True), private("QuitReturns", "int", -1),
    private("QuitGear", "int", -1), private("QuitSpeed", "float", -1.0), private("FreeState", "string", ""),
    private("FreeHeld", "bool", True), private("FreeThrottle", "float", -1.0), private("StartsDone", "bool", False),
)


def bike(b):
    return b.var("Bike")


def race(b):
    return b.var("Race")


def director(b, name):
    return b.once(("director", name), lambda: b.get(name, DIRECTOR, race(b)))


def elapsed(b):
    return b.once("starts elapsed", lambda: b.expr("step - start", step=b.var("SStep"), start=b.var("SPhaseStep")))


def first_step(b):
    return b.branch(b.expr("elapsed == 0", elapsed=elapsed(b)), comment="The phase's first step")


def enter(b, phase):
    return b.own("StartsEnter", Next=phase)


def drive(b, throttle=0.0):
    sets = []
    for name, value in (("Throttle", throttle), ("FrontBrake", 0.0), ("RearBrake", 0.0), ("Lean", 0.0)):
        sets.append(b.set(name, value, graph=BIKE, target=bike(b)))
    for first, second in zip(sets, sets[1:]):
        b.flow(first, second)
    return sets[0], sets[-1]


def put(b, array, index, value):
    return b.set(array, b.pure("Array.Set", Array=b.var(array), Index=index, Value=value))


def at(b, array, index):
    return b.add("Array.Get", None, "Element", Array=b.var(array), Index=index).out


def speed(b):
    return b.once("speed", lambda: b.pure("Get:Strada.VehicleComponent.ForwardSpeed", "Value", Target=bike(b)))


def gear(b):
    return b.once("gear", lambda: b.pure("Get:Strada.VehicleComponent.Gear", "Value", Target=bike(b)))


def lean(b):
    return b.once("lean", lambda: b.pure("Get:Strada.VehicleComponent.LeanAngle", "Value", Target=bike(b)))


def engine(b):
    return b.once("engine", lambda: b.pure("Get:Strada.VehicleComponent.EngineSpeed", "Value", Target=bike(b)))


def position(b):
    return b.once("position", lambda: b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=bike(b)))


def spin(b, index):
    state = b.pure("Call:Strada.VehicleComponent.GetWheelState(int)", Target=bike(b), Index=index)
    return b.pure("Get:Strada.VehicleWheelState.Spin", "Value", Target=state)


def is_state(b, state):
    return b.once(("state", state), lambda: b.equal(director(b, "State"), b.lit("string", state)))


def start_race(b, comment=None):
    """StartRace to the director, as the menus send it, with the request's step noted."""
    asked = b.set("SAsked", director(b, "Steps"))
    sent = b.send("StartRace", race(b), graph=DIRECTOR, comment=comment)
    b.flow(asked, sent)
    return asked, sent


def note_from(b):
    """The bike's state as a start is asked for."""
    index = b.var("SIndex")
    steps = [put(b, "FromSpeed", index, speed(b)), put(b, "FromLean", index, lean(b)), put(b, "FromGear", index, gear(b)),
             put(b, "FromSpin", index, spin(b, 1))]
    for first, second in zip(steps, steps[1:]):
        b.flow(first, second)
    return steps[0], steps[-1]


# --- The helpers and the phases ----------------------------------------------------------------------------------------------

def write_enter(b):
    begins = b.set("SPhaseStep", b.expr("step + 1", step=b.var("SStep")), comment="Its first step is the next")
    b.chain(b.entry, begins, b.set("SPhase", b.entry["Next"]), b.ret)


def write_title(b):
    """The title shows: the bike on its test driver (the assists as a new player has them), asked to rest; StartRace."""
    first, last = drive(b)
    test = b.set("Driver", Vehicle.TEST, graph=BIKE, target=bike(b))
    titled = b.branch(b.both(is_state(b, "Title"), b.expr(f"elapsed >= {TITLE_STEPS}", elapsed=elapsed(b))),
                      comment="The title shows a while, as a player reads it")
    noted_first, noted_last = note_from(b)
    asked, sent = start_race(b, comment="RACE on the title")
    b.chain(b.entry, test, first)
    b.flow(last, titled)
    b.flow(titled["True"], noted_first)
    b.flow(noted_last, asked)
    b.chain(sent, enter(b, "Count"), b.ret)
    b.flow(titled["False"], b.ret)


def write_count(b):
    """The countdown: full throttle asked throughout, ShiftUp sent, the most throttle and gear the vehicle applied once the
    bike is back, and the bike as each counted step finds it, the last before the signal kept; at the signal the
    countdown's steps. In the first, a second StartRace; in the second, the pause."""
    first, last = drive(b, 1.0)
    index = b.var("SIndex")
    shift = b.branch(b.expr(f"index == 0 && elapsed == {SHIFT_AT}", index=index, elapsed=elapsed(b)), comment="A jump start asked")
    shifted = b.send("ShiftUp", bike(b), graph=BIKE)
    again = b.branch(b.expr(f"index == 0 && elapsed == {AGAIN_AT}", index=index, elapsed=elapsed(b)),
                     comment="A second StartRace in the countdown")
    resent = b.send("StartRace", race(b), graph=DIRECTOR, comment="Refused: the director is counting down")
    noted = [b.set("ReturnsAfterAgain", director(b, "Returns")), b.set("AgainState", director(b, "State"))]
    pause = b.branch(b.expr(f"index == 1 && elapsed == {PAUSE_AT}", index=index, elapsed=elapsed(b)),
                     comment="The game paused in the countdown; the frames resume it")
    paused = b.send("SetPaused", race(b), graph=DIRECTOR, Paused=True)
    pause_noted = [b.set("PauseSent", True)]
    settled = b.branch(b.expr(f"elapsed >= {SETTLED}", elapsed=elapsed(b)), comment="Back on the grid and held")
    counting = b.branch(is_state(b, "Countdown"),
                        comment="Each step of the countdown: the bike as the step before left it, the last before the signal kept")
    held = [put(b, "HeldThrottle", index, b.expr("max(most, applied)", most=at(b, "HeldThrottle", index),
                                                 applied=b.pure("Get:Strada.VehicleComponent.Throttle", "Value", Target=bike(b)))),
            put(b, "HeldGear", index, b.expr("max(most, gear)", most=at(b, "HeldGear", index), gear=gear(b)))]
    signal = b.branch(is_state(b, "Racing"), comment="The start signal")
    grid, yaw, along = BikeChecks.grid_spot()
    forward = b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=bike(b))
    readings = [
        put(b, "GridOff", index, b.expr("distance(at, grid)", at=position(b), grid=b.lit(VECTOR, grid))),
        put(b, "HeadingOff", index, b.expr("abs(degrees(atan2(a.Z * c.X - a.X * c.Z, a.X * c.X + a.Z * c.Z)))",
                                           a=b.lit(VECTOR, along), c=forward)),
        put(b, "SpinOff", index, b.expr("max(abs(front), abs(rear))", front=spin(b, 0), rear=spin(b, 1))),
        put(b, "SignalEngine", index, engine(b)), put(b, "SignalGear", index, gear(b)), put(b, "SignalLap", index, director(b, "Lap")),
    ]
    steps = put(b, "CountSteps", index, b.expr("start - asked", start=director(b, "StartStep"), asked=b.var("SAsked")))
    b.chain(b.entry, first)
    b.flow(last, shift)
    b.chain(shift["True"], shifted, again)
    b.flow(shift["False"], again)
    b.chain(again["True"], resent, *noted, pause)
    b.flow(again["False"], pause)
    b.chain(pause["True"], paused, *pause_noted, settled)
    b.flow(pause["False"], settled)
    b.chain(settled["True"], counting)
    b.flow(settled["False"], b.ret)
    b.chain(counting["True"], *held, *readings, b.ret)
    b.flow(counting["False"], signal)
    b.chain(signal["True"], steps, enter(b, "Launch"), b.ret)
    b.flow(signal["False"], b.ret)


def write_launch(b):
    """Full throttle from the signal for LAUNCH_STEPS: the speed then; then the next start's way there."""
    first, last = drive(b, 1.0)
    index = b.var("SIndex")
    done = b.branch(b.expr(f"elapsed >= {LAUNCH_STEPS}", elapsed=elapsed(b)))
    reading = put(b, "Launch", index, speed(b))
    following = b.add("Flow.Switch", {"Type": "int", "Cases": list(range(1, COUNT + 1))}, Selection=index,
                      comment="The next start, counted")
    nexts = ("Race", "Fall", "Gravel", "Quit")
    counted = b.set("SIndex", b.expr("index + 1", index=index))
    b.chain(b.entry, first)
    b.flow(last, done)
    b.chain(done["True"], reading, counted, following)
    b.flow(done["False"], b.ret)
    for case, phase in enumerate(nexts, 1):
        b.chain(following[f"Case:{case}"], enter(b, phase), b.ret)
    b.flow(following["Default"], b.ret)


def write_race(b):
    """The first race: lap 1 begun at the bike's real crossing of the line, then the sector gates and the line sent as
    crossed, a step apart, which finishes the scene's one lap: the results."""
    first, last = drive(b, 1.0)
    begun = b.branch(b.expr("lap >= 1 && sent < 3", lap=director(b, "Lap"), sent=b.var("SSent")),
                     comment="Lap 1 begun; then a gate a step")
    gate = b.expr("sent == 2 ? 0 : sent + 1", sent=b.var("SSent"))
    crossed = b.send("GateCrossed", race(b), graph=DIRECTOR, Gate=gate, Step=director(b, "Steps"), Fraction=0.5,
                     comment="As the gate's timing sends it")
    counted = b.set("SSent", b.expr("sent + 1", sent=b.var("SSent")))
    results = b.branch(is_state(b, "Results"))
    b.chain(b.entry, first)
    b.flow(last, begun)
    b.chain(begun["True"], crossed, counted, results)
    b.flow(begun["False"], results)
    b.chain(results["True"], b.set("SLastGear", gear(b)), enter(b, "Results"), b.ret)
    b.flow(results["False"], b.ret)


def write_results(b):
    """The results show; full throttle until the automatic shift takes the next gear: StartRace in that step (RACE AGAIN)."""
    first, last = drive(b, 1.0)
    shifted = b.branch(b.expr("gear > last", gear=gear(b), last=b.var("SLastGear")), comment="In the middle of a shift")
    kept = b.set("SLastGear", gear(b))
    noted_first, noted_last = note_from(b)
    asked, sent = start_race(b, comment="RACE AGAIN on the results")
    b.chain(b.entry, first)
    b.flow(last, shifted)
    b.flow(shifted["True"], noted_first)
    b.flow(noted_last, asked)
    b.chain(sent, enter(b, "Count"), b.ret)
    b.chain(shifted["False"], kept, b.ret)


def write_fall(b):
    """The bike rolled onto its side where it is, at its speed, then FALL_STEPS; StartRace."""
    start = first_step(b)
    _, yaw, _ = BikeChecks.grid_spot()
    turn = b.pure("Operator.Multiply", A=b.pure(ANGLE_AXIS, Degrees=yaw, Axis=[0.0, 1.0, 0.0]),
                  B=b.pure(ANGLE_AXIS, Degrees=-ROLL, Axis=[0.0, 0.0, 1.0]))
    rolled = b.call(BikeChecks.TELEPORT, Target=bike(b), Position=b.expr("at + vec3(0, 0.3, 0)", at=position(b)), Rotation=turn,
                    comment="Rolled to its right where it is, keeping its speed")
    first, last = drive(b)
    fallen = b.branch(b.expr(f"elapsed >= {FALL_STEPS}", elapsed=elapsed(b)))
    noted_first, noted_last = note_from(b)
    asked, sent = start_race(b, comment="RACE from a fall")
    b.chain(b.entry, start)
    b.chain(start["True"], rolled, first)
    b.flow(start["False"], first)
    b.flow(last, fallen)
    b.flow(fallen["True"], noted_first)
    b.flow(noted_last, asked)
    b.chain(sent, enter(b, "Count"), b.ret)
    b.flow(fallen["False"], b.ret)


def write_gravel(b):
    """In the gravel trap outside Turn 6 from rest, full throttle for GRAVEL_STEPS, the rear wheel spinning; StartRace."""
    start = first_step(b)
    spot, yaw, _ = BikeChecks.gravel_spot()
    placed = b.call(BikeChecks.TELEPORT, Target=bike(b), Position=spot, Rotation=b.pure(ANGLE_AXIS, Degrees=yaw, Axis=[0.0, 1.0, 0.0]),
                    comment="Into the gravel trap")
    still = [b.call("Set:Strada.RigidBodyComponent.LinearVelocity", Target=bike(b), Value=[0.0, 0.0, 0.0]),
             b.call("Set:Strada.RigidBodyComponent.AngularVelocity", Target=bike(b), Value=[0.0, 0.0, 0.0])]
    first, last = drive(b, 1.0)
    spun = b.branch(b.expr(f"elapsed >= {GRAVEL_STEPS}", elapsed=elapsed(b)))
    noted_first, noted_last = note_from(b)
    asked, sent = start_race(b, comment="RACE from the gravel")
    b.chain(b.entry, start)
    b.chain(start["True"], placed, *still, first)
    b.flow(start["False"], first)
    b.flow(last, spun)
    b.flow(spun["True"], noted_first)
    b.flow(noted_last, asked)
    b.chain(sent, enter(b, "Count"), b.ret)
    b.flow(spun["False"], b.ret)


def write_quit(b):
    """A last StartRace, then AbandonRace in its countdown (QUIT TO TITLE): the title, the hold ended; full throttle after."""
    first, last = drive(b, 1.0)
    begin = first_step(b)
    asked, sent = start_race(b, comment="A last race, left in its countdown")
    quit_now = b.branch(b.expr(f"elapsed == {QUIT_AT}", elapsed=elapsed(b)))
    abandoned = b.send("AbandonRace", race(b), graph=DIRECTOR, comment="QUIT TO TITLE")
    noted = b.branch(b.expr(f"elapsed == {QUIT_AT + SETTLED}", elapsed=elapsed(b)), comment="The director's next steps")
    readings = [b.set("QuitState", director(b, "State")), b.set("QuitHeld", b.get(Vehicle.HELD, BIKE, bike(b))),
                b.set("QuitReturns", director(b, "Returns"))]
    done = b.branch(b.expr(f"elapsed >= {QUIT_AT + QUIT_STEPS}", elapsed=elapsed(b)))
    ridden = [b.set("QuitGear", gear(b)), b.set("QuitSpeed", speed(b))]
    b.chain(b.entry, first)
    b.flow(last, begin)
    b.flow(begin["True"], asked)
    b.flow(sent, quit_now)
    b.flow(begin["False"], quit_now)
    b.chain(quit_now["True"], abandoned, noted)
    b.flow(quit_now["False"], noted)
    b.chain(noted["True"], *readings, done)
    b.flow(noted["False"], done)
    b.chain(done["True"], *ridden, enter(b, "Free"), b.ret)
    b.flow(done["False"], b.ret)


def write_free(b):
    """A last StartRace; FREE_AT steps into its countdown the director's Subject set to the runner, so that the director holds
    the bike no more: FREE_STEPS steps later the bike's Held and the throttle the vehicle applied (the driver asks it full),
    the director still counting down; then the Subject given back and QUIT TO TITLE."""
    first, last = drive(b, 1.0)
    begin = first_step(b)
    asked, sent = start_race(b, comment="A race whose director lets go of the bike in its countdown")
    away = b.branch(b.expr(f"elapsed == {FREE_AT}", elapsed=elapsed(b)))
    changed = b.set("Subject", b.self_(), graph=DIRECTOR, target=race(b), comment="A subject that runs no riding graph")
    noted = b.branch(b.expr(f"elapsed == {FREE_AT + FREE_STEPS}", elapsed=elapsed(b)), comment="The hold lapsed on the bike's side")
    readings = [b.set("FreeState", director(b, "State")), b.set("FreeHeld", b.get(Vehicle.HELD, BIKE, bike(b))),
                b.set("FreeThrottle", b.pure("Get:Strada.VehicleComponent.Throttle", "Value", Target=bike(b))),
                b.set("Subject", bike(b), graph=DIRECTOR, target=race(b))]
    abandoned = b.send("AbandonRace", race(b), graph=DIRECTOR, comment="QUIT TO TITLE")
    done = b.branch(b.expr(f"elapsed >= {FREE_AT + FREE_STEPS + SETTLED}", elapsed=elapsed(b)))
    b.chain(b.entry, first)
    b.flow(last, begin)
    b.flow(begin["True"], asked)
    b.flow(sent, away)
    b.flow(begin["False"], away)
    b.chain(away["True"], changed, noted)
    b.flow(away["False"], noted)
    b.chain(noted["True"], *readings, abandoned, done)
    b.flow(noted["False"], done)
    b.chain(done["True"], enter(b, "Stand"), b.ret)
    b.flow(done["False"], b.ret)


def write_stand(b):
    """Every reading logged in one line."""
    first, last = drive(b)
    readings = b.fmt(
        "Moto's starts, from the title, the results, a fall and the gravel: asked at speeds {FromSpeed} m/s, leans {FromLean} "
        "degrees, gears {FromGear}, the rear wheel spinning {FromSpin} rad/s; at the signal {GridOff} m from the grid, "
        "{HeadingOff} degrees off its heading, the wheels at most {SpinOff} rad/s, the engine at {Engine} rpm in gears {Gear}, "
        "laps {Lap}, {Steps} steps after StartRace; through the countdowns the vehicle applied at most {Throttle} of the "
        "throttle and gears {Held}; launched to {Launch} m/s; a second StartRace in the countdown left {Again} with {Returns} "
        "returns; while paused the bike moved {Moved} m and the director counted {StepsMoved} steps; QUIT TO TITLE in a countdown "
        "left {QuitState}, held {QuitHeld}, {QuitReturns} returns, then gear {QuitGear} at {QuitSpeed} m/s; a director that let go of "
        "the bike in its countdown left it held {FreeHeld}, the throttle applied {FreeThrottle}, the director {FreeState}; done at "
        "step {Step}",
        FromSpeed=b.var("FromSpeed"), FromLean=b.var("FromLean"), FromGear=b.var("FromGear"),
        FromSpin=b.var("FromSpin"), GridOff=b.var("GridOff"), HeadingOff=b.var("HeadingOff"),
        SpinOff=b.var("SpinOff"), Engine=b.var("SignalEngine"), Gear=b.var("SignalGear"), Lap=b.var("SignalLap"),
        Steps=b.var("CountSteps"), Throttle=b.var("HeldThrottle"), Held=b.var("HeldGear"), Launch=b.var("Launch"),
        Again=b.var("AgainState"), Returns=b.var("ReturnsAfterAgain"), Moved=b.var("PauseMoved"),
        StepsMoved=b.var("PauseStepsMoved"), QuitState=b.var("QuitState"), QuitHeld=b.var("QuitHeld"),
        QuitReturns=b.var("QuitReturns"), QuitGear=b.var("QuitGear"), QuitSpeed=b.var("QuitSpeed"), FreeHeld=b.var("FreeHeld"),
        FreeThrottle=b.var("FreeThrottle"), FreeState=b.var("FreeState"), Step=b.var("SStep"))
    log = b.call("Call:Strada.Log.Info(string)", Message=readings, comment="Every reading, the same at every frame rate")
    b.chain(b.entry, first)
    b.chain(last, log, b.set("StartsDone", True), enter(b, "Done"), b.ret)


HELPERS = (
    (("StartsEnter", False, (("Next", "string"),), (), ()), "Starts enter: the next phase, which begins at the next step",
     write_enter),
    (("StartsTitle", False, (), (), ()), "Title: the bike on its test driver, StartRace", write_title),
    (("StartsCount", False, (), (), ()), "Count: full throttle asked through the countdown, the signal read", write_count),
    (("StartsLaunch", False, (), (), ()), "Launch: full throttle from the signal, its speed", write_launch),
    (("StartsRace", False, (), (), ()), "Race: lap 1 at the real line, then the gates sent: the results", write_race),
    (("StartsResults", False, (), (), ()), "Results: on at full throttle, RACE AGAIN in a shift", write_results),
    (("StartsFall", False, (), (), ()), "Fall: rolled onto its side at speed, then StartRace", write_fall),
    (("StartsGravel", False, (), (), ()), "Gravel: spinning in the trap, then StartRace", write_gravel),
    (("StartsQuit", False, (), (), ()), "Quit: a countdown left for the title, the bike free again", write_quit),
    (("StartsFree", False, (), (), ()), "Free: the director lets go of the bike, which the hold no longer holds", write_free),
    (("StartsStand", False, (), (), ()), "Stand: every reading logged", write_stand),
)


def write_ride(b):
    """The starts: each fixed step the phase's function, then the step's count."""
    steps = b.sequence(2)
    phases = b.add("Flow.Switch", {"Type": "string", "Cases": [name for name, _ in PHASES]}, Selection=b.var("SPhase"))
    b.chain(b.entry, steps)
    b.flow(steps["Then0"], phases)
    for name, function in PHASES:
        b.flow(phases[f"Case:{name}"], b.own(function))
    b.chain(steps["Then1"], b.set("SStep", b.expr("step + 1", step=b.var("SStep"))), b.ret)


def write_frames(b):
    """Each frame while the countdown's pause holds: the director's steps and the bike stand still; after PAUSE_FRAMES frames
    the game is resumed, as the pause screen's RESUME does."""
    waiting = b.branch(b.expr("sent && !done", sent=b.var("PauseSent"), done=b.var("PauseDone")))
    paused = b.branch(director(b, "Paused"), comment="Paused: no fixed step runs")
    first = b.branch(b.expr("frames == 0", frames=b.var("PausedFrames")), comment="The first paused frame: where all stands")
    noted = [b.set("PauseSteps", director(b, "Steps")), b.set("PauseAt", position(b))]
    counted = b.set("PausedFrames", b.expr("frames + 1", frames=b.var("PausedFrames")))
    over = b.branch(b.expr(f"frames >= {PAUSE_FRAMES}", frames=b.var("PausedFrames")))
    readings = [b.set("PauseMoved", b.expr("distance(at, before)", at=position(b), before=b.var("PauseAt"))),
                b.set("PauseStepsMoved", b.expr("steps - before", steps=director(b, "Steps"), before=b.var("PauseSteps")))]
    resumed = b.send("SetPaused", race(b), graph=DIRECTOR, Paused=False, comment="RESUME")
    b.chain(b.entry, waiting)
    b.flow(waiting["True"], paused)
    b.chain(paused["True"], first)
    b.chain(first["True"], *noted, counted)
    b.flow(first["False"], counted)
    b.chain(counted, over)
    b.chain(over["True"], *readings, resumed, b.set("PauseDone", True), b.ret)
    for source in (waiting["False"], paused["False"], over["False"]):
        b.flow(source, b.ret)


STEPS = (("StartsRide", "Starts: each fixed step, the phase's driving", write_ride),)
FRAMES = (("StartsFrames", "Pause: the countdown's pause read each frame, then resumed", write_frames),)

# --- The checks ------------------------------------------------------------------------------------------------------------


def checks():
    if WAIT >= scene_timeout():
        raise ValueError(f"a start check waits {WAIT} s, not less than the start test scene's timeout of {scene_timeout()} s")
    return (
        ("CheckJumpStart", "Jump start: full throttle and ShiftUp through the countdown move nothing",
         "a throttle held open and a shift asked through the countdown move nothing: the bike waits in neutral, braked"),
        ("CheckStartFromTitle", "Title: the race starts on the grid, the bike at rest in neutral",
         "a race started from the title begins on the grid, its wheels still, the engine at idle in neutral"),
        ("CheckStartFromResults", "Results: RACE AGAIN in a shift starts on the grid",
         "a race started again from the results in the middle of a shift begins on the grid as the first did"),
        ("CheckStartFromFall", "Fall: a race started from a fall begins on the grid",
         "a race started from a fall begins on the grid as the first did"),
        ("CheckStartFromGravel", "Gravel: a race started from the gravel begins on the grid",
         "a race started from the gravel begins on the grid as the first did"),
        ("CheckSecondStart", "Second start: StartRace in the countdown is refused",
         "a second StartRace in the countdown is refused: one countdown, one return"),
        ("CheckPausedCountdown", "Pause: the countdown and the bike stand still while paused",
         "a pause in the countdown stops it and the bike, and the countdown goes on from where it was"),
        ("CheckQuitCountdown", "Quit: QUIT TO TITLE in a countdown ends the hold",
         "QUIT TO TITLE in a countdown goes back to the title and lets the bike go, with no return after"),
        ("CheckFreedHold", "Let go: a director that stops holding frees the bike",
         "a bike its director no longer holds in a countdown is free in the next step, whatever else the director does"),
    )


def check_start(b, name, index):
    """A start's readings at the signal against the bands, and its launch against the first start's."""
    least = round(IDLE_SHARE * RacingLine.IDLE, 1)
    ready = b.var("StartsDone")
    inputs = {"off": at(b, "GridOff", index), "heading": at(b, "HeadingOff", index), "spin": at(b, "SpinOff", index),
              "engine": at(b, "SignalEngine", index), "gear": at(b, "SignalGear", index), "lap": at(b, "SignalLap", index),
              "steps": at(b, "CountSteps", index), "throttle": at(b, "HeldThrottle", index), "held": at(b, "HeldGear", index),
              "launch": at(b, "Launch", index), "first": at(b, "Launch", 0)}
    passed = b.both(ready, b.expr(
        f"off >= 0 && off <= {GRID_ROOM} && heading <= {HEADING_ROOM} && spin <= {SPIN_ROOM} && engine <= {least} && gear == 0 && "
        f"lap == 0 && steps >= {COUNTDOWN_STEPS} && steps <= {COUNTDOWN_STEPS + 1} && throttle == 0 && held == 0 && "
        f"abs(launch - first) <= {LAUNCH_SHARE} * first", **inputs))
    text = b.fmt(
        f"asked at {{Speed}} m/s, leaned {{Lean}} degrees in gear {{From}}; at the signal {{Off}} m from the grid (at most "
        f"{GRID_ROOM}), {{Heading}} degrees off its heading (at most {HEADING_ROOM}), the wheels at most {{Spin}} rad/s (at most "
        f"{SPIN_ROOM}), the engine at {{Engine}} rpm (at most {least}) in gear {{Gear}}, lap {{Lap}}, {{Steps}} steps after "
        f"StartRace (the countdown's {COUNTDOWN_STEPS}); the vehicle applied at most {{Throttle}} of the throttle and gear {{Held}} "
        f"through it; launched to {{Launch}} m/s against the first start's {{First}} (within {LAUNCH_SHARE * 100:g} %)",
        Speed=at(b, "FromSpeed", index), Lean=at(b, "FromLean", index), From=at(b, "FromGear", index), Off=inputs["off"],
        Heading=inputs["heading"], Spin=inputs["spin"], Engine=inputs["engine"], Gear=inputs["gear"], Lap=inputs["lap"],
        Steps=inputs["steps"], Throttle=inputs["throttle"], Held=inputs["held"], Launch=inputs["launch"], First=inputs["first"])
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_jump_start(b, name):
    ready = b.var("StartsDone")
    passed = b.both(ready, b.expr("throttle == 0 && held == 0 && gear == 0 && off >= 0 && off <= room", throttle=at(b, "HeldThrottle", 0),
                                  held=at(b, "HeldGear", 0), gear=at(b, "SignalGear", 0), off=at(b, "GridOff", 0), room=GRID_ROOM))
    text = b.fmt(f"full throttle asked through the countdown and ShiftUp sent {SHIFT_AT} steps into it: the vehicle applied at most "
                 f"{{Throttle}} of the throttle, the gear read at most {{Held}} and {{Gear}} at the signal, the bike {{Off}} m from the "
                 f"grid", Throttle=at(b, "HeldThrottle", 0), Held=at(b, "HeldGear", 0), Gear=at(b, "SignalGear", 0),
                 Off=at(b, "GridOff", 0))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_second_start(b, name):
    ready = b.var("StartsDone")
    passed = b.both(ready, b.expr(f"returns == 1 && steps >= {COUNTDOWN_STEPS} && steps <= {COUNTDOWN_STEPS + 1}",
                                  returns=b.var("ReturnsAfterAgain"), steps=at(b, "CountSteps", 0)))
    passed = b.both(passed, b.equal(b.var("AgainState"), b.lit("string", "Countdown")))
    text = b.fmt(f"StartRace sent again {AGAIN_AT} steps into the countdown left the director {{State}} with {{Returns}} returns, "
                 f"and the race started {{Steps}} steps after the first StartRace (the countdown's {COUNTDOWN_STEPS})",
                 State=b.var("AgainState"), Returns=b.var("ReturnsAfterAgain"), Steps=at(b, "CountSteps", 0))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_paused_countdown(b, name):
    ready = b.var("StartsDone")
    passed = b.both(ready, b.expr(f"frames >= {PAUSE_FRAMES} && moved >= 0 && moved <= 0.0001 && steps == 0 && count >= "
                                  f"{COUNTDOWN_STEPS} && count <= {COUNTDOWN_STEPS + 1}", frames=b.var("PausedFrames"),
                                  moved=b.var("PauseMoved"), steps=b.var("PauseStepsMoved"), count=at(b, "CountSteps", 1)))
    text = b.fmt(f"paused {PAUSE_AT} steps into the second countdown for {{Frames}} frames: the bike moved {{Moved}} m and the "
                 f"director counted {{Steps}} steps meanwhile; the race started {{Count}} steps after StartRace (the countdown's "
                 f"{COUNTDOWN_STEPS})", Frames=b.var("PausedFrames"), Moved=b.var("PauseMoved"), Steps=b.var("PauseStepsMoved"),
                 Count=at(b, "CountSteps", 1))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_quit_countdown(b, name):
    ready = b.var("StartsDone")
    returns = COUNT + 1
    passed = b.both(ready, b.expr(f"!held && returns == {returns} && gear >= 1 && speed > 1", held=b.var("QuitHeld"),
                                  returns=b.var("QuitReturns"), gear=b.var("QuitGear"), speed=b.var("QuitSpeed")))
    passed = b.both(passed, b.equal(b.var("QuitState"), b.lit("string", "Title")))
    text = b.fmt(f"AbandonRace {QUIT_AT} steps into a countdown left the director {{State}}, the bike held {{Held}}, {{Returns}} "
                 f"returns in all (the {returns} races'), and full throttle then rode it in gear {{Gear}} at {{Speed}} m/s",
                 State=b.var("QuitState"), Held=b.var("QuitHeld"), Returns=b.var("QuitReturns"), Gear=b.var("QuitGear"),
                 Speed=b.var("QuitSpeed"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_freed_hold(b, name):
    ready = b.var("StartsDone")
    passed = b.both(ready, b.expr("!held && throttle == 1", held=b.var("FreeHeld"), throttle=b.var("FreeThrottle")))
    passed = b.both(passed, b.equal(b.var("FreeState"), b.lit("string", "Countdown")))
    text = b.fmt(f"the director's Subject changed {FREE_AT} steps into a countdown: {FREE_STEPS} steps later the bike was held "
                 f"{{Held}} and the vehicle applied {{Throttle}} of the throttle its driver asks, the director {{State}}",
                 Held=b.var("FreeHeld"), Throttle=b.var("FreeThrottle"), State=b.var("FreeState"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


WRITERS = {
    "CheckJumpStart": check_jump_start,
    "CheckStartFromTitle": lambda b, name: check_start(b, name, 0),
    "CheckStartFromResults": lambda b, name: check_start(b, name, 1),
    "CheckStartFromFall": lambda b, name: check_start(b, name, 2),
    "CheckStartFromGravel": lambda b, name: check_start(b, name, 3),
    "CheckSecondStart": check_second_start,
    "CheckPausedCountdown": check_paused_countdown,
    "CheckQuitCountdown": check_quit_countdown,
    "CheckFreedHold": check_freed_hold,
}


def write_check(b, function, name):
    WRITERS[function](b, name)


SUITE = Suite(checks, write_check, ENTITIES, VARIABLES, HELPERS, steps=STEPS, frames=FRAMES)

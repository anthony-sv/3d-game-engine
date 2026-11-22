"""The bike at its limits, a module of Checks.MODULES that joins the ride test scene (Scenes/Ride.sscene) after the best lap's
(GhostChecks.py): its ride starts where that one ends (GhostPhase Done: the bike on the grid at rest in neutral, the riding
graph on its test driver with the assists off), and its checks run after theirs. It reads what a player meets at the
bike's limits: the engine in neutral once the throttle closes, the automatic shift at the shift point, and a full front
lever at speed, which the riding graph's yaw damper holds upright (Vehicle.py, Docs/Architecture.md section 15).

It is driven from the runner graph's Event.FixedUpdate by its own count of fixed steps and by what the bike reads, never by
a frame (the rule of the rides compared across frame rates), and logs every reading in one line ("Moto's limits: ..."),
the same at every frame rate. The phases (PHASES), one after another:

1. Wait: until GhostPhase is Done; the assists stay off, so the automatic shifts leave the bike in neutral.
2. Rev: on the grid in neutral, both brakes on, full throttle until the engine is at its red line (REV_STEPS at most).
3. Idle: the throttle closed, both brakes still on: the steps until the engine is at idle (IDLE_SHARE of the definition's
   MinRPM) and the most it reads for IDLE_WATCH steps after (it must not rise), IDLE_LIMIT steps at most.
4. Climb and Lever, twice (CASES): from the grid at rest, full throttle with the automatic shifts on up to the case's
   speed, then the throttle closed and the front lever full at once, until the bike is under LEVER_TO (LEVER_LIMIT steps at
   most): its largest lean meanwhile and the steps a wheel touched gravel or grass. The first case has the linked brakes
   and the lean assist on, the second both off, so that the damper is read on either steering. In the first climb each
   upshift out of first and second is read in the step the riding graph decided it, the step before the gear reads one
   higher (the shift takes effect at the next physics step): the engine's speed, the road's speed in the gear (as the
   riding graph computes it), and the steps the engine stood at its red line in that gear with the road's speed within
   Vehicle.SHIFT_SLIP_ROOM of the shift point; and in each gear from first to fifth the steps the engine stood at its red
   line whatever the road's speed (the launch's wheelspin holds it there).
   Settle, after each lever: back on the grid at rest, the automatic shifts off, both brakes on and shifted down into
   neutral until its wheels stop.
5. Stand: every reading logged; then Done, which no case of the switch runs, for modules that join the scene after this.

The checks (latent functions of the update) wait for the line to be logged (LimitDone) and report what it holds. Bands: Docs/Architecture.md section 15,
the feel pass, from the figures Vehicle.py states.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import BikeChecks  # noqa: E402  (the grid, the bike's helpers)
import Bike  # noqa: E402  (the rear wheel's radius)
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import RideChecks  # noqa: E402  (the assists' switches)
import Vehicle  # noqa: E402  (the ridden bike: its figures and its riding graph)
from Checks import Suite, run  # noqa: E402
from Graphs import private  # noqa: E402

BIKE = Vehicle.BIKE
STEP = BikeChecks.STEP

# --- The ride ------------------------------------------------------------------------------------------------------------

# The rev: full throttle in neutral until the engine reads its red line, at most REV_STEPS steps.
REV_STEPS = 60
# Idle: within IDLE_SHARE of MinRPM; read for IDLE_WATCH steps once there; at most IDLE_LIMIT steps from the throttle's close.
IDLE_SHARE = 1.01
IDLE_WATCH = 60
IDLE_LIMIT = 180
# The full lever: the speeds it goes on at (m/s), with the linked brakes and the lean assist on, then both off; the speed
# it brakes down to and the most steps it may take (a full lever slows the bike by 8 to 10 m/s^2).
CASES = ((62.0, True, True), (64.0, False, False))
LEVER_TO = 30.0
LEVER_LIMIT = 360
# The gears whose upshift the first climb reads.
SHIFT_GEARS = (1, 2)
# The phases in the order they ride: (name, the function its case calls).
PHASES = (
    ("Wait", "LimitWait"),
    ("Rev", "LimitRev"),
    ("Idle", "LimitIdle"),
    ("Climb1", "LimitClimb1"),
    ("Lever1", "LimitLever1"),
    ("Settle1", "LimitSettle1"),
    ("Climb2", "LimitClimb2"),
    ("Lever2", "LimitLever2"),
    ("Settle2", "LimitSettle2"),
    ("Stand", "LimitStand"),
)

# --- The bands -------------------------------------------------------------------------------------------------------------

# The engine at idle this many steps after the throttle closes at most either way of Vehicle.idle_steps(): the throttle's
# last open step may run before or after the riding graph's in the step it closes (the scene's order of its graphs), and
# the rpm the red line leaves stands within a step's fall of it.
IDLE_ROOM = 2
# The steps a gear stands at the red line with the road's speed within SHIFT_SLIP_ROOM of the shift point before it shifts
# up: the step that decides, when the rear tyre's drive slip has the engine at its limiter there (it does in first and
# second at full throttle); by the road's speed alone it stood there 23 and 14 steps.
LIMITER_STEPS = 1
# The steps the engine stands at its red line in gears 1 to 5 over the climb, whatever the road's speed: the rear tyre's
# wheelspin from the grid holds first and second there, which no shift rule frees without lugging the gear above under
# ShiftDownRPM (Vehicle.py, the shifts). Read 171 and 86 steps in first and second and 9 in third and fourth (the shifts'
# open clutch), 266 in all; DWELL_LIMIT is a tenth above that, which the shift by the road's speed alone passes (296).
DWELL_GEARS = 5
DWELL_LIMIT = 293
# The full lever: the largest lean while it brakes (degrees: the damper holds it within 0.3; without it the first case
# leaned 29.5 degrees and left the road, the second 50), and no step with a wheel off the road.
LEVER_LEAN = 2.0
# How long a check waits for its reading (seconds of the game, from when it starts); the scene's timeout stays above it.
WAIT = 100


def ride_timeout():
    return BikeChecks.ride_timeout()


def bands():
    idle = Vehicle.idle_steps()
    return {"idle": (idle - IDLE_ROOM, idle + IDLE_ROOM, idle)}


# --- The runner graph's interface -----------------------------------------------------------------------------------------

VARIABLES = (
    private("LimitPhase", "string", "Wait"), private("LimitStep", "int", 0), private("LimitPhaseStep", "int", 0),
    private("RevTop", "float", -1.0), private("IdleSteps", "int", -1), private("IdleMost", "float", -1.0),
    private("LastGear", "int", 0), private("LastEngine", "float", 0.0), private("LastRoad", "float", 0.0),
    private("ShiftEngine", "float[]", [-1.0, -1.0]), private("ShiftRoad", "float[]", [-1.0, -1.0]),
    private("LimiterSteps", "int[]", [0, 0]), private("DwellSteps", "int[]", [0] * DWELL_GEARS),
    private("LeverFrom", "float[]", [-1.0, -1.0]), private("LeverLean", "float[]", [0.0, 0.0]),
    private("LeverRough", "int[]", [0, 0]), private("LeverEnd", "float[]", [-1.0, -1.0]),
    private("LimitDone", "bool", False),
)


def bike(b):
    return b.var("Bike")


def elapsed(b):
    return b.once("limit elapsed", lambda: b.expr("step - start", step=b.var("LimitStep"), start=b.var("LimitPhaseStep")))


def first_step(b):
    return b.branch(b.expr("elapsed == 0", elapsed=elapsed(b)), comment="The phase's first step")


def enter(b, phase):
    return b.own("LimitEnter", Next=phase)


def drive(b, throttle=0.0, front=0.0, rear=0.0):
    return b.own("Drive", Throttle=throttle, FrontBrake=front, RearBrake=rear, Lean=0.0)


def definition(b):
    return b.once("limit definition", lambda: b.pure("Get:Strada.VehicleComponent.Definition", "Value", Target=bike(b)))


def engine(b):
    return b.once("limit engine", lambda: b.pure("Get:Strada.VehicleComponent.EngineSpeed", "Value", Target=bike(b)))


def red_line(b):
    return b.once("limit red line", lambda: b.pure("Get:Strada.Vehicle.MaxRPM", "Value", Target=definition(b)))


def put(b, array, index, value):
    return b.set(array, b.pure("Array.Set", Array=b.var(array), Index=index, Value=value))


def at(b, array, index):
    return b.add("Array.Get", None, "Element", Array=b.var(array), Index=index).out


# --- The helpers and the phases ----------------------------------------------------------------------------------------------

def write_enter(b):
    begins = b.set("LimitPhaseStep", b.expr("step + 1", step=b.var("LimitStep")), comment="Its first step is the next")
    b.chain(b.entry, begins, b.set("LimitPhase", b.entry["Next"]), b.ret)


def write_wait(b):
    done = b.branch(b.equal(b.var("GhostPhase"), b.lit("string", "Done")), comment="The best lap's ride is done: on the grid")
    b.chain(b.entry, done)
    b.chain(done["True"], enter(b, "Rev"), b.ret)
    b.flow(done["False"], b.ret)


def write_rev(b):
    held = drive(b, throttle=1.0, front=1.0, rear=1.0)
    done = b.branch(b.expr(f"engine >= most - 1 || elapsed >= {REV_STEPS}", engine=engine(b), most=red_line(b), elapsed=elapsed(b)),
                    comment="At the red line, in neutral")
    top = b.set("RevTop", engine(b))
    b.chain(b.entry, held, done)
    b.chain(done["True"], top, enter(b, "Idle"), b.ret)
    b.flow(done["False"], b.ret)


def write_idle(b):
    held = drive(b, front=1.0, rear=1.0)
    idle = b.expr(f"engine <= {IDLE_SHARE} * least", engine=engine(b),
                  least=b.pure("Get:Strada.Vehicle.MinRPM", "Value", Target=definition(b)))
    reached = b.branch(b.expr("steps < 0 && idle", steps=b.var("IdleSteps"), idle=idle), comment="At idle for the first time")
    noted = b.set("IdleSteps", elapsed(b), comment="Steps since the throttle closed")
    watching = b.branch(b.expr("steps >= 0", steps=b.var("IdleSteps")), comment="Since idle: the most the engine reads")
    most = b.set("IdleMost", b.expr("max(most, engine)", most=b.var("IdleMost"), engine=engine(b)))
    done = b.branch(b.expr(f"(steps >= 0 && elapsed >= steps + {IDLE_WATCH}) || elapsed >= {IDLE_LIMIT}", steps=b.var("IdleSteps"),
                           elapsed=elapsed(b)))
    b.chain(b.entry, held, reached)
    b.chain(reached["True"], noted, watching)
    b.flow(reached["False"], watching)
    b.chain(watching["True"], most, done)
    b.flow(watching["False"], done)
    b.chain(done["True"], enter(b, "Climb1"), b.ret)
    b.flow(done["False"], b.ret)


def road_rpm(b):
    """The engine's speed at the road's speed in the gear, as the riding graph computes it (rpm)."""
    def make():
        ratio = b.pure("Call:Strada.Vehicle.GetGearRatio(int)", Target=definition(b), Gear=BikeChecks.gear(b))
        final = b.pure("Get:Strada.Vehicle.FinalDrive", "Value", Target=definition(b))
        return b.expr(f"max(speed, 0) / {round(Bike.REAR_RADIUS, 6)} * ratio * final * {round(30.0 / 3.141592653589793, 6)}",
                      speed=BikeChecks.speed(b), ratio=ratio, final=final)
    return b.once("limit road", make)


def write_climb(b, case):
    """Full throttle from the grid on the automatic shifts up to the case's speed; in the first, the upshifts out of first
    and second read in their decision's step (the last step's readings when the gear reads one higher)."""
    speed_to, linked, assisted = CASES[case]
    start = first_step(b)
    switches = [RideChecks.assist(b, "LeanAssist", assisted), RideChecks.assist(b, "LinkedBrakes", linked),
                RideChecks.assist(b, "AutoShift", True)]
    held = drive(b, throttle=1.0)
    fast = b.branch(b.expr(f"speed >= {speed_to}", speed=BikeChecks.speed(b)))
    enter_lever = enter(b, f"Lever{case + 1}")
    b.chain(b.entry, start)
    b.chain(start["True"], *switches, held)
    b.flow(start["False"], held)
    if case == 0:
        gear = BikeChecks.gear(b)
        shifted = b.branch(b.expr(f"gear == last + 1 && last >= {SHIFT_GEARS[0]} && last <= {SHIFT_GEARS[-1]}", gear=gear,
                                  last=b.var("LastGear")), comment="Shifted up out of first or second: read its decision's step")
        index = b.expr(f"last - {SHIFT_GEARS[0]}", last=b.var("LastGear"))
        decided = [put(b, "ShiftEngine", index, b.var("LastEngine")), put(b, "ShiftRoad", index, b.var("LastRoad"))]
        limited = b.branch(b.expr(f"gear >= {SHIFT_GEARS[0]} && gear <= {SHIFT_GEARS[-1]} && engine >= most - 1 && road >= "
                                  f"min(up, most) - {Vehicle.SHIFT_SLIP_ROOM}", gear=gear, engine=engine(b), most=red_line(b),
                                  road=road_rpm(b), up=b.pure("Get:Strada.Vehicle.ShiftUpRPM", "Value", Target=definition(b))),
                           comment="At the red line with the road's speed at the shift point")
        slot = b.expr(f"gear - {SHIFT_GEARS[0]}", gear=gear)
        counted = put(b, "LimiterSteps", slot, b.expr("count + 1", count=at(b, "LimiterSteps", slot)))
        dwelling = b.branch(b.expr(f"gear >= 1 && gear <= {DWELL_GEARS} && engine >= most - 1", gear=gear, engine=engine(b),
                                   most=red_line(b)), comment="At the red line in a gear, whatever the road's speed")
        place = b.expr("gear - 1", gear=gear)
        dwelt = put(b, "DwellSteps", place, b.expr("count + 1", count=at(b, "DwellSteps", place)))
        kept = [b.set("LastGear", gear), b.set("LastEngine", engine(b)), b.set("LastRoad", road_rpm(b))]
        b.chain(held, shifted)
        b.chain(shifted["True"], *decided, dwelling)
        b.flow(shifted["False"], dwelling)
        b.chain(dwelling["True"], dwelt, limited)
        b.flow(dwelling["False"], limited)
        b.chain(limited["True"], counted, kept[0])
        b.flow(limited["False"], kept[0])
        b.chain(kept[0], *kept[1:], fast)
    else:
        b.chain(held, fast)
    b.chain(fast["True"], put(b, "LeverFrom", case, BikeChecks.speed(b)), enter_lever, b.ret)
    b.flow(fast["False"], b.ret)


def write_lever(b, case):
    """The throttle closed and the front lever full at once, until the bike is under LEVER_TO: its largest lean and the
    steps a wheel touched gravel or grass."""
    held = drive(b, front=1.0)
    wheels = []
    for index in (0, 1):
        state = b.pure("Call:Strada.VehicleComponent.GetWheelState(int)", Target=bike(b), Index=index)
        surface = b.pure("Get:Strada.VehicleWheelState.Surface", "Value", Target=state)
        wheels.append(b.both(b.pure("Get:Strada.VehicleWheelState.HasContact", "Value", Target=state),
                             b.expr("gravel || grass", gravel=b.equal(surface, b.lit("string", "Gravel")),
                                    grass=b.equal(surface, b.lit("string", "Grass")))))
    leaned = put(b, "LeverLean", case, b.expr("max(most, abs(lean))", most=at(b, "LeverLean", case), lean=BikeChecks.lean(b)))
    rough = put(b, "LeverRough", case, b.expr("count + (front || rear ? 1 : 0)", count=at(b, "LeverRough", case), front=wheels[0],
                                               rear=wheels[1]))
    done = b.branch(b.expr(f"speed < {LEVER_TO} || elapsed >= {LEVER_LIMIT}", speed=BikeChecks.speed(b), elapsed=elapsed(b)))
    end = put(b, "LeverEnd", case, BikeChecks.speed(b))
    b.chain(b.entry, held, leaned, rough, done)
    b.chain(done["True"], end, enter(b, f"Settle{case + 1}"), b.ret)
    b.flow(done["False"], b.ret)


def write_settle(b, case):
    """Back on the grid at rest, the automatic shifts off, braked into neutral until its wheels stop."""
    following = "Climb2" if case == 0 else "Stand"
    start = first_step(b)
    at_grid, yaw, along = BikeChecks.grid_spot()
    place = b.own("Place", At=at_grid, Yaw=yaw, Along=along, Speed=0.0, comment="Back onto the grid, at rest")
    off = RideChecks.assist(b, "AutoShift", False)
    held = drive(b, front=1.0, rear=1.0)
    down = b.branch(b.expr(f"elapsed % {BikeChecks.SHIFT_DOWN_EVERY} == 0 && gear > 0", elapsed=elapsed(b), gear=BikeChecks.gear(b)),
                    comment="One gear down at a time, until neutral")
    shift = b.send("ShiftDown", bike(b), graph=BIKE)
    done = b.branch(b.expr(f"elapsed >= {BikeChecks.STAND_STEPS} && speed < {BikeChecks.STOPPED} && gear == 0", elapsed=elapsed(b),
                           speed=BikeChecks.speed(b), gear=BikeChecks.gear(b)), comment="Its wheels have stopped turning too, in neutral")
    b.chain(b.entry, start)
    b.chain(start["True"], place, off, held)
    b.flow(start["False"], held)
    b.chain(held, down)
    b.flow(down["True"], shift)
    b.chain(shift, done)
    b.flow(down["False"], done)
    b.chain(done["True"], drive(b), enter(b, following), b.ret)
    b.flow(done["False"], b.ret)


def write_stand(b):
    readings = b.fmt(
        "Moto's limits: in neutral the engine closed from {Top} rpm was at idle after {Idle} steps and read at most {Most} rpm "
        "after; the automatic shift left first at {Engine1} rpm, the road's speed {Road1} rpm, and second at {Engine2} rpm, the "
        "road's {Road2} rpm, after {Limiter1} and {Limiter2} steps at the red line, and stood at the red line {Dwell} steps in "
        "gears 1 to 5 whatever the road's speed; a full front lever from {From1} m/s with the "
        "linked brakes and the lean assist on leaned it {Lean1} degrees at most, {Rough1} steps off the road, down to {End1} m/s, "
        "and from {From2} m/s with both off {Lean2} degrees, {Rough2} steps off the road, down to {End2} m/s; done at step {Step}",
        Top=b.var("RevTop"), Idle=b.var("IdleSteps"), Most=b.var("IdleMost"), Engine1=at(b, "ShiftEngine", 0),
        Road1=at(b, "ShiftRoad", 0), Engine2=at(b, "ShiftEngine", 1), Road2=at(b, "ShiftRoad", 1), Limiter1=at(b, "LimiterSteps", 0),
        Limiter2=at(b, "LimiterSteps", 1), Dwell=b.var("DwellSteps"), From1=at(b, "LeverFrom", 0), Lean1=at(b, "LeverLean", 0),
        Rough1=at(b, "LeverRough", 0),
        End1=at(b, "LeverEnd", 0), From2=at(b, "LeverFrom", 1), Lean2=at(b, "LeverLean", 1), Rough2=at(b, "LeverRough", 1),
        End2=at(b, "LeverEnd", 1), Step=b.var("LimitStep"))
    log = b.call("Call:Strada.Log.Info(string)", Message=readings, comment="Every reading, the same at every frame rate")
    b.chain(b.entry, log, b.set("LimitDone", True), enter(b, "Done"), b.ret)


HELPERS = (
    (("LimitEnter", False, (("Next", "string"),), (), ()), "Limit enter: the next phase, which begins at the next step",
     write_enter),
    (("LimitWait", False, (), (), ()), "Wait: the best lap's ride done, the bike on the grid", write_wait),
    (("LimitRev", False, (), (), ()), "Rev: full throttle in neutral to the red line", write_rev),
    (("LimitIdle", False, (), (), ()), "Idle: the throttle closed, the engine to idle", write_idle),
    (("LimitClimb1", False, (), (), ()), "Climb 1: up to 62 m/s, the upshifts read", lambda b: write_climb(b, 0)),
    (("LimitLever1", False, (), (), ()), "Lever 1: the full front lever, the assists on", lambda b: write_lever(b, 0)),
    (("LimitSettle1", False, (), (), ()), "Settle 1: on the grid, braked into neutral", lambda b: write_settle(b, 0)),
    (("LimitClimb2", False, (), (), ()), "Climb 2: up to 64 m/s", lambda b: write_climb(b, 1)),
    (("LimitLever2", False, (), (), ()), "Lever 2: the full front lever, the assists off", lambda b: write_lever(b, 1)),
    (("LimitSettle2", False, (), (), ()), "Settle 2: on the grid, braked into neutral", lambda b: write_settle(b, 1)),
    (("LimitStand", False, (), (), ()), "Stand: every reading logged", write_stand),
)


def write_ride(b):
    """The ride: each fixed step after the best lap's (the runner graph's Event.FixedUpdate calls it), the phase's
    function, then the step's count."""
    steps = b.sequence(2)
    phases = b.add("Flow.Switch", {"Type": "string", "Cases": [name for name, _ in PHASES]}, Selection=b.var("LimitPhase"))
    b.chain(b.entry, steps)
    b.flow(steps["Then0"], phases)
    for name, function in PHASES:
        b.flow(phases[f"Case:{name}"], b.own(function))
    b.chain(steps["Then1"], b.set("LimitStep", b.expr("step + 1", step=b.var("LimitStep"))), b.ret)


STEPS = (("LimitRide", "Limits: each fixed step, the phase's driving", write_ride),)

# --- The checks ------------------------------------------------------------------------------------------------------------


def checks():
    if WAIT >= ride_timeout():
        raise ValueError(f"a limit check waits {WAIT} s, not less than the ride test scene's timeout of {ride_timeout()} s")
    return (
        ("CheckNeutralIdle", "Neutral: the engine at idle a second after the throttle closes",
         "in neutral the engine falls from its red line to idle about a second after the throttle closes, and stays there"),
        ("CheckShiftPoint", "Shift point: first and second left at the shift point",
         "the automatic shift leaves first and second gear at the shift point, never held at the red line once the road's speed "
         "is there, and the launch's wheelspin holds the red line no longer than its bound"),
        ("CheckLeverLinked", "Full lever: from 62 m/s, the assists on, upright",
         "a full front lever from 62 m/s with the linked brakes and the lean assist on keeps the bike upright on the road"),
        ("CheckLeverRaw", "Full lever: from 64 m/s, the assists off, upright",
         "a full front lever from 64 m/s with the linked brakes and the lean assist off keeps the bike upright on the road"),
    )


def check_neutral_idle(b, name):
    low, high, figure = bands()["idle"]
    least = round(IDLE_SHARE * Vehicle.RacingLine.IDLE, 1)
    ready = b.var("LimitDone")
    passed = b.expr(f"ready && top >= {Vehicle.RacingLine.RED_LINE - 1} && steps >= {low} && steps <= {high} && most <= {least}",
                    ready=ready, top=b.var("RevTop"), steps=b.var("IdleSteps"), most=b.var("IdleMost"))
    text = b.fmt(f"closed from {{Top}} rpm in neutral, the engine was at idle (at most {least} rpm) after {{Steps}} steps (the "
                 f"definition's figure {figure}, the band {low} to {high}) and read at most {{Most}} rpm for {IDLE_WATCH} steps after",
                 Top=b.var("RevTop"), Steps=b.var("IdleSteps"), Most=b.var("IdleMost"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_shift_point(b, name):
    up, most = Vehicle.SHIFT_UP, Vehicle.RacingLine.RED_LINE
    room = Vehicle.SHIFT_SLIP_ROOM
    ready = b.var("LimitDone")
    parts = []
    inputs = {}
    for index in range(len(SHIFT_GEARS)):
        inputs[f"engine{index}"] = at(b, "ShiftEngine", index)
        inputs[f"road{index}"] = at(b, "ShiftRoad", index)
        inputs[f"limiter{index}"] = at(b, "LimiterSteps", index)
        parts.append(f"engine{index} >= {up} && road{index} >= {up - room} && limiter{index} <= {LIMITER_STEPS}")
    dwell = b.expr(" + ".join(f"d{gear}" for gear in range(DWELL_GEARS)),
                   **{f"d{gear}": at(b, "DwellSteps", gear) for gear in range(DWELL_GEARS)})
    inputs["dwell"] = dwell
    parts.append(f"dwell <= {DWELL_LIMIT}")
    passed = b.both(ready, b.expr(" && ".join(parts), **inputs))
    text = b.fmt(f"full throttle from the grid shifted out of first at {{E1}} rpm, the road's speed {{R1}} rpm, and out of second "
                 f"at {{E2}} rpm, the road's {{R2}} rpm (the shift point {up} rpm, the red line {most}, the road within {room} of the "
                 f"point), {{L1}} and {{L2}} steps at the red line with the road's speed there (at most {LIMITER_STEPS}: the step "
                 f"that decides); {{D}} steps at the red line in gears 1 to 5 whatever the road's speed (at most {DWELL_LIMIT}: "
                 f"the launch's wheelspin)", D=dwell, E1=at(b, "ShiftEngine", 0),
                 R1=at(b, "ShiftRoad", 0), E2=at(b, "ShiftEngine", 1), R2=at(b, "ShiftRoad", 1), L1=at(b, "LimiterSteps", 0),
                 L2=at(b, "LimiterSteps", 1))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_lever(b, name, case):
    speed_to, linked, assisted = CASES[case]
    ready = b.var("LimitDone")
    passed = b.both(ready, b.expr(f"lean < {LEVER_LEAN} && rough == 0 && end < {LEVER_TO} && from >= {speed_to}",
                                  lean=at(b, "LeverLean", case), rough=at(b, "LeverRough", case), end=at(b, "LeverEnd", case),
                                  **{"from": at(b, "LeverFrom", case)}))
    assists = "on" if linked and assisted else "off"
    text = b.fmt(f"a full front lever from {{From}} m/s with the linked brakes and the lean assist {assists} leaned the bike "
                 f"{{Lean}} degrees at most (under {LEVER_LEAN}), {{Rough}} steps with a wheel on gravel or grass (none), down to "
                 f"{{End}} m/s (under {LEVER_TO} within {LEVER_LIMIT} steps)", From=at(b, "LeverFrom", case),
                 Lean=at(b, "LeverLean", case), Rough=at(b, "LeverRough", case), End=at(b, "LeverEnd", case))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


WRITERS = {
    "CheckNeutralIdle": check_neutral_idle,
    "CheckShiftPoint": check_shift_point,
    "CheckLeverLinked": lambda b, name: check_lever(b, name, 0),
    "CheckLeverRaw": lambda b, name: check_lever(b, name, 1),
}


def write_check(b, function, name):
    WRITERS[function](b, name)


SUITE = Suite(checks, write_check, (), VARIABLES, HELPERS, steps=STEPS)

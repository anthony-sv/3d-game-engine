"""The rider assists' and the autopilot's checks, a module of Checks.MODULES that joins the ride test scene (Scenes/Ride.sscene)
after the ridden bike's (BikeChecks.py): its ride starts where that one ends, with the bike on the grid at rest in neutral and
the riding graph on its test driver (Vehicle.TEST), and its checks run after that module's seven.

The scene starts with the assists off (prepare), so that the bike's own ride is the raw vehicle's; this ride turns them on.
It is driven from the runner graph's Event.FixedUpdate by its own count of fixed steps and by what the bike reads, never by
a frame or a time (the rule of the rides compared across frame rates): it writes the riding graph's public values through
BikeChecks' Drive, places the bike through its Place, sends the riding graph its shifts and the Ride graph StartAutopilot
and StopAutopilot, and takes every reading in the step it belongs to. The phases (PHASES), one after another:

1. Wait: until the bike's ride is done (its phase Rest); then the three assists on.
2. Launch: full throttle and nothing else from neutral: the automatic shifts take it into first (its gear AUTO_GEAR_STEPS
   later is the check's) and up, to BRAKE_FROM.
3. Neutral: the automatic shifts off, the throttle closed and shifted down into neutral, coasting down to BRAKE_FROM.
4. Linked: the front lever alone with the linked brakes on, until the bike stops: how far it went (against the front brake
   alone of BikeChecks' ride, which the assists did not touch), and the rear brake the vehicle applied at LINKED_READ.
5. Speed Up: placed on the main straight (BikeChecks.resume_spot), the automatic shifts on, full throttle up to LEAN_FROM.
6. Lean: the throttle closed and a lean request of ASSIST_REQUEST for LEAN_STEPS: how far the heading turned over the last
   LEAN_STEPS - TURN_FROM steps (the assist steers the turn that lean balances), the lean and the speed then.
7. Autopilot A: placed on the grid at rest, the Ride graph sent StartAutopilot: SEGMENT_A steps from the grid through Turn 1
   and Turn 2; Autopilot B: SEGMENT_B steps on, through the corners after. Each segment's lap distance covered (the Ride
   graph's Distance), the farthest the bike rode from the racing line (its Offset), the steps a wheel touched gravel or
   grass, and the deepest lean.
8. Done: StopAutopilot; every reading logged in one line ("Moto's assists and autopilot: ..."), the same at every frame rate.

The checks (latent functions of the update) wait for a reading and report it; the pose's two read the rider and the fork as
the riding graph posed them in a frame of the autopilot's ride. Bands: Docs/Architecture.md section 15, the ride in graphs.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import BikeChecks  # noqa: E402  (the bike's ride, whose end this one starts from, and its helpers)
import Bike  # noqa: E402  (the rider's hips)
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import Circuit  # noqa: E402  (the lap's length)
import RacingLine  # noqa: E402  (the line's lean limit)
import Ride  # noqa: E402  (the Ride graph and the autopilot's figures)
import Vehicle  # noqa: E402  (the ridden bike: its figures, its assists and its riding graph)
from Checks import Suite, run  # noqa: E402
from Graphs import private  # noqa: E402

VECTOR = "Strada.Vector3"
BIKE = Vehicle.BIKE
RIDE = Ride.RIDE
STEP = BikeChecks.STEP

# --- The ride ------------------------------------------------------------------------------------------------------------

AUTO_GEAR_STEPS = 20
BRAKE_FROM = BikeChecks.BRAKE_FROM
SHIFT_DOWN_EVERY = BikeChecks.SHIFT_DOWN_EVERY
LINKED_READ = 3
LEAN_FROM = 20.0
ASSIST_REQUEST = -0.4
LEAN_STEPS = 90
TURN_FROM = 60
SEGMENT_A = 1200
SEGMENT_B = 900
STOPPED = BikeChecks.STOPPED

# The phases in the order they ride: (name, the function its case calls).
PHASES = (
    ("Wait", "AssistWait"),
    ("Launch", "AssistLaunch"),
    ("Neutral", "AssistNeutral"),
    ("Linked", "AssistLinked"),
    ("SpeedUp", "AssistSpeedUp"),
    ("Lean", "AssistLean"),
    ("AutopilotA", "AutopilotA"),
    ("AutopilotB", "AutopilotB"),
)

# --- The bands -------------------------------------------------------------------------------------------------------------

# The linked stop's room (shares of Vehicle.py's form, as BikeChecks' stops: the springs' pitch moves the weight to the front
# over the first tenths of a second); the rear brake the vehicle applies at LINKED_READ: the share, eased by the lean of a
# bike braking upright (at most 3 degrees: an ease of 0.9).
LINKED_ROOM = BikeChecks.STOP_ROOM
REAR_SHARE_BAND = (round(Vehicle.LINKED_SHARE * Vehicle.brake_ease(3.0), 3), Vehicle.LINKED_SHARE)
# The turn the assist rides for a lean request: the heading's rate over the request's last half second against the rate at
# which the lean asked for balances the turn at the speed (g tan(lean) / v), as a share of it. Jolt's motorcycle turns by its
# steering's geometry less its tyres' slip (a probe read 0.78 of the rate) and leans less than a rider balances, so the
# turn is what the mapping is checked by, and the lean is read and logged.
TURN_ROOM = (0.6, 1.25)
# The autopilot's segments: the lap distance covered against Ride.predict's point mass, the farthest from the line (meters),
# no step with a wheel on gravel or grass, and no lean past the line's limit.
COVERED_ROOM = (0.85, 1.15)
MOST_OFFSET = 3.0
MOST_LEAN = RacingLine.MAX_LEAN
# The pose: how far the rider may stand from where the riding graph poses him for the bike's drawn lean and speed (meters;
# the check may read a frame after the pose was set), and the fork's axle from the front wheel's (degrees).
POSE_TOLERANCE = 0.01
FORK_TOLERANCE = 0.5
HANG_LEAN = 5.0
TUCK_SPEED = Vehicle.TUCK_FULL + 2.0
# How long a check waits for its reading (seconds of the game, from when it starts): the bike's ride and this one take about
# 80 s, and the ride test scene's timeout (Checks.SCENES) must stay above the wait.
WAIT = 100


def bands():
    figures = Vehicle.ride_figures()
    linked = figures["linked_stop_20"]
    lean = ASSIST_REQUEST * Vehicle.MAX_LEAN
    covered_a = Ride.predict(SEGMENT_A * STEP)
    covered_b = Ride.predict((SEGMENT_A + SEGMENT_B) * STEP) - covered_a
    return {
        "linked": (round(LINKED_ROOM[0] * linked, 2), round(LINKED_ROOM[1] * linked, 2), round(linked, 2)),
        "turn": (TURN_ROOM[0], TURN_ROOM[1], round(lean, 2)),
        "a": (round(COVERED_ROOM[0] * covered_a, 1), round(COVERED_ROOM[1] * covered_a, 1), round(covered_a, 1)),
        "b": (round(COVERED_ROOM[0] * covered_b, 1), round(COVERED_ROOM[1] * covered_b, 1), round(covered_b, 1)),
    }


# --- The runner graph's interface -----------------------------------------------------------------------------------------

ENTITIES = (("Ride", Ride.ROOT, "The Ride entity, whose graph keeps the bike's place on the lap and rides the autopilot"),)
VARIABLES = (
    private("AssistPhase", "string", "Wait"), private("AssistStep", "int", 0), private("AssistPhaseStep", "int", 0),
    private("AutoGear", "int", -1), private("LinkedFrom", VECTOR), private("LinkedStop", "float", -1.0),
    private("LinkedRear", "float", -1.0), private("LeanReading", "float", 0.0), private("LeanSpeed", "float", -1.0),
    private("LeanHeading", VECTOR), private("LeanTurn", "float", 0.0),
    private("SegmentStart", "float", 0.0), private("CoveredA", "float", -1.0), private("CoveredB", "float", -1.0),
    private("OffsetA", "float", 0.0), private("OffsetB", "float", 0.0), private("OffRoadA", "int", 0), private("OffRoadB", "int", 0),
    private("DeepestA", "float", 0.0), private("DeepestB", "float", 0.0), private("AutopilotDone", "bool", False),
)


def bike(b):
    return b.var("Bike")


def speed(b):
    return BikeChecks.speed(b)


def gear(b):
    return BikeChecks.gear(b)


def lean(b):
    return BikeChecks.lean(b)


def elapsed(b):
    return b.once("assist elapsed", lambda: b.expr("step - start", step=b.var("AssistStep"), start=b.var("AssistPhaseStep")))


def first_step(b):
    return b.branch(b.expr("elapsed == 0", elapsed=elapsed(b)), comment="The phase's first step")


def drive(b, throttle=0.0, front=0.0, rear=0.0, lean_input=0.0):
    return b.own("Drive", Throttle=throttle, FrontBrake=front, RearBrake=rear, Lean=lean_input)


def assist(b, name, value):
    """Sets one of the riding graph's assists."""
    return b.set(name, value, graph=BIKE, target=bike(b))


def enter(b, phase):
    return b.own("AssistEnter", Next=phase)


def ride_distance(b):
    return b.once("ride distance", lambda: b.get("Distance", RIDE, b.var("Ride")))


# --- The helpers and the phases ----------------------------------------------------------------------------------------------

def write_enter(b):
    begins = b.set("AssistPhaseStep", b.expr("step + 1", step=b.var("AssistStep")), comment="Its first step is the next")
    b.chain(b.entry, begins, b.set("AssistPhase", b.entry["Next"]), b.ret)


def write_wait(b):
    done = b.branch(b.equal(b.var("Phase"), b.lit("string", "Rest")), comment="The bike's ride is done: on the grid at rest")
    on = [assist(b, name, True) for name in ("LeanAssist", "LinkedBrakes", "AutoShift")]
    b.chain(b.entry, done)
    b.chain(done["True"], *on, enter(b, "Launch"), b.ret)
    b.flow(done["False"], b.ret)


def write_launch(b):
    held = drive(b, throttle=1.0)
    read = b.branch(b.expr(f"elapsed == {AUTO_GEAR_STEPS}", elapsed=elapsed(b)), comment="The gear the throttle alone took")
    noted = b.set("AutoGear", gear(b))
    fast = b.branch(b.expr(f"elapsed > {AUTO_GEAR_STEPS} && speed >= {BRAKE_FROM}", elapsed=elapsed(b), speed=speed(b)))
    b.chain(b.entry, held, read)
    b.chain(read["True"], noted, fast)
    b.flow(read["False"], fast)
    b.chain(fast["True"], assist(b, "AutoShift", False), enter(b, "Neutral"), b.ret)
    b.flow(fast["False"], b.ret)


def write_neutral(b):
    held = drive(b)
    down = b.branch(b.expr(f"elapsed % {SHIFT_DOWN_EVERY} == 0 && gear > 0", elapsed=elapsed(b), gear=gear(b)),
                    comment="One gear down at a time, until neutral")
    shift = b.send("ShiftDown", bike(b), graph=BIKE)
    done = b.branch(b.expr(f"elapsed >= {SHIFT_DOWN_EVERY} && gear == 0 && speed <= {BRAKE_FROM}", elapsed=elapsed(b), gear=gear(b),
                           speed=speed(b)), comment="In neutral and slow enough")
    b.chain(b.entry, held, down)
    b.flow(down["True"], shift)
    b.chain(shift, done)
    b.flow(down["False"], done)
    b.chain(done["True"], enter(b, "Linked"), b.ret)
    b.flow(done["False"], b.ret)


def write_linked(b):
    start = first_step(b)
    mark = b.set("LinkedFrom", BikeChecks.position(b), comment="Where the front lever goes on")
    held = drive(b, front=1.0)
    read = b.branch(b.expr(f"elapsed == {LINKED_READ}", elapsed=elapsed(b)), comment="The rear brake the vehicle applied")
    rear = b.set("LinkedRear", b.pure("Get:Strada.VehicleComponent.RearBrake", "Value", Target=bike(b)))
    stopped = b.branch(b.expr(f"speed < {STOPPED}", speed=speed(b)))
    distance = b.set("LinkedStop", b.expr("distance(vec2(at.X, at.Z), vec2(start.X, start.Z))", at=BikeChecks.position(b),
                                          start=b.var("LinkedFrom")))
    at, yaw, along = BikeChecks.resume_spot()
    place = b.own("Place", At=at, Yaw=yaw, Along=along, Speed=0.0, comment="Onto the main straight, at rest")
    b.chain(b.entry, start)
    b.flow(start["True"], mark)
    b.chain(mark, held)
    b.flow(start["False"], held)
    b.chain(held, read)
    b.chain(read["True"], rear, stopped)
    b.flow(read["False"], stopped)
    b.chain(stopped["True"], distance, place, assist(b, "AutoShift", True), enter(b, "SpeedUp"), b.ret)
    b.flow(stopped["False"], b.ret)


def write_speed_up(b):
    held = drive(b, throttle=1.0)
    fast = b.branch(b.expr(f"speed >= {LEAN_FROM}", speed=speed(b)))
    b.chain(b.entry, held, fast)
    b.chain(fast["True"], enter(b, "Lean"), b.ret)
    b.flow(fast["False"], b.ret)


def write_lean(b):
    held = drive(b, lean_input=ASSIST_REQUEST)
    turning = b.branch(b.expr(f"elapsed == {TURN_FROM}", elapsed=elapsed(b)), comment="The turn's last half second starts")
    mark = b.set("LeanHeading", BikeChecks.forward(b))
    done = b.branch(b.expr(f"elapsed >= {LEAN_STEPS}", elapsed=elapsed(b)), comment="The lean held for 1.5 s")
    turned = b.set("LeanTurn", b.expr("degrees(atan2(a.Z * c.X - a.X * c.Z, a.X * c.X + a.Z * c.Z))", a=b.var("LeanHeading"),
                                       c=BikeChecks.forward(b)), comment="Degrees, positive to the left")
    reached = b.set("LeanReading", lean(b))
    when = b.set("LeanSpeed", speed(b))
    at, yaw, along = BikeChecks.grid_spot()
    place = b.own("Place", At=at, Yaw=yaw, Along=along, Speed=0.0, comment="Back onto the grid, at rest")
    released = drive(b)
    start = b.send("StartAutopilot", b.var("Ride"), graph=RIDE)
    b.chain(b.entry, held, turning)
    b.chain(turning["True"], mark, done)
    b.flow(turning["False"], done)
    b.chain(done["True"], turned, reached, when, released, place, start, enter(b, "AutopilotA"), b.ret)
    b.flow(done["False"], b.ret)


def write_segment(b, offset, off_road, deepest, covered, steps, after, starts=False):
    """A segment of the autopilot's ride: its farthest offset, its steps off the road and its deepest lean noted each step;
    at its end the lap distance it covered since SegmentStart (set at its second step when it starts the ride: the Ride graph
    has followed the bike onto the grid by then)."""
    wheels = []
    for index in (0, 1):
        state = b.pure("Call:Strada.VehicleComponent.GetWheelState(int)", Target=bike(b), Index=index)
        surface = b.pure("Get:Strada.VehicleWheelState.Surface", "Value", Target=state)
        wheels.append(b.both(b.pure("Get:Strada.VehicleWheelState.HasContact", "Value", Target=state),
                             b.expr("gravel || grass", gravel=b.equal(surface, b.lit("string", "Gravel")),
                                    grass=b.equal(surface, b.lit("string", "Grass")))))
    away = b.set(offset, b.expr("max(most, abs(offset))", most=b.var(offset), offset=b.get("Offset", RIDE, b.var("Ride"))))
    rough = b.set(off_road, b.expr("count + (front || rear ? 1 : 0)", count=b.var(off_road), front=wheels[0], rear=wheels[1]))
    leaned = b.set(deepest, b.expr("max(most, abs(lean))", most=b.var(deepest), lean=lean(b)))
    done = b.branch(b.expr(f"elapsed >= {steps}", elapsed=elapsed(b)))
    length = round(Circuit.LENGTH, 3)
    reading = b.set(covered, b.expr(f"(here - start + {length}) % {length}", here=ride_distance(b), start=b.var("SegmentStart")))
    if starts:
        first = b.branch(b.expr("elapsed == 1", elapsed=elapsed(b)), comment="Where on the lap it starts")
        b.chain(b.entry, first)
        b.chain(first["True"], b.set("SegmentStart", ride_distance(b)), away)
        b.flow(first["False"], away)
    else:
        b.chain(b.entry, away)
    b.chain(away, rough, leaned, done)
    b.chain(done["True"], reading, *after, b.ret)
    b.flow(done["False"], b.ret)


def write_autopilot_a(b):
    write_segment(b, "OffsetA", "OffRoadA", "DeepestA", "CoveredA", SEGMENT_A,
                  [b.set("SegmentStart", ride_distance(b), comment="The next segment starts here"), enter(b, "AutopilotB")],
                  starts=True)


def write_autopilot_b(b):
    stop = b.send("StopAutopilot", b.var("Ride"), graph=RIDE)
    readings = b.fmt(
        "Moto's assists and autopilot: in gear {Gear} after the throttle alone; the linked brakes stopped it in {Linked} m, the "
        "rear at {Rear}; turned {Turn} degrees in 0.5 s leaned {Lean} degrees at {Speed} m/s; the autopilot covered {A} m and {B} m, {OffsetA} and {OffsetB} m "
        "off the line at most, {RoughA} and {RoughB} steps off the road, leaned {DeepA} and {DeepB} degrees at most; done at "
        "step {Step}", Gear=b.var("AutoGear"), Linked=b.var("LinkedStop"), Rear=b.var("LinkedRear"), Turn=b.var("LeanTurn"), Lean=b.var("LeanReading"),
        Speed=b.var("LeanSpeed"), A=b.var("CoveredA"), B=b.var("CoveredB"), OffsetA=b.var("OffsetA"), OffsetB=b.var("OffsetB"),
        RoughA=b.var("OffRoadA"), RoughB=b.var("OffRoadB"), DeepA=b.var("DeepestA"), DeepB=b.var("DeepestB"), Step=b.var("AssistStep"))
    log = b.call("Call:Strada.Log.Info(string)", Message=readings, comment="Every reading, the same at every frame rate")
    write_segment(b, "OffsetB", "OffRoadB", "DeepestB", "CoveredB", SEGMENT_B,
                  [stop, log, b.set("AutopilotDone", True), enter(b, "Done")])


HELPERS = (
    (("AssistEnter", False, (("Next", "string"),), (), ()), "Assist enter: the next phase, which begins at the next step",
     write_enter),
    (("AssistWait", False, (), (), ()), "Wait: the bike's ride done, the assists on", write_wait),
    (("AssistLaunch", False, (), (), ()), "Launch: the throttle alone, the assist shifting", write_launch),
    (("AssistNeutral", False, (), (), ()), "Neutral: shifted down by hand, coasting to 20 m/s", write_neutral),
    (("AssistLinked", False, (), (), ()), "Linked: the front lever alone with the linked brakes on", write_linked),
    (("AssistSpeedUp", False, (), (), ()), "Speed up: on the straight, the throttle alone to 20 m/s", write_speed_up),
    (("AssistLean", False, (), (), ()), "Lean: a lean asked of the assist, then the grid", write_lean),
    (("AutopilotA", False, (), (), ()), "Autopilot A: from the grid through Turns 1 and 2", write_autopilot_a),
    (("AutopilotB", False, (), (), ()), "Autopilot B: on through the corners after, then stopped", write_autopilot_b),
)


def write_ride(b):
    """The ride: each fixed step after the bike's ride's (the runner graph's Event.FixedUpdate calls it), the phase's
    function, then the step's count."""
    steps = b.sequence(2)
    phases = b.add("Flow.Switch", {"Type": "string", "Cases": [name for name, _ in PHASES]}, Selection=b.var("AssistPhase"))
    b.chain(b.entry, steps)
    b.flow(steps["Then0"], phases)
    for name, function in PHASES:
        b.flow(phases[f"Case:{name}"], b.own(function))
    b.chain(steps["Then1"], b.set("AssistStep", b.expr("step + 1", step=b.var("AssistStep"))), b.ret)


STEPS = (("AssistRide", "Assists and autopilot: each fixed step, the phase's driving", write_ride),)

# --- The checks ------------------------------------------------------------------------------------------------------------


def checks():
    if WAIT >= ride_timeout():
        raise ValueError(f"an assist check waits {WAIT} s, not less than the ride test scene's timeout of {ride_timeout()} s")
    return (
        ("CheckAutoShift", "Auto shift: the throttle alone takes the bike out of neutral into first",
         "the throttle alone takes the bike out of neutral into first gear with the automatic shifts on"),
        ("CheckLinkedBrakes", "Linked brakes: the front lever stops shorter than the front brake alone",
         "with the linked brakes on the front lever also applies the rear and stops the bike from 20 m/s in its distance, "
         "shorter than the front brake alone"),
        ("CheckAssistLean", "Lean assist: a lean asked for turns the bike as that lean balances",
         "the rider assist turns the bike at the rate the lean asked for balances"),
        ("CheckPoseTuck", "Tuck: the rider tucks in at speed",
         "the rider tucks in at speed as the riding graph poses him"),
        ("CheckPoseHang", "Hang off: the rider hangs into a lean, the fork turns with the wheel",
         "the rider hangs off into a lean and the fork turns with the front wheel"),
        ("CheckAutopilotA", "Autopilot A: from the grid through Turns 1 and 2 on the line",
         "the autopilot rides from the grid through Turns 1 and 2 on the racing line at its pace"),
        ("CheckAutopilotB", "Autopilot B: on through the corners after, on the line",
         "the autopilot rides on through the corners after Turn 2 on the racing line at its pace"),
    )


def ride_timeout():
    return BikeChecks.ride_timeout()


def check_auto_shift(b, name):
    ready = b.expr("gear >= 0", gear=b.var("AutoGear"))
    passed = b.expr("gear == 1", gear=b.var("AutoGear"))
    text = b.fmt(f"in gear {{Gear}} {AUTO_GEAR_STEPS} steps after the throttle alone opened from neutral (first gear expected)",
                 Gear=b.var("AutoGear"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_linked_brakes(b, name):
    low, high, figure = bands()["linked"]
    share_low, share_high = REAR_SHARE_BAND
    stop, front, rear = b.var("LinkedStop"), b.var("FrontStop"), b.var("LinkedRear")
    ready = b.expr("value >= 0", value=stop)
    passed = b.both(ready, b.expr(f"value >= {low} && value <= {high} && value < front && rear >= {share_low} && rear <= {share_high}",
                                  value=stop, front=front, rear=rear))
    text = b.fmt(f"the front lever with the linked brakes stopped the bike in {{Stop}} m, the front brake alone in {{Front}} m (the "
                 f"definition's figure {figure}, the band {low} to {high}); the rear brake applied {{Rear}} (the share "
                 f"{share_low} to {share_high})", Stop=stop, Front=front, Rear=rear)
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_assist_lean(b, name):
    low, high, figure = bands()["turn"]
    seconds = round((LEAN_STEPS - TURN_FROM) * STEP, 6)
    speed_then = b.var("LeanSpeed")
    # The rate (degrees a second, to the left) at which the lean asked for balances the turn at the speed.
    balanced = b.expr(f"degrees({Vehicle.GRAVITY} * tan(radians({abs(figure)})) / max(speed, 1))", speed=speed_then)
    rate = b.expr(f"turn / {seconds}", turn=b.var("LeanTurn"))
    ready = b.expr("speed >= 0", speed=speed_then)
    passed = b.both(ready, b.expr(f"rate >= {low} * balanced && rate <= {high} * balanced", rate=rate, balanced=balanced))
    text = b.fmt(f"a lean request of {ASSIST_REQUEST} ({figure} degrees) turned the bike {{Rate}} degrees a second to the left at {{Speed}} "
                 f"m/s, where that lean balances {{Balanced}} (the band {low} to {high} times it); it leaned {{Lean}} degrees",
                 Rate=rate, Speed=speed_then, Balanced=balanced, Lean=b.var("LeanReading"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def drawn_lean(b):
    return b.once("drawn lean", lambda: b.expr("degrees(asin(clamp(-right.Y, -1, 1)))",
                                                right=b.pure("Get:Strada.TransformComponent.Right", "Value", Target=bike(b))))


def rider(b):
    return b.once("rider", lambda: b.get("Rider", BIKE, bike(b)))


def expected_pose(b):
    """Where the riding graph poses the rider for the bike's drawn lean and speed (Vehicle.rider_pose)."""
    hip = Bike.HIP
    return b.once("pose", lambda: b.expr(
        f"vec3({hip[0]} + {Vehicle.HANG_SHIFT} * clamp(lean / {Vehicle.MAX_LEAN}, -1, 1), "
        f"{hip[1]} - {Vehicle.TUCK_DROP} * smoothstep({Vehicle.TUCK_FROM}, {Vehicle.TUCK_FULL}, speed), "
        f"{hip[2]} - {Vehicle.TUCK_FORWARD} * smoothstep({Vehicle.TUCK_FROM}, {Vehicle.TUCK_FULL}, speed))", lean=drawn_lean(b), speed=speed(b)))


def check_pose_tuck(b, name):
    at = b.pure("Get:Strada.TransformComponent.Translation", "Value", Target=rider(b))
    riding = b.expr(f"autopilot && speed > {TUCK_SPEED}", autopilot=b.both(b.equal(b.var("AssistPhase"), b.lit("string", "AutopilotA")),
                                                                         b.pure("IsValid", Value=rider(b))), speed=speed(b))
    hip = Bike.HIP
    passed = b.expr(f"distance(at, pose) <= {POSE_TOLERANCE} && abs(at.Y - {round(hip[1] - Vehicle.TUCK_DROP, 4)}) <= {POSE_TOLERANCE} "
                    f"&& abs(at.Z - {round(hip[2] - Vehicle.TUCK_FORWARD, 4)}) <= {POSE_TOLERANCE}", at=at, pose=expected_pose(b))
    text = b.fmt("at {Speed} m/s the rider's hips stood at {At} in the bike, posed for {Pose}", Speed=speed(b), At=at, Pose=expected_pose(b))
    run(b, Checks.check(b, name, riding, WAIT, text, b.both(riding, passed)))


def check_pose_hang(b, name):
    at = b.pure("Get:Strada.TransformComponent.Translation", "Value", Target=rider(b))
    fork = b.get("Fork", BIKE, bike(b))
    wheel = b.get("FrontWheel", BIKE, bike(b))
    fork_axle = b.pure("Get:Strada.TransformComponent.Right", "Value", Target=fork)
    wheel_axle = b.pure("Get:Strada.TransformComponent.Right", "Value", Target=wheel)
    apart = b.expr("degrees(acos(clamp(dot(normalize(a), normalize(c)), -1, 1)))", a=fork_axle, c=wheel_axle)
    autopilot = b.expr("a || c", a=b.equal(b.var("AssistPhase"), b.lit("string", "AutopilotA")),
                       c=b.equal(b.var("AssistPhase"), b.lit("string", "AutopilotB")))
    leaning = b.expr(f"riding && valid && abs(lean) > {HANG_LEAN}", riding=autopilot, valid=b.pure("IsValid", Value=rider(b)),
                     lean=drawn_lean(b))
    hip = Bike.HIP
    passed = b.expr(f"distance(at, pose) <= {POSE_TOLERANCE} && (at.X - {hip[0]}) * lean > 0 && apart <= {FORK_TOLERANCE}", at=at,
                    pose=expected_pose(b), lean=drawn_lean(b), apart=apart)
    text = b.fmt("leaned {Lean} degrees the rider's hips stood at {At} in the bike, posed for {Pose}; the fork's axle {Apart} degrees "
                 "from the front wheel's", Lean=drawn_lean(b), At=at, Pose=expected_pose(b), Apart=apart)
    run(b, Checks.check(b, name, leaning, WAIT, text, b.both(leaning, passed)))


def check_autopilot(b, name, segment):
    low, high, figure = bands()[segment.lower()]
    covered = b.var(f"Covered{segment}")
    offset, rough, deepest = b.var(f"Offset{segment}"), b.var(f"OffRoad{segment}"), b.var(f"Deepest{segment}")
    ready = b.expr("covered >= 0", covered=covered)
    passed = b.both(ready, b.expr(f"covered >= {low} && covered <= {high} && offset <= {MOST_OFFSET} && rough == 0 && deepest <= "
                                  f"{MOST_LEAN}", covered=covered, offset=offset, rough=rough, deepest=deepest))
    text = b.fmt(f"the autopilot covered {{Covered}} m of the lap (its point mass {figure}, the band {low} to {high}), {{Offset}} m off "
                 f"the line at most (at most {MOST_OFFSET}), {{Rough}} steps with a wheel on gravel or grass, leaned {{Deepest}} "
                 f"degrees at most (at most {MOST_LEAN})", Covered=covered, Offset=offset, Rough=rough, Deepest=deepest)
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


WRITERS = {
    "CheckAutoShift": check_auto_shift,
    "CheckLinkedBrakes": check_linked_brakes,
    "CheckAssistLean": check_assist_lean,
    "CheckPoseTuck": check_pose_tuck,
    "CheckPoseHang": check_pose_hang,
    "CheckAutopilotA": lambda b, name: check_autopilot(b, name, "A"),
    "CheckAutopilotB": lambda b, name: check_autopilot(b, name, "B"),
}


def write_check(b, function, name):
    WRITERS[function](b, name)


def prepare_scene(session, found):
    """The scene starts with the assists off, so that the bike's own ride is the raw vehicle's, whatever the main scene's
    bike starts with; the riding graph's other values stay as the scene has them (the test driver)."""
    component = session.call("component_get", {"entity": found["Bike"], "component": "VisualScript"})["fields"]
    variables = dict(component.get("Variables", {}))
    variables.update({name: {"Type": "Bool", "Value": False} for name in ("LeanAssist", "LinkedBrakes", "AutoShift")})
    session.call("component_set", {"entity": found["Bike"], "component": "VisualScript", "fields": {"Variables": variables}})


SUITE = Suite(checks, write_check, ENTITIES, VARIABLES, HELPERS, prepare=prepare_scene, steps=STEPS)

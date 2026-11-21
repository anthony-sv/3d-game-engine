"""The ridden bike's checks, a module of Checks.MODULES: they join the ride test scene (Scenes/Ride.sscene, the main scene
without its props), whose runner, the Ride Tests entity, runs Scripts/RideTests.sgraph, and the flow test scene
(Scenes/Flow.sscene), whose runner, the Flow Tests entity, runs Scripts/FlowTests.sgraph, the same ride in each. The runner
rides the grid bike and reports what the ride read.

The ride is driven from the graph's Event.FixedUpdate, as a function of its own count of fixed steps and of what the bike
reads in them (its gear, its speed), never of a frame or of a time: the scene sets the bike's riding graph to its test
driver (Vehicle.TEST) with the rider assists off (the bands are the raw vehicle's, whatever the main scene's bike starts
with), and this graph writes that graph's public Throttle, FrontBrake, RearBrake and Lean and sends it
ShiftUp and ShiftDown, so the riding graph stays the one writer of the bike's inputs and shifts. Every reading is taken in
the fixed step it belongs to, so the ride, and what it reads, are the same at every frame rate, step for step; the checks
(latent functions of the update, as every test scene's) only wait for a reading and report it.

The ride, one phase after another (PHASES; the step a phase begins at is its step 0):

1. Launch: from the grid, ShiftUp into first gear and full throttle for LAUNCH_STEPS (3 s): the speed then.
2. Climb, ShiftUp into second on full throttle up to BRAKE_FROM; Ease To Front: the throttle closed, shifted down into
   neutral, coasting down to BRAKE_FROM; Front Brake: the front brake alone until the bike stops: how far it went.
3. Relaunch in first up to BRAKE_FROM, Ease To Rear, Rear Brake: the rear brake alone, the same way.
4. Roll in first up to COAST_FROM, Glide in neutral down to COAST_FROM; Coast: COAST_STEPS (1 s) with nothing
   applied, upright on the asphalt: the speed lost. (The bike's largest lean over the launch and this coast is the
   upright check's.)
5. Gravel: the bike teleported into the gravel trap outside Turn 6 (where the test scene's gravel block slides), facing
   along the lap at the speed it had, its wheels turning as they were, for COAST_STEPS more: the speed lost there.
6. Resume: teleported back onto the main straight (RESUME_AT, RESUME_LATERAL right of its center line) at the speed it had,
   up to COAST_FROM in first, Settle in neutral down to COAST_FROM; Lean: LEAN_INPUT (to the left) for LEAN_STEPS
   (1 s): its lean then, and how far its heading turned (positive to the left).
7. Done: teleported back onto the grid at rest, both brakes until its wheels and it stop; then the ride gives the riding
   graph no input and enters Rest, which no case of its switch runs, so it writes nothing more: the bike stands on the
   grid in neutral, its riding graph still on the test driver, for the checks that join this scene after these
   (Checks.MODULES), which may write its values from then on.

Each ShiftUp is counted SHIFT_SETTLE steps after it was sent: one gear up from where it was sent, exactly. The bands
(BANDS) come from Vehicle.ride_figures(), the closed forms the definition's figures give (Docs/Architecture.md section 15,
the ridden bike), each with the room its form leaves stated beside it.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import Bike  # noqa: E402  (the grid)
import Circuit  # noqa: E402  (the layout)
import Vehicle  # noqa: E402  (the ridden bike: its figures and its riding graph)
from Checks import Suite, run  # noqa: E402
from Graphs import private  # noqa: E402

VECTOR = "Strada.Vector3"
BIKE = Vehicle.BIKE
ANGLE_AXIS = "Call:Strada.Quaternion.AngleAxis(float,Strada.Vector3)"
TELEPORT = "Call:Strada.RigidBodyComponent.Teleport(Strada.Vector3,Strada.Quaternion)"

# --- The ride ------------------------------------------------------------------------------------------------------------

# How long the launch holds full throttle, how long each coast and the lean last (fixed steps of the project's 1/60 s).
STEP = 1.0 / 60.0
LAUNCH_STEPS = 180
COAST_STEPS = 60
LEAN_STEPS = 60
# The speeds the phases ride from (m/s): the brakes' (Vehicle.ride_figures' 20 m/s), and the coasts' and the lean's (its
# 15 m/s). A phase that accelerates rides up to the speed, and the one after it coasts in neutral down to it: as the
# throttle closes, the rear wheel, which spins faster than the road under full throttle, gives the bike back a little
# speed, so the coast starts above it.
BRAKE_FROM = 20.0
COAST_FROM = 15.0
# The lean asked for in the lean phase (-1 left to 1 right).
LEAN_INPUT = -0.5
# How many steps after a ShiftUp its gear is read, and how many steps every phase that shifts up stays at least, so that
# the next phase shifts nothing before the count; how many steps apart a coast shifts down.
SHIFT_SETTLE = 10
SHIFT_HOLD = SHIFT_SETTLE + 2
SHIFT_DOWN_EVERY = 10
# Below this speed (m/s) a brake has stopped the bike; a brake that has not stopped it after BRAKE_LIMIT steps gives up.
# Back on the grid at the end, the bike is braked for STAND_STEPS at least, while its wheels, which turned on, stop.
STOPPED = 0.1
BRAKE_LIMIT = 900
STAND_STEPS = 30
# Where the gravel coast and the lean are ridden: the middle of the gravel trap 30 m before Turn 6's apex (Checks.
# gravel_slide), and the main straight before Turn 1, 3 m right of its center line so that the lean to the left (which
# turns the bike about 45 degrees) ends on the road. The bike's origin is on the surface it stands on.
GRAVEL_AT = Checks.gravel_slide()
RESUME_AT = 320.0
RESUME_LATERAL = 3.0


def spot(s, lateral, height):
    """A place to teleport the bike to: its origin on the surface, its yaw along the lap and the direction of travel."""
    at = [round(value, 4) for value in Circuit.offset_point(s, lateral, height)]
    return at, round(Circuit.yaw(s), 4), [round(value, 6) for value in Circuit.frame(s)[1]]


def gravel_spot():
    lateral, height = Checks.gravel_spot(GRAVEL_AT)
    return spot(GRAVEL_AT, lateral, height)


def resume_spot():
    return spot(RESUME_AT, RESUME_LATERAL, Circuit.road_height(RESUME_LATERAL))


def grid_spot():
    """The grid, where the scene places the bike, facing along the lap."""
    translation, yaw = Bike.grid_place()
    return [round(value, 4) for value in translation], round(yaw, 4), [round(value, 6) for value in Circuit.frame(0.0)[1]]


# --- The bands -------------------------------------------------------------------------------------------------------------

# Each band's room, as shares of its figure (low, high). The launch's form takes the spinning rear tyre at its sliding
# friction (1.0) for the whole 3 s, where it grips up to its peak (1.2) whenever it spins less: the ride is faster than
# the form, never much slower. The stops' forms move the weight to the front tyre at once, where the springs' pitch moves
# it over the first tenths of a second: the stops are longer than the forms, never much shorter. The gravel's coast
# compares two rolling drags the vehicle applies by the wheels' load, which the forms take as the bike's weight alone.
LAUNCH_ROOM = (0.9, 1.3)
STOP_ROOM = (0.85, 1.25)
GRAVEL_ROOM = (0.75, 1.25)

def bands():
    """Each check's band, from the figures Vehicle.py states: (low, high, the figure it is drawn from)."""
    figures = Vehicle.ride_figures()
    launch = figures["launch_3s"]
    front = figures["front_stop_20"]
    rear = figures["rear_stop_20"]
    gravel = figures["coast_asphalt_15"] - figures["coast_gravel_15"]
    return {
        "launch": (round(LAUNCH_ROOM[0] * launch, 2), round(LAUNCH_ROOM[1] * launch, 2), round(launch, 2)),
        "front": (round(STOP_ROOM[0] * front, 2), round(STOP_ROOM[1] * front, 2), round(front, 2)),
        "rear": (round(STOP_ROOM[0] * rear, 2), round(STOP_ROOM[1] * rear, 2), round(rear, 2)),
        "gravel": (round(GRAVEL_ROOM[0] * gravel, 2), round(GRAVEL_ROOM[1] * gravel, 2), round(gravel, 2)),
    }


# The largest lean on the straight with no lean asked (degrees either way), and the least lean and turn of the heading a
# lean to the left gives in LEAN_STEPS (degrees).
UPRIGHT = 3.0
LEANED = 5.0
TURNED = 5.0

# --- The runner graph's interface -----------------------------------------------------------------------------------------

ENTITIES = (("Bike", "Bike", "The ridden bike, whose riding graph rides it on the values this graph writes"),)
VARIABLES = (
    # Where a check's wait started (Checks.wait_until), and the ride's own: its fixed steps, its phase and the step it began.
    private("StepStart", "double", 0.0),
    private("Step", "int", 0), private("Phase", "string", "Launch"), private("PhaseStep", "int", 0),
    private("ShiftFrom", "int", 0), private("ShiftAt", "int", -1), private("Shifts", "int", 0), private("ExactShifts", "int", 0),
    private("LaunchSpeed", "float", -1.0), private("MaxLean", "float", 0.0), private("BrakeFrom", VECTOR),
    private("FrontStop", "float", -1.0), private("RearStop", "float", -1.0), private("CoastFrom", "float", 0.0),
    private("AsphaltLoss", "float", -1.0), private("GravelLoss", "float", -1.0), private("Heading", VECTOR),
    private("LeanReached", "float", 0.0), private("Turned", "float", 0.0), private("Finished", "bool", False),
)


def bike(b):
    return b.var("Bike")


def speed(b):
    return b.once("speed", lambda: b.pure("Get:Strada.VehicleComponent.ForwardSpeed", "Value", Target=bike(b)))


def gear(b):
    return b.once("gear", lambda: b.pure("Get:Strada.VehicleComponent.Gear", "Value", Target=bike(b)))


def lean(b):
    return b.once("lean", lambda: b.pure("Get:Strada.VehicleComponent.LeanAngle", "Value", Target=bike(b)))


def position(b):
    return b.once("position", lambda: b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=bike(b)))


def forward(b):
    return b.once("forward", lambda: b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=bike(b)))


def elapsed(b):
    """The steps since the phase began (0 in its first step)."""
    return b.once("elapsed", lambda: b.expr("step - start", step=b.var("Step"), start=b.var("PhaseStep")))


def first_step(b):
    return b.branch(b.expr("elapsed == 0", elapsed=elapsed(b)), comment="The phase's first step")


def drive(b, throttle=0.0, front=0.0, rear=0.0, lean_input=0.0):
    return b.own("Drive", Throttle=throttle, FrontBrake=front, RearBrake=rear, Lean=lean_input)


def note_lean(b):
    """The largest lean either way so far, kept in MaxLean."""
    return b.set("MaxLean", b.expr("max(most, abs(lean))", most=b.var("MaxLean"), lean=lean(b)))


# --- The helpers and the phases: functions of the runner graph -----------------------------------------------------------

def write_drive(b):
    sets = [b.set(name, b.entry[name], graph=BIKE, target=bike(b)) for name in ("Throttle", "FrontBrake", "RearBrake", "Lean")]
    b.chain(b.entry, *sets, b.ret)


def write_enter(b):
    begins = b.set("PhaseStep", b.expr("step + 1", step=b.var("Step")), comment="Its first step is the next")
    b.chain(b.entry, begins, b.set("Phase", b.entry["Next"]), b.ret)


def write_shift_up(b):
    b.chain(b.entry, b.set("ShiftFrom", gear(b)), b.set("ShiftAt", b.var("Step")), b.send("ShiftUp", bike(b), graph=BIKE), b.ret)


def write_place(b):
    """Teleports the bike, upright and facing along the lap, keeping its speed (its wheels turn on at it) and no spin."""
    turn = b.pure(ANGLE_AXIS, Degrees=b.entry["Yaw"], Axis=[0.0, 1.0, 0.0])
    moved = b.call(TELEPORT, Target=bike(b), Position=b.entry["At"], Rotation=turn)
    along = b.call("Set:Strada.RigidBodyComponent.LinearVelocity", Target=bike(b),
                   Value=b.expr("along * speed", along=b.entry["Along"], speed=b.entry["Speed"]))
    still = b.call("Set:Strada.RigidBodyComponent.AngularVelocity", Target=bike(b), Value=[0.0, 0.0, 0.0])
    b.chain(b.entry, moved, along, still, b.ret)


def write_launch(b):
    start = first_step(b)
    shift = b.own("ShiftUp", comment="Out of neutral into first gear")
    held = drive(b, throttle=1.0)
    noted = note_lean(b)
    done = b.branch(b.expr(f"elapsed >= {LAUNCH_STEPS}", elapsed=elapsed(b)), comment="3 s of full throttle")
    reading = b.set("LaunchSpeed", speed(b))
    enter = b.own("Enter", Next="Climb")
    b.chain(b.entry, start)
    b.flow(start["True"], shift)
    b.chain(shift, held)
    b.flow(start["False"], held)
    b.chain(held, noted, done)
    b.flow(done["True"], reading)
    b.chain(reading, enter, b.ret)
    b.flow(done["False"], b.ret)


def write_accelerate(b):
    start = first_step(b)
    shift = b.own("ShiftUp")
    held = drive(b, throttle=1.0)
    done = b.branch(b.expr(f"elapsed >= {SHIFT_HOLD} && speed >= target", elapsed=elapsed(b), speed=speed(b), target=b.entry["UpTo"]),
                    comment="Fast enough, once the shift has been counted")
    enter = b.own("Enter", Next=b.entry["Next"])
    b.chain(b.entry, start)
    b.flow(start["True"], shift)
    b.chain(shift, held)
    b.flow(start["False"], held)
    b.chain(held, done)
    b.flow(done["True"], enter)
    b.chain(enter, b.ret)
    b.flow(done["False"], b.ret)


def write_ease(b):
    held = drive(b)
    down = b.branch(b.expr(f"elapsed % {SHIFT_DOWN_EVERY} == 0 && gear > 0", elapsed=elapsed(b), gear=gear(b)),
                    comment="One gear down at a time, until neutral")
    shift = b.send("ShiftDown", bike(b), graph=BIKE)
    done = b.branch(b.expr(f"elapsed >= {SHIFT_DOWN_EVERY} && gear == 0 && speed <= target", elapsed=elapsed(b), gear=gear(b),
                           speed=speed(b), target=b.entry["DownTo"]), comment="In neutral and slow enough")
    enter = b.own("Enter", Next=b.entry["Next"])
    b.chain(b.entry, held, down)
    b.flow(down["True"], shift)
    b.chain(shift, done)
    b.flow(down["False"], done)
    b.flow(done["True"], enter)
    b.chain(enter, b.ret)
    b.flow(done["False"], b.ret)


def write_brake(b):
    start = first_step(b)
    mark = b.set("BrakeFrom", position(b), comment="Where the brake goes on")
    held = drive(b, front=b.entry["Front"], rear=b.entry["Rear"])
    stopped = b.set("Done", b.expr(f"speed < {STOPPED} || elapsed >= {BRAKE_LIMIT}", speed=speed(b), elapsed=elapsed(b)))
    done = b.branch(b.var("Done"))
    enter = b.own("Enter", Next=b.entry["Next"])
    b.link(b.var("Done"), b.ret["Stopped"])
    b.link(b.expr("distance(vec2(at.X, at.Z), vec2(start.X, start.Z))", at=position(b), start=b.var("BrakeFrom")), b.ret["Distance"])
    b.chain(b.entry, start)
    b.flow(start["True"], mark)
    b.chain(mark, held)
    b.flow(start["False"], held)
    b.chain(held, stopped, done)
    b.flow(done["True"], enter)
    b.chain(enter, b.ret)
    b.flow(done["False"], b.ret)


def write_coast(b):
    at, yaw, along = gravel_spot()
    start = first_step(b)
    mark = b.set("CoastFrom", speed(b))
    held = drive(b)
    noted = note_lean(b)
    done = b.branch(b.expr(f"elapsed >= {COAST_STEPS}", elapsed=elapsed(b)), comment="1 s on the asphalt")
    loss = b.set("AsphaltLoss", b.expr("start - speed", start=b.var("CoastFrom"), speed=speed(b)))
    again = b.set("CoastFrom", speed(b), comment="The gravel's coast starts at this speed")
    place = b.own("Place", At=at, Yaw=yaw, Along=along, Speed=speed(b), comment="Into the gravel trap outside Turn 6")
    enter = b.own("Enter", Next="Gravel")
    b.chain(b.entry, start)
    b.flow(start["True"], mark)
    b.chain(mark, held)
    b.flow(start["False"], held)
    b.chain(held, noted, done)
    b.flow(done["True"], loss)
    b.chain(loss, again, place, enter, b.ret)
    b.flow(done["False"], b.ret)


def write_gravel(b):
    at, yaw, along = resume_spot()
    held = drive(b)
    done = b.branch(b.expr(f"elapsed >= {COAST_STEPS}", elapsed=elapsed(b)), comment="1 s in the gravel")
    loss = b.set("GravelLoss", b.expr("start - speed", start=b.var("CoastFrom"), speed=speed(b)))
    place = b.own("Place", At=at, Yaw=yaw, Along=along, Speed=speed(b), comment="Back onto the main straight")
    enter = b.own("Enter", Next="Resume")
    b.chain(b.entry, held, done)
    b.flow(done["True"], loss)
    b.chain(loss, place, enter, b.ret)
    b.flow(done["False"], b.ret)


def write_lean(b):
    start = first_step(b)
    mark = b.set("Heading", forward(b), comment="The heading the lean starts from")
    held = drive(b, lean_input=LEAN_INPUT)
    done = b.branch(b.expr(f"elapsed >= {LEAN_STEPS}", elapsed=elapsed(b)), comment="1 s of the lean")
    reached = b.set("LeanReached", lean(b))
    turned = b.set("Turned", b.expr("degrees(atan2(a.Z * c.X - a.X * c.Z, a.X * c.X + a.Z * c.Z))", a=b.var("Heading"), c=forward(b)))
    readings = b.fmt("Moto's ride: {Launch} m/s after the launch; stops of {Front} m on the front brake and {Rear} m on the rear; "
                     "{Asphalt} m/s lost coasting on the asphalt and {Gravel} m/s in the gravel; leaned {Most} degrees at most "
                     "upright; {Exact} of {Shifts} shifts one gear up; leaned {Lean} degrees and turned {Turned} degrees; done at step {Step}",
                     Launch=b.var("LaunchSpeed"), Front=b.var("FrontStop"), Rear=b.var("RearStop"), Asphalt=b.var("AsphaltLoss"),
                     Gravel=b.var("GravelLoss"), Most=b.var("MaxLean"), Exact=b.var("ExactShifts"), Shifts=b.var("Shifts"),
                     Lean=b.var("LeanReached"), Turned=b.var("Turned"), Step=b.var("Step"))
    log = b.call("Call:Strada.Log.Info(string)", Message=readings, comment="Every reading, the same at every frame rate")
    finished = b.set("Finished", True)
    at, yaw, along = grid_spot()
    place = b.own("Place", At=at, Yaw=yaw, Along=along, Speed=0.0, comment="Back onto the grid, at rest")
    enter = b.own("Enter", Next="Done")
    b.chain(b.entry, start)
    b.flow(start["True"], mark)
    b.chain(mark, held)
    b.flow(start["False"], held)
    b.chain(held, done)
    b.flow(done["True"], reached)
    b.chain(reached, turned, log, finished, place, enter, b.ret)
    b.flow(done["False"], b.ret)


def write_stand(b):
    held = drive(b, front=1.0, rear=1.0)
    done = b.branch(b.expr(f"elapsed >= {STAND_STEPS} && speed < {STOPPED}", elapsed=elapsed(b), speed=speed(b)),
                    comment="Its wheels have stopped turning too")
    released = drive(b)
    enter = b.own("Enter", Next="Rest", comment="No phase of that name: the ride writes nothing more")
    b.chain(b.entry, held, done)
    b.flow(done["True"], released)
    b.chain(released, enter, b.ret)
    b.flow(done["False"], b.ret)


FLOAT = "float"
STRING = "string"
HELPERS = (
    (("Drive", False, (("Throttle", FLOAT), ("FrontBrake", FLOAT), ("RearBrake", FLOAT), ("Lean", FLOAT)), (), ()),
     "Drive: the riding graph's test values, which it gives the bike this step", write_drive),
    (("Enter", False, (("Next", STRING),), (), ()), "Enter: the next phase, which begins at the next step", write_enter),
    (("ShiftUp", False, (), (), ()), "Shift up: one gear, noted to be counted", write_shift_up),
    (("Place", False, (("At", VECTOR), ("Yaw", FLOAT), ("Along", VECTOR), ("Speed", FLOAT)), (), ()),
     "Place: the bike teleported upright along the lap, at its speed", write_place),
    (("Launch", False, (), (), ()), "Launch: from the grid, first gear and full throttle for 3 s", write_launch),
    (("Accelerate", False, (("UpTo", FLOAT), ("Next", STRING)), (), ()),
     "Accelerate: one gear up and full throttle, up to a speed", write_accelerate),
    (("Ease", False, (("DownTo", FLOAT), ("Next", STRING)), (), ()),
     "Ease: the throttle closed, down into neutral, coasting down to a speed", write_ease),
    (("Brake", False, (("Front", FLOAT), ("Rear", FLOAT), ("Next", STRING)), (("Stopped", "bool"), ("Distance", FLOAT)),
      (("Done", "bool", False),)),
     "Brake: one brake until the bike stops, and how far it went", write_brake),
    (("Coast", False, (), (), ()), "Coast: 1 s upright on the asphalt, then into the gravel", write_coast),
    (("Gravel", False, (), (), ()), "Gravel: 1 s in the trap, then back onto the straight", write_gravel),
    (("Lean", False, (), (), ()), "Lean: to the left for 1 s, its lean and its turn read, then the grid", write_lean),
    (("Stand", False, (), (), ()), "Stand: both brakes until the bike stops, then the ride lets go", write_stand),
)


# The phases in the order they ride: (name, the function its case calls, that function's inputs, the variable a brake's
# distance goes to). Each phase's function enters the next.
PHASES = (
    ("Launch", "Launch", {}, None),
    ("Climb", "Accelerate", {"UpTo": BRAKE_FROM, "Next": "EaseToFront"}, None),
    ("EaseToFront", "Ease", {"DownTo": BRAKE_FROM, "Next": "FrontBrake"}, None),
    ("FrontBrake", "Brake", {"Front": 1.0, "Rear": 0.0, "Next": "Relaunch"}, "FrontStop"),
    ("Relaunch", "Accelerate", {"UpTo": BRAKE_FROM, "Next": "EaseToRear"}, None),
    ("EaseToRear", "Ease", {"DownTo": BRAKE_FROM, "Next": "RearBrake"}, None),
    ("RearBrake", "Brake", {"Front": 0.0, "Rear": 1.0, "Next": "Roll"}, "RearStop"),
    ("Roll", "Accelerate", {"UpTo": COAST_FROM, "Next": "Glide"}, None),
    ("Glide", "Ease", {"DownTo": COAST_FROM, "Next": "Coast"}, None),
    ("Coast", "Coast", {}, None),
    ("Gravel", "Gravel", {}, None),
    ("Resume", "Accelerate", {"UpTo": COAST_FROM, "Next": "Settle"}, None),
    ("Settle", "Ease", {"DownTo": COAST_FROM, "Next": "Lean"}, None),
    ("Lean", "Lean", {}, None),
    ("Done", "Stand", {}, None),
)
# The ShiftUps the ride sends: one as each phase that accelerates begins (the launch's among them).
SHIFT_UPS = sum(1 for _, function, _, _ in PHASES if function in ("Launch", "Accelerate"))


def phase_call(b, function, inputs, reading):
    """A phase's case: its function's call, and for a brake the distance noted once the bike has stopped."""
    call = b.own(function, **inputs)
    if reading is not None:
        stopped = b.branch(call["Stopped"])
        b.chain(call, stopped)
        b.flow(stopped["True"], b.set(reading, call["Distance"]))
    return call


def write_ride(b):
    """The ride: each fixed step (the runner graph's Event.FixedUpdate calls it, Checks.Suite.steps), the phase's function,
    then the count of a ShiftUp sent SHIFT_SETTLE steps ago, then the step's count."""
    steps = b.sequence(3)
    phases = b.add("Flow.Switch", {"Type": STRING, "Cases": [name for name, *_ in PHASES]}, Selection=b.var("Phase"))
    b.chain(b.entry, steps)
    b.flow(steps["Then0"], phases)
    for name, function, inputs, reading in PHASES:
        b.flow(phases[f"Case:{name}"], phase_call(b, function, inputs, reading))
    due = b.branch(b.expr(f"at >= 0 && step == at + {SHIFT_SETTLE}", at=b.var("ShiftAt"), step=b.var("Step")),
                   comment="A ShiftUp's gear, read once")
    counted = b.set("Shifts", b.expr("shifts + 1", shifts=b.var("Shifts")))
    exact = b.set("ExactShifts", b.expr("exact + (gear == before + 1 ? 1 : 0)", exact=b.var("ExactShifts"), gear=gear(b),
                                        before=b.var("ShiftFrom")))
    b.flow(steps["Then1"], due)
    b.flow(due["True"], counted)
    b.chain(counted, exact)
    b.chain(steps["Then2"], b.set("Step", b.expr("step + 1", step=b.var("Step"))), b.ret)


STEPS = (("BikeRide", "Ride: each fixed step, the phase's driving, the shifts counted", write_ride),)

# --- The checks ------------------------------------------------------------------------------------------------------------

# How long a check waits for its reading (seconds of the game, from when it starts). The checks run in turn and none waits
# for a reading the ride takes after it ends, so no wait need be longer than the ride: about RIDE_SECONDS (it ends at step
# 1,871 in Debug and 1,877 in Release, at the project's 1/60 s), and BRAKE_LIMIT's steps more when a brake never stops the
# bike (that stop gives up and the ride goes on, and its check fails on its reading). The phases that ride up or coast down
# to a speed have no step limit of their own, so a ride that never reaches one is ended by the scene's test timeout
# (Checks.SCENES), which must stay above WAIT for a check that waits in vain to report first.
RIDE_SECONDS = 32
WAIT = math.ceil(RIDE_SECONDS + BRAKE_LIMIT * STEP)


def ride_timeout():
    """The ride test scene's test timeout (seconds), which a check's wait must stay below."""
    return next(scene.timeout for scene in Checks.SCENES if scene.name == "Ride")


def shortest_timeout():
    """The shortest test timeout (seconds) of the test scenes this module joins, which its checks' waits stay below."""
    joined = {name for name, module in Checks.MODULES if module == "BikeChecks"}
    return min(scene.timeout for scene in Checks.SCENES if scene.name in joined)


def checks():
    if WAIT >= shortest_timeout():
        raise ValueError(f"a ride check waits {WAIT} s, not less than a test scene's timeout of {shortest_timeout()} s")
    return (
        ("CheckLaunch", "Launch: the speed 3 s from the grid in first gear at full throttle",
         "the bike launched from the grid in first gear on full throttle reaches its speed after 3 s"),
        ("CheckFrontBrake", "Front brake: the stop from 20 m/s on the front brake alone",
         "the front brake alone stops the bike from 20 m/s in its distance"),
        ("CheckRearBrake", "Rear brake: the stop from 20 m/s on the rear brake alone",
         "the rear brake alone stops the bike from 20 m/s in its distance, farther than the front brake"),
        ("CheckUpright", "Upright: no lean on the straight with none asked",
         "the bike stays upright on the straight with no lean asked"),
        ("CheckGravel", "Gravel: the coast in the trap slows it more than on the asphalt",
         "coasting in the gravel trap outside Turn 6 slows the bike more than on the asphalt"),
        ("CheckShifts", "Shifts: each ShiftUp one gear up, exactly", "each ShiftUp shifts the bike one gear up, exactly"),
        ("CheckLean", "Lean: a lean to the left leans the bike left and turns it left",
         "a lean to the left leans the bike to the left and turns it to the left"),
    )


def check_band(b, name, variable, band, unit, detail):
    low, high, figure = band
    reading = b.var(variable)
    ready = b.expr("value >= 0", value=reading)
    passed = b.both(ready, b.expr(f"value >= {low} && value <= {high}", value=reading))
    text = b.fmt(f"{detail} {{Value}} {unit} (the definition's figure {figure}, the band {low} to {high})", Value=reading)
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_launch(b, name):
    check_band(b, name, "LaunchSpeed", bands()["launch"], "m/s", "the speed after 3 s:")


def check_front_brake(b, name):
    check_band(b, name, "FrontStop", bands()["front"], "m", "the front brake stopped the bike in")


def check_rear_brake(b, name):
    low, high, figure = bands()["rear"]
    rear, front = b.var("RearStop"), b.var("FrontStop")
    ready = b.expr("value >= 0", value=rear)
    passed = b.both(ready, b.expr(f"value >= {low} && value <= {high} && value > front", value=rear, front=front))
    text = b.fmt(f"the rear brake stopped the bike in {{Rear}} m, the front in {{Front}} m (the definition's figure {figure}, "
                 f"the band {low} to {high})", Rear=rear, Front=front)
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_upright(b, name):
    ready = b.expr("loss >= 0", loss=b.var("AsphaltLoss"))
    passed = b.both(ready, b.expr(f"most <= {UPRIGHT}", most=b.var("MaxLean")))
    text = b.fmt(f"the bike leaned {{Most}} degrees at most over the launch and the coast (at most {UPRIGHT})", Most=b.var("MaxLean"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_gravel(b, name):
    low, high, figure = bands()["gravel"]
    asphalt, gravel = b.var("AsphaltLoss"), b.var("GravelLoss")
    ready = b.expr("loss >= 0", loss=gravel)
    passed = b.both(ready, b.expr(f"gravel - asphalt >= {low} && gravel - asphalt <= {high}", gravel=gravel, asphalt=asphalt))
    text = b.fmt(f"1 s of coasting lost {{Asphalt}} m/s on the asphalt and {{Gravel}} m/s in the gravel (the definition's "
                 f"difference {figure}, the band {low} to {high})", Asphalt=asphalt, Gravel=gravel)
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_shifts(b, name):
    shifts = b.var("Shifts")
    ready = b.expr(f"shifts >= {SHIFT_UPS}", shifts=shifts)
    passed = b.expr(f"shifts == {SHIFT_UPS} && exact == {SHIFT_UPS}", shifts=shifts, exact=b.var("ExactShifts"))
    text = b.fmt(f"{{Exact}} of {{Shifts}} ShiftUps shifted one gear up (of {SHIFT_UPS} sent)", Exact=b.var("ExactShifts"), Shifts=shifts)
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_lean(b, name):
    finished = b.var("Finished")
    leaned, turned = b.var("LeanReached"), b.var("Turned")
    passed = b.both(finished, b.expr(f"lean <= -{LEANED} && turned >= {TURNED}", lean=leaned, turned=turned))
    text = b.fmt(f"after 1 s of a lean of {LEAN_INPUT}, the bike leans {{Lean}} degrees (negative to the left) and its heading "
                 "turned {Turned} degrees to the left", Lean=leaned, Turned=turned)
    run(b, Checks.check(b, name, finished, WAIT, text, passed))


WRITERS = {
    "CheckLaunch": check_launch,
    "CheckFrontBrake": check_front_brake,
    "CheckRearBrake": check_rear_brake,
    "CheckUpright": check_upright,
    "CheckGravel": check_gravel,
    "CheckShifts": check_shifts,
    "CheckLean": check_lean,
}


def write_check(b, function, name):
    WRITERS[function](b, name)


def prepare_scene(session, found):
    """The bike's riding graph takes its values from this graph (its test driver) with the rider assists off, so that the
    ride is the raw vehicle's that the bands are drawn for."""
    session.call("component_set", {"entity": found["Bike"], "component": "VisualScript", "fields": {
        "Variables": {"Driver": {"Type": "Int32", "Value": Vehicle.TEST}}}})
    Checks.set_variables(session, found["Bike"], {name: {"Type": "Bool", "Value": False} for name in ("LeanAssist", "LinkedBrakes", "AutoShift")})


SUITE = Suite(checks, write_check, ENTITIES, VARIABLES, HELPERS, prepare=prepare_scene, steps=STEPS)

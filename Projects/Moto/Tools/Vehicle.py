"""Moto's ridden bike: the grid bike's vehicle definition (Vehicles/Moto.svehicle) and the curve of its engine's torque
(Vehicles/EngineTorque.scurve), its body and chassis collider, and its riding graph (Scripts/Bike.sgraph). Author.py's
bike part gives the Bike entity a dynamic body, the collider and a Vehicle on the definition, and its wheels a
VehicleWheel each; Jolt's motorcycle controller rides it (Docs/Architecture.md section 8, "Vehicles").

The definition takes its figures from the bike's tools, so that the bike rides as the ghost's line was computed for:

- the geometry from Bike.py: the wheels on their axles 1,485 mm apart, the front one steering about an axis raked 24
  degrees that meets the ground 98 mm ahead of its contact patch, the 120/70 ZR17 front tyre (0.2999 m in radius, 120 mm
  wide) and the 200/55 ZR17 rear one (0.3259 m, 200 mm), which Bike.py draws; the front suspension runs along the fork, the
  rear one straight up; each wheel's attachment lies where its spring, loaded by the bike at rest, holds the wheel on its
  axle, so the bike stands in play as the scene placed it;
- each wheel's contact width (the width of the cylinder the wheel casts against the ground, the definition's Width) its
  own, narrower than the tyre drawn: FRONT_CONTACT and REAR_CONTACT, at most MAX_CONTACT (below);
- the rest from RacingLine.py: 262 kg with the rider (the body's mass), 150 kW at the rear wheel at the top of the
  engine's torque curve, the primary drive of 1.80, the six gears from 2.714 to 1.304 and the final drive of 2.5625 (the
  definition's FinalDrive is the primary times the final: 4.6125), the shift at 15,500 rpm, the red line at 16,000 and
  idle at 1,700, the lean limit of the line's GRIP (atan 1.2 = 50 degrees), and the air's drag (DRAG_AREA), which Jolt has
  only as a body's linear damping: matched to it at the line's average speed.

The gearbox is Manual: the rider shifts with ShiftUp and ShiftDown, one gear per press, and the bike waits on the grid in
neutral; the riding graph's automatic shifts (a rider assist, below) shift it by the engine's speed. Each brake has its own
wheel: the front lever the front wheel, the rear pedal the rear one, and the linked brakes (an assist) add the rear to the
front lever.

The chassis collider is a box low between the wheels (never the wheels: a wheel entity has no collider): its center is
the body's center of mass, and its roll inertia must stay above the lean spring's damping times the collision step, or
the spring throws the bike over at its first contact (the engine warns of it); check() asserts it with room.

check() recomputes, from what it writes, the figures the ride's checks are drawn from (Docs/Architecture.md section 15):
the overall ratios, the speed at the red line in each gear, the wheel's power, the launch from rest at full throttle, the
stops from 20 m/s on each brake, the coast on the asphalt and in the gravel, the lean spring's bound and the center of
mass; RIDE gives them to the tests.

The riding graph (declare() and write()) is the bike's only writer of its driver's inputs and of its shifts: at each fixed
step it reads the driver Driver names and calls SetDriverInput, and it shifts on the shift actions' presses or on the
ShiftUp and ShiftDown events. Driver 0 is the player: the actions Throttle, FrontBrake, RearBrake and Lean, read at each
fixed step (each press of ShiftUp and ShiftDown is seen by exactly one step). Driver 1 is the autopilot and 2 a test: they
write the graph's public Throttle, FrontBrake, RearBrake and Lean, which the step reads, and send it ShiftUp and ShiftDown,
each one gear; the actions' presses do nothing then, as the events do while Driver is 0. A value a writer sets in its own
Event.FixedUpdate before this graph's runs reaches the same step, one set after it the next; which runs first is the
scene's order of its graphs, the same in every run (a sender made after the bike in the test scene ran first: its
throttle and its ShiftUp reached the step it sent them in). A sent ShiftUp moves one gear at that step. Every driver's
values pass through the rider assists (LeanAssist, LinkedBrakes with LinkedBrakeShare, AutoShift: public switches, on by
default; with all three off the vehicle gets the driver's values as given), and each frame the graph poses the rider and
turns the fork from the bike as it is drawn (the rider assist's and the pose's sections below).

    python Projects/Moto/Tools/Vehicle.py      (prints the figures check() asserts)
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Bike  # noqa: E402  (the motorcycle's geometry)
import Graphs  # noqa: E402  (writing graphs)
import MotoLib  # noqa: E402  (the assist's mapping and the brakes' easing, which the riding graph calls)
import RacingLine  # noqa: E402  (the line's figures: mass, power, gearbox, engine speeds)
from Graphs import EFFECTS, ENTITY as ENTITY_TYPE, EVENTS, private, public  # noqa: E402

DEFINITION = "Vehicles/Moto.svehicle"
TORQUE_CURVE = "Vehicles/EngineTorque.scurve"
BIKE = Graphs.script("Bike")
GRAVITY = RacingLine.GRAVITY

# --- The engine and the gearbox -----------------------------------------------------------------------------------------

# The definition's final drive: the primary drive and the chain's together, from the gearbox's output to the rear wheel.
FINAL_DRIVE = RacingLine.PRIMARY * RacingLine.FINAL
# The engine's largest torque (N m) and its torque's fraction of it over the engine's speed (rpm): low at idle, full in
# the middle of its range and falling off towards the red line, where the power peaks at RacingLine.POWER (150 kW at the
# rear wheel: the torque here is the crank's as the wheel receives it). Linear between the keys, held beyond them.
MAX_TORQUE = 112.0
TORQUE = ((RacingLine.IDLE, 0.45), (4000.0, 0.62), (7000.0, 0.82), (10500.0, 1.0), (12500.0, 0.98), (14000.0, 0.917),
          (RacingLine.RED_LINE, 0.78))
# The engine's moment of inertia (kg m^2, the crank and what it turns: in first gear it weighs as 28 % more mass at the
# wheel, where a heavier one would hold the launch back) and how fast its speed decays by itself (per second): small,
# since Jolt takes I c w of torque from it at every speed (VehicleEngine.cpp:60-67).
ENGINE_INERTIA = 0.05
ENGINE_DAMPING = 0.005
# A quick shift: the clutch open for SWITCH_TIME, then closed again over CLUTCH_RELEASE (seconds); how hard the closed
# clutch couples the engine to the gearbox.
SWITCH_TIME = 0.05
CLUTCH_RELEASE = 0.1
CLUTCH_STRENGTH = 10.0
# The rider's shift points (rpm): up near the red line, down when braking below the line's DOWNSHIFT (the gearbox is
# Manual: the shifts are the rider's, or the autopilot's, which reads these).
SHIFT_UP = RacingLine.SHIFT_UP
SHIFT_DOWN = RacingLine.DOWNSHIFT

# --- The wheels -----------------------------------------------------------------------------------------------------------

# How far each wheel's suspension reaches (meters along its direction from its attachment) and where its wheel rests,
# loaded by the bike standing: the front fork travels 115 mm, the rear 130 mm at the wheel; each reaches its spring's sag
# below its rest length, so that a wheel the bike's pitch unloads stays on the ground.
FRONT_TRAVEL = (0.20, 0.315)
REAR_TRAVEL = (0.17, 0.30)
FRONT_REST = 0.27
REAR_REST = 0.25
# The springs (hertz and damping ratio) and their preload (meters compressed at full reach): the preload that holds each
# wheel at its rest length under the bike's weight (SAG: the spring's static compression, which Jolt's spring of this
# frequency gives over the chassis's effective mass at the wheel, measured in play).
SPRING_FREQUENCY = 3.5
SPRING_DAMPING = 0.6
FRONT_SAG = 0.048
REAR_SAG = 0.0538
# The wheels' moments of inertia (kg m^2) and how fast their spin decays by itself (per second; the rolling drag is the
# surface's RollingResistance, which the vehicle applies by the wheel's load).
FRONT_INERTIA = 0.6
REAR_INERTIA = 1.0
WHEEL_DAMPING = 0.02
# The wheels' contact widths (meters): the width of the cylinder each wheel casts against the ground along its suspension
# (the definition's Width), apart from the tyres Bike.py draws (Bike.FRONT_WIDTH and Bike.REAR_WIDTH). The engine rounds the
# cast's edges by half its width, but Jolt caps a cast's convex radius at 0.05 m (ScaleHelpers.h, from the cylinder's
# support function), so a wheel cast wider than MAX_CONTACT rides on a flat bottom: its contact sits off the wheel's plane
# by half the width less 0.05 m even upright, the ground's force misses the center of mass at the lean Jolt's controller
# aims for, and the bike rides its corners nearly upright (the feel pass, Docs/Architecture.md section 15: the bike as
# drawn leaned 0.07 of the lean that balances its turns). Narrower casts lean it the more: these lean it about two thirds of
# the way, a share LapChecks pins.
FRONT_CONTACT = 0.024
REAR_CONTACT = 0.04
MAX_CONTACT = 0.1
# The steering lock (degrees) and the brakes' torques at full input (N m): the front lever brakes the front wheel, the
# rear pedal the rear one. Applied all at once, the front's asks more of its tyre than the tyre's load at rest holds, so
# the front wheel locks and its tyre slides at SLIDING_FRICTION, its load growing as the bike pitches forward (locks());
# the rear's stays under that and slows the bike by its torque's force (in play the lightened rear wheel locks only as the
# bike slows). The front stops the bike in about half the rear's distance.
STEER_LOCK = 28.0
FRONT_BRAKE = 750.0
REAR_BRAKE = 300.0
# The tyres: Jolt's own curves (None), whose peak friction of 1.2 at a slip of 0.06 (and 3 degrees of slip angle), falling
# to 1.0 beyond, is the line's GRIP; the surface's TyreGrip multiplies them.
PEAK_FRICTION = 1.2
SLIDING_FRICTION = 1.0

# --- The lean -------------------------------------------------------------------------------------------------------------

# The lean limit, and Jolt's lean controller: the spring that turns the bike to its target lean and its damping (which
# stays under the roll inertia's bound, check()), and how much of the last target it keeps each step. The feel pass stiffened
# the spring from 5,000 with the damping held: on the narrow contacts it leans the bike deeper and faster (Turn 1's peak
# roll rate 43 degrees a second against 31); a damping raised with it slows the lean instead.
MAX_LEAN = round(math.degrees(math.atan(RacingLine.GRIP)), 1)
LEAN_SPRING = 10000.0
LEAN_DAMPING = 400.0
LEAN_SMOOTHING = 0.8
MAX_PITCH_ROLL = 60.0

# --- The body and the chassis collider ------------------------------------------------------------------------------------

MASS = RacingLine.MASS
# The box between the wheels (half extents and its center in the bike's frame, meters): the engine, the frame and the tank
# with the rider's weight low on them; its center is the body's center of mass.
CHASSIS_HALF = (0.15, 0.3, 0.55)
CHASSIS_CENTER = (0.0, 0.55, 0.0)
# The air's drag, 0.5 rho CdA v^2, as the linear damping Jolt has (dv/dt = -c v), matched at the line's average speed.
AVERAGE_SPEED = 37.1
AIR_DAMPING = round(0.5 * RacingLine.AIR_DENSITY * RacingLine.DRAG_AREA * AVERAGE_SPEED / MASS, 4)
CHASSIS_FRICTION = 0.5
# The physics step and its collision steps (Moto.sproj): the lean spring's bound is the damping times one collision step.
COLLISION_STEP = 1.0 / 60.0
# The surfaces' rolling resistance (Author.SURFACE_MATERIALS), for the coast figures.
ASPHALT_ROLLING = 0.015
GRAVEL_ROLLING = 0.25


def _round(vector):
    return [round(value, 6) + 0.0 for value in vector]


def _scale(vector, factor):
    return tuple(value * factor for value in vector)


def _subtract(a, b):
    return tuple(x - y for x, y in zip(a, b))


# The suspensions' directions, pointing down: the fork's along the steering axis, the rear one straight down.
FRONT_DIRECTION = _scale(Bike.STEERING_AXIS, -1.0)
REAR_DIRECTION = (0.0, -1.0, 0.0)


def attachment(axle, direction, rest):
    """Where a suspension is attached: rest meters up its direction from the wheel's axle."""
    return _subtract(axle, _scale(direction, rest))


def wheel(name, axle, direction, rest, travel, sag, steering, lock, radius, width, inertia, front_brake, rear_brake):
    minimum, maximum = travel
    return {"Name": name, "Position": _round(attachment(axle, direction, rest)), "SuspensionDirection": _round(direction),
            "SuspensionMinLength": minimum, "SuspensionMaxLength": maximum, "SuspensionPreload": round(rest - maximum + sag, 4),
            "SuspensionFrequency": SPRING_FREQUENCY, "SuspensionDamping": SPRING_DAMPING, "SteeringAxis": _round(steering),
            "MaxSteerAngle": lock, "Radius": round(radius, 4), "Width": width, "Inertia": inertia, "AngularDamping": WHEEL_DAMPING,
            "FrontBrakeTorque": front_brake, "RearBrakeTorque": rear_brake, "LongitudinalFriction": "0", "LateralFriction": "0"}


def definition():
    """The definition's fields, all of them, so that writing them again over the asset gives the same file."""
    return {
        "Wheels": [
            wheel("Front", Bike.FRONT_AXLE, FRONT_DIRECTION, FRONT_REST, FRONT_TRAVEL, FRONT_SAG, Bike.STEERING_AXIS, STEER_LOCK,
                  Bike.FRONT_RADIUS, FRONT_CONTACT, FRONT_INERTIA, FRONT_BRAKE, 0.0),
            wheel("Rear", Bike.REAR_AXLE, REAR_DIRECTION, REAR_REST, REAR_TRAVEL, REAR_SAG, (0.0, 1.0, 0.0), 0.0, Bike.REAR_RADIUS,
                  REAR_CONTACT, REAR_INERTIA, 0.0, REAR_BRAKE),
        ],
        "Up": [0.0, 1.0, 0.0], "Forward": [0.0, 0.0, -1.0], "MaxPitchRoll": MAX_PITCH_ROLL,
        "Engine": {"MaxTorque": MAX_TORQUE, "MinRPM": RacingLine.IDLE, "MaxRPM": RacingLine.RED_LINE,
                   "Torque": f"asset://{TORQUE_CURVE}", "Inertia": ENGINE_INERTIA, "AngularDamping": ENGINE_DAMPING},
        "Transmission": {"Mode": "Manual", "GearRatios": list(RacingLine.GEARS), "FinalDrive": round(FINAL_DRIVE, 6),
                         "SwitchTime": SWITCH_TIME, "ClutchReleaseTime": CLUTCH_RELEASE, "SwitchLatency": 0.5,
                         "ShiftUpRPM": SHIFT_UP, "ShiftDownRPM": SHIFT_DOWN, "ClutchStrength": CLUTCH_STRENGTH},
        "Lean": {"MaxLeanAngle": MAX_LEAN, "SpringConstant": LEAN_SPRING, "SpringDamping": LEAN_DAMPING,
                 "SmoothingFactor": LEAN_SMOOTHING, "SteeringLimit": True},
    }


def torque_curve():
    """The engine's torque curve's fields: the fraction of MAX_TORQUE over the engine's speed."""
    return {"PreExtrapolation": "Hold", "PostExtrapolation": "Hold", "Keys": [
        {"Time": speed, "Value": fraction, "Interpolation": "Linear", "HasIn": False, "In": 0.0, "HasOut": False, "Out": 0.0}
        for speed, fraction in TORQUE]}


def curves():
    """The curves the definition names, by their paths: written before it."""
    return {TORQUE_CURVE: torque_curve()}


def body():
    """The Bike entity's body and chassis collider (components' fields)."""
    return {"RigidBody": {"Type": "Dynamic", "Mass": MASS, "LinearDamping": AIR_DAMPING, "AngularDamping": 0.05},
            "BoxCollider": {"HalfExtents": list(CHASSIS_HALF), "Offset": list(CHASSIS_CENTER), "Friction": CHASSIS_FRICTION,
                            "Restitution": 0.0}}


# --- The figures ------------------------------------------------------------------------------------------------------------

def engine_torque(speed):
    """The engine's torque (N m) at an engine speed (rpm) at full throttle, as the curve gives it."""
    keys = TORQUE
    if speed <= keys[0][0]:
        return MAX_TORQUE * keys[0][1]
    for (s0, f0), (s1, f1) in zip(keys, keys[1:]):
        if speed <= s1:
            return MAX_TORQUE * (f0 + (f1 - f0) * (speed - s0) / (s1 - s0))
    return MAX_TORQUE * keys[-1][1]


def wheel_power(speed):
    """The power the engine gives the rear wheel at an engine speed (W): its torque, less what its own damping takes,
    times its angular speed."""
    omega = speed * math.pi / 30.0
    return (engine_torque(speed) - ENGINE_INERTIA * ENGINE_DAMPING * omega) * omega


def overall(gear):
    """The ratio from the engine to the rear wheel in a gear (1 to 6)."""
    return RacingLine.GEARS[gear - 1] * FINAL_DRIVE


def road_speed(engine_speed, gear):
    """The bike's speed (m/s) at an engine speed in a gear, the rear tyre rolling without slip."""
    return engine_speed * math.pi / 30.0 / overall(gear) * Bike.REAR_RADIUS


def roll_inertia():
    """The chassis box's moment of inertia about its front (kg m^2): what the lean spring turns."""
    width, height = 2.0 * CHASSIS_HALF[0], 2.0 * CHASSIS_HALF[1]
    return MASS / 12.0 * (width * width + height * height)


def pitch_inertia():
    height, length = 2.0 * CHASSIS_HALF[1], 2.0 * CHASSIS_HALF[2]
    return MASS / 12.0 * (height * height + length * length)


def load_transfer():
    """How much of the bike's weight each m/s^2 of acceleration moves from the front tyre to the rear (as a share of the
    mass): the center of mass's height over the wheelbase."""
    return CHASSIS_CENTER[1] / Bike.WHEELBASE


def drive_limit(friction):
    """The most the rear tyre drives the bike (m/s^2) at a friction, its load growing as the bike accelerates (the center of
    mass midway between the wheels)."""
    return friction * GRAVITY / 2.0 / (1.0 - friction * load_transfer())


def front_brake_limit(friction):
    """The most the front tyre brakes the bike (m/s^2) at a friction, its load growing as the bike pitches forward."""
    return friction * GRAVITY / 2.0 / (1.0 - friction * load_transfer())


def rear_brake_limit(friction):
    """The most the rear tyre brakes the bike (m/s^2) at a friction, its load shrinking as the bike pitches forward."""
    return friction * GRAVITY / 2.0 / (1.0 + friction * load_transfer())


def locks(torque, radius):
    """Whether a brake's torque, applied at once, locks its wheel: more force than the tyre's peak friction holds under its
    load at rest (half the bike's weight)."""
    return torque / radius > PEAK_FRICTION * MASS * GRAVITY / 2.0


def resistance(speed, rolling):
    """What slows the bike by itself at a speed (m/s^2): the air (as its linear damping) and the surface's rolling."""
    return AIR_DAMPING * speed + rolling * GRAVITY


def launch(seconds=3.0, step=1.0 / 600.0):
    """The speed after the seconds from rest in first gear at full throttle (m/s): the engine's force at the wheel over the
    bike's mass and what turns with the wheels (the engine through the gear), at most what the rear tyre holds at its
    sliding friction (a full throttle from rest spins it), less what slows the bike."""
    turning = MASS + (ENGINE_INERTIA * overall(1) ** 2 + REAR_INERTIA) / Bike.REAR_RADIUS ** 2 + FRONT_INERTIA / Bike.FRONT_RADIUS ** 2
    speed, time = 0.0, 0.0
    while time < seconds - 1e-9:
        engine_speed = max(RacingLine.IDLE, speed / Bike.REAR_RADIUS * overall(1) * 30.0 / math.pi)
        engine = engine_torque(min(engine_speed, RacingLine.RED_LINE)) * overall(1) / Bike.REAR_RADIUS / turning
        speed += (min(engine, drive_limit(SLIDING_FRICTION)) - resistance(speed, ASPHALT_ROLLING)) * step
        time += step
    return speed


def brake_deceleration(torque, radius, limit):
    """A brake's deceleration in neutral (m/s^2): its tyre sliding at the limit when it locks, else its torque's force over
    the bike's mass."""
    return limit(SLIDING_FRICTION) if locks(torque, radius) else torque / radius / MASS


def stop(start, deceleration, step=1.0 / 600.0):
    """The distance (m) the bike takes to stop from a speed at a braking deceleration, with what slows it by itself."""
    speed, distance = start, 0.0
    while speed > 0.0:
        slowing = deceleration + resistance(speed, ASPHALT_ROLLING)
        distance += speed * step - 0.5 * slowing * step * step
        speed -= slowing * step
    return distance


def coast(start, seconds, rolling, step=1.0 / 600.0):
    """The speed after coasting the seconds from a speed on a surface's rolling resistance."""
    speed, time = start, 0.0
    while time < seconds - 1e-9:
        speed -= resistance(speed, rolling) * step
        time += step
    return speed


def linked_force(share=None):
    """The rear brake's force (N) when the linked brakes give it a share of the front lever at full input."""
    return (LINKED_SHARE if share is None else share) * REAR_BRAKE / Bike.REAR_RADIUS


def linked_deceleration(share=None):
    """The front lever's deceleration in neutral with the linked brakes on (m/s^2): the front tyre sliding (its brake locks
    it) under the load the deceleration moves onto it, and the rear brake's share by its torque's force, which stays under
    what the lightened rear tyre holds (check() asserts it), so the rear wheel rolls on."""
    return (SLIDING_FRICTION * GRAVITY / 2.0 + linked_force(share) / MASS) / (1.0 - SLIDING_FRICTION * load_transfer())


def rear_holds(deceleration, force):
    """Whether the rear tyre holds a brake force at its peak friction under the load a deceleration leaves on it."""
    return force <= PEAK_FRICTION * MASS * (GRAVITY / 2.0 - deceleration * load_transfer())


def ride_figures():
    """The figures the ride's checks are drawn from (Architecture.md section 15, the ridden bike): the brakes in neutral,
    each lever alone, and the front lever with the linked brakes on."""
    front = brake_deceleration(FRONT_BRAKE, Bike.FRONT_RADIUS, front_brake_limit)
    rear = brake_deceleration(REAR_BRAKE, Bike.REAR_RADIUS, rear_brake_limit)
    linked = linked_deceleration()
    return {
        "launch_3s": launch(),
        "front_stop_20": stop(20.0, front),
        "rear_stop_20": stop(20.0, rear),
        "linked_stop_20": stop(20.0, linked),
        "front_deceleration": front,
        "rear_deceleration": rear,
        "linked_deceleration": linked,
        "coast_asphalt_15": coast(15.0, 1.0, ASPHALT_ROLLING),
        "coast_gravel_15": coast(15.0, 1.0, GRAVEL_ROLLING),
    }


# --- The rider assist ----------------------------------------------------------------------------------------------------------
#
# The rider asks for a lean (the Lean input, -1 to 1, as a share of MAX_LEAN) and the assist gives the vehicle the steering that
# holds that lean at the bike's speed: Jolt's motorcycle turns on a radius of wheelbase / (sin(steer) cos(caster)) and leans
# to atan(v^2 / (g R)) (MotorcycleController.cpp:141-147, whose steering limit is this mapping at the lean limit), so
# sin(steer) = wheelbase g tan(lean) / (v^2 cos(caster)), the steering input that steer over the lock. The speed is taken at
# ASSIST_MIN_SPEED at least, so that a request at a crawl turns the bars to its share of the lock and no further.
#
# The linked brakes: the front lever also applies the rear brake by LINKED_SHARE of its input, a share small enough that the
# lightened rear tyre rolls on under the front's hardest stop (check()), and both brakes are eased by the lean as Jolt's own
# motorcycle sample eases them ((1 - sin|lean|)^2, MotorcycleTest.cpp:179-186): a tyre braking hard while leaned pulls the
# bike off its line, since the stock friction limits its grip forwards and sideways apart. With the linked brakes off each
# lever is its own brake, as given.
#
# The shifts: with AutoShift on the riding graph shifts the Manual gearbox itself, at the definition's own speeds (so an edit
# of them moves its shifts), reading the engine's speed at the road's speed in the gear (the rear wheel's radius, the gear's
# ratio and the final drive), which a spinning rear tyre or a slipping clutch does not raise: out of neutral once the throttle
# opens, up at ShiftUpRPM, down below ShiftDownRPM while the gear
# below stays under ShiftUpRPM less SHIFT_DOWN_ROOM, never into neutral, and no shift within SHIFT_HOLD steps of the last
# (a shift opens the clutch for SWITCH_TIME and closes it over CLUTCH_RELEASE). The rider's ShiftUp and ShiftDown shift as
# well. A gearbox the definition makes Automatic would shift by Jolt's rules alone, the same for every rider and only with an
# edit of the definition; the assist is the rider's, a switch the player turns off to shift alone.

# The wheelbase as Jolt measures it (between the suspensions' farthest reach, along the bike's forward axis:
# MotorcycleController::GetWheelBase), the cosine of the steering axis' caster angle and the slowest speed the mapping takes.
ASSIST_WHEELBASE = 1.5033
CASTER_COS = round(Bike.STEERING_AXIS[1], 6)
ASSIST_MIN_SPEED = 4.0
LINKED_SHARE = 0.3
SHIFT_HOLD = 12
SHIFT_DOWN_ROOM = 1000.0


def assist_steer(request, speed):
    """The steering input (-1 to 1) the assist gives for a lean request (-1 to 1) at a speed (m/s), as MotoLib.sgraph's
    AssistSteer computes it."""
    lean = math.radians(request * MAX_LEAN)
    v = max(speed, ASSIST_MIN_SPEED)
    ratio = max(-1.0, min(1.0, ASSIST_WHEELBASE * GRAVITY * math.tan(lean) / (v * v * CASTER_COS)))
    return max(-1.0, min(1.0, math.degrees(math.asin(ratio)) / STEER_LOCK))


def brake_ease(lean):
    """The linked brakes' share of each lever at a lean (degrees)."""
    return (1.0 - abs(math.sin(math.radians(lean)))) ** 2


# --- The rider's pose --------------------------------------------------------------------------------------------------------
#
# The rider is rigid and posed by the riding graph each frame from the bike as it is drawn: he leans with the bike (he is its
# child), hangs off to the inside of a lean (his hips shifted HANG_SHIFT and his body turned HANG_ROLL more into the turn at
# the lean limit, in proportion below it) and tucks in at speed (from TUCK_FROM to TUCK_FULL, a smoothstep: lowered
# TUCK_DROP, moved TUCK_FORWARD towards the bars and pitched TUCK_PITCH forward). The fork turns about the steering axis with
# the front wheel: its steer is read from the wheel as the vehicle drew it (the wheel's axle against the bike's), a frame
# late at most; the vehicle draws the wheel in the fork's space, so turning the fork moves the wheel nowhere.
HANG_SHIFT = 0.12
HANG_ROLL = 10.0
TUCK_FROM = 20.0
TUCK_FULL = 45.0
TUCK_DROP = 0.09
TUCK_FORWARD = 0.08
TUCK_PITCH = 14.0


def rider_pose(lean, speed):
    """The rider's translation in the bike's frame and his turns (roll about the bike's forward axis, positive to the right,
    and pitch forward, degrees) at a drawn lean (degrees, positive right) and a speed (m/s)."""
    hang = max(-1.0, min(1.0, lean / MAX_LEAN))
    t = max(0.0, min(1.0, (speed - TUCK_FROM) / (TUCK_FULL - TUCK_FROM)))
    tuck = t * t * (3.0 - 2.0 * t)
    return ((Bike.HIP[0] + HANG_SHIFT * hang, Bike.HIP[1] - TUCK_DROP * tuck, Bike.HIP[2] - TUCK_FORWARD * tuck),
            HANG_ROLL * hang, TUCK_PITCH * tuck)


RIDE = None


def check():
    """Asserts the definition's figures against the tools' and what keeps the bike upright and on its wheels; returns a
    summary of the ride's figures."""
    global RIDE
    problems = []
    fields = definition()
    front, rear = fields["Wheels"]
    # The wheels rest on their axles: attachment + direction x rest length.
    for row, axle, rest, travel in ((front, Bike.FRONT_AXLE, FRONT_REST, FRONT_TRAVEL), (rear, Bike.REAR_AXLE, REAR_REST, REAR_TRAVEL)):
        center = tuple(p + d * rest for p, d in zip(row["Position"], row["SuspensionDirection"]))
        if math.dist(center, axle) > 1e-4:
            problems.append(f"the {row['Name']} wheel rests {math.dist(center, axle) * 1000.0:.2f} mm off its axle")
        if not travel[0] < rest < travel[1]:
            problems.append(f"the {row['Name']} wheel's rest length {rest} lies outside its travel {travel}")
    if abs(math.degrees(math.acos(-FRONT_DIRECTION[1])) - Bike.RAKE) > 1e-3:
        problems.append("the fork's suspension does not run along the steering axis")
    # The contacts are narrow enough to be round (a wider cast rides on a flat bottom), and no wider than the tyres drawn.
    for row, drawn in ((front, Bike.FRONT_WIDTH), (rear, Bike.REAR_WIDTH)):
        if not 0.0 < row["Width"] <= min(MAX_CONTACT, drawn):
            problems.append(f"the {row['Name']} wheel's contact width {row['Width']} m is not within 0 to {min(MAX_CONTACT, drawn)} m")
    # The figures the tools state.
    if abs(Bike.REAR_AXLE[2] - Bike.FRONT_AXLE[2] - 1.485) > 1e-6 or Bike.RAKE != 24.0 or abs(Bike.TRAIL - 0.098) > 1e-9:
        problems.append("Bike.py's geometry is not the published 1,485 mm, 24 degrees and 98 mm")
    if (MASS, RacingLine.POWER, RacingLine.PRIMARY, RacingLine.FINAL) != (262.0, 150000.0, 1.80, 2.5625):
        problems.append("RacingLine.py's mass, power or drives changed")
    if RacingLine.GEARS[0] != 2.714 or RacingLine.GEARS[-1] != 1.304 or len(RacingLine.GEARS) != 6:
        problems.append("RacingLine.py's gears changed")
    # The power at the wheel peaks at the line's 150 kW and never passes it.
    speeds = [RacingLine.IDLE + 10.0 * k for k in range(int((RacingLine.RED_LINE - RacingLine.IDLE) / 10.0) + 1)]
    powers = [wheel_power(speed) for speed in speeds]
    peak = max(powers)
    peak_speed = speeds[powers.index(peak)]
    if abs(peak - RacingLine.POWER) > 0.01 * RacingLine.POWER:
        problems.append(f"the wheel's power peaks at {peak / 1000.0:.1f} kW, not {RacingLine.POWER / 1000.0:.0f}")
    # The red line in each gear, and the shift point below it.
    if not SHIFT_UP < RacingLine.RED_LINE or not SHIFT_DOWN < SHIFT_UP:
        problems.append("the shift points are out of order")
    # The lean spring's bound (the engine warns below it): the roll inertia above the damping times the collision step.
    bound = LEAN_DAMPING * COLLISION_STEP
    if roll_inertia() < 1.25 * bound:
        problems.append(f"the chassis's roll inertia {roll_inertia():.2f} kg m^2 is within a quarter of the lean spring's bound {bound:.2f}")
    # The chassis box stays off the ground at the lean limit, turned about the tyres' contact line.
    lowest = (CHASSIS_CENTER[1] - CHASSIS_HALF[1]) * math.cos(math.radians(MAX_LEAN)) - CHASSIS_HALF[0] * math.sin(math.radians(MAX_LEAN))
    if lowest < 0.02:
        problems.append(f"the chassis box comes {lowest * 1000.0:.0f} mm from the ground at the lean limit")
    # The front brake stops the bike in about half the rear's distance; the front tyre never brakes the bike past the point
    # where the rear would lift (a stoppie: the deceleration at which the center of mass's moment lifts the rear).
    figures = ride_figures()
    stoppie = GRAVITY * (Bike.WHEELBASE / 2.0) / CHASSIS_CENTER[1]
    if front_brake_limit(PEAK_FRICTION) >= stoppie:
        problems.append("the front tyre holds more braking than keeps the rear wheel down")
    if not figures["front_stop_20"] < 0.65 * figures["rear_stop_20"]:
        problems.append("the front brake does not stop the bike shorter than the rear one")
    # The launch's 3 s stay in first gear, under the shift.
    if road_speed(SHIFT_UP, 1) <= figures["launch_3s"]:
        problems.append("the launch reaches first gear's shift within 3 s")
    # The linked brakes: the rear tyre rolls on under the front lever's hardest stop, which stays short of a stoppie and
    # stops the bike shorter than the front brake alone.
    if not rear_holds(figures["linked_deceleration"], linked_force()):
        problems.append("the linked brakes' share of the rear brake locks the rear wheel under the front's hardest stop")
    if figures["linked_deceleration"] >= stoppie or not figures["linked_stop_20"] < figures["front_stop_20"]:
        problems.append("the linked brakes do not stop the bike shorter than the front brake alone, short of a stoppie")
    # The assist's wheelbase is Jolt's: the suspensions' farthest reach, along the forward axis.
    reach = [[p + d * row["SuspensionMaxLength"] for p, d in zip(row["Position"], row["SuspensionDirection"])] for row in (front, rear)]
    if abs(reach[1][2] - reach[0][2] - ASSIST_WHEELBASE) > 1e-4:
        problems.append(f"the assist's wheelbase {ASSIST_WHEELBASE} is not Jolt's {reach[1][2] - reach[0][2]:.4f}")
    # At the lean limit the assist steers as far as Jolt's own limit lets the bars turn, and never past the lock.
    if not 0.0 < assist_steer(1.0, 20.0) < 1.0 or assist_steer(1.0, 0.0) != 1.0 or assist_steer(-0.5, 20.0) != -assist_steer(0.5, 20.0):
        problems.append("the assist's mapping is not within the lock, odd in its request and full at a crawl")
    assert not problems, "; ".join(problems)
    RIDE = figures
    tops = ", ".join(f"{road_speed(RacingLine.RED_LINE, gear) * 3.6:.0f}" for gear in range(1, 7))
    return (f"overall ratios {overall(1):.2f} to {overall(6):.2f}, red line at {tops} km/h; {peak / 1000.0:.1f} kW at "
            f"{peak_speed:.0f} rpm; center of mass {CHASSIS_CENTER[1]:.2f} m up, roll inertia {roll_inertia():.2f} kg m^2 "
            f"against the lean spring's {bound:.2f}; launch {figures['launch_3s']:.1f} m/s after 3 s (the rear tyre holds "
            f"{drive_limit(SLIDING_FRICTION):.2f} m/s^2 sliding); from 20 m/s in neutral the front brake stops in "
            f"{figures['front_stop_20']:.1f} m ({figures['front_deceleration']:.2f} m/s^2), the rear in "
            f"{figures['rear_stop_20']:.1f} m ({figures['rear_deceleration']:.2f} m/s^2), the front lever with the linked "
            f"brakes in {figures['linked_stop_20']:.1f} m ({figures['linked_deceleration']:.2f} m/s^2); coasting 1 s from 15 m/s: "
            f"{figures['coast_asphalt_15']:.2f} m/s on the asphalt, {figures['coast_gravel_15']:.2f} m/s in the gravel")


# --- The riding graph ---------------------------------------------------------------------------------------------------------

SET_INPUT = "Call:Strada.VehicleComponent.SetDriverInput(float,float,float,float)"
SHIFT_UP_CALL = "Call:Strada.VehicleComponent.ShiftUp()"
SHIFT_DOWN_CALL = "Call:Strada.VehicleComponent.ShiftDown()"
GET_ACTION = "Call:Strada.Input.GetAction(string)"
ACTION_PRESSED = "Call:Strada.Input.IsActionPressed(string)"
ANGLE_AXIS = "Call:Strada.Quaternion.AngleAxis(float,Strada.Vector3)"
# The drivers Driver names.
PLAYER, AUTOPILOT, TEST = 0, 1, 2


def declare(author):
    author.declare(BIKE, "The bike's riding graph: at each fixed step it gives the vehicle its driver's throttle, brakes and "
                   "lean through the rider assists, and it shifts when its driver asks or the automatic shifts say; each frame "
                   "it poses the rider and turns the fork. The only writer of the bike's inputs and shifts.", variables=(
        public("Driver", "int", PLAYER, "Who rides the bike: 0 the player (the actions), 1 the autopilot, 2 a test (both write "
               "Throttle, FrontBrake, RearBrake and Lean, and send ShiftUp and ShiftDown)", (PLAYER, TEST)),
        public("Throttle", "float", 0.0, "The throttle the autopilot or a test asks for, 0 to 1 (read while Driver is not 0)",
               (0.0, 1.0)),
        public("FrontBrake", "float", 0.0, "The front brake the autopilot or a test asks for, 0 to 1 (read while Driver is not 0)",
               (0.0, 1.0)),
        public("RearBrake", "float", 0.0, "The rear brake the autopilot or a test asks for, 0 to 1 (read while Driver is not 0)",
               (0.0, 1.0)),
        public("Lean", "float", 0.0, "The lean the autopilot or a test asks for, -1 left to 1 right (read while Driver is not 0)",
               (-1.0, 1.0)),
        public("LeanAssist", "bool", True, "The rider assist: the lean asked for is a lean, a share of the lean limit, which the "
               "assist steers the bike into at its speed (off: it is the steering itself)"),
        public("LinkedBrakes", "bool", True, "The linked brakes: the front brake also applies the rear by LinkedBrakeShare, and "
               "both ease as the bike leans (off: each brake alone, as given)"),
        public("LinkedBrakeShare", "float", LINKED_SHARE, "How much of the front brake the linked brakes give the rear, 0 to 1",
               (0.0, 1.0)),
        public("AutoShift", "bool", True, "The automatic shifts: out of neutral when the throttle opens, up and down by the "
               "engine's speed at the definition's shift speeds (off: only the rider shifts)"),
        public("Rider", ENTITY_TYPE, None, "The rider, found among the bike's children at the start (a read-out)"),
        public("Fork", ENTITY_TYPE, None, "The fork, found among the bike's children at the start (a read-out)"),
        public("FrontWheel", ENTITY_TYPE, None, "The front wheel, the fork's child, found at the start (a read-out)"),
        private("ShiftWait", "int", 0),
    ), events=(("ShiftUp", ()), ("ShiftDown", ())))


def _chosen(b, action, variable):
    """The driver's value of an input: the action's while the player rides, else the public value."""
    return b.expr(f"driver == {PLAYER} ? action : value", driver=b.var("Driver"), action=b.pure(GET_ACTION, Action=action),
                  value=b.var(variable))


def _definition(b):
    """The vehicle's definition, read by one node per region."""
    return b.once("definition", lambda: b.pure("Get:Strada.VehicleComponent.Definition", "Value"))


def _hold(b):
    """No automatic shift for SHIFT_HOLD steps after a shift."""
    return b.set("ShiftWait", SHIFT_HOLD)


def write(author):
    with author.region(BIKE, "Ride: each step, the driver's inputs through the assists, and shifts", EVENTS) as b:
        fixed = b.event("Event.FixedUpdate")
        steps = b.sequence(4)
        throttle = _chosen(b, "Throttle", "Throttle")
        front, rear, request = _chosen(b, "FrontBrake", "FrontBrake"), _chosen(b, "RearBrake", "RearBrake"), _chosen(b, "Lean", "Lean")
        speed = b.pure("Get:Strada.VehicleComponent.ForwardSpeed", "Value")
        lean = b.pure("Get:Strada.VehicleComponent.LeanAngle", "Value")
        steer = b.library(MotoLib.LIB, "AssistSteer", output="Steer", Request=request, Speed=speed).out
        ease = b.library(MotoLib.LIB, "BrakeEase", output="Share", Lean=lean).out
        linked = b.var("LinkedBrakes")
        ridden = b.call(SET_INPUT, comment="The assists off give each input as the driver gave it",
                        Throttle=throttle,
                        FrontBrake=b.expr("linked ? front * ease : front", linked=linked, front=front, ease=ease),
                        RearBrake=b.expr("linked ? max(rear, share * front) * ease : rear", linked=linked, rear=rear,
                                         share=b.var("LinkedBrakeShare"), front=front, ease=ease),
                        Lean=b.expr("assist ? steer : request", assist=b.var("LeanAssist"), steer=steer, request=request))
        up = b.branch(b.expr(f"driver == {PLAYER} && pressed", driver=b.var("Driver"), pressed=b.pure(ACTION_PRESSED, Action="ShiftUp")),
                      comment="Each press is seen by one step")
        down = b.branch(b.expr(f"driver == {PLAYER} && pressed", driver=b.var("Driver"),
                               pressed=b.pure(ACTION_PRESSED, Action="ShiftDown")))
        gear = b.pure("Get:Strada.VehicleComponent.Gear", "Value")
        definition = _definition(b)
        lower = b.pure("Call:Strada.Vehicle.GetGearRatio(int)", Target=definition, Gear=b.expr("gear - 1", gear=gear))
        ratio = b.pure("Call:Strada.Vehicle.GetGearRatio(int)", Target=definition, Gear=gear)
        waited = b.set("ShiftWait", b.expr("max(wait - 1, 0)", wait=b.var("ShiftWait")))
        # The engine's speed at the road's speed in the gear (rpm), not the engine's own: a rear tyre spinning on a full
        # throttle, or the clutch slipping, would shift too soon.
        road = b.expr(f"max(speed, 0) / {round(Bike.REAR_RADIUS, 6)} * ratio * final * {round(30.0 / math.pi, 6)}", speed=speed,
                      ratio=ratio, final=b.pure("Get:Strada.Vehicle.FinalDrive", "Value", Target=definition))
        shift = b.expr(f"auto && wait <= 0 ? (gear == 0 ? (throttle > 0 ? 1 : 0) : (rpm >= up && gear < count ? 1 : "
                       f"(gear > 1 && rpm < down && rpm * lower / ratio < up - {SHIFT_DOWN_ROOM} ? -1 : 0))) : 0",
                       auto=b.var("AutoShift"), wait=b.var("ShiftWait"), gear=gear, throttle=throttle, rpm=road,
                       up=b.pure("Get:Strada.Vehicle.ShiftUpRPM", "Value", Target=definition),
                       count=b.pure("Get:Strada.Vehicle.GearCount", "Value", Target=definition),
                       down=b.pure("Get:Strada.Vehicle.ShiftDownRPM", "Value", Target=definition), lower=lower, ratio=ratio)
        auto_up = b.branch(b.expr("shift == 1", shift=shift), comment="The automatic shifts, at the definition's speeds")
        auto_down = b.branch(b.expr("shift == -1", shift=shift))
        b.chain(fixed, steps)
        b.flow(steps["Then0"], ridden)
        b.flow(steps["Then1"], up)
        b.chain(up["True"], b.call(SHIFT_UP_CALL), _hold(b))
        b.flow(steps["Then2"], down)
        b.chain(down["True"], b.call(SHIFT_DOWN_CALL), _hold(b))
        b.flow(steps["Then3"], waited)
        b.chain(waited, auto_up)
        b.chain(auto_up["True"], b.call(SHIFT_UP_CALL), _hold(b))
        b.flow(auto_up["False"], auto_down)
        b.chain(auto_down["True"], b.call(SHIFT_DOWN_CALL), _hold(b))
    with author.region(BIKE, "Shifts: the autopilot or a test sends ShiftUp and ShiftDown", EFFECTS) as b:
        for event, call in (("ShiftUp", SHIFT_UP_CALL), ("ShiftDown", SHIFT_DOWN_CALL)):
            sent = b.custom(event)
            driven = b.branch(b.expr(f"driver != {PLAYER}", driver=b.var("Driver")), comment="The player shifts on the actions")
            b.chain(sent, driven)
            b.chain(driven["True"], b.call(call), _hold(b))
    with author.region(BIKE, "Start: the rider, the fork and the front wheel, among the children", EVENTS) as b:
        each = b.add("Flow.ForEach", Array=b.pure("Get:Strada.Entity.Children", "Value", Target=b.self_()))
        name = b.pure("Get:Strada.Entity.Name", "Value", Target=each["Element"])
        rider = b.branch(b.equal(name, b.lit("string", "Rider")))
        fork = b.branch(b.equal(name, b.lit("string", "Fork")))
        wheels = b.pure("Get:Strada.Entity.Children", "Value", Target=each["Element"])
        has_wheel = b.branch(b.expr("count > 0", count=b.add("Array.Length", None, "Length", Array=wheels).out))
        b.chain(b.event("Event.Create"), each)
        b.chain(each["Body"], rider)
        b.flow(rider["True"], b.set("Rider", each["Element"]))
        b.flow(rider["False"], fork)
        found = b.set("Fork", each["Element"])
        b.flow(fork["True"], found)
        b.chain(found, has_wheel)
        b.flow(has_wheel["True"], b.set("FrontWheel", b.add("Array.Get", None, "Element", Array=wheels, Index=0).out))
    with author.region(BIKE, "Pose: the rider hangs off and tucks in, the fork turns with the wheel", EFFECTS) as b:
        rider, fork, wheel = b.var("Rider"), b.var("Fork"), b.var("FrontWheel")
        ready = b.branch(b.expr("r && f && w", r=b.pure("IsValid", Value=rider), f=b.pure("IsValid", Value=fork),
                                w=b.pure("IsValid", Value=wheel)), comment="Once the start has found them")
        right = b.pure("Get:Strada.TransformComponent.Right", "Value")
        up = b.pure("Get:Strada.TransformComponent.Up", "Value")
        forward = b.pure("Get:Strada.TransformComponent.Forward", "Value")
        drawn = b.expr("degrees(asin(clamp(-right.Y, -1, 1)))", right=right)
        hang = b.expr(f"clamp(lean / {MAX_LEAN}, -1, 1)", lean=drawn)
        tuck = b.expr(f"smoothstep({TUCK_FROM}, {TUCK_FULL}, speed)", speed=b.pure("Get:Strada.VehicleComponent.ForwardSpeed", "Value"))
        hip = Bike.HIP
        place = b.call("Set:Strada.TransformComponent.Translation", Target=rider, comment="Hips to the inside, low and forward",
                       Value=b.expr(f"vec3({hip[0]} + {HANG_SHIFT} * hang, {hip[1]} - {TUCK_DROP} * tuck, {hip[2]} - {TUCK_FORWARD} * tuck)",
                                    hang=hang, tuck=tuck))
        roll = b.pure(ANGLE_AXIS, Degrees=b.expr(f"-{HANG_ROLL} * hang", hang=hang), Axis=[0.0, 0.0, 1.0])
        pitch = b.pure(ANGLE_AXIS, Degrees=b.expr(f"-{TUCK_PITCH} * tuck", tuck=tuck), Axis=[1.0, 0.0, 0.0])
        turn = b.call("Set:Strada.TransformComponent.Rotation", Target=rider, Value=b.pure("Operator.Multiply", A=roll, B=pitch))
        rake = math.radians(Bike.RAKE)
        steer = b.expr(f"degrees(atan2(dot(axle, up * {math.sin(rake):.6f} + forward * {math.cos(rake):.6f}), dot(axle, right)))",
                       axle=b.pure("Get:Strada.TransformComponent.Right", "Value", Target=wheel), up=up, forward=forward, right=right)
        raked = b.pure(ANGLE_AXIS, Degrees=Bike.RAKE, Axis=[1.0, 0.0, 0.0])
        steered = b.call("Set:Strada.TransformComponent.Rotation", Target=fork, comment="The front wheel's steer, as drawn",
                         Value=b.pure("Operator.Multiply", A=raked, B=b.pure(ANGLE_AXIS, Degrees=steer, Axis=[0.0, 1.0, 0.0])))
        b.chain(b.event("Event.Update"), ready)
        b.chain(ready["True"], place, turn, steered)


if __name__ == "__main__":
    print(check())

"""Moto's ghost bike: a second motorcycle and rider, of the bike part's meshes and materials (so that the renderer draws
both in the same batches), that rides the racing line (RacingLine.py) round the lap without end while the scene plays.
Author.py's ghost part makes it: the Ghost entity at the line's pose where it starts, with the bike's parts under it as
Bike.ENTITIES has them, and its graph (Scripts/Ghost.sgraph). It has no rigid body and no collider: nothing simulates it
and nothing touches it; its graph places it every update (kinematic, in the plain sense).

The graph (declare() and write()): when the scene starts, the ghost sends itself Ride, which moves its timeline to
StartAt (seconds after the start and finish line) and plays it. The timeline loops over one lap of the line's keys:
every update places the ghost at Position and turns it to Heading (about +Y), leaned by Lean (positive to the right:
a turn about its forward axis), counts a lap when the timeline's time comes round past the line, and keeps the
read-outs other graphs follow: LapTime, and the engine's EngineSpeed and Load, which the engine's sound plays from.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Graphs  # noqa: E402  (writing graphs)
import RacingLine  # noqa: E402  (the line the ghost rides)
from Graphs import EFFECTS, EVENTS, public  # noqa: E402

GHOST = Graphs.script("Ghost")
# The Ghost entity's name, and where the main scene's ghost sets out (seconds after the start and finish line): at the
# line itself.
ENTITY = "Ghost"
START_AT = 0.0
# The test scene's ghost sets out this far into its lap, so that it leans through Turn 6 early in the test run and
# crosses the line well before the camera tour ends (Checks.py).
TEST_START_AT = 30.0
ANGLE_AXIS = "Call:Strada.Quaternion.AngleAxis(float,Strada.Vector3)"


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


def declare(author):
    lap = RacingLine.length()
    author.declare(GHOST, "The ghost bike: it rides the racing line round the lap without end, from StartAt seconds after "
                   "the start and finish line. Tools/RacingLine.py makes its keys.", variables=(
        public("StartAt", "float", START_AT, "Where on its lap the ghost sets out, in seconds after the start and finish line",
               (0.0, lap)),
        public("Laps", "int", 0, "How many times the ghost has crossed the start and finish line (a read-out)"),
        public("LapTime", "float", 0.0, "How far into its lap the ghost is, in seconds after the start and finish line (a read-out)"),
        public("EngineSpeed", "float", 0.0, "The ghost's engine speed in revolutions a minute, which its engine's sound follows "
               "(a read-out)"),
        public("Load", "float", 0.0, "How far open the ghost's throttle is, from 0 to 1, which its engine's sound follows (a read-out)"),
    ), events=(("Ride", ()),))


def write(author):
    with author.region(GHOST, "Start: the ghost sets out on its lap with the scene", EVENTS) as b:
        b.chain(b.event("Event.Create"), b.send("Ride", b.self_()))
    with author.region(GHOST, "Ride: the racing line's timeline moves the ghost round and round", EFFECTS) as b:
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
        b.chain(line["Update"], place, turn, lapped)
        b.flow(lapped["True"], count)
        b.flow(lapped["False"], mark)
        b.chain(count, mark, engine, load)

"""Moto's trackside cameras and the camera director: four cameras on masts behind the catch fences (the start line, Turn
1, the footbridge and Turn 6), each turning to follow the ghost bike, and a graph (Scripts/CameraDirector.sgraph) that
cuts between them and the camera tour. Author.py's trackside part makes the Trackside entity, which runs the graph, with
the four cameras as its children, in the order the cuts go round.

The cameras: each stands CAMERA_BEHIND meters behind the wall's face (behind the fence, whose panels are 2.6 m high) on
the side given, HEIGHT meters up, at a lap distance where no prop stands within CLEARANCE meters of it (tall ones, which
could stand in its view, within TALL_CLEARANCE); placing() moves it along the lap, a meter at a time either way, until
none does. A corner's camera stands on its inside at its apex (its outside has a grandstand at Turns 1 and 6); the start
line's 80 m before the line, across the road from the grandstands; the footbridge's 35 m before the bridge on the side
the racing line passes it, so the ghost rides close by it and on under the deck. They are not primary and their
listeners are off: the tour's camera shows and hears the scene until a cut.

The director (the graph): its public TourCamera is the tour's camera and Subject what the trackside cameras follow (the
ghost). When the scene starts it cuts to StartCamera (0, the tour, by default). The NextCamera action sends it
NextCamera, which cuts to the next trackside camera (round to the first), and ResetCamera sends it ResetCamera, which cuts
back to the tour without restarting it; a test, or any graph, sends it the same events. A cut makes one camera primary
and its listener the one that hears (the others off): the scene renders from it, and the sounds are heard where it
stands (a cut is no doppler: the listener is another entity). Every update turns each trackside camera towards the
subject, AIM meters above its origin. Active reads which camera shows: 0 the tour, 1 to 4 a trackside camera.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Circuit  # noqa: E402  (the layout)
import Graphs  # noqa: E402  (writing graphs)
import Props  # noqa: E402  (where the props stand)
import RacingLine  # noqa: E402  (the line the ghost rides)
from Graphs import EFFECTS, ENTITY, EVENTS, RULES, private, public  # noqa: E402

CAMERA_DIRECTOR = Graphs.script("CameraDirector")
ROOT = "Trackside"
# How far behind a wall's face a camera's mast stands, and how high the camera is (meters); its field of view (degrees).
CAMERA_BEHIND = Props.FENCE + 1.5
HEIGHT = 5.0
FOV = 35.0
# How far from a camera props stand: any prop, and one that rises within a meter of the camera's height or above it.
CLEARANCE = 1.5
TALL_CLEARANCE = 6.0
# How far above the ghost's origin (on the road) the cameras aim: about the rider's middle.
AIM = 0.9
# Where the footbridge's camera stands before the bridge (meters), and how far along the lap a camera may move from its
# site to find room.
FOOTBRIDGE_LEAD = 35.0
MAX_MOVE = 20


def _corner(name):
    """A corner's apex (lap distance) and its inside: +1 right of travel, -1 left."""
    _, start, end, side = next(row for row in Circuit.corner_ranges() if row[0].name == name)
    return start + 0.5 * (end - start), side


def _line_side(s):
    """The side of the center line the racing line is on at a lap distance: +1 right of travel, -1 left."""
    point = min(RacingLine.LINE, key=lambda candidate: abs(math.remainder(candidate.s - s, Circuit.LENGTH)))
    return 1 if point.lateral >= 0.0 else -1


def sites():
    """The four cameras before placing: (name, lap distance, side)."""
    turn1, turn1_inside = _corner("Turn 1")
    turn6, turn6_inside = _corner("Turn 6")
    footbridge = Circuit.FOOTBRIDGE - FOOTBRIDGE_LEAD
    return (("Start Line Camera", Circuit.LENGTH - 80.0, -1),
            ("Turn 1 Camera", turn1, turn1_inside),
            ("Footbridge Camera", footbridge, _line_side(footbridge)),
            ("Turn 6 Camera", turn6, turn6_inside))


def _distance(point, footprint):
    """How far a point (x, z) lies from a footprint seen from above (0 inside it)."""
    dx, dz = point[0] - footprint.center[0], point[1] - footprint.center[1]
    along_u = max(abs(dx * footprint.u[0] + dz * footprint.u[1]) - footprint.half_u, 0.0)
    along_v = max(abs(dx * footprint.v[0] + dz * footprint.v[1]) - footprint.half_v, 0.0)
    return math.hypot(along_u, along_v)


def _position(s, side):
    lateral = side * (Circuit.barrier_offset(s % Circuit.LENGTH, "right" if side > 0 else "left") + CAMERA_BEHIND)
    return Circuit.offset_point(s % Circuit.LENGTH, lateral, HEIGHT)


def _clear(position, footprints):
    for footprint, top in footprints:
        margin = TALL_CLEARANCE if top >= HEIGHT - 1.0 else CLEARANCE
        if _distance((position[0], position[2]), footprint) < margin:
            return False
    return True


def placing(props=None):
    """The four cameras: (name, world position), each at the lap distance nearest its site (a meter at a time, ahead
    first) where no prop stands in its way."""
    footprints = [prop.footprint() for prop in (props if props is not None else Props.place())]
    placed = []
    for name, s, side in sites():
        for step in [0] + [sign * k for k in range(1, MAX_MOVE + 1) for sign in (1, -1)]:
            position = _position(s + step, side)
            if _clear(position, footprints):
                break
        else:
            raise SystemExit(f"no room for the {name} within {MAX_MOVE} m of its site")
        placed.append((name, tuple(round(value, 3) for value in position)))
    return placed


def camera(position):
    """A trackside camera's components where it stands, unturned until the director's first update turns it."""
    return {"Transform": {"Translation": list(position)},
            "Camera": {"PerspectiveFOV": FOV, "PerspectiveNear": 0.1, "PerspectiveFar": 5000.0, "Primary": False},
            "AudioListener": {"Active": False}}


# --- The graph ---------------------------------------------------------------------------------------------------------

def declare(author):
    author.declare(CAMERA_DIRECTOR, "The camera director: it cuts between the tour's camera and its children, the trackside "
                   "cameras, on the NextCamera and ResetCamera actions (or the events of those names any graph sends), "
                   "and turns the trackside cameras to follow the subject.", variables=(
        public("TourCamera", ENTITY, None, "The camera the tour flies: it shows and hears the scene when no trackside camera does"),
        public("Subject", ENTITY, None, "What the trackside cameras follow"),
        public("StartCamera", "int", 0, "The camera that shows when the scene starts: 0 the tour, 1 to 4 a trackside camera",
               (0, 4)),
        public("Active", "int", 0, "The camera that shows: 0 the tour, 1 to 4 a trackside camera (a read-out)"),
        private("Cameras", "Strada.Entity[]"),
    ), events=(("NextCamera", ()), ("ResetCamera", ())), functions=(("Cut", False, (("Index", "int"),), (), ()),))


def write(author):
    with author.region(CAMERA_DIRECTOR, "Cut: camera Index shows and hears the scene (0: the tour's camera)", RULES, "Cut") as b:
        each = b.add("Flow.ForEach", Array=b.var("Cameras"))
        chosen = b.expr("index + 1 == cut", index=each["Index"], cut=b.entry["Index"])
        shows = b.call("Set:Strada.CameraComponent.Primary", Target=each["Element"], Value=chosen)
        hears = b.call("Set:Strada.AudioListenerComponent.Active", Target=each["Element"], Value=chosen)
        tour = b.var("TourCamera")
        known = b.branch(b.pure("IsValid", Value=tour))
        home = b.expr("cut == 0", cut=b.entry["Index"])
        tour_shows = b.call("Set:Strada.CameraComponent.Primary", Target=tour, Value=home)
        tour_hears = b.call("Set:Strada.AudioListenerComponent.Active", Target=tour, Value=home)
        active = b.set("Active", b.entry["Index"])
        b.chain(b.entry, each)
        b.chain(each["Body"], shows, hears)
        b.chain(each["Completed"], known)
        b.chain(known["True"], tour_shows, tour_hears, active)
        b.flow(known["False"], active)
        b.flow(active, b.ret)
    with author.region(CAMERA_DIRECTOR, "Start: the cameras are the director's children; it cuts to StartCamera", EVENTS) as b:
        cameras = b.set("Cameras", b.pure("Get:Strada.Entity.Children", "Value", Target=b.self_()))
        b.chain(b.event("Event.Create"), cameras, b.own("Cut", Index=b.var("StartCamera")))
    with author.region(CAMERA_DIRECTOR, "Actions: NextCamera and ResetCamera send the director its cuts", EVENTS) as b:
        b.chain(b.event("Event.ActionPressed", {"Action": "NextCamera"}), b.send("NextCamera", b.self_()))
        b.chain(b.event("Event.ActionPressed", {"Action": "ResetCamera"}), b.send("ResetCamera", b.self_()))
    with author.region(CAMERA_DIRECTOR, "Next camera: a cut to the next trackside camera, round to the first", RULES) as b:
        following = b.expr("active % count + 1", active=b.var("Active"),
                           count=b.add("Array.Length", None, "Length", Array=b.var("Cameras")).out)
        b.chain(b.custom("NextCamera"), b.own("Cut", Index=following))
    with author.region(CAMERA_DIRECTOR, "Reset camera: a cut back to the tour, which flies on", RULES) as b:
        b.chain(b.custom("ResetCamera"), b.own("Cut", Index=0))
    with author.region(CAMERA_DIRECTOR, "Follow: every trackside camera turns towards the subject", EFFECTS) as b:
        subject = b.var("Subject")
        known = b.branch(b.pure("IsValid", Value=subject))
        each = b.add("Flow.ForEach", Array=b.var("Cameras"))
        target = b.expr(f"at + vec3(0, {AIM}, 0)", at=b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value",
                                                               Target=subject))
        eye = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=each["Element"])
        turn = b.call("Set:Strada.TransformComponent.EulerAngles", Target=each["Element"],
                      Value=b.expr(LOOK, target=target, eye=eye),
                      comment="Pitch, then yaw: the director stands at the origin unturned, so its children's angles are the world's")
        b.chain(b.event("Event.Update"), known)
        b.flow(known["True"], each)
        b.flow(each["Body"], turn)


# The camera's Euler angles (degrees: pitch, then yaw, no roll) that turn its forward (-Z) from the eye towards the target,
# as the tour turns its camera.
LOOK = ("vec3(degrees(atan2(target.Y - eye.Y, length(vec2(target.X - eye.X, target.Z - eye.Z)))), "
        "degrees(atan2(eye.X - target.X, eye.Z - target.Z)), 0)")


if __name__ == "__main__":
    for name, position in placing():
        print(f"{name}: {position}")

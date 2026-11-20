"""Moto's trackside cameras, its chase camera and the camera director: seven television cameras on masts behind the catch
fences (the start line, Turns 1 and 3, the footbridge, Turns 6, 7 and 9), each turning to follow what it shows, a chase
camera that rides behind the ridden bike, and a graph (Scripts/CameraDirector.sgraph) that cuts between them and the
camera tour. Author.py's trackside part makes the Trackside entity, which runs the graph, with the cameras as its
children: the trackside cameras in the order of the lap from the start line (the order the cuts go round), then the
chase camera, last.

The trackside cameras: each stands CAMERA_BEHIND meters behind the wall's face (behind the fence, whose panels are 2.6 m
high) on the side given, HEIGHT meters up, at a lap distance where no prop stands within CLEARANCE meters of it (tall
ones, which could stand in its view, within TALL_CLEARANCE); placing() moves it along the lap, a meter at a time either
way, until none does. A corner's camera stands on its inside at its apex (its outside has a grandstand at Turns 1 and 6);
the start line's 80 m before the line, across the road from the grandstands; the footbridge's 35 m before the bridge on
the side the racing line passes it, so the ghost rides close by it and on under the deck. They are not primary and their
listeners are off: the tour's camera shows and hears the scene until a cut. The chase camera stands behind the grid bike
where the chase view would put it, not primary and its listener off too.

The director (the graph): its public TourCamera is the tour's camera, Subject what the trackside cameras follow in the
attract mode (the ghost) and Bike the ridden bike, which the chase camera follows, and the trackside cameras in the race.
When the scene starts it cuts to StartCamera (0, the tour, by default; 1 to 7 a trackside camera, 8 the chase camera; a
number that names no camera is warned of, and the tour shows). It cuts when told:

- the NextCamera action sends it NextCamera, which cuts to the next trackside camera (round to the first; from the tour
  or the chase camera, to the first), and ResetCamera sends it ResetCamera, which cuts back to the tour without
  restarting it, or in the race to the chase camera; a test, or any graph, sends it the same events.

And by itself, with its public Directs on (as Moto has it: the race director's events start it; the test scene turns it
off, so that the director cuts only when told, as Moto's did before the race):

- the attract mode, from the race director's AttractStarted (a custom event of no parameters, which the race director
  broadcasts; the director's read-out Mode is 1): the trackside cameras follow the Subject, and the director cuts to the
  trackside camera nearest the point LEAD meters ahead of it (along its heading, on the level), at once and then whenever
  that is another camera and the one that shows has shown for Hold seconds (HOLD). Along the racing line the rule gives
  each camera one stretch of the lap (plan(): the ghost's time on the lap at which each camera's stretch starts), each
  longer than HOLD, so the director cuts round the cameras in the lap's order, one cut a camera;
- the race, from RaceStarted (Mode 2): the trackside cameras follow the Bike, and the director cuts to the chase camera,
  which every update stands CHASE_DISTANCE meters behind the bike on the level and CHASE_HEIGHT above it, looking at the
  point CHASE_AIM above the bike's origin. The bike's lean never moves it: its heading follows the bike's on the level,
  eased by CHASE_TURN a second (exponentially, so a frame's change is the same share of the turn whatever the frame
  rate), and it jumps to its place, the heading too, when it is cut to and when the bike moved more than CHASE_SNAP
  meters in a frame (placed elsewhere).

A cut makes one camera primary and its listener the one that hears (the others off): the scene renders from it, and the
sounds are heard where it stands (a cut is no doppler: the listener is another entity). The trackside camera cut to is
turned towards what the cameras follow, AIM meters above its origin, before it shows, and so is the trackside camera that
shows on every update (Face); the others stand as they were left. Active reads which camera shows: 0 the tour, 1 to 7 a
trackside camera, 8 the chase camera.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Bike  # noqa: E402  (the grid)
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
# How far above the followed bike's origin (on the road) the cameras aim: about the rider's middle.
AIM = 0.9
# Where the footbridge's camera stands before the bridge (meters), and how far along the lap a camera may move from its
# site to find room.
FOOTBRIDGE_LEAD = 35.0
MAX_MOVE = 20
# The attract mode's rule: how far ahead of what it follows the director looks for the nearest camera (meters), and how
# long a camera shows at least before the rule cuts away from it (seconds).
LEAD = 40.0
HOLD = 3.0
# The chase camera: its name, how far behind the bike and how high above it it rides, the point it looks at above the
# bike's origin (meters), how fast its heading follows the bike's (a second), how far the bike must move in a frame for the
# camera to jump with it (meters), and its field of view (degrees).
CHASE = "Chase Camera"
CHASE_DISTANCE = 6.5
CHASE_HEIGHT = 2.2
CHASE_AIM = 1.0
CHASE_TURN = 4.0
CHASE_SNAP = 20.0
CHASE_FOV = 60.0


def _corner(name):
    """A corner's apex (lap distance) and its inside: +1 right of travel, -1 left."""
    _, start, end, side = next(row for row in Circuit.corner_ranges() if row[0].name == name)
    return start + 0.5 * (end - start), side


def _line_side(s):
    """The side of the center line the racing line is on at a lap distance: +1 right of travel, -1 left."""
    point = min(RacingLine.LINE, key=lambda candidate: abs(math.remainder(candidate.s - s, Circuit.LENGTH)))
    return 1 if point.lateral >= 0.0 else -1


def sites():
    """The trackside cameras before placing, in the order of the lap from the start line: (name, lap distance, side)."""
    footbridge = Circuit.FOOTBRIDGE - FOOTBRIDGE_LEAD
    corners = {name: _corner(name) for name in ("Turn 1", "Turn 3", "Turn 6", "Turn 7", "Turn 9")}
    return (("Start Line Camera", Circuit.LENGTH - 80.0, -1),
            ("Turn 1 Camera",) + corners["Turn 1"],
            ("Turn 3 Camera",) + corners["Turn 3"],
            ("Footbridge Camera", footbridge, _line_side(footbridge)),
            ("Turn 6 Camera",) + corners["Turn 6"],
            ("Turn 7 Camera",) + corners["Turn 7"],
            ("Turn 9 Camera",) + corners["Turn 9"])


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
    """The trackside cameras: (name, world position), each at the lap distance nearest its site (a meter at a time,
    ahead first) where no prop stands in its way."""
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


# The chase camera's index for StartCamera and Active: the trackside cameras' count and one.
CHASE_INDEX = len(sites()) + 1
# A time step of the plan (seconds).
PLAN_STEP = 0.1


def nearest(point, forward, positions):
    """The rule's camera (1 to the trackside cameras' count) for something at a point heading along a direction: the
    camera nearest the point LEAD meters ahead of it on the level."""
    length = math.hypot(forward[0], forward[2])
    ahead = (point[0] + forward[0] / length * LEAD, point[1], point[2] + forward[2] / length * LEAD)
    return min(range(len(positions)), key=lambda k: math.dist(ahead, positions[k])) + 1


def plan(placed=None):
    """The attract mode's cuts as the ghost rides the racing line: [(its time on the lap at which a camera's stretch
    starts, the camera's number)], the first at 0; read every PLAN_STEP seconds."""
    positions = [position for _, position in (placed if placed is not None else placing())]
    cuts = []
    lap = RacingLine.length()
    for step in range(int(lap / PLAN_STEP) + 1):
        time = step * PLAN_STEP
        camera = nearest(RacingLine.pose(time)[0], RacingLine.velocity(time), positions)
        if not cuts or cuts[-1][1] != camera:
            cuts.append((round(time, 3), camera))
    return cuts


def check(placed=None):
    """Asserts that the rule gives every trackside camera one stretch of the lap, in the lap's order, each longer than
    HOLD with a second to spare (the stretch over the start and finish line counts once); returns the figures."""
    placed = placed if placed is not None else placing()
    cuts = plan(placed)
    lap = RacingLine.length()
    stretches = [(camera, (cuts[k + 1][0] if k + 1 < len(cuts) else lap) - start) for k, (start, camera) in enumerate(cuts)]
    if len(cuts) > 1 and cuts[0][1] == cuts[-1][1]:
        stretches = [(stretches[0][0], stretches[0][1] + stretches[-1][1])] + stretches[1:-1]
    order = [camera for camera, _ in stretches]
    assert sorted(order) == list(range(1, len(placed) + 1)), f"the rule's cameras round the lap are {order}"
    assert order == sorted(order, key=lambda camera: (camera - order[0]) % len(placed)), f"the rule cuts out of order: {order}"
    shortest = min(seconds for _, seconds in stretches)
    assert shortest >= HOLD + 1.0, f"a camera shows only {shortest:.1f} s of the ghost's lap, under {HOLD + 1.0}"
    return (f"{len(placed)} trackside cameras; the attract mode cuts at "
            + ", ".join(f"{start:g} s to {placed[camera - 1][0]}" for start, camera in cuts)
            + f" of the ghost's lap, the shortest stretch {shortest:.1f} s")


def camera(position):
    """A trackside camera's components where it stands, unturned until the director's first update turns it."""
    return {"Transform": {"Translation": list(position)},
            "Camera": {"PerspectiveFOV": FOV, "PerspectiveNear": 0.1, "PerspectiveFar": 5000.0, "Primary": False},
            "AudioListener": {"Active": False}}


def chase_place():
    """Where the chase view puts its camera behind the grid bike: (eye, target), world points."""
    translation, yaw = Bike.grid_place()
    heading = math.radians(yaw)
    forward = (-math.sin(heading), 0.0, -math.cos(heading))
    eye = tuple(translation[k] - forward[k] * CHASE_DISTANCE + (CHASE_HEIGHT if k == 1 else 0.0) for k in range(3))
    target = tuple(translation[k] + (CHASE_AIM if k == 1 else 0.0) for k in range(3))
    return eye, target


def chase_camera():
    """The chase camera's components: behind the grid bike, looking at it, until the director moves it."""
    eye, target = chase_place()
    dx, dy, dz = (target[k] - eye[k] for k in range(3))
    euler = [round(math.degrees(math.atan2(dy, math.hypot(dx, dz))), 4), round(math.degrees(math.atan2(-dx, -dz)), 4), 0.0]
    return {"Transform": {"Translation": [round(value, 4) for value in eye], "RotationEuler": euler},
            "Camera": {"PerspectiveFOV": CHASE_FOV, "PerspectiveNear": 0.1, "PerspectiveFar": 5000.0, "Primary": False},
            "AudioListener": {"Active": False}}


# --- The graph ---------------------------------------------------------------------------------------------------------

def declare(author):
    count = CHASE_INDEX - 1
    author.declare(CAMERA_DIRECTOR, "The camera director: it cuts between the tour's camera and its children, the trackside "
                   "cameras and the chase camera, on the NextCamera and ResetCamera actions (or the events of those names any "
                   "graph sends), by its own rule in the attract mode and to the chase view in the race (the race director's "
                   "AttractStarted and RaceStarted), and turns the trackside camera that shows to follow. Tools/Trackside.py "
                   "says how.", variables=(
        public("TourCamera", ENTITY, None, "The camera the tour flies: it shows and hears the scene when no other camera does"),
        public("Subject", ENTITY, None, "What the trackside cameras follow in the attract mode (the ghost)"),
        public("Bike", ENTITY, None, "The ridden bike: the chase camera follows it, and so do the trackside cameras in the race"),
        public("StartCamera", "int", 0, f"The camera that shows when the scene starts: 0 the tour, 1 to {count} a trackside camera, "
               f"{CHASE_INDEX} the chase camera (any other number is warned of, and the tour shows)", (0, CHASE_INDEX)),
        public("Active", "int", 0, f"The camera that shows: 0 the tour, 1 to {count} a trackside camera, {CHASE_INDEX} the chase "
               "camera (a read-out)"),
        public("Directs", "bool", True, "Whether the director cuts by itself once the race director starts the attract mode or "
               "the race; off, it cuts only when told (StartCamera, NextCamera and ResetCamera)"),
        public("Mode", "int", 0, "How the director cuts: 0 when told, 1 the attract mode (by its rule), 2 the race (the chase "
               "view) (a read-out)"),
        public("Hold", "float", HOLD, "How long a camera shows at least in the attract mode before the rule cuts away (seconds)",
               (0.0, 30.0)),
        private("Cameras", "Strada.Entity[]"), private("Followed", ENTITY), private("CutAt", "double", 0.0),
        private("ChaseHeading", "Strada.Vector3", [0.0, 0.0, -1.0]), private("BikeAt", "Strada.Vector3"),
    ), events=(("NextCamera", ()), ("ResetCamera", ()), ("AttractStarted", ()), ("RaceStarted", ())), functions=(
        ("Face", False, (("Camera", ENTITY),), (), ()),
        ("Chase", False, (("Snap", "bool"),), (), ()),
        ("Nearest", False, (), (("Index", "int"),), (("Best", "int", 1), ("Closest", "float", 1.0e9))),
        ("Cut", False, (("Index", "int"),), (), ()),
    ))


def level(forward):
    """An expression's direction on the level from a forward (v): its X and Z, of length 1."""
    return f"normalize(vec3({forward}.X, 0, {forward}.Z))"


def write(author):
    with author.region(CAMERA_DIRECTOR, "Face: a trackside camera turns towards what the cameras follow", EFFECTS, "Face") as b:
        followed = b.var("Followed")
        known = b.branch(b.pure("IsValid", Value=followed))
        target = b.expr(f"at + vec3(0, {AIM}, 0)", at=b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value",
                                                               Target=followed))
        eye = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=b.entry["Camera"])
        turn = b.call("Set:Strada.TransformComponent.EulerAngles", Target=b.entry["Camera"],
                      Value=b.expr(LOOK, target=target, eye=eye),
                      comment="Pitch, then yaw: the director stands at the origin unturned, so its children's angles are the world's")
        b.chain(b.entry, known)
        b.chain(known["True"], turn, b.ret)
        b.flow(known["False"], b.ret)
    with author.region(CAMERA_DIRECTOR, "Chase: the chase camera rides behind the bike, looking at it", EFFECTS, "Chase") as b:
        bike = b.var("Bike")
        known = b.branch(b.pure("IsValid", Value=bike))
        at = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=bike)
        forward = b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=bike)
        heading = b.var("ChaseHeading")
        # The bike's heading on the level (its lean turns its forward about itself, which changes nothing here); a bike
        # whose forward stands upright keeps the heading it had.
        flat = b.expr(f"length(vec2(f.X, f.Z)) > 0.01 ? {level('f')} : h", f=forward, h=heading)
        followed = b.set("ChaseHeading", b.expr(f"snap ? flat : normalize(h + (flat - h) * (1 - exp(-{CHASE_TURN} * dt)))",
                                                snap=b.entry["Snap"], flat=flat, h=heading,
                                                dt=b.pure("Get:Strada.Time.DeltaTime", "Value")),
                         comment="Eased towards the bike's: the same share of the turn a second at any frame rate")
        chase = b.add("Array.Get", None, "Element", Array=b.var("Cameras"),
                      Index=b.expr("count - 1", count=b.add("Array.Length", None, "Length", Array=b.var("Cameras")).out)).out
        eye = b.expr(f"at - h * {CHASE_DISTANCE} + vec3(0, {CHASE_HEIGHT}, 0)", at=at, h=b.var("ChaseHeading"))
        placed = b.call("Set:Strada.TransformComponent.Translation", Target=chase, Value=eye,
                        comment="The director stands at the origin unturned: its child's place is the world's")
        turned = b.call("Set:Strada.TransformComponent.EulerAngles", Target=chase,
                        Value=b.expr(LOOK, target=b.expr(f"at + vec3(0, {CHASE_AIM}, 0)", at=at), eye=eye))
        b.chain(b.entry, known)
        b.chain(known["True"], followed, placed, turned, b.ret)
        b.flow(known["False"], b.ret)
    with author.region(CAMERA_DIRECTOR, "Nearest: the trackside camera nearest the point ahead of what it follows", RULES,
                       "Nearest") as b:
        followed = b.var("Followed")
        known = b.branch(b.pure("IsValid", Value=followed))
        at = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=followed)
        forward = b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=followed)
        ahead = b.expr(f"length(vec2(f.X, f.Z)) > 0.01 ? at + {level('f')} * {LEAD} : at", at=at, f=forward)
        start = b.set("Best", b.var("Active"), comment="Nothing to follow: the camera that shows")
        reset = b.set("Closest", 1.0e9)
        each = b.add("Flow.ForEach", Array=b.var("Cameras"))
        count = b.add("Array.Length", None, "Length", Array=b.var("Cameras")).out
        distance = b.expr("distance(at, ahead)", at=b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value",
                                                             Target=each["Element"]), ahead=ahead)
        closer = b.branch(b.expr("index < count - 1 && d < closest", index=each["Index"], count=count, d=distance,
                                 closest=b.var("Closest")), comment="A trackside camera (the chase camera is the last child)")
        noted = b.set("Closest", distance)
        chosen = b.set("Best", b.expr("index + 1", index=each["Index"]))
        b.link(b.var("Best"), b.ret["Index"])
        b.chain(b.entry, start, known)
        b.chain(known["True"], reset, each)
        b.flow(known["False"], b.ret)
        b.chain(each["Body"], closer)
        b.chain(closer["True"], noted, chosen)
        b.flow(each["Completed"], b.ret)
    with author.region(CAMERA_DIRECTOR, "Cut: camera Index shows and hears the scene (0: the tour's camera)", RULES, "Cut") as b:
        count = b.add("Array.Length", None, "Length", Array=b.var("Cameras")).out
        # A number that names no camera (from the scene's StartCamera, or an agent's) is warned of, and the tour shows.
        known_index = b.branch(b.expr("index >= 0 && index <= count", index=b.entry["Index"], count=count))
        warn = b.call("Call:Strada.Log.Warn(string)", Message=b.fmt(
            "The camera director has no camera {Index} (0 is the tour's, 1 to {Count} the trackside cameras and the chase "
            "camera): the tour's shows", Index=b.entry["Index"], Count=count))
        cut = b.expr("index >= 0 && index <= count ? index : 0", index=b.entry["Index"], count=count)
        each = b.add("Flow.ForEach", Array=b.var("Cameras"))
        chosen = b.expr("index + 1 == cut", index=each["Index"], cut=cut)
        shows = b.call("Set:Strada.CameraComponent.Primary", Target=each["Element"], Value=chosen)
        hears = b.call("Set:Strada.AudioListenerComponent.Active", Target=each["Element"], Value=chosen)
        tour = b.var("TourCamera")
        known = b.branch(b.pure("IsValid", Value=tour))
        home = b.expr("cut == 0", cut=cut)
        tour_shows = b.call("Set:Strada.CameraComponent.Primary", Target=tour, Value=home)
        tour_hears = b.call("Set:Strada.AudioListenerComponent.Active", Target=tour, Value=home)
        active = b.set("Active", cut)
        timed = b.set("CutAt", b.pure("Get:Strada.Time.Elapsed", "Value"), comment="When it cut: the attract mode holds it")
        # The trackside camera cut to is turned to what the cameras follow before it shows, so a cut opens on it; the chase
        # camera is put in its place.
        trackside = b.branch(b.expr("cut > 0 && cut < count", cut=cut, count=count))
        face = b.own("Face", Camera=b.add("Array.Get", None, "Element", Array=b.var("Cameras"),
                                          Index=b.expr("cut - 1", cut=cut)).out)
        chasing = b.branch(b.expr("cut > 0 && cut == count", cut=cut, count=count))
        chase = b.own("Chase", Snap=True)
        b.chain(b.entry, known_index)
        b.flow(known_index["True"], each)
        b.chain(known_index["False"], warn, each)
        b.chain(each["Body"], shows, hears)
        b.chain(each["Completed"], known)
        b.chain(known["True"], tour_shows, tour_hears, active)
        b.flow(known["False"], active)
        b.chain(active, timed, trackside)
        b.chain(trackside["True"], face, b.ret)
        b.flow(trackside["False"], chasing)
        b.chain(chasing["True"], chase, b.ret)
        b.flow(chasing["False"], b.ret)
    with author.region(CAMERA_DIRECTOR, "Start: the cameras are the director's children; it cuts to StartCamera", EVENTS) as b:
        cameras = b.set("Cameras", b.pure("Get:Strada.Entity.Children", "Value", Target=b.self_()))
        b.chain(b.event("Event.Create"), cameras, b.set("Followed", b.var("Subject")), b.own("Cut", Index=b.var("StartCamera")))
    with author.region(CAMERA_DIRECTOR, "Actions: NextCamera and ResetCamera send the director its cuts", EVENTS) as b:
        b.chain(b.event("Event.ActionPressed", {"Action": "NextCamera"}), b.send("NextCamera", b.self_()))
        b.chain(b.event("Event.ActionPressed", {"Action": "ResetCamera"}), b.send("ResetCamera", b.self_()))
    with author.region(CAMERA_DIRECTOR, "Next camera: a cut to the next trackside camera, round to the first", RULES) as b:
        following = b.expr("active >= count - 1 ? 1 : active + 1", active=b.var("Active"),
                           count=b.add("Array.Length", None, "Length", Array=b.var("Cameras")).out)
        b.chain(b.custom("NextCamera"), b.own("Cut", Index=following))
    with author.region(CAMERA_DIRECTOR, "Reset camera: back to the tour, or in the race to the chase view", RULES) as b:
        home = b.expr("mode == 2 ? count : 0", mode=b.var("Mode"),
                      count=b.add("Array.Length", None, "Length", Array=b.var("Cameras")).out)
        b.chain(b.custom("ResetCamera"), b.own("Cut", Index=home))
    with author.region(CAMERA_DIRECTOR, "Attract mode: the rule's cuts follow the subject round the lap", RULES) as b:
        directs = b.branch(b.var("Directs"), comment="Off: the director cuts only when told")
        nearest = b.own("Nearest", output="Index")
        b.chain(b.custom("AttractStarted"), directs)
        b.chain(directs["True"], b.set("Mode", 1), b.set("Followed", b.var("Subject")), nearest, b.own("Cut", Index=nearest.out))
    with author.region(CAMERA_DIRECTOR, "Race: the chase view follows the ridden bike", RULES) as b:
        directs = b.branch(b.var("Directs"), comment="Off: the director cuts only when told")
        bike = b.branch(b.pure("IsValid", Value=b.var("Bike")))
        mode = b.set("Mode", 2)
        follows_bike = b.set("Followed", b.var("Bike"))
        follows_subject = b.set("Followed", b.var("Subject"), comment="No bike: the trackside cameras follow the subject still")
        chase = b.own("Cut", Index=b.add("Array.Length", None, "Length", Array=b.var("Cameras")).out)
        b.chain(b.custom("RaceStarted"), directs)
        b.chain(directs["True"], mode, bike)
        b.chain(bike["True"], follows_bike, chase)
        b.chain(bike["False"], follows_subject, chase)
    with author.region(CAMERA_DIRECTOR, "Update: the rule's cut, the trackside camera's turn, the chase", EFFECTS) as b:
        steps = b.sequence(3)
        now = b.pure("Get:Strada.Time.Elapsed", "Value")
        due = b.branch(b.expr("mode == 1 && now - at >= hold", mode=b.var("Mode"), now=now, at=b.var("CutAt"), hold=b.var("Hold")),
                       comment="The attract mode, the camera shown for Hold seconds")
        nearest = b.own("Nearest", output="Index")
        other = b.branch(b.expr("best != active", best=nearest.out, active=b.var("Active")))
        rule = b.own("Cut", Index=nearest.out)
        active = b.var("Active")
        count = b.add("Array.Length", None, "Length", Array=b.var("Cameras")).out
        trackside = b.branch(b.expr("active > 0 && active < count", active=active, count=count),
                             comment="The tour's camera turns itself")
        face = b.own("Face", Camera=b.add("Array.Get", None, "Element", Array=b.var("Cameras"),
                                          Index=b.expr("active - 1", active=active)).out)
        bike = b.var("Bike")
        known = b.branch(b.pure("IsValid", Value=bike))
        at = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=bike)
        chasing = b.branch(b.expr("active > 0 && active == count", active=active, count=count))
        chase = b.own("Chase", Snap=b.expr(f"distance(at, last) > {CHASE_SNAP}", at=at, last=b.var("BikeAt")),
                      comment="Placed elsewhere: the camera jumps with it")
        moved = b.set("BikeAt", at)
        b.chain(b.event("Event.Update"), steps)
        b.flow(steps["Then0"], due)
        b.chain(due["True"], nearest, other)
        b.flow(other["True"], rule)
        b.flow(steps["Then1"], trackside)
        b.flow(trackside["True"], face)
        b.flow(steps["Then2"], known)
        b.chain(known["True"], chasing)
        b.chain(chasing["True"], chase, moved)
        b.flow(chasing["False"], moved)


# The camera's Euler angles (degrees: pitch, then yaw, no roll) that turn its forward (-Z) from the eye towards the target,
# as the tour turns its camera.
LOOK = ("vec3(degrees(atan2(target.Y - eye.Y, length(vec2(target.X - eye.X, target.Z - eye.Z)))), "
        "degrees(atan2(eye.X - target.X, eye.Z - target.Z)), 0)")


if __name__ == "__main__":
    for name, position in placing():
        print(f"{name}: {position}")
    print(check())

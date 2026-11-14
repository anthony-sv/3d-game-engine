"""Moto's test scene (Scenes/Tests.sscene) and its checks, all of them graphs: the main scene as it is, with a crate to
drop on the road, a second entity running the tour that waits to be sent it (the Waiting Tour) and the Tests entity,
whose graph checks the circuit's surfaces and the camera tour one after another, then reports to the test run.

- Checks.sgraph, a function library: reporting a check, and comparing points and directions.
- Tests.sgraph (the Tests entity): each check a latent function that acts or waits for its moment, then reports through
  Checks; the event graph runs them in turn from a Sequence and finishes the run.

The circuit's checks cast rays down at points the layout (Circuit.py) gives and require the surface hit, its height and
that it faces up; a crate dropped on the road must come to rest on it. The tour's checks compare the camera with the path
Tour.py made: at the home view when the scene starts, under the footbridge when the path passes it, home again when the
tour ends, and off on the next tour after it. The Waiting Tour checks the tour's interface for other graphs: with
PlayOnStart off it stays where the scene put it until the Tests graph sends it PlayTour, then it flies, and with Repeat
off it stays at the home view after its one tour.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Circuit  # noqa: E402  (the layout)
import Graphs  # noqa: E402  (writing graphs)
import Tour  # noqa: E402  (the camera tour)
from Graphs import CHECK, ENTITY, private, public  # noqa: E402

CHECKS = Graphs.script("Checks")
TESTS = Graphs.script("Tests")
GRAPHS = (CHECKS, TESTS)
LIBRARIES = (CHECKS,)
VECTOR = "Strada.Vector3"
RAYCAST = "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)"

# The crate dropped on the road (half its size, meters: builtin://Cube is a 1 m cube, and its collider is as large) and
# where it falls from: over the main straight, right of the camera's path.
CRATE_HALF = 0.5
CRATE_AT = 300.0
CRATE_LATERAL = 4.0
CRATE_DROP = 1.0
# How close a surface hit must come to its expected height (meters).
SURFACE_TOLERANCE = 0.03
# Where the Waiting Tour stands until it is sent PlayTour: 20 m over the home view, where its tour starts.
WAITING_SPOT = (Tour.HOME_EYE[0], Tour.HOME_EYE[1] + 20.0, Tour.HOME_EYE[2])


def crate_start():
    """Where the crate's center starts: CRATE_DROP over the road."""
    return Circuit.offset_point(CRATE_AT, CRATE_LATERAL, Circuit.road_height(CRATE_LATERAL) + CRATE_HALF + CRATE_DROP)


def probes():
    """The surfaces the rays find: (function, title of its box, the check's name, the scene entity, where the ray starts
    (world, 3 m above the surface), the surface's height)."""
    apex = next(start + 0.5 * (end - start) for corner, start, end, side in Circuit.corner_ranges() if corner.name == "Turn 6")
    inside = 1 if Circuit.curvature(apex) < 0.0 else -1
    kerb = (Circuit.HALF_WIDTH + Circuit.KERB_WIDTH - 0.3) * inside
    face = Circuit.barrier_offset(Circuit.FOOTBRIDGE + 30.0, "right")
    bridge = Circuit.offset_point(Circuit.FOOTBRIDGE, 0.0)
    deck = Circuit.BRIDGE_CLEARANCE + 0.5
    rows = (
        ("CheckRoad", "Road: the main straight's crown, 2 m right of the line", "the road on the main straight is the track's surface",
         "Track", Circuit.offset_point(300.0, 2.0), Circuit.road_height(2.0)),
        ("CheckKerb", "Kerb: raised on the inside of Turn 6", "a kerb stands on the inside of Turn 6", "Track",
         Circuit.offset_point(apex, kerb), Circuit.ROAD_HEIGHT + Circuit.KERB_HEIGHT),
        ("CheckBarrier", "Barrier: its top, beside the back straight", "the barrier stands 0.85 m high beside the back straight",
         "Barriers", Circuit.offset_point(Circuit.FOOTBRIDGE + 30.0, face + 0.3), Circuit.BARRIER_HEIGHT),
        ("CheckGround", "Ground: the grass beyond the barrier", "the ground lies beyond the barrier", "Ground",
         Circuit.offset_point(Circuit.FOOTBRIDGE + 30.0, face + 6.0), 0.0),
        ("CheckFootbridge", "Footbridge: its deck over the back straight", "the footbridge's deck spans the back straight 6.5 m up",
         "Footbridge", (bridge[0], 0.0, bridge[2]), deck),
    )
    return tuple((function, title, name, entity, [round(point[0], 3), round(height + 3.0, 3), round(point[2], 3)], round(height, 4))
                 for function, title, name, entity, point, height in rows)


# The checks in the order they run: (function, title of its box, the check's name in the report). The surface checks come
# from probes(); the others' bodies are written by their writers in WRITERS.
def checks():
    tour_start = (("CheckTourStartsHome", "Tour start: the camera at the home view, looking down the straight",
                   "the tour starts at the home view"),
                  ("CheckTourWaits", "Waiting tour: with PlayOnStart off it stays put, then is sent PlayTour",
                   "a tour with PlayOnStart off waits until it is sent PlayTour"))
    surfaces = tuple((function, title, name) for function, title, name, _, _, _ in probes())
    later = (
        ("CheckCrateRests", "Crate: dropped on the road, it comes to rest on it", "a crate dropped on the road comes to rest on it"),
        ("CheckUnderFootbridge", "Footbridge pass: the camera where the tour puts it, under the deck",
         "the tour flies the camera under the footbridge"),
        ("CheckSentTourFlies", "Sent tour: PlayTour from another graph flies it", "a tour sent PlayTour by another graph flies"),
        ("CheckTourEndsHome", "Tour end: the camera back at the home view", "the tour ends at the home view"),
        ("CheckTourRepeats", "Repeat: the next tour takes off", "the tour takes off again"),
        ("CheckSentTourStops", "Repeat off: the sent tour stays at the home view after one tour",
         "a tour with Repeat off stays at the home view after one tour"),
    )
    return tour_start + surfaces + later


# The entities the checks look at, by the names of Tests.sgraph's variables: (variable, the scene entity's name, tooltip).
TEST_ENTITIES = (
    ("Camera", "Main Camera", "The camera the tour flies"),
    ("Track", "Track", "The circuit's road, kerbs, verges and run-off"),
    ("Barriers", "Barriers", "The concrete walls along the circuit"),
    ("Ground", "Ground", "The grass the circuit lies on"),
    ("Footbridge", "Footbridge", "The footbridge over the back straight"),
    ("Crate", "Probe Crate", "The crate dropped on the road"),
    ("WaitingTour", "Waiting Tour", "Runs the tour with PlayOnStart and Repeat off; the checks send it PlayTour"),
)


# --- The interfaces ---------------------------------------------------------------------------------------------------------

def declare(author):
    author.declare(CHECKS, "What the test scene's checks share: reporting a check, and comparing points and directions.",
                   functions=(
                       ("Check", False, (("Name", "string"), ("Passed", "bool"), ("Detail", "string")), (), ()),
                       ("NearVector", True, (("A", VECTOR), ("B", VECTOR), ("Tolerance", "float")), (("Result", "bool"),), ()),
                       ("Facing", True, (("Direction", VECTOR), ("Expected", VECTOR), ("Degrees", "float")), (("Result", "bool"),), ()),
                   ))
    author.declare(TESTS, f"The test scene's checks: {len(checks())} checks of Moto's circuit and its camera tour, one after "
                   "another, each a latent function that acts or waits for its moment, then reports it.", variables=(
        tuple(public(name, ENTITY, None, tooltip) for name, _, tooltip in TEST_ENTITIES) + (private("StepStart", "double", 0.0),)),
        functions=(("Probe", False, (("From", VECTOR), ("Surface", ENTITY), ("Height", "float")),
                    (("Passed", "bool"), ("Detail", "string")), ()),) + tuple((name, False, (), (), ()) for name, _, _ in checks()))


# --- Checks.sgraph -----------------------------------------------------------------------------------------------------------

def write_checks(author):
    with author.region(CHECKS, "Check: report a pass, or a failure with its detail", CHECK, "Check") as b:
        passed = b.branch(b.entry["Passed"])
        report_pass = b.call("Call:Strada.Testing.TestReporter.Pass(string)", Name=b.entry["Name"])
        report_fail = b.call("Call:Strada.Testing.TestReporter.Fail(string,string)", Name=b.entry["Name"], Message=b.entry["Detail"])
        b.chain(b.entry, passed)
        b.chain(passed["True"], report_pass, b.ret)
        b.chain(passed["False"], report_fail, b.ret)
    with author.region(CHECKS, "Near vector: two points within a tolerance", CHECK, "NearVector") as b:
        b.link(b.expr("distance(a, b) <= tolerance", a=b.entry["A"], b=b.entry["B"], tolerance=b.entry["Tolerance"]), b.ret["Result"])
    with author.region(CHECKS, "Facing: two directions within an angle", CHECK, "Facing") as b:
        b.link(b.expr("dot(normalize(direction), normalize(expected)) >= cos(radians(angle))", direction=b.entry["Direction"],
                      expected=b.entry["Expected"], angle=b.entry["Degrees"]), b.ret["Result"])


# --- Tests.sgraph ------------------------------------------------------------------------------------------------------------

def now(b):
    return b.once("now", lambda: b.pure("Get:Strada.Time.Elapsed", "Value"))


def wait_until(b, done, limit):
    """Marks the step's start and waits until done holds or limit seconds have passed (without done: the seconds), so a
    check that never comes true fails instead of hanging; returns (first, last) of the chain."""
    mark = b.set("StepStart", now(b))
    inputs = {"now": now(b), "start": b.var("StepStart")}
    if done is None:
        condition = f"now - start > {limit}"
    else:
        condition = f"done || now - start > {limit}"
        inputs["done"] = done
    wait = b.add("Flow.WaitUntil", Condition=b.expr(condition, **inputs))
    b.flow(mark, wait)
    return mark, wait["Completed"]


def check(b, name, done, limit, detail, passed=None):
    """Waits for done (at most limit seconds), then reports the check; passed defaults to done."""
    first, waited = wait_until(b, done, limit)
    reported = b.library(CHECKS, "Check", Name=f"Moto: {name}", Passed=done if passed is None else passed, Detail=detail)
    b.flow(waited, reported)
    return first, reported


def run(b, *steps):
    """Runs a function's steps in order, from its entry to its return: each a node, or (first, last) of a chain."""
    previous = b.entry
    for step in steps:
        first, last = step if isinstance(step, tuple) else (step, step)
        b.flow(previous, first)
        previous = last
    b.flow(previous, b.ret)


def camera_at(b):
    return b.once("camera at", lambda: b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=b.var("Camera")))


def camera_forward(b):
    return b.once("camera forward", lambda: b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=b.var("Camera")))


def tours(b, entity="Camera"):
    """The tour's read-out of the tours that came home on an entity (a remote Get of its graph): the camera's, or another
    of TEST_ENTITIES that runs the tour."""
    return b.once(("tours", entity), lambda: b.get("Tours", Tour.TOUR, b.var(entity)))


def waiting_at(b):
    return b.once("waiting at", lambda: b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=b.var("WaitingTour")))


def waiting_detail(b):
    return b.fmt("the Waiting Tour at {At} after {Tours} tours", At=waiting_at(b), Tours=tours(b, "WaitingTour"))


def near(b, a, expected, tolerance):
    return b.library(CHECKS, "NearVector", output="Result", A=a, B=expected, Tolerance=tolerance).out


def facing(b, direction, expected, degrees):
    return b.library(CHECKS, "Facing", output="Result", Direction=direction, Expected=expected, Degrees=degrees).out


def rounded(vector):
    return [round(value, 3) for value in vector]


def home_direction():
    return rounded(Tour._normalize([Tour.HOME_TARGET[k] - Tour.HOME_EYE[k] for k in range(3)]))


def view_detail(b):
    return b.fmt("the camera at {At}, looking along {Forward}", At=camera_at(b), Forward=camera_forward(b))


def write_tests(author):
    order = checks()
    with author.region(TESTS, "Start: every check in turn, then the report", CHECK) as b:
        start = b.event("Event.Create")
        settle = b.add("Flow.WaitFrames", Frames=1, comment="Every entity has run its Create first")
        steps = b.sequence(len(order) + 1)
        b.chain(start, settle)
        b.flow(settle["Completed"], steps)
        for index, (name, _, _) in enumerate(order):
            b.flow(steps[f"Then{index}"], b.own(name))
        b.flow(steps[f"Then{len(order)}"], b.call("Call:Strada.Testing.TestReporter.Finish()"))

    with author.region(TESTS, "Probe: the surface a ray down from a point hits, at its height", CHECK, "Probe") as b:
        ray = b.call(RAYCAST, Origin=b.entry["From"], Direction=[0.0, -1.0, 0.0], MaxDistance=20.0)
        hit = ray["Hit"]
        surface = b.pure("Get:Strada.RaycastHit.Entity", "Value", Target=hit)
        point = b.pure("Get:Strada.RaycastHit.Point", "Value", Target=hit)
        normal = b.pure("Get:Strada.RaycastHit.Normal", "Value", Target=hit)
        passed = b.expr(f"hit && same && abs(point.Y - height) <= {SURFACE_TOLERANCE} && normal.Y > 0.95", hit=ray["Result"],
                        same=b.equal(surface, b.entry["Surface"]), point=point, height=b.entry["Height"], normal=normal)
        b.link(passed, b.ret["Passed"])
        b.link(b.fmt("the ray hit {Surface} at {Point}, facing {Normal}", Surface=surface, Point=point, Normal=normal), b.ret["Detail"])
        b.chain(b.entry, ray, b.ret)

    for function, title, name in order:
        with author.region(TESTS, title, CHECK, function) as b:
            WRITERS.get(function, check_surface)(b, name)


def check_tour_starts_home(b, name):
    done = b.both(near(b, camera_at(b), rounded(Tour.HOME_EYE), 0.1), facing(b, camera_forward(b), home_direction(), 1.0))
    run(b, check(b, name, done, 1, view_detail(b)))


def check_surface(b, name):
    _, _, _, entity, start, height = next(row for row in probes() if row[2] == name)
    probe = b.own("Probe", From=start, Surface=b.var(entity), Height=height)
    reported = b.library(CHECKS, "Check", Name=f"Moto: {name}", Passed=probe["Passed"], Detail=probe["Detail"])
    run(b, probe, reported)


def check_crate_rests(b, name):
    crate = b.var("Crate")
    at = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=crate)
    speed = b.expr("length(v)", v=b.pure("Get:Strada.RigidBodyComponent.LinearVelocity", "Value", Target=crate))
    start = crate_start()
    # Jolt lets resting bodies sink up to its penetration slop (0.02 m, PhysicsSettings::mPenetrationSlop) into what they
    # rest on, and where in it they settle depends on the drop: 0.05 m leaves room for that and a little sliding.
    resting = [start[0], Circuit.road_height(CRATE_LATERAL) + CRATE_HALF, start[2]]
    settled = b.expr("now > 1 && speed < 0.02", now=now(b), speed=speed)
    passed = b.both(settled, near(b, at, rounded(resting), 0.05))
    run(b, check(b, name, settled, 6, b.fmt("the crate at {At}, moving at {Speed} m/s", At=at, Speed=speed), passed))


def check_under_footbridge(b, name):
    time, eye, look, speed = Tour.under_footbridge()
    # The camera is read in the first frame at or after the moment, up to a frame of the slowest test run (1/30 s) late, and
    # the tour's time may run a frame behind the scene's: three such frames of flight.
    tolerance = round(speed * 3.0 / 30.0 + 0.5, 1)
    arrived = b.add("Flow.WaitUntil", Condition=b.expr(f"now >= {time}", now=now(b)))
    under = b.expr(f"at.Y < {Circuit.BRIDGE_CLEARANCE}", at=camera_at(b))
    done = b.both(b.both(near(b, camera_at(b), rounded(eye), tolerance), facing(b, camera_forward(b), rounded(look), 10.0)), under)
    run(b, (arrived, arrived["Completed"]), check(b, name, done, 0, view_detail(b)))


def check_tour_ends_home(b, name):
    ended = b.expr("tours >= 1", tours=tours(b))
    done = b.both(ended, near(b, camera_at(b), rounded(Tour.HOME_EYE), 0.1))
    run(b, check(b, name, ended, math.ceil(Tour.LENGTH) + 5, b.fmt("{Tours} tours; {View}", Tours=tours(b), View=view_detail(b)), done))


def check_tour_repeats(b, name):
    away = b.expr("distance(at, home) > 5", at=camera_at(b), home=b.lit(VECTOR, rounded(Tour.HOME_EYE)))
    done = b.both(away, b.expr("tours == 1", tours=tours(b)))
    run(b, wait_until(b, None, 3), check(b, name, done, 0, b.fmt("{Tours} tours; {View}", Tours=tours(b), View=view_detail(b))))


def check_tour_waits(b, name):
    # The entity stands 20 m from the home view, where a tour starts: had it taken off with the scene, half a second of its
    # tour would have put it within a meter of the home view. (In the first frames a tour sent at the start has not run
    # its first update yet.)
    done = b.both(near(b, waiting_at(b), rounded(WAITING_SPOT), 0.01), b.expr("tours == 0", tours=tours(b, "WaitingTour")))
    send = b.send("PlayTour", b.var("WaitingTour"), graph=Tour.TOUR, comment="Its tour takes off now, half a second after the camera's")
    run(b, wait_until(b, None, 0.5), check(b, name, done, 0, waiting_detail(b)), send)


def check_sent_tour_flies(b, name):
    # Halfway round, a tour is hundreds of meters from the home view and from the Waiting Tour's spot.
    away = b.expr("distance(at, home) > 100 && distance(at, spot) > 100", at=waiting_at(b), home=b.lit(VECTOR, rounded(Tour.HOME_EYE)),
                  spot=b.lit(VECTOR, rounded(WAITING_SPOT)))
    done = b.both(away, b.expr("tours == 0", tours=tours(b, "WaitingTour")))
    run(b, check(b, name, done, 0, waiting_detail(b)))


def check_sent_tour_stops(b, name):
    # It was sent PlayTour half a second after the camera took off, so its tour ended about 2.5 s ago (CheckTourRepeats
    # waited 3 s after the camera's): one that took off again would be about 20 m from the home view by now.
    ended = b.expr("tours >= 1", tours=tours(b, "WaitingTour"))
    done = b.both(b.expr("tours == 1", tours=tours(b, "WaitingTour")), near(b, waiting_at(b), rounded(Tour.HOME_EYE), 0.1))
    run(b, check(b, name, ended, 5, waiting_detail(b), done))


WRITERS = {
    "CheckTourStartsHome": check_tour_starts_home,
    "CheckTourWaits": check_tour_waits,
    "CheckSentTourFlies": check_sent_tour_flies,
    "CheckSentTourStops": check_sent_tour_stops,
    "CheckCrateRests": check_crate_rests,
    "CheckUnderFootbridge": check_under_footbridge,
    "CheckTourEndsHome": check_tour_ends_home,
    "CheckTourRepeats": check_tour_repeats,
}


def write(author):
    write_checks(author)
    author.finish(CHECKS)
    write_tests(author)
    author.finish(TESTS)


# --- The scene ---------------------------------------------------------------------------------------------------------------

def entity_value(entity):
    return {"Type": "Entity", "Value": entity}


def build_scene(session, handles):
    """The test scene: the main scene saved as Tests (its entities keep their IDs), the crate over the road, the Waiting
    Tour and the entity whose graph runs the checks. Leaves the main scene open."""
    session.call("scene_open", {"path": "Scenes/Main.sscene"})
    session.call("scene_save", {"path": "Scenes/Tests.sscene"})
    session.call("scene_settings", {"name": "Tests"})
    session.call("entity_create", {"name": "Probe Crate", "components": {
        "Transform": {"Translation": list(crate_start()), "RotationEuler": [0.0, Circuit.yaw(CRATE_AT), 0.0]},
        "Mesh": {"Mesh": "builtin://Cube"},
        "RigidBody": {"Type": "Dynamic", "Mass": 20.0, "LinearDamping": 0.5, "AngularDamping": 0.5},
        "BoxCollider": {"HalfExtents": [CRATE_HALF] * 3, "Friction": 0.8}}})
    session.call("entity_create", {"name": "Waiting Tour", "components": {
        "Transform": {"Translation": list(WAITING_SPOT)},
        "VisualScript": {"Graph": handles[Tour.TOUR], "Variables": {
            "PlayOnStart": {"Type": "Bool", "Value": False}, "Repeat": {"Type": "Bool", "Value": False}}}}})
    found = {}

    def walk(entities):
        for entity in entities:
            found.setdefault(entity["name"], entity["id"])
            walk(entity.get("children", []))

    walk(session.call("scene_hierarchy")["entities"])
    missing = [scene_name for _, scene_name, _ in TEST_ENTITIES if scene_name not in found]
    if missing:
        raise SystemExit(f"the test scene has no {', '.join(missing)}")
    session.call("entity_create", {"name": "Tests", "components": {"VisualScript": {"Graph": handles[TESTS], "Variables": {
        name: entity_value(found[scene_name]) for name, scene_name, _ in TEST_ENTITIES}}}})
    session.call("scene_save")
    session.call("scene_open", {"path": "Scenes/Main.sscene"})

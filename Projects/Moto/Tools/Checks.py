"""Moto's test scene (Scenes/Tests.sscene) and its checks, all of them graphs: the main scene as it is, with a crate to
drop on the road, two blocks that slide off along the asphalt and along a gravel trap, a second entity running the tour
that waits to be sent it (the Waiting Tour) and the Tests entity, whose graph checks the circuit's surfaces and the camera
tour one after another, then reports to the test run. Its own entities keep their IDs when it is made again.

- Checks.sgraph, a function library: reporting a check, and comparing points and directions.
- Tests.sgraph (the Tests entity): each check a latent function that acts or waits for its moment, then reports through
  Checks; the event graph runs them in turn from a Sequence and finishes the run.

The circuit's checks cast rays down at points the layout (Circuit.py) gives and require the surface hit, its height and
that it faces up, and the physics material it has (its Surface name: asphalt, also where a timing line is painted,
kerb, gravel, grass on the verges and the ground); a crate dropped on the road must come to rest on it, and of two blocks
sent off at one speed the one on the asphalt slides farther than the one in the gravel, each as far as its friction
says. The tour's checks compare the camera with the path
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
# The sliding blocks (builtin://Cube, its collider as large, rotation locked, friction 1.0): sent off at SLIDE_SPEED along
# the lap at a lap distance and a lateral offset, they stop after v^2 / (2 g mu), mu the friction their contact combines
# (the asphalt's, by the geometric mean; the gravel's, which wins by Maximum: Author.SURFACE_MATERIALS).
SLIDE_SPEED = 8.0
GRAVITY = 9.81
SLIDE_TOLERANCE = 0.1
ASPHALT_SLIDE = (360.0, -3.0)
ASPHALT_FRICTION = 1.0
GRAVEL_FRICTION = 1.5
# Where the Waiting Tour stands until it is sent PlayTour: 20 m over the home view, where its tour starts.
WAITING_SPOT = (Tour.HOME_EYE[0], Tour.HOME_EYE[1] + 20.0, Tour.HOME_EYE[2])


def crate_start():
    """Where the crate's center starts: CRATE_DROP over the road."""
    return Circuit.offset_point(CRATE_AT, CRATE_LATERAL, Circuit.road_height(CRATE_LATERAL) + CRATE_HALF + CRATE_DROP)


def turn6():
    """Turn 6's apex (lap distance) and the side its inside lies on (1 right of travel, -1 left)."""
    apex = next(start + 0.5 * (end - start) for corner, start, end, side in Circuit.corner_ranges() if corner.name == "Turn 6")
    return apex, 1 if Circuit.curvature(apex) < 0.0 else -1


def gravel_spot(s):
    """The middle of the gravel trap across the lap at s (Turn 6's outside), as (lateral offset, the gravel's height
    there): the trap's surface falls in a straight line from the verge's edge to the trap's outer edge."""
    _, inside = turn6()
    side = "left" if inside > 0 else "right"
    width = Circuit.gravel(s, side)
    inner = Circuit.HALF_WIDTH + Circuit.KERB_WIDTH + Circuit.VERGE_WIDTH
    outer_height = Circuit.VERGE_HEIGHT + (Circuit.GRAVEL_HEIGHT - Circuit.VERGE_HEIGHT) * min(1.0, width)
    height = Circuit.VERGE_HEIGHT + (outer_height - Circuit.VERGE_HEIGHT) * 0.5
    return -inside * (inner + 0.5 * width), height


def gravel_slide():
    """Where the gravel's block starts: 30 m before Turn 6's apex, in the middle of the trap."""
    apex, _ = turn6()
    return apex - 30.0


def slider_start(s, lateral, surface):
    """A sliding block's center at rest on the surface at s and the lateral offset, and the direction of travel there."""
    return Circuit.offset_point(s, lateral, surface + CRATE_HALF), Circuit.frame(s)[1]


def sliders():
    """The two sliding blocks: (scene entity, variable, start, direction, friction)."""
    asphalt_s, asphalt_lateral = ASPHALT_SLIDE
    gravel_s = gravel_slide()
    gravel_lateral, gravel_height = gravel_spot(gravel_s)
    asphalt = slider_start(asphalt_s, asphalt_lateral, Circuit.road_height(asphalt_lateral))
    gravel = slider_start(gravel_s, gravel_lateral, gravel_height)
    return (("Asphalt Slider", "AsphaltSlider") + asphalt + (ASPHALT_FRICTION,),
            ("Gravel Slider", "GravelSlider") + gravel + (GRAVEL_FRICTION,))


def stopping_distance(friction):
    return SLIDE_SPEED * SLIDE_SPEED / (2.0 * GRAVITY * friction)


def probes():
    """The surfaces the rays find: (function, title of its box, the check's name, the scene entity, where the ray starts
    (world, 3 m above the surface), the surface's height)."""
    apex, inside = turn6()
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


def surface_probes():
    """The physics materials the rays find: (function, title of its box, the check's name, the expected Surface, the rays'
    starts (world, 3 m above the surface))."""
    apex, inside = turn6()
    kerb = (Circuit.HALF_WIDTH + Circuit.KERB_WIDTH - 0.3) * inside
    gravel_lateral, _ = gravel_spot(apex)
    verge = Circuit.HALF_WIDTH + Circuit.KERB_WIDTH + 1.5
    face = Circuit.barrier_offset(Circuit.FOOTBRIDGE + 30.0, "right")
    rows = (
        ("CheckAsphaltSurface", "Asphalt: the road, and where the first sector line is painted", "the road's surface is asphalt",
         "Asphalt", (Circuit.offset_point(300.0, 2.0), Circuit.offset_point(Circuit.SECTOR_GATES[0], 1.0))),
        ("CheckKerbSurface", "Kerb: the kerb on the inside of Turn 6", "the kerb's surface is kerb", "Kerb",
         (Circuit.offset_point(apex, kerb),)),
        ("CheckGravelSurface", "Gravel: the trap on the outside of Turn 6", "the gravel trap's surface is gravel", "Gravel",
         (Circuit.offset_point(apex, gravel_lateral),)),
        ("CheckGrassSurface", "Grass: a verge, and the ground beyond the barrier", "the verges and the ground are grass", "Grass",
         (Circuit.offset_point(Circuit.FOOTBRIDGE - 40.0, -verge), Circuit.offset_point(Circuit.FOOTBRIDGE + 30.0, face + 6.0))),
    )
    return tuple((function, title, name, surface, tuple([round(point[0], 3), 3.0, round(point[2], 3)] for point in points))
                 for function, title, name, surface, points in rows)


# The checks in the order they run: (function, title of its box, the check's name in the report). The surface checks come
# from probes() and surface_probes(); the others' bodies are written by their writers in WRITERS.
def checks():
    tour_start = (("CheckTourStartsHome", "Tour start: the camera at the home view, looking down the straight",
                   "the tour starts at the home view"),
                  ("CheckTourWaits", "Waiting tour: with PlayOnStart off it stays put, then is sent PlayTour",
                   "a tour with PlayOnStart off waits until it is sent PlayTour"))
    surfaces = tuple((function, title, name) for function, title, name, _, _, _ in probes())
    materials = tuple((function, title, name) for function, title, name, _, _ in surface_probes())
    later = (
        ("CheckCrateRests", "Crate: dropped on the road, it comes to rest on it", "a crate dropped on the road comes to rest on it"),
        ("CheckSlides", "Slides: a block slides farther on the asphalt than in the gravel",
         "a block slides farther on the asphalt than in the gravel"),
        ("CheckUnderFootbridge", "Footbridge pass: the camera where the tour puts it, under the deck",
         "the tour flies the camera under the footbridge"),
        ("CheckSentTourFlies", "Sent tour: PlayTour from another graph flies it", "a tour sent PlayTour by another graph flies"),
        ("CheckTourEndsHome", "Tour end: the camera back at the home view", "the tour ends at the home view"),
        ("CheckTourRepeats", "Repeat: the next tour takes off", "the tour takes off again"),
        ("CheckSentTourStops", "Repeat off: the sent tour stays at the home view after one tour",
         "a tour with Repeat off stays at the home view after one tour"),
    )
    return tour_start + surfaces + materials + later


# The entities the checks look at, by the names of Tests.sgraph's variables: (variable, the scene entity's name, tooltip).
TEST_ENTITIES = (
    ("Camera", "Main Camera", "The camera the tour flies"),
    ("Track", "Track", "The circuit's road, kerbs, verges and run-off"),
    ("Barriers", "Barriers", "The concrete walls along the circuit"),
    ("Ground", "Ground", "The grass the circuit lies on"),
    ("Footbridge", "Footbridge", "The footbridge over the back straight"),
    ("Crate", "Probe Crate", "The crate dropped on the road"),
    ("WaitingTour", "Waiting Tour", "Runs the tour with PlayOnStart and Repeat off; the checks send it PlayTour"),
    ("AsphaltSlider", "Asphalt Slider", "The block sent sliding along the asphalt"),
    ("GravelSlider", "Gravel Slider", "The block sent sliding along a gravel trap"),
)
# The test scene's own entities, which it keeps the IDs of when it is made again.
OWN_ENTITIES = ("Probe Crate", "Asphalt Slider", "Gravel Slider", "Waiting Tour", "Tests")


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
                    (("Passed", "bool"), ("Detail", "string")), ()),
                   ("ProbeSurface", False, (("From", VECTOR), ("Expected", "string")), (("Passed", "bool"), ("Detail", "string")),
                    (("Found", "string", ""),)),) + tuple((name, False, (), (), ()) for name, _, _ in checks()))


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

    with author.region(TESTS, "Probe surface: the physics material a ray down from a point hits", CHECK, "ProbeSurface") as b:
        ray = b.call(RAYCAST, Origin=b.entry["From"], Direction=[0.0, -1.0, 0.0], MaxDistance=20.0)
        material = b.pure("Get:Strada.RaycastHit.Material", "Value", Target=ray["Hit"])
        known = b.branch(b.pure("IsValid", Value=material), comment="A collider without a physics material names none")
        found = b.set("Found", b.pure("Get:Strada.PhysicsMaterial.Surface", "Value", Target=material))
        passed = b.both(ray["Result"], b.equal(b.var("Found"), b.entry["Expected"]))
        b.link(passed, b.ret["Passed"])
        b.link(b.fmt("the ray hit the surface '{Found}' at {Point}", Found=b.var("Found"),
                     Point=b.pure("Get:Strada.RaycastHit.Point", "Value", Target=ray["Hit"])), b.ret["Detail"])
        b.chain(b.entry, ray, known)
        b.chain(known["True"], found, b.ret)
        b.flow(known["False"], b.ret)

    for function, title, name in order:
        with author.region(TESTS, title, CHECK, function) as b:
            WRITERS.get(function, check_material if function in MATERIAL_CHECKS else check_surface)(b, name)


def check_tour_starts_home(b, name):
    done = b.both(near(b, camera_at(b), rounded(Tour.HOME_EYE), 0.1), facing(b, camera_forward(b), home_direction(), 1.0))
    run(b, check(b, name, done, 1, view_detail(b)))


def check_surface(b, name):
    _, _, _, entity, start, height = next(row for row in probes() if row[2] == name)
    probe = b.own("Probe", From=start, Surface=b.var(entity), Height=height)
    reported = b.library(CHECKS, "Check", Name=f"Moto: {name}", Passed=probe["Passed"], Detail=probe["Detail"])
    run(b, probe, reported)


MATERIAL_CHECKS = {function for function, _, _, _, _ in surface_probes()}


def check_material(b, name):
    _, _, _, surface, starts = next(row for row in surface_probes() if row[2] == name)
    steps = []
    passes = []
    details = []
    for start in starts:
        probe = b.own("ProbeSurface", From=start, Expected=surface)
        steps.append(probe)
        passes.append(probe["Passed"])
        details.append(probe["Detail"])
    passed = passes[0] if len(passes) == 1 else b.both(passes[0], passes[1])
    detail = details[0] if len(details) == 1 else b.fmt("{First}; {Second}", First=details[0], Second=details[1])
    reported = b.library(CHECKS, "Check", Name=f"Moto: {name}", Passed=passed, Detail=detail)
    run(b, *steps, reported)


def check_slides(b, name):
    (_, asphalt_variable, asphalt_start, _, asphalt_friction), (_, gravel_variable, gravel_start, _, gravel_friction) = sliders()
    asphalt, gravel = b.var(asphalt_variable), b.var(gravel_variable)

    def travelled(block, start):
        at = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=block)
        return b.expr("distance(vec2(at.X, at.Z), vec2(start.X, start.Z))", at=at, start=b.lit(VECTOR, rounded(start)))

    def speed(block):
        return b.expr("length(v)", v=b.pure("Get:Strada.RigidBodyComponent.LinearVelocity", "Value", Target=block))

    on_asphalt, in_gravel = travelled(asphalt, asphalt_start), travelled(gravel, gravel_start)
    stopped = b.expr("a < 0.02 && g < 0.02", a=speed(asphalt), g=speed(gravel))
    expected_asphalt, expected_gravel = round(stopping_distance(asphalt_friction), 3), round(stopping_distance(gravel_friction), 3)
    passed = b.both(stopped, b.expr(f"abs(a - {expected_asphalt}) <= {round(SLIDE_TOLERANCE * expected_asphalt, 3)} && "
                                    f"abs(g - {expected_gravel}) <= {round(SLIDE_TOLERANCE * expected_gravel, 3)} && a > g",
                                    a=on_asphalt, g=in_gravel))
    detail = b.fmt(f"{{Asphalt}} m on the asphalt (about {expected_asphalt}), {{Gravel}} m in the gravel (about {expected_gravel})",
                   Asphalt=on_asphalt, Gravel=in_gravel)
    run(b, check(b, name, stopped, 4, detail, passed))


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
    "CheckSlides": check_slides,
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


def build_scene(session, handles, project):
    """The test scene: the main scene saved as Tests (its entities keep their IDs), the crate over the road, the two
    sliding blocks, the Waiting Tour and the entity whose graph runs the checks, each with the ID it had in the test scene
    when there was one. Leaves the main scene open."""
    previous = {}
    if os.path.exists(os.path.join(project, "Assets", "Scenes", "Tests.sscene")):
        session.call("scene_open", {"path": "Scenes/Tests.sscene"})
        previous = {entity["name"]: entity["id"] for entity in session.call("scene_hierarchy")["entities"]
                    if entity["name"] in OWN_ENTITIES}

    def create(name, components):
        arguments = {"name": name, "components": components}
        if name in previous:
            arguments["id"] = previous[name]
        return session.call("entity_create", arguments)["id"]

    session.call("scene_open", {"path": "Scenes/Main.sscene"})
    session.call("scene_save", {"path": "Scenes/Tests.sscene"})
    session.call("scene_settings", {"name": "Tests"})
    create("Probe Crate", {
        "Transform": {"Translation": list(crate_start()), "RotationEuler": [0.0, Circuit.yaw(CRATE_AT), 0.0]},
        "Mesh": {"Mesh": "builtin://Cube"},
        "RigidBody": {"Type": "Dynamic", "Mass": 20.0, "LinearDamping": 0.5, "AngularDamping": 0.5},
        "BoxCollider": {"HalfExtents": [CRATE_HALF] * 3, "Friction": 0.8}})
    # The blocks start sliding with the scene, rotation locked and undamped, so that friction alone stops them.
    for entity, _, start, direction, _ in sliders():
        heading = math.degrees(math.atan2(-direction[0], -direction[2]))
        create(entity, {
            "Transform": {"Translation": [round(value, 4) for value in start], "RotationEuler": [0.0, heading, 0.0]},
            "Mesh": {"Mesh": "builtin://Cube"},
            "RigidBody": {"Type": "Dynamic", "Mass": 20.0, "LinearDamping": 0.0, "AngularDamping": 0.0, "LockRotation": [True, True, True],
                          "InitialLinearVelocity": [round(SLIDE_SPEED * value, 4) for value in direction]},
            "BoxCollider": {"HalfExtents": [CRATE_HALF] * 3, "Friction": 1.0}})
    create("Waiting Tour", {
        "Transform": {"Translation": list(WAITING_SPOT)},
        "VisualScript": {"Graph": handles[Tour.TOUR], "Variables": {
            "PlayOnStart": {"Type": "Bool", "Value": False}, "Repeat": {"Type": "Bool", "Value": False}}}})
    found = {}

    def walk(entities):
        for entity in entities:
            found.setdefault(entity["name"], entity["id"])
            walk(entity.get("children", []))

    walk(session.call("scene_hierarchy")["entities"])
    missing = [scene_name for _, scene_name, _ in TEST_ENTITIES if scene_name not in found]
    if missing:
        raise SystemExit(f"the test scene has no {', '.join(missing)}")
    create("Tests", {"VisualScript": {"Graph": handles[TESTS], "Variables": {
        name: entity_value(found[scene_name]) for name, scene_name, _ in TEST_ENTITIES}}})
    session.call("scene_save")
    session.call("scene_open", {"path": "Scenes/Main.sscene"})

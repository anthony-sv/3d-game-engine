"""Moto's test scenes and their checks, all of them graphs. A test scene is the main scene as it is without its props, with
what its checks need; its runner graph runs them in turn and reports to the test run. The checks come in modules, each
registered by one line of MODULES (the scene it joins and the module's name): a module gives its checks, how each is
written, the scene's entities its checks look at, its own entities and its helper functions (a Suite). SCENES has the
test scenes, one line each.

The first module is this one's (CORE): Scenes/Tests.sscene and its checks: the main scene as it is without its props
(they stand clear of all it checks and have no colliders), with a crate to drop on the road, two blocks that slide off along the asphalt and along a gravel trap, a second entity running the tour
that waits to be sent it (the Waiting Tour) and the Tests entity, whose graph checks the circuit's surfaces, the camera
tour, the ghost bike, its engine's sound, the camera director and the audio director one after another, then reports to
the test run. Its own entities keep their IDs when it is made again. The scene's ghost sets out Ghost.TEST_START_AT
seconds into its lap, so that it leans through Turn 6 and crosses the start and finish line while the tour flies.

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
off it stays at the home view after its one tour. The ghost's checks compare it with the racing line RacingLine.py made,
as its timeline plays the line's keys: leaned into Turn 6 as the line leans there (to the corner's inside, which the
layout gives), where the line puts it at three moments of its lap and facing the way the line runs (each read with the
ghost's own LapTime in the same update, the line's point carried on by its velocity for the moments between), and
across the start and finish line onto its next lap. Its engine's sound plays every voice, driven by the ghost's engine
speed, the on-load voices' volumes squared and the off-load voices' over EngineSound.OFF_LOAD_GAIN squared summing to 1
within EngineSound.POWER_BAND (the crossfades are equal-power, the off-load voices quieter by their gain) and each that
sounds pitched within its layer's band, on the Engine bus. The camera director, sent the events its actions send, cuts to the first trackside camera, then the second,
each the one camera that shows and hears the scene and turned to the ghost, then back to the tour's camera. The audio
director, sent TogglePause twice, blends the mix to the Paused snapshot and back. The scene turns the directors' own
cuts and the ride's sounds off (prepare_scene: the camera director's Directs and the audio director's RideSounds), so that
it plays as Moto did before the race and the ridden bike.
"""
import importlib
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Circuit  # noqa: E402  (the layout)
import EngineSound  # noqa: E402  (the engine's sound)
import Ghost  # noqa: E402  (the ghost bike)
import Graphs  # noqa: E402  (writing graphs)
import RacingLine  # noqa: E402  (the line the ghost rides)
import Sound  # noqa: E402  (the mix and the audio director)
import Tour  # noqa: E402  (the camera tour)
import Trackside  # noqa: E402  (the trackside cameras and their director)
from Graphs import CHECK, ENTITY, EVENTS, private, public  # noqa: E402

CHECKS = Graphs.script("Checks")
TESTS = Graphs.script("Tests")
RIDE_TESTS = Graphs.script("RideTests")
LIBRARIES = (CHECKS,)
VECTOR = "Strada.Vector3"


class TestScene:
    """A test scene: its name (Scenes/<name>.sscene), its runner entity and graph, the runner graph's description (with
    {count}, the number of its checks), how long a test run of it may take (seconds) and the main scene's roots it leaves
    out."""

    def __init__(self, name, runner, graph, about, timeout, left_out=("Props",)):
        self.name = name
        self.runner = runner
        self.graph = graph
        self.about = about
        self.timeout = timeout
        self.left_out = left_out

    @property
    def path(self):
        return f"Scenes/{self.name}.sscene"


class Suite:
    """A module's checks in a test scene:

    - checks(): its checks in the order they run, (function, title of its box, the check's name in the report);
    - write(b, function, name): writes one check's function into its region;
    - entities: the scene's entities its checks look at, (variable of the runner graph, the entity's name, tooltip);
    - variables: private variables of the runner graph its checks share (Graphs.private);
    - helpers: functions of the runner graph its checks call, declared before the checks: ((name, pure, inputs, outputs,
      locals), title of its box, write(b));
    - own: the names of the entities it makes in the scene, which keep their IDs when the scene is made again;
    - make(create): makes them, create(name, components) returning the new entity's ID;
    - prepare(session, found): adjusts the scene's entities (found: their IDs by name) before the runner is made;
    - events: regions of the runner graph's event graph it adds beside the start, which runs the checks: (title of its
      box, its colour, write(b)), for events other than Event.FixedUpdate (a graph handles each event once);
    - steps: functions of the runner graph its one Event.FixedUpdate calls each fixed step, every module's in the order
      MODULES registers them: (name, title of its box, write(b)), such as a ride driven from the fixed step whose readings
      its checks report; declared after the helpers, which they may call;
    - frames: functions of the runner graph its one Event.Update calls each frame, after the steps' and the same way, such
      as readings of what the graphs set every update (sounds, cameras), which no ride compared across frame rates may
      take."""

    def __init__(self, checks, write, entities=(), variables=(), helpers=(), own=(), make=None, prepare=None, events=(),
                 steps=(), frames=()):
        self.checks = checks
        self.write = write
        self.entities = entities
        self.variables = variables
        self.helpers = helpers
        self.own = own
        self.make = make
        self.prepare = prepare
        self.events = events
        self.steps = steps
        self.frames = frames


# The test scenes, one line each.
SCENES = (
    TestScene("Tests", "Tests", TESTS, "The test scene's checks: {count} checks of Moto's circuit, its camera tour and its ghost "
              "bike, one after another, each a latent function that acts or waits for its moment, then reports it.",
              math.ceil(Tour.LENGTH) + 30),
    TestScene("Ride", "Ride Tests", RIDE_TESTS, "The ride test scene's checks: {count} checks of the ridden bike, its rider "
              "assists and its autopilot, each a latent function that waits for a reading the ride takes in its fixed steps, then "
              "reports it.", 120),
)
# The modules whose checks the test scenes run, one line each, in the order their checks run: (the scene they join, the
# module that holds them, by its name). A module's Suite is its SUITE; it is imported when the checks are written, so a
# module may import this one for its helpers.
MODULES = (
    ("Tests", "Checks"),
    ("Ride", "BikeChecks"),
    ("Ride", "RideChecks"),
    ("Ride", "DirectorChecks"),
)
# The runner graphs and the library they report through, and the tests part's files.
GRAPHS = (CHECKS,) + tuple(scene.graph for scene in SCENES)
FILES = tuple(f"Assets/{scene.path}" for scene in SCENES) + tuple(f"Assets/{graph[len('asset://'):]}" for graph in GRAPHS) + \
    ("Assets/AssetRegistry.sreg",)


def suites(scene):
    """The suites of a test scene, in the order MODULES registers them."""
    return [importlib.import_module(module).SUITE for name, module in MODULES if name == scene.name]


def scene_checks(scene):
    """A test scene's checks, every suite's in turn: (suite, function, title, name)."""
    return [(suite,) + row for suite in suites(scene) for row in suite.checks()]
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
# The moments of the ghost's lap (seconds after the start and finish line) at which it must be where the line puts it,
# within GHOST_TOLERANCE meters, and its lean in Turn 6 within LEAN_TOLERANCE degrees of the line's. The test scene's
# ghost reaches them between the slides and the tour's footbridge pass (Ghost.TEST_START_AT).
GHOST_MOMENTS = (44.0, 48.0, 52.0)
GHOST_TOLERANCE = 0.1
# How far the ghost's forward may turn from the line's direction of travel at those moments (degrees).
HEADING_TOLERANCE = 2.0
LEAN_TOLERANCE = 1.0
# How far a trackside camera that shows may look from the ghost (degrees): it turns every update, and the ghost moves
# after it in some frames.
AIM_TOLERANCE = 2.0
# How long the pause check lets the mix blend to the Paused snapshot and back (seconds): past Sound.PAUSE_SECONDS.
PAUSE_WAIT = Sound.PAUSE_SECONDS + 0.1


def turn6_lean():
    """The moment of the ghost's lap in the middle of its deepest lean through Turn 6, and that lean (degrees, positive
    right)."""
    _, start, end, _ = next(row for row in Circuit.corner_ranges() if row[0].name == "Turn 6")
    inside = [point for point in RacingLine.LINE if start <= point.s <= end]
    deepest = max(abs(point.lean) for point in inside)
    plateau = [point for point in inside if abs(point.lean) >= deepest - 0.05]
    middle = plateau[len(plateau) // 2]
    return round(middle.time, 3), round(RacingLine.pose(middle.time)[2][0], 3)


def ghost_moment(time):
    """Where the ghost's timeline puts it at a moment of its lap, and its velocity there (rounded as the graph holds them)."""
    return rounded(RacingLine.pose(time)[0]), rounded(RacingLine.velocity(time))


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
        ("CheckGhostLeans", "Ghost lean: leaned into Turn 6 as the racing line leans there",
         "the ghost leans into Turn 6 as the racing line does"),
        ("CheckGhostRides", "Ghost line: on the racing line and facing its way at three moments",
         "the ghost rides the racing line"),
        ("CheckEngineSounds", "Engine sound: every layer plays, faded to one in power, each in its band",
         "the ghost's engine plays every layer, crossfaded to a steady power, each pitched within its band"),
        ("CheckCameraCuts", "Cuts: NextCamera cuts to the trackside cameras in turn, ResetCamera back",
         "the camera director cuts to its cameras and back to the tour on the events its actions send"),
        ("CheckPauseSnapshot", "Pause: TogglePause blends the mix to the Paused snapshot and back",
         "the pause event blends the mix to the Paused snapshot and back"),
        ("CheckUnderFootbridge", "Footbridge pass: the camera where the tour puts it, under the deck",
         "the tour flies the camera under the footbridge"),
        ("CheckSentTourFlies", "Sent tour: PlayTour from another graph flies it", "a tour sent PlayTour by another graph flies"),
        ("CheckGhostLaps", "Ghost lap: across the start and finish line and on round its next lap",
         "the ghost crosses the line and rides on round its next lap"),
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
    ("Ghost", Ghost.ENTITY, "The ghost bike riding the racing line"),
    ("EngineSound", EngineSound.ENTITY, "The ghost's engine sound, whose children are its voices"),
    ("CameraDirector", Trackside.ROOT, "The camera director, whose children are the trackside cameras"),
    ("AudioDirector", Sound.DIRECTOR, "The audio director, which blends the mix to the Paused snapshot"),
)
# The test scene's own entities (besides its runner), which it keeps the IDs of when it is made again.
OWN_ENTITIES = ("Probe Crate", "Asphalt Slider", "Gravel Slider", "Waiting Tour")
# The private variables of the runner graph the checks share.
VARIABLES = (private("StepStart", "double", 0.0), private("LineError", "float", 0.0), private("HeadingError", "float", 0.0),
             private("Power", "float", 0.0), private("Playing", "int", 0), private("Strays", "int", 0), private("Cuts", "int", 0),
             private("Aim", "float", 0.0), private("Snapshot", "string", ""))


# --- The interfaces ---------------------------------------------------------------------------------------------------------

def declare(author):
    author.declare(CHECKS, "What the test scene's checks share: reporting a check, and comparing points and directions.",
                   functions=(
                       ("Check", False, (("Name", "string"), ("Passed", "bool"), ("Detail", "string")), (), ()),
                       ("NearVector", True, (("A", VECTOR), ("B", VECTOR), ("Tolerance", "float")), (("Result", "bool"),), ()),
                       ("Facing", True, (("Direction", VECTOR), ("Expected", VECTOR), ("Degrees", "float")), (("Result", "bool"),), ()),
                   ))
    # Each runner graph: the entities and the variables of its suites, then their helpers and their checks, in turn.
    for scene in SCENES:
        parts = suites(scene)
        rows = scene_checks(scene)
        author.declare(scene.graph, scene.about.format(count=len(rows)), variables=(
            tuple(public(name, ENTITY, None, tooltip) for suite in parts for name, _, tooltip in suite.entities) +
            tuple(variable for suite in parts for variable in suite.variables)),
            functions=tuple(declaration for suite in parts for declaration, _, _ in suite.helpers) +
            tuple((name, False, (), (), ()) for suite in parts for name, _, _ in suite.steps) +
            tuple((name, False, (), (), ()) for suite in parts for name, _, _ in suite.frames) +
            tuple((function, False, (), (), ()) for _, function, _, _ in rows))


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


def write_tests(author, scene):
    """A runner graph: its start, which runs every check in turn and finishes the report, then its suites' helpers and
    checks."""
    order = scene_checks(scene)
    with author.region(scene.graph, "Start: every check in turn, then the report", CHECK) as b:
        start = b.event("Event.Create")
        settle = b.add("Flow.WaitFrames", Frames=1, comment="Every entity has run its Create first")
        steps = b.sequence(len(order) + 1)
        b.chain(start, settle)
        b.flow(settle["Completed"], steps)
        for index, (_, name, _, _) in enumerate(order):
            b.flow(steps[f"Then{index}"], b.own(name))
        b.flow(steps[f"Then{len(order)}"], b.call("Call:Strada.Testing.TestReporter.Finish()"))

    for suite in suites(scene):
        for title, color, write_events in suite.events:
            with author.region(scene.graph, title, color) as b:
                write_events(b)
    steps = [name for suite in suites(scene) for name, _, _ in suite.steps]
    if steps:
        with author.region(scene.graph, "Fixed steps: each module's ride, in turn, every fixed step", EVENTS) as b:
            calls = b.sequence(len(steps))
            b.chain(b.event("Event.FixedUpdate"), calls)
            for index, name in enumerate(steps):
                b.flow(calls[f"Then{index}"], b.own(name))
    frames = [name for suite in suites(scene) for name, _, _ in suite.frames]
    if frames:
        with author.region(scene.graph, "Frames: each module's readings, in turn, every frame", EVENTS) as b:
            calls = b.sequence(len(frames))
            b.chain(b.event("Event.Update"), calls)
            for index, name in enumerate(frames):
                b.flow(calls[f"Then{index}"], b.own(name))
    for suite in suites(scene):
        for (function, *_), title, write_helper in suite.helpers:
            with author.region(scene.graph, title, CHECK, function) as b:
                write_helper(b)
    for suite in suites(scene):
        for function, title, write_step in suite.steps + suite.frames:
            with author.region(scene.graph, title, EVENTS, function) as b:
                write_step(b)
    for suite, function, title, name in order:
        with author.region(scene.graph, title, CHECK, function) as b:
            suite.write(b, function, name)


def write_probe(b):
    """Probe: the surface a ray down from a point hits, at its height."""
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


def write_probe_surface(b):
    """Probe surface: the physics material a ray down from a point hits."""
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


# The helper functions the checks call: their declarations, the titles of their boxes and their writers.
HELPERS = (
    (("Probe", False, (("From", VECTOR), ("Surface", ENTITY), ("Height", "float")), (("Passed", "bool"), ("Detail", "string")), ()),
     "Probe: the surface a ray down from a point hits, at its height", write_probe),
    (("ProbeSurface", False, (("From", VECTOR), ("Expected", "string")), (("Passed", "bool"), ("Detail", "string")),
      (("Found", "string", ""),)),
     "Probe surface: the physics material a ray down from a point hits", write_probe_surface),
)


def write_check(b, function, name):
    """One of this module's checks, by its function's writer (WRITERS), or a probe of a surface or of its material."""
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


def ghost_at(b):
    return b.once("ghost at", lambda: b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=b.var("Ghost")))


def ghost_lap(b):
    """The ghost's read-out of how far into its lap it is (a remote Get of its graph), set in the update that placed it."""
    return b.once("ghost lap", lambda: b.get("LapTime", Ghost.GHOST, b.var("Ghost")))


def ghost_laps(b):
    return b.once("ghost laps", lambda: b.get("Laps", Ghost.GHOST, b.var("Ghost")))


def line_error(b, time):
    """How far the ghost is from where the line puts it at a moment of its lap, carried on by the line's velocity to the
    ghost's own LapTime (a frame at most later)."""
    point, velocity = ghost_moment(time)
    return b.once(("line error", time), lambda: b.expr("distance(at, point + velocity * (lap - moment))", at=ghost_at(b),
                                                       point=b.lit(VECTOR, point), velocity=b.lit(VECTOR, velocity),
                                                       lap=ghost_lap(b), moment=time))


def check_ghost_leans(b, name):
    moment, lean = turn6_lean()
    # Into the corner: the layout's side of Turn 6's inside (+1 right of travel), which the line's own lean cannot get wrong
    # with it.
    inside = next(side for corner, _, _, side in Circuit.corner_ranges() if corner.name == "Turn 6")
    # The lean to the right is the ghost's right side tilting down: its Right's height is -sin(lean).
    leaned = b.expr("degrees(asin(-right.Y))", right=b.pure("Get:Strada.TransformComponent.Right", "Value", Target=b.var("Ghost")))
    reached = b.expr(f"lap >= {moment}", lap=ghost_lap(b))
    passed = b.both(reached, b.expr(f"abs(lean - {lean}) <= {LEAN_TOLERANCE} && lean * {inside} >= {RacingLine.MIN_CORNER_LEAN}",
                                    lean=leaned))
    side = "right" if inside > 0 else "left"
    detail = b.fmt(f"the ghost leans {{Lean}} degrees at {{Lap}} s of its lap (the line: {lean} at {moment} s, into the corner: "
                   f"to the {side})", Lean=leaned, Lap=ghost_lap(b))
    run(b, check(b, name, reached, 20, detail, passed))


def check_ghost_rides(b, name):
    # The worst distance from the line and the worst turn from its direction over the moments, each read when the ghost's
    # lap reaches it.
    steps = [b.set("LineError", 0.0), b.set("HeadingError", 0.0)]
    forward = b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=b.var("Ghost"))
    for moment in GHOST_MOMENTS:
        first, waited = wait_until(b, b.expr(f"lap >= {moment}", lap=ghost_lap(b)), 20)
        worst = b.set("LineError", b.expr("max(worst, error)", worst=b.var("LineError"), error=line_error(b, moment)))
        _, velocity = ghost_moment(moment)
        turned = b.set("HeadingError", b.expr("max(worst, degrees(acos(clamp(dot(normalize(forward), normalize(along)), -1, 1))))",
                                              worst=b.var("HeadingError"), forward=forward, along=b.lit(VECTOR, velocity)))
        b.flow(waited, worst)
        b.flow(worst, turned)
        steps.append((first, turned))
    passed = b.expr(f"worst <= {GHOST_TOLERANCE} && turned <= {HEADING_TOLERANCE}", worst=b.var("LineError"),
                    turned=b.var("HeadingError"))
    moments = ", ".join(f"{moment:g}" for moment in GHOST_MOMENTS)
    detail = b.fmt(f"the ghost strays {{Worst}} m at most from the line and turns {{Turned}} degrees at most from its way at "
                   f"{moments} s of its lap", Worst=b.var("LineError"), Turned=b.var("HeadingError"))
    reported = b.library(CHECKS, "Check", Name=f"Moto: {name}", Passed=passed, Detail=detail)
    run(b, *steps, reported)


def check_ghost_laps(b, name):
    crossed = b.expr("laps >= 1", laps=ghost_laps(b))
    near = b.expr(f"laps == 1 && lap < 0.1 && error <= {GHOST_TOLERANCE}", laps=ghost_laps(b), lap=ghost_lap(b),
                  error=line_error(b, 0.0))
    detail = b.fmt("{Laps} laps; {Lap} s into the next, {Error} m from the line", Laps=ghost_laps(b), Lap=ghost_lap(b),
                   Error=line_error(b, 0.0))
    run(b, check(b, name, crossed, math.ceil(RacingLine.length()), detail, near))


def check_engine_sounds(b, name):
    # Every voice: playing, its volume squared added to the power (an off-load voice's over the off-load gain squared, so
    # that the sum is 1 at any load), and counted as a stray when it sounds outside its layer's band or plays off the
    # Engine bus.
    engine = b.var("EngineSound")
    each = b.add("Flow.ForEach", Array=b.pure("Get:Strada.Entity.Children", "Value", Target=engine))
    voice = each["Element"]
    volume = b.pure("Get:Strada.AudioSourceComponent.Volume", "Value", Target=voice)
    pitch = b.pure("Get:Strada.AudioSourceComponent.Pitch", "Value", Target=voice)
    on_bus = b.equal(b.pure("Get:Strada.AudioSourceComponent.Bus", "Value", Target=voice), b.lit("string", "Engine"))
    playing = b.pure("Get:Strada.AudioSourceComponent.IsPlaying", "Value", Target=voice)
    low, high = 1.0 / EngineSound.LAYER_RATIO - 1e-4, EngineSound.LAYER_RATIO + 1e-4
    gain = round(EngineSound.OFF_LOAD_GAIN ** 2, 6)
    power = b.set("Power", b.expr(f"power + volume * volume / (index < {EngineSound.LAYERS} ? 1.0 : {gain})", power=b.var("Power"),
                                  volume=volume, index=each["Index"]))
    count = b.set("Playing", b.expr("count + (playing ? 1 : 0)", count=b.var("Playing"), playing=playing))
    stray = b.set("Strays", b.expr(f"strays + ((volume > 0.001 && (pitch < {low:.6f} || pitch > {high:.6f})) || !engine ? 1 : 0)",
                                   strays=b.var("Strays"), volume=volume, pitch=pitch, engine=on_bus))
    b.chain(each["Body"], power, count, stray)
    speed = b.get("EngineSpeed", EngineSound.ENGINE_SOUND, engine)
    voices = len(EngineSound.VOICES)
    passed = b.expr(f"count == {voices} && abs(power - 1) <= {EngineSound.POWER_BAND} && strays == 0 && "
                    f"abs(speed - ghost) < 1 && speed > {RacingLine.IDLE}", count=b.var("Playing"), power=b.var("Power"),
                    strays=b.var("Strays"), speed=speed, ghost=b.get("EngineSpeed", Ghost.GHOST, b.var("Ghost")))
    detail = b.fmt(f"{{Playing}} of {voices} voices play, their volumes squared summing to {{Power}} (the off-load voices' over "
                   f"{gain}), {{Strays}} out of their band "
                   "or off the Engine bus, at {Speed} rpm", Playing=b.var("Playing"), Power=b.var("Power"), Strays=b.var("Strays"),
                   Speed=speed)
    reported = b.library(CHECKS, "Check", Name=f"Moto: {name}", Passed=passed, Detail=detail)
    run(b, b.set("Power", 0.0), b.set("Playing", 0), b.set("Strays", 0), (each, each["Completed"]), reported)


def check_camera_cuts(b, name):
    # A cut is seen when the camera it names alone shows and hears the scene; the director is sent each event, and two frames
    # later (its Follow has turned the cameras since) the cut is counted.
    director = b.var("CameraDirector")
    cameras = b.pure("Get:Strada.Entity.Children", "Value", Target=director)
    first = b.add("Array.Get", None, "Element", Array=cameras, Index=0).out
    second = b.add("Array.Get", None, "Element", Array=cameras, Index=1).out
    tour = b.var("Camera")
    active = b.get("Active", Trackside.CAMERA_DIRECTOR, director)

    def shows(camera):
        return b.both(b.pure("Get:Strada.CameraComponent.Primary", "Value", Target=camera),
                      b.pure("Get:Strada.AudioListenerComponent.Active", "Value", Target=camera))

    def silent(camera):
        return b.expr("!primary && !listener", primary=b.pure("Get:Strada.CameraComponent.Primary", "Value", Target=camera),
                      listener=b.pure("Get:Strada.AudioListenerComponent.Active", "Value", Target=camera))

    def cut(event, expected, showing, hidden):
        send = b.send(event, director, graph=Trackside.CAMERA_DIRECTOR)
        settle = b.add("Flow.WaitFrames", Frames=2)
        seen = b.set("Cuts", b.expr(f"cuts + (active == {expected} && showing && hidden ? 1 : 0)", cuts=b.var("Cuts"), active=active,
                                    showing=shows(showing), hidden=silent(hidden)))
        b.flow(send, settle)
        b.flow(settle["Completed"], seen)
        return send, seen

    ghost = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=b.var("Ghost"))
    eye = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=first)
    forward = b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=first)
    first_cut = cut("NextCamera", 1, first, tour)
    aim = b.set("Aim", b.expr(f"degrees(acos(clamp(dot(normalize(forward), normalize(ghost + vec3(0, {Trackside.AIM}, 0) - eye)), "
                              "-1, 1)))", forward=forward, ghost=ghost, eye=eye))
    b.flow(first_cut[1], aim)
    second_cut = cut("NextCamera", 2, second, first)
    back = cut("ResetCamera", 0, tour, second)
    passed = b.expr(f"cuts == 3 && aim <= {AIM_TOLERANCE}", cuts=b.var("Cuts"), aim=b.var("Aim"))
    detail = b.fmt("{Cuts} of 3 cuts seen (to the first trackside camera, the second and back to the tour); the first looked "
                   "{Aim} degrees from the ghost", Cuts=b.var("Cuts"), Aim=b.var("Aim"))
    reported = b.library(CHECKS, "Check", Name=f"Moto: {name}", Passed=passed, Detail=detail)
    run(b, b.set("Cuts", 0), (first_cut[0], aim), second_cut, back, reported)


def check_pause_snapshot(b, name):
    director = b.var("AudioDirector")
    snapshot = b.pure("Get:Strada.Audio.CurrentSnapshot", "Value")
    paused = b.get("Paused", Sound.AUDIO_DIRECTOR, director)
    pause = b.send("TogglePause", director, graph=Sound.AUDIO_DIRECTOR)
    muffled = wait_until(b, None, PAUSE_WAIT)
    noted = b.set("Snapshot", snapshot)
    resume = b.send("TogglePause", director, graph=Sound.AUDIO_DIRECTOR, comment="And back")
    resumed = wait_until(b, None, PAUSE_WAIT)
    passed = b.both(b.both(b.equal(b.var("Snapshot"), b.lit("string", Sound.PAUSED)), b.equal(snapshot, b.lit("string", ""))),
                    b.expr("!paused", paused=paused))
    detail = b.fmt("the snapshot was '{Paused}' after the first event, and is '{Resumed}' after the second",
                   Paused=b.var("Snapshot"), Resumed=snapshot)
    reported = b.library(CHECKS, "Check", Name=f"Moto: {name}", Passed=passed, Detail=detail)
    run(b, pause, muffled, noted, resume, resumed, reported)


WRITERS = {
    "CheckEngineSounds": check_engine_sounds,
    "CheckCameraCuts": check_camera_cuts,
    "CheckPauseSnapshot": check_pause_snapshot,
    "CheckGhostLeans": check_ghost_leans,
    "CheckGhostRides": check_ghost_rides,
    "CheckGhostLaps": check_ghost_laps,
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
    for scene in SCENES:
        write_tests(author, scene)
        author.finish(scene.graph)


# --- The scenes --------------------------------------------------------------------------------------------------------------

def entity_value(entity):
    return {"Type": "Entity", "Value": entity}


def build_scenes(session, handles, project):
    """Every test scene (build_scene). Leaves the main scene open."""
    for scene in SCENES:
        build_scene(session, handles, project, scene)


def build_scene(session, handles, project, scene):
    """A test scene: the main scene saved under the scene's name (its entities keep their IDs) without the roots it leaves
    out, its suites' own entities and the runner entity whose graph runs the checks, each with the ID it had in the test
    scene when there was one. Leaves the main scene open."""
    parts = suites(scene)
    own = {scene.runner} | {name for suite in parts for name in suite.own}
    previous = {}
    if os.path.exists(os.path.join(project, "Assets", *scene.path.split("/"))):
        session.call("scene_open", {"path": scene.path})
        previous = {entity["name"]: entity["id"] for entity in session.call("scene_hierarchy")["entities"] if entity["name"] in own}

    def create(name, components):
        arguments = {"name": name, "components": components}
        if name in previous:
            arguments["id"] = previous[name]
        return session.call("entity_create", arguments)["id"]

    session.call("scene_open", {"path": "Scenes/Main.sscene"})
    session.call("scene_save", {"path": scene.path})
    session.call("scene_settings", {"name": scene.name})
    # The props stand clear of everything the checks touch and have no colliders: a test scene leaves them out, which
    # keeps the project small (they are most of the main scene's file) and the test runs short.
    left_out = [entity["id"] for entity in session.call("scene_hierarchy")["entities"] if entity["name"] in scene.left_out]
    if left_out:
        session.call("entity_delete", {"entities": left_out})
    for suite in parts:
        if suite.make is not None:
            suite.make(create, handles)
    found = {}

    def walk(entities):
        for entity in entities:
            found.setdefault(entity["name"], entity["id"])
            walk(entity.get("children", []))

    walk(session.call("scene_hierarchy")["entities"])
    entities = [row for suite in parts for row in suite.entities]
    missing = [scene_name for _, scene_name, _ in entities if scene_name not in found]
    if missing:
        raise SystemExit(f"the test scene {scene.name} has no {', '.join(missing)}")
    for suite in parts:
        if suite.prepare is not None:
            suite.prepare(session, found)
    create(scene.runner, {"VisualScript": {"Graph": handles[scene.graph], "Variables": {
        name: entity_value(found[scene_name]) for name, scene_name, _ in entities}}})
    session.call("scene_save")
    session.call("scene_open", {"path": "Scenes/Main.sscene"})


# --- This module's suite: the test scene's checks of the circuit, the tour, the ghost and its sound, the cameras and the
# pause ------------------------------------------------------------------------------------------------------------------------

def make_entities(create, handles):
    """The crate over the road, the two sliding blocks and the Waiting Tour."""
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


def set_variables(session, entity, values):
    """Sets public variables of the graph an entity of the scene runs, keeping its others as the scene has them."""
    component = session.call("component_get", {"entity": entity, "component": "VisualScript"})["fields"]
    variables = dict(component.get("Variables", {}))
    variables.update(values)
    session.call("component_set", {"entity": entity, "component": "VisualScript", "fields": {"Variables": variables}})


def prepare_scene(session, found):
    """The ghost sets out into its lap where its checks find it early in the run, from the line's pose there; the camera
    director cuts only when told and the audio director plays no ride, as Moto did before the race and the ridden bike."""
    ghost = found[Ghost.ENTITY]
    session.call("component_set", {"entity": ghost, "component": "Transform", "fields": Ghost.transform(Ghost.TEST_START_AT)})
    session.call("component_set", {"entity": ghost, "component": "VisualScript", "fields": {
        "Variables": {"StartAt": {"Type": "Float", "Value": Ghost.TEST_START_AT}}}})
    set_variables(session, found[Trackside.ROOT], {"Directs": {"Type": "Bool", "Value": False}})
    set_variables(session, found[Sound.DIRECTOR], {"RideSounds": {"Type": "Bool", "Value": False}})


CORE = Suite(checks, write_check, TEST_ENTITIES, VARIABLES, HELPERS, OWN_ENTITIES, make_entities, prepare_scene)
SUITE = CORE

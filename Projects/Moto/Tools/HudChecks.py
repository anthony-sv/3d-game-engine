"""The HUD's and the pause's checks, a module of Checks.MODULES: they join the flow test scene (Scenes/Flow.sscene) after
the race's. Two things drive what they read:

- the Hud Tests entity, whose graph (Scripts/HudTests.sgraph) pauses the game in the ridden bike's launch from the grid
  (the riding graph's Throttle open and the bike past LAUNCH_SPEED), while the race director is still idle and the HUD
  shows the riding group, through the Pause action, as a player presses it (an action override, one press). A frame's
  time step is scaled as the frame begins, before the press reaches the director, so the frame of the press still runs
  its steps and the game stands still from the next: the graph notes the bike's speed in that frame and takes its
  readings two frames later (time scale 0, the mix's snapshot, the director's Paused, the pause screen, the bike's speed
  again, and the HUD's texts, bars and mark against the vehicle's members), writes STEADY over the HUD's speed and
  presses Pause again; the next frame, still standing, the speed must read STEADY (the HUD writes a text only when its
  value changes, and the speed has not), and the game goes on two frames later. The pause inserts frames without fixed steps, so the ride and the race read what they read
  without it, step for step;
- three checks of the runner graph, after the race's: a fourth race started and its lap 1 begun by a crossing of the line
  sent the director as its gates send them, against the scene's ghost (Scripts/Ghost.sgraph, which records no lap
  there: RaceChecks clears its Bike), given a best lap and a Delta, then given none
  and a crossing of the first sector gate, whose split the HUD compares with the best lap's (the second scripted race's,
  RaceChecks.SCRIPT) when the lap began; then the race is abandoned, and a fifth race's countdown shows neither the
  fourth's delta nor its sectors, and is abandoned too.

The pause screen is the menus' (Menu.py): the checks read it through the menus' graph.

The HUD writes what the vehicle reads in its own formats (Hud.ROUNDING); the checks compare each text with the same
member written so, each bar's length and the marks' places with what the applied inputs and the bike's place give.
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import Ghost  # noqa: E402  (the scene's ghost, whose Delta the HUD shows)
import Graphs  # noqa: E402  (writing graphs)
import Hud  # noqa: E402  (the screen and its formats)
import Menu  # noqa: E402  (the menus, whose pause screen the pause shows)
import Race  # noqa: E402  (the race director and the ghost's interface)
import RaceChecks  # noqa: E402  (the scripted races, whose best lap the split delta reads against)
import Vehicle  # noqa: E402  (the riding graph, whose public lean the pause waits for)
from Checks import Suite, run  # noqa: E402
from Graphs import EVENTS, ENTITY, private, public  # noqa: E402

HUD_TESTS = Graphs.script("HudTests")
DRIVER = "Hud Tests"
VECTOR2 = "Strada.Vector2"

# --- The pause in the ride ---------------------------------------------------------------------------------------------------

# When the pause is pressed: the riding graph's throttle open past this (the launch's is 1), the bike faster than this
# (m/s; the launch reaches about 20 in 3 s); the frame after the press, from which the game stands still, in which the
# bike's speed is noted; how many frames after the press the readings are taken, and how many after the second press.
THROTTLE_INPUT = 0.5
LAUNCH_SPEED = 10.0
STOPPED_FRAME = 1
PAUSED_FRAMES = 3
RESUMED_FRAMES = 2
# What the checks write over the HUD's speed while the game stands still, which the HUD leaves while the speed stays.
STEADY = "STEADY"
# How far a bar's length or a mark's place may lie from the one the inputs and the place give (canvas units): they are
# the same formula, so only float rounding.
PLACE_TOLERANCE = 0.01

# --- The deltas ------------------------------------------------------------------------------------------------------------

# The fourth race's crossings: the line LINE_STEP steps after the start signal begins lap 1, the first sector gate
# SPLIT_STEPS later. The ghost's best lap and the two Deltas it is given while it has one.
LINE_STEP = 100
GHOST_BEST = 90.0
AHEAD = -1.25
BEHIND = 0.4321
FRAMES = 3


def _float(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def signed(seconds):
    """Hud.sgraph's Signed, in the floats the graph computes in."""
    total = round(_float(abs(seconds) * 1000.0))
    sign = "-" if seconds < 0 and total > 0 else "+"
    return f"{sign}{total // 1000}.{total % 1000:03d}"


def split_delta():
    """The split delta the HUD shows at the first sector gate: SPLIT_STEPS after the line against the scripted best lap's
    split there, as the director and the HUD compute it in floats; with the number of steps."""
    dt = _float(Race.STEP)
    best = RaceChecks.expected_races()["races"][-1]["best_splits"][0]
    # About 0.6 s behind it, at a whole step whose delta in milliseconds is far from a rounding's half.
    steps = int(round((best + 0.6) / dt))
    while True:
        line = _float(_float(float(LINE_STEP)) * dt)
        gate = _float(_float(float(LINE_STEP + steps)) * dt)
        delta = _float(_float(gate - line) - best)
        millis = _float(abs(delta) * 1000.0)
        if abs(millis - math.floor(millis) - 0.5) > 0.1:
            return steps, delta
        steps += 1


SPLIT_STEPS, SPLIT_DELTA = split_delta()


# --- The driver's graph ------------------------------------------------------------------------------------------------------

READOUTS = (
    ("Phase", "string", "Wait", "Where the drive is: Wait, Press, Paused, Release, Resumed or Done"),
    ("Frames", "int", 0, "Frames counted in the phase"),
    ("Done", "bool", False, "Whether the readings have been taken"),
    ("Moving", "float", 0.0, "The bike's speed in the first frame the game stood still (m/s)"),
    ("Held", "float", -1.0, "The bike's speed frames later, while paused (m/s)"),
    ("Scale", "float", -1.0, "The time scale while paused"),
    ("Snapshot", "string", "", "The mix's snapshot while paused"),
    ("DirectorPaused", "bool", False, "The race director's Paused while paused"),
    ("ScreenShown", "bool", False, "Whether the pause screen showed while paused"),
    ("ResumedScale", "float", -1.0, "The time scale after the second press"),
    ("ResumedSnapshot", "string", "-", "The mix's snapshot after the second press"),
    ("DirectorAfter", "bool", True, "The race director's Paused after the second press"),
    ("ScreenAfter", "bool", True, "Whether the pause screen showed after the second press"),
    ("SpeedShown", "string", "", "The HUD's speed while paused"),
    ("SpeedExpected", "string", "-", "The vehicle's speed, written as the HUD writes it"),
    ("GearShown", "string", "", "The HUD's gear while paused"),
    ("GearExpected", "string", "-", "The vehicle's gear, written as the HUD writes it"),
    ("RpmShown", "string", "", "The HUD's engine speed while paused"),
    ("RpmExpected", "string", "-", "The vehicle's engine speed, written as the HUD writes it"),
    ("LeanShown", "string", "", "The HUD's lean while paused"),
    ("LeanExpected", "string", "-", "The vehicle's lean, written as the HUD writes it"),
    ("InputError", "float", 1.0, "The worst distance of an input's bar or mark from where the applied input puts it"),
    ("ThrottleInput", "float", 0.0, "The throttle the vehicle applied while paused"),
    ("MarkError", "float", 1.0, "How far the map's mark stands from where the bike's place puts it (canvas units)"),
    ("MarkAt", VECTOR2, [0.0, 0.0], "Where the map's mark stands while paused (canvas units)"),
    ("SteadyShown", "string", "", f"The HUD's speed the frame after {STEADY} was written over it, the game still standing"),
)
ENTITY_VARIABLES = (
    ("Race", "The race director"),
    ("Bike", "The ridden bike"),
    ("Hud", "The screen, which runs the HUD"),
    ("Menus", "The screen's menus, whose pause screen the pause shows"),
    ("Ghost", "The scene's ghost, the director's"),
)
PHASES = ("Wait", "Press", "Paused", "Release", "Resumed")


def declare_graphs(author):
    author.declare(HUD_TESTS, "Drives the HUD's and the pause's checks in the flow test scene: pauses the game in the ridden "
                   "bike's launch through the Pause action, reads the HUD and the pause against the vehicle, then resumes.",
                   variables=(
        tuple(public(name, ENTITY, None, tooltip) for name, tooltip in ENTITY_VARIABLES) +
        tuple(public(name, type_id, value, f"{tooltip} (a read-out)") for name, type_id, value, tooltip in READOUTS)),
        functions=tuple((phase, False, (), (), ()) for phase in PHASES) + (("Read", False, (), (), ()),))


def element(b, variable):
    """An element of the screen, by the HUD's variable that names it."""
    return b.once(("element", variable), lambda: b.get(variable, Hud.HUD, b.var("Hud")))


def member(b, name):
    return b.once(("member", name), lambda: b.pure(f"Get:Strada.VehicleComponent.{name}", "Value", Target=b.var("Bike")))


def pause_pressed(b, value):
    if value:
        return b.call("Call:Strada.Input.SetActionOverride(string,float)", Action=Race.PAUSE, Value=1.0,
                      comment="Pressed as a player presses it, from the next frame's evaluation")
    return b.call("Call:Strada.Input.ClearActionOverride(string)", Action=Race.PAUSE)


def enter(b, phase):
    return [b.set("Frames", 0), b.set("Phase", phase)]


def frames_reached(b, count):
    counted = b.set("Frames", b.expr("frames + 1", frames=b.var("Frames")))
    reached = b.branch(b.expr(f"frames >= {count}", frames=b.var("Frames")))
    b.flow(counted, reached)
    return counted, reached


def pause_screen(b):
    return b.once("pause screen", lambda: b.get("PauseScreen", Menu.MENU, b.var("Menus")))


def text_of(b, variable):
    return b.pure("Get:Strada.UITextComponent.Text", "Value", Target=element(b, variable))


def write_functions(author):
    with author.region(HUD_TESTS, "Wait: the pause pressed in the bike's launch from the grid", EVENTS, "Wait") as b:
        launching = b.branch(b.expr(f"throttle > {THROTTLE_INPUT} && speed > {LAUNCH_SPEED}",
                                    throttle=b.get("Throttle", Vehicle.BIKE, b.var("Bike")), speed=member(b, "ForwardSpeed")))
        b.chain(b.entry, launching)
        b.chain(launching["True"], pause_pressed(b, True), *enter(b, "Press"), b.ret)
        b.flow(launching["False"], b.ret)
    with author.region(HUD_TESTS, "Press: the press let go; this frame's steps still run", EVENTS, "Press") as b:
        b.chain(b.entry, pause_pressed(b, False), *enter(b, "Paused"), b.ret)
    with author.region(HUD_TESTS, "Paused: the speed as the game stands, every reading, Pause again", EVENTS, "Paused") as b:
        counted = b.set("Frames", b.expr("frames + 1", frames=b.var("Frames")))
        stopped = b.branch(b.expr(f"frames == {STOPPED_FRAME}", frames=b.var("Frames")), comment="The first frame without steps")
        moving = b.set("Moving", member(b, "ForwardSpeed"))
        reached = b.branch(b.expr(f"frames >= {PAUSED_FRAMES}", frames=b.var("Frames")))
        b.chain(b.entry, counted, stopped)
        b.chain(stopped["True"], moving, reached)
        b.flow(stopped["False"], reached)
        steady = b.call("Set:Strada.UITextComponent.Text", Target=element(b, "SpeedText"), Value=STEADY,
                        comment="The speed stays while the game stands: the HUD leaves the text")
        b.chain(reached["True"], b.own("Read"), steady, pause_pressed(b, True), *enter(b, "Release"), b.ret)
        b.flow(reached["False"], b.ret)
    with author.region(HUD_TESTS, "Release: the second press let go; the speed's text kept as it stands", EVENTS,
                       "Release") as b:
        kept = b.set("SteadyShown", text_of(b, "SpeedText"))
        b.chain(b.entry, kept, pause_pressed(b, False), *enter(b, "Resumed"), b.ret)
    with author.region(HUD_TESTS, "Resumed: frames later, the game going on", EVENTS, "Resumed") as b:
        counted, reached = frames_reached(b, RESUMED_FRAMES)
        readings = [b.set("ResumedScale", b.pure("Get:Strada.Time.TimeScale", "Value")),
                    b.set("ResumedSnapshot", b.pure("Get:Strada.Audio.CurrentSnapshot", "Value")),
                    b.set("DirectorAfter", b.get("Paused", Race.DIRECTOR, b.var("Race"))),
                    b.set("ScreenAfter", b.pure("Get:Strada.UIRectComponent.Visible", "Value", Target=pause_screen(b)))]
        b.chain(b.entry, counted)
        b.chain(reached["True"], *readings, b.set("Done", True), b.set("Phase", "Done"), b.ret)
        b.flow(reached["False"], b.ret)
    with author.region(HUD_TESTS, "Read: the pause, and the HUD against the vehicle's members", EVENTS, "Read") as b:
        speed = b.expr(Hud.ROUNDING["speed"], v=member(b, "ForwardSpeed"))
        gear = member(b, "Gear")
        rpm = b.expr(Hud.ROUNDING["rpm"], rpm=member(b, "EngineSpeed"))
        lean = b.expr(Hud.ROUNDING["lean"], lean=member(b, "LeanAngle"))
        gear_text = b.pure("Select", Condition=b.expr("gear == 0", gear=gear),
                           **{"True": b.lit("string", "N"), "False": b.fmt("{Gear}", Gear=gear)})
        side = b.pure("Select", Condition=b.expr("lean < 0", lean=lean), **{"True": b.lit("string", "L"), "False": b.lit("string", "R")})
        lean_text = b.pure("Select", Condition=b.expr("lean == 0", lean=lean), **{
            "True": b.lit("string", "0°"),
            "False": b.fmt("{Degrees}° {Side}", Degrees=b.expr("abs(lean)", lean=lean), Side=side)})

        def bar_error(variable, name):
            size = b.pure("Get:Strada.UIRectComponent.Size", "Value", Target=element(b, variable))
            return b.expr(f"abs(size.X - float({Hud.ROUNDING['input']}) * {Hud.BAR / 100.0})", size=size, value=member(b, name))

        steer_at = b.pure("Get:Strada.UIRectComponent.Position", "Value", Target=element(b, "LeanMark"))
        steer_error = b.expr(f"abs(at.X - ({Hud.STEER_MIDDLE} + float({Hud.ROUNDING['steer']}) * {Hud.STEER_REACH / 100.0}))",
                             at=steer_at, value=member(b, "Lean"))
        worst = b.expr("max(max(throttle, front), max(rear, steer))", throttle=bar_error("ThrottleBar", "Throttle"),
                       front=bar_error("FrontBar", "FrontBrake"), rear=bar_error("RearBar", "RearBrake"), steer=steer_error)
        cx, cz, scale = Hud.mark_transform()
        half = Hud.MAP_SIZE / 2.0
        at = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=b.var("Bike"))
        mark_at = b.set("MarkAt", b.pure("Get:Strada.UIRectComponent.Position", "Value", Target=element(b, "MapMark")))
        mark_error = b.set("MarkError", b.expr(f"distance(mark, vec2(round((at.X - {cx}) * {scale} + {half}), "
                                               f"round((at.Z - {cz}) * {scale} + {half})))", mark=b.var("MarkAt"), at=at))
        readings = [
            b.set("Scale", b.pure("Get:Strada.Time.TimeScale", "Value")),
            b.set("Snapshot", b.pure("Get:Strada.Audio.CurrentSnapshot", "Value")),
            b.set("DirectorPaused", b.get("Paused", Race.DIRECTOR, b.var("Race"))),
            b.set("ScreenShown", b.pure("Get:Strada.UIRectComponent.Visible", "Value", Target=pause_screen(b))),
            b.set("Held", member(b, "ForwardSpeed")),
            b.set("SpeedShown", text_of(b, "SpeedText")), b.set("SpeedExpected", b.fmt("{Kmh}", Kmh=speed)),
            b.set("GearShown", text_of(b, "GearText")), b.set("GearExpected", gear_text),
            b.set("RpmShown", text_of(b, "RpmText")), b.set("RpmExpected", b.fmt("{Rpm}", Rpm=rpm)),
            b.set("LeanShown", text_of(b, "LeanText")), b.set("LeanExpected", lean_text),
            b.set("InputError", worst), b.set("ThrottleInput", member(b, "Throttle")), mark_at, mark_error,
        ]
        b.chain(b.entry, *readings, b.ret)


def write_graphs(author):
    write_functions(author)
    with author.region(HUD_TESTS, "Drive: each frame, the phase's work", EVENTS) as b:
        phases = b.add("Flow.Switch", {"Type": "string", "Cases": list(PHASES)}, Selection=b.var("Phase"))
        b.chain(b.event("Event.Update"), phases)
        for phase in PHASES:
            b.flow(phases[f"Case:{phase}"], b.own(phase))


def write_module_graphs(author):
    write_graphs(author)
    author.finish(HUD_TESTS)


# --- The checks -------------------------------------------------------------------------------------------------------------

# How long a check waits for its reading (seconds of the game): the pause comes in the ride's launch, about 1 s into the
# scene, long before the ride's and the race's checks have reported.
HUD_WAIT = 20
# How long the fourth race's countdown may take to start it (seconds).
START_WAIT = 5


def driver(b, name):
    return b.get(name, HUD_TESTS, b.var("HudTests"))


def checks():
    return (
        ("CheckHudTexts", "HUD: the speed, the gear, the engine and the lean as the vehicle reads",
         "the HUD shows the bike's speed, gear, engine speed and lean as its vehicle reads them"),
        ("CheckHudInputs", "HUD: the throttle, the brakes and the lean as the vehicle applied them",
         "the HUD's bars and lean mark show the inputs the vehicle applied"),
        ("CheckHudMap", "HUD: the bike's mark on the track map where the bike is",
         "the track map's mark stands where the bike is"),
        ("CheckHudSteady", "HUD: a text left as it is while its value stays",
         "the HUD writes a text only when its value changes: a text written over while the game stands is left"),
        ("CheckPause", "Pause: the game stops, the mix muffles and the screen shows, then back",
         "the Pause action stops the game with the Paused snapshot and the pause screen, and lets it go on again"),
        ("CheckLiveDelta", "Live delta: the ghost's delta while it rides a best lap",
         "racing against a ghost that rides a best lap, the HUD shows the ghost's live delta"),
        ("CheckSplitDelta", "Split delta: at a sector gate against the best lap's split",
         "without a ghost's best lap, the HUD shows the split delta at a sector gate against the best lap's split"),
        ("CheckCountdownClears", "Countdown: the last race's delta and sectors gone",
         "a countdown shows neither the last race's delta nor its sectors"),
    )


def write_hud_texts(b, name):
    done = driver(b, "Done")
    pairs = (("SpeedShown", "SpeedExpected"), ("GearShown", "GearExpected"), ("RpmShown", "RpmExpected"),
             ("LeanShown", "LeanExpected"))
    passed = done
    for shown, expected in pairs:
        passed = b.both(passed, b.equal(driver(b, shown), driver(b, expected)))
    detail = b.fmt("speed '{Speed}' ('{SpeedWanted}'), gear '{Gear}' ('{GearWanted}'), engine '{Rpm}' ('{RpmWanted}'), lean "
                   "'{Lean}' ('{LeanWanted}')", Speed=driver(b, "SpeedShown"), SpeedWanted=driver(b, "SpeedExpected"),
                   Gear=driver(b, "GearShown"), GearWanted=driver(b, "GearExpected"), Rpm=driver(b, "RpmShown"),
                   RpmWanted=driver(b, "RpmExpected"), Lean=driver(b, "LeanShown"), LeanWanted=driver(b, "LeanExpected"))
    run(b, Checks.check(b, name, done, HUD_WAIT, detail, passed))


def write_hud_inputs(b, name):
    done = driver(b, "Done")
    passed = b.both(done, b.expr(f"error <= {PLACE_TOLERANCE} && throttle > {THROTTLE_INPUT}", error=driver(b, "InputError"),
                                 throttle=driver(b, "ThrottleInput")))
    detail = b.fmt("the bars and the lean mark {Error} units at most from the inputs; the throttle applied {Throttle}",
                   Error=driver(b, "InputError"), Throttle=driver(b, "ThrottleInput"))
    run(b, Checks.check(b, name, done, HUD_WAIT, detail, passed))


def write_hud_map(b, name):
    done = driver(b, "Done")
    passed = b.both(done, b.expr(f"error <= {PLACE_TOLERANCE}", error=driver(b, "MarkError")))
    detail = b.fmt("the mark at {At}, {Error} units from where the bike is", At=driver(b, "MarkAt"), Error=driver(b, "MarkError"))
    run(b, Checks.check(b, name, done, HUD_WAIT, detail, passed))


def write_hud_steady(b, name):
    done = driver(b, "Done")
    passed = b.both(done, b.equal(driver(b, "SteadyShown"), b.lit("string", STEADY)))
    detail = b.fmt(f"the speed read '{{Steady}}' a frame after '{STEADY}' was written over it, the speed unchanged",
                   Steady=driver(b, "SteadyShown"))
    run(b, Checks.check(b, name, done, HUD_WAIT, detail, passed))


def write_pause(b, name):
    done = driver(b, "Done")
    paused = b.both(b.both(b.expr("scale == 0 && moving > 1 && held == moving", scale=driver(b, "Scale"), moving=driver(b, "Moving"),
                                  held=driver(b, "Held")),
                           b.equal(driver(b, "Snapshot"), b.lit("string", "Paused"))),
                    b.both(driver(b, "DirectorPaused"), driver(b, "ScreenShown")))
    resumed = b.both(b.both(b.expr("scale == 1", scale=driver(b, "ResumedScale")),
                            b.equal(driver(b, "ResumedSnapshot"), b.lit("string", ""))),
                     b.expr("!paused && !shown", paused=driver(b, "DirectorAfter"), shown=driver(b, "ScreenAfter")))
    passed = b.both(done, b.both(paused, resumed))
    detail = b.fmt("paused: time scale {Scale}, snapshot '{Snapshot}', director paused {Director}, screen {Screen}, the bike "
                   "{Moving} then {Held} m/s; resumed: time scale {After}, snapshot '{Mix}', director paused {Still}, screen "
                   "{Shown}", Scale=driver(b, "Scale"), Snapshot=driver(b, "Snapshot"), Director=driver(b, "DirectorPaused"),
                   Screen=driver(b, "ScreenShown"), Moving=driver(b, "Moving"), Held=driver(b, "Held"),
                   After=driver(b, "ResumedScale"), Mix=driver(b, "ResumedSnapshot"), Still=driver(b, "DirectorAfter"),
                   Shown=driver(b, "ScreenAfter"))
    run(b, Checks.check(b, name, done, HUD_WAIT, detail, passed))


def race(b):
    return b.once("race", lambda: driver(b, "Race"))


def ghost(b):
    return b.once("ghost", lambda: driver(b, "Ghost"))


def delta_text(b):
    return b.pure("Get:Strada.UITextComponent.Text", "Value", Target=b.get("DeltaText", Hud.HUD, driver(b, "Hud")))


def ghost_set(b, name, value):
    return b.set(name, value, graph=Race.GHOST_GRAPH, target=ghost(b))


def crossing(b, gate, steps):
    start = b.get("StartStep", Race.DIRECTOR, race(b))
    return b.send("GateCrossed", race(b), graph=Race.DIRECTOR, Gate=gate, Step=b.expr(f"start + {steps}", start=start), Fraction=0.0)


def frames(b):
    return b.add("Flow.WaitFrames", Frames=FRAMES, comment="The HUD has read it since")


def write_live_delta(b, name):
    started = b.send("StartRace", race(b), graph=Race.DIRECTOR, comment="A fourth race, against the scene's ghost")
    racing = Checks.wait_until(b, b.equal(b.get("State", Race.DIRECTOR, race(b)), b.lit("string", "Racing")), START_WAIT)
    line = crossing(b, 0, LINE_STEP)
    ahead = [ghost_set(b, "BestLapTime", GHOST_BEST), ghost_set(b, "Delta", AHEAD)]
    first = frames(b)
    noted = b.set("AheadShown", delta_text(b))
    behind = ghost_set(b, "Delta", BEHIND)
    second = frames(b)
    later = b.set("BehindShown", delta_text(b))
    b.chain(line, *ahead, first)
    b.flow(first["Completed"], noted)
    b.chain(noted, behind, second)
    b.flow(second["Completed"], later)
    expected = (signed(AHEAD), signed(BEHIND))
    passed = b.both(b.equal(b.var("AheadShown"), b.lit("string", expected[0])), b.equal(b.var("BehindShown"), b.lit("string", expected[1])))
    detail = b.fmt(f"the delta read '{{Ahead}}' and '{{Behind}}' (the ghost's {expected[0]} and {expected[1]})",
                   Ahead=b.var("AheadShown"), Behind=b.var("BehindShown"))
    reported = b.library(Checks.CHECKS, "Check", Name=f"Moto: {name}", Passed=passed, Detail=detail)
    run(b, started, racing, (line, later), reported)


def write_split_delta(b, name):
    none = ghost_set(b, "BestLapTime", 0.0)
    first = frames(b)
    gate = crossing(b, 1, LINE_STEP + SPLIT_STEPS)
    second = frames(b)
    noted = b.set("SplitShown", delta_text(b))
    abandoned = b.send("AbandonRace", race(b), graph=Race.DIRECTOR)
    b.chain(none, first)
    b.flow(first["Completed"], gate)
    b.chain(gate, second)
    b.flow(second["Completed"], noted)
    b.chain(noted, abandoned)
    expected = signed(SPLIT_DELTA)
    passed = b.equal(b.var("SplitShown"), b.lit("string", expected))
    detail = b.fmt(f"the delta at the gate read '{{Split}}' (the split against the best lap's: {expected})", Split=b.var("SplitShown"))
    reported = b.library(Checks.CHECKS, "Check", Name=f"Moto: {name}", Passed=passed, Detail=detail)
    run(b, (none, abandoned), reported)


def write_countdown_clears(b, name):
    started = b.send("StartRace", race(b), graph=Race.DIRECTOR, comment="A fifth race, after the fourth's delta and sectors")
    counting = Checks.wait_until(b, b.equal(b.get("State", Race.DIRECTOR, race(b)), b.lit("string", "Countdown")), START_WAIT)
    shown = frames(b)
    hud = driver(b, "Hud")
    sectors = b.set("SectorsShown", b.pure("Get:Strada.UITextComponent.Text", "Value", Target=b.get("SectorsText", Hud.HUD, hud)))
    delta = b.set("CountdownDelta", delta_text(b))
    plate = b.set("PlateShown", b.pure("Get:Strada.UIRectComponent.Visible", "Value", Target=b.get("DeltaPlate", Hud.HUD, hud)))
    abandoned = b.send("AbandonRace", race(b), graph=Race.DIRECTOR)
    b.flow(shown["Completed"], sectors)
    b.chain(sectors, delta, plate, abandoned)
    passed = b.both(b.both(b.pure("String.IsEmpty", Text=b.var("SectorsShown")), b.pure("String.IsEmpty", Text=b.var("CountdownDelta"))),
                    b.pure("Operator.Not", A=b.var("PlateShown")))
    detail = b.fmt("in the countdown the sectors read '{Sectors}', the delta '{Delta}', its plate shown {Plate}",
                   Sectors=b.var("SectorsShown"), Delta=b.var("CountdownDelta"), Plate=b.var("PlateShown"))
    reported = b.library(Checks.CHECKS, "Check", Name=f"Moto: {name}", Passed=passed, Detail=detail)
    run(b, started, counting, (shown, abandoned), reported)


WRITERS = {
    "CheckHudTexts": write_hud_texts,
    "CheckHudInputs": write_hud_inputs,
    "CheckHudMap": write_hud_map,
    "CheckHudSteady": write_hud_steady,
    "CheckPause": write_pause,
    "CheckLiveDelta": write_live_delta,
    "CheckSplitDelta": write_split_delta,
    "CheckCountdownClears": write_countdown_clears,
}


def write_check(b, function, name):
    WRITERS[function](b, name)


# --- The scene ---------------------------------------------------------------------------------------------------------------

ENTITIES = (("HudTests", DRIVER, "Drives the HUD's and the pause's checks: the pause in the bike's launch"),)
VARIABLES = (private("AheadShown", "string", ""), private("BehindShown", "string", ""), private("SplitShown", "string", ""),
             private("SectorsShown", "string", "-"), private("CountdownDelta", "string", "-"), private("PlateShown", "bool", True))


def make(create, handles):
    """The driver of the HUD's and the pause's checks."""
    create(DRIVER, {"VisualScript": {"Graph": handles.get(HUD_TESTS, HUD_TESTS)}})


def prepare(session, found):
    """The driver knows the director, the bike, the screen and the scene's ghost."""
    Checks.set_variables(session, found[DRIVER], {
        "Race": Checks.entity_value(found[Race.ROOT]), "Bike": Checks.entity_value(found["Bike"]),
        "Hud": Checks.entity_value(found[Hud.ROOT]), "Menus": Checks.entity_value(found[Menu.ROOT]),
        "Ghost": Checks.entity_value(found[Ghost.ENTITY])})


SUITE = Suite(checks, write_check, ENTITIES, VARIABLES, own=(DRIVER,), make=make, prepare=prepare)

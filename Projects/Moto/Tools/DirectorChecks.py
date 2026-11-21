"""The directors' checks, a module of Checks.MODULES that joins the ride test scene (Scenes/Ride.sscene): the camera director's
cuts by its own rule in the attract mode and its chase view in the race, the listener on the camera that shows, and the
audio director's sounds of the ridden bike (its engine, the squeal, the shift blip, the gravel's crunch, the wind) and the
crowd, within the engine's 64 voices.

It rides nothing: it reads, every frame (the runner graph's Event.Update, Checks.Suite.frames), what the directors set in
their updates while the bike's ride (BikeChecks.py) and the assists' and the autopilot's (RideChecks.py) go on, from the
scene's start to the end of the second (AssistPhase Done), and its checks, which run after theirs, report what it read. The
directors set what they set once a frame, so the readings are of frames, not of fixed steps, and no line compared across
frame rates holds them; a graph's update may run before or after another's in a frame, so a reading that compares what the
director set with what the bike reads takes the nearer of the bike's reading in this frame and in the last.

It stands in for the race director (slice 4's GF), which it does not run: it broadcasts AttractStarted at ATTRACT_FROM
seconds of the game and RaceStarted at ATTRACT_TO, as the race director broadcasts them.

- Attract mode, from AttractStarted to RaceStarted: in every frame the camera that shows is the one the rule's plan
  (Trackside.plan(): the camera nearest the point ahead of the ghost along the racing line) gives for the ghost's time on
  its lap, or for a time PLAN_MARGIN seconds either side of it (a cut lands within a frame of the plan's moment, which the
  ghost's own place on its line and the hold give or take); and the director cuts as often as the plan does, once a camera.
- Race, from RaceStarted: in every frame the chase camera shows, CHASE_DISTANCE meters behind the bike on the level and
  CHASE_HEIGHT above it within CHASE_TOLERANCE, looking at the point above the bike within AIM_TOLERANCE degrees, and never
  more than BEHIND_TOLERANCE degrees round from straight behind it on the level (its heading follows the bike's eased).
- The listener: in every frame of both, the camera that shows is the one camera whose listener hears, and the director's
  Active names it.
- The engine: every frame the bike's engine voices are fed the vehicle's engine speed and throttle (within the frame), all
  sixteen play on the Engine bus, their powers balance as the curves give (EngineSound.balance(), within
  EngineSound.POWER_BAND) whenever the engine turns; and through the launch in first gear (from DRIVE_FROM steps of it to
  its end) the engine speed the voices play is the rear wheel's turning through the gearbox within DRIVE_BAND: the voices
  follow the bike's speed through its drivetrain.
- The squeal: the front brake alone locks the front wheel (BikeChecks' FrontBrake phase): above SLIDE_SPEED the squeal
  sounds at SLIDE_SHARE of its level or more; rolling on the asphalt with nothing applied (its Coast phase) it is silent
  (ROLL_SHARE at most).
- The blip: the director plays one on every change of gear the bike makes (its read-out Blips equals the changes this
  module counts), every one of them in the ride.
- The crunch: rolling in the gravel trap (the Gravel phase, after GRAVEL_SETTLE steps) at CRUNCH_SHARE of its level or more;
  on the asphalt (the Coast phase) silent.
- The voices: Audio.ActiveVoiceCount is Sound.voice_count() or more in every frame (every voice mixes, silent ones
  included) and never more than Sound.most_voices(), those and the blips their source plays at once: a voice that does not
  end, or one more than the scene plays, fails it. A blip is the one voice between the two: its clip runs on the mix's
  time, which a test run's null device advances in real time, so the frames it spans depend on how fast they run: test
  runs read 39 to 41 or 40 to 41 voices, a run paced at 60 frames a second 39 to 41.
- The wind and the crowd: the wind's volume at speed against its volume at a lower speed is the speeds' ratio squared
  within WIND_ROOM, and every crowd voice plays at its level.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import EngineSound  # noqa: E402  (the engine's sound)
import Ghost  # noqa: E402  (the ghost the attract mode follows)
import RacingLine  # noqa: E402  (the ghost's lap)
import RideChecks  # noqa: E402  (the ride whose end ends the readings, and its wait)
import Sound  # noqa: E402  (the audio director and the ride's sounds)
import Trackside  # noqa: E402  (the camera director, its cameras and its plan)
import Vehicle  # noqa: E402  (the ridden bike's gearbox)
from Checks import Suite, run  # noqa: E402
from Graphs import EVENTS, private  # noqa: E402

VECTOR = "Strada.Vector3"
DIRECTOR = Trackside.CAMERA_DIRECTOR
AUDIO = Sound.AUDIO_DIRECTOR
ENGINE = EngineSound.ENGINE_SOUND

# --- What it reads, and the bands ----------------------------------------------------------------------------------------

# When the stand-in broadcasts the race director's events (seconds of the game): the attract mode while the ghost rides
# from near the start and finish line through Turns 1 to 6, then the race while the bike rides on.
ATTRACT_FROM = 0.5
ATTRACT_TO = 40.0
# How far from the ghost's time on its lap the plan's camera may be (seconds).
PLAN_MARGIN = 0.6
# The chase view: how far the camera may stand from its place behind and above the bike (meters), how far it may look
# from the point above the bike (degrees), and how far round from straight behind the bike it may stand on the level
# (degrees: its heading follows the bike's at Trackside.CHASE_TURN a second, so it lags in a corner by about the bike's
# turn rate over that: 7.5 to 8.1 degrees at most on the autopilot's line at 1/30, 1/60 and 1/144 s a frame, in Debug and
# Release).
CHASE_TOLERANCE = 0.02
AIM_TOLERANCE = 0.5
BEHIND_TOLERANCE = 15.0
# The launch in first gear whose engine speed is read against the rear wheel's (steps of BikeChecks' Launch phase): from
# when its clutch has closed (a probe read the engine 9 % above the wheel's turning at step 60 and within 1.5 % from step 90)
# to the end, and how far apart they may be (a share).
DRIVE_FROM = 90
DRIVE_TO = 179
DRIVE_BAND = 0.03
# The squeal and the crunch: the speed above which a locked front wheel is read (m/s), the share of its level the squeal
# sounds at least while it slides and at most while it rolls; the steps of the gravel's coast after which the crunch is
# read (its ease has risen by then), and the share of its level it sounds at least (the bike rolls at 14 to 12 m/s there, of
# Sound.CRUNCH_SPEED's 15) and at most on the asphalt.
SLIDE_SPEED = 8.0
SLIDE_SHARE = 0.7
ROLL_SHARE = 0.02
GRAVEL_SETTLE = 15
CRUNCH_SHARE = 0.6
SILENT = 0.001
# The wind: read in the first frame at or after 30 steps into the coast to 20 m/s in neutral and into the coast on the
# asphalt (both steady); the room of their ratio against the speeds' ratio squared.
WIND_STEP = 30
WIND_ROOM = 0.1
# The frames at the scene's start a reading of the engine's voices leaves out: until the engine sound's graph has set
# them from what the director fed it.
SETTLE_FRAMES = 5
WAIT = RideChecks.WAIT


def expected_cuts():
    """The cuts of the attract mode from ATTRACT_FROM to ATTRACT_TO of the ghost's lap (it sets out at the line with the
    scene): the first to the plan's camera then, and one at each of the plan's changes between."""
    cuts = Trackside.plan()
    changes = [start for start, _ in cuts[1:] if ATTRACT_FROM < start < ATTRACT_TO]
    for start in changes:
        assert start - ATTRACT_FROM > 1.0 + PLAN_MARGIN and ATTRACT_TO - start > 1.0 + PLAN_MARGIN, \
            f"the plan cuts at {start} s, too near the attract mode's window ({ATTRACT_FROM} to {ATTRACT_TO} s)"
    return 1 + len(changes)


def dense_plan():
    """The plan's camera every Trackside.PLAN_STEP seconds of the ghost's lap, as the check reads it."""
    cuts = Trackside.plan()
    lap = RacingLine.length()
    cameras = []
    for step in range(int(lap / Trackside.PLAN_STEP) + 1):
        time = step * Trackside.PLAN_STEP
        cameras.append(next(camera for start, camera in reversed(cuts) if start <= time + 1e-9))
    return cameras


def gear_changes():
    """The changes of gear BikeChecks' ride makes at least (each ShiftUp, and each ShiftDown into neutral after it)."""
    return 9


# --- The runner graph's interface -----------------------------------------------------------------------------------------

ENTITIES = (
    ("Cameras", Trackside.ROOT, "The camera director, whose children are the trackside cameras and the chase camera"),
    ("Tour", "Main Camera", "The camera the tour flies"),
    ("GhostBike", Ghost.ENTITY, "The ghost, which the attract mode follows"),
    ("Director", Sound.DIRECTOR, "The audio director, which plays the ride's sounds"),
    ("BikeEngine", EngineSound.BIKE_ENTITY, "The ridden bike's engine sound, whose children are its voices"),
    ("Squeal", Sound.SQUEAL, "The ridden bike's tyre squeal"),
    ("Crunch", Sound.CRUNCH, "The ridden bike's gravel crunch"),
    ("Wind", Sound.WIND, "The wind over the ridden bike's rider"),
    ("Crowd", Sound.CROWD, "The crowd, whose children are the grandstands' voices"),
)
VARIABLES = (
    private("CamPlan", "int[]", dense_plan()), private("CamAttractSent", "bool", False), private("CamRaceSent", "bool", False),
    private("CamDone", "bool", False), private("CamFrames", "int", 0), private("CamOffPlan", "int", 0), private("CamCuts", "int", 0),
    private("CamLast", "int", 0), private("CamListenerFaults", "int", 0), private("CamListenerFrames", "int", 0),
    private("CamRaceFrames", "int", 0), private("CamChaseFaults", "int", 0), private("CamChaseWorst", "float", 0.0),
    private("CamAimWorst", "float", 0.0), private("CamBehindWorst", "float", 0.0), private("CamBikeLast", VECTOR),
    private("CamForwardLast", VECTOR),
    private("EarDone", "bool", False), private("EarFrames", "int", 0), private("EarFeedWorst", "float", 0.0),
    private("EarLoadWorst", "float", 0.0), private("EarPowerWorst", "float", 0.0), private("EarPlayingLeast", "int", 1000),
    private("EarLastRpm", "float", 0.0), private("EarLastThrottle", "float", 0.0), private("EarDriveWorst", "float", -1.0),
    private("EarSlide", "float", -1.0), private("EarRoll", "float", 0.0), private("EarGravel", "float", 1.0e9),
    private("EarAsphalt", "float", 0.0), private("EarGears", "int", 0), private("EarLastGear", "int", -1),
    private("EarVoicesLeast", "int", 1000000), private("EarVoicesMost", "int", 0), private("EarWindFast", "float", -1.0),
    private("EarFastSpeed", "float", 0.0), private("EarWindSlow", "float", -1.0), private("EarSlowSpeed", "float", 0.0),
    private("EarCrowdLeast", "int", 1000),
)


def bike(b):
    return b.var("Bike")


def speed(b):
    return b.once("forward speed", lambda: b.pure("Get:Strada.VehicleComponent.ForwardSpeed", "Value", Target=bike(b)))


def phase_is(b, name):
    return b.equal(b.var("Phase"), b.lit("string", name))


def elapsed(b):
    """The steps since BikeChecks' phase began."""
    return b.once("bike elapsed", lambda: b.expr("step - start", step=b.var("Step"), start=b.var("PhaseStep")))


def broadcast(b, name):
    """The race director's event, as it broadcasts it: to every graph with an event of that name and no parameters."""
    return b.add("Event.Broadcast", {"Name": name, "Parameters": []}, "Count",
                 comment="The race director's event, which this test stands in for")


def write_listeners(b, faults):
    """Counts a fault in the frame unless the camera that shows (the director's Active) alone hears: every camera that is
    primary has its listener on and none other, and exactly one does, the one Active names (0 the tour's)."""
    each = b.add("Flow.ForEach", Array=b.pure("Get:Strada.Entity.Children", "Value", Target=b.var("Cameras")))
    active = b.get("Active", DIRECTOR, b.var("Cameras"))
    hears = b.pure("Get:Strada.AudioListenerComponent.Active", "Value", Target=each["Element"])
    shows = b.pure("Get:Strada.CameraComponent.Primary", "Value", Target=each["Element"])
    counted = b.set("Hearing", b.expr("count + (hears ? 1 : 0)", count=b.var("Hearing"), hears=hears))
    wrong = b.set("Wrong", b.expr("wrong + (hears != shows || hears != (index + 1 == active) ? 1 : 0)", wrong=b.var("Wrong"),
                                  hears=hears, shows=shows, index=each["Index"], active=active))
    tour_hears = b.pure("Get:Strada.AudioListenerComponent.Active", "Value", Target=b.var("Tour"))
    tour_shows = b.pure("Get:Strada.CameraComponent.Primary", "Value", Target=b.var("Tour"))
    fault = b.set(faults, b.expr("faults + (hearing + (tour ? 1 : 0) != 1 || wrong > 0 || tour != shows || tour != (active == 0) "
                                 "? 1 : 0)", faults=b.var(faults), hearing=b.var("Hearing"), tour=tour_hears, wrong=b.var("Wrong"),
                                 shows=tour_shows, active=active))
    frames = b.set("CamListenerFrames", b.expr("frames + 1", frames=b.var("CamListenerFrames")))
    b.chain(each["Body"], counted, wrong)
    return [b.set("Hearing", 0), b.set("Wrong", 0), (each, each["Completed"]), fault, frames]


def write_attract(b):
    """Each frame of the attract mode: the camera that shows against the plan's, the cuts counted, the listener."""
    active = b.get("Active", DIRECTOR, b.var("Cameras"))
    lap = b.get("LapTime", Ghost.GHOST, b.var("GhostBike"))
    plan = b.var("CamPlan")
    length = b.add("Array.Length", None, "Length", Array=plan).out

    def planned(offset):
        index = b.expr(f"clamp(int((lap + {offset}) / {Trackside.PLAN_STEP}), 0, count - 1)", lap=lap, count=length)
        return b.add("Array.Get", None, "Element", Array=plan, Index=index).out

    on_plan = b.expr("active == early || active == now || active == late", active=active, early=planned(-PLAN_MARGIN),
                     now=planned(0.0), late=planned(PLAN_MARGIN))
    off = b.set("CamOffPlan", b.expr("off + (on ? 0 : 1)", off=b.var("CamOffPlan"), on=on_plan))
    frames = b.set("CamFrames", b.expr("frames + 1", frames=b.var("CamFrames")))
    cut = b.set("CamCuts", b.expr("cuts + (active != last ? 1 : 0)", cuts=b.var("CamCuts"), active=active, last=b.var("CamLast")))
    last = b.set("CamLast", active)
    steps = write_listeners(b, "CamListenerFaults")
    run(b, off, frames, cut, last, *steps)


def write_race(b):
    """Each frame of the race: the chase camera shows, behind and above the bike, looking at it, and alone hears."""
    active = b.get("Active", DIRECTOR, b.var("Cameras"))
    chase = b.add("Array.Get", None, "Element", Array=b.pure("Get:Strada.Entity.Children", "Value", Target=b.var("Cameras")),
                  Index=Trackside.CHASE_INDEX - 1).out
    eye = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=chase)
    look = b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=chase)
    now = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=bike(b))
    last = b.var("CamBikeLast")
    forward = b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=bike(b))
    distance, height, aim = Trackside.CHASE_DISTANCE, Trackside.CHASE_HEIGHT, Trackside.CHASE_AIM

    def place_error(at):
        return b.expr(f"max(abs(length(vec2(eye.X - at.X, eye.Z - at.Z)) - {distance}), abs(eye.Y - at.Y - {height}))", eye=eye, at=at)

    def aim_error(at):
        return b.expr(f"degrees(acos(clamp(dot(normalize(look), normalize(at + vec3(0, {aim}, 0) - eye)), -1, 1)))", look=look, at=at,
                      eye=eye)

    # The director may have moved the camera before this frame's update of the bike's place or after it: the nearer of the
    # two places is the one it stood for.
    placed = b.expr("min(a, c)", a=place_error(now), c=place_error(last))
    aimed = b.expr("min(a, c)", a=aim_error(now), c=aim_error(last))
    def behind_error(at, along):
        return b.expr("degrees(acos(clamp(dot(normalize(vec2(at.X - eye.X, at.Z - eye.Z)), normalize(vec2(f.X, f.Z))), -1, 1)))",
                      at=at, eye=eye, f=along)

    behind = b.expr("min(a, c)", a=behind_error(now, forward), c=behind_error(last, b.var("CamForwardLast")))
    faults = b.set("CamChaseFaults", b.expr(f"faults + (active == {Trackside.CHASE_INDEX} ? 0 : 1)", faults=b.var("CamChaseFaults"),
                                            active=active))
    worst = b.set("CamChaseWorst", b.expr("max(worst, e)", worst=b.var("CamChaseWorst"), e=placed))
    aim_worst = b.set("CamAimWorst", b.expr("max(worst, e)", worst=b.var("CamAimWorst"), e=aimed))
    behind_worst = b.set("CamBehindWorst", b.expr("max(worst, e)", worst=b.var("CamBehindWorst"), e=behind))
    frames = b.set("CamRaceFrames", b.expr("frames + 1", frames=b.var("CamRaceFrames")))
    steps = write_listeners(b, "CamListenerFaults")
    run(b, faults, worst, aim_worst, behind_worst, frames, *steps)


def write_camera_frame(b):
    """The cameras each frame: the stand-in's two broadcasts at their times, then the attract mode's readings or the race's,
    until the assists' ride is done; the bike's place noted for the next frame."""
    now = b.pure("Get:Strada.Time.Elapsed", "Value")
    sent, raced, done = b.var("CamAttractSent"), b.var("CamRaceSent"), b.var("CamDone")
    attract_due = b.branch(b.expr(f"!sent && now >= {ATTRACT_FROM}", sent=sent, now=now))
    attract = broadcast(b, "AttractStarted")
    attract_sent = b.set("CamAttractSent", True)
    race_due = b.branch(b.expr(f"sent && !raced && now >= {ATTRACT_TO}", sent=sent, raced=raced, now=now))
    race = broadcast(b, "RaceStarted")
    race_sent = b.set("CamRaceSent", True)
    finished = b.set("CamDone", b.expr("done || finished", done=done, finished=b.equal(b.var("AssistPhase"), b.lit("string", "Done"))))
    reading = b.branch(b.expr("sent && !done", sent=sent, done=done))
    which = b.branch(raced)
    attract_reading = b.own("CameraAttract")
    race_reading = b.own("CameraRace")
    noted = b.set("CamBikeLast", b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=bike(b)))
    heading = b.set("CamForwardLast", b.pure("Get:Strada.TransformComponent.Forward", "Value", Target=bike(b)))
    b.chain(b.entry, attract_due)
    b.chain(attract_due["True"], attract, attract_sent, race_due)
    b.flow(attract_due["False"], race_due)
    b.chain(race_due["True"], race, race_sent, finished)
    b.flow(race_due["False"], finished)
    b.chain(finished, reading)
    b.chain(reading["True"], which)
    b.chain(which["True"], race_reading, noted)
    b.chain(which["False"], attract_reading, noted)
    b.flow(reading["False"], noted)
    b.chain(noted, heading, b.ret)


def write_engine(b):
    """Each frame: the bike's engine voices fed the vehicle's engine speed and throttle, every one playing on the Engine bus,
    their powers balanced; in the launch's window, the engine speed against the rear wheel's turning."""
    engine = b.var("BikeEngine")
    rpm = b.pure("Get:Strada.VehicleComponent.EngineSpeed", "Value", Target=bike(b))
    throttle = b.pure("Get:Strada.VehicleComponent.Throttle", "Value", Target=bike(b))
    fed = b.get("EngineSpeed", ENGINE, engine)
    loaded = b.get("Load", ENGINE, engine)
    feed = b.set("EarFeedWorst", b.expr("max(worst, min(abs(fed - now), abs(fed - last)) / max(now, 1))", worst=b.var("EarFeedWorst"),
                                        fed=fed, now=rpm, last=b.var("EarLastRpm")))
    load = b.set("EarLoadWorst", b.expr("max(worst, min(abs(fed - now), abs(fed - last)))", worst=b.var("EarLoadWorst"), fed=loaded,
                                        now=throttle, last=b.var("EarLastThrottle")))
    each = b.add("Flow.ForEach", Array=b.pure("Get:Strada.Entity.Children", "Value", Target=engine))
    voice = each["Element"]
    volume = b.pure("Get:Strada.AudioSourceComponent.Volume", "Value", Target=voice)
    playing = b.pure("Get:Strada.AudioSourceComponent.IsPlaying", "Value", Target=voice)
    on_bus = b.equal(b.pure("Get:Strada.AudioSourceComponent.Bus", "Value", Target=voice), b.lit("string", "Engine"))
    gain = round(EngineSound.OFF_LOAD_GAIN ** 2, 6)
    power = b.set("Power", b.expr(f"power + volume * volume / (index < {EngineSound.LAYERS} ? 1.0 : {gain})", power=b.var("Power"),
                                  volume=volume, index=each["Index"]))
    count = b.set("Playing", b.expr("count + (playing && engine ? 1 : 0)", count=b.var("Playing"), playing=playing, engine=on_bus))
    b.chain(each["Body"], power, count)
    settled = b.expr(f"frames > {SETTLE_FRAMES} && fed > 0", frames=b.var("EarFrames"), fed=fed)
    balanced = b.set("EarPowerWorst", b.expr("settled ? max(worst, abs(power - 1)) : worst", settled=settled, worst=b.var("EarPowerWorst"),
                                             power=b.var("Power")))
    plays = b.set("EarPlayingLeast", b.expr("settled ? min(least, count) : least", settled=settled, least=b.var("EarPlayingLeast"),
                                            count=b.var("Playing")))
    spin = b.pure("Get:Strada.VehicleWheelState.Spin", "Value",
                  Target=b.pure("Call:Strada.VehicleComponent.GetWheelState(int)", Target=bike(b), Index=1))
    ratio = round(Vehicle.overall(1) * 60.0 / (2.0 * math.pi), 6)
    launching = b.both(b.equal(b.var("Phase"), b.lit("string", "Launch")),
                       b.expr(f"elapsed >= {DRIVE_FROM} && elapsed <= {DRIVE_TO} && gear == 1", elapsed=elapsed(b),
                              gear=b.pure("Get:Strada.VehicleComponent.Gear", "Value", Target=bike(b))))
    drive = b.set("EarDriveWorst", b.expr(f"launching ? max(worst, abs(rpm - spin * {ratio}) / max(spin * {ratio}, 1)) : worst",
                                          launching=launching, worst=b.var("EarDriveWorst"), rpm=rpm, spin=spin),
                  comment="The rear wheel's turning through first gear and the final drive, in revolutions a minute")
    noted_rpm = b.set("EarLastRpm", rpm)
    noted_throttle = b.set("EarLastThrottle", throttle)
    run(b, feed, load, b.set("Power", 0.0), b.set("Playing", 0), (each, each["Completed"]), balanced, plays, drive, noted_rpm,
        noted_throttle)


def write_tyres(b):
    """Each frame: the squeal while the front wheel is locked at speed and while the bike rolls on the asphalt, the crunch
    in the gravel and on the asphalt, the wind at two steady speeds, the changes of gear, the voices mixed and the crowd."""
    squeal = b.pure("Get:Strada.AudioSourceComponent.Volume", "Value", Target=b.var("Squeal"))
    crunch = b.pure("Get:Strada.AudioSourceComponent.Volume", "Value", Target=b.var("Crunch"))
    wind = b.pure("Get:Strada.AudioSourceComponent.Volume", "Value", Target=b.var("Wind"))
    sliding = b.both(phase_is(b, "FrontBrake"), b.expr(f"speed > {SLIDE_SPEED}", speed=speed(b)))
    slide = b.set("EarSlide", b.expr("sliding ? max(most, volume) : most", sliding=sliding, most=b.var("EarSlide"), volume=squeal))
    coasting = phase_is(b, "Coast")
    roll = b.set("EarRoll", b.expr("coasting ? max(most, volume) : most", coasting=coasting, most=b.var("EarRoll"), volume=squeal))
    in_gravel = b.both(phase_is(b, "Gravel"), b.expr(f"elapsed >= {GRAVEL_SETTLE}", elapsed=elapsed(b)))
    gravel = b.set("EarGravel", b.expr("rolling ? min(least, volume) : least", rolling=in_gravel, least=b.var("EarGravel"), volume=crunch))
    asphalt = b.set("EarAsphalt", b.expr("coasting ? max(most, volume) : most", coasting=coasting, most=b.var("EarAsphalt"),
                                         volume=crunch))
    fast_due = b.both(phase_is(b, "EaseToFront"), b.expr(f"elapsed >= {WIND_STEP} && was < 0", elapsed=elapsed(b),
                                                         was=b.var("EarWindFast")))
    fast = b.set("EarWindFast", b.expr("due ? volume : was", due=fast_due, volume=wind, was=b.var("EarWindFast")))
    fast_speed = b.set("EarFastSpeed", b.expr("due ? abs(speed) : was", due=fast_due, speed=speed(b), was=b.var("EarFastSpeed")))
    slow_due = b.both(coasting, b.expr(f"elapsed >= {WIND_STEP} && was < 0", elapsed=elapsed(b), was=b.var("EarWindSlow")))
    slow = b.set("EarWindSlow", b.expr("due ? volume : was", due=slow_due, volume=wind, was=b.var("EarWindSlow")))
    slow_speed = b.set("EarSlowSpeed", b.expr("due ? abs(speed) : was", due=slow_due, speed=speed(b), was=b.var("EarSlowSpeed")))
    gear = b.pure("Get:Strada.VehicleComponent.Gear", "Value", Target=bike(b))
    changed = b.set("EarGears", b.expr("gears + (last >= 0 && gear != last ? 1 : 0)", gears=b.var("EarGears"), last=b.var("EarLastGear"),
                                       gear=gear))
    noted = b.set("EarLastGear", gear)
    voices = b.pure("Get:Strada.Audio.ActiveVoiceCount", "Value")
    settled = b.expr(f"frames > {SETTLE_FRAMES}", frames=b.var("EarFrames"))
    least = b.set("EarVoicesLeast", b.expr("settled ? min(least, voices) : least", settled=settled, least=b.var("EarVoicesLeast"),
                                           voices=voices))
    most = b.set("EarVoicesMost", b.expr("max(most, voices)", most=b.var("EarVoicesMost"), voices=voices))
    each = b.add("Flow.ForEach", Array=b.pure("Get:Strada.Entity.Children", "Value", Target=b.var("Crowd")))
    crowd_volume = b.pure("Get:Strada.AudioSourceComponent.Volume", "Value", Target=each["Element"])
    crowd_plays = b.pure("Get:Strada.AudioSourceComponent.IsPlaying", "Value", Target=each["Element"])
    heard = b.set("Playing", b.expr(f"count + (plays && abs(volume - {Sound.CROWD_LEVEL}) < 0.0001 ? 1 : 0)", count=b.var("Playing"),
                                    plays=crowd_plays, volume=crowd_volume))
    b.chain(each["Body"], heard)
    crowd = b.set("EarCrowdLeast", b.expr("settled ? min(least, count) : least", settled=settled, least=b.var("EarCrowdLeast"),
                                          count=b.var("Playing")))
    frames = b.set("EarFrames", b.expr("frames + 1", frames=b.var("EarFrames")))
    done = b.set("EarDone", b.expr("done || finished", done=b.var("EarDone"), finished=b.equal(b.var("AssistPhase"), b.lit("string", "Done"))))
    # Each speed is noted before its volume, while the volume's sentinel (below 0) still says the reading is due.
    run(b, slide, roll, gravel, asphalt, fast_speed, fast, slow_speed, slow, changed, noted, least, most, b.set("Playing", 0),
        (each, each["Completed"]), crowd, frames, done)


def write_sound_frame(b):
    """The ride's sounds each frame until the assists' ride is done: the engine's readings, then the others'."""
    reading = b.branch(b.expr("!done", done=b.var("EarDone")))
    engine = b.own("SoundEngine")
    tyres = b.own("SoundOthers")
    b.chain(b.entry, reading)
    b.chain(reading["True"], engine, tyres, b.ret)
    b.flow(reading["False"], b.ret)


LOCALS = (("Hearing", "int", 0), ("Wrong", "int", 0))
HELPERS = (
    (("CameraAttract", False, (), (), LOCALS), "Camera attract: the camera that shows against the plan, the listener",
     write_attract),
    (("CameraRace", False, (), (), LOCALS), "Camera race: the chase camera behind the bike, the listener", write_race),
    (("SoundEngine", False, (), (), (("Power", "float", 0.0), ("Playing", "int", 0))),
     "Sound engine: the bike's voices fed the vehicle's engine, balanced", write_engine),
    (("SoundOthers", False, (), (), (("Playing", "int", 0),)),
     "Sound others: squeal, crunch, wind, shifts, voices and the crowd", write_tyres),
)
FRAMES = (
    ("DirectorCameras", "Directors' cameras: each frame, the stand-in's events and the cuts", write_camera_frame),
    ("DirectorSounds", "Directors' sounds: each frame, the ride's sounds read", write_sound_frame),
)


# --- The checks ------------------------------------------------------------------------------------------------------------

def checks():
    if WAIT >= RideChecks.ride_timeout():
        raise ValueError(f"a director check waits {WAIT} s, not less than the ride test scene's timeout of "
                         f"{RideChecks.ride_timeout()} s")
    return (
        ("CheckAttractCuts", "Attract mode: the director cuts round the trackside cameras by its rule",
         "in the attract mode the camera director cuts to the trackside camera its rule gives for where the ghost rides"),
        ("CheckChaseView", "Chase view: in the race the chase camera rides behind the bike",
         "in the race the camera director shows the chase view, which rides behind the bike looking at it"),
        ("CheckCutListener", "Listener: the camera that shows is the one that hears",
         "the camera that shows is the one camera whose listener hears, as the director's Active names it"),
        ("CheckBikeEngine", "Bike engine: its voices play the vehicle's engine",
         "the ridden bike's engine voices play the vehicle's engine speed and throttle, its speed through the gearbox"),
        ("CheckSqueal", "Squeal: a locked front wheel squeals, a rolling one does not",
         "the tyres squeal while the front wheel slides locked and are silent while the bike rolls"),
        ("CheckShiftBlip", "Shift blip: a blip on every change of gear", "the audio director plays a shift blip on every change of gear"),
        ("CheckGravelCrunch", "Gravel: the tyres crunch in the gravel, not on the asphalt",
         "the tyres crunch while they roll in the gravel trap and not on the asphalt"),
        ("CheckVoices", "Voices: every voice mixes, within the engine's 64",
         "every voice of the ghost, the ridden bike and the crowd mixes, within the engine's 64"),
        ("CheckWindCrowd", "Wind and crowd: the wind with the speed, the crowd at the stands",
         "the wind rises with the square of the bike's speed and the crowd murmurs at the grandstands"),
    )


def check_attract_cuts(b, name):
    expected = expected_cuts()
    ready = b.var("CamRaceSent")
    passed = b.expr(f"frames > 0 && off == 0 && cuts == {expected}", frames=b.var("CamFrames"), off=b.var("CamOffPlan"),
                    cuts=b.var("CamCuts"))
    text = b.fmt(f"from {ATTRACT_FROM} to {ATTRACT_TO} s of the game the director cut {{Cuts}} times (the plan {expected}), and in "
                 f"{{Off}} of {{Frames}} frames the camera that showed was not the one its plan gives within {PLAN_MARGIN} s of the "
                 f"ghost's time on its lap", Cuts=b.var("CamCuts"), Off=b.var("CamOffPlan"), Frames=b.var("CamFrames"))
    run(b, Checks.check(b, name, ready, WAIT, text, b.both(ready, passed)))


def check_chase_view(b, name):
    ready = b.var("CamDone")
    passed = b.expr(f"frames > 0 && faults == 0 && worst <= {CHASE_TOLERANCE} && aim <= {AIM_TOLERANCE} && behind <= {BEHIND_TOLERANCE}",
                    frames=b.var("CamRaceFrames"), faults=b.var("CamChaseFaults"), worst=b.var("CamChaseWorst"),
                    aim=b.var("CamAimWorst"), behind=b.var("CamBehindWorst"))
    text = b.fmt(f"in {{Frames}} frames of the race another camera showed in {{Faults}}; the chase camera stood {{Worst}} m at most "
                 f"from its place {Trackside.CHASE_DISTANCE} m behind and {Trackside.CHASE_HEIGHT} m above the bike (at most "
                 f"{CHASE_TOLERANCE}), looked {{Aim}} degrees at most from it (at most {AIM_TOLERANCE}) and stood {{Behind}} "
                 f"degrees at most round from straight behind it (at most {BEHIND_TOLERANCE})", Frames=b.var("CamRaceFrames"),
                 Faults=b.var("CamChaseFaults"), Worst=b.var("CamChaseWorst"), Aim=b.var("CamAimWorst"), Behind=b.var("CamBehindWorst"))
    run(b, Checks.check(b, name, ready, WAIT, text, b.both(ready, passed)))


def check_cut_listener(b, name):
    ready = b.var("CamDone")
    passed = b.expr("frames > 0 && faults == 0", frames=b.var("CamListenerFrames"), faults=b.var("CamListenerFaults"))
    text = b.fmt("in {Faults} of {Frames} frames of the attract mode and the race the camera that showed was not the one camera "
                 "that heard, or not the one the director's Active names", Faults=b.var("CamListenerFaults"),
                 Frames=b.var("CamListenerFrames"))
    run(b, Checks.check(b, name, ready, WAIT, text, b.both(ready, passed)))


def check_bike_engine(b, name):
    voices = len(EngineSound.VOICES)
    ready = b.var("EarDone")
    passed = b.expr(f"feed <= 0.0001 && load <= 0.0001 && power <= {EngineSound.POWER_BAND} && playing == {voices} && drive >= 0 && "
                    f"drive <= {DRIVE_BAND}", feed=b.var("EarFeedWorst"), load=b.var("EarLoadWorst"), power=b.var("EarPowerWorst"),
                    playing=b.var("EarPlayingLeast"), drive=b.var("EarDriveWorst"))
    text = b.fmt(f"the voices were fed the engine speed within {{Feed}} of it and the throttle within {{Load}}; {{Playing}} of {voices} "
                 f"played on the Engine bus at the least, their powers balanced within {{Power}} (at most {EngineSound.POWER_BAND}); "
                 f"through the launch in first gear the engine speed was within {{Drive}} of the rear wheel's turning through the "
                 f"gearbox (at most {DRIVE_BAND})", Feed=b.var("EarFeedWorst"), Load=b.var("EarLoadWorst"), Playing=b.var("EarPlayingLeast"),
                 Power=b.var("EarPowerWorst"), Drive=b.var("EarDriveWorst"))
    run(b, Checks.check(b, name, ready, WAIT, text, b.both(ready, passed)))


def check_squeal(b, name):
    slide, roll = round(SLIDE_SHARE * Sound.SQUEAL_LEVEL, 4), round(ROLL_SHARE * Sound.SQUEAL_LEVEL, 4)
    ready = b.var("EarDone")
    passed = b.expr(f"slide >= {slide} && roll <= {roll}", slide=b.var("EarSlide"), roll=b.var("EarRoll"))
    text = b.fmt(f"the squeal sounded at {{Slide}} at most while the front wheel slid locked above {SLIDE_SPEED} m/s (at least "
                 f"{slide}) and at {{Roll}} at most while the bike rolled on the asphalt (at most {roll})", Slide=b.var("EarSlide"),
                 Roll=b.var("EarRoll"))
    run(b, Checks.check(b, name, ready, WAIT, text, b.both(ready, passed)))


def check_shift_blip(b, name):
    least = gear_changes()
    blips = b.get("Blips", AUDIO, b.var("Director"))
    ready = b.var("EarDone")
    passed = b.expr(f"gears >= {least} && blips == gears", gears=b.var("EarGears"), blips=blips)
    text = b.fmt(f"the bike changed gear {{Gears}} times (at least {least}) and the audio director played {{Blips}} blips",
                 Gears=b.var("EarGears"), Blips=blips)
    run(b, Checks.check(b, name, ready, WAIT, text, b.both(ready, passed)))


def check_gravel_crunch(b, name):
    least = round(CRUNCH_SHARE * Sound.CRUNCH_LEVEL, 4)
    ready = b.var("EarDone")
    passed = b.expr(f"gravel >= {least} && gravel < 2 && asphalt <= {SILENT}", gravel=b.var("EarGravel"), asphalt=b.var("EarAsphalt"))
    text = b.fmt(f"the crunch sounded at {{Gravel}} at the least while the bike rolled in the gravel (at least {least}) and at "
                 f"{{Asphalt}} at most on the asphalt (at most {SILENT})", Gravel=b.var("EarGravel"), Asphalt=b.var("EarAsphalt"))
    run(b, Checks.check(b, name, ready, WAIT, text, b.both(ready, passed)))


def check_voices(b, name):
    least, most = Sound.voice_count(), Sound.most_voices()
    ready = b.var("EarDone")
    passed = b.expr(f"low >= {least} && high <= {most}", low=b.var("EarVoicesLeast"), high=b.var("EarVoicesMost"))
    text = b.fmt(f"the engine mixed {{Least}} to {{Most}} voices (at least {least}, the ghost's, the bike's and the crowd's; at most "
                 f"{most}, those and the blips)", Least=b.var("EarVoicesLeast"), Most=b.var("EarVoicesMost"))
    run(b, Checks.check(b, name, ready, WAIT, text, b.both(ready, passed)))


def check_wind_crowd(b, name):
    crowd = len(Sound.crowd_sites())
    ready = b.var("EarDone")
    fast, slow = b.var("EarWindFast"), b.var("EarWindSlow")
    ratio = b.expr("slow > 0 ? fast / slow : 0", fast=fast, slow=slow)
    squared = b.expr("s > 0 ? (f / s) * (f / s) : 0", f=b.var("EarFastSpeed"), s=b.var("EarSlowSpeed"))
    passed = b.expr(f"fast > 0 && slow > 0 && abs(ratio / squared - 1) <= {WIND_ROOM} && crowd == {crowd}", fast=fast, slow=slow,
                    ratio=ratio, squared=squared, crowd=b.var("EarCrowdLeast"))
    text = b.fmt(f"the wind sounded at {{Fast}} at {{FastSpeed}} m/s and at {{Slow}} at {{SlowSpeed}} m/s, a ratio of {{Ratio}} against "
                 f"the speeds' squared {{Squared}} (within {WIND_ROOM}); {{Crowd}} of {crowd} crowd voices played at their level at "
                 "the least", Fast=fast, FastSpeed=b.var("EarFastSpeed"), Slow=slow, SlowSpeed=b.var("EarSlowSpeed"), Ratio=ratio,
                 Squared=squared, Crowd=b.var("EarCrowdLeast"))
    # Every reading in one line, for whoever reads the run's log: of frames, so not the same at every frame rate.
    readings = b.fmt(
        "Moto's directors: {Cuts} cuts in the attract mode, {Off} frames off the plan; the chase view {Chase} m and {Aim} degrees "
        "from its place and aim at most, {Behind} degrees round from behind; {Faults} listener faults in {Frames} frames; the "
        "engine's voices fed within {Feed}, balanced within {Power}, the drivetrain within {Drive}; the squeal {Slide} sliding "
        "and {Roll} rolling, the crunch {Gravel} in the gravel and {Asphalt} on the asphalt; {Gears} changes of gear; {Least} to "
        "{Most} voices; the wind {Fast} and {Slow}", Cuts=b.var("CamCuts"), Off=b.var("CamOffPlan"), Chase=b.var("CamChaseWorst"),
        Aim=b.var("CamAimWorst"), Behind=b.var("CamBehindWorst"), Faults=b.var("CamListenerFaults"), Frames=b.var("CamListenerFrames"),
        Feed=b.var("EarFeedWorst"), Power=b.var("EarPowerWorst"), Drive=b.var("EarDriveWorst"), Slide=b.var("EarSlide"),
        Roll=b.var("EarRoll"), Gravel=b.var("EarGravel"), Asphalt=b.var("EarAsphalt"), Gears=b.var("EarGears"),
        Least=b.var("EarVoicesLeast"), Most=b.var("EarVoicesMost"), Fast=fast, Slow=slow)
    log = b.call("Call:Strada.Log.Info(string)", Message=readings)
    run(b, Checks.check(b, name, ready, WAIT, text, b.both(ready, passed)), log)


WRITERS = {
    "CheckAttractCuts": check_attract_cuts,
    "CheckChaseView": check_chase_view,
    "CheckCutListener": check_cut_listener,
    "CheckBikeEngine": check_bike_engine,
    "CheckSqueal": check_squeal,
    "CheckShiftBlip": check_shift_blip,
    "CheckGravelCrunch": check_gravel_crunch,
    "CheckVoices": check_voices,
    "CheckWindCrowd": check_wind_crowd,
}


def write_check(b, function, name):
    WRITERS[function](b, name)


SUITE = Suite(checks, write_check, ENTITIES, VARIABLES, HELPERS, frames=FRAMES)

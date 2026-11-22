"""A whole race, the module of the trial test scene (Scenes/Trial.sscene, a line of Checks.SCENES), which the player runs
behind STRADA_LONG_RIDES=1 (MotoRideTests.cpp): the main scene without its props, the race director, the bike, the ghost
and the screen wired as the main scene wires them, nothing standing in for any of them. The scene's switches give the race
LAPS laps and start the autopilot with the scene (the Ride graph's Autopilot), the director idle until StartRace: the
runner sends it in its fixed step START_STEP, as the title's RACE does (not in the scene's first: a reset in the step after
the vehicle was made leaves it launching otherwise, Docs/Architecture.md section 15). So the director puts the bike on the
grid, holds it
through the countdown while the autopilot already asks for throttle, starts the race, times its laps at the gates and
broadcasts its lap events, which the ghost records the best lap from; at the results the director saves the best lap with
the ghost's samples in the game's save slot (Race.SLOT).

The runner reads the race from its own fixed steps and logs one line ("Moto's trial: ..."): each lap's time as the
director timed it against its own count of the steps between the bike's crossings of the line's plane, the race's time,
the ghost's best lap and its samples, and what the slot holds once the director saved it. A run that keeps its data (a
development run, not a test run) then shows the next run of the scene loading the slot: the ghost given the samples and
riding them, and the HUD showing the live delta against them (its frames' reading, logged once in lap 1: "Moto's kept
lap: ...").
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import Ghost  # noqa: E402  (the ghost and its best lap)
import LapChecks  # noqa: E402  (the laps' band)
import Race  # noqa: E402  (the race director and its slot)
import RacingLine  # noqa: E402  (the line's lap time)
from Checks import Suite, run  # noqa: E402
from Graphs import private  # noqa: E402

DIRECTOR = Race.DIRECTOR
VECTOR = "Strada.Vector3"
GHOST = Ghost.GHOST
SAMPLES = Race.GHOST_SAMPLES[0][0]

LAPS = Checks.TRIAL_LAPS
STEP = 1.0 / 60.0
COUNTDOWN_STEPS = round(Race.COUNTDOWN / STEP)
# A lap the director timed against the runner's count of the steps between two crossings of the line's plane: the director
# interpolates its crossing within a step (Race.py), the count takes the step of the bike's first position past the plane,
# so the two differ by under a step, and by a step more as the scene orders the two graphs in a step.
LAP_ROOM = round(2.0 * STEP, 4)
# The ghost's samples: Race.SAMPLE_RATE a second from the step its lap starts, one more either way of the lap's time.
SAMPLE_ROOM = 1
# How far into lap 1 of a run with a kept best lap the HUD's delta is read (seconds of the lap's clock).
DELTA_AT = 3.0
WAIT = 220
# The runner's fixed step that sends StartRace (its first is 0): one step after the scene's first, since a vehicle reset in
# the step after it was made launches 1.2 % slower than one reset later (Docs/Architecture.md section 15, "The race's
# start"), and every start the game makes comes later than that.
START_STEP = 1


def scene_timeout():
    return next(scene.timeout for scene in Checks.SCENES if scene.name == "Trial")


ENTITIES = (
    ("Bike", "Bike", "The ridden bike, which the autopilot rides"),
    ("Race", Race.ROOT, "The race director, which runs the race"),
    ("GhostBike", Ghost.ENTITY, "The ghost, which records the best lap"),
    ("DeltaPlate", "Delta Plate", "The HUD's plate under the delta"),
    ("DeltaText", "Delta Text", "The HUD's delta"),
)
VARIABLES = (
    private("StepStart", "double", 0.0),
    private("TStep", "int", 0), private("TAsked", "int", -1), private("TLast", "float", 1.0), private("TCrossings", "int", 0),
    private("TCrossStep", "int", 0), private("TOwnLaps", "float[]", []), private("TLaps", "float[]", []),
    private("TSeenLap", "int", 0), private("THeldThrottle", "float", 0.0), private("THeldGear", "int", 0),
    private("TCount", "int", -1), private("TReturns", "int", -1), private("TRaceTime", "float", -1.0),
    private("TBest", "float", -1.0), private("TGhostBest", "float", -1.0), private("TGhostSamples", "int", -1),
    private("TSaves", "int", -1), private("TSlotTime", "float", -1.0), private("TSlotSamples", "int", -1),
    private("TResults", "bool", False), private("TrialDone", "bool", False), private("TKeptLogged", "bool", False),
)


def director(b, name):
    return b.once(("director", name), lambda: b.get(name, DIRECTOR, b.var("Race")))


def ghost(b, name):
    return b.once(("ghost", name), lambda: b.get(name, GHOST, b.var("GhostBike")))


def write_race(b):
    """Each fixed step: StartRace in START_STEP; the hold read through the countdown; the bike's crossings of the line's
    plane counted; each lap the director finishes noted; at the results the race's readings."""
    first = b.branch(b.expr(f"asked < 0 && step >= {START_STEP}", asked=b.var("TAsked"), step=b.var("TStep")),
                     comment="The race starts as the scene does, as RACE starts it")
    asked = b.set("TAsked", director(b, "Steps"))
    sent = b.send("StartRace", b.var("Race"), graph=DIRECTOR)
    counting = b.branch(b.expr("counting && steps >= asked + 2", counting=b.equal(director(b, "State"), b.lit("string", "Countdown")),
                               steps=director(b, "Steps"), asked=b.var("TAsked")),
                        comment="Held on the grid: what the vehicle applied")
    held = [b.set("THeldThrottle", b.expr("max(most, applied)", most=b.var("THeldThrottle"),
                                          applied=b.pure("Get:Strada.VehicleComponent.Throttle", "Value", Target=b.var("Bike")))),
            b.set("THeldGear", b.expr("max(most, gear)", most=b.var("THeldGear"),
                                      gear=b.pure("Get:Strada.VehicleComponent.Gear", "Value", Target=b.var("Bike"))))]
    origin, forward = Race.gate_frame(0)
    here = b.expr("dot(at - origin, forward)", at=b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=b.var("Bike")),
                  origin=b.lit(VECTOR, [round(value, 4) for value in origin]), forward=b.lit(VECTOR, [round(value, 6) for value in forward]))
    crossed = b.branch(b.expr(f"last < 0 && here >= 0 && last > -{Race.REACH}", last=b.var("TLast"), here=here),
                       comment="Forward across the start and finish line's plane since the step before")
    timed = b.branch(b.expr("crossings >= 1", crossings=b.var("TCrossings")), comment="A lap of the runner's count ends")
    own = b.set("TOwnLaps", b.pure("Array.Add", Array=b.var("TOwnLaps"),
                                   Value=b.expr("float(step - from) * dt", step=b.var("TStep"), **{"from": b.var("TCrossStep")},
                                                dt=b.pure("Get:Strada.Time.FixedDeltaTime", "Value"))))
    noted = [b.set("TCrossings", b.expr("crossings + 1", crossings=b.var("TCrossings"))), b.set("TCrossStep", b.var("TStep"))]
    kept = b.set("TLast", here)
    changed = b.branch(b.expr("lap != seen", lap=director(b, "Lap"), seen=b.var("TSeenLap")), comment="The director began a lap")
    ended = b.branch(b.expr("seen >= 1", seen=b.var("TSeenLap")), comment="And finished the one before")

    def lap_time():
        return b.set("TLaps", b.pure("Array.Add", Array=b.var("TLaps"), Value=director(b, "LastLapTime")))

    lap = lap_time()
    seen = b.set("TSeenLap", director(b, "Lap"))
    results = b.branch(b.expr("results && !done", results=b.equal(director(b, "State"), b.lit("string", "Results")),
                              done=b.var("TResults")), comment="The results: the race's last lap finished")
    readings = [lap_time(), b.set("TResults", True), b.set("TRaceTime", director(b, "RaceTime")),
                b.set("TBest", director(b, "BestLapTime")),
                b.set("TGhostBest", ghost(b, "BestLapTime")),
                b.set("TGhostSamples", b.pure("Array.Length", "Length", Array=ghost(b, SAMPLES))),
                b.set("TReturns", director(b, "Returns")),
                b.set("TCount", b.expr("start - asked", start=director(b, "StartStep"), asked=b.var("TAsked")))]
    step = b.set("TStep", b.expr("step + 1", step=b.var("TStep")))
    b.chain(b.entry, first)
    b.chain(first["True"], asked, sent, counting)
    b.flow(first["False"], counting)
    b.chain(counting["True"], *held, crossed)
    b.flow(counting["False"], crossed)
    b.chain(crossed["True"], timed)
    b.chain(timed["True"], own, *noted)
    b.flow(timed["False"], noted[0])
    b.flow(noted[-1], kept)
    b.flow(crossed["False"], kept)
    b.chain(kept, changed)
    b.chain(changed["True"], ended)
    b.chain(ended["True"], lap, seen)
    b.flow(ended["False"], seen)
    b.flow(seen, results)
    b.flow(changed["False"], results)
    b.chain(results["True"], *readings, step)
    b.flow(results["False"], step)
    b.chain(step, b.ret)


def write_saved(b):
    """Once the director saved its slot (in its update of the frame after the results show): the slot read back."""
    waiting = b.branch(b.expr("results && !done && saves >= 1", results=b.var("TResults"), done=b.var("TrialDone"),
                              saves=director(b, "Saves")))
    opened = b.call("Call:Strada.SaveSlot.Open(string)", "Result", Name=Race.SLOT)
    slot = opened.out
    time = b.pure("Call:Strada.SaveSlot.Get<T>(string,T)", Target=slot, Name="LapTime", Fallback=b.lit("float", -1.0))
    samples = b.pure("Call:Strada.SaveSlot.Get<T>(string,T)", Target=slot, Name=SAMPLES, Fallback=b.lit(Race.GHOST_SAMPLES[0][1], []))
    readings = [b.set("TSaves", director(b, "Saves")), b.set("TSlotTime", time),
                b.set("TSlotSamples", b.pure("Array.Length", "Length", Array=samples))]
    line = b.fmt("Moto's trial: laps {Laps} s as the director timed them, {Own} s by the steps between the line's crossings; the "
                 "race {Race} s; started {Count} steps after StartRace with {Returns} returns, the vehicle applying at most {Throttle} "
                 "of the throttle and gear {Gear} through the countdown; the best lap {Best} s, the ghost's {GhostBest} s of {Samples} "
                 "samples; saved {Saves} times, the slot keeping {SlotSamples} samples of a {SlotTime} s lap",
                 Laps=b.var("TLaps"), Own=b.var("TOwnLaps"), Race=b.var("TRaceTime"), Count=b.var("TCount"),
                 Returns=b.var("TReturns"), Throttle=b.var("THeldThrottle"), Gear=b.var("THeldGear"), Best=b.var("TBest"),
                 GhostBest=b.var("TGhostBest"), Samples=b.var("TGhostSamples"), Saves=b.var("TSaves"), SlotTime=b.var("TSlotTime"),
                 SlotSamples=b.var("TSlotSamples"))
    log = b.call("Call:Strada.Log.Info(string)", Message=line, comment="Every reading of the race")
    b.chain(b.entry, waiting)
    b.chain(waiting["True"], opened, *readings, log, b.set("TrialDone", True), b.ret)
    b.flow(waiting["False"], b.ret)


def write_kept(b):
    """In a run that starts with a kept best lap: once in lap 1, DELTA_AT s into it, what the ghost and the HUD show."""
    due = b.branch(b.expr(f"!logged && lap == 1 && time >= {DELTA_AT}", logged=b.var("TKeptLogged"), lap=director(b, "Lap"),
                          time=director(b, "LapTime")))
    shown = b.pure("Get:Strada.UIRectComponent.Visible", "Value", Target=b.var("DeltaPlate"))
    text = b.pure("Get:Strada.UITextComponent.Text", "Value", Target=b.var("DeltaText"))
    line = b.fmt("Moto's kept lap: loaded at start {Loaded} s, the ghost riding it {Rides} with {Samples} samples; {Time} s into "
                 "lap 1 the ghost's Delta {Delta} s, the HUD's delta plate shown {Shown} reading '{Text}'",
                 Loaded=director(b, "LoadedLapTime"), Rides=ghost(b, "RidesBestLap"),
                 Samples=b.pure("Array.Length", "Length", Array=ghost(b, SAMPLES)), Time=director(b, "LapTime"),
                 Delta=ghost(b, "Delta"), Shown=shown, Text=text)
    log = b.call("Call:Strada.Log.Info(string)", Message=line, comment="Read in a frame: the HUD updates in frames")
    b.chain(b.entry, due)
    b.chain(due["True"], log, b.set("TKeptLogged", True), b.ret)
    b.flow(due["False"], b.ret)


STEPS = (("TrialRace", "Race: each fixed step, StartRace first, the laps and the results", write_race),
         ("TrialSaved", "Saved: the slot read back once the director saved it", write_saved))
FRAMES = (("TrialKept", "Kept lap: what the ghost and the HUD show in lap 1", write_kept),)


def lap_band():
    low, high = LapChecks.LAP_BAND
    return round(low * RacingLine.LAP_TIME, 2), round(high * RacingLine.LAP_TIME, 2)


def checks():
    if WAIT >= scene_timeout():
        raise ValueError(f"a trial check waits {WAIT} s, not less than the trial test scene's timeout of {scene_timeout()} s")
    return (
        ("CheckTrialStart", "Start: the race begins on the grid, held through the countdown",
         "the race begins on the grid: the bike put back once and held through the countdown while the autopilot asks"),
        ("CheckTrialLaps", "Laps: the director's times, the race's and the steps between crossings",
         f"the director times the race's {LAPS} laps at the gates as the bike's crossings of the line do"),
        ("CheckTrialGhost", "Ghost: the race's best lap recorded, its samples",
         "the ghost records the race's best lap at the director's lap events"),
        ("CheckTrialSlot", "Slot: the best lap saved with the ghost's samples",
         "the director saves the best lap in the slot with the ghost's samples once the results show"),
    )


def check_start(b, name):
    ready = b.var("TrialDone")
    passed = b.both(ready, b.expr(f"returns == 1 && count >= {COUNTDOWN_STEPS} && count <= {COUNTDOWN_STEPS + 1} && throttle == 0 && "
                                  "gear == 0", returns=b.var("TReturns"), count=b.var("TCount"), throttle=b.var("THeldThrottle"),
                                  gear=b.var("THeldGear")))
    text = b.fmt(f"started {{Count}} steps after StartRace (the countdown's {COUNTDOWN_STEPS}) with {{Returns}} returns; the vehicle "
                 "applied at most {Throttle} of the throttle and gear {Gear} through the countdown", Count=b.var("TCount"),
                 Returns=b.var("TReturns"), Throttle=b.var("THeldThrottle"), Gear=b.var("THeldGear"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def element(b, array, index):
    return b.add("Array.Get", None, "Element", Array=b.var(array), Index=index).out


def check_laps(b, name):
    low, high = lap_band()
    ready = b.var("TrialDone")
    first, second = element(b, "TLaps", 0), element(b, "TLaps", 1)
    passed = b.both(ready, b.expr(
        f"count == {LAPS} && own >= {LAPS} && abs(first - own1) <= {LAP_ROOM} && abs(second - own2) <= {LAP_ROOM} && "
        f"first >= {low} && first <= {high} && second >= {low} && second <= {high} && abs(race - first - second) <= 0.001",
        count=b.pure("Array.Length", "Length", Array=b.var("TLaps")), own=b.pure("Array.Length", "Length", Array=b.var("TOwnLaps")),
        first=first, second=second, own1=element(b, "TOwnLaps", 0), own2=element(b, "TOwnLaps", 1), race=b.var("TRaceTime")))
    text = b.fmt(f"the director timed laps of {{Laps}} s (in {low} to {high}), the steps between the crossings {{Own}} s (within "
                 f"{LAP_ROOM} s), the race {{Race}} s", Laps=b.var("TLaps"), Own=b.var("TOwnLaps"), Race=b.var("TRaceTime"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_ghost(b, name):
    ready = b.var("TrialDone")
    rate = Race.SAMPLE_RATE
    passed = b.both(ready, b.expr(f"abs(ghost - best) <= 0.0001 && abs(float(samples) - (floor(best * {rate}) + 1)) <= {SAMPLE_ROOM}",
                                  ghost=b.var("TGhostBest"), best=b.var("TBest"), samples=b.var("TGhostSamples")))
    text = b.fmt(f"the race's best lap {{Best}} s, the ghost's {{Ghost}} s of {{Samples}} samples ({rate:g} a second, one either "
                 "way)", Best=b.var("TBest"), Ghost=b.var("TGhostBest"), Samples=b.var("TGhostSamples"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


def check_slot(b, name):
    ready = b.var("TrialDone")
    passed = b.both(ready, b.expr("saves == 1 && abs(slot - best) <= 0.0001 && samples == ghost && ghost > 0", saves=b.var("TSaves"),
                                  slot=b.var("TSlotTime"), best=b.var("TBest"), samples=b.var("TSlotSamples"),
                                  ghost=b.var("TGhostSamples")))
    text = b.fmt("saved {Saves} times: the slot keeps {Slot} s with {Samples} samples, the ghost's {Ghost}", Saves=b.var("TSaves"),
                 Slot=b.var("TSlotTime"), Samples=b.var("TSlotSamples"), Ghost=b.var("TGhostSamples"))
    run(b, Checks.check(b, name, ready, WAIT, text, passed))


WRITERS = {
    "CheckTrialStart": check_start,
    "CheckTrialLaps": check_laps,
    "CheckTrialGhost": check_ghost,
    "CheckTrialSlot": check_slot,
}


def write_check(b, function, name):
    WRITERS[function](b, name)


SUITE = Suite(checks, write_check, ENTITIES, VARIABLES, steps=STEPS, frames=FRAMES)

"""Moto's mix: the mixer its sounds play through (Audio/Moto.smixer, which Moto.sproj names), the echo under the
footbridge, the engine sound's graph and curves (EngineSound.py), the ridden bike's sounds and the crowd's, and the audio
director that plays them, written by Author.py's sound part with the sounds GenerateAudio.py makes.

The buses: Master, with a limiter at CEILING, over Engine (the bikes' engines and the shift blip), Tyres (the squeal and
the gravel's crunch), Ambience (the wind and the crowd), Interface (the menus' and the HUD's sounds) and Reverb, which carries only the
reverb's tail: the footbridge's reverb zone sends the sounds under its deck to it, and the bus lifts the tail by
REVERB_DB, since a reverb gives back far less than it takes in and the ghost is under the deck for under half a second.
Every bus but Interface (and Master) runs a low-pass that stands open at OPEN Hz, so that a snapshot can close it.

The Paused snapshot muffles everything but Interface: each of those buses down by PAUSED_DB from its own volume (a
snapshot's value takes the bus's place) and its low-pass closed to PAUSED_CUTOFF Hz, so the menus of a paused game stay clear over a muffled track. A graph blends to it with
Audio.TransitionTo("Paused") and back with Audio.TransitionTo("").

The echo (the Footbridge Echo entity): a reverb zone, a box under the footbridge's deck across the road and its verges,
from the road up to the deck, that sends what sounds inside it to the Reverb bus, fading out over ECHO_FADE meters round
it. The ghost passes under the deck at about 190 km/h (RacingLine.py): the box and its fade span ECHO_SPAN meters of the
road, about 0.4 s of its pass, so the tail it leaves rings on after it is gone. At the footbridge camera the pass with the
echo is about 5 to 7 dB louder than the same pass without it in the half second under the deck and the one after.

The audio director (the Audio Director entity, Scripts/AudioDirector.sgraph) plays the ride and the pause:

- The pause: the custom event TogglePause blends the mix to the Paused snapshot over PAUSE_SECONDS, or back to the mixer's
  own values; with StartPaused on it sends itself one when the scene starts (the player's sound test measures the muffled
  mix that way). It reads no action: the game's pause is the race director's (slice 4's GF), which owns the Pause action
  and sends it TogglePause; a test, or any graph, sends it the same event.
- The ridden bike's sounds (its public Bike, the grid bike): the Bike Sounds entity, its child, which every update puts
  where the bike is as drawn (the director stands at the origin unturned, and the bike is a root), with the bike's own
  engine (EngineSound.BIKE_ENTITY, the engine sound's graph with a voice for each layer, fed the vehicle's engine speed
  and its throttle as the load), the tyre squeal, the gravel's crunch, the wind and the shift blip as its children:
  - the squeal from the tyres' slip: each wheel on the ground off the gravel and the grass slides by the larger of its
    slip ratio's share from SQUEAL_SLIP's first figure to its second and its slip angle's from SQUEAL_ANGLE's, the more
    of the two wheels counts, and the slide sounds in full above SQUEAL_SPEEDS' second speed and not at all below its
    first (a slip at a crawl is no slide: the slips of a tyre that barely turns read large and change step by step);
  - the crunch while a wheel rolls on gravel: half its level for each wheel on it, in full from CRUNCH_SPEED;
  - both eased towards what they sound with the time constants EASE_IN and EASE_OUT, so that a slip that changes from
    one step to the next does not click;
  - the wind from the bike's speed, as its square up to WIND_SPEED, its pitch rising with it;
  - the blip, a one-shot played on every shift (each frame the gear differs from the last frame's), counted by the
    read-out Blips;
  - the crowd: a voice at each grandstand (the Crowd entity's children, where crowd_sites() puts them), murmuring at
    CROWD_LEVEL from the scene's start.
  With its public RideSounds off they all stay silent (their volumes are 0 in the scene), as Moto sounded before the
  ridden bike: the test scene sets it so. Every voice mixes, silent ones included (below the engine's 64): the ghost's
  sixteen, the bike's sixteen and its three other loops, and the crowd's, voice_count() in all (one or two more while
  blips ring).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Circuit  # noqa: E402  (the layout)
import EngineSound  # noqa: E402  (the engine's sound)
import Graphs  # noqa: E402  (writing graphs)
import Props  # noqa: E402  (where the grandstands stand)
from Graphs import EFFECTS, ENTITY, EVENTS, RULES, private, public  # noqa: E402

MIXER = "Audio/Moto.smixer"
# The limiter's ceiling on Master (dBFS).
CEILING = -1.0
# The low-pass filters' cutoff while open, and while the Paused snapshot closes them (Hz); how much it lowers the buses (dB).
OPEN = 20000.0
PAUSED_CUTOFF = 650.0
PAUSED_DB = -10.0
# How far the Reverb bus lifts the tail (dB).
REVERB_DB = 18.0
# The buses' own volumes (dB).
VOLUMES = {"Master": 0.0, "Engine": 0.0, "Tyres": -4.0, "Ambience": -8.0, "Interface": 0.0, "Reverb": REVERB_DB}
# The buses that the Paused snapshot muffles, and the index of the low-pass in each one's effects.
MUFFLED = {"Engine": 0, "Tyres": 0, "Ambience": 0, "Reverb": 2}
PAUSED = "Paused"


def low_pass(frequency=OPEN):
    return {"Kind": "LowPass", "Frequency": frequency, "Q": 0.7071}


def bus(name, effects=(), parent="Master"):
    return {"Name": name, "Parent": parent, "VolumeDb": VOLUMES[name], "Muted": False, "Effects": list(effects), "DuckedBy": ""}


def mixer():
    """The mixer's fields, all of them, so that writing them again over the asset gives the same file."""
    buses = [
        bus("Master", [{"Kind": "Limiter", "CeilingDb": CEILING, "ReleaseSeconds": 0.1}], parent=""),
        bus("Engine", [low_pass()]),
        bus("Tyres", [low_pass()]),
        bus("Ambience", [low_pass()]),
        bus("Interface"),
        # A tail alone (no dry signal): what reaches it is already heard on its own bus. Its low end is cut, so the echo
        # under the deck does not boom.
        bus("Reverb", [{"Kind": "HighPass", "Frequency": 150.0, "Q": 0.7071},
                       {"Kind": "Reverb", "RoomSize": 0.55, "Damping": 0.45, "Width": 1.0, "WetDb": 0.0, "DryDb": -80.0},
                       low_pass()]),
    ]
    paused = []
    for name, index in MUFFLED.items():
        paused.append({"Bus": name, "Target": "Volume", "Value": VOLUMES[name] + PAUSED_DB})
        paused.append({"Bus": name, "Target": "Effect", "Effect": index, "Parameter": "Frequency", "Value": PAUSED_CUTOFF})
    return {"Buses": buses, "Snapshots": [{"Name": PAUSED, "Values": paused}], "SpeedOfSound": 343.0,
            "Impacts": {"Bus": "", "MinImpulse": 1.0, "FullImpulse": 20.0, "MaxPerFrame": 8}}


# --- The echo under the footbridge ---------------------------------------------------------------------------------------

ECHO = "Footbridge Echo"
# The box's half extents (meters): across the road to beyond its verges, from the road up to the deck's underside, and
# along the road a little beyond the deck (3.2 m deep); and how far round it the send fades out.
ECHO_HALF_EXTENTS = (Circuit.HALF_WIDTH + Circuit.KERB_WIDTH + Circuit.VERGE_WIDTH + 2.0, Circuit.BRIDGE_CLEARANCE / 2.0, 3.0)
ECHO_FADE = 8.0
ECHO_SEND = 1.0
ECHO_SPAN = 2.0 * (ECHO_HALF_EXTENTS[2] + ECHO_FADE)


def echo():
    """The Footbridge Echo's components: the box centered under the deck, turned with the road."""
    return {"Transform": {"Translation": [round(value, 4) for value in Circuit.offset_point(Circuit.FOOTBRIDGE, 0.0,
                                                                                           ECHO_HALF_EXTENTS[1])],
                          "RotationEuler": [0.0, Circuit.yaw(Circuit.FOOTBRIDGE), 0.0]},
            "ReverbZone": {"Shape": "Box", "HalfExtents": list(ECHO_HALF_EXTENTS), "FadeDistance": ECHO_FADE, "Bus": "Reverb",
                           "SendLevel": ECHO_SEND}}


# --- The ride's sounds and the crowd ------------------------------------------------------------------------------------

def clip(name):
    return f"asset://Audio/{name}.wav"


# The Bike Sounds entity, the audio director's first child, and its children in the order the director reads them: the
# bike's engine, then (name, clip, bus, where on the bike in its own meters, nearest distance at full volume, farthest
# distance it grows quieter to, whether it loops). The squeal sounds between the tyres, the crunch under them, the wind at
# the rider's helmet; the blip from the engine.
BIKE_SOUNDS = "Bike Sounds"
SQUEAL = "Tyre Squeal"
CRUNCH = "Gravel Crunch"
WIND = "Wind"
BLIP = "Shift Blip"
BIKE_VOICES = (
    (SQUEAL, clip("TyreSqueal"), "Tyres", (0.0, 0.3, 0.0), 5.0, 400.0, True),
    (CRUNCH, clip("GravelCrunch"), "Tyres", (0.0, 0.15, 0.0), 4.0, 250.0, True),
    (WIND, clip("Wind"), "Ambience", (0.0, 1.3, 0.2), 2.0, 60.0, True),
    (BLIP, clip("ShiftBlip"), "Engine", EngineSound.OFFSET, EngineSound.MIN_DISTANCE, EngineSound.MAX_DISTANCE, False),
)
# The voices' priorities (higher plays first when more sound than the engine mixes): the bike's engine's (EngineSound.py)
# over these, these over the crowd.
BIKE_PRIORITY = 180
CROWD_PRIORITY = 100
# How loud each plays at its fullest (0 to 1, the voice's volume).
SQUEAL_LEVEL = 0.8
CRUNCH_LEVEL = 0.9
WIND_LEVEL = 0.6
BLIP_LEVEL = 0.9
CROWD_LEVEL = 0.8
# The squeal: a wheel's slip ratio (a size: 0 rolling, 1 locked) and its slip angle (degrees) where the slide starts and
# where it is full, and the bike's speeds (m/s) between which a slide comes to sound in full (a smoothstep). Moto's bike
# rolls at slip ratios of 0.002 to 0.005 and angles under 1 degree, locks its front wheel (1) under a full front brake and
# spins its rear (1 and more) under a full throttle in first gear; below about 4 m/s the slips of its tyres change from
# one step to the next (a probe read angles of 0, then 17 degrees, at 0.3 m/s), which the speeds keep out.
SQUEAL_SLIP = (0.15, 0.6)
SQUEAL_ANGLE = (4.0, 10.0)
SQUEAL_SPEEDS = (3.0, 8.0)
# The crunch: in full from this speed (m/s); its pitch rises from 0.8 at rest to 1.2 at CRUNCH_PITCH_SPEED.
CRUNCH_SPEED = 15.0
CRUNCH_PITCH_SPEED = 30.0
# The time constants (seconds) the squeal and the crunch ease towards what they sound with, rising and falling.
EASE_IN = 0.04
EASE_OUT = 0.15
# The wind: its volume the square of the speed's share of WIND_SPEED (m/s), held beyond it, its pitch from 0.85 to 1.2.
WIND_SPEED = 60.0
# A grandstand's crowd: the voice stands this high over the ground at the middle of the stand, within CROWD_NEAR meters at
# full volume, quieter by the inverse of the distance out to CROWD_FAR (beyond it no quieter); stands closer than
# CROWD_GAP meters to each other are one grandstand. A murmur carries less than an engine: from the footbridge's camera,
# 490 m from the nearest stand, a probe of the sound test's captures read the crowd at -62.7 dBFS, 17 dB under the ghost
# riding by at its quietest (-45.5 dBFS), where with a CROWD_NEAR of 30 m it read -45.8 dBFS, as loud as the ghost.
CROWD = "Crowd"
CROWD_HEIGHT = 4.0
CROWD_NEAR = 5.0
CROWD_FAR = 2000.0
CROWD_GAP = 30.0


def crowd_sites(props=None):
    """The grandstands' crowds: (name, world position), one at the middle of each grandstand that Props.py places (its
    stands, gathered by CROWD_GAP), in the order it places them."""
    stands = [prop.position for prop in (props if props is not None else Props.place()) if prop.mesh == "Grandstand"]
    groups = []
    for position in stands:
        if groups and math.dist(position, groups[-1][-1]) < CROWD_GAP:
            groups[-1].append(position)
        else:
            groups.append([position])
    sites = []
    for number, group in enumerate(groups, start=1):
        x = sum(point[0] for point in group) / len(group)
        z = sum(point[2] for point in group) / len(group)
        sites.append((f"Crowd {number}", (round(x, 3), CROWD_HEIGHT, round(z, 3))))
    return sites


def bike_voice(clip_reference, bus, near, far, loops):
    """One of the bike's sounds: a loop playing from the start, silent until the director sounds it, or a one-shot."""
    return {"Clip": clip_reference, "Loop": loops, "PlayOnStart": loops, "Volume": 0.0 if loops else 1.0, "Spatial": True,
            "MinDistance": near, "MaxDistance": far, "Bus": bus, "Priority": BIKE_PRIORITY, "MaxVoices": 1 if loops else 2}


def crowd_voice():
    """A grandstand's crowd: a loop playing from the start, silent until the director sounds it (RideSounds)."""
    return {"Clip": clip("Crowd"), "Loop": True, "PlayOnStart": True, "Volume": 0.0, "Spatial": True, "MinDistance": CROWD_NEAR,
            "MaxDistance": CROWD_FAR, "Bus": "Ambience", "Priority": CROWD_PRIORITY, "MaxVoices": 1}


def voice_count(sites=None):
    """The voices that mix while the scene plays (blips aside): the ghost's engine, the bike's engine and its other loops,
    and the crowd's."""
    loops = sum(1 for *_, loops in BIKE_VOICES if loops)
    return 2 * len(EngineSound.VOICES) + loops + len(sites if sites is not None else crowd_sites())


# --- The audio director ------------------------------------------------------------------------------------------------

AUDIO_DIRECTOR = Graphs.script("AudioDirector")
DIRECTOR = "Audio Director"
PAUSE_SECONDS = 0.5
# The functions an update runs while the director plays the ride, in turn.
RIDE_FUNCTIONS = ("FollowBike", "Tyres", "Air", "Shifts")


def declare(author):
    author.declare(AUDIO_DIRECTOR, "The audio director: a TogglePause event (the race director's pause, or any graph's) blends "
                   "the mix to the Paused snapshot and back; it plays the ridden bike's engine, tyres, gravel, wind and shifts "
                   "from the vehicle, and the crowd at the grandstands. Tools/Sound.py says how.", variables=(
        public("Seconds", "float", PAUSE_SECONDS, "How long the mix takes to blend to the Paused snapshot, and back",
               (0.0, 10.0)),
        public("Paused", "bool", False, "Whether the mix is muffled by the Paused snapshot (a read-out)"),
        public("StartPaused", "bool", False, "Whether the mix starts muffled, blending to the Paused snapshot when the scene "
               "starts (a game that opens paused)"),
        public("RideSounds", "bool", True, "Whether the director plays the ridden bike's sounds and the crowd; off, they stay "
               "silent, as Moto sounded before the bike was ridden (the test scene)"),
        public("Bike", ENTITY, None, "The ridden bike, whose engine, tyres, gravel, wind and shifts the director plays"),
        public("Blips", "int", 0, "How many shift blips the director has played (a read-out)"),
        private("Sounds", ENTITY), private("Engine", ENTITY), private("Squeal", ENTITY), private("Crunch", ENTITY),
        private("Wind", ENTITY), private("Blip", ENTITY), private("LastGear", "int", -1),
    ), events=(("TogglePause", ()),), functions=tuple((name, False, (), (), ()) for name in RIDE_FUNCTIONS))


def eased(b, voice, target):
    """A voice's volume eased from what it is towards the target over the frame (EASE_IN rising, EASE_OUT falling)."""
    current = b.pure("Get:Strada.AudioSourceComponent.Volume", "Value", Target=voice)
    return b.expr(f"current + (target - current) * (1 - exp(-dt / (target > current ? {EASE_IN} : {EASE_OUT})))", current=current,
                  target=target, dt=b.pure("Get:Strada.Time.DeltaTime", "Value"))


def vehicle(b, member):
    return b.once(("vehicle", member), lambda: b.pure(f"Get:Strada.VehicleComponent.{member}", "Value", Target=b.var("Bike")))


def write(author):
    with author.region(AUDIO_DIRECTOR, "Follow the bike: its sounds where it is, its engine fed", EFFECTS, "FollowBike") as b:
        bike = b.var("Bike")
        sounds = b.var("Sounds")
        moved = b.call("Set:Strada.TransformComponent.Translation", Target=sounds,
                       Value=b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=bike),
                       comment="The director stands at the origin unturned and the bike is a root: their places are the world's")
        turned = b.call("Set:Strada.TransformComponent.Rotation", Target=sounds,
                        Value=b.pure("Get:Strada.TransformComponent.Rotation", "Value", Target=bike))
        engine = b.var("Engine")
        speed = b.set("EngineSpeed", vehicle(b, "EngineSpeed"), graph=EngineSound.ENGINE_SOUND, target=engine)
        load = b.set("Load", vehicle(b, "Throttle"), graph=EngineSound.ENGINE_SOUND, target=engine,
                     comment="The throttle the vehicle applied is the engine's load")
        b.chain(b.entry, moved, turned, speed, load, b.ret)
    with author.region(AUDIO_DIRECTOR, "Tyres: the squeal from the slip, the crunch on gravel", EFFECTS, "Tyres") as b:
        speed = b.expr("abs(v)", v=vehicle(b, "ForwardSpeed"))
        wheels = []
        for index in (0, 1):
            state = b.pure("Call:Strada.VehicleComponent.GetWheelState(int)", Target=b.var("Bike"), Index=index)
            surface = b.pure("Get:Strada.VehicleWheelState.Surface", "Value", Target=state)
            wheels.append({
                "contact": b.pure("Get:Strada.VehicleWheelState.HasContact", "Value", Target=state),
                "gravel": b.equal(surface, b.lit("string", "Gravel")),
                "grass": b.equal(surface, b.lit("string", "Grass")),
                "ratio": b.pure("Get:Strada.VehicleWheelState.LongitudinalSlip", "Value", Target=state),
                "angle": b.pure("Get:Strada.VehicleWheelState.LateralSlip", "Value", Target=state),
            })
        slides = [b.expr(f"contact && !gravel && !grass ? max(smoothstep({SQUEAL_SLIP[0]}, {SQUEAL_SLIP[1]}, ratio), "
                         f"smoothstep({SQUEAL_ANGLE[0]}, {SQUEAL_ANGLE[1]}, angle)) : 0.0", **wheel) for wheel in wheels]
        slide = b.expr(f"max(front, rear) * smoothstep({SQUEAL_SPEEDS[0]}, {SQUEAL_SPEEDS[1]}, speed)", front=slides[0],
                       rear=slides[1], speed=speed)
        squeal = b.var("Squeal")
        loud = b.call("Set:Strada.AudioSourceComponent.Volume", Target=squeal,
                      Value=eased(b, squeal, b.expr(f"{SQUEAL_LEVEL} * slide", slide=slide)),
                      comment="The more of the two wheels' slides, on the ground off the gravel and the grass, at speed")
        pitched = b.call("Set:Strada.AudioSourceComponent.Pitch", Target=squeal, Value=b.expr("0.9 + 0.2 * slide", slide=slide))
        rolling = b.expr("((front && fg) ? 0.5 : 0.0) + ((rear && rg) ? 0.5 : 0.0)", front=wheels[0]["contact"],
                         fg=wheels[0]["gravel"], rear=wheels[1]["contact"], rg=wheels[1]["gravel"])
        crunch = b.var("Crunch")
        crunching = b.call("Set:Strada.AudioSourceComponent.Volume", Target=crunch, Value=eased(b, crunch, b.expr(
            f"{CRUNCH_LEVEL} * share * min(speed / {CRUNCH_SPEED}, 1.0)", share=rolling, speed=speed)),
                           comment="Half for each wheel rolling on gravel, in full from 15 m/s")
        ground = b.call("Set:Strada.AudioSourceComponent.Pitch", Target=crunch,
                        Value=b.expr(f"0.8 + 0.4 * min(speed / {CRUNCH_PITCH_SPEED}, 1.0)", speed=speed))
        b.chain(b.entry, loud, pitched, crunching, ground, b.ret)
    with author.region(AUDIO_DIRECTOR, "Air: the wind rises with the bike's speed", EFFECTS, "Air") as b:
        share = b.expr(f"min(abs(v) / {WIND_SPEED}, 1.0)", v=vehicle(b, "ForwardSpeed"))
        wind = b.var("Wind")
        loud = b.call("Set:Strada.AudioSourceComponent.Volume", Target=wind, Value=b.expr(f"{WIND_LEVEL} * share * share", share=share))
        pitched = b.call("Set:Strada.AudioSourceComponent.Pitch", Target=wind, Value=b.expr("0.85 + 0.35 * share", share=share))
        b.chain(b.entry, loud, pitched, b.ret)
    with author.region(AUDIO_DIRECTOR, "Shifts: a blip on every change of gear", RULES, "Shifts") as b:
        gear = vehicle(b, "Gear")
        shifted = b.branch(b.expr("last >= 0 && gear != last", last=b.var("LastGear"), gear=gear),
                           comment="The gear differs from the last frame's")
        blip = b.call("Call:Strada.AudioSourceComponent.PlayOneShot(float)", "Result", Target=b.var("Blip"), Volume=BLIP_LEVEL)
        played = b.branch(blip.out)
        counted = b.set("Blips", b.expr("blips + 1", blips=b.var("Blips")))
        noted = b.set("LastGear", gear)
        b.chain(b.entry, shifted)
        b.chain(shifted["True"], blip, played)
        b.chain(played["True"], counted, noted)
        b.flow(played["False"], noted)
        b.flow(shifted["False"], noted)
        b.chain(noted, b.ret)
    with author.region(AUDIO_DIRECTOR, "Start: muffled if it starts paused; the ride's sounds found", EVENTS) as b:
        steps = b.sequence(2)
        starts = b.branch(b.var("StartPaused"))
        b.chain(b.event("Event.Create"), steps)
        b.flow(steps["Then0"], starts)
        b.flow(starts["True"], b.send("TogglePause", b.self_()))
        children = b.pure("Get:Strada.Entity.Children", "Value", Target=b.self_())
        found = b.set("Sounds", b.add("Array.Get", None, "Element", Array=children, Index=0).out,
                      comment="The director's children: the bike's sounds, then the crowd")
        voices = b.pure("Get:Strada.Entity.Children", "Value", Target=b.var("Sounds"))
        each = [b.set(name, b.add("Array.Get", None, "Element", Array=voices, Index=index).out)
                for index, name in enumerate(("Engine", "Squeal", "Crunch", "Wind", "Blip"))]
        plays = b.branch(b.var("RideSounds"))
        crowd = b.add("Flow.ForEach", Array=b.pure("Get:Strada.Entity.Children", "Value",
                                                    Target=b.add("Array.Get", None, "Element", Array=children, Index=1).out))
        murmur = b.call("Set:Strada.AudioSourceComponent.Volume", Target=crowd["Element"], Value=CROWD_LEVEL)
        b.chain(steps["Then1"], found, *each, plays)
        b.flow(plays["True"], crowd)
        b.flow(crowd["Body"], murmur)
    with author.region(AUDIO_DIRECTOR, "Ride: every update, the bike's sounds from the vehicle", EVENTS) as b:
        plays = b.branch(b.both(b.var("RideSounds"), b.pure("IsValid", Value=b.var("Bike"))),
                         comment="Off, the ride's sounds stay silent")
        steps = b.sequence(len(RIDE_FUNCTIONS))
        b.chain(b.event("Event.Update"), plays)
        b.flow(plays["True"], steps)
        for index, name in enumerate(RIDE_FUNCTIONS):
            b.flow(steps[f"Then{index}"], b.own(name))
    with author.region(AUDIO_DIRECTOR, "Toggle pause: the mix blends to the Paused snapshot, or back", EFFECTS) as b:
        toggle = b.custom("TogglePause")
        paused = b.branch(b.var("Paused"))
        resume = b.call("Call:Strada.Audio.TransitionTo(string,float)", Snapshot="", Seconds=b.var("Seconds"),
                        comment="Empty: back to the mixer's own values")
        muffle = b.call("Call:Strada.Audio.TransitionTo(string,float)", Snapshot=PAUSED, Seconds=b.var("Seconds"))
        b.chain(toggle, paused)
        b.chain(paused["True"], resume, b.set("Paused", False))
        b.chain(paused["False"], muffle, b.set("Paused", True))

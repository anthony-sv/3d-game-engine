"""Moto's mix: the mixer its sounds play through (Audio/Moto.smixer, which Moto.sproj names), the echo under the
footbridge and the audio director, written by Author.py's sound part with the sounds GenerateAudio.py makes.

The buses: Master, with a limiter at CEILING, over Engine (the bikes' engines), Tyres (squeal, and later the gravel's
crunch), Ambience (the wind and the crowd), Interface (the menus' and the HUD's sounds) and Reverb, which carries only the
reverb's tail: the footbridge's reverb zone sends the sounds under its deck to it. Every bus but Interface (and Master)
runs a low-pass that stands open at OPEN Hz, so that a snapshot can close it.

The Paused snapshot muffles everything but Interface: each of those buses down by PAUSED_DB and its low-pass closed to
PAUSED_CUTOFF Hz, so the menus of a paused game stay clear over a muffled track. A graph blends to it with
Audio.TransitionTo("Paused") and back with Audio.TransitionTo("").

The echo (the Footbridge Echo entity): a reverb zone, a box under the footbridge's deck across the road and its verges,
from the road up to the deck, that sends what sounds inside it to the Reverb bus, fading out over ECHO_FADE meters round
it. The ghost passes under the deck at about 190 km/h (RacingLine.py): the box and its fade span ECHO_SPAN meters of the
road, about 0.4 s of its pass, so the tail it leaves rings on after it is gone.

The audio director (the Audio Director entity, Scripts/AudioDirector.sgraph): the Pause action sends it TogglePause,
which blends the mix to the Paused snapshot over PAUSE_SECONDS, or back to the mixer's own values. A test (or any graph)
sends it the same event, and with StartPaused on it sends itself one when the scene starts (the player's sound test
measures the muffled mix that way). Nothing else pauses yet: the game's own pause comes with its menus.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Circuit  # noqa: E402  (the layout)
import Graphs  # noqa: E402  (writing graphs)
from Graphs import EFFECTS, EVENTS, public  # noqa: E402

MIXER = "Audio/Moto.smixer"
# The limiter's ceiling on Master (dBFS).
CEILING = -1.0
# The low-pass filters' cutoff while open, and while the Paused snapshot closes them (Hz); how much it lowers the buses (dB).
OPEN = 20000.0
PAUSED_CUTOFF = 650.0
PAUSED_DB = -10.0
# The buses that the Paused snapshot muffles, and the index of the low-pass in each one's effects.
MUFFLED = {"Engine": 0, "Tyres": 0, "Ambience": 0, "Reverb": 2}
PAUSED = "Paused"


def low_pass(frequency=OPEN):
    return {"Kind": "LowPass", "Frequency": frequency, "Q": 0.7071}


def bus(name, volume, effects=(), parent="Master"):
    return {"Name": name, "Parent": parent, "VolumeDb": volume, "Muted": False, "Effects": list(effects), "DuckedBy": ""}


def mixer():
    """The mixer's fields, all of them, so that writing them again over the asset gives the same file."""
    buses = [
        bus("Master", 0.0, [{"Kind": "Limiter", "CeilingDb": CEILING, "ReleaseSeconds": 0.1}], parent=""),
        bus("Engine", 0.0, [low_pass()]),
        bus("Tyres", -4.0, [low_pass()]),
        bus("Ambience", -8.0, [low_pass()]),
        bus("Interface", 0.0),
        # A tail alone (no dry signal): what reaches it is already heard on its own bus. Its low end is cut, so the echo
        # under the deck does not boom.
        bus("Reverb", 0.0, [{"Kind": "HighPass", "Frequency": 150.0, "Q": 0.7071},
                            {"Kind": "Reverb", "RoomSize": 0.55, "Damping": 0.45, "Width": 1.0, "WetDb": 0.0, "DryDb": -80.0},
                            low_pass()]),
    ]
    paused = []
    for name, index in MUFFLED.items():
        paused.append({"Bus": name, "Target": "Volume", "Value": PAUSED_DB})
        paused.append({"Bus": name, "Target": "Effect", "Effect": index, "Parameter": "Frequency", "Value": PAUSED_CUTOFF})
    return {"Buses": buses, "Snapshots": [{"Name": PAUSED, "Values": paused}], "SpeedOfSound": 343.0,
            "Impacts": {"Bus": "", "MinImpulse": 1.0, "FullImpulse": 20.0, "MaxPerFrame": 8}}


# --- The echo under the footbridge ---------------------------------------------------------------------------------------

ECHO = "Footbridge Echo"
# The box's half extents (meters): across the road to beyond its verges, from the road up to the deck's underside, and
# along the road a little beyond the deck (3.2 m deep); and how far round it the send fades out.
ECHO_HALF_EXTENTS = (Circuit.HALF_WIDTH + Circuit.KERB_WIDTH + Circuit.VERGE_WIDTH + 2.0, Circuit.BRIDGE_CLEARANCE / 2.0, 3.0)
ECHO_FADE = 8.0
ECHO_SEND = 0.8
ECHO_SPAN = 2.0 * (ECHO_HALF_EXTENTS[2] + ECHO_FADE)


def echo():
    """The Footbridge Echo's components: the box centered under the deck, turned with the road."""
    return {"Transform": {"Translation": [round(value, 4) for value in Circuit.offset_point(Circuit.FOOTBRIDGE, 0.0,
                                                                                           ECHO_HALF_EXTENTS[1])],
                          "RotationEuler": [0.0, Circuit.yaw(Circuit.FOOTBRIDGE), 0.0]},
            "ReverbZone": {"Shape": "Box", "HalfExtents": list(ECHO_HALF_EXTENTS), "FadeDistance": ECHO_FADE, "Bus": "Reverb",
                           "SendLevel": ECHO_SEND}}


# --- The audio director ------------------------------------------------------------------------------------------------

AUDIO_DIRECTOR = Graphs.script("AudioDirector")
DIRECTOR = "Audio Director"
PAUSE_SECONDS = 0.5


def declare(author):
    author.declare(AUDIO_DIRECTOR, "The audio director: the Pause action (or a TogglePause event any graph sends) blends the mix "
                   "to the Paused snapshot and back.", variables=(
        public("Seconds", "float", PAUSE_SECONDS, "How long the mix takes to blend to the Paused snapshot, and back",
               (0.0, 10.0)),
        public("Paused", "bool", False, "Whether the mix is muffled by the Paused snapshot (a read-out)"),
        public("StartPaused", "bool", False, "Whether the mix starts muffled, blending to the Paused snapshot when the scene "
               "starts (a game that opens paused)"),
    ), events=(("TogglePause", ()),))


def write(author):
    with author.region(AUDIO_DIRECTOR, "Start: a director that starts paused muffles the mix", EVENTS) as b:
        starts = b.branch(b.var("StartPaused"))
        b.chain(b.event("Event.Create"), starts)
        b.flow(starts["True"], b.send("TogglePause", b.self_()))
    with author.region(AUDIO_DIRECTOR, "Pause: the Pause action sends the director TogglePause", EVENTS) as b:
        b.chain(b.event("Event.ActionPressed", {"Action": "Pause"}), b.send("TogglePause", b.self_()))
    with author.region(AUDIO_DIRECTOR, "Toggle pause: the mix blends to the Paused snapshot, or back", EFFECTS) as b:
        toggle = b.custom("TogglePause")
        paused = b.branch(b.var("Paused"))
        resume = b.call("Call:Strada.Audio.TransitionTo(string,float)", Snapshot="", Seconds=b.var("Seconds"),
                        comment="Empty: back to the mixer's own values")
        muffle = b.call("Call:Strada.Audio.TransitionTo(string,float)", Snapshot=PAUSED, Seconds=b.var("Seconds"))
        b.chain(toggle, paused)
        b.chain(paused["True"], resume, b.set("Paused", False))
        b.chain(paused["False"], muffle, b.set("Paused", True))

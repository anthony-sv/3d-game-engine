"""Moto's mix: the mixer its sounds play through (Audio/Moto.smixer, which Moto.sproj names), written by Author.py's sound
part with the sounds GenerateAudio.py makes.

The buses: Master, with a limiter at CEILING, over Engine (the bikes' engines), Tyres (squeal, and later the gravel's
crunch), Ambience (the wind and the crowd), Interface (the menus' and the HUD's sounds) and Reverb, which carries only the
reverb's tail: the footbridge's reverb zone sends the sounds under its deck to it. Every bus but Interface (and Master)
runs a low-pass that stands open at OPEN Hz, so that a snapshot can close it.

The Paused snapshot muffles everything but Interface: each of those buses down by PAUSED_DB and its low-pass closed to
PAUSED_CUTOFF Hz, so the menus of a paused game stay clear over a muffled track. A graph blends to it with
Audio.TransitionTo("Paused") and back with Audio.TransitionTo("").
"""

MIXER = "Audio/Moto.smixer"
# The limiter's ceiling on Master (dBFS).
CEILING = -1.0
# The low-pass filters' cutoff while open, and while the Paused snapshot closes them (Hz); how much it lowers the buses (dB).
OPEN = 20000.0
PAUSED_CUTOFF = 650.0
PAUSED_DB = -10.0
# The buses that the Paused snapshot muffles, and the index of the low-pass in each one's effects.
MUFFLED = {"Engine": 0, "Tyres": 0, "Ambience": 0, "Reverb": 2}


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
    return {"Buses": buses, "Snapshots": [{"Name": "Paused", "Values": paused}], "SpeedOfSound": 343.0,
            "Impacts": {"Bus": "", "MinImpulse": 1.0, "FullImpulse": 20.0, "MaxPerFrame": 8}}

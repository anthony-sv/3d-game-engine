"""Moto's engine sound: the sixteen layers GenerateAudio.py makes (eight engine speeds, each on load and off load), each a
looping voice of its own on the mixer's Engine bus, crossfaded by a graph (Scripts/EngineSound.sgraph) from the engine's
speed and load. Author.py's sound part writes the graph and its curves; its ghost part puts the Engine Sound entity,
with a child for each voice, under the ghost bike, whose graph sets the engine sound's EngineSpeed and Load from its
racing line every update, and the sound part puts the ridden bike's (BIKE_ENTITY, under the audio director, Sound.py),
which the audio director feeds from the vehicle's engine speed and throttle. The graph's inputs are variables, so any
graph that runs an engine drives it the same way.

How the layers are mixed, every update, for each voice:

- by engine speed: where the engine speed lies between the layers, in layers (x = log(speed / layer speed) /
  log(LAYER_RATIO): 0 at the layer's own speed, 1 at the next one's), is read through the curve asset LAYER_FADE, an
  equal-power crossfade (cos(x pi / 2) from -1 to 1, nothing beyond), so of the two layers either side of the engine
  speed the one nearer sounds the louder, and their powers sum to 1. Below the lowest layer and above the highest, those
  layers play alone at full volume;
- by load: the on-load voices by the curve ON_LOAD of the load (sin(load pi / 2)), the off-load ones by OFF_LOAD
  (OFF_LOAD_GAIN cos(load pi / 2): an engine braking on a closed throttle sounds OFF_LOAD_DB below one pulling), so a
  voice's volume is its fade times its load gain (times the graph's Volume); the on-load voices' powers sum to Volume
  squared times sin^2(load pi / 2) and the off-load ones' to Volume squared times OFF_LOAD_GAIN^2 cos^2(load pi / 2), so
  the first sum and the second over OFF_LOAD_GAIN^2 add up to Volume squared at any speed and load;
- pitched within its band: played at the engine speed over the layer's own (the exact speed GenerateAudio.py recorded
  it at), held within a layer's step either way (1 / LAYER_RATIO to LAYER_RATIO), the speeds at which it fades out.

The voices are spatial and take doppler (the graph's DopplerLevel, given to every voice when the scene starts), so the
note rises as the bike comes and falls as it goes; they play from the start at volume 0, and stay silent while the
engine speed is 0 (an engine that does not turn, or one whose speed has not been set yet: the graphs of an entity and
of its child update in no stated order).
check() evaluates the curves as the engine does and asserts the powers' sums over the engine's range and every load.
"""
import bisect
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import GenerateAudio  # noqa: E402  (the layers)
import Graphs  # noqa: E402  (writing graphs)
from Graphs import EFFECTS, EVENTS, private, public  # noqa: E402

ENGINE_SOUND = Graphs.script("EngineSound")
# The entity under the ghost that runs the graph, where the engine is on the bike (bike-local meters), and the ridden
# bike's (a name of its own: tools and tests find the ghost's by its name).
ENTITY = "Engine Sound"
BIKE_ENTITY = "Bike Engine"
OFFSET = (0.0, 0.5, 0.0)
# The layers' exact speeds (rpm): what each file plays (GenerateAudio.layer_cycles), and the step between them.
LAYER_SPEEDS = tuple(round(GenerateAudio.layer_cycles(rpm)[2], 2) for rpm in GenerateAudio.LAYER_SPEEDS)
LAYER_RATIO = GenerateAudio.LAYER_RATIO
LAYERS = GenerateAudio.LAYER_COUNT
# The voices, in the order of the Engine Sound's children (the graph reads a voice's layer from its index): the on-load
# layers from the lowest, then the off-load ones.
VOICES = tuple((f"{kind} {index}", f"asset://Audio/Engine/{kind}{index}.wav") for kind in ("On", "Off")
               for index in range(1, LAYERS + 1))
# The curve assets, paths relative to Assets.
LAYER_FADE = "Curves/EngineLayerFade.scurve"
ON_LOAD = "Curves/EngineOnLoad.scurve"
OFF_LOAD = "Curves/EngineOffLoad.scurve"
# The curves' keys lie this far apart (their input's units), each with the function's slope: a cubic through them stays
# within about 1e-4 of the function.
CURVE_STEP = 0.125
# How far below the on-load voices the off-load ones play at the same speed (dB): the engine braking on a closed throttle
# is quieter than one pulling, by about what a superbike's intake and exhaust give up off load (FR-R45.3.4 asked for the
# retune: MO3's off-load voices played as loud as the on-load ones). The off-load curve's gain is its amplitude.
OFF_LOAD_DB = -6.0
OFF_LOAD_GAIN = round(10.0 ** (OFF_LOAD_DB / 20.0), 6)
# How far the voices' powers may sum from what the curves give anywhere in the engine's range (the curves are cubics
# through the functions' keys); the test scene's check allows POWER_BAND around it.
POWER_TOLERANCE = 0.002
POWER_BAND = 0.02
# A voice: within this distance (meters) it plays at full volume, and it grows no quieter beyond MAX_DISTANCE. Its
# priority keeps the engine over the other sounds when more play than the engine mixes, the ridden bike's over the
# ghost's.
MIN_DISTANCE = 6.0
MAX_DISTANCE = 600.0
PRIORITY = 200
RIDDEN_PRIORITY = 220


def _keys(function, slope, start, end):
    """A curve's keys from start to end every CURVE_STEP: the function's value and its slope at each, rounded."""
    count = int(round((end - start) / CURVE_STEP))
    keys = []
    for k in range(count + 1):
        x = start + (end - start) * k / count
        gradient = round(slope(x), 6)
        keys.append({"Time": round(x, 6), "Value": round(function(x), 6) + 0.0, "Interpolation": "Cubic", "HasIn": True,
                     "In": gradient + 0.0, "HasOut": True, "Out": gradient + 0.0})
    return keys


def _curve(keys):
    return {"PreExtrapolation": "Hold", "PostExtrapolation": "Hold", "Keys": keys}


def curves():
    """The three curve assets' fields, by path relative to Assets."""
    half = math.pi / 2.0
    return {
        LAYER_FADE: _curve(_keys(lambda x: math.cos(half * x), lambda x: -half * math.sin(half * x), -1.0, 1.0)),
        ON_LOAD: _curve(_keys(lambda x: math.sin(half * x), lambda x: half * math.cos(half * x), 0.0, 1.0)),
        OFF_LOAD: _curve(_keys(lambda x: OFF_LOAD_GAIN * math.cos(half * x), lambda x: -OFF_LOAD_GAIN * half * math.sin(half * x),
                               0.0, 1.0)),
    }


def evaluate(curve, x):
    """A curve's value as the engine evaluates one whose keys all have both slopes (cubic Hermite), held outside them."""
    keys = curve["Keys"]
    times = [key["Time"] for key in keys]
    if x <= times[0]:
        return keys[0]["Value"]
    if x >= times[-1]:
        return keys[-1]["Value"]
    k = bisect.bisect_right(times, x) - 1
    a, b = keys[k], keys[k + 1]
    span = b["Time"] - a["Time"]
    s = (x - a["Time"]) / span
    h00, h10, h01, h11 = 2 * s ** 3 - 3 * s ** 2 + 1, s ** 3 - 2 * s ** 2 + s, -2 * s ** 3 + 3 * s ** 2, s ** 3 - s ** 2
    return h00 * a["Value"] + h10 * span * a["Out"] + h01 * b["Value"] + h11 * span * b["In"]


def layer_position(engine_speed, layer):
    """Where an engine speed lies from a layer, in layers, as the graph reads its fade: held at 0 below the lowest layer
    and above the highest."""
    x = math.log(engine_speed / LAYER_SPEEDS[layer]) / math.log(LAYER_RATIO)
    return min(max(x, 0.0 if layer == 0 else -1.0), 0.0 if layer == LAYERS - 1 else 1.0)


def mix(engine_speed, load, made=None):
    """The voices' volumes and pitches (in VOICES' order) as the graph sets them, at Volume 1."""
    made = made or curves()
    voices = []
    for index in range(2 * LAYERS):
        layer = index % LAYERS
        fade = evaluate(made[LAYER_FADE], layer_position(engine_speed, layer))
        gain = evaluate(made[ON_LOAD if index < LAYERS else OFF_LOAD], load)
        pitch = min(max(engine_speed / LAYER_SPEEDS[layer], 1.0 / LAYER_RATIO), LAYER_RATIO)
        voices.append((fade * gain, pitch))
    return voices


def balance(voices):
    """The on-load voices' power plus the off-load voices' over OFF_LOAD_GAIN squared: 1 at any speed and load (times
    Volume squared), whatever the load's share of each."""
    on = sum(volume * volume for volume, _ in voices[:LAYERS])
    off = sum(volume * volume for volume, _ in voices[LAYERS:])
    return on + off / (OFF_LOAD_GAIN * OFF_LOAD_GAIN)


def check():
    """Asserts that the on-load voices' powers sum to sin^2(load pi / 2) and the off-load ones' to OFF_LOAD_GAIN^2
    cos^2(load pi / 2), each within POWER_TOLERANCE, over the engine's range and every load (so balance() is 1), and that
    each voice that sounds plays within its band; returns the figures."""
    made = curves()
    worst = 0.0
    half = math.pi / 2.0
    for rpm in range(1000, 17001, 25):
        for step in range(11):
            load = step / 10.0
            voices = mix(float(rpm), load, made)
            on = sum(volume * volume for volume, _ in voices[:LAYERS])
            off = sum(volume * volume for volume, _ in voices[LAYERS:])
            worst = max(worst, abs(on - math.sin(half * load) ** 2), abs(off - (OFF_LOAD_GAIN * math.cos(half * load)) ** 2),
                        abs(balance(voices) - 1.0) * OFF_LOAD_GAIN * OFF_LOAD_GAIN)
            for volume, pitch in voices:
                assert volume >= -1e-6, f"a voice's volume is {volume} at {rpm} rpm"
                assert 1.0 / LAYER_RATIO - 1e-9 <= pitch <= LAYER_RATIO + 1e-9, f"a voice plays at {pitch} at {rpm} rpm"
    assert worst <= POWER_TOLERANCE, f"the voices' powers sum {worst:.5f} from the curves'"
    return (f"{len(VOICES)} voices on layers of {', '.join(f'{speed:g}' for speed in LAYER_SPEEDS)} rpm, off load {OFF_LOAD_DB:g} dB; "
            f"their powers sum within {worst:.5f} of the load's from 1,000 to 17,000 rpm at every load")


def voice(clip, priority=PRIORITY):
    """A voice's audio source: its layer, looping from the scene's start, silent until the graph's first update."""
    return {"Clip": clip, "Loop": True, "PlayOnStart": True, "Volume": 0.0, "Spatial": True, "MinDistance": MIN_DISTANCE,
            "MaxDistance": MAX_DISTANCE, "Bus": "Engine", "Priority": priority, "MaxVoices": 1}


# --- The graph ---------------------------------------------------------------------------------------------------------

def declare(author):
    author.declare(ENGINE_SOUND, "An engine's sound: its children's voices, one an engine layer on or off load, crossfaded by "
                   "the engine's speed and load through curve assets, each pitched within its layer's band. Whoever runs "
                   "the engine sets EngineSpeed and Load. Tools/EngineSound.py says how.", variables=(
        public("EngineSpeed", "float", 0.0, "The engine's speed in revolutions a minute: whoever runs the engine sets it every "
               "update", (0.0, 20000.0)),
        public("Load", "float", 0.0, "How far open the throttle is, from 0 (off load) to 1 (full load): whoever runs the "
               "engine sets it every update", (0.0, 1.0)),
        public("Volume", "float", 1.0, "The engine's loudness: every voice's volume is scaled by it", (0.0, 4.0)),
        public("DopplerLevel", "float", 1.0, "The doppler every voice takes when the scene starts: 0 none, 1 physical", (0.0, 5.0)),
        public("LayerFade", "Strada.Curve", f"asset://{LAYER_FADE}", "A layer's volume by where the engine speed lies from it, "
               "in layers (0 at its own speed, 1 at the next one's)"),
        public("OnLoad", "Strada.Curve", f"asset://{ON_LOAD}", "The on-load voices' volume by the load"),
        public("OffLoad", "Strada.Curve", f"asset://{OFF_LOAD}", "The off-load voices' volume by the load"),
        private("Voices", "Strada.Entity[]"),
        private("LayerSpeeds", "float[]", list(LAYER_SPEEDS)),
    ))


def write(author):
    with author.region(ENGINE_SOUND, "Start: the voices are the entity's children, each given the doppler", EVENTS) as b:
        voices = b.set("Voices", b.pure("Get:Strada.Entity.Children", "Value", Target=b.self_()))
        each = b.add("Flow.ForEach", Array=b.var("Voices"))
        doppler = b.call("Set:Strada.AudioSourceComponent.DopplerLevel", Target=each["Element"], Value=b.var("DopplerLevel"))
        b.chain(b.event("Event.Create"), voices, each)
        b.flow(each["Body"], doppler)
    with author.region(ENGINE_SOUND, "Mix: each voice faded by speed and load, and pitched within its band", EFFECTS) as b:
        each = b.add("Flow.ForEach", Array=b.var("Voices"))
        layer = b.expr(f"index % {LAYERS}", index=each["Index"])
        speed = b.add("Array.Get", None, "Element", Array=b.var("LayerSpeeds"), Index=layer).out
        position = b.expr(f"clamp(log(rpm / speed) / {math.log(LAYER_RATIO):.9f}, layer == 0 ? 0.0 : -1.0, "
                          f"layer == {LAYERS - 1} ? 0.0 : 1.0)", rpm=b.var("EngineSpeed"), speed=speed, layer=layer)
        fade = b.pure("Call:Strada.Curve.Evaluate(float)", Target=b.var("LayerFade"), Time=position)
        on = b.pure("Call:Strada.Curve.Evaluate(float)", Target=b.var("OnLoad"), Time=b.var("Load"))
        off = b.pure("Call:Strada.Curve.Evaluate(float)", Target=b.var("OffLoad"), Time=b.var("Load"))
        volume = b.call("Set:Strada.AudioSourceComponent.Volume", Target=each["Element"],
                        Value=b.expr(f"rpm > 0 ? max(fade * (index < {LAYERS} ? on : off) * volume, 0.0) : 0.0", rpm=b.var("EngineSpeed"),
                                     fade=fade, index=each["Index"], on=on, off=off, volume=b.var("Volume")),
                        comment="Equal-power crossfades, off load a little quieter; a stopped engine is silent")
        pitch = b.call("Set:Strada.AudioSourceComponent.Pitch", Target=each["Element"],
                       Value=b.expr(f"clamp(rpm / speed, {1.0 / LAYER_RATIO:.9f}, {LAYER_RATIO:.9f})",
                                    rpm=b.var("EngineSpeed"), speed=speed),
                       comment="The layer played at the engine's speed, within a layer's step of its own")
        b.chain(b.event("Event.Update"), each)
        b.flow(each["Body"], volume)
        b.flow(volume, pitch)


if __name__ == "__main__":
    print(check())

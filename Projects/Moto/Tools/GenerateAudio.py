"""Generates the Moto project's sounds (Assets/Audio/*.wav): the engine of a 90 degree V4 with a twin-pulse firing order, as
eight layers at fixed engine speeds on load (the throttle open) and eight off load (closed, the engine braking), and the
wind, a tyre squeal, a shift blip and a crowd bed. Standard library only; everything is made here, seeded, so the files
have no license of their own.

    python Projects/Moto/Tools/GenerateAudio.py [--output DIRECTORY]
    python Projects/Moto/Tools/GenerateAudio.py --check

--check writes nothing: it makes every file twice in memory, compares the two byte for byte and with the files in
Assets/Audio, and fails when the two differ or when Assets holds something else. Every run, writing or checking, measures
each file and fails when one breaks a promise: its length and rate, its peak (at most PEAK dBFS, at least -12 dBFS), a loop
that closes without a click (the step and the bend across its seam no larger than the largest inside it, its level
across the seam among the levels inside it), a one-shot that starts and ends in silence, an engine layer that repeats every engine cycle
and not every turn of the crank (the twin pulse), and all of them together within BUDGET bytes. It prints what it
measured.

The sounds (mono, 16-bit PCM at RATE samples a second; every loop is periodic by construction: its sources are placed on a
circle of its length, and its filters run round the circle twice, keeping the second turn):

- The engine (Audio/Engine/On<n>.wav and Off<n>.wav, n from 1 to 8 at LAYER_SPEEDS): a 90 degree V4 whose crankpins are
  offset so that its cylinders fire at FIRING (0, 90, 290 and 380 degrees of the crank's 720 degree cycle: intervals of
  90, 200, 90 and 340 degrees), two pulses close together, then a long gap, which is what makes it sound like a twin.
  Each firing is a pressure pulse into the exhaust, whose pipes ring at fixed resonances (EXHAUST), with the noise of the
  burning gas on load; off load the pulses are weaker and less even, the exhaust pops now and then, and the gearbox
  whines. A layer is a whole number of engine cycles long, so its engine speed is the cycles over its length (stated by
  the run, within 0.5 % of its nominal speed); a graph plays it pitched within its band (the layers are LAYER_RATIO apart)
  and crossfades it with its neighbours.
- Wind (Audio/Wind.wav): the air over the rider's helmet, filtered noise rising and falling in slow gusts.
- Tyre squeal (Audio/TyreSqueal.wav): a rubbery tone near 950 Hz that wavers, with a hiss.
- Shift blip (Audio/ShiftBlip.wav, a one-shot): the engine blipped for a downshift, its speed swept up and back.
- Crowd (Audio/Crowd.wav): the murmur of a grandstand, voices of filtered noise swelling and fading.
"""
import argparse
import io
import math
import os
import random
import struct
import sys
import wave

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(TOOLS)
SEED = 2026
# Samples a second: half the mixer's 48 kHz, so that its resampling is an even one, with room above the engine's
# highest resonance and the squeal's partials.
RATE = 24000
# The loudest a sample may be, and the level every engine layer is brought to (dBFS).
PEAK = -1.0
LAYER_RMS = -16.0
# A loop's level across its seam (the RMS of SEAM_WINDOW seconds that straddle it) lies within the levels of the windows
# of that length inside it, give or take SEAM_LEVEL_DB.
SEAM_LEVEL_DB = 1.0
SEAM_WINDOW = 0.02
# What every file together may weigh (bytes).
BUDGET = 3 * 1024 * 1024

# --- The engine ----------------------------------------------------------------------------------------------------------

# Where in the crank's 720 degree cycle each cylinder fires (degrees): the twin-pulse order.
FIRING = (0.0, 90.0, 290.0, 380.0)
# The layers' nominal engine speeds (rpm): from near idle to the red line, each LAYER_RATIO above the one before.
LAYER_COUNT = 8
LOWEST_LAYER = 2000.0
HIGHEST_LAYER = 16000.0
LAYER_RATIO = (HIGHEST_LAYER / LOWEST_LAYER) ** (1.0 / (LAYER_COUNT - 1))
LAYER_SPEEDS = tuple(LOWEST_LAYER * LAYER_RATIO ** index for index in range(LAYER_COUNT))
# A layer lasts about this long (seconds): a whole number of engine cycles.
LAYER_SECONDS = 0.8
# The exhaust's resonances: (frequency in Hz, Q, gain on load, gain off load).
EXHAUST = ((92.0, 3.0, 1.0, 0.7), (205.0, 4.0, 0.85, 0.6), (470.0, 5.0, 0.55, 0.45), (1120.0, 6.0, 0.3, 0.2),
           (2550.0, 6.0, 0.12, 0.05))
# A pulse's width (seconds) and strength, on and off load, how much the cylinders differ and how much a pulse varies.
PULSE_WIDTH = {True: 0.0012, False: 0.0008}
PULSE_STRENGTH = {True: 1.0, False: 0.32}
CYLINDER_SPREAD = (1.0, 0.94, 1.05, 0.97)
PULSE_JITTER = {True: 0.04, False: 0.18}
# Off load: the chance that a firing pops in the exhaust, and how loud a pop is against an on-load pulse.
POP_CHANCE = 0.04
POP_STRENGTH = 0.9
# On load, the burning gas's noise (gain, and its band in Hz); off load, the gearbox's whine (gain, and its frequency as a
# multiple of the crank's).
COMBUSTION_NOISE = (0.18, 1500.0, 4000.0)
GEAR_WHINE = (0.035, 23.0)


def layer_cycles(rpm):
    """The engine cycles a layer at a nominal speed holds, its length in samples and the engine speed it plays."""
    cycle = RATE * 120.0 / rpm
    cycles = max(1, round(LAYER_SECONDS * RATE / cycle))
    samples = round(cycles * cycle)
    return cycles, samples, cycles * RATE * 120.0 / samples


# --- Filters (each runs round a loop twice, keeping the second turn, so that it closes) ----------------------------------

def resonator(samples, frequency, q, gain=1.0, circular=True):
    """A two-pole band-pass (constant peak gain) at a frequency."""
    w = 2.0 * math.pi * frequency / RATE
    alpha = math.sin(w) / (2.0 * q)
    a0 = 1.0 + alpha
    b0, b2 = alpha / a0 * gain, -alpha / a0 * gain
    a1, a2 = -2.0 * math.cos(w) / a0, (1.0 - alpha) / a0
    return _biquad(samples, b0, 0.0, b2, a1, a2, circular)


def low_pass(samples, frequency, q=0.7071, circular=True):
    w = 2.0 * math.pi * frequency / RATE
    alpha = math.sin(w) / (2.0 * q)
    cos = math.cos(w)
    a0 = 1.0 + alpha
    return _biquad(samples, (1.0 - cos) / 2.0 / a0, (1.0 - cos) / a0, (1.0 - cos) / 2.0 / a0, -2.0 * cos / a0, (1.0 - alpha) / a0,
                   circular)


def high_pass(samples, frequency, q=0.7071, circular=True):
    w = 2.0 * math.pi * frequency / RATE
    alpha = math.sin(w) / (2.0 * q)
    cos = math.cos(w)
    a0 = 1.0 + alpha
    return _biquad(samples, (1.0 + cos) / 2.0 / a0, -(1.0 + cos) / a0, (1.0 + cos) / 2.0 / a0, -2.0 * cos / a0, (1.0 - alpha) / a0,
                   circular)


def _biquad(samples, b0, b1, b2, a1, a2, circular):
    """Robert Bristow-Johnson's cookbook biquad in direct form I; round a loop twice when circular."""
    x1 = x2 = y1 = y2 = 0.0
    out = [0.0] * len(samples)
    for turn in range(2 if circular else 1):
        for i, x in enumerate(samples):
            y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, x, y1, y
            out[i] = y
    return out


def add_pulse(buffer, center, width, strength):
    """Adds a raised-cosine pulse centered at a fractional sample position, wrapping round the buffer."""
    count = len(buffer)
    half = width * RATE / 2.0
    first, last = math.ceil(center - half), math.floor(center + half)
    for i in range(first, last + 1):
        buffer[i % count] += strength * 0.5 * (1.0 + math.cos(math.pi * (i - center) / half))


def mix(*parts):
    return [sum(values) for values in zip(*parts)]


def scale(samples, gain):
    return [gain * value for value in samples]


# --- The sounds ----------------------------------------------------------------------------------------------------------

def engine_layer(rpm, on_load, generator):
    """One layer of the engine at a nominal speed, on or off load: a loop of whole engine cycles."""
    cycles, count, _ = layer_cycles(rpm)
    cycle = count / cycles
    pulses = [0.0] * count
    envelope = [0.0] * count
    for index in range(cycles):
        for cylinder, angle in enumerate(FIRING):
            center = (index + angle / 720.0) * cycle
            strength = PULSE_STRENGTH[on_load] * CYLINDER_SPREAD[cylinder] * (1.0 + PULSE_JITTER[on_load] * generator.gauss(0.0, 1.0))
            if not on_load and generator.random() < POP_CHANCE:
                # A pop: unburnt fuel lighting in the pipe, short and sharp.
                add_pulse(pulses, center + 0.25 * cycle / 4.0, 0.0004, POP_STRENGTH * (0.6 + 0.4 * generator.random()))
            add_pulse(pulses, center, PULSE_WIDTH[on_load], strength)
            add_pulse(envelope, center + 0.001 * RATE, 0.004, 1.0)
    exhaust = mix(*(resonator(pulses, frequency, q, on if on_load else off) for frequency, q, on, off in EXHAUST))
    parts = [exhaust, scale(low_pass(pulses, 140.0), 1.2 if on_load else 0.8)]
    if on_load:
        gain, low, high = COMBUSTION_NOISE
        noise = [generator.uniform(-1.0, 1.0) * envelope[i] for i in range(count)]
        parts.append(scale(low_pass(high_pass(noise, low), high), gain))
    else:
        gain, multiple = GEAR_WHINE
        turns = cycles * 2 * multiple
        parts.append([gain * math.sin(2.0 * math.pi * turns * i / count) for i in range(count)])
    return high_pass(mix(*parts), 25.0)


def wind(generator, seconds=4.0):
    """The air over the helmet: noise in two bands, rising and falling in gusts that repeat with the loop."""
    count = round(seconds * RATE)
    noise = [generator.uniform(-1.0, 1.0) for _ in range(count)]
    body = low_pass(noise, 380.0)
    whistle = resonator(noise, 900.0, 0.9, 0.35)
    gusts = [(1 + generator.randrange(3), generator.uniform(0.0, 2.0 * math.pi), 0.25 / (1 + k)) for k in range(4)]
    out = []
    for i in range(count):
        level = 0.75 + sum(depth * math.sin(2.0 * math.pi * cycles * i / count + phase) for cycles, phase, depth in gusts)
        out.append(level * (body[i] + whistle[i]))
    return high_pass(out, 30.0)


def tyre_squeal(generator, seconds=2.0, base=950.0):
    """A tyre at its grip's limit: a tone that wavers about base Hz (a whole number of its cycles a loop, so that the
    loop closes), two partials and a hiss."""
    count = round(seconds * RATE)
    frequency = round(base * seconds) / seconds
    wobble = (5, 0.012)
    out = []
    for i in range(count):
        t = i / RATE
        # The phase of a tone whose frequency wavers by a sine of whole cycles a loop: it ends where it started.
        phase = frequency * (t - wobble[1] * seconds / (2.0 * math.pi * wobble[0]) * math.cos(2.0 * math.pi * wobble[0] * i / count))
        swell = 0.8 + 0.2 * math.sin(2.0 * math.pi * 3 * i / count)
        out.append(swell * (math.sin(2.0 * math.pi * phase) + 0.45 * math.sin(4.0 * math.pi * phase + 0.6) +
                            0.18 * math.sin(6.0 * math.pi * phase + 1.3)))
    hiss = resonator([generator.uniform(-1.0, 1.0) for _ in range(count)], 2200.0, 1.2, 0.5)
    return high_pass(mix(out, hiss), 200.0)


def shift_blip(generator, seconds=0.42):
    """A downshift's blip, once: the engine's pulses on load while its speed sweeps from 9,500 rpm up to 13,500 and back,
    faded in and out so that it starts and ends in silence."""
    count = round(seconds * RATE)
    pulses = [0.0] * count
    envelope = [0.0] * count
    angle = 0.0
    firings = sorted(FIRING)
    for i in range(count):
        progress = i / count
        rpm = 9500.0 + 4000.0 * math.sin(math.pi * progress) ** 2
        before = angle
        angle += rpm / 60.0 * 360.0 / RATE
        for turn in range(int(before // 720.0), int(angle // 720.0) + 1):
            for cylinder, firing in enumerate(firings):
                at = turn * 720.0 + firing
                if before < at <= angle:
                    center = i - 1 + (at - before) / (angle - before)
                    strength = CYLINDER_SPREAD[cylinder] * (1.0 + PULSE_JITTER[True] * generator.gauss(0.0, 1.0))
                    add_pulse(pulses, center, PULSE_WIDTH[True], strength)
                    add_pulse(envelope, center + 0.001 * RATE, 0.004, 1.0)
    exhaust = mix(*(resonator(pulses, frequency, q, on, circular=False) for frequency, q, on, _ in EXHAUST))
    out = high_pass(mix(exhaust, scale(low_pass(pulses, 140.0, circular=False), 1.2)), 25.0, circular=False)
    attack, release = round(0.006 * RATE), round(0.08 * RATE)
    for i in range(count):
        gain = min(1.0, i / attack, (count - 1 - i) / release)
        out[i] *= max(0.0, gain) ** 2
    return out


def crowd(generator, seconds=6.0, voices=6):
    """A grandstand's murmur: voices of noise filtered to a speaking voice's bands, each swelling and fading a whole number
    of times a loop, over a low rumble."""
    count = round(seconds * RATE)
    total = [0.0] * count
    for voice in range(voices):
        noise = [generator.uniform(-1.0, 1.0) for _ in range(count)]
        formant = resonator(noise, generator.uniform(450.0, 1100.0), 2.5)
        swell = (2 + generator.randrange(5), generator.uniform(0.0, 2.0 * math.pi))
        for i in range(count):
            total[i] += formant[i] * (0.6 + 0.4 * math.sin(2.0 * math.pi * swell[0] * i / count + swell[1]))
    rumble = low_pass([generator.uniform(-1.0, 1.0) for _ in range(count)], 220.0)
    return high_pass(mix(total, scale(rumble, 1.5)), 60.0)


# --- Levels and files ----------------------------------------------------------------------------------------------------

def decibels(value):
    return 20.0 * math.log10(max(value, 1e-12))


def rms(samples):
    return math.sqrt(sum(value * value for value in samples) / len(samples))


def level(samples, target_rms=None):
    """Scales a sound to a target RMS (dBFS), never past PEAK; without a target, to PEAK."""
    peak = max(abs(value) for value in samples)
    gain = 10.0 ** (PEAK / 20.0) / peak
    if target_rms is not None:
        gain = min(gain, 10.0 ** (target_rms / 20.0) / rms(samples))
    return scale(samples, gain)


def pcm(samples):
    return b"".join(struct.pack("<h", max(-32767, min(32767, round(value * 32767.0)))) for value in samples)


def wav_bytes(samples):
    """A mono 16-bit PCM WAV file of the samples at RATE."""
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(RATE)
        file.writeframes(pcm(samples))
    return buffer.getvalue()


def sounds(seed=SEED):
    """Every sound: {path relative to Assets: (samples as written, whether it loops, nominal engine speed or None)}."""
    generator = random.Random(seed)
    made = {}
    for on_load in (True, False):
        for index, rpm in enumerate(LAYER_SPEEDS, start=1):
            samples = level(engine_layer(rpm, on_load, generator), LAYER_RMS)
            made[f"Audio/Engine/{'On' if on_load else 'Off'}{index}.wav"] = (samples, True, rpm)
    made["Audio/Wind.wav"] = (level(wind(generator), -14.0), True, None)
    made["Audio/TyreSqueal.wav"] = (level(tyre_squeal(generator), -14.0), True, None)
    made["Audio/ShiftBlip.wav"] = (level(shift_blip(generator)), False, None)
    made["Audio/Crowd.wav"] = (level(crowd(generator), -18.0), True, None)
    # As written: 16-bit values read back, so that the checks measure the file.
    return {path: ([value / 32767.0 for value in struct.unpack(f"<{len(samples)}h", pcm(samples))], loops, rpm)
            for path, (samples, loops, rpm) in made.items()}


OUTPUTS = tuple(f"Audio/Engine/{kind}{index}.wav" for kind in ("On", "Off") for index in range(1, LAYER_COUNT + 1)) + \
    ("Audio/Wind.wav", "Audio/TyreSqueal.wav", "Audio/ShiftBlip.wav", "Audio/Crowd.wav")


# --- Checks --------------------------------------------------------------------------------------------------------------

def correlation(samples, lag):
    """The normalized circular autocorrelation of a loop at a lag (samples)."""
    count = len(samples)
    energy = sum(value * value for value in samples)
    return sum(samples[i] * samples[(i + lag) % count] for i in range(count)) / energy


def measure(path, samples, loops, rpm):
    """Asserts a sound's promises; returns a line of what was measured."""
    count = len(samples)
    seconds = count / RATE
    peak = decibels(max(abs(value) for value in samples))
    problems = []
    if peak > PEAK + 0.01 or peak < -12.0:
        problems.append(f"peak {peak:.2f} dBFS")
    figures = f"{path}: {seconds:.3f} s at {RATE} Hz, {44 + 2 * count} bytes, peak {peak:.1f} dBFS, RMS {decibels(rms(samples)):.1f} dBFS"
    if loops:
        steps = [samples[i + 1] - samples[i] for i in range(count - 1)]
        bends = [steps[i + 1] - steps[i] for i in range(count - 2)]
        seam_step = samples[0] - samples[-1]
        seam_bends = (samples[0] - 2.0 * samples[-1] + samples[-2], samples[1] - 2.0 * samples[0] + samples[-1])
        largest_step, largest_bend = max(abs(step) for step in steps), max(abs(bend) for bend in bends)
        window = round(SEAM_WINDOW * RATE)
        across = decibels(rms(samples[-window // 2:] + samples[:window - window // 2]))
        inside = [decibels(rms(samples[start:start + window])) for start in range(0, count - window, window // 4)]
        if abs(seam_step) > largest_step or max(abs(bend) for bend in seam_bends) > largest_bend:
            problems.append(f"a click at the seam: a step of {abs(seam_step):.5f} against {largest_step:.5f} inside")
        if not min(inside) - SEAM_LEVEL_DB <= across <= max(inside) + SEAM_LEVEL_DB:
            problems.append(f"its level across the seam, {across:.1f} dBFS, is not among the {min(inside):.1f} to {max(inside):.1f} inside")
        figures += (f"; the seam steps {abs(seam_step):.4f} (inside at most {largest_step:.4f}), bends "
                    f"{max(abs(bend) for bend in seam_bends):.4f} (at most {largest_bend:.4f}), its level {across:.1f} dBFS "
                    f"(inside {min(inside):.1f} to {max(inside):.1f})")
    else:
        if abs(samples[0]) > 2.0 / 32767.0 or abs(samples[-1]) > 2.0 / 32767.0:
            problems.append("it does not start and end in silence")
        figures += "; starts and ends in silence"
    if rpm is not None:
        cycles, length, played = layer_cycles(rpm)
        cycle = length / cycles
        whole, half = correlation(samples, round(cycle)), correlation(samples, round(cycle / 2.0))
        if abs(played - rpm) > 0.005 * rpm:
            problems.append(f"plays {played:.1f} rpm")
        if whole < 0.6 or half > whole - 0.1:
            problems.append(f"repeats {whole:.2f} each cycle and {half:.2f} each turn")
        figures += (f"; {cycles} cycles at {played:.1f} rpm (nominal {rpm:.0f}), repeats {whole:.2f} each cycle and {half:.2f} "
                    "each turn of the crank")
    if problems:
        raise SystemExit(f"{path}: " + "; ".join(problems))
    return figures


def measure_all(made):
    lines = [measure(path, samples, loops, rpm) for path, (samples, loops, rpm) in made.items()]
    total = sum(44 + 2 * len(samples) for samples, _, _ in made.values())
    if total > BUDGET:
        raise SystemExit(f"the sounds weigh {total} bytes, more than {BUDGET}")
    lines.append(f"{len(made)} sounds, {total} bytes in all (at most {BUDGET})")
    return lines


def generate(assets, seed=SEED, log=print):
    """Writes every sound into the assets folder and prints what it measured."""
    made = sounds(seed)
    for line in measure_all(made):
        log(line)
    for path, (samples, _, _) in made.items():
        target = os.path.join(assets, *path.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as file:
            file.write(wav_bytes(samples))


def check(seed=SEED):
    """Makes every sound twice in memory, then compares the runs with each other and with Assets; writes nothing."""
    first, second = sounds(seed), sounds(seed)
    for line in measure_all(first):
        print(line)
    problems = [path for path in OUTPUTS if wav_bytes(first[path][0]) != wav_bytes(second[path][0])]
    print(f"two runs with seed {seed}: {'the same bytes' if not problems else 'different'} ({len(OUTPUTS)} files)")
    if seed == SEED:
        for path in OUTPUTS:
            committed = os.path.join(PROJECT, "Assets", *path.split("/"))
            if not os.path.exists(committed):
                problems.append(f"{path}: missing from Assets")
                continue
            with open(committed, "rb") as file:
                if file.read() != wav_bytes(first[path][0]):
                    problems.append(f"{path}: differs from Assets")
        if not any("Assets" in problem for problem in problems):
            print("Assets holds what the generator makes")
    if problems:
        raise SystemExit("\n".join(problems))


def main():
    parser = argparse.ArgumentParser(description="Generates the Moto project's sounds.")
    parser.add_argument("--output", help="the folder to write into instead of the project's Assets")
    parser.add_argument("--check", action="store_true", help="make every sound twice and compare the runs and Assets, writing nothing")
    arguments = parser.parse_args()
    if arguments.check:
        check()
    else:
        generate(os.path.abspath(arguments.output) if arguments.output else os.path.join(PROJECT, "Assets"))


if __name__ == "__main__":
    main()

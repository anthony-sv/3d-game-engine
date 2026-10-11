"""Generates the Blocks project's sounds, its beveled block mesh and the grid texture of its board (Assets/Audio, Assets/Meshes,
Assets/Textures), then checks the sounds: their lengths, levels, clean starts and ends, and that each one is what its action
needs (short ticks for moves, a low thump for hard drops, clears that grow with the rows). Everything is computed here,
deterministically, so the files have no license of their own:

    python Projects/Blocks/Tools/GenerateAssets.py
"""
import base64
import cmath
import json
import math
import os
import random
import struct
import wave
import zlib

RATE = 22050
# Effects are short: they use the full rate for crisp transients and sparkle.
EFFECT_RATE = 44100
ASSETS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Assets")


# --- Sound ------------------------------------------------------------------------------------------------------------

def write_wav(name, samples, peak=0.85, rate=RATE):
    loudest = max(1e-9, max(abs(sample) for sample in samples))
    scale = peak / loudest
    path = os.path.join(ASSETS, "Audio", name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(rate)
        file.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, sample * scale)) * 32767)) for sample in samples))


def note(midi):
    return 440.0 * 2.0 ** ((midi - 69) / 12.0)


def square(phase, duty=0.5):
    return 1.0 if (phase % 1.0) < duty else -1.0


def triangle(phase):
    return 4.0 * abs((phase % 1.0) - 0.5) - 1.0


def sine(phase):
    return math.sin(2.0 * math.pi * phase)


def pulse(phase, duty=0.25):
    """A pulse wave without the offset an uneven duty cycle gives it."""
    return square(phase, duty) - (2.0 * duty - 1.0)


def tone(frequency, seconds, wave_shape, volume=1.0, attack=0.005, decay=None, slide=0.0, rate=RATE):
    """One note: a waveform with a short attack and an exponential or linear fade; slide bends the pitch (octaves)."""
    count = int(seconds * rate)
    out = []
    phase = 0.0
    for i in range(count):
        t = i / rate
        progress = i / count
        phase += frequency * 2.0 ** (slide * progress) / rate
        envelope = min(1.0, t / attack) * (math.exp(-t / decay) if decay else 1.0 - progress)
        out.append(volume * envelope * wave_shape(phase))
    return out


def mix(*parts, rate=RATE):
    """Adds sounds, each (start in seconds, samples)."""
    length = max(int(start * rate) + len(samples) for start, samples in parts)
    out = [0.0] * length
    for start, samples in parts:
        offset = int(start * rate)
        for i, sample in enumerate(samples):
            out[offset + i] += sample
    return out


def fade_out(samples, seconds=0.01, rate=RATE):
    count = min(len(samples), int(seconds * rate))
    for i in range(count):
        samples[-1 - i] *= i / count
    return samples


def music():
    """An upbeat minor loop at 128 BPM, 16 bars: a pulse-wave lead, a triangle bass and a noise hi-hat."""
    random.seed(11)
    beat = 60.0 / 128.0
    step = beat / 2.0
    # Melody of eighth notes over the chords A minor, F, C, G (rests as None).
    melody = [
        [76, None, 72, 74, 76, None, 74, 72], [72, None, 69, 72, 74, None, 72, 69],
        [67, None, 64, 67, 72, None, 71, 72], [74, None, 71, 67, 71, 74, None, None],
        [76, 76, 72, 74, 76, 79, 76, 74], [72, None, 69, 72, 74, 72, 69, None],
        [67, 72, 76, 72, 67, 72, 76, 79], [79, None, 78, 76, 74, None, 71, None],
    ] * 2
    roots = [45, 41, 48, 43] * 4
    total_bars = 16
    loop = int(total_bars * 4 * beat * RATE)
    out = [0.0] * (loop + int(RATE))
    for bar in range(total_bars):
        bar_start = bar * 4 * beat
        # Bass: root and octave on eighth notes.
        for i in range(8):
            midi = roots[bar] + (12 if i % 2 else 0)
            samples = tone(note(midi), step * 0.9, triangle, 0.32, decay=0.25)
            offset = int((bar_start + i * step) * RATE)
            for j, sample in enumerate(samples):
                out[offset + j] += sample
        # Lead.
        for i, midi in enumerate(melody[bar % len(melody)]):
            if midi is None:
                continue
            samples = tone(note(midi), step * 0.85, lambda phase: square(phase, 0.25), 0.11, decay=0.18)
            offset = int((bar_start + i * step) * RATE)
            for j, sample in enumerate(samples):
                out[offset + j] += sample
        # Hi-hat on every eighth, a kick on every beat.
        for i in range(8):
            offset = int((bar_start + i * step) * RATE)
            for j in range(int(0.03 * RATE)):
                out[offset + j] += random.uniform(-1.0, 1.0) * 0.05 * (1.0 - j / (0.03 * RATE))
            if i % 2 == 0:
                for j, sample in enumerate(tone(110.0, 0.12, sine, 0.4, decay=0.05, slide=-1.5)):
                    out[offset + j] += sample
    for i, sample in enumerate(out[loop:]):
        out[i] += sample
    return out[:loop]


# --- Effects (EFFECT_RATE) --------------------------------------------------------------------------------------------

def effect_mix(*parts):
    return mix(*parts, rate=EFFECT_RATE)


def bell(frequency, seconds, volume=1.0, decay=0.25, attack=0.002, partials=((1.0, 1.0, 1.0), (2.0, 0.45, 0.6), (3.0, 0.2, 0.4),
                                                                          (4.16, 0.1, 0.25))):
    """A bell-like note: sine partials (frequency ratio, amplitude, decay relative to the note's) that ring out."""
    count = int(seconds * EFFECT_RATE)
    out = [0.0] * count
    for ratio, amplitude, relative in partials:
        # Partials above half the sample rate would fold back as harsh, unrelated tones.
        if frequency * ratio >= EFFECT_RATE / 2.0:
            continue
        step = 2.0 * math.pi * frequency * ratio / EFFECT_RATE
        partial_decay = decay * relative
        for i in range(count):
            t = i / EFFECT_RATE
            out[i] += volume * amplitude * min(1.0, t / attack) * math.exp(-t / partial_decay) * math.sin(step * i)
    return out


def sweep(start, end, seconds, wave_shape, volume=1.0, attack=0.003, decay=None):
    """A note whose pitch glides exponentially from start to end (Hz), with an attack and an exponential or linear fade."""
    count = int(seconds * EFFECT_RATE)
    out = []
    phase = 0.0
    for i in range(count):
        t = i / EFFECT_RATE
        progress = i / count
        phase += start * (end / start) ** progress / EFFECT_RATE
        envelope = min(1.0, t / attack) * (math.exp(-t / decay) if decay else 1.0 - progress)
        out.append(volume * envelope * wave_shape(phase))
    return out


def noise_burst(seconds, volume=1.0, attack=0.001, decay=0.01, seed=0):
    generator = random.Random(seed)
    count = int(seconds * EFFECT_RATE)
    return [volume * min(1.0, (i / EFFECT_RATE) / attack) * math.exp(-(i / EFFECT_RATE) / decay) * generator.uniform(-1.0, 1.0)
            for i in range(count)]


def band_pass(samples, center, q=2.0):
    """A resonant band-pass (RBJ biquad, constant peak gain)."""
    omega = 2.0 * math.pi * center / EFFECT_RATE
    alpha = math.sin(omega) / (2.0 * q)
    b0, b2 = alpha, -alpha
    a0, a1, a2 = 1.0 + alpha, -2.0 * math.cos(omega), 1.0 - alpha
    out = []
    x1 = x2 = y1 = y2 = 0.0
    for x in samples:
        y = (b0 * x + b2 * x2 - a1 * y1 - a2 * y2) / a0
        out.append(y)
        x2, x1 = x1, x
        y2, y1 = y1, y
    return out


def low_pass(samples, cutoff):
    """A one-pole low-pass."""
    k = 1.0 - math.exp(-2.0 * math.pi * cutoff / EFFECT_RATE)
    out = []
    y = 0.0
    for x in samples:
        y += k * (x - y)
        out.append(y)
    return out


def saturate(samples, drive):
    """Soft clipping that thickens a sound without hard edges."""
    norm = math.tanh(drive)
    return [math.tanh(drive * sample) / norm for sample in samples]


def finish(samples, fade=0.012):
    """Ends the sound smoothly."""
    return fade_out(samples, fade, EFFECT_RATE)


def move_sound():
    """A soft, short tick: it repeats quickly while a key is held."""
    return finish(effect_mix((0.0, bell(1480.0, 0.045, 0.8, decay=0.014, partials=((1.0, 1.0, 1.0), (2.7, 0.3, 0.5)))),
                             (0.0, band_pass(noise_burst(0.01, 0.6, decay=0.0015, seed=1), 4200.0, 1.5))), 0.008)


def rotate_sound():
    """A quick rising blip with a breath of air: the piece swings round."""
    blip = sweep(620.0, 1500.0, 0.085, triangle, 0.9, attack=0.002, decay=0.03)
    air = band_pass(noise_burst(0.07, 0.5, attack=0.012, decay=0.02, seed=2), 3200.0, 1.2)
    return finish(effect_mix((0.0, blip), (0.0, air)))


def hold_sound():
    """A swap: down, then up, with a soft whoosh, as a piece goes aside and another comes back."""
    down = sweep(1050.0, 520.0, 0.07, triangle, 0.8, attack=0.004, decay=0.05)
    up = sweep(520.0, 1250.0, 0.09, triangle, 0.85, attack=0.004, decay=0.05)
    whoosh = band_pass(noise_burst(0.17, 0.45, attack=0.05, decay=0.06, seed=3), 1600.0, 0.9)
    return finish(effect_mix((0.0, down), (0.065, up), (0.0, whoosh)), 0.02)


def soft_drop_sound():
    """A muted low tick for each row a soft drop moves the piece."""
    body = bell(240.0, 0.035, 1.0, decay=0.009, partials=((1.0, 1.0, 1.0), (2.0, 0.3, 0.6)))
    click = band_pass(noise_burst(0.006, 0.4, decay=0.001, seed=4), 2400.0, 1.0)
    return finish(effect_mix((0.0, body), (0.0, click)), 0.006)


def lock_sound():
    """A woody clack: a piece settles."""
    knock = band_pass(noise_burst(0.05, 1.0, decay=0.012, seed=5), 1900.0, 3.0)
    body = bell(210.0, 0.12, 0.9, decay=0.04, partials=((1.0, 1.0, 1.0), (2.03, 0.5, 0.6), (3.1, 0.2, 0.4)))
    return finish(effect_mix((0.0, knock), (0.0, body)))


def drop_sound():
    """The hard drop: a solid, low thump with a knock on top, so it lands on small speakers too."""
    count = int(0.42 * EFFECT_RATE)
    phase = 0.0
    thump = []
    for i in range(count):
        t = i / EFFECT_RATE
        # The pitch falls fast from a punch to a low body.
        frequency = 46.0 + 120.0 * math.exp(-t / 0.035)
        phase += frequency / EFFECT_RATE
        thump.append(min(1.0, t / 0.0015) * math.exp(-t / 0.11) * math.sin(2.0 * math.pi * phase))
    knock = bell(300.0, 0.08, 0.35, decay=0.02, partials=((1.0, 1.0, 1.0), (2.4, 0.4, 0.5)))
    crack = low_pass(noise_burst(0.03, 0.6, decay=0.006, seed=6), 2500.0)
    return finish(saturate(effect_mix((0.0, thump), (0.0, knock), (0.0, crack)), 1.8), 0.03)


def chime(notes, spacing, decay, volume=0.8, brightness=1.0):
    """Notes rung one after another (MIDI numbers)."""
    partials = ((1.0, 1.0, 1.0), (2.0, 0.45 * brightness, 0.6), (3.0, 0.22 * brightness, 0.4), (4.16, 0.1 * brightness, 0.25))
    return effect_mix(*[(index * spacing, bell(note(midi), decay * 5.0, volume, decay=decay, partials=partials))
                        for index, midi in enumerate(notes)])


def sparkle(seconds, count, seed, volume=0.18):
    """High glints at random moments."""
    generator = random.Random(seed)
    parts = [(generator.uniform(0.0, seconds * 0.7), bell(note(generator.choice((96, 100, 103, 108))), 0.25, volume, decay=0.05))
             for _ in range(count)]
    return effect_mix(*parts)


def pad(notes, seconds, volume, decay):
    """A soft body under a chime: sine notes (MIDI numbers) that swell in and fade."""
    count = int(seconds * EFFECT_RATE)
    out = [0.0] * count
    for midi in notes:
        step = 2.0 * math.pi * note(midi) / EFFECT_RATE
        for i in range(count):
            t = i / EFFECT_RATE
            out[i] += volume * min(1.0, t / 0.03) * math.exp(-t / decay) * math.sin(step * i)
    return out


def single_sound():
    return finish(chime((72, 79), 0.06, 0.07, brightness=0.8), 0.03)


def double_sound():
    return finish(effect_mix((0.0, chime((72, 76, 79), 0.055, 0.1)), (0.0, pad((60, 67), 0.6, 0.25, 0.18)),
                             (0.08, sparkle(0.3, 3, 7, 0.1))), 0.03)


def triple_sound():
    bright = chime((72, 76, 79, 84), 0.05, 0.12, brightness=1.2)
    body = effect_mix(*[(0.0, tone(note(midi), 0.45, pulse, 0.12, attack=0.01, decay=0.15, rate=EFFECT_RATE))
                        for midi in (60, 64)])
    return finish(effect_mix((0.0, bright), (0.0, low_pass(body, 2500.0)), (0.0, pad((48, 60, 67), 0.8, 0.22, 0.24)),
                             (0.1, sparkle(0.4, 5, 8, 0.12))), 0.04)


def tetris_sound():
    """Four rows at once: a fast run up two octaves over a swelling chord, a low boom and glints."""
    run = chime((60, 64, 67, 72, 76, 79, 84), 0.035, 0.16, 0.7, brightness=1.2)
    chord = []
    count = int(1.3 * EFFECT_RATE)
    phases = [0.0] * 8
    voices = [note(midi) * detune for midi in (60, 64, 67, 72) for detune in (0.997, 1.003)]
    for i in range(count):
        t = i / EFFECT_RATE
        envelope = min(1.0, t / 0.05) * math.exp(-t / 0.55)
        value = 0.0
        for v, frequency in enumerate(voices):
            phases[v] += frequency / EFFECT_RATE
            # Sawtooth.
            value += 2.0 * (phases[v] % 1.0) - 1.0
        chord.append(0.1 * envelope * value)
    chord = low_pass(low_pass(chord, 2200.0), 3000.0)
    boom = sweep(90.0, 42.0, 0.5, sine, 0.55, attack=0.002, decay=0.16)
    whoosh = band_pass(noise_burst(0.6, 0.35, attack=0.18, decay=0.14, seed=9), 2600.0, 0.8)
    body = pad((48, 55, 60, 64), 1.3, 0.16, 0.5)
    layers = effect_mix((0.0, boom), (0.0, chord), (0.0, body), (0.02, run), (0.0, whoosh), (0.25, sparkle(0.9, 9, 10, 0.2)))
    return finish(saturate(layers, 1.4), 0.08)


def level_up_sound():
    """A fanfare: a quick run up to a ringing chord with vibrato."""
    run = effect_mix(*[(index * 0.075, tone(note(midi), 0.12, pulse, 0.4, attack=0.004, decay=0.08,
                                            rate=EFFECT_RATE))
                       for index, midi in enumerate((67, 72, 76))])
    count = int(0.75 * EFFECT_RATE)
    held = []
    phases = [0.0, 0.0]
    for i in range(count):
        t = i / EFFECT_RATE
        vibrato = 1.0 + 0.006 * math.sin(2.0 * math.pi * 6.0 * t) * min(1.0, t / 0.2)
        envelope = min(1.0, t / 0.01) * math.exp(-t / 0.35)
        value = 0.0
        for v, midi in enumerate((79, 84)):
            phases[v] += note(midi) * vibrato / EFFECT_RATE
            value += pulse(phases[v])
        held.append(0.22 * envelope * value)
    return finish(effect_mix((0.0, run), (0.225, low_pass(held, 3500.0)), (0.25, sparkle(0.5, 4, 11, 0.1))), 0.06)


def game_over_sound():
    """A falling minor phrase that settles on a low, fading note."""
    phrase = effect_mix(*[(index * 0.24, tone(note(midi), 0.36, triangle, 0.7, attack=0.008, decay=0.22, rate=EFFECT_RATE))
                          for index, midi in enumerate((67, 63, 60))])
    count = int(1.1 * EFFECT_RATE)
    low = []
    phases = [0.0, 0.0]
    for i in range(count):
        t = i / EFFECT_RATE
        vibrato = 1.0 + 0.008 * math.sin(2.0 * math.pi * 4.5 * t)
        envelope = min(1.0, t / 0.01) * math.exp(-t / 0.42)
        value = 0.0
        for v, midi in enumerate((43, 55)):
            phases[v] += note(midi) * vibrato / EFFECT_RATE
            value += triangle(phases[v])
        low.append(0.45 * envelope * value)
    rumble = low_pass(noise_burst(0.9, 0.5, attack=0.05, decay=0.3, seed=12), 180.0)
    return finish(effect_mix((0.0, phrase), (0.72, low), (0.72, rumble)), 0.1)


def start_sound():
    """A power-up: a rising sweep into a bright chord."""
    rise = sweep(note(57), note(81), 0.24, pulse, 0.35, attack=0.01)
    stab = chime((72, 76, 79, 84), 0.0, 0.14, 0.6)
    return finish(effect_mix((0.0, low_pass(rise, 3000.0)), (0.22, stab), (0.24, sparkle(0.3, 3, 13, 0.1))), 0.05)


def pause_sound():
    """Two soft notes: the game holds (played higher when it goes on)."""
    return finish(chime((76, 71), 0.07, 0.06, 0.7, brightness=0.6), 0.02)


EFFECTS = {
    "Move.wav": move_sound,
    "Rotate.wav": rotate_sound,
    "Hold.wav": hold_sound,
    "SoftDrop.wav": soft_drop_sound,
    "Lock.wav": lock_sound,
    "Drop.wav": drop_sound,
    "Single.wav": single_sound,
    "Double.wav": double_sound,
    "Triple.wav": triple_sound,
    "Tetris.wav": tetris_sound,
    "LevelUp.wav": level_up_sound,
    "GameOver.wav": game_over_sound,
    "Start.wav": start_sound,
    "Pause.wav": pause_sound,
}

# What each effect must be, checked on the written files: (shortest, longest) in seconds, and the highest spectral centroid
# in Hz (where the sound's energy sits) for the low ones.
EXPECTED_LENGTHS = {
    "Move.wav": (0.02, 0.08), "Rotate.wav": (0.05, 0.15), "Hold.wav": (0.1, 0.25), "SoftDrop.wav": (0.015, 0.06),
    "Lock.wav": (0.05, 0.2), "Drop.wav": (0.2, 0.6), "Single.wav": (0.2, 0.7), "Double.wav": (0.3, 0.9),
    "Triple.wav": (0.4, 1.2), "Tetris.wav": (0.9, 2.0), "LevelUp.wav": (0.6, 1.4), "GameOver.wav": (1.2, 2.6),
    "Start.wav": (0.3, 1.0), "Pause.wav": (0.1, 0.5),
}
EFFECT_PEAK = 0.89


# --- Checks -----------------------------------------------------------------------------------------------------------

def read_wav(path):
    with wave.open(path, "rb") as file:
        if file.getnchannels() != 1 or file.getsampwidth() != 2:
            raise SystemExit(f"{path}: expected 16-bit mono")
        rate = file.getframerate()
        frames = file.readframes(file.getnframes())
    return [value / 32767.0 for (value,) in struct.iter_unpack("<h", frames)], rate


def decibels(value):
    return 20.0 * math.log10(max(value, 1e-9))


def spectral_centroid(samples, rate, size=2048, bins=256, top=8000.0):
    """The energy-weighted mean frequency of the sound's start (a Hann-windowed DFT of its first samples)."""
    window = samples[:size]
    count = len(window)
    weighted = [sample * (0.5 - 0.5 * math.cos(2.0 * math.pi * i / max(count - 1, 1))) for i, sample in enumerate(window)]
    total = 0.0
    moment = 0.0
    for k in range(1, bins + 1):
        frequency = top * k / bins
        step = cmath.exp(-2j * math.pi * frequency / rate)
        value = 0j
        rotation = 1 + 0j
        for sample in weighted:
            value += sample * rotation
            rotation *= step
        power = abs(value) ** 2
        total += power
        moment += power * frequency
    return moment / total if total > 0.0 else 0.0


def check_sounds():
    """Reads the effects back and checks them; returns their measurements, or exits naming what is wrong."""
    problems = []
    report = {}
    for name in EFFECTS:
        samples, rate = read_wav(os.path.join(ASSETS, "Audio", name))
        seconds = len(samples) / rate
        peak = max(abs(sample) for sample in samples)
        energy = sum(sample * sample for sample in samples)
        rms = math.sqrt(energy / len(samples))
        dc = sum(samples) / len(samples)
        centroid = spectral_centroid(samples, rate)
        report[name] = {"seconds": seconds, "peak": decibels(peak), "rms": decibels(rms), "energy": energy, "centroid": centroid}
        shortest, longest = EXPECTED_LENGTHS[name]
        if not shortest <= seconds <= longest:
            problems.append(f"{name} lasts {seconds:.3f} s, expected {shortest} to {longest} s")
        if not -1.5 <= decibels(peak) <= -0.5:
            problems.append(f"{name} peaks at {decibels(peak):.1f} dBFS, expected about {decibels(EFFECT_PEAK):.1f}")
        if abs(samples[0]) > 0.02 or abs(samples[-1]) > 0.02:
            problems.append(f"{name} starts or ends with a click ({samples[0]:.3f}, {samples[-1]:.3f})")
        if abs(dc) > 0.02:
            problems.append(f"{name} is off center by {dc:.3f}")
        if rate != EFFECT_RATE:
            problems.append(f"{name} has {rate} Hz, expected {EFFECT_RATE}")
    # A hard drop is a low thump, lower than a lock's clack and the ticks.
    if report["Drop.wav"]["centroid"] > 400.0:
        problems.append(f"Drop.wav is not low: its energy centers at {report['Drop.wav']['centroid']:.0f} Hz")
    for other in ("Lock.wav", "Move.wav", "SoftDrop.wav"):
        if report[other]["centroid"] <= report["Drop.wav"]["centroid"]:
            problems.append(f"{other} is not higher than Drop.wav")
    # Bigger clears last longer and carry more energy.
    clears = ["Single.wav", "Double.wav", "Triple.wav", "Tetris.wav"]
    for smaller, bigger in zip(clears, clears[1:]):
        if report[bigger]["seconds"] <= report[smaller]["seconds"] or report[bigger]["energy"] <= report[smaller]["energy"]:
            problems.append(f"{bigger} is not bigger than {smaller}")
    for name, values in report.items():
        print(f"  {name:13} {values['seconds']:6.3f} s  peak {values['peak']:5.1f} dBFS  rms {values['rms']:6.1f} dBFS  "
              f"centroid {values['centroid']:6.0f} Hz")
    if problems:
        raise SystemExit("the sounds are not as expected:\n  " + "\n  ".join(problems))
    return report


# --- The block mesh: a cube with chamfered edges, flat-shaded ------------------------------------------------------------

def bevel_cube(size=1.0, bevel=0.09):
    """Faces, edge chamfers and corner triangles of a cube of the given size centered on the origin."""
    h = size / 2.0
    i = h - bevel
    positions = []
    normals = []
    indices = []

    def add_polygon(points):
        # Flat shading: every polygon has its own vertices with the polygon's normal.
        a, b, c = points[0], points[1], points[2]
        u = [b[k] - a[k] for k in range(3)]
        v = [c[k] - a[k] for k in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        length = math.sqrt(sum(x * x for x in n))
        n = [x / length for x in n]
        # Outward winding (counter-clockwise seen from outside): flip when the normal points inward.
        center = [sum(point[k] for point in points) / len(points) for k in range(3)]
        if sum(n[k] * center[k] for k in range(3)) < 0.0:
            points = list(reversed(points))
            n = [-x for x in n]
        base = len(positions)
        positions.extend(points)
        normals.extend([n] * len(points))
        for k in range(1, len(points) - 1):
            indices.extend((base, base + k, base + k + 1))

    for axis in range(3):
        for sign in (-1.0, 1.0):
            # The face: the inner square on the plane at +-h.
            u, v = (axis + 1) % 3, (axis + 2) % 3
            corners = []
            for du, dv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                point = [0.0, 0.0, 0.0]
                point[axis] = sign * h
                point[u] = du * i
                point[v] = dv * i
                corners.append(point)
            add_polygon(corners)
    # Edge chamfers: between the faces of two axes, along the third.
    for a in range(3):
        for b in range(a + 1, 3):
            c = 3 - a - b
            for sa in (-1.0, 1.0):
                for sb in (-1.0, 1.0):
                    quad = []
                    for sc in (-1.0, 1.0):
                        for (pa, pb) in ((h, i), (i, h)):
                            point = [0.0, 0.0, 0.0]
                            point[a] = sa * pa
                            point[b] = sb * pb
                            point[c] = sc * i
                            quad.append(point)
                    add_polygon([quad[0], quad[1], quad[3], quad[2]])
    # Corner triangles.
    for sx in (-1.0, 1.0):
        for sy in (-1.0, 1.0):
            for sz in (-1.0, 1.0):
                add_polygon([[sx * h, sy * i, sz * i], [sx * i, sy * h, sz * i], [sx * i, sy * i, sz * h]])
    return positions, normals, indices


def write_mesh():
    positions, normals, indices = bevel_cube()
    buffer = b"".join(struct.pack("<3f", *p) for p in positions) + b"".join(struct.pack("<3f", *n) for n in normals)
    index_offset = len(buffer)
    buffer += b"".join(struct.pack("<H", index) for index in indices)
    while len(buffer) % 4:
        buffer += b"\0"
    count = len(positions)
    document = {
        "asset": {"version": "2.0", "generator": "Blocks/Tools/GenerateAssets.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": "Block", "mesh": 0}],
        "meshes": [{"name": "Block", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}]}],
        "buffers": [{"byteLength": len(buffer), "uri": "data:application/octet-stream;base64," + base64.b64encode(buffer).decode()}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": count * 12}, {"buffer": 0, "byteOffset": count * 12, "byteLength": count * 12},
                        {"buffer": 0, "byteOffset": index_offset, "byteLength": len(indices) * 2}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": count, "type": "VEC3",
             "min": [min(p[k] for p in positions) for k in range(3)], "max": [max(p[k] for p in positions) for k in range(3)]},
            {"bufferView": 1, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
    }
    path = os.path.join(ASSETS, "Meshes", "Block.gltf")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as file:
        json.dump(document, file)


# --- The board's grid: thin glowing lines on black, one cell per tile ------------------------------------------------------

def write_png(path, width, height, pixel):
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                   chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def grid_pixel(x, y, size=128):
    distance = min(x, y, size - 1 - x, size - 1 - y)
    glow = max(0.0, 1.0 - distance / 3.0) ** 2
    value = int(255 * glow)
    return (value, value, value)


if __name__ == "__main__":
    write_wav("Music.wav", music(), 0.75)
    for name, make in EFFECTS.items():
        write_wav(name, make(), EFFECT_PEAK, EFFECT_RATE)
    write_mesh()
    write_png(os.path.join(ASSETS, "Textures", "Grid.png"), 128, 128, grid_pixel)
    print("generated the sounds, Meshes/Block.gltf and Textures/Grid.png in", ASSETS)
    check_sounds()
    print("the sounds are as expected")

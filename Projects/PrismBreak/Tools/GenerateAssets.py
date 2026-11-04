"""Generates the Prism Break project's sounds, its bevelled prism mesh and its textures (Assets/Audio, Assets/Meshes,
Assets/Textures), then checks the sounds: their lengths, levels, clean starts and ends, and that each one is what its moment
needs (a chime on C5 for the graphs to pitch, a low thud for a lost ball, a music loop that ends where it starts).
Everything is computed here, deterministically, so the files have no license of their own:

    python Projects/PrismBreak/Tools/GenerateAssets.py
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

def write_wav(name, samples, peak, rate):
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


def triangle(phase):
    return 4.0 * abs((phase % 1.0) - 0.5) - 1.0


def sine(phase):
    return math.sin(2.0 * math.pi * phase)


def saw(phase):
    return 2.0 * (phase % 1.0) - 1.0


def pulse(phase, duty=0.25):
    """A pulse wave without the offset an uneven duty cycle gives it."""
    return (1.0 if (phase % 1.0) < duty else -1.0) - (2.0 * duty - 1.0)


def tone(frequency, seconds, wave_shape, volume=1.0, attack=0.005, decay=None, slide=0.0, rate=EFFECT_RATE):
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


def mix(*parts, rate=EFFECT_RATE):
    """Adds sounds, each (start in seconds, samples)."""
    length = max(int(start * rate) + len(samples) for start, samples in parts)
    out = [0.0] * length
    for start, samples in parts:
        offset = int(start * rate)
        for i, sample in enumerate(samples):
            out[offset + i] += sample
    return out


def fade_out(samples, seconds, rate=EFFECT_RATE):
    count = min(len(samples), int(seconds * rate))
    for i in range(count):
        samples[-1 - i] *= i / count
    return samples


def bell(frequency, seconds, volume=1.0, decay=0.25, attack=0.002, partials=((1.0, 1.0, 1.0), (2.0, 0.45, 0.6), (3.0, 0.2, 0.4),
                                                                          (4.16, 0.1, 0.25)), rate=EFFECT_RATE):
    """A bell-like note: sine partials (frequency ratio, amplitude, decay relative to the note's) that ring out."""
    count = int(seconds * rate)
    out = [0.0] * count
    for ratio, amplitude, relative in partials:
        # Partials above half the sample rate would fold back as harsh, unrelated tones.
        if frequency * ratio >= rate / 2.0:
            continue
        step = 2.0 * math.pi * frequency * ratio / rate
        partial_decay = decay * relative
        for i in range(count):
            t = i / rate
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


def band_pass(samples, center, q=2.0, rate=EFFECT_RATE):
    """A resonant band-pass (RBJ biquad, constant peak gain)."""
    omega = 2.0 * math.pi * center / rate
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


def low_pass(samples, cutoff, rate=EFFECT_RATE):
    """A one-pole low-pass."""
    k = 1.0 - math.exp(-2.0 * math.pi * cutoff / rate)
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
    return fade_out(samples, fade)


def chime(notes, spacing, decay, volume=0.8, brightness=1.0):
    """Notes rung one after another (MIDI numbers)."""
    partials = ((1.0, 1.0, 1.0), (2.0, 0.45 * brightness, 0.6), (3.0, 0.22 * brightness, 0.4), (4.16, 0.1 * brightness, 0.25))
    return mix(*[(index * spacing, bell(note(midi), decay * 5.0, volume, decay=decay, partials=partials))
                 for index, midi in enumerate(notes)])


def sparkle(seconds, count, seed, volume=0.18):
    """High glints at random moments."""
    generator = random.Random(seed)
    parts = [(generator.uniform(0.0, seconds * 0.7), bell(note(generator.choice((93, 96, 100, 105))), 0.25, volume, decay=0.05))
             for _ in range(count)]
    return mix(*parts)


def paddle_sound():
    """A round, springy bump: the ball meets the paddle."""
    body = sweep(330.0, 180.0, 0.14, sine, 1.0, attack=0.002, decay=0.045)
    click = band_pass(noise_burst(0.012, 0.5, decay=0.002, seed=1), 2600.0, 1.4)
    return finish(mix((0.0, body), (0.0, click), (0.0, bell(660.0, 0.12, 0.25, decay=0.03))))


def wall_sound():
    """A short glassy tick off the arena's neon walls."""
    tick = bell(1760.0, 0.06, 0.8, decay=0.012, partials=((1.0, 1.0, 1.0), (2.76, 0.35, 0.5)))
    air = band_pass(noise_burst(0.02, 0.4, decay=0.003, seed=2), 5200.0, 1.6)
    return finish(mix((0.0, tick), (0.0, air)), 0.008)


def chime_sound():
    """A glass chime on C5 (MIDI 72): Arcade.PitchOf pitches it by semitones for each brick of a combo."""
    partials = ((1.0, 1.0, 1.0), (2.0, 0.32, 0.55), (3.0, 0.16, 0.35), (5.4, 0.06, 0.2))
    ring = bell(note(72), 0.9, 1.0, decay=0.2, attack=0.0015, partials=partials)
    strike = band_pass(noise_burst(0.01, 0.3, decay=0.0015, seed=3), 6000.0, 2.0)
    return finish(mix((0.0, ring), (0.0, strike)), 0.05)


def break_sound():
    """A prism shatters: a bright crack, falling glass and a low body."""
    crack = band_pass(noise_burst(0.12, 1.0, decay=0.03, seed=4), 3800.0, 0.9)
    body = sweep(220.0, 90.0, 0.25, triangle, 0.6, attack=0.002, decay=0.08)
    glass = sparkle(0.4, 7, 5, 0.25)
    return finish(saturate(mix((0.0, crack), (0.0, body), (0.02, glass)), 1.3), 0.04)


def power_up_sound():
    """A capsule caught: a quick major arpeggio up to a shimmer."""
    run = mix(*[(index * 0.055, tone(note(midi), 0.12, pulse, 0.35, attack=0.003, decay=0.07)) for index, midi in enumerate((72, 76, 79, 84))])
    return finish(mix((0.0, low_pass(run, 4000.0)), (0.18, chime((88, 91), 0.05, 0.1, 0.5)), (0.15, sparkle(0.3, 4, 6, 0.12))), 0.04)


def launch_sound():
    """The ball leaves the paddle: a rising whoosh with a ping."""
    rise = sweep(260.0, 1040.0, 0.22, triangle, 0.7, attack=0.004)
    whoosh = band_pass(noise_burst(0.22, 0.4, attack=0.05, decay=0.08, seed=7), 2000.0, 0.8)
    return finish(mix((0.0, rise), (0.0, whoosh), (0.16, bell(note(84), 0.25, 0.4, decay=0.05))), 0.03)


def lose_sound():
    """A ball falls out: a low, falling thud with a fading buzz."""
    count = int(0.9 * EFFECT_RATE)
    phase = 0.0
    thud = []
    for i in range(count):
        t = i / EFFECT_RATE
        frequency = 55.0 + 180.0 * math.exp(-t / 0.08)
        phase += frequency / EFFECT_RATE
        thud.append(min(1.0, t / 0.002) * math.exp(-t / 0.25) * math.sin(2.0 * math.pi * phase))
    buzz = low_pass(tone(note(45), 0.8, saw, 0.25, attack=0.01, decay=0.25, slide=-0.5), 900.0)
    return finish(saturate(mix((0.0, thud), (0.0, buzz)), 1.5), 0.06)


def level_clear_sound():
    """A level is cleared: a rainbow run up the A minor pentatonic into a ringing chord."""
    run = chime((69, 72, 74, 76, 79, 81, 84, 86, 88), 0.06, 0.12, 0.6, brightness=1.1)
    count = int(1.2 * EFFECT_RATE)
    chord = []
    phases = [0.0] * 6
    voices = [note(midi) * detune for midi in (69, 76, 81) for detune in (0.997, 1.003)]
    for i in range(count):
        t = i / EFFECT_RATE
        envelope = min(1.0, t / 0.04) * math.exp(-t / 0.45)
        chord.append(0.09 * envelope * sum(saw(phases[v]) for v in range(6)))
        for v, frequency in enumerate(voices):
            phases[v] += frequency / EFFECT_RATE
    return finish(mix((0.0, run), (0.5, low_pass(chord, 2800.0)), (0.5, sparkle(0.6, 6, 8, 0.15))), 0.08)


def game_over_sound():
    """The last ball is lost: a falling minor phrase that settles on a low, fading note."""
    phrase = mix(*[(index * 0.24, tone(note(midi), 0.36, triangle, 0.7, attack=0.008, decay=0.22)) for index, midi in enumerate((69, 65, 64, 57))])
    low = tone(note(45), 1.2, triangle, 0.5, attack=0.01, decay=0.45)
    rumble = low_pass(noise_burst(1.0, 0.5, attack=0.05, decay=0.3, seed=9), 180.0)
    return finish(mix((0.0, phrase), (0.96, low), (0.96, rumble)), 0.1)


def music():
    """A 32-second synth loop in A minor at 120 BPM, 16 bars (Am, F, C, G): a soft saw pad, a pulse arpeggio of the chords,
    a triangle bass and a quiet beat. The chimes' pentatonic notes fit over every chord."""
    generator = random.Random(11)
    beat = 0.5
    step = beat / 4.0
    chords = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)] * 4
    roots = [33, 29, 36, 31] * 4
    loop = int(16 * 4 * beat * RATE)
    out = [0.0] * (loop + RATE * 2)

    def add(start, samples):
        offset = int(start * RATE)
        for j, sample in enumerate(samples):
            out[offset + j] += sample

    for bar in range(16):
        start = bar * 4 * beat
        chord = chords[bar]
        # The pad: the chord held for the bar, low-passed.
        pad = [0.0] * int(4 * beat * RATE)
        for midi in chord:
            for detune in (0.996, 1.004):
                samples = tone(note(midi + 12) * detune, 4 * beat, saw, 0.03, attack=0.3, decay=None, rate=RATE)
                for j, sample in enumerate(samples):
                    pad[j] += sample
        add(start, low_pass(pad, 1200.0, RATE))
        # The arpeggio: sixteenth notes up and down the chord, an octave up.
        pattern = [0, 1, 2, 1] * 4
        for i, index in enumerate(pattern):
            midi = chord[index] + 24 if i % 8 < 4 else chord[index] + 12
            add(start + i * step, tone(note(midi), step * 0.9, pulse, 0.06, attack=0.003, decay=0.06, rate=RATE))
        # The bass: roots on eighth notes with octave jumps.
        for i in range(8):
            midi = roots[bar] + (12 if i in (3, 7) else 0)
            add(start + i * beat / 2.0, tone(note(midi), beat / 2.0 * 0.85, triangle, 0.3, decay=0.2, rate=RATE))
        # The beat: a kick on every beat, a hi-hat between.
        for i in range(4):
            add(start + i * beat, tone(110.0, 0.14, sine, 0.45, attack=0.002, decay=0.05, slide=-1.6, rate=RATE))
            hat_start = int((start + i * beat + beat / 2.0) * RATE)
            for j in range(int(0.035 * RATE)):
                out[hat_start + j] += generator.uniform(-1.0, 1.0) * 0.04 * (1.0 - j / (0.035 * RATE))
    # What rings past the loop's end comes back at its start, so the loop is seamless.
    for i, sample in enumerate(out[loop:]):
        out[i] += sample
    samples = out[:loop]
    # A few milliseconds of fade at both ends keep the file itself free of clicks where a player starts it.
    edge = int(0.004 * RATE)
    for i in range(edge):
        samples[i] *= i / edge
        samples[-1 - i] *= i / edge
    return samples


EFFECTS = {
    "Paddle.wav": paddle_sound,
    "Wall.wav": wall_sound,
    "Chime.wav": chime_sound,
    "Break.wav": break_sound,
    "PowerUp.wav": power_up_sound,
    "Launch.wav": launch_sound,
    "Lose.wav": lose_sound,
    "LevelClear.wav": level_clear_sound,
    "GameOver.wav": game_over_sound,
}

# What each effect must be, checked on the written files: (shortest, longest) in seconds.
EXPECTED_LENGTHS = {
    "Paddle.wav": (0.08, 0.25), "Wall.wav": (0.03, 0.1), "Chime.wav": (0.6, 1.2), "Break.wav": (0.2, 0.6),
    "PowerUp.wav": (0.3, 0.8), "Launch.wav": (0.2, 0.5), "Lose.wav": (0.6, 1.2), "LevelClear.wav": (1.2, 2.4),
    "GameOver.wav": (1.6, 3.0),
}
EFFECT_PEAK = 0.89
MUSIC_PEAK = 0.75
MUSIC_SECONDS = 32.0


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


def dominant_frequency(samples, rate, low, high, resolution=2.0):
    """The strongest frequency between low and high (Hz) of the sound's first 4096 samples (a Hann-windowed DFT)."""
    window = samples[:4096]
    count = len(window)
    weighted = [sample * (0.5 - 0.5 * math.cos(2.0 * math.pi * i / max(count - 1, 1))) for i, sample in enumerate(window)]
    best, best_power = low, -1.0
    frequency = low
    while frequency <= high:
        step = cmath.exp(-2j * math.pi * frequency / rate)
        value = 0j
        rotation = 1 + 0j
        for sample in weighted:
            value += sample * rotation
            rotation *= step
        power = abs(value)
        if power > best_power:
            best, best_power = frequency, power
        frequency += resolution
    return best


def check_sounds():
    """Reads the sounds back and checks them; exits naming what is wrong."""
    problems = []
    report = {}
    names = list(EFFECTS) + ["Music.wav"]
    for name in names:
        samples, rate = read_wav(os.path.join(ASSETS, "Audio", name))
        seconds = len(samples) / rate
        peak = max(abs(sample) for sample in samples)
        rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples))
        dc = sum(samples) / len(samples)
        report[name] = (seconds, decibels(peak), decibels(rms))
        expected_peak = MUSIC_PEAK if name == "Music.wav" else EFFECT_PEAK
        if not decibels(expected_peak) - 0.5 <= decibels(peak) <= decibels(expected_peak) + 0.5:
            problems.append(f"{name} peaks at {decibels(peak):.1f} dBFS, expected about {decibels(expected_peak):.1f}")
        if abs(samples[0]) > 0.02 or abs(samples[-1]) > 0.02:
            problems.append(f"{name} starts or ends with a click ({samples[0]:.3f}, {samples[-1]:.3f})")
        if abs(dc) > 0.02:
            problems.append(f"{name} is off center by {dc:.3f}")
        if name == "Music.wav":
            if rate != RATE or abs(seconds - MUSIC_SECONDS) > 0.01:
                problems.append(f"Music.wav is {seconds:.3f} s at {rate} Hz, expected {MUSIC_SECONDS} s at {RATE} Hz")
            if decibels(rms) < -30.0:
                problems.append(f"Music.wav is too quiet: {decibels(rms):.1f} dBFS RMS")
            continue
        shortest, longest = EXPECTED_LENGTHS[name]
        if not shortest <= seconds <= longest:
            problems.append(f"{name} lasts {seconds:.3f} s, expected {shortest} to {longest} s")
        if rate != EFFECT_RATE:
            problems.append(f"{name} has {rate} Hz, expected {EFFECT_RATE}")
    # The chime rings on C5, the note the graphs pitch from.
    samples, rate = read_wav(os.path.join(ASSETS, "Audio", "Chime.wav"))
    pitch = dominant_frequency(samples, rate, 400.0, 700.0)
    if abs(pitch - note(72)) > 6.0:
        problems.append(f"Chime.wav rings at {pitch:.0f} Hz, not on C5 ({note(72):.0f} Hz)")
    for name, (seconds, peak, rms) in report.items():
        print(f"  {name:15} {seconds:6.3f} s  peak {peak:5.1f} dBFS  rms {rms:6.1f} dBFS")
    if problems:
        raise SystemExit("the sounds are not as expected:\n  " + "\n  ".join(problems))


# --- The prism mesh: a box with chamfered edges, flat-shaded --------------------------------------------------------------

def bevel_box(size, bevel):
    """Faces, edge chamfers and corner triangles of a box of the given size (x, y, z) centered on the origin."""
    h = [s / 2.0 for s in size]
    inner = [s - bevel for s in h]
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
            u, v = (axis + 1) % 3, (axis + 2) % 3
            corners = []
            for du, dv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                point = [0.0, 0.0, 0.0]
                point[axis] = sign * h[axis]
                point[u] = du * inner[u]
                point[v] = dv * inner[v]
                corners.append(point)
            add_polygon(corners)
    for a in range(3):
        for b in range(a + 1, 3):
            c = 3 - a - b
            for sa in (-1.0, 1.0):
                for sb in (-1.0, 1.0):
                    quad = []
                    for sc in (-1.0, 1.0):
                        for (pa, pb) in ((h[a], inner[b]), (inner[a], h[b])):
                            point = [0.0, 0.0, 0.0]
                            point[a] = sa * pa
                            point[b] = sb * pb
                            point[c] = sc * inner[c]
                            quad.append(point)
                    add_polygon([quad[0], quad[1], quad[3], quad[2]])
    for sx in (-1.0, 1.0):
        for sy in (-1.0, 1.0):
            for sz in (-1.0, 1.0):
                add_polygon([[sx * h[0], sy * inner[1], sz * inner[2]], [sx * inner[0], sy * h[1], sz * inner[2]],
                             [sx * inner[0], sy * inner[1], sz * h[2]]])
    return positions, normals, indices


def write_mesh(name, size, bevel):
    positions, normals, indices = bevel_box(size, bevel)
    buffer = b"".join(struct.pack("<3f", *p) for p in positions) + b"".join(struct.pack("<3f", *n) for n in normals)
    index_offset = len(buffer)
    buffer += b"".join(struct.pack("<H", index) for index in indices)
    while len(buffer) % 4:
        buffer += b"\0"
    count = len(positions)
    document = {
        "asset": {"version": "2.0", "generator": "PrismBreak/Tools/GenerateAssets.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}]}],
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
    path = os.path.join(ASSETS, "Meshes", f"{name}.gltf")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as file:
        json.dump(document, file)


# --- Images ----------------------------------------------------------------------------------------------------------------

def write_png(path, width, height, pixel):
    """Writes an 8-bit PNG of the pixels (RGB tuples, or RGBA ones for images with transparency)."""
    channels = len(pixel(0, 0))
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    color_type = 6 if channels == 4 else 2
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)) +
                   chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def grid_pixel(x, y, size=128):
    """Thin glowing lines on black, one cell per tile: the arena's floor."""
    distance = min(x, y, size - 1 - x, size - 1 - y)
    glow = max(0.0, 1.0 - distance / 2.5) ** 2
    value = int(255 * glow)
    return (value, value, value)


def glow_pixel(x, y, size=128):
    """White light fading out from the middle: behind the title and the banner."""
    dx = (x + 0.5) / size * 2.0 - 1.0
    dy = (y + 0.5) / size * 2.0 - 1.0
    alpha = max(0.0, 1.0 - math.hypot(dx, dy)) ** 2
    return (255, 255, 255, round(255 * alpha))


def spark_pixel(x, y, size=64):
    """A bright core with a soft halo: the particles' sprite."""
    distance = math.hypot((x + 0.5) / size * 2.0 - 1.0, (y + 0.5) / size * 2.0 - 1.0)
    alpha = min(1.0, math.exp(-(distance / 0.18) ** 2) + 0.45 * max(0.0, 1.0 - distance) ** 3)
    return (255, 255, 255, round(255 * alpha))


def life_pixel(x, y, size=64, samples=4):
    """A glowing ball with a highlight: one life of the HUD."""
    hits = 0.0
    shade = 0.0
    for i in range(samples):
        for j in range(samples):
            px = (x + (i + 0.5) / samples) / size * 2.0 - 1.0
            py = (y + (j + 0.5) / samples) / size * 2.0 - 1.0
            if math.hypot(px, py) <= 0.78:
                hits += 1.0
                shade += 0.75 + 0.25 * max(0.0, 1.0 - math.hypot(px + 0.3, py + 0.3) / 0.6)
    coverage = hits / (samples * samples)
    value = round(255 * (shade / hits if hits else 1.0))
    halo = 0.35 * max(0.0, 1.0 - math.hypot((x + 0.5) / size * 2.0 - 1.0, (y + 0.5) / size * 2.0 - 1.0)) ** 2
    return (value, value, value, round(255 * min(1.0, coverage + halo)))


# The brick's mesh, in metres: as large as a brick of the arena, with a bevel that catches the lights.
PRISM_SIZE = (1.35, 0.55, 0.7)
PRISM_BEVEL = 0.08

if __name__ == "__main__":
    write_wav("Music.wav", music(), MUSIC_PEAK, RATE)
    for name, make in EFFECTS.items():
        write_wav(name, make(), EFFECT_PEAK, EFFECT_RATE)
    write_mesh("Prism", PRISM_SIZE, PRISM_BEVEL)
    textures = os.path.join(ASSETS, "Textures")
    write_png(os.path.join(textures, "Grid.png"), 128, 128, grid_pixel)
    write_png(os.path.join(textures, "Glow.png"), 128, 128, glow_pixel)
    write_png(os.path.join(textures, "Spark.png"), 64, 64, spark_pixel)
    write_png(os.path.join(textures, "Life.png"), 64, 64, life_pixel)
    print("generated the sounds, Meshes/Prism.gltf and the textures in", ASSETS)
    check_sounds()
    print("the sounds are as expected")

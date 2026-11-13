"""Generates the starter templates' sounds, their spark mesh and their textures into a project's Assets folder (Audio,
Meshes, Textures), then checks the sounds: their lengths, levels, clean starts and ends, and that each one is what its
moment needs (a pickup on C6 for the graphs to pitch, a hum that loops without a seam). Everything is computed here,
deterministically, so the files have no license of their own and two runs write the same bytes:

    python Templates/Tools/GenerateAssets.py <project directory>
"""
import base64
import json
import math
import os
import random
import struct
import sys
import wave
import zlib

RATE = 44100
# The hum loops all the time: a lower rate keeps the file small, and it has nothing above 2 kHz.
HUM_RATE = 22050


# --- Sound ------------------------------------------------------------------------------------------------------------

def write_wav(path, samples, peak, rate):
    loudest = max(1e-9, max(abs(sample) for sample in samples))
    scale = peak / loudest
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(rate)
        file.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, sample * scale)) * 32767)) for sample in samples))


def note(midi):
    return 440.0 * 2.0 ** ((midi - 69) / 12.0)


def sine(phase):
    return math.sin(2.0 * math.pi * phase)


def triangle(phase):
    return 4.0 * abs((phase % 1.0) - 0.5) - 1.0


def tone(frequency, seconds, wave_shape, volume=1.0, attack=0.005, decay=None):
    """One note: a waveform with a short attack and an exponential or linear fade."""
    count = int(seconds * RATE)
    out = []
    phase = 0.0
    for i in range(count):
        t = i / RATE
        phase += frequency / RATE
        envelope = min(1.0, t / attack) * (math.exp(-t / decay) if decay else 1.0 - i / count)
        out.append(volume * envelope * wave_shape(phase))
    return out


def sweep(start, end, seconds, wave_shape, volume=1.0, attack=0.003, decay=None):
    """A note whose pitch glides exponentially from start to end (Hz)."""
    count = int(seconds * RATE)
    out = []
    phase = 0.0
    for i in range(count):
        t = i / RATE
        progress = i / count
        phase += start * (end / start) ** progress / RATE
        envelope = min(1.0, t / attack) * (math.exp(-t / decay) if decay else 1.0 - progress)
        out.append(volume * envelope * wave_shape(phase))
    return out


def bell(frequency, seconds, volume=1.0, decay=0.25, attack=0.002, partials=((1.0, 1.0, 1.0), (2.0, 0.4, 0.6), (3.0, 0.18, 0.4))):
    """A bell-like note: sine partials (frequency ratio, amplitude, decay relative to the note's) that ring out."""
    count = int(seconds * RATE)
    out = [0.0] * count
    for ratio, amplitude, relative in partials:
        if frequency * ratio >= RATE / 2.0:
            continue
        step = 2.0 * math.pi * frequency * ratio / RATE
        partial_decay = decay * relative
        for i in range(count):
            t = i / RATE
            out[i] += volume * amplitude * min(1.0, t / attack) * math.exp(-t / partial_decay) * math.sin(step * i)
    return out


def noise_burst(seconds, volume=1.0, attack=0.001, decay=0.01, seed=0):
    generator = random.Random(seed)
    count = int(seconds * RATE)
    return [volume * min(1.0, (i / RATE) / attack) * math.exp(-(i / RATE) / decay) * generator.uniform(-1.0, 1.0) for i in range(count)]


def band_pass(samples, center, q=2.0):
    """A resonant band-pass (RBJ biquad, constant peak gain)."""
    omega = 2.0 * math.pi * center / RATE
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
    k = 1.0 - math.exp(-2.0 * math.pi * cutoff / RATE)
    out = []
    y = 0.0
    for x in samples:
        y += k * (x - y)
        out.append(y)
    return out


def mix(*parts):
    """Adds sounds, each (start in seconds, samples)."""
    length = max(int(start * RATE) + len(samples) for start, samples in parts)
    out = [0.0] * length
    for start, samples in parts:
        offset = int(start * RATE)
        for i, sample in enumerate(samples):
            out[offset + i] += sample
    return out


def finish(samples, fade=0.012):
    """Ends the sound smoothly."""
    count = min(len(samples), int(fade * RATE))
    for i in range(count):
        samples[-1 - i] *= i / count
    return samples


def sparkle(seconds, count, seed, volume=0.15):
    """High glints at random moments."""
    generator = random.Random(seed)
    return mix(*[(generator.uniform(0.0, seconds * 0.7), bell(note(generator.choice((96, 100, 103, 108))), 0.2, volume, decay=0.04))
                 for _ in range(count)])


def pickup_sound():
    """A spark is collected: two bright bell notes a fifth apart on C6 (MIDI 84), with glints. The Game pitches it a semitone
    higher for every spark of a run, so a run climbs an octave."""
    first = bell(note(84), 0.5, 0.8, decay=0.09)
    second = bell(note(91), 0.55, 0.7, decay=0.12)
    return finish(mix((0.0, first), (0.06, second), (0.03, sparkle(0.3, 5, 1))), 0.03)


def win_sound():
    """Every spark found: a rising C major run into a ringing chord."""
    run = mix(*[(index * 0.09, bell(note(midi), 0.6, 0.6, decay=0.14)) for index, midi in enumerate((72, 76, 79, 84, 88))])
    chord = mix(*[(0.0, tone(note(midi), 1.3, triangle, 0.22, attack=0.02, decay=0.5)) for midi in (60, 64, 67, 72)])
    return finish(mix((0.0, run), (0.45, low_pass(chord, 3000.0)), (0.45, sparkle(0.8, 8, 2))), 0.08)


def lose_sound():
    """The clock ran out: a falling minor phrase that settles low."""
    phrase = mix(*[(index * 0.2, tone(note(midi), 0.32, triangle, 0.6, attack=0.008, decay=0.2)) for index, midi in enumerate((67, 63, 60))])
    low = tone(note(48), 0.9, triangle, 0.5, attack=0.01, decay=0.35)
    return finish(mix((0.0, phrase), (0.6, low)), 0.08)


def tick_sound():
    """A second of the last ten: a short wooden tick."""
    knock = bell(1250.0, 0.07, 0.8, decay=0.012, partials=((1.0, 1.0, 1.0), (2.6, 0.4, 0.5)))
    air = band_pass(noise_burst(0.015, 0.4, decay=0.002, seed=3), 3500.0, 1.5)
    return finish(mix((0.0, knock), (0.0, air)), 0.008)


def bump_sound():
    """The orb hits a wall: a soft, low thud."""
    body = sweep(170.0, 70.0, 0.22, sine, 1.0, attack=0.002, decay=0.06)
    click = band_pass(noise_burst(0.015, 0.35, decay=0.003, seed=4), 1800.0, 1.2)
    return finish(mix((0.0, body), (0.0, click)), 0.02)


def hum_sound():
    """The orb's hum: one second that loops without a seam (every partial and the tremolo complete whole cycles in it). The
    Orb graph raises its pitch and volume with the orb's speed."""
    count = HUM_RATE
    out = []
    for i in range(count):
        t = i / HUM_RATE
        tremolo = 0.85 + 0.15 * math.sin(2.0 * math.pi * 4.0 * t)
        value = math.sin(2.0 * math.pi * 110.0 * t) + 0.45 * math.sin(2.0 * math.pi * 220.0 * t) + 0.12 * math.sin(2.0 * math.pi * 330.0 * t)
        out.append(tremolo * value)
    return out


EFFECTS = {
    "Pickup.wav": pickup_sound,
    "Win.wav": win_sound,
    "Lose.wav": lose_sound,
    "Tick.wav": tick_sound,
    "Bump.wav": bump_sound,
}
# What each effect must be, checked on the written files: (shortest, longest) in seconds.
EXPECTED_LENGTHS = {"Pickup.wav": (0.4, 0.8), "Win.wav": (1.2, 2.2), "Lose.wav": (1.0, 1.8), "Tick.wav": (0.04, 0.12),
                    "Bump.wav": (0.15, 0.35)}
EFFECT_PEAK = 0.89
HUM_PEAK = 0.6


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


def dominant_frequency(samples, rate, low, high, resolution=4.0):
    """The strongest frequency between low and high (Hz) of the sound's first 2048 samples (a Hann-windowed DFT)."""
    window = samples[:2048]
    count = len(window)
    weighted = [sample * (0.5 - 0.5 * math.cos(2.0 * math.pi * i / max(count - 1, 1))) for i, sample in enumerate(window)]
    best, best_power = low, -1.0
    frequency = low
    while frequency <= high:
        real = sum(sample * math.cos(2.0 * math.pi * frequency * i / rate) for i, sample in enumerate(weighted))
        imaginary = sum(sample * math.sin(2.0 * math.pi * frequency * i / rate) for i, sample in enumerate(weighted))
        power = math.hypot(real, imaginary)
        if power > best_power:
            best, best_power = frequency, power
        frequency += resolution
    return best


def check_sounds(audio):
    """Reads the sounds back and checks them; exits naming what is wrong."""
    problems = []
    for name in list(EFFECTS) + ["Hum.wav"]:
        samples, rate = read_wav(os.path.join(audio, name))
        seconds = len(samples) / rate
        peak = max(abs(sample) for sample in samples)
        dc = sum(samples) / len(samples)
        expected_peak = HUM_PEAK if name == "Hum.wav" else EFFECT_PEAK
        if not decibels(expected_peak) - 0.5 <= decibels(peak) <= decibels(expected_peak) + 0.5:
            problems.append(f"{name} peaks at {decibels(peak):.1f} dBFS, expected about {decibels(expected_peak):.1f}")
        # A loop has no ends to click at: its seam is checked below.
        if name != "Hum.wav" and (abs(samples[0]) > 0.02 or abs(samples[-1]) > 0.02):
            problems.append(f"{name} starts or ends with a click ({samples[0]:.3f}, {samples[-1]:.3f})")
        if abs(dc) > 0.02:
            problems.append(f"{name} is off center by {dc:.3f}")
        if name == "Hum.wav":
            if rate != HUM_RATE or len(samples) != HUM_RATE:
                problems.append(f"Hum.wav is {len(samples)} samples at {rate} Hz, expected one second at {HUM_RATE} Hz")
            # A seamless loop: the step from the last sample back to the first is no larger than the steps inside it.
            largest = max(abs(samples[i + 1] - samples[i]) for i in range(len(samples) - 1))
            if abs(samples[0] - samples[-1]) > largest * 1.05:
                problems.append("Hum.wav does not loop without a seam")
            print(f"  {name:12} {seconds:6.3f} s  peak {decibels(peak):5.1f} dBFS")
            continue
        shortest, longest = EXPECTED_LENGTHS[name]
        if not shortest <= seconds <= longest:
            problems.append(f"{name} lasts {seconds:.3f} s, expected {shortest} to {longest} s")
        if rate != RATE:
            problems.append(f"{name} has {rate} Hz, expected {RATE}")
        print(f"  {name:12} {seconds:6.3f} s  peak {decibels(peak):5.1f} dBFS")
    samples, rate = read_wav(os.path.join(audio, "Pickup.wav"))
    pitch = dominant_frequency(samples, rate, 900.0, 1200.0)
    if abs(pitch - note(84)) > 12.0:
        problems.append(f"Pickup.wav rings at {pitch:.0f} Hz, not on C6 ({note(84):.0f} Hz)")
    if problems:
        raise SystemExit("the sounds are not as expected:\n  " + "\n  ".join(problems))


# --- The spark mesh: a gem of two six-sided pyramids, flat-shaded -----------------------------------------------------------

def gem(radius, top, bottom, girdle, sides=6):
    """The faces of a gem: a ring of `sides` points at height girdle, joined to a top and a bottom point."""
    ring = [[radius * math.cos(2.0 * math.pi * k / sides), girdle, radius * math.sin(2.0 * math.pi * k / sides)] for k in range(sides)]
    apexes = ([0.0, top, 0.0], [0.0, bottom, 0.0])
    positions = []
    normals = []
    indices = []
    for apex in apexes:
        for k in range(sides):
            triangle_points = [apex, ring[k], ring[(k + 1) % sides]]
            a, b, c = triangle_points
            u = [b[i] - a[i] for i in range(3)]
            v = [c[i] - a[i] for i in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            length = math.sqrt(sum(x * x for x in n))
            n = [x / length for x in n]
            # Outward winding (counter-clockwise seen from outside): flip when the normal points inward.
            center = [sum(point[i] for point in triangle_points) / 3.0 for i in range(3)]
            if sum(n[i] * center[i] for i in range(3)) < 0.0:
                triangle_points = [a, c, b]
                n = [-x for x in n]
            base = len(positions)
            positions.extend(triangle_points)
            normals.extend([n] * 3)
            indices.extend((base, base + 1, base + 2))
    return positions, normals, indices


def write_mesh(path, name, positions, normals, indices):
    buffer = b"".join(struct.pack("<3f", *p) for p in positions) + b"".join(struct.pack("<3f", *n) for n in normals)
    index_offset = len(buffer)
    buffer += b"".join(struct.pack("<H", index) for index in indices)
    while len(buffer) % 4:
        buffer += b"\0"
    count = len(positions)
    document = {
        "asset": {"version": "2.0", "generator": "Templates/Tools/GenerateAssets.py"},
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
    """A thin glowing line round each tile, brighter at the corners: the arena's floor."""
    distance = min(x, y, size - 1 - x, size - 1 - y)
    line = max(0.0, 1.0 - distance / 2.0) ** 2
    corner = max(0.0, 1.0 - math.hypot(min(x, size - 1 - x), min(y, size - 1 - y)) / 9.0) ** 2
    value = round(255 * min(1.0, line * 0.8 + corner * 0.6))
    return (value, value, value)


def glow_pixel(x, y, size=64):
    """A bright core with a soft halo: the particles' sprite."""
    distance = math.hypot((x + 0.5) / size * 2.0 - 1.0, (y + 0.5) / size * 2.0 - 1.0)
    alpha = min(1.0, math.exp(-(distance / 0.2) ** 2) + 0.4 * max(0.0, 1.0 - distance) ** 3)
    return (255, 255, 255, round(255 * alpha))


def orb_pixel(x, y, width=256, height=128):
    """The orb's glow: bright, with a band round its middle and four rings through its poles, so its rolling shows."""
    u = (x + 0.5) / width
    v = (y + 0.5) / height
    band = max(0.0, 1.0 - abs(v - 0.5) / 0.035)
    rings = max(0.0, 1.0 - min(abs((u * 4.0) % 1.0), 1.0 - (u * 4.0) % 1.0) / 0.018)
    value = 0.62 + 0.38 * max(band, rings)
    shade = round(255 * value)
    return (shade, shade, shade)


def spark_icon_pixel(x, y, size=64, samples=4):
    """A gold gem with a halo: the HUD's spark counter."""
    coverage = 0.0
    light = 0.0
    for i in range(samples):
        for j in range(samples):
            px = (x + (i + 0.5) / samples) / size * 2.0 - 1.0
            py = (y + (j + 0.5) / samples) / size * 2.0 - 1.0
            if abs(px) / 0.5 + abs(py) / 0.82 <= 1.0:
                coverage += 1.0
                # The facets: the left half and the top catch more light.
                light += 1.0 if px < 0.0 else 0.78
                light -= 0.12 if py > 0.0 else 0.0
    coverage /= samples * samples
    light = light / (coverage * samples * samples) if coverage else 0.0
    halo = 0.4 * max(0.0, 1.0 - math.hypot((x + 0.5) / size * 2.0 - 1.0, (y + 0.5) / size * 2.0 - 1.0)) ** 2
    gold = (1.0, 0.78, 0.3)
    red, green, blue = (round(255 * min(1.0, channel * (light if coverage else 1.0))) for channel in gold)
    return (red, green, blue, round(255 * min(1.0, coverage + halo)))


def generate(project):
    assets = os.path.join(project, "Assets")
    audio = os.path.join(assets, "Audio")
    for name, make in EFFECTS.items():
        write_wav(os.path.join(audio, name), make(), EFFECT_PEAK, RATE)
    write_wav(os.path.join(audio, "Hum.wav"), hum_sound(), HUM_PEAK, HUM_RATE)
    write_mesh(os.path.join(assets, "Meshes", "Spark.gltf"), "Spark", *gem(0.26, 0.42, -0.42, 0.06))
    textures = os.path.join(assets, "Textures")
    write_png(os.path.join(textures, "Grid.png"), 128, 128, grid_pixel)
    write_png(os.path.join(textures, "Glow.png"), 64, 64, glow_pixel)
    write_png(os.path.join(textures, "Orb.png"), 256, 128, orb_pixel)
    write_png(os.path.join(textures, "SparkIcon.png"), 64, 64, spark_icon_pixel)
    print("generated the sounds, Meshes/Spark.gltf and the textures in", assets)
    check_sounds(audio)
    print("the sounds are as expected")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: python Templates/Tools/GenerateAssets.py <project directory>")
    generate(os.path.abspath(sys.argv[1]))

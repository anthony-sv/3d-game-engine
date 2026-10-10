"""Generates the Blocks project's sounds, its beveled block mesh and the grid texture of its board (Assets/Audio, Assets/Meshes,
Assets/Textures). Everything is computed here, deterministically, so the files have no license of their own:

    python Projects/Blocks/Tools/GenerateAssets.py
"""
import base64
import json
import math
import os
import random
import struct
import wave
import zlib

RATE = 22050
ASSETS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Assets")


# --- Sound ------------------------------------------------------------------------------------------------------------

def write_wav(name, samples, peak=0.85):
    loudest = max(1e-9, max(abs(sample) for sample in samples))
    scale = peak / loudest
    path = os.path.join(ASSETS, "Audio", name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(RATE)
        file.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, sample * scale)) * 32767)) for sample in samples))


def note(midi):
    return 440.0 * 2.0 ** ((midi - 69) / 12.0)


def square(phase, duty=0.5):
    return 1.0 if (phase % 1.0) < duty else -1.0


def triangle(phase):
    return 4.0 * abs((phase % 1.0) - 0.5) - 1.0


def tone(frequency, seconds, wave_shape, volume=1.0, attack=0.005, decay=None, slide=0.0):
    """One note: a waveform with a short attack and an exponential or linear fade; slide bends the pitch (octaves)."""
    count = int(seconds * RATE)
    out = []
    phase = 0.0
    for i in range(count):
        t = i / RATE
        progress = i / count
        phase += frequency * 2.0 ** (slide * progress) / RATE
        envelope = min(1.0, t / attack) * (math.exp(-t / decay) if decay else 1.0 - progress)
        out.append(volume * envelope * wave_shape(phase))
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


def fade_out(samples, seconds=0.01):
    count = min(len(samples), int(seconds * RATE))
    for i in range(count):
        samples[-1 - i] *= i / count
    return samples


def music():
    """An upbeat minor loop at 128 BPM, 16 bars: a pulse-wave lead, a triangle bass and a noise hi-hat."""
    random.seed(11)
    beat = 60.0 / 128.0
    step = beat / 2.0
    # Chords per bar (A minor, F, C, G), with a melody of eighth notes over them (rests as None).
    bars = [(57, 53, 48, 55)] * 4
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
                for j, sample in enumerate(tone(110.0, 0.12, lambda phase: math.sin(2.0 * math.pi * phase), 0.4, decay=0.05, slide=-1.5)):
                    out[offset + j] += sample
    for i, sample in enumerate(out[loop:]):
        out[i] += sample
    return out[:loop]


def sounds():
    random.seed(12)
    pulse = lambda duty: (lambda phase: square(phase, duty))  # noqa: E731
    sine = lambda phase: math.sin(2.0 * math.pi * phase)  # noqa: E731
    noise = lambda phase: random.uniform(-1.0, 1.0)  # noqa: E731
    return {
        "Move.wav": fade_out(tone(880.0, 0.035, pulse(0.5), decay=0.02)),
        "Rotate.wav": fade_out(tone(660.0, 0.07, pulse(0.25), slide=0.6, decay=0.05)),
        "Lock.wav": fade_out(mix((0.0, tone(180.0, 0.09, sine, decay=0.04, slide=-0.5)), (0.0, tone(0.0, 0.02, noise, 0.4)))),
        "Drop.wav": fade_out(mix((0.0, tone(150.0, 0.22, sine, decay=0.08, slide=-1.2)), (0.0, tone(0.0, 0.06, noise, 0.5, decay=0.02)))),
        "Clear.wav": fade_out(mix(*[(index * 0.06, tone(note(midi), 0.25, pulse(0.5), 0.5, decay=0.12))
                                    for index, midi in enumerate((72, 76, 79))])),
        "BigClear.wav": fade_out(mix(*[(index * 0.07, tone(note(midi), 0.45, pulse(0.25), 0.5, decay=0.25))
                                       for index, midi in enumerate((72, 76, 79, 84, 88))],
                                     (0.0, tone(0.0, 0.5, noise, 0.15, decay=0.2)))),
        "LevelUp.wav": fade_out(mix(*[(index * 0.09, tone(note(midi), 0.2, pulse(0.5), 0.5, decay=0.15))
                                      for index, midi in enumerate((67, 72, 76, 79, 84))])),
        "GameOver.wav": fade_out(mix(*[(index * 0.22, tone(note(midi), 0.4, triangle, 0.7, decay=0.3))
                                       for index, midi in enumerate((67, 63, 60, 55))])),
    }


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
    for name, samples in sounds().items():
        write_wav(name, samples)
    write_mesh()
    write_png(os.path.join(ASSETS, "Textures", "Grid.png"), 128, 128, grid_pixel)
    print("generated the sounds, Meshes/Block.gltf and Textures/Grid.png in", ASSETS)

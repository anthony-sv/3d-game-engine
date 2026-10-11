"""Draws the Strada icon (a road running into a sunset) and writes the PNG the engine embeds as the default window icon and
the Windows .ico of the executables. Pure Python: run it again after changing the drawing.

    python Tools/GenerateIcon.py
"""

import math
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG_PATH = os.path.join(ROOT, "Strada", "Resources", "Icons", "Strada.png")
ICO_PATH = os.path.join(ROOT, "Strada", "Resources", "Icons", "Strada.ico")
SIZE = 256
SAMPLES = 4
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)

HORIZON = 0.56


def mix(a, b, t):
    t = min(max(t, 0.0), 1.0)
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def hex_color(value):
    return tuple(int(value[i:i + 2], 16) / 255.0 for i in (1, 3, 5))


SKY_TOP = hex_color("#141B3C")
SKY_BOTTOM = hex_color("#B8406A")
GLOW = hex_color("#FF8A4C")
SUN_TOP = hex_color("#FFE27A")
SUN_BOTTOM = hex_color("#FF5E57")
GROUND_NEAR = hex_color("#0B0F24")
GROUND_FAR = hex_color("#1C1840")
GRID = hex_color("#C85BFF")
ROAD_NEAR = hex_color("#2B2F4F")
ROAD_FAR = hex_color("#1B1D38")
EDGE = hex_color("#F4F1FF")
DASH = hex_color("#FFC24A")


def rounded_box_distance(x, y, half, radius):
    qx = abs(x) - half + radius
    qy = abs(y) - half + radius
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    return outside + min(max(qx, qy), 0.0) - radius


def road_half_width(y):
    # Perspective: the road narrows linearly to a point just below the sun's center.
    t = (y - HORIZON) / (1.0 - HORIZON)
    return 0.012 + 0.40 * t


def shade(x, y):
    """Color and coverage of the icon at (x, y) in [0, 1]^2, y down."""
    if rounded_box_distance(x - 0.5, y - 0.5, 0.47, 0.2) > 0.0:
        return None
    if y < HORIZON:
        sky = mix(SKY_TOP, SKY_BOTTOM, (y / HORIZON) ** 1.6)
        # Sun with horizontal bands cut out of its lower half.
        dx, dy = x - 0.5, y - (HORIZON - 0.02)
        r = math.hypot(dx, dy)
        sun_radius = 0.23
        glow = max(0.0, 1.0 - r / 0.45) ** 2 * 0.55
        color = mix(sky, GLOW, glow)
        if r < sun_radius:
            band = (y - (HORIZON - 0.02 - sun_radius)) / (2.0 * sun_radius)
            stripe = False
            if band > 0.3:
                period = 0.045
                phase = ((HORIZON - y) % period) / period
                thickness = 0.08 + 0.9 * (band - 0.3) / 0.22
                stripe = phase < thickness
            if not stripe:
                color = mix(SUN_TOP, SUN_BOTTOM, (y - (HORIZON - 0.02 - sun_radius)) / (sun_radius * 1.3))
        return color
    t = (y - HORIZON) / (1.0 - HORIZON)
    color = mix(GROUND_FAR, GROUND_NEAR, t ** 0.6)
    # Perspective grid on the ground: lines converging to the vanishing point, and rows getting closer near the horizon.
    depth = 1.0 / max(t, 1e-3)
    row = depth % 1.0
    row_width = 0.04 * depth
    lateral = (x - 0.5) / max(t, 1e-3)
    column = (lateral / 0.35) % 1.0
    line = min(row, 1.0 - row) < row_width * 0.5 or min(column, 1.0 - column) < 0.035 / max(t, 0.25)
    if line:
        color = mix(color, GRID, 0.55 * min(1.0, t * 2.0 + 0.2))
    half = road_half_width(y)
    offset = abs(x - 0.5)
    if offset < half:
        color = mix(ROAD_FAR, ROAD_NEAR, t)
        edge = half * 0.12 + 0.004
        if offset > half - edge:
            color = EDGE
        elif offset < half * 0.06 + 0.002:
            # Dashes: evenly spaced in depth, so they shorten towards the horizon.
            if (depth * 1.25) % 1.0 < 0.5:
                color = DASH
    return color


def render(size):
    pixels = bytearray()
    for py in range(size):
        pixels.append(0)
        for px in range(size):
            r = g = b = a = 0.0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    x = (px + (sx + 0.5) / SAMPLES) / size
                    y = (py + (sy + 0.5) / SAMPLES) / size
                    color = shade(x, y)
                    if color is not None:
                        r += color[0]
                        g += color[1]
                        b += color[2]
                        a += 1.0
            if a > 0.0:
                pixels += bytes((round(r / a * 255), round(g / a * 255), round(b / a * 255), round(a / SAMPLES ** 2 * 255)))
            else:
                pixels += bytes(4)
    return bytes(pixels)


def encode_png(size, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def downsample(rows, size, target):
    """Box-filters the RGBA rows (with filter bytes) of a size x size image down to target x target (premultiplied)."""
    stride = size * 4 + 1
    factor = size / target
    result = bytearray()
    for ty in range(target):
        result.append(0)
        for tx in range(target):
            y0, y1 = int(ty * factor), max(int(ty * factor) + 1, int((ty + 1) * factor))
            x0, x1 = int(tx * factor), max(int(tx * factor) + 1, int((tx + 1) * factor))
            r = g = b = a = 0.0
            count = 0
            for y in range(y0, y1):
                for x in range(x0, x1):
                    offset = y * stride + 1 + x * 4
                    alpha = rows[offset + 3] / 255.0
                    r += rows[offset] * alpha
                    g += rows[offset + 1] * alpha
                    b += rows[offset + 2] * alpha
                    a += alpha
                    count += 1
            if a > 0.0:
                result += bytes((round(r / a), round(g / a), round(b / a), round(a / count * 255)))
            else:
                result += bytes(4)
    return bytes(result)


def main():
    rows = render(SIZE)
    os.makedirs(os.path.dirname(PNG_PATH), exist_ok=True)
    png = encode_png(SIZE, rows)
    with open(PNG_PATH, "wb") as file:
        file.write(png)

    # The .ico holds PNG images (supported since Windows Vista) at the sizes the shell asks for.
    images = [png if size == SIZE else encode_png(size, downsample(rows, SIZE, size)) for size in ICO_SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    directory = b""
    for size, image in zip(ICO_SIZES, images):
        directory += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    with open(ICO_PATH, "wb") as file:
        file.write(header + directory + b"".join(images))
    print(f"Wrote {os.path.relpath(PNG_PATH, ROOT)} and {os.path.relpath(ICO_PATH, ROOT)}")


if __name__ == "__main__":
    main()

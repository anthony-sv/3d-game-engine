"""Moto's screen: the HUD over the race and the pause screen (Docs/Architecture.md section 15, "The race"), written by
Author.py's hud part. It makes the Screen entity, a UI canvas that runs Scripts/Hud.sgraph, its elements, and the track
map's image (Textures/Hud/TrackMap.png, made here from the layout in Circuit.py).

What the HUD shows, all of it read by its graph while the game plays:

- riding, while the bike is the player's (the race director Idle, counting down or racing): the speed (km/h), the gear
  (N for neutral), the engine speed with a shift light that lights from the bike's definition's ShiftUpRPM, the lean
  (degrees, to the left or the right), the inputs the vehicle applied (the throttle, the front and the rear brake as
  bars, the lean as a mark either side of the middle) and the track map with the bike's mark on it;
- timing, while a race runs and at its results: the lap of the laps, the lap's time, the best lap, the lap's sectors,
  the countdown, and the delta: the ghost's live Delta while it rides a best lap (its BestLapTime is above 0), otherwise
  the split delta at each gate, the lap's time there against the best lap's when the lap began (its splits, and its time
  at the line);
- the pause screen while the race director's Paused reads true.

Every element is saved hidden: the graph shows each group as the director's state and pause ask, so the scene as saved
draws no UI (the editor's view at the start line and MotoSceneTests keep their draws). A text, a bar or the map's mark
changes only when the value it shows does (the speed by a km/h, the engine speed by 100 rpm, the lean by a degree, a bar
by a hundredth, the mark by a canvas unit, a time by a millisecond). The screen draws in the order of its hierarchy and a
batch of the UI ends wherever the texture changes, so the elements stand in four groups after the map: every rectangle
(the plates, the bars, the shift light, the mark), then every text, then the pause screen over them: the HUD draws in
three batches (the map, the rectangles, the texts), and the pause screen adds two.

    python Projects/Moto/Tools/Hud.py      (prints the figures check() asserts)
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Circuit  # noqa: E402  (the layout)
import GenerateAssets  # noqa: E402  (writing textures)
import Graphs  # noqa: E402  (writing graphs)
import Race  # noqa: E402  (the race director and the ghost's interface)
from Graphs import EFFECTS, ENTITY, EVENTS, RULES, private, public  # noqa: E402

ROOT = "Screen"
HUD = Graphs.script("Hud")
MAP_TEXTURE = "Hud/TrackMap"
REFERENCE = (1920.0, 1080.0)

# --- The track map ---------------------------------------------------------------------------------------------------------

# The image (pixels, square) and how far the circuit stands from its edges; the size the map is shown at (canvas units).
MAP_IMAGE = 512
MAP_MARGIN = 44
MAP_SIZE = 300.0
MAP_AT = (1580.0, 40.0)
# The track's line and the ticks across it (pixels of the image): the start and finish line and the two sector gates.
MAP_LINE = 4.5
MAP_TICK = 15.0
MAP_TICK_WIDTH = 3.0
MAP_CORNER = 40.0
MAP_PLATE = (0.02, 0.03, 0.06, 0.62)
MAP_TRACK = (0.92, 0.94, 0.98, 0.95)
MAP_START = (0.95, 0.2, 0.15, 1.0)
MAP_SECTOR = (1.0, 0.8, 0.2, 1.0)
# The bike's mark (canvas units).
MARK_SIZE = 18.0


def map_frame():
    """The map's center (world X and Z) and its scale (image pixels a meter): the circuit's center line fits the image within
    its margins."""
    points = [Circuit.offset_point(s, 0.0) for s in frange(0.0, Circuit.LENGTH, 2.0)]
    xs, zs = [p[0] for p in points], [p[2] for p in points]
    center = ((min(xs) + max(xs)) / 2.0, (min(zs) + max(zs)) / 2.0)
    extent = max(max(xs) - min(xs), max(zs) - min(zs))
    return center, (MAP_IMAGE - 2.0 * MAP_MARGIN) / extent


def frange(start, stop, step):
    count = int(math.ceil((stop - start) / step))
    return [start + step * k for k in range(count)]


def map_pixel(point):
    """Where a world point lies on the map's image (pixels from its top-left): +X to the right, +Z down, so the map is the
    circuit seen from above."""
    (cx, cz), scale = map_frame()
    return ((point[0] - cx) * scale + MAP_IMAGE / 2.0, (point[2] - cz) * scale + MAP_IMAGE / 2.0)


def mark_transform():
    """The map's center (world X, Z) and canvas units a meter, as the graph places the mark: rounded as it holds them."""
    (cx, cz), scale = map_frame()
    return round(cx, 3), round(cz, 3), round(scale * MAP_SIZE / MAP_IMAGE, 6)


def generate(assets, log=print):
    """Writes the map's image: a dark plate with rounded corners, the circuit's center line drawn round it, and the ticks of
    the start and finish line and of the sector gates across it."""
    size = MAP_IMAGE
    coverage = [[0.0] * size for _ in range(size)]
    ticks = [[0.0] * size for _ in range(size)]
    kinds = [[None] * size for _ in range(size)]

    def stamp(layer, x, y, radius, kind=None):
        for py in range(max(0, int(y - radius - 1)), min(size, int(y + radius + 2))):
            for px in range(max(0, int(x - radius - 1)), min(size, int(x + radius + 2))):
                d = math.hypot(px + 0.5 - x, py + 0.5 - y)
                value = min(1.0, max(0.0, radius + 0.5 - d))
                if value > layer[py][px]:
                    layer[py][px] = value
                    if kind is not None:
                        kinds[py][px] = kind

    (_, _), scale = map_frame()
    step = 0.4 / scale
    for s in frange(0.0, Circuit.LENGTH, step):
        x, y = map_pixel(Circuit.offset_point(s, 0.0))
        stamp(coverage, x, y, MAP_LINE)
    for gate, (_, s) in enumerate(Race.GATES):
        origin, _, right = Circuit.frame(s)
        for k in range(-int(MAP_TICK * 4), int(MAP_TICK * 4) + 1):
            offset = k / 4.0 / scale
            x, y = map_pixel([origin[i] + right[i] * offset for i in range(3)])
            stamp(ticks, x, y, MAP_TICK_WIDTH / 2.0, MAP_START if gate == 0 else MAP_SECTOR)

    def plate(px, py):
        # A rounded square: the distance outside the corners' circles fades over a pixel.
        dx = max(MAP_CORNER - (px + 0.5), (px + 0.5) - (size - MAP_CORNER), 0.0)
        dy = max(MAP_CORNER - (py + 0.5), (py + 0.5) - (size - MAP_CORNER), 0.0)
        return min(1.0, max(0.0, MAP_CORNER + 0.5 - math.hypot(dx, dy)))

    def over(top, alpha, bottom):
        a = alpha + bottom[3] * (1.0 - alpha)
        if a <= 0.0:
            return (0.0, 0.0, 0.0, 0.0)
        return tuple((top[i] * alpha + bottom[i] * bottom[3] * (1.0 - alpha)) / a for i in range(3)) + (a,)

    def pixel(px, py):
        color = (MAP_PLATE[0], MAP_PLATE[1], MAP_PLATE[2], MAP_PLATE[3] * plate(px, py))
        color = over(MAP_TRACK, MAP_TRACK[3] * coverage[py][px], color)
        if kinds[py][px] is not None:
            color = over(kinds[py][px], ticks[py][px], color)
        return tuple(GenerateAssets.quantize(value, 255) for value in color)

    GenerateAssets.write_png(assets, MAP_TEXTURE, size, size, pixel, log, alpha=True)


# --- The screen's elements ----------------------------------------------------------------------------------------------------

PLATE = [0.02, 0.03, 0.06, 0.62]
INSET = [0.09, 0.11, 0.17, 0.9]
CAPTION = [0.62, 0.7, 0.84, 1.0]
VALUE = [1.0, 1.0, 1.0, 1.0]
THROTTLE = [0.2, 0.85, 0.35, 1.0]
FRONT = [0.95, 0.25, 0.2, 1.0]
REAR = [1.0, 0.6, 0.15, 1.0]
STEER = [0.4, 0.75, 1.0, 1.0]
MARK = [1.0, 0.55, 0.1, 1.0]
LIGHT_OFF = [0.16, 0.17, 0.2, 0.9]
LIGHT_ON = [1.0, 0.18, 0.08, 1.0]
AHEAD = [0.35, 1.0, 0.45, 1.0]
BEHIND = [1.0, 0.42, 0.36, 1.0]
DIM = [0.0, 0.0, 0.02, 0.62]

# The input bars: each fills its track from the left, BAR long at full input; the lean's mark moves either side of its
# track's middle, STEER_REACH at a full lean.
BAR = 116.0
BAR_HEIGHT = 12.0
BARS_Y = 990.0
BAR_XS = {"Throttle": 1364.0, "Front": 1494.0, "Rear": 1624.0}
STEER_TRACK = (1754.0, 110.0)
STEER_MARK = (8.0, 20.0)
STEER_REACH = 51.0
STEER_Y = BARS_Y - (STEER_MARK[1] - BAR_HEIGHT) / 2.0
# The lean mark's place at no lean, from the canvas's bottom-right corner (canvas units).
STEER_MIDDLE = STEER_TRACK[0] + STEER_TRACK[1] / 2.0 - REFERENCE[0]
# Texts shown when there is nothing to show yet.
NO_TIME = "-:--.---"


def anchored(anchor, x, y, width, height, visible=True, pivot=(0.0, 0.0)):
    """A rectangle at a point of the reference canvas (its pivot there: its top-left by default), anchored to a point of
    its parent, which is the whole canvas for every group, so that it keeps to its corner on any screen."""
    return {"AnchorMin": list(anchor), "AnchorMax": list(anchor), "Pivot": list(pivot),
            "Position": [round(x - anchor[0] * REFERENCE[0], 3), round(y - anchor[1] * REFERENCE[1], 3)],
            "Size": [width, height], "Visible": visible, "Opacity": 1.0}


def fill(visible=True):
    return {"AnchorMin": [0.0, 0.0], "AnchorMax": [1.0, 1.0], "Pivot": [0.5, 0.5], "Position": [0.0, 0.0], "Size": [0.0, 0.0],
            "Visible": visible, "Opacity": 1.0}


def image(color, radius=0.0, texture=None):
    fields = {"Color": list(color), "CornerRadius": radius, "PreserveAspect": False}
    if texture is not None:
        fields["Texture"] = f"asset://Textures/{texture}.png"
    return fields


def label(value, size, color, alignment="Left", vertical="Middle"):
    return {"Text": value, "FontSize": size, "Color": list(color), "Alignment": alignment, "VerticalAlignment": vertical,
            "Wrap": False, "LineSpacing": 1.0}


TOP_LEFT, TOP, TOP_RIGHT, CENTER, BOTTOM_RIGHT = (0.0, 0.0), (0.5, 0.0), (1.0, 0.0), (0.5, 0.5), (1.0, 1.0)


def bar(name, x, color):
    """An input's track and its fill: (track, fill) rows; the fill is empty until the graph sizes it."""
    return [(f"{name} Track", "Riding Plates", {"UIRect": anchored(BOTTOM_RIGHT, x, BARS_Y, BAR, BAR_HEIGHT),
                                                 "UIImage": image(INSET, BAR_HEIGHT / 2.0)}, None),
            (f"{name} Bar", "Riding Plates", {"UIRect": anchored(BOTTOM_RIGHT, x, BARS_Y, 0.0, BAR_HEIGHT),
                                               "UIImage": image(color, BAR_HEIGHT / 2.0)}, f"{name}Bar")]


def caption(name, x, y, width, height, text, size, alignment="Left", anchor=BOTTOM_RIGHT):
    return (name, "Riding Labels", {"UIRect": anchored(anchor, x, y, width, height), "UIText": label(text, size, CAPTION, alignment)}, None)


def elements():
    """The screen's elements in the order they draw: (name, parent (None for the screen), components, the graph's variable
    that names it or None)."""
    steer_center = STEER_TRACK[0] + STEER_TRACK[1] / 2.0
    rows = [
        # The track map and the bike's mark on it, the map's own texture first.
        ("Track Map", None, {"UIRect": anchored(TOP_RIGHT, MAP_AT[0], MAP_AT[1], MAP_SIZE, MAP_SIZE, visible=False),
                             "UIImage": image(VALUE, texture=MAP_TEXTURE)}, "TrackMap"),
        ("Map Mark", "Track Map", {"UIRect": {"AnchorMin": [0.0, 0.0], "AnchorMax": [0.0, 0.0], "Pivot": [0.5, 0.5],
                                              "Position": [MAP_SIZE / 2.0, MAP_SIZE / 2.0], "Size": [MARK_SIZE, MARK_SIZE],
                                              "Visible": True, "Opacity": 1.0},
                                   "UIImage": image(MARK, MARK_SIZE / 2.0)}, "MapMark"),
        # Every rectangle: the timing's plates, then the riding's plates, bars and light.
        ("Timing Plates", None, {"UIRect": fill(visible=False)}, "TimingPlates"),
        ("Timing Plate", "Timing Plates", {"UIRect": anchored(TOP_LEFT, 40.0, 40.0, 480.0, 250.0), "UIImage": image(PLATE, 18.0)}, None),
        ("Delta Plate", "Timing Plates", {"UIRect": anchored(TOP, 810.0, 40.0, 300.0, 96.0, visible=False),
                                          "UIImage": image(PLATE, 18.0)}, "DeltaPlate"),
        ("Riding Plates", None, {"UIRect": fill(visible=False)}, "RidingPlates"),
        ("Riding Plate", "Riding Plates", {"UIRect": anchored(BOTTOM_RIGHT, 1340.0, 780.0, 540.0, 260.0), "UIImage": image(PLATE, 18.0)},
         None),
        ("Gear Plate", "Riding Plates", {"UIRect": anchored(BOTTOM_RIGHT, 1364.0, 804.0, 120.0, 120.0), "UIImage": image(INSET, 14.0)},
         None),
        ("Shift Light", "Riding Plates", {"UIRect": anchored(BOTTOM_RIGHT, 1364.0, 936.0, 120.0, 18.0), "UIImage": image(LIGHT_OFF, 9.0)},
         "ShiftLight"),
    ]
    rows += bar("Throttle", BAR_XS["Throttle"], THROTTLE) + bar("Front", BAR_XS["Front"], FRONT) + bar("Rear", BAR_XS["Rear"], REAR)
    rows += [
        ("Lean Track", "Riding Plates", {"UIRect": anchored(BOTTOM_RIGHT, STEER_TRACK[0], BARS_Y, STEER_TRACK[1], BAR_HEIGHT),
                                         "UIImage": image(INSET, BAR_HEIGHT / 2.0)}, None),
        ("Lean Mark", "Riding Plates", {"UIRect": anchored(BOTTOM_RIGHT, steer_center, STEER_Y, STEER_MARK[0], STEER_MARK[1],
                                                            pivot=(0.5, 0.0)),
                                        "UIImage": image(STEER, STEER_MARK[0] / 2.0)}, "LeanMark"),
        # Every text: the timing's, then the riding's.
        ("Timing Labels", None, {"UIRect": fill(visible=False)}, "TimingLabels"),
        ("Lap Text", "Timing Labels", {"UIRect": anchored(TOP_LEFT, 68.0, 58.0, 420.0, 40.0), "UIText": label("LAP -/3", 32.0, CAPTION)},
         "LapText"),
        ("Lap Time Text", "Timing Labels", {"UIRect": anchored(TOP_LEFT, 68.0, 96.0, 420.0, 80.0),
                                            "UIText": label("0:00.000", 72.0, VALUE)},
         "LapTimeText"),
        ("Best Text", "Timing Labels", {"UIRect": anchored(TOP_LEFT, 68.0, 180.0, 420.0, 36.0),
                                        "UIText": label(f"BEST {NO_TIME}", 28.0, CAPTION)}, "BestText"),
        ("Sectors Text", "Timing Labels", {"UIRect": anchored(TOP_LEFT, 68.0, 222.0, 440.0, 36.0), "UIText": label("", 26.0, VALUE)},
         "SectorsText"),
        ("Delta Text", "Timing Labels", {"UIRect": anchored(TOP, 810.0, 40.0, 300.0, 96.0), "UIText": label("", 64.0, VALUE, "Center")},
         "DeltaText"),
        ("Countdown Text", "Timing Labels", {"UIRect": anchored(CENTER, 760.0, 330.0, 400.0, 300.0, visible=False),
                                             "UIText": label("3", 260.0, VALUE, "Center")}, "CountdownText"),
        ("Riding Labels", None, {"UIRect": fill(visible=False)}, "RidingLabels"),
        ("Gear Text", "Riding Labels", {"UIRect": anchored(BOTTOM_RIGHT, 1364.0, 804.0, 120.0, 120.0),
                                        "UIText": label("N", 100.0, VALUE, "Center")}, "GearText"),
        ("Speed Text", "Riding Labels", {"UIRect": anchored(BOTTOM_RIGHT, 1500.0, 798.0, 250.0, 100.0),
                                         "UIText": label("0", 96.0, VALUE, "Right")}, "SpeedText"),
        caption("Speed Unit", 1758.0, 834.0, 110.0, 40.0, "KM/H", 28.0),
        ("Rpm Text", "Riding Labels", {"UIRect": anchored(BOTTOM_RIGHT, 1500.0, 900.0, 250.0, 44.0),
                                       "UIText": label("0", 40.0, VALUE, "Right")}, "RpmText"),
        caption("Rpm Unit", 1758.0, 904.0, 110.0, 36.0, "RPM", 26.0),
        ("Lean Text", "Riding Labels", {"UIRect": anchored(BOTTOM_RIGHT, 1500.0, 946.0, 250.0, 40.0),
                                        "UIText": label("0°", 34.0, VALUE, "Right")}, "LeanText"),
        caption("Lean Unit", 1758.0, 948.0, 110.0, 34.0, "LEAN", 26.0),
        caption("Throttle Label", BAR_XS["Throttle"], 1008.0, BAR, 24.0, "THROTTLE", 18.0),
        caption("Front Label", BAR_XS["Front"], 1008.0, BAR, 24.0, "FRONT", 18.0),
        caption("Rear Label", BAR_XS["Rear"], 1008.0, BAR, 24.0, "REAR", 18.0),
        caption("Lean Label", STEER_TRACK[0], 1008.0, STEER_TRACK[1], 24.0, "LEAN", 18.0, "Center"),
        # The pause screen, over everything.
        ("Pause Screen", None, {"UIRect": fill(visible=False), "UIImage": image(DIM)}, "PauseScreen"),
        ("Pause Title", "Pause Screen", {"UIRect": anchored(CENTER, 460.0, 400.0, 1000.0, 150.0),
                                         "UIText": label("PAUSED", 132.0, VALUE, "Center")},
         None),
        ("Pause Hint", "Pause Screen", {"UIRect": anchored(CENTER, 460.0, 560.0, 1000.0, 50.0),
                                        "UIText": label("ESC OR START TO RESUME", 34.0, CAPTION, "Center")}, None),
    ]
    return rows


# The graph's variables that name the elements, with their tooltips.
TOOLTIPS = {
    "TrackMap": "The track map, the riding group's first element",
    "MapMark": "The bike's mark on the track map",
    "TimingPlates": "The timing group's rectangles",
    "DeltaPlate": "The plate under the delta, shown once there is one",
    "RidingPlates": "The riding group's rectangles",
    "ShiftLight": "The light that turns on at the definition's shift point",
    "ThrottleBar": "The throttle's bar",
    "FrontBar": "The front brake's bar",
    "RearBar": "The rear brake's bar",
    "LeanMark": "The lean input's mark",
    "TimingLabels": "The timing group's texts",
    "LapText": "The lap of the laps",
    "LapTimeText": "The lap's time",
    "BestText": "The best lap's time",
    "SectorsText": "The lap's sectors so far",
    "DeltaText": "The delta against the best lap",
    "CountdownText": "The countdown's seconds left",
    "RidingLabels": "The riding group's texts",
    "GearText": "The gear",
    "SpeedText": "The speed in km/h",
    "RpmText": "The engine speed in rpm",
    "LeanText": "The bike's lean",
    "PauseScreen": "The pause screen",
}
# The groups each state shows: riding while the bike is the player's, timing while a race runs and at its results.
RIDING_STATES = ("Idle", "Countdown", "Racing")
TIMING_STATES = ("Countdown", "Racing", "Results")
RIDING_GROUP = ("TrackMap", "RidingPlates", "RidingLabels")
TIMING_GROUP = ("TimingPlates", "TimingLabels")


def check():
    """Asserts what the screen promises; returns a summary."""
    rows = elements()
    names = [name for name, *_ in rows]
    variables = [variable for *_, variable in rows if variable is not None]
    assert len(set(names)) == len(names), "the screen's elements share names"
    assert sorted(variables) == sorted(TOOLTIPS), "every element the graph changes has a variable with a tooltip"
    # The batches: the map's texture, then rectangles, then texts, then the pause screen's rectangle and texts.
    def batches_of(drawn):
        kinds = []
        for _, _, components, _ in drawn:
            if "UIImage" in components:
                kinds.append("map" if "Texture" in components["UIImage"] else "rectangle")
            elif "UIText" in components:
                kinds.append("text")
        return 1 + sum(1 for previous, kind in zip(kinds, kinds[1:]) if previous != kind)

    batches = batches_of(rows)
    hud = batches_of(rows[:names.index("Pause Screen")])
    assert hud == 3 and batches == 5, f"the HUD draws in {hud} batches and {batches} with the pause screen"
    # The mark stays on the map wherever the bike is on the circuit.
    cx, cz, scale = mark_transform()
    for s in frange(0.0, Circuit.LENGTH, 5.0):
        x, _, z = Circuit.offset_point(s, 0.0)
        u, v = (x - cx) * scale + MAP_SIZE / 2.0, (z - cz) * scale + MAP_SIZE / 2.0
        assert 0.0 < u < MAP_SIZE and 0.0 < v < MAP_SIZE, f"the mark leaves the map at {s:.0f} m"
    return f"{len(rows)} elements, the HUD in {hud} batches ({batches} with the pause screen); the map at {scale:.4f} canvas units a meter"


# --- The graph -------------------------------------------------------------------------------------------------------------

ROUNDING = {"speed": "int(round(abs(v) * 3.6))", "rpm": "int(round(rpm / 100)) * 100", "lean": "int(round(lean))",
            "input": "int(round(clamp(value, 0, 1) * 100))", "steer": "int(round(clamp(value, -1, 1) * 100))",
            "millis": "int(round(seconds * 1000))", "clock": "int(round(max(seconds, 0) * 1000))"}


def declare(author):
    elements_variables = tuple(public(variable, ENTITY, None, TOOLTIPS[variable]) for *_, variable in elements() if variable is not None)
    author.declare(HUD, "The HUD over the race and the pause screen: it reads the bike's vehicle and the race director every "
                   "frame and changes a text, a bar or the map's mark only when the value it shows does; each group shows "
                   "as the director's state and pause ask.", variables=(
        public("Race", ENTITY, None, "The race director, whose state, laps, times and pause the HUD shows"),
        public("Bike", ENTITY, None, "The ridden bike, whose vehicle the HUD reads"),
    ) + elements_variables + (
        private("ShownState", "string", "-"),
        private("ShownPaused", "bool", False),
        private("Riding", "bool", False),
        private("Timing", "bool", False),
        private("ShiftAt", "float", 0.0),
        private("ShownSpeed", "int", -1),
        private("ShownGear", "int", -1),
        private("ShownRpm", "int", -1),
        private("ShownLean", "int", -1000),
        private("ShownLit", "bool", False),
        private("ShownThrottle", "int", 0),
        private("ShownFront", "int", 0),
        private("ShownRear", "int", 0),
        private("ShownSteer", "int", 0),
        private("ShownMark", "Strada.Vector2", [MAP_SIZE / 2.0, MAP_SIZE / 2.0]),
        private("ShownLap", "int", -1),
        private("ShownLaps", "int", -1),
        private("ShownClock", "int", -1),
        private("ShownBest", "int", -1),
        private("ShownCountdown", "int", -1),
        private("ShownDelta", "int", 0),
        private("Live", "bool", False),
        private("LapBest", "float", 0.0),
        private("LapBestSplits", "float[]", []),
        private("SectorsShown", "string", ""),
    ), events=tuple((name, Race.BROADCASTS[name]) for name in ("RaceStarted", "LapStarted", "SectorCrossed", "LapFinished")),
        functions=(
        ("Clock", True, (("Seconds", "float"),), (("Text", "string"),), ()),
        ("Signed", True, (("Seconds", "float"),), (("Text", "string"),), ()),
        ("ShowDelta", False, (("Seconds", "float"),), (), ()),
        ("ShowPanels", False, (), (), (("State", "string", ""), ("Paused", "bool", False))),
        ("ShowReadings", False, (), (), ()),
        ("ShowInputs", False, (), (), ()),
        ("ShowMark", False, (), (), ()),
        ("ShowTiming", False, (), (), ()),
    ))


def director(b, name):
    return b.once(("director", name), lambda: b.get(name, Race.DIRECTOR, b.var("Race")))


def member(b, name):
    return b.once(("member", name), lambda: b.pure(f"Get:Strada.VehicleComponent.{name}", "Value", Target=b.var("Bike")))


def show(b, variable, visible):
    return b.call("Set:Strada.UIRectComponent.Visible", Target=b.var(variable), Value=visible)


def text(b, variable, value):
    return b.call("Set:Strada.UITextComponent.Text", Target=b.var(variable), Value=value)


def select(b, condition, when_true, when_false):
    return b.pure("Select", Condition=condition, **{"True": when_true, "False": when_false})


def string(b, value):
    return b.lit("string", value)


def color(b, value):
    return b.lit("Strada.Color", value)


def changed(b, value, shown):
    """A branch on whether a value differs from the one shown."""
    return b.branch(b.pure("Operator.NotEqual", A=value, B=b.var(shown)))


def in_turn(b, steps, entry, end):
    """Runs steps one after the other from entry to end: a step is (a branch on whether a value changed, the first and the
    last node of what shows it when it did)."""
    targets = [test for test, _, _ in steps] + [end]
    b.flow(entry, targets[0])
    for (test, first, last), following in zip(steps, targets[1:]):
        b.flow(test["True"], first)
        b.flow(last, following)
        b.flow(test["False"], following)


def step(b, test, *nodes):
    b.chain(*nodes)
    return test, nodes[0], nodes[-1]


def clock(b, seconds):
    return b.own("Clock", "Text", Seconds=seconds).out


def write_formats(author):
    with author.region(HUD, "Clock: a time as minutes, seconds and milliseconds", RULES, "Clock") as b:
        total = b.expr(ROUNDING["clock"], seconds=b.entry["Seconds"])
        minutes = b.expr("total / 60000", total=total)
        seconds = b.pure("String.Substring", Text=b.fmt("{Value}", Value=b.expr("total / 1000 % 60 + 100", total=total)), Start=1,
                         Length=2)
        millis = b.pure("String.Substring", Text=b.fmt("{Value}", Value=b.expr("total % 1000 + 1000", total=total)), Start=1,
                        Length=3)
        b.link(b.fmt("{Minutes}:{Seconds}.{Millis}", Minutes=minutes, Seconds=seconds, Millis=millis), b.ret["Text"])
    with author.region(HUD, "Signed: a delta in seconds and milliseconds, with its sign", RULES, "Signed") as b:
        total = b.expr("int(round(abs(seconds) * 1000))", seconds=b.entry["Seconds"])
        millis = b.pure("String.Substring", Text=b.fmt("{Value}", Value=b.expr("total % 1000 + 1000", total=total)), Start=1,
                        Length=3)
        sign = select(b, b.expr("seconds < 0 && total > 0", seconds=b.entry["Seconds"], total=total), string(b, "-"),
                      string(b, "+"))
        b.link(b.fmt("{Sign}{Whole}.{Millis}", Sign=sign, Whole=b.expr("total / 1000", total=total), Millis=millis), b.ret["Text"])
    with author.region(HUD, "Show delta: its text, ahead in green and behind in red", EFFECTS, "ShowDelta") as b:
        kept = b.set("ShownDelta", b.expr(ROUNDING["millis"], seconds=b.entry["Seconds"]))
        written = text(b, "DeltaText", b.own("Signed", "Text", Seconds=b.entry["Seconds"]).out)
        tint = select(b, b.expr("millis < 0", millis=b.var("ShownDelta")), color(b, AHEAD),
                      select(b, b.expr("millis > 0", millis=b.var("ShownDelta")), color(b, BEHIND), color(b, VALUE)))
        tinted = b.call("Set:Strada.UITextComponent.Color", Target=b.var("DeltaText"), Value=tint)
        b.chain(b.entry, kept, written, tinted, show(b, "DeltaPlate", True), b.ret)


def write_panels(author):
    with author.region(HUD, "Show panels: each group as the director's state and pause ask", RULES, "ShowPanels") as b:
        state = b.set("State", director(b, "State"))
        paused = b.set("Paused", director(b, "Paused"))
        moved = b.branch(b.pure("Operator.Or", A=b.pure("Operator.NotEqual", A=b.var("State"), B=b.var("ShownState")),
                                B=b.pure("Operator.NotEqual", A=b.var("Paused"), B=b.var("ShownPaused"))),
                         comment="Only when the state or the pause changed")

        def any_of(states):
            result = b.equal(b.var("State"), string(b, states[0]))
            for name in states[1:]:
                result = b.pure("Operator.Or", A=result, B=b.equal(b.var("State"), string(b, name)))
            return result

        kept = [b.set("ShownState", b.var("State")), b.set("ShownPaused", b.var("Paused")),
                b.set("Riding", any_of(RIDING_STATES)), b.set("Timing", any_of(TIMING_STATES))]
        groups = [show(b, variable, b.var("Riding")) for variable in RIDING_GROUP] + \
            [show(b, variable, b.var("Timing")) for variable in TIMING_GROUP]
        counting = show(b, "CountdownText", b.equal(b.var("State"), string(b, "Countdown")))
        pause = show(b, "PauseScreen", b.var("Paused"))
        b.chain(b.entry, state, paused, moved)
        b.chain(moved["True"], *kept, *groups, counting, pause, b.ret)
        b.flow(moved["False"], b.ret)


def write_riding(author):
    with author.region(HUD, "Show readings: the speed, the gear, the engine and the lean", RULES, "ShowReadings") as b:
        speed = b.expr(ROUNDING["speed"], v=member(b, "ForwardSpeed"))
        gear = member(b, "Gear")
        rpm = b.expr(ROUNDING["rpm"], rpm=member(b, "EngineSpeed"))
        lit = b.expr("at > 0 && rpm >= at", at=b.var("ShiftAt"), rpm=member(b, "EngineSpeed"))
        lean = b.expr(ROUNDING["lean"], lean=member(b, "LeanAngle"))
        neutral = b.expr("gear == 0", gear=b.var("ShownGear"))
        side = select(b, b.expr("lean < 0", lean=b.var("ShownLean")), string(b, "L"), string(b, "R"))
        degrees = b.fmt("{Degrees}° {Side}", Degrees=b.expr("abs(lean)", lean=b.var("ShownLean")), Side=side)
        upright = b.expr("lean == 0", lean=b.var("ShownLean"))
        steps = [
            step(b, changed(b, speed, "ShownSpeed"), b.set("ShownSpeed", speed),
                 text(b, "SpeedText", b.fmt("{Kmh}", Kmh=b.var("ShownSpeed")))),
            step(b, changed(b, gear, "ShownGear"), b.set("ShownGear", gear),
                 text(b, "GearText", select(b, neutral, string(b, "N"), b.fmt("{Gear}", Gear=b.var("ShownGear"))))),
            step(b, changed(b, rpm, "ShownRpm"), b.set("ShownRpm", rpm), text(b, "RpmText", b.fmt("{Rpm}", Rpm=b.var("ShownRpm")))),
            step(b, changed(b, lit, "ShownLit"), b.set("ShownLit", lit),
                 b.call("Set:Strada.UIImageComponent.Color", Target=b.var("ShiftLight"),
                        Value=select(b, b.var("ShownLit"), color(b, LIGHT_ON), color(b, LIGHT_OFF)))),
            step(b, changed(b, lean, "ShownLean"), b.set("ShownLean", lean),
                 text(b, "LeanText", select(b, upright, string(b, "0°"), degrees))),
        ]
        in_turn(b, steps, b.entry, b.ret)
    with author.region(HUD, "Show inputs: the throttle, the brakes and the lean the vehicle applied", RULES, "ShowInputs") as b:
        steps = []
        for name, shown, variable in (("Throttle", "ShownThrottle", "ThrottleBar"), ("FrontBrake", "ShownFront", "FrontBar"),
                                      ("RearBrake", "ShownRear", "RearBar")):
            value = b.expr(ROUNDING["input"], value=member(b, name))
            size = b.call("Set:Strada.UIRectComponent.Size", Target=b.var(variable),
                          Value=b.expr(f"vec2(float(shown) * {BAR / 100.0}, {BAR_HEIGHT})", shown=b.var(shown)))
            steps.append(step(b, changed(b, value, shown), b.set(shown, value), size))
        steer = b.expr(ROUNDING["steer"], value=member(b, "Lean"))
        placed = b.call("Set:Strada.UIRectComponent.Position", Target=b.var("LeanMark"),
                        Value=b.expr(f"vec2({STEER_MIDDLE} + float(shown) * {STEER_REACH / 100.0}, {STEER_Y - REFERENCE[1]})",
                                     shown=b.var("ShownSteer")))
        steps.append(step(b, changed(b, steer, "ShownSteer"), b.set("ShownSteer", steer), placed))
        in_turn(b, steps, b.entry, b.ret)
    with author.region(HUD, "Show mark: the bike's place on the track map", RULES, "ShowMark") as b:
        cx, cz, scale = mark_transform()
        half = MAP_SIZE / 2.0
        at = b.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=b.var("Bike"))
        mark = b.expr(f"vec2(round((at.X - {cx}) * {scale} + {half}), round((at.Z - {cz}) * {scale} + {half}))", at=at)
        placed = b.call("Set:Strada.UIRectComponent.Position", Target=b.var("MapMark"), Value=b.var("ShownMark"))
        in_turn(b, [step(b, changed(b, mark, "ShownMark"), b.set("ShownMark", mark), placed)], b.entry, b.ret)


def write_timing(author):
    with author.region(HUD, "Show timing: the lap, its time, the best, the countdown, the live delta", RULES, "ShowTiming") as b:
        lap, laps = director(b, "Lap"), director(b, "Laps")
        lap_moved = b.branch(b.pure("Operator.Or", A=b.pure("Operator.NotEqual", A=lap, B=b.var("ShownLap")),
                                    B=b.pure("Operator.NotEqual", A=laps, B=b.var("ShownLaps"))))
        counted = select(b, b.expr("lap >= 1", lap=b.var("ShownLap")),
                         b.fmt("LAP {Lap}/{Laps}", Lap=b.var("ShownLap"), Laps=b.var("ShownLaps")),
                         b.fmt("LAP -/{Laps}", Laps=b.var("ShownLaps")))
        lap_time, best = director(b, "LapTime"), director(b, "BestLapTime")
        clock_millis = b.expr(ROUNDING["millis"], seconds=lap_time)
        best_millis = b.expr(ROUNDING["millis"], seconds=best)
        best_text = select(b, b.expr("best > 0", best=b.var("ShownBest")), b.fmt("BEST {Time}", Time=clock(b, best)),
                           string(b, f"BEST {NO_TIME}"))
        left = b.expr("int(ceil(left))", left=director(b, "CountdownLeft"))
        steps = [
            step(b, lap_moved, b.set("ShownLap", lap), b.set("ShownLaps", laps), text(b, "LapText", counted)),
            step(b, changed(b, clock_millis, "ShownClock"), b.set("ShownClock", clock_millis),
                 text(b, "LapTimeText", clock(b, lap_time))),
            step(b, changed(b, best_millis, "ShownBest"), b.set("ShownBest", best_millis), text(b, "BestText", best_text)),
            step(b, changed(b, left, "ShownCountdown"), b.set("ShownCountdown", left),
                 text(b, "CountdownText", b.fmt("{Seconds}", Seconds=b.var("ShownCountdown")))),
        ]
        # The live delta: while a lap is ridden against a ghost that rides a best lap, the ghost's Delta.
        racing = b.branch(b.expr("racing && lap >= 1", racing=b.equal(director(b, "State"), string(b, "Racing")), lap=lap),
                          comment="A lap is ridden")
        ghost = director(b, "Ghost")
        rides = b.branch(b.runs(Race.GHOST_GRAPH, ghost), comment="Against a ghost")
        delta = b.get("Delta", Race.GHOST_GRAPH, ghost)
        has_best = b.branch(b.expr("best > 0", best=b.get("BestLapTime", Race.GHOST_GRAPH, ghost)), comment="That rides a best lap")
        moved = b.branch(b.expr("!live || millis != shown", live=b.var("Live"), millis=b.expr(ROUNDING["millis"], seconds=delta),
                                shown=b.var("ShownDelta")))
        live = b.set("Live", True)
        shown = b.own("ShowDelta", Seconds=delta)
        split = b.set("Live", False, comment="The split delta at each gate shows instead")
        in_turn(b, steps, b.entry, racing)
        b.flow(racing["True"], rides)
        b.flow(rides["True"], has_best)
        b.flow(has_best["True"], moved)
        b.chain(moved["True"], live, shown, b.ret)
        b.flow(moved["False"], b.ret)
        for source in (racing["False"], rides["False"], has_best["False"]):
            b.flow(source, split)
        b.flow(split, b.ret)


def write_events(author):
    with author.region(HUD, "Start: the shift light's point, from the bike's definition", EVENTS) as b:
        known = b.branch(b.pure("IsValid", Value=b.var("Bike")))
        definition = b.pure("Get:Strada.VehicleComponent.Definition", "Value", Target=b.var("Bike"))
        defined = b.branch(b.pure("IsValid", Value=definition))
        point = b.set("ShiftAt", b.pure("Get:Strada.Vehicle.ShiftUpRPM", "Value", Target=definition))
        b.chain(b.event("Event.Create"), known)
        b.flow(known["True"], defined)
        b.flow(defined["True"], point)
    with author.region(HUD, "Read the race: each frame, the groups, then what they show", EVENTS) as b:
        known = b.branch(b.runs(Race.DIRECTOR, b.var("Race")))
        panels = b.own("ShowPanels")
        groups = b.sequence(2)
        riding = b.branch(b.both(b.var("Riding"), b.pure("IsValid", Value=b.var("Bike"))))
        timing = b.branch(b.var("Timing"))
        b.chain(b.event("Event.Update"), known)
        b.flow(known["True"], panels)
        b.flow(panels, groups)
        b.flow(groups["Then0"], riding)
        b.chain(riding["True"], b.own("ShowReadings"), b.own("ShowInputs"), b.own("ShowMark"))
        b.flow(groups["Then1"], timing)
        b.flow(timing["True"], b.own("ShowTiming"))
    with author.region(HUD, "Race started: no sectors and no delta yet", EVENTS) as b:
        b.chain(b.custom("RaceStarted"), b.set("SectorsShown", ""), text(b, "SectorsText", ""), b.set("Live", False),
                b.set("ShownDelta", 0), text(b, "DeltaText", ""), show(b, "DeltaPlate", False))
    with author.region(HUD, "Lap started: the best lap as it stands, for the lap's splits", EVENTS) as b:
        b.chain(b.custom("LapStarted"), b.set("LapBest", director(b, "BestLapTime")),
                b.set("LapBestSplits", director(b, "BestSplits")), b.set("SectorsShown", ""), text(b, "SectorsText", ""))
    with author.region(HUD, "Sector crossed: its split shown, and its delta when no ghost gives one", EVENTS) as b:
        crossed = b.custom("SectorCrossed")
        entry = b.fmt("S{Sector} {Time}", Sector=crossed["Sector"], Time=clock(b, crossed["Time"]))
        joined = select(b, b.pure("String.IsEmpty", Text=b.var("SectorsShown")), entry,
                        b.fmt("{Before}   {Entry}", Before=b.var("SectorsShown"), Entry=entry))
        best = b.add("Array.Get", None, "Element", Array=b.var("LapBestSplits"),
                     Index=b.expr("sector - 1", sector=crossed["Sector"])).out
        known = b.branch(b.expr("!live && count >= sector", live=b.var("Live"), sector=crossed["Sector"],
                                count=b.pure("Array.Length", "Length", Array=b.var("LapBestSplits"))),
                         comment="The best lap's split at this gate, and no ghost's delta")
        b.chain(crossed, b.set("SectorsShown", joined), text(b, "SectorsText", b.var("SectorsShown")), known)
        b.flow(known["True"], b.own("ShowDelta", Seconds=b.expr("time - best", time=crossed["Time"], best=best)))
    with author.region(HUD, "Lap finished: its delta at the line when no ghost gives one", EVENTS) as b:
        finished = b.custom("LapFinished")
        known = b.branch(b.expr("!live && best > 0", live=b.var("Live"), best=b.var("LapBest")),
                         comment="The best lap as the lap began, and no ghost's delta")
        b.chain(finished, known)
        b.flow(known["True"], b.own("ShowDelta", Seconds=b.expr("time - best", time=finished["Time"], best=b.var("LapBest"))))


def write(author):
    write_formats(author)
    write_panels(author)
    write_riding(author)
    write_timing(author)
    write_events(author)


if __name__ == "__main__":
    print(check())

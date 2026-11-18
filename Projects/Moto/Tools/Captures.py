"""Moto's captures: the player's own screenshots of the main scene from a fixed list of cameras (CAMERAS), for the look
review and for whoever takes the same views again. Author.py --captures runs it:

    python Projects/Moto/Tools/Author.py --captures <output folder> --player <StradaRuntime executable> [--views name ...]

The player draws the scene's primary camera and has no camera option, so each view is a copy of the project, outside the
repository (the system's temporary folder), whose main scene has the main camera where the view puts it, its tour held
(PlayOnStart off), and the bike where the view puts it: on the grid where it waits, or on the circuit, leaned into a
corner. A view of the tour instead plays the main scene as it is and takes the frame of a time of the tour. The ghost
bike is hidden in every view (the views are the grid bike's, and a tour's would catch it riding). The player runs each
with a per-user root of its own (APPDATA, LOCALAPPDATA, HOME and XDG_DATA_HOME in a temporary folder), draws
1920 by 1080 pixels (--resolution) and saves <output folder>/<view>.png. A run that exits with an error, saves no
picture, or saves one of another size is run again, twice at most, and then fails the captures.
"""
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import Bike  # noqa: E402  (the motorcycle and its rider)
import Circuit  # noqa: E402  (the layout)
import GenerateAssets  # noqa: E402  (the circuit's center)
import Ghost  # noqa: E402  (the ghost bike's name)

PROJECT = os.path.dirname(TOOLS)
WIDTH, HEIGHT = 1920, 1080
# Frames a still view runs before its picture: the shadows and the exposure settle in the first few.
STILL_FRAMES = 30
ATTEMPTS = 3


# --- Where things are ----------------------------------------------------------------------------------------------------

def multiply(a, b):
    """The product of two quaternions (x, y, z, w): b's rotation, then a's."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def about(axis, degrees):
    half = math.radians(degrees) / 2.0
    return tuple(component * math.sin(half) for component in axis) + (math.cos(half),)


def look(eye, target):
    """A camera's rotation at the eye looking at the target: its yaw about +Y, then its pitch about its own X."""
    dx, dy, dz = (target[k] - eye[k] for k in range(3))
    pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
    yaw = math.degrees(math.atan2(-dx, -dz))
    return multiply(about((0.0, 1.0, 0.0), yaw), about((1.0, 0.0, 0.0), pitch))


class Pose:
    """Where the bike stands: at a lap distance and a lateral offset (right of travel), its front along the travel,
    leaned by a lean in degrees (positive to the right)."""

    def __init__(self, s, lateral, lean=0.0):
        self.s, self.lateral, self.lean = s % Circuit.LENGTH, lateral, lean

    def transform(self):
        # A leaned tyre touches the road off its middle: the bike's origin rises by what its section's roundness takes.
        crown = Bike.REAR_WIDTH / 2.0
        height = Circuit.road_height(self.lateral) + crown * (1.0 - math.cos(math.radians(self.lean)))
        translation = Circuit.offset_point(self.s, self.lateral, height)
        # Its top towards +X (its right) is a turn of -lean about its forward axis (Z).
        rotation = multiply(about((0.0, 1.0, 0.0), Circuit.yaw(self.s)), about((0.0, 0.0, 1.0), -self.lean))
        return translation, rotation

    def point(self, right, up, ahead):
        """A world point in the bike's upright frame: to its right, up and ahead of its origin (meters)."""
        _, forward, side = Circuit.frame(self.s)
        origin = Circuit.offset_point(self.s, self.lateral)
        return tuple(origin[k] + side[k] * right + forward[k] * ahead + (up if k == 1 else 0.0) for k in range(3))


def grid_pose():
    return Pose(Circuit.LENGTH - Bike.GRID_BEHIND, Bike.GRID_LATERAL)


def corner(name):
    start, end, side = next((start, end, side) for found, start, end, side in Circuit.corner_ranges() if found.name == name)
    return start, end, side


def trackside(s, side, beyond, height):
    """A point beyond a wall: at a lap distance, on a side (+1 right of travel, -1 left), beyond meters past the wall's
    face, at a height."""
    face = Circuit.barrier_offset(s % Circuit.LENGTH, "right" if side > 0 else "left")
    return Circuit.offset_point(s % Circuit.LENGTH, side * (face + beyond), height)


# The bike and its rider stand this high: a view that names the bike's size on the screen frames it by this.
RIDDEN_HEIGHT = 1.5


def framing(eye, target, size):
    """The vertical field of view (degrees) in which the bike at the target fills a size of the frame's height."""
    return math.degrees(2.0 * math.atan(RIDDEN_HEIGHT / size / (2.0 * math.dist(eye, target))))


def apex_pose(name, lean, fraction=0.5, inside=3.5):
    """The bike leaned into a corner, at a fraction of its length, inside meters in from the center line."""
    start, end, side = corner(name)
    return Pose(start + fraction * (end - start), side * inside, side * lean)


class View:
    """A camera: its eye and target (world points), its vertical field of view, where the bike is (None leaves it on
    the grid), whether the rider shows, and what the view is for. A tour view plays the tour instead and takes its
    frame at the tour's time."""

    def __init__(self, name, purpose, eye=None, target=None, fov=50.0, bike=None, rider=True, tour_time=None):
        self.name, self.purpose, self.eye, self.target, self.fov = name, purpose, eye, target, fov
        self.bike, self.rider, self.tour_time = bike, rider, tour_time


def views():
    """The fixed list of cameras, in the order the look review reads them."""
    grid = grid_pose()
    turn1 = apex_pose("Turn 1", 42.0, 0.55)
    turn6 = apex_pose("Turn 6", 38.0, 0.62)
    gate = Pose(Circuit.SECTOR_GATES[0] - 28.0, 1.5)
    bridge = Pose(Circuit.FOOTBRIDGE - 32.0, -2.0)
    straight = Pose(Circuit.FOOTBRIDGE - 130.0, -1.0)
    center_x, center_z = GenerateAssets.circuit_center()
    _, _, t1_side = corner("Turn 1")
    _, t6_end, t6_side = corner("Turn 6")
    # A camera on the inside of Turn 1 ahead of the bike, which leans towards it; one beyond the gravel on Turn 6's
    # outside, looking back across the trap.
    t1_eye, t1_target = trackside(turn1.s + 18.0, t1_side, 2.0, 2.5), turn1.point(0.0, 0.7, 0.0)
    t6_eye, t6_target = trackside(t6_end - 10.0, -t6_side, 3.0, 4.5), turn6.point(0.0, 0.7, 0.0)
    return (
        View("grid", "close: the bike and its rider on the grid, three-quarters from behind, the gantry ahead (the bike about "
             "a third of the frame's height)", grid.point(3.0, 1.25, -2.2), grid.point(-0.5, 0.75, 1.8), 50.0),
        View("grid-front", "close: the bike and its rider on the grid, three-quarters from the front",
             grid.point(2.4, 1.15, 3.1), grid.point(0.0, 0.62, 0.05), 50.0),
        View("grid-profile", "the bike's side at 11 m through a long lens: its proportions",
             grid.point(11.0, 0.8, 0.0), grid.point(0.0, 0.62, 0.0), 12.0),
        View("trackside-turn1", "mid range: Turn 1 from a camera on its inside, the bike leaned towards it (the bike a seventh "
             "of the frame's height)", t1_eye, t1_target, framing(t1_eye, t1_target, 0.14), turn1),
        View("trackside-turn6", "far: Turn 6 from beyond its gravel trap, the bike leaned at the apex (the bike a twentieth of "
             "the frame's height)", t6_eye, t6_target, framing(t6_eye, t6_target, 0.05), turn6),
        View("trackside-gate1", "the run to Sector Gate 1 from the inside verge: boards, fences and stands, the bike passing",
             gate.point(-13.0, 1.6, -24.0), gate.point(1.0, 1.0, 24.0), 55.0, gate),
        View("footbridge", "the footbridge over the back straight from the road, the bike riding towards it",
             bridge.point(1.5, 1.6, -26.0), bridge.point(-1.0, 3.5, 40.0), 55.0, bridge),
        View("aerial", "the circuit from 160 m up over its south side: the props' scale, the ground's tiling, the horizon",
             (center_x, 160.0, center_z + 700.0), (center_x, 0.0, center_z - 150.0), 50.0),
        View("rider", "the rider's view down the back straight: the screen, the bars and the road ahead (the rider hidden)",
             straight.point(0.0, 1.36, 0.26), straight.point(0.0, 0.9, 30.0), 70.0, straight, rider=False),
        View("tour-fences-a", "a frame of the tour along the catch fences of the main straight (FXAA)", tour_time=8.0),
        View("tour-fences-b", "the frame after tour-fences-a: what changes between them shimmers in motion",
             tour_time=8.0 + 1.0 / 60.0),
    )


# --- Copies and runs -----------------------------------------------------------------------------------------------------

def find(entities, name, parent="0"):
    """The entity of a name under a parent (by its ID; "0" for a root)."""
    return next(entity for entity in entities
                if entity["Components"].get("Tag", {}).get("Tag") == name and entity["Components"]["Relationship"]["Parent"] == parent)


def hide(entities, root):
    """Hides an entity's meshes and its descendants'."""
    by_id = {entity["ID"]: entity for entity in entities}
    pending = [root["ID"]]
    while pending:
        components = by_id[pending.pop()]["Components"]
        if "Mesh" in components:
            components["Mesh"]["Visible"] = False
        pending += components["Relationship"]["Children"]


def stage(view, scene_path, original):
    """Writes the copy's main scene for a still view: the camera, its tour held, the bike and its rider."""
    scene = json.loads(json.dumps(original))
    entities = scene["Entities"]
    camera = find(entities, "Main Camera")["Components"]
    camera["Transform"]["Translation"] = list(view.eye)
    camera["Transform"]["Rotation"] = list(look(view.eye, view.target))
    camera["Camera"]["PerspectiveFOV"] = view.fov
    camera["VisualScript"]["Variables"] = {"PlayOnStart": {"Type": "Bool", "Value": False}}
    bike = find(entities, "Bike")
    if view.bike is not None:
        translation, rotation = view.bike.transform()
        transform = bike["Components"]["Transform"]
        transform["Translation"], transform["Rotation"] = list(translation), list(rotation)
    if not view.rider:
        find(entities, "Rider", bike["ID"])["Components"]["Mesh"]["Visible"] = False
    with open(scene_path, "w", encoding="utf-8") as file:
        json.dump(scene, file, indent="\t")


def png_size(path):
    with open(path, "rb") as file:
        header = file.read(24)
    if len(header) < 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", header[16:24])


def run_player(player, project, out, frames, timestep=None):
    """Runs the player on the copy with a per-user root of its own until it saves a picture of the right size."""
    for attempt in range(1, ATTEMPTS + 1):
        root = tempfile.mkdtemp(prefix="moto-user-")
        try:
            # The window's sound goes to no device: a tool's capture plays nothing through the speakers.
            environment = dict(os.environ, STRADA_AUDIO_OUTPUT="null")
            for variable, folder in (("APPDATA", "Roaming"), ("LOCALAPPDATA", "Local"), ("HOME", "Home"), ("XDG_DATA_HOME", "Data")):
                environment[variable] = os.path.join(root, folder)
                os.makedirs(environment[variable])
            command = [player, "--project", project, "--resolution", f"{WIDTH}x{HEIGHT}", "--frames", str(frames),
                       "--screenshot", out]
            if timestep is not None:
                command += ["--timestep", repr(timestep)]
            if os.path.exists(out):
                os.remove(out)
            result = subprocess.run(command, env=environment, capture_output=True, text=True, errors="replace")
            size = png_size(out) if os.path.exists(out) else None
            if result.returncode == 0 and size == (WIDTH, HEIGHT):
                return
            print(f"  {os.path.basename(out)}: attempt {attempt} exited {result.returncode} with "
                  f"{'no picture' if size is None else f'a picture of {size[0]}x{size[1]}'}")
            for line in (result.stdout + result.stderr).splitlines():
                if "rror" in line or "creenshot" in line:
                    print("    " + line[-300:])
        finally:
            shutil.rmtree(root, ignore_errors=True)
    raise SystemExit(f"{os.path.basename(out)} was not captured")


def take(player, output, names=None):
    """Takes the views named (all of them by default) into the output folder."""
    chosen = [view for view in views() if names is None or view.name in names]
    unknown = set(names or ()) - {view.name for view in chosen}
    if unknown:
        raise SystemExit(f"no such view: {', '.join(sorted(unknown))} (the views: {', '.join(view.name for view in views())})")
    os.makedirs(output, exist_ok=True)
    copy = tempfile.mkdtemp(prefix="moto-captures-")
    try:
        shutil.copytree(os.path.join(PROJECT, "Assets"), os.path.join(copy, "Assets"))
        shutil.copy(os.path.join(PROJECT, "Moto.sproj"), copy)
        project = os.path.join(copy, "Moto.sproj")
        scene_path = os.path.join(copy, "Assets", "Scenes", "Main.sscene")
        with open(scene_path, encoding="utf-8") as file:
            original = json.load(file)
        hide(original["Entities"], find(original["Entities"], Ghost.ENTITY))
        for view in chosen:
            out = os.path.join(output, f"{view.name}.png")
            if view.tour_time is not None:
                with open(scene_path, "w", encoding="utf-8") as file:
                    json.dump(original, file, indent="\t")
                # The tour starts with the first frame: at a step of 1/60 s, the frame of a time is its 60ths.
                run_player(player, project, out, round(view.tour_time * 60.0) + 1, 1.0 / 60.0)
            else:
                stage(view, scene_path, original)
                run_player(player, project, out, STILL_FRAMES)
            print(f"{view.name}: {view.purpose}" + ("" if view.eye is None else
                  f"; {math.dist(view.eye, view.target):.1f} m from what it looks at, field of view {view.fov:.1f} degrees"))
    finally:
        shutil.rmtree(copy, ignore_errors=True)

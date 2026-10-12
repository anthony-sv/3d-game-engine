"""Builds the Showcase project through `strada mcp`, the way an agent does: Poly Haven assets (sky, ground, texture sets,
props), the generated sounds, meshes and textures, the C# scripts, materials, prefabs, the main scene with its exhibits and
the test scene; then runs the tests. The downloads are not in the repository (FetchAssets.py restores them).

    python Projects/Showcase/Tools/Author.py [strada executable]

The project is rebuilt from scratch: everything but this Tools folder, the .gitignore and the scripts' sources
(Scripts/Source) is replaced. Without an executable, strada runs from the repository (dotnet run).

The clearing is laid out in exhibit areas around a fountain, each with a signpost: physically based materials (west), glass
and cutouts (east), lights and shadows (north-west, in an evening zone of post-processing), physics (north), triggers
(north-east), particles and sound around the fountain and the campfire, and a yard to walk and climb in (south-east). A global
post-process volume grades the scene; day turns to night at the press of a button. The camera tours the exhibits when the game
starts, under a neon sign that flickers on; then it flies or walks, and a HUD (a UI canvas) lists the features, counts what was
knocked down and scored, and sets the exhibits off.
"""
import json
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(TOOLS)
ROOT = os.path.dirname(os.path.dirname(PROJECT))
SCRIPTS = ["CameraRig", "Walker", "CameraShake", "Flythrough", "CameraShot", "ITourCue", "Impact", "LanternFlicker", "Playground",
           "WreckingBall", "DominoRun", "BallLauncher", "GoalZone", "PressurePlate", "Spinner", "Sway", "LightShow", "DebugView",
           "NeonSign", "DayNight", "Lift", "Hud", "HudFeature", "ShowcaseTests"]

HDRI = "autumn_forest_04"
GROUND = "dry_decay_leaves"
# Texture sets (color, normal and ambient occlusion/roughness/metallic maps): the hero objects, pedestals, wood and paving.
RUST = "rusty_metal_02"
PAINTED_METAL = "blue_metal_plate"
MARBLE = "marble_01"
PLANKS = "brown_planks_05"
COBBLES = "cobblestone_floor_08"
BARK = "bark_brown_02"
TEXTURES = (GROUND, RUST, PAINTED_METAL, MARBLE, PLANKS, COBBLES, BARK)
CRATE = "wooden_crate_01"
BARREL = "wine_barrel_01"
TABLE = "wooden_picnic_table"
LANTERN = "Lantern_01"
BOULDER = "boulder_01"
LOG = "dead_tree_trunk"
FERN = "fern_02"
BRANCHES = "dry_branches_medium_01"
STUMP = "tree_stump_01"
STUMP_2 = "tree_stump_02"
ROCKS = "rock_moss_set_02"
ROCKS_2 = "rock_moss_set_01"
BIG_LOG = "dead_tree_trunk_02"
BUST = "marble_bust_01"
STREET_LAMP = "street_lamp_01"
BENCH = "painted_wooden_bench"
SHRUBS = "shrub_02"
MODELS = (CRATE, BARREL, TABLE, LANTERN, BOULDER, LOG, FERN, BRANCHES, STUMP, STUMP_2, ROCKS, ROCKS_2, BIG_LOG, BUST, STREET_LAMP, BENCH, SHRUBS)
# The sun of the sky (HDRI): the direction towards it in the panorama (u = 0.5 looks down -Z), 8.3 degrees above the
# horizon, measured from its brightest pixels. The sky turns by SKY_ROTATION degrees, which brings the sun round to the
# east-south-east: it lights the gallery and the neon sign the tour opens on from the front, and the exhibits around the
# plaza from the side with long shadows.
HDRI_SUN = (0.582, 0.144, -0.800)
SKY_ROTATION = 85.0
# The directional light comes from the sun's bearing but higher than the panorama's 8 degrees: a sun that low lights the
# ground so little that shadows on it fade into the sky's light. From 16 degrees the trees' and exhibits' shadows stretch
# four times their height across sunlit leaves, and the disc in the sky stays behind the camera in most views.
SUN_ELEVATION = 16.0


def texture_resolution(texture):
    """The large surfaces (the ground, the paving) get 2k maps, the rest 1k."""
    return "2k" if texture in (GROUND, COBBLES) else "1k"


class McpClient:
    """A minimal MCP client over stdio."""

    def __init__(self, command):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=ROOT)
        self.counter = 0
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "author", "version": "1"}})
        self.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        self.process.stdin.flush()

    def request(self, method, params=None):
        self.counter += 1
        message = {"jsonrpc": "2.0", "id": self.counter, "method": method}
        if params is not None:
            message["params"] = params
        self.process.stdin.write((json.dumps(message) + "\n").encode())
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise SystemExit(f"strada mcp ended during {method}: {self.process.stderr.read().decode()}")
        response = json.loads(line)
        if "error" in response:
            raise SystemExit(f"{method} failed: {response['error']}")
        return response["result"]

    def call(self, tool, arguments=None):
        result = self.request("tools/call", {"name": tool, "arguments": arguments or {}})
        text = result["content"][-1]["text"]
        if result["isError"]:
            raise SystemExit(f"{tool} {json.dumps(arguments)} failed: {text}")
        return json.loads(text)

    def close(self):
        self.process.stdin.close()
        return self.process.wait(timeout=120)


# --- glTF bounds: colliders are fitted to the models ------------------------------------------------------------------

def multiply(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def node_matrix(node):
    if "matrix" in node:
        m = node["matrix"]
        return [[m[column * 4 + row] for column in range(4)] for row in range(4)]
    tx, ty, tz = node.get("translation", [0.0, 0.0, 0.0])
    qx, qy, qz, qw = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    sx, sy, sz = node.get("scale", [1.0, 1.0, 1.0])
    rotation = [[1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)],
                [2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)],
                [2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)]]
    scale = (sx, sy, sz)
    return [[rotation[r][0] * scale[0], rotation[r][1] * scale[1], rotation[r][2] * scale[2], (tx, ty, tz)[r]] for r in range(3)] + \
           [[0.0, 0.0, 0.0, 1.0]]


def gltf_bounds(path):
    """The model's bounds in meters (Y up), from the accessors' min and max through the node transforms."""
    with open(path, encoding="utf-8") as file:
        document = json.load(file)
    nodes = document["nodes"]
    scene = document["scenes"][document.get("scene", 0)]
    low = [math.inf] * 3
    high = [-math.inf] * 3
    stack = [(index, [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]) for index in scene["nodes"]]
    while stack:
        index, parent = stack.pop()
        node = nodes[index]
        world = multiply(parent, node_matrix(node))
        if "mesh" in node:
            for primitive in document["meshes"][node["mesh"]]["primitives"]:
                accessor = document["accessors"][primitive["attributes"]["POSITION"]]
                minimum, maximum = accessor["min"], accessor["max"]
                for corner in range(8):
                    point = [maximum[axis] if corner >> axis & 1 else minimum[axis] for axis in range(3)]
                    transformed = [sum(world[row][k] * point[k] for k in range(3)) + world[row][3] for row in range(3)]
                    low = [min(low[axis], transformed[axis]) for axis in range(3)]
                    high = [max(high[axis], transformed[axis]) for axis in range(3)]
        stack.extend((child, world) for child in node.get("children", []))
    return low, high


# --- Transforms ------------------------------------------------------------------------------------------------------------

def quaternion(euler):
    """The rotation of Euler angles in degrees as [x, y, z, w], as the engine (glm) composes them: Z * Y * X."""
    half = [math.radians(angle) * 0.5 for angle in euler]
    c = [math.cos(angle) for angle in half]
    s = [math.sin(angle) for angle in half]
    return [s[0] * c[1] * c[2] - c[0] * s[1] * s[2], c[0] * s[1] * c[2] + s[0] * c[1] * s[2],
            c[0] * c[1] * s[2] - s[0] * s[1] * c[2], c[0] * c[1] * c[2] + s[0] * s[1] * s[2]]


def quaternion_multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]


def yaw_point(point, yaw):
    """The point turned by the yaw (degrees) about the vertical axis, as a rotation of the entity by the yaw turns it."""
    angle = math.radians(yaw)
    x, y, z = point
    return (x * math.cos(angle) + z * math.sin(angle), y, -x * math.sin(angle) + z * math.cos(angle))


def transform(translation, euler=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    """A local transform (children, and entities that are not part of the turned layout)."""
    return {"Translation": list(translation), "Rotation": quaternion(euler), "Scale": list(scale)}


def sun_euler():
    """The directional light's Euler angles: it shines from the sky sun's bearing, SUN_ELEVATION degrees up. The sky shows the
    panorama's direction turned by SKY_ROTATION about +Y in each world direction, so the sun is at HDRI_SUN turned back by
    it."""
    x, _, z = HDRI_SUN
    angle = math.radians(-SKY_ROTATION)
    toward_x, toward_z = x * math.cos(angle) + z * math.sin(angle), -x * math.sin(angle) + z * math.cos(angle)
    # The light's forward (-Z) points away from the sun.
    return (-SUN_ELEVATION, math.degrees(math.atan2(toward_x, toward_z)), 0.0)


class Frame:
    """An area of the layout: an origin and a yaw. Points and rotations given in it are turned into world space."""

    def __init__(self, origin=(0.0, 0.0, 0.0), yaw=0.0):
        self.origin = origin
        self.yaw = yaw

    def point(self, local):
        turned = yaw_point(local, self.yaw)
        return (self.origin[0] + turned[0], self.origin[1] + turned[1], self.origin[2] + turned[2])

    def rotation(self, euler=(0.0, 0.0, 0.0)):
        return quaternion_multiply(quaternion((0.0, self.yaw, 0.0)), quaternion(euler))

    def place(self, local, euler=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
        return {"Translation": list(self.point(local)), "Rotation": self.rotation(euler), "Scale": list(scale)}

    def angles(self, eye, target):
        """The pitch and yaw (degrees, in world space) of a view from the eye towards the target (both in the frame)."""
        dx, dy, dz = (target[k] - eye[k] for k in range(3))
        return math.degrees(math.atan2(dy, math.hypot(dx, dz))), self.yaw + math.degrees(math.atan2(-dx, -dz))

    def look(self, eye, target):
        """A camera transform at the eye looking at the target (both in the frame): pitch and yaw, no roll."""
        pitch, yaw = self.angles(eye, target)
        return {"Translation": list(self.point(eye)), "Rotation": quaternion((pitch, yaw, 0.0)), "Scale": [1.0, 1.0, 1.0]}


# The layout: north is -Z, the opening looks north from the south.
LAYOUT = Frame()
# The materials gallery (west) and the glass garden (east) face the plaza.
GALLERY = Frame((-12.0, 0.0, 1.0), 62.0)
GARDEN = Frame((11.6, 0.0, 2.2), -58.0)
# The yard (south-east), where the walker climbs: its front (+Z) faces the plaza, +X is on the right seen from there.
YARD = Frame((11.0, 0.0, 14.5), -142.8)
# The yard's heights: the deck the stairs climb to, and the lookout the lift rises to.
DECK_TOP = 1.5
LOOKOUT_TOP = 3.4

# Where the free camera starts and the tour ends: south of the plaza, looking north over the fountain.
HOME = (LAYOUT, (0.0, 2.9, 16.0), (0.0, 1.1, -2.0))

# The opening tour: the shots the camera glides through, as (name, frame, eye, target, seconds to the next shot, caption,
# detail line, the exhibit set off on the way: an attribute of Showcase whose script is a tour cue, and how many seconds before
# the camera arrives it goes off). It opens on the neon sign, the engine's name in light, and pulls back to the gallery under
# it; it ends at the campfire, beside the home view.
TOUR = (
    ("Opening", GALLERY, (-0.8, 5.6, 4.6), (0.2, 6.5, -0.6), 6.0, "", "", "neon_sign", 0.0),
    ("Gallery", GALLERY, (0.8, 3.2, 8.2), (0.0, 2.7, 0.0), 4.5, "Physically based materials",
     "metallic and roughness, normal and occlusion maps, emission and bloom", None, 0.0),
    ("Lights", LAYOUT, (-6.55, 5.2, -0.35), (-9.6, 1.2, -7.2), 4.5, "Lights and shadows", "a lighthouse beam, colored spots and soft sun shadows",
     None, 0.0),
    ("Physics", LAYOUT, (2.5, 2.4, -5.5), (-1.4, 1.5, -10.2), 4.5, "Rigid bodies and joints", "a wrecking ball on a ball joint meets a tower of crates",
     "wrecking_ball", 0.6),
    ("Dominoes", LAYOUT, (2.8, 3.0, -11.2), (1.8, 0.4, -16.0), 4.0, "Chain reactions", "every domino is a rigid body", "dominoes", 3.0),
    ("Triggers", LAYOUT, (4.4, 2.8, -0.6), (12.4, 3.0, -10.4), 4.5, "Triggers", "a goal counts, a pressure plate opens the gate", "launcher", 1.3),
    ("Glass", LAYOUT, (4.2, 2.5, 8.6), (11.0, 1.7, 2.8), 4.5, "Transparency", "blended glass, alpha-tested lattice, two-sided cloth", None, 0.0),
    ("Yard", LAYOUT, (3.6, 5.0, 8.6), (10.6, 1.4, 15.0), 4.5, "Character controller",
     "press F to walk: climb the stairs, jump the posts, ride the lift", None, 0.0),
    ("Campfire", LAYOUT, (-1.0, 1.5, 13.8), (-4.8, 1.0, 6.2), 4.0, "Particles and sound",
     "fire, smoke, sparks, water and fireflies, heard where they are", None, 0.0),
    ("Home", *HOME, 3.0, "", "", None, 0.0),
)


def world_text(text, size, color, alignment="Center"):
    return {"Text": text, "Font": "builtin://DefaultFont", "Color": list(color), "FontSize": size, "Alignment": alignment}


# --- The screen: UI elements ---------------------------------------------------------------------------------------------

def linear(r, g, b, a=1.0):
    """A color picked in sRGB (as design tools show it) as the linear color the engine's colors are."""
    def channel(value):
        return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4
    return [channel(r), channel(g), channel(b), a]


def ui_rect(anchor, pivot, position, size, anchor_max=None, **fields):
    """A UIRect placed at a point of its parent (anchor, from (0, 0) at the top-left corner to (1, 1)), or stretched with it up
    to anchor_max; its pivot sits there, moved by the position; canvas units, y downwards."""
    rect = {"AnchorMin": list(anchor), "AnchorMax": list(anchor_max or anchor), "Pivot": list(pivot), "Position": list(position),
            "Size": list(size)}
    rect.update(fields)
    return rect


def ui_fill(**fields):
    """A UIRect covering its parent."""
    return ui_rect((0.0, 0.0), (0.5, 0.5), (0.0, 0.0), (0.0, 0.0), (1.0, 1.0), **fields)


def ui_text(text, size, color, alignment="Left", vertical="Middle", **fields):
    return {"Text": text, "FontSize": size, "Color": color, "Alignment": alignment, "VerticalAlignment": vertical, **fields}


# The HUD's palette: dark glass panels, warm white text, an amber accent (the neon sign's and the lamps' warmth).
PANEL = linear(0.05, 0.055, 0.07, 0.68)
TEXT = linear(0.96, 0.94, 0.9)
DIM = linear(0.66, 0.66, 0.66)
AMBER = linear(1.0, 0.68, 0.3)
BUTTON = {"NormalColor": linear(0.05, 0.055, 0.07, 0.7), "HoveredColor": linear(0.5, 0.29, 0.08, 0.92),
          "PressedColor": linear(0.32, 0.17, 0.04, 0.95), "DisabledColor": linear(0.1, 0.1, 0.1, 0.45)}

# The feature list: a row per exhibit area, lit while the camera is near it (name, what it shows, where on the ground (x, z), and
# within how many meters it is fully lit); the last row, with no place, lights up while the mouse is over the HUD.
FEATURES = (
    ("Physically based materials", "metallic, roughness, normal, occlusion, emission", (-11.2, 1.4), 6.0),
    ("Transparency", "blended glass, alpha-tested lattice, two-sided cloth", (11.6, 2.2), 6.0),
    ("Lights and shadows", "sun and sky, spot and point lights, soft shadows", (-8.6, -10.5), 7.0),
    ("Post-processing", "bloom, ambient occlusion, grading, local volumes", (-9.6, -7.2), 4.5),
    ("Rigid bodies and joints", "Jolt physics: crates, dominoes, a wrecking ball", (0.5, -11.5), 7.0),
    ("Triggers", "a goal that counts, a plate that opens a gate", (11.0, -8.0), 6.0),
    ("Character controller", "walk, run, jump, climb stairs, ride a lift, push", (11.0, 14.5), 7.0),
    ("Particles and 3D sound", "fire, water, sparks and fireflies, heard in space", (-2.2, 4.0), 7.0),
    ("UI and C# scripting", "this HUD, and how every exhibit behaves", None, 0.0),
)

# The HUD's buttons: (entity name, label, icon, key, Hud field).
BUTTONS = (
    ("Reset Button", "Reset exhibits", "Reset", "R", "ResetButton"),
    ("Swing Button", "Swing the wrecking ball", "Swing", "1", "SwingButton"),
    ("Launch Button", "Launch a ball", "Launch", "3", "LaunchButton"),
    ("Lights Button", "Toggle lights", "Sun", "L", "LightsButton"),
    ("Mode Button", "Walk", "Walk", "F", "ModeButton"),
)

# The hint line, by the way the camera moves.
FLY_HINT = ("Right mouse  look      W A S D  move      Q / E  down · up      Shift  faster      Click  throw a ball      F  walk      "
            "T  debug view      H  hide HUD      C  tour")
WALK_HINT = ("Mouse  look      W A S D  walk      Shift  run      Space  jump      Click  throw a ball      Esc  free the cursor      "
             "F  fly      H  hide HUD")
FREE_CURSOR_HINT = ("Click the scene  look around      W A S D  walk      Shift  run      Space  jump      F  fly      H  hide HUD      "
                    "C  tour")


def use_engine_sources(csproj):
    """In the engine's repository, builds without the machine's Strada.props (dotnet format in CI, IDEs) take the scripting
    API from its sources; CMake builds pass StradaScriptCoreAssembly."""
    with open(csproj, encoding="utf-8") as file:
        text = file.read()
    reference = '\t<ItemGroup>\n\t\t<Reference Include="Strada.ScriptCore">'
    if reference not in text:
        raise SystemExit(f"{csproj} does not reference Strada.ScriptCore as expected")
    text = text.replace(reference, '\t<ItemGroup Condition="\'$(StradaScriptCoreAssembly)\' != \'\'">\n\t\t<Reference Include="Strada.ScriptCore">')
    text = text.replace("</Project>", """\t<!-- In the engine's repository, builds without Strada.props (dotnet format, IDEs) use the scripting API from source;
\t     CMake builds pass StradaScriptCoreAssembly. -->
\t<ItemGroup Condition="'$(StradaScriptCoreAssembly)' == ''">
\t\t<ProjectReference Include="../../../Strada-ScriptCore/Strada.ScriptCore.csproj">
\t\t\t<Private>false</Private>
\t\t</ProjectReference>
\t</ItemGroup>
</Project>""")
    with open(csproj, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)


def main():
    command = [sys.argv[1]] if len(sys.argv) > 1 else \
        ["dotnet", "run", "--project", os.path.join(ROOT, "StradaTool"), "-c", "Release", "--no-launch-profile", "--"]

    # The editor starts before anything is touched: a tool that cannot run leaves the project as it is.
    client = McpClient(command + ["mcp", "--headless", "--new"])
    # The editor creates projects in empty directories only: what is kept waits outside meanwhile.
    aside = tempfile.mkdtemp(prefix="showcase-author-")
    sources = os.path.join(aside, "Source")
    shutil.copytree(os.path.join(PROJECT, "Scripts", "Source"), sources)
    kept = ("Tools", ".gitignore")
    for entry in kept:
        if os.path.exists(os.path.join(PROJECT, entry)):
            shutil.move(os.path.join(PROJECT, entry), os.path.join(aside, entry))
    try:
        shutil.rmtree(PROJECT)
        client.call("project_create", {"directory": PROJECT, "name": "Showcase"})
    except BaseException:
        # The sources come back when the project could not be made (on success the scripts are created from them).
        if not os.path.isdir(os.path.join(PROJECT, "Scripts", "Source")):
            shutil.copytree(sources, os.path.join(PROJECT, "Scripts", "Source"))
        raise
    finally:
        os.makedirs(PROJECT, exist_ok=True)
        for entry in kept:
            if os.path.exists(os.path.join(aside, entry)):
                shutil.move(os.path.join(aside, entry), os.path.join(PROJECT, entry))
    try:
        Showcase(client).build(sources)
    finally:
        client.close()


class Showcase:
    """The authoring session: assets first, then the scene, exhibit by exhibit."""

    def __init__(self, client):
        self.client = client
        self.models = {}
        self.textures = {}
        self.ids = {}

    def call(self, tool, arguments=None):
        return self.client.call(tool, arguments)

    def entity(self, name, components=None, parent=None, local=True):
        """Creates an entity (under the parent, with its transform relative to it when local) and returns its ID."""
        arguments = {"name": name, "components": components or {}}
        if parent is not None and local:
            arguments["parent"] = parent
        created = self.call("entity_create", arguments)["id"]
        if parent is not None and not local:
            self.call("entity_reparent", {"entity": created, "parent": parent})
        return created

    def material(self, path, fields):
        self.call("material_create", {"path": f"Materials/{path}.smat", "fields": fields})
        return f"asset://Materials/{path}.smat"

    def mesh(self, mesh, material, extra=None, shadows=True):
        """A Mesh component of a built-in or project mesh with one material. Fittings around a shadow-casting light (lenses,
        housings, lamp glass) pass shadows=False: in its shadow map they would hide everything the light shines on."""
        components = {"Mesh": {"Mesh": mesh if "://" in mesh else f"builtin://{mesh}", "Materials": [material] if material else [],
                               "CastShadows": shadows}}
        components.update(extra or {})
        return components

    # --- Assets ---------------------------------------------------------------------------------------------------------

    def build(self, sources):
        call = self.call
        call("project_settings", {"settings": {"ScriptModule": "Scripts/Binaries/Showcase.dll",
                                               "Window": {"Title": "Strada Showcase", "Width": 1600, "Height": 900}}})

        # Scripts: the editor makes the C# project with the first class; the sources replace the templates.
        for name in SCRIPTS:
            call("script_create", {"className": name})
        for name in SCRIPTS:
            shutil.copyfile(os.path.join(sources, f"{name}.cs"), os.path.join(PROJECT, "Scripts", "Source", f"{name}.cs"))
        use_engine_sources(os.path.join(PROJECT, "Scripts", "Showcase.csproj"))
        built = call("script_build")
        if not built["succeeded"]:
            raise SystemExit(f"the scripts did not build: {built}")

        # Generated sounds, meshes and textures.
        subprocess.run([sys.executable, os.path.join(TOOLS, "GenerateAudio.py")], check=True)
        subprocess.run([sys.executable, os.path.join(TOOLS, "GenerateAssets.py")], check=True)
        call("asset_refresh")

        # Poly Haven: the sky, the texture sets and the props.
        self.sky = call("polyhaven_import_hdri", {"id": HDRI, "resolution": "4k"})["asset"]["reference"]
        for texture in TEXTURES:
            imported = call("polyhaven_import_texture", {"id": texture, "resolution": texture_resolution(texture)})
            self.textures[texture] = imported["material"]["reference"]
        call("material_set", {"material": self.textures[GROUND], "fields": {"UVTiling": [26.0, 26.0]}})
        for model in MODELS:
            imported = call("polyhaven_import_model", {"id": model, "resolution": "1k"})
            path = os.path.join(PROJECT, "Assets", *imported["mesh"]["path"].split("/"))
            self.models[model] = (imported["mesh"]["reference"], gltf_bounds(path))
            print(model, "bounds", [[round(v, 3) for v in corner] for corner in self.models[model][1]])

        self.make_materials()
        self.make_scene()
        self.make_test_scene()

    def make_materials(self):
        material = self.material
        self.ball_metal = material("BallMetal", {"BaseColor": [0.95, 0.93, 0.88, 1.0], "Metallic": 1.0, "Roughness": 0.18})
        self.iron = material("Iron", {"BaseColor": [0.16, 0.16, 0.18, 1.0], "Metallic": 1.0, "Roughness": 0.38})
        self.red_paint = material("RedPaint", {"BaseColor": [0.62, 0.09, 0.05, 1.0], "Roughness": 0.45})
        self.white_paint = material("WhitePaint", {"BaseColor": [0.86, 0.85, 0.82, 1.0], "Roughness": 0.4})
        self.brass = material("Brass", {"BaseColor": [0.98, 0.76, 0.42, 1.0], "Metallic": 1.0, "Roughness": 0.28})
        self.chrome = material("Chrome", {"BaseColor": [0.97, 0.97, 0.98, 1.0], "Metallic": 1.0, "Roughness": 0.04})
        self.charcoal = material("Charcoal", {"BaseColor": [0.045, 0.045, 0.05, 1.0], "Roughness": 0.55})
        self.slate = material("Slate", {"BaseColor": [0.012, 0.014, 0.02, 1.0], "Roughness": 0.8})
        # Polished gray, so the spots' colored pools show on it.
        self.stage_floor = material("StageFloor", {"BaseColor": [0.16, 0.16, 0.17, 1.0], "Roughness": 0.28})
        self.plaster = material("Plaster", {"BaseColor": [0.92, 0.91, 0.88, 1.0], "Roughness": 0.65})
        self.orange_ball = material("OrangeBall", {"BaseColor": [1.0, 0.32, 0.04, 1.0], "Roughness": 0.4})
        # Warm lamp glass: lit from inside, it keeps its color instead of burning white.
        self.lamp_glass = material("LampGlass", {"BaseColor": [1.0, 0.85, 0.65, 0.45], "Roughness": 0.3, "AlphaMode": "Blend",
                                                 "EmissiveColor": [1.0, 0.6, 0.28], "EmissiveIntensity": 1.6})
        # Glass: alpha-blended, smooth; its reflections stay as its tint fades what is behind.
        self.glass = {}
        for name, color in (("Clear", (0.9, 0.95, 1.0, 0.16)), ("Amber", (1.0, 0.55, 0.12, 0.38)), ("Teal", (0.15, 0.85, 0.75, 0.32)),
                            ("Rose", (1.0, 0.25, 0.55, 0.32)), ("Violet", (0.62, 0.32, 1.0, 0.42)), ("Ice", (0.6, 0.9, 1.0, 0.3)),
                            ("Water", (0.08, 0.22, 0.26, 0.62)), ("Lamp", (1.0, 0.9, 0.7, 0.25)), ("Backboard", (0.55, 0.75, 1.0, 0.22))):
            fields = {"BaseColor": list(color), "Roughness": 0.04, "AlphaMode": "Blend"}
            if name == "Lamp":
                fields.update({"EmissiveColor": [1.0, 0.8, 0.5], "EmissiveIntensity": 3.0})
            self.glass[name] = material(f"Glass{name}", fields)
        # Neon tubes and glowing parts: bright emission that the bloom spreads.
        self.neon = {}
        for name, color, intensity in (("Pink", (1.0, 0.12, 0.45), 26.0), ("Cyan", (0.1, 0.85, 1.0), 22.0), ("Amber", (1.0, 0.55, 0.12), 20.0),
                                       ("Lamp", (1.0, 0.66, 0.36), 9.0), ("Lens", (1.0, 1.0, 1.0), 14.0), ("Plate", (0.2, 1.0, 0.45), 8.0)):
            self.neon[name] = material(f"Neon{name}", {"BaseColor": [0.02, 0.02, 0.02, 1.0], "Roughness": 0.3,
                                                       "EmissiveColor": list(color), "EmissiveIntensity": intensity})
        # Light shafts: glowing, nearly clear, fading out along their length (the texture's alpha and color).
        self.beam = material("LighthouseBeam", {"BaseColor": [0.0, 0.0, 0.0, 0.025], "BaseColorTexture": "asset://Textures/BeamFade.png",
                                                "Roughness": 1.0, "AlphaMode": "Blend", "EmissiveColor": [1.0, 0.8, 0.5],
                                                "EmissiveIntensity": 10.0, "EmissiveTexture": "asset://Textures/BeamFade.png",
                                                "DoubleSided": True})
        self.spot_beam = material("SpotBeam", {"BaseColor": [0.0, 0.0, 0.0, 0.02], "BaseColorTexture": "asset://Textures/BeamFade.png",
                                               "Roughness": 1.0, "AlphaMode": "Blend", "EmissiveColor": [1.0, 1.0, 1.0],
                                               "EmissiveIntensity": 9.0, "EmissiveTexture": "asset://Textures/BeamFade.png",
                                               "DoubleSided": True})
        # Cutouts and cloth: alpha-tested, both sides lit.
        self.lattice = material("Lattice", {"BaseColorTexture": "asset://Textures/Lattice.png", "AlphaMode": "Mask", "AlphaCutoff": 0.5,
                                            "DoubleSided": True, "Roughness": 0.7, "UVTiling": [2.0, 2.0]})
        self.net = material("Net", {"BaseColorTexture": "asset://Textures/Net.png", "AlphaMode": "Mask", "AlphaCutoff": 0.5,
                                    "DoubleSided": True, "Roughness": 0.9, "UVTiling": [1.5, 1.0]})
        self.banners = [material(f"Banner{name}", {"BaseColor": list(color), "BaseColorTexture": "asset://Textures/Banner.png",
                                                   "AlphaMode": "Mask", "DoubleSided": True, "Roughness": 0.85})
                        for name, color in (("Crimson", (0.62, 0.05, 0.08, 1.0)), ("Royal", (0.08, 0.18, 0.62, 1.0)))]
        # The gallery: metallic from 0 to 1 left to right, roughness from 0 to 1 top to bottom, one material per sphere.
        self.sweep = {}
        for column in range(5):
            for row in range(5):
                metallic, roughness = column / 4.0, row / 4.0
                self.sweep[(column, row)] = material(f"Gallery/Metallic{column * 25}Roughness{row * 25}", {
                    "BaseColor": [1.0, 0.766, 0.336, 1.0], "Metallic": metallic, "Roughness": roughness})
        # Dominoes in the colors of the rainbow.
        self.domino_colors = []
        for index in range(8):
            hue = index / 8.0
            sector = hue * 6.0
            rgb = [min(1.0, max(0.0, abs(sector - 3.0) - 1.0)), min(1.0, max(0.0, 2.0 - abs(sector - 2.0))),
                   min(1.0, max(0.0, 2.0 - abs(sector - 4.0)))]
            self.domino_colors.append(material(f"Dominoes/Domino{index + 1}", {
                "BaseColor": [0.08 + 0.8 * channel for channel in rgb] + [1.0], "Roughness": 0.3}))
        # Stained wood for the signposts.
        self.call("material_create", {"path": "Materials/SignWood.smat", "fields": {}})
        planks = self.call("material_get", {"material": self.textures[PLANKS]})["fields"]
        planks.update({"BaseColor": [0.55, 0.42, 0.32, 1.0], "UVTiling": [1.0, 0.35]})
        self.call("material_set", {"material": "asset://Materials/SignWood.smat", "fields": planks})
        self.sign_wood = "asset://Materials/SignWood.smat"
        self.call("material_set", {"material": self.textures[COBBLES], "fields": {"UVTiling": [5.0, 5.0]}})
        self.call("material_set", {"material": self.textures[MARBLE], "fields": {"UVTiling": [1.0, 1.0]}})
        # The trees' texture coordinates go once around a limb and once per its girth along it: two repeats each way.
        self.call("material_set", {"material": self.textures[BARK], "fields": {"UVTiling": [2.0, 2.0]}})

    # --- The scene ------------------------------------------------------------------------------------------------------

    def make_scene(self):
        call = self.call
        # The scene's own settings are neutral: its look (golden hour) comes from a global post-process volume (make_grading).
        call("scene_settings", {"name": "Showcase", "settings": {"Renderer": {
            "Tonemapper": "ACES", "EV100": -0.55, "Shadows": True, "SoftShadows": True, "ShadowDistance": 60.0, "CascadeCount": 4,
            "ShadowMapSize": 4096, "LocalShadowMapSize": 1024, "AmbientOcclusion": True, "AmbientOcclusionRadius": 0.8,
            "AmbientOcclusionIntensity": 1.2, "Bloom": True, "FXAA": True}}})
        found = {entity["name"]: entity["id"] for entity in call("entity_find", {})["entities"]}
        call("entity_delete", {"entities": [found[name] for name in ("Floor", "Cube", "Sphere", "Capsule")]})
        self.found = found

        call("component_set", {"entity": found["Sun"], "component": "Transform", "fields": {"RotationEuler": list(sun_euler())}})
        call("component_set", {"entity": found["Sun"], "component": "DirectionalLight",
                               "fields": {"Color": [1.0, 0.74, 0.48], "Intensity": 4.6, "CastShadows": True, "LightSize": 1.2}})
        call("component_set", {"entity": found["Sky"], "component": "SkyLight",
                               "fields": {"Environment": self.sky, "Intensity": 0.4, "Rotation": SKY_ROTATION, "DrawSkybox": True}})

        # The ground ends before the horizon: beyond it, the sky's own forest floor continues it.
        self.entity("Ground", {
            "Transform": transform((0.0, 0.0, 0.0), scale=(110.0, 1.0, 110.0)),
            "Mesh": {"Mesh": "builtin://Plane", "Materials": [self.textures[GROUND]]},
            "RigidBody": {"Type": "Static"},
            "BoxCollider": {"HalfExtents": [0.5, 0.05, 0.5], "Offset": [0.0, -0.05, 0.0], "Friction": 0.8}})

        self.make_camera()
        self.make_signpost_prefab()
        self.make_plaza()
        self.make_gallery()
        self.make_glass_garden()
        self.make_lights()
        self.make_physics()
        self.make_triggers()
        self.make_yard()
        self.make_scenery()
        self.make_sound()
        self.make_grading()
        self.make_playground()
        self.make_hud()
        self.make_tour()
        call("scene_save")

    def model_entity(self, name, model, transform_fields, components=None, parent=None):
        fields = {"Transform": transform_fields, "Mesh": {"Mesh": self.models[model][0]}}
        fields.update(components or {})
        return self.entity(name, fields, parent)

    def static_model(self, name, model, transform_fields, parent=None):
        return self.model_entity(name, model, transform_fields, {
            "RigidBody": {"Type": "Static"}, "MeshCollider": {"Mesh": self.models[model][0], "Convex": False, "Friction": 0.7}}, parent)

    def signpost(self, position, title, detail, facing=(0.0, 0.0)):
        """An instance of the signpost prefab standing at the position (x, z) and facing a point (x, z; the plaza's center by
        default), with the exhibit's name and what it shows."""
        x, z = position
        yaw = math.degrees(math.atan2(facing[0] - x, facing[1] - z))
        instance = self.call("prefab_instantiate", {"prefab": self.signpost_prefab})["id"]
        self.call("component_set", {"entity": instance, "component": "Transform", "fields": LAYOUT.place((x, 0.0, z), (0.0, yaw, 0.0))})
        for child in self.call("entity_get", {"entity": instance})["children"]:
            name = self.call("entity_get", {"entity": child})["name"]
            if name in ("Title", "Detail"):
                self.call("component_set", {"entity": child, "component": "Text", "fields": {"Text": title if name == "Title" else detail}})
        self.call("entity_rename", {"entity": instance, "name": f"Signpost: {title.title()}"})
        self.call("entity_reparent", {"entity": instance, "parent": self.signposts})
        return instance

    def make_signpost_prefab(self):
        """A board on two posts with the exhibit's name and a line about it in light letters on a dark face: a prefab placed
        at every exhibit."""
        post = self.entity("Signpost", {"Transform": transform((0.0, -100.0, 0.0)),
                                        "RigidBody": {"Type": "Static"},
                                        "BoxCollider": {"HalfExtents": [1.4, 1.15, 0.08], "Offset": [0.0, 1.15, 0.0]}})
        for name, mesh, position, scale, material in (
                ("Left Post", "Cylinder", (-1.2, 1.15, -0.07), (0.12, 2.3, 0.12), self.sign_wood),
                ("Right Post", "Cylinder", (1.2, 1.15, -0.07), (0.12, 2.3, 0.12), self.sign_wood),
                ("Board", "Cube", (0.0, 1.72, 0.0), (2.7, 1.0, 0.08), self.sign_wood),
                ("Face", "Cube", (0.0, 1.72, 0.03), (2.5, 0.82, 0.03), self.charcoal),
                ("Cap", "Cube", (0.0, 2.25, 0.0), (2.82, 0.07, 0.16), self.iron)):
            self.entity(name, {"Transform": transform(position, scale=scale), **self.mesh(mesh, material)}, post)
        self.entity("Title", {"Transform": transform((0.0, 2.09, 0.05)), "Text": world_text("TITLE", 0.28, (1.0, 0.86, 0.6, 1.0))}, post)
        self.entity("Detail", {"Transform": transform((0.0, 1.7, 0.05)),
                               "Text": world_text("detail", 0.12, (0.95, 0.93, 0.88, 1.0)) | {"LineSpacing": 1.15}}, post)
        self.signpost_prefab = self.call("prefab_create", {"entity": post, "path": "Prefabs/Signpost.sprefab"})["reference"]
        self.call("entity_delete", {"entities": [post]})
        self.signposts = self.entity("Signposts")

    # --- The camera: a rig that flies, with the camera (and its shake) under it -------------------------------------------

    def make_camera(self):
        call = self.call
        found = self.found
        frame, eye, target = HOME
        rig = self.entity("Camera Rig", {"Transform": frame.look(eye, target),
                                         "AudioSource": {"Clip": "asset://Audio/Throw.wav", "Volume": 0.6, "Spatial": False}})
        camera = found["Main Camera"]
        call("entity_reparent", {"entity": camera, "parent": rig, "keepWorldTransform": False})
        call("component_set", {"entity": camera, "component": "Transform", "fields": transform((0.0, 0.0, 0.0))})
        call("component_set", {"entity": camera, "component": "Camera", "fields": {"PerspectiveFOV": 55.0}})
        call("component_add", {"entity": camera, "component": "AudioListener"})
        call("component_add", {"entity": camera, "component": "Script", "fields": {"ClassName": "Showcase.CameraShake"}})
        self.rig = rig
        self.camera = camera

    def finish_camera(self, ball, crosshair):
        self.call("component_add", {"entity": self.rig, "component": "Script", "fields": {
            "ClassName": "Showcase.CameraRig", "Fields": {"View": {"Type": "Entity", "Value": self.camera},
                                                          "Crosshair": {"Type": "Entity", "Value": crosshair},
                                                          "Walker": {"Type": "Entity", "Value": self.walker},
                                                          "Ball": {"Type": "Prefab", "Value": ball}}}})

    # --- The plaza: a fountain on cobbles, the campfire and a picnic table ------------------------------------------------

    def make_plaza(self):
        plaza = self.entity("Plaza")
        self.entity("Paving", {"Transform": LAYOUT.place((0.0, 0.0, 0.0), scale=(12.0, 0.04, 12.0)),
                               **self.mesh("Cylinder", self.textures[COBBLES])}, plaza)
        fountain = self.entity("Fountain", {"Transform": LAYOUT.place((0.0, 0.02, 0.0)),
                                            **self.mesh("asset://Meshes/Fountain.gltf", self.textures[MARBLE]),
                                            "RigidBody": {"Type": "Static"},
                                            "MeshCollider": {"Mesh": "asset://Meshes/Fountain.gltf", "Convex": False}}, plaza)
        self.entity("Pool", {"Transform": transform((0.0, 0.47, 0.0), scale=(3.74, 0.02, 3.74)), **self.mesh("Cylinder", self.glass["Water"])},
                    fountain)
        self.entity("Bowl Water", {"Transform": transform((0.0, 1.295, 0.0), scale=(1.44, 0.02, 1.44)),
                                   **self.mesh("Cylinder", self.glass["Water"])}, fountain)
        # Water: drops a couple of centimeters apart along each jet, so they overlap into a stream; each is faint, the stream
        # is not.
        water = {"Rate": 130.0, "LifetimeMin": 0.9, "LifetimeMax": 1.1, "SpeedMin": 3.9, "SpeedMax": 4.2, "ConeAngle": 2.5, "Radius": 0.03,
                 "GravityFactor": 1.0, "Drag": 0.05, "StartSize": 0.065, "EndSize": 0.15, "StartColor": [0.85, 0.94, 1.0, 0.42],
                 "EndColor": [0.8, 0.9, 1.0, 0.0], "BlendMode": "Alpha", "MaxParticles": 220}
        jets = self.entity("Jets", {"Transform": transform((0.0, 0.0, 0.0))}, fountain)
        self.entity("Center Jet", {"Transform": transform((0.0, 1.66, 0.0), (90.0, 0.0, 0.0)), "ParticleSystem": water | {
            "SpeedMin": 4.4, "SpeedMax": 4.8, "Rate": 380.0, "LifetimeMax": 1.25, "StartSize": 0.075, "EndSize": 0.2, "MaxParticles": 700}}, jets)
        # Eight jets around the rim arc into the basin, towards the column.
        for index in range(8):
            angle = 360.0 * index / 8.0
            radians = math.radians(angle)
            position = (1.78 * math.cos(radians), 0.6, 1.78 * math.sin(radians))
            # Up (90) leaning inwards: the jet's forward (-Z) turned to point at the column and tipped up.
            yaw = math.degrees(math.atan2(math.cos(radians), math.sin(radians)))
            self.entity(f"Rim Jet {index + 1}", {"Transform": transform(position, (58.0, yaw, 0.0)),
                                                 "ParticleSystem": water | {"SpeedMin": 2.9, "SpeedMax": 3.1}}, jets)
        self.entity("Spray", {"Transform": transform((0.0, 0.55, 0.0), (90.0, 0.0, 0.0)), "ParticleSystem": {
            "Rate": 30.0, "Shape": "Sphere", "Radius": 0.9, "LifetimeMin": 0.4, "LifetimeMax": 0.8, "SpeedMin": 0.2, "SpeedMax": 0.6,
            "GravityFactor": 0.2, "StartSize": 0.12, "EndSize": 0.3, "StartColor": [0.9, 0.95, 1.0, 0.25], "EndColor": [0.9, 0.95, 1.0, 0.0],
            "BlendMode": "Alpha"}}, jets)
        self.fountain_jets = jets
        self.fountain = fountain

        # Two street lamps flank the way into the plaza from the south; benches face the fountain.
        # The lamp's glass panes and bulb (its second and third materials) glow; its light sits at the bulb.
        (lamp_low, _) = self.models[STREET_LAMP][1]
        for index, (x, z) in enumerate(((-2.4, 7.6), (2.4, 7.6))):
            facing = math.degrees(math.atan2(-x, -z))
            lamp = self.model_entity(f"Street Lamp {index + 1}", STREET_LAMP, LAYOUT.place((x, -lamp_low[1], z), (0.0, facing, 0.0)), {
                "Mesh": {"Mesh": self.models[STREET_LAMP][0], "Materials": ["0", self.lamp_glass, self.neon["Lamp"]]},
                "RigidBody": {"Type": "Static"}, "BoxCollider": {"HalfExtents": [0.12, 1.9, 0.12], "Offset": [0.0, 1.9, 0.0]}}, plaza)
            self.entity("Lamp Light", {"Transform": transform((0.0, 3.27 - lamp_low[1], 0.0)),
                                       "PointLight": {"Color": [1.0, 0.72, 0.42], "Intensity": 9.0, "Range": 8.0}}, lamp)
        for index, angle in enumerate((165.0, 195.0, 15.0, -15.0)):
            radians = math.radians(angle)
            position = (4.4 * math.cos(radians), -self.models[BENCH][1][0][1], 4.4 * math.sin(radians))
            # The seat (the model's +Z) faces the fountain.
            facing = math.degrees(math.atan2(-math.cos(radians), -math.sin(radians)))
            self.model_entity(f"Bench {index + 1}", BENCH, LAYOUT.place(position, (0.0, facing, 0.0)), {
                "RigidBody": {"Type": "Static"}, "BoxCollider": {"HalfExtents": [0.58, 0.44, 0.25], "Offset": [0.0, 0.44, 0.0]}}, plaza)

        # The campfire: a ring of stones, logs, fire, smoke, sparks and a flickering light.
        campfire = self.entity("Campfire", {"Transform": LAYOUT.place((-4.4, 0.0, 8.2))})
        (boulder_low, _) = self.models[BOULDER][1]
        for index in range(9):
            stone_angle = 2.0 * math.pi * index / 9.0
            size = 0.11 + 0.02 * (index % 3)
            self.model_entity(f"Stone {index + 1}", BOULDER, transform((math.cos(stone_angle) * 0.6, -boulder_low[1] * size,
                                                                        math.sin(stone_angle) * 0.6), (0.0, 47.0 * index, 0.0),
                                                                       (size, size, size)), parent=campfire)
        for index, yaw in enumerate((25.0, 115.0)):
            self.model_entity(f"Log {index + 1}", LOG, transform((0.0, -self.models[LOG][1][0][1] * 0.16 + 0.04 * index, 0.0), (0.0, yaw, 0.0),
                                                                 (0.16, 0.16, 0.16)), parent=campfire)
        effects = (
            # Tongues of flame rising from the logs, and a hotter, smaller core at their foot: bright enough to read as fire
            # in the low sun.
            ("Fire", 0.05, {"Rate": 140.0, "LifetimeMin": 0.45, "LifetimeMax": 1.0, "SpeedMin": 1.0, "SpeedMax": 2.1, "ConeAngle": 10.0,
                            "Radius": 0.25, "GravityFactor": -0.2, "Drag": 1.1, "StartSize": 0.42, "EndSize": 0.06,
                            "StartColor": [1.0, 0.36, 0.07, 0.75], "EndColor": [0.65, 0.04, 0.01, 0.0], "Intensity": 2.6,
                            "MaxParticles": 300}),
            ("Fire Core", 0.04, {"Rate": 60.0, "LifetimeMin": 0.25, "LifetimeMax": 0.5, "SpeedMin": 0.6, "SpeedMax": 1.2, "ConeAngle": 6.0,
                                 "Radius": 0.12, "GravityFactor": -0.2, "Drag": 0.6, "StartSize": 0.3, "EndSize": 0.04,
                                 "StartColor": [1.0, 0.68, 0.3, 0.8], "EndColor": [1.0, 0.28, 0.05, 0.0], "Intensity": 3.5}),
            ("Smoke", 0.7, {"Rate": 10.0, "LifetimeMin": 3.0, "LifetimeMax": 5.0, "SpeedMin": 0.4, "SpeedMax": 0.9, "ConeAngle": 15.0,
                            "Radius": 0.3, "GravityFactor": -0.05, "Drag": 0.3, "StartSize": 0.5, "EndSize": 1.8,
                            "StartColor": [0.22, 0.21, 0.2, 0.35], "EndColor": [0.4, 0.4, 0.4, 0.0], "BlendMode": "Alpha"}),
            ("Sparks", 0.2, {"Rate": 25.0, "LifetimeMin": 0.6, "LifetimeMax": 1.4, "SpeedMin": 2.0, "SpeedMax": 4.0, "ConeAngle": 25.0,
                             "GravityFactor": 0.25, "Drag": 0.4, "StartSize": 0.05, "EndSize": 0.02,
                             "StartColor": [1.0, 0.8, 0.4, 1.0], "EndColor": [1.0, 0.3, 0.05, 0.0], "Intensity": 6.0}))
        for name, height, settings in effects:
            # Particle systems emit along -Z: turned to emit up.
            self.entity(name, {"Transform": transform((0.0, height, 0.0), (90.0, 0.0, 0.0)), "ParticleSystem": settings}, campfire)
        self.entity("Fire Light", {"Transform": transform((0.0, 0.6, 0.0)),
                                   "PointLight": {"Color": [1.0, 0.5, 0.18], "Intensity": 14.0, "Range": 9.0, "CastShadows": True},
                                   "Script": {"ClassName": "Showcase.LanternFlicker", "Fields": {
                                       "Amount": {"Type": "Float", "Value": 0.35}, "Speed": {"Type": "Float", "Value": 9.0}}}}, campfire)
        self.call("component_add", {"entity": campfire, "component": "AudioSource", "fields": {
            "Clip": "asset://Audio/Crackle.wav", "Volume": 0.9, "Loop": True, "PlayOnStart": True, "Spatial": True, "MinDistance": 1.5,
            "MaxDistance": 22.0}})
        self.campfire = campfire
        self.static_model("Fallen Trunk", LOG, LAYOUT.place((-4.7, -self.models[LOG][1][0][1], 6.2), (0.0, 8.0, 0.0)), plaza)

        # A picnic table with a lantern.
        (table_low, table_high) = self.models[TABLE][1]
        self.static_model("Picnic Table", TABLE, LAYOUT.place((-8.4, -table_low[1], 10.0), (0.0, -20.0, 0.0)), plaza)
        (lantern_low, lantern_high) = self.models[LANTERN][1]
        # The lantern's frame and glass surround its light, which casts the table's shadows: the lantern itself casts none.
        lantern = self.model_entity("Lantern", LANTERN, LAYOUT.place((-8.2, table_high[1] - table_low[1] - lantern_low[1], 9.9)), {
            "Mesh": {"Mesh": self.models[LANTERN][0], "CastShadows": False}}, parent=plaza)
        self.lantern_light = self.entity("Lantern Light", {
            "Transform": transform((0.0, (lantern_high[1] - lantern_low[1]) * 0.45, 0.0)),
            "PointLight": {"Color": [1.0, 0.6, 0.28], "Intensity": 6.0, "Range": 7.0, "CastShadows": True},
            "Script": {"ClassName": "Showcase.LanternFlicker"}}, lantern)

        # Fireflies drift over the clearing; magic sparks come later around the crystals.
        self.fireflies = self.entity("Fireflies", {"Transform": LAYOUT.place((0.0, 1.4, -2.0), (90.0, 0.0, 0.0)), "ParticleSystem": {
            "Rate": 14.0, "Shape": "Box", "BoxHalfExtents": [15.0, 14.0, 1.0], "ConeAngle": 80.0, "LifetimeMin": 5.0, "LifetimeMax": 9.0,
            "SpeedMin": 0.05, "SpeedMax": 0.3, "Drag": 0.1, "GravityFactor": -0.01, "StartSize": 0.07, "EndSize": 0.04,
            "StartColor": [0.85, 1.0, 0.35, 1.0], "EndColor": [1.0, 0.75, 0.2, 0.0], "Intensity": 7.0, "MaxParticles": 300}})
        # Autumn leaves drift down over the clearing in three tints, turned to emit downwards; they live longer than their fall
        # takes, so they end below the ground rather than vanish in the air.
        self.leaves = self.entity("Falling Leaves", {"Transform": LAYOUT.place((0.0, 9.0, -2.0))})
        for name, start, end in (("Orange", (1.0, 0.5, 0.14), (0.8, 0.32, 0.08)), ("Red", (0.85, 0.2, 0.08), (0.6, 0.14, 0.06)),
                                 ("Gold", (1.0, 0.78, 0.25), (0.85, 0.55, 0.15))):
            self.entity(f"{name} Leaves", {"Transform": transform((0.0, 0.0, 0.0), (-90.0, 0.0, 0.0)), "ParticleSystem": {
                "Rate": 4.0, "Shape": "Box", "BoxHalfExtents": [17.0, 17.0, 0.5], "ConeAngle": 70.0, "LifetimeMin": 13.0, "LifetimeMax": 16.0,
                "SpeedMin": 0.1, "SpeedMax": 0.6, "GravityFactor": 0.08, "Drag": 0.9, "StartSize": 0.14, "EndSize": 0.12,
                "StartColor": list(start) + [1.0], "EndColor": list(end) + [1.0], "Texture": "asset://Textures/Leaf.png", "BlendMode": "Alpha",
                "MaxParticles": 120}}, self.leaves)
        self.signpost((-10.2, 7.4), "PARTICLES & SOUND", "fire, water, sparks and fireflies,\nheard where they are", HOME[1][0::2])

    # --- The gallery: physically based materials --------------------------------------------------------------------------

    def make_gallery(self):
        frame = GALLERY
        gallery = self.entity("Gallery")
        self.entity("Plinth", {"Transform": frame.place((0.0, 0.125, 0.9), scale=(7.6, 0.25, 4.0)), **self.mesh("Cube", self.textures[MARBLE]),
                               "RigidBody": {"Type": "Static"}, "BoxCollider": {}}, gallery)
        self.entity("Backdrop", {"Transform": frame.place((0.0, 3.2, -0.75), scale=(6.6, 5.9, 0.3)), **self.mesh("Cube", self.slate),
                                 "RigidBody": {"Type": "Static"}, "BoxCollider": {}}, gallery)
        spheres = self.entity("Spheres", None, gallery)
        # The grid's middle row: high enough that the heroes in front leave the bottom row and its labels in view.
        spacing, radius, middle = 0.85, 0.33, 3.3
        for column in range(5):
            for row in range(5):
                self.entity(f"Sphere M{column * 25} R{row * 25}", {
                    "Transform": frame.place(((column - 2) * spacing, middle + (2 - row) * spacing, -0.15), scale=(2.0 * radius,) * 3),
                    **self.mesh("Sphere", self.sweep[(column, row)]),
                    "RigidBody": {"Type": "Static"}, "SphereCollider": {}}, spheres)
        self.gallery_spheres = spheres
        labels = self.entity("Labels", None, gallery)
        text_color = (0.92, 0.9, 0.86, 1.0)
        bottom = middle - 2 * spacing - radius
        for column in range(5):
            self.entity(f"Metallic {column}", {"Transform": frame.place(((column - 2) * spacing, bottom - 0.1, -0.58)),
                                               "Text": world_text(f"{column / 4.0:g}", 0.17, text_color)}, labels)
        self.entity("Metallic", {"Transform": frame.place((0.0, bottom - 0.38, -0.58)), "Text": world_text("METALLIC", 0.15, text_color)},
                    labels)
        for row in range(5):
            self.entity(f"Roughness {row}", {"Transform": frame.place((-2 * spacing - radius - 0.3, middle + (2 - row) * spacing + 0.085, -0.58)),
                                             "Text": world_text(f"{row / 4.0:g}", 0.17, text_color, "Right")}, labels)
        self.entity("Roughness", {"Transform": frame.place((-3.1, middle, -0.58), (0.0, 0.0, 90.0)),
                                  "Text": world_text("ROUGHNESS", 0.15, text_color)}, labels)

        # The neon sign over the wall, with its glow on the wall and the spheres.
        sign = self.entity("Neon Sign", {"Transform": frame.place((0.0, 6.85, -0.66)), "AudioSource": {
            "Clip": "asset://Audio/NeonIgnite.wav", "Volume": 0.8, "Spatial": True, "MinDistance": 3.0, "MaxDistance": 40.0}}, gallery)
        self.entity("Panel", {"Transform": transform((0.0, 0.0, -0.08), scale=(5.6, 1.6, 0.12)), **self.mesh("Cube", self.slate)}, sign)
        letters = self.entity("Letters", {"Transform": transform((0.0, 0.0, 0.05), scale=(0.85, 0.85, 0.85)),
                                          **self.mesh("asset://Meshes/NeonStrada.gltf", self.neon["Pink"])}, sign)
        tubes = self.entity("Frame", {"Transform": transform((0.0, 0.0, 0.05), scale=(0.85, 0.85, 0.85)),
                                      **self.mesh("asset://Meshes/NeonFrame.gltf", self.neon["Cyan"])}, sign)
        glow = self.entity("Glow", {"Transform": transform((0.0, -0.3, 1.2)),
                                    "PointLight": {"Color": [1.0, 0.3, 0.6], "Intensity": 6.0, "Range": 7.0}}, sign)
        hum = self.entity("Hum", {"AudioSource": {"Clip": "asset://Audio/NeonHum.wav", "Volume": 0.22, "Loop": True, "PlayOnStart": True,
                                                  "Spatial": True, "MinDistance": 1.5, "MaxDistance": 10.0}}, sign)
        self.call("component_add", {"entity": sign, "component": "Script", "fields": {"ClassName": "Showcase.NeonSign", "Fields": {
            "Letters": {"Type": "Entity", "Value": letters}, "Frame": {"Type": "Entity", "Value": tubes},
            "Glow": {"Type": "Entity", "Value": glow}, "Hum": {"Type": "Entity", "Value": hum}}}})
        self.neon_sign = sign

        # Hero objects on marble pedestals: Poly Haven texture sets with normal and occlusion-roughness-metallic maps, and a
        # scanned marble bust.
        heroes = self.entity("Heroes", None, gallery)
        for index, (name, x, z, base, height) in enumerate((("Rusted Sphere", -2.95, 2.2, 0.25, 1.0), ("Bust", 0.0, 4.0, 0.0, 1.0),
                                                            ("Painted Sphere", 2.95, 2.2, 0.25, 1.0))):
            top = base + height
            self.entity(f"Pedestal {index + 1}", {"Transform": frame.place((x, base + height / 2.0, z), scale=(0.9, height, 0.9)),
                                                  **self.mesh("Cylinder", self.textures[MARBLE]),
                                                  "RigidBody": {"Type": "Static"},
                                                  "MeshCollider": {"Mesh": "builtin://Cylinder", "Convex": True}}, heroes)
            if name == "Bust":
                (bust_low, _) = self.models[BUST][1]
                # Turned a little towards the plaza.
                self.model_entity(name, BUST, frame.place((x, top - bust_low[1] * 2.2, z), (0.0, 12.0, 0.0), (2.2, 2.2, 2.2)), parent=heroes)
            else:
                texture = self.textures[RUST if name == "Rusted Sphere" else PAINTED_METAL]
                self.entity(name, {"Transform": frame.place((x, top + 0.57, z), scale=(1.1, 1.1, 1.1)), **self.mesh("Sphere", texture),
                                   "RigidBody": {"Type": "Static"}, "SphereCollider": {}}, heroes)
        # The rusted sphere's maps as sprites on a board: what the material is made of.
        board = self.entity("Map Board", {"Transform": frame.place((-4.7, 0.0, 3.6), (0.0, 35.0, 0.0))}, gallery)
        self.entity("Board", {"Transform": transform((0.0, 1.25, -0.03), scale=(2.3, 1.0, 0.05)), **self.mesh("Cube", self.charcoal)}, board)
        for leg in (-0.95, 0.95):
            self.entity("Leg", {"Transform": transform((leg, 0.4, -0.06), scale=(0.06, 0.8, 0.06)), **self.mesh("Cube", self.iron)}, board)
        folder = f"asset://Textures/{RUST}/{RUST}"
        for index, (suffix, label) in enumerate((("diff", "BASE COLOR"), ("nor_gl", "NORMAL"), ("arm", "AO · ROUGH · METAL"))):
            x = (index - 1) * 0.72
            self.entity(f"Map {label.title()}", {"Transform": transform((x, 1.34, 0.0), scale=(0.6, 0.6, 1.0)),
                                                 "SpriteRenderer": {"Texture": f"{folder}_{suffix}_1k.jpg"}}, board)
            self.entity(f"Label {index + 1}", {"Transform": transform((x, 0.98, 0.0)), "Text": world_text(label, 0.065, text_color)}, board)
        self.signpost((-7.3, -1.9), "MATERIALS", "physically based: metallic, roughness,\nnormal, occlusion and emissive maps")

    # --- The glass garden: transparency, cutouts and cloth ------------------------------------------------------------------

    def make_glass_garden(self):
        frame = GARDEN
        garden = self.entity("Glass Garden")
        self.entity("Deck", {"Transform": frame.place((0.0, 0.1, 0.4), scale=(7.4, 0.2, 5.4)), **self.mesh("Cube", self.textures[PLANKS]),
                             "RigidBody": {"Type": "Static"}, "BoxCollider": {}}, garden)
        self.entity("Sculpture Pedestal", {"Transform": frame.place((0.0, 0.6, 0.6), scale=(1.3, 0.8, 1.3)), **self.mesh("Cylinder", self.charcoal),
                                           "RigidBody": {"Type": "Static"}, "MeshCollider": {"Mesh": "builtin://Cylinder", "Convex": True}}, garden)
        # A glass torus knot turning slowly on its pedestal (a kinematic body, so balls bounce off it).
        self.sculpture = self.entity("Torus Knot", {"Transform": frame.place((0.0, 2.25, 0.6), scale=(1.15, 1.15, 1.15)),
                                                    **self.mesh("asset://Meshes/TorusKnot.gltf", self.glass["Violet"]),
                                                    "RigidBody": {"Type": "Kinematic"},
                                                    "MeshCollider": {"Mesh": "asset://Meshes/TorusKnot.gltf", "Convex": False},
                                                    "Script": {"ClassName": "Showcase.Spinner", "Fields": {
                                                        "Speed": {"Type": "Float", "Value": 14.0}}}}, garden)
        # Tinted panes in iron frames behind it, staggered so they overlap.
        panes = self.entity("Panes", None, garden)
        for index, (tint, x, yaw) in enumerate((("Clear", -2.55, 14.0), ("Amber", -0.85, -10.0), ("Teal", 0.85, 10.0), ("Rose", 2.55, -14.0))):
            pane = self.entity(f"{tint} Pane", {"Transform": frame.place((x, 1.45, -1.25 - 0.25 * (index % 2)), (0.0, yaw, 0.0)),
                                                "RigidBody": {"Type": "Static"},
                                                "BoxCollider": {"HalfExtents": [0.7, 1.2, 0.03]}}, panes)
            self.entity("Glass", {"Transform": transform((0.0, 0.0, 0.0), scale=(1.36, 2.36, 0.03)), **self.mesh("Cube", self.glass[tint])}, pane)
            for name, position, scale in (("Top", (0.0, 1.2, 0.0), (1.48, 0.06, 0.07)), ("Bottom", (0.0, -1.2, 0.0), (1.48, 0.06, 0.07)),
                                          ("Left", (-0.71, 0.0, 0.0), (0.06, 2.46, 0.07)), ("Right", (0.71, 0.0, 0.0), (0.06, 2.46, 0.07))):
                self.entity(name, {"Transform": transform(position, scale=scale), **self.mesh("Cube", self.iron)}, pane)
        # Crystals of ice-blue glass with magic sparks and a light inside.
        crystals = self.entity("Crystals", {"Transform": frame.place((2.35, 0.2, 1.9))}, garden)
        generator = random.Random(3)
        for index in range(7):
            angle = 2.0 * math.pi * index / 7.0
            lean = 12.0 + 18.0 * (index % 3) / 2.0
            size = 0.55 + 0.25 * ((index * 5) % 3) / 2.0 if index else 1.0
            position = (0.0, 0.0, 0.0) if index == 0 else (0.18 * math.cos(angle), 0.0, 0.18 * math.sin(angle))
            self.entity(f"Crystal {index + 1}", {"Transform": transform(position, (0.0 if index == 0 else lean, math.degrees(-angle) + 90.0,
                                                                                    generator.uniform(-8.0, 8.0)), (size, size, size)),
                                                 **self.mesh("asset://Meshes/Crystal.gltf", self.glass["Ice"])}, crystals)
        self.entity("Crystal Light", {"Transform": transform((0.0, 0.45, 0.0)),
                                      "PointLight": {"Color": [0.4, 0.75, 1.0], "Intensity": 4.0, "Range": 4.0}}, crystals)
        self.sparks = self.entity("Magic Sparks", {"Transform": transform((0.0, 0.5, 0.0)), "ParticleSystem": {
            "Rate": 26.0, "Shape": "Sphere", "Radius": 0.7, "LifetimeMin": 1.2, "LifetimeMax": 2.4, "SpeedMin": 0.1, "SpeedMax": 0.4,
            "GravityFactor": -0.06, "Drag": 0.5, "StartSize": 0.06, "EndSize": 0.01, "StartColor": [0.55, 0.85, 1.0, 1.0],
            "EndColor": [0.85, 0.35, 1.0, 0.0], "Intensity": 6.0}}, crystals)
        # A trellis of alpha-tested lattice behind the panes.
        fence = self.entity("Trellis", None, garden)
        for index in range(3):
            x = (index - 1) * 2.3
            self.entity(f"Lattice {index + 1}", {"Transform": frame.place((x, 1.25, -2.45), scale=(2.2, 2.1, 1.0)),
                                                 **self.mesh("Quad", self.lattice), "RigidBody": {"Type": "Static"},
                                                 "BoxCollider": {"HalfExtents": [0.5, 0.5, 0.02]}}, fence)
        for index in range(4):
            x = (index - 1.5) * 2.3
            self.entity(f"Trellis Post {index + 1}", {"Transform": frame.place((x, 1.3, -2.45), scale=(0.1, 2.6, 0.1)),
                                                      **self.mesh("Cube", self.textures[PLANKS])}, fence)
        # Two-sided banners swaying on poles at the deck's front corners.
        for index, (x, banner) in enumerate(((-3.35, self.banners[0]), (3.35, self.banners[1]))):
            pole = self.entity(f"Banner Pole {index + 1}", {"Transform": frame.place((x, 0.0, 2.75))}, garden)
            self.entity("Pole", {"Transform": transform((0.0, 2.1, 0.0), scale=(0.08, 4.2, 0.08)), **self.mesh("Cylinder", self.iron)}, pole)
            self.entity("Arm", {"Transform": transform((0.0, 4.0, 0.0), scale=(1.2, 0.05, 0.05)), **self.mesh("Cube", self.iron)}, pole)
            self.entity("Finial", {"Transform": transform((0.0, 4.28, 0.0), scale=(0.16, 0.16, 0.16)), **self.mesh("Sphere", self.brass)}, pole)
            hinge = self.entity("Banner", {"Transform": transform((0.0, 3.98, 0.0)), "Script": {"ClassName": "Showcase.Sway", "Fields": {
                "Amplitude": {"Type": "Float", "Value": 7.0}, "Frequency": {"Type": "Float", "Value": 0.28 + 0.06 * index}}}}, pole)
            self.entity("Cloth", {"Transform": transform((0.0, -1.0, 0.0), scale=(1.0, 2.0, 1.0)), **self.mesh("Quad", banner)}, hinge)
        self.signpost((9.6, 7.6), "TRANSPARENCY", "blended glass, alpha-tested lattice\nand two-sided cloth")

    # --- Lights: a lighthouse and a light show ------------------------------------------------------------------------------

    def make_lights(self):
        lights = self.entity("Lights")
        # The lighthouse: banded tower, lamp room, two beams turning.
        lighthouse = self.entity("Lighthouse", {"Transform": LAYOUT.place((-5.5, 0.0, -21.0))}, lights)
        self.entity("Foundation", {"Transform": transform((0.0, 0.3, 0.0), scale=(3.2, 0.6, 3.2)), **self.mesh("Cylinder", self.textures[MARBLE]),
                                   "RigidBody": {"Type": "Static"}, "MeshCollider": {"Mesh": "builtin://Cylinder", "Convex": True}}, lighthouse)
        for band in range(5):
            radius = 1.15 - 0.07 * band
            self.entity(f"Band {band + 1}", {"Transform": transform((0.0, 0.6 + 1.4 * band + 0.7, 0.0), scale=(2.0 * radius, 1.4, 2.0 * radius)),
                                             **self.mesh("Cylinder", self.red_paint if band % 2 else self.white_paint)}, lighthouse)
        self.entity("Tower Collider", {"Transform": transform((0.0, 4.1, 0.0)), "RigidBody": {"Type": "Static"},
                                       "CapsuleCollider": {"Radius": 1.0, "HalfHeight": 2.6}}, lighthouse)
        self.entity("Gallery Deck", {"Transform": transform((0.0, 7.68, 0.0), scale=(2.4, 0.16, 2.4)), **self.mesh("Cylinder", self.iron)}, lighthouse)
        # The lamp, the lamp room and the railing around it would block the beams' light in their shadow maps.
        self.entity("Railing", {"Transform": transform((0.0, 8.15, 0.0), scale=(1.15, 1.0, 1.15)),
                                **self.mesh("asset://Meshes/Hoop.gltf", self.iron, shadows=False)}, lighthouse)
        self.entity("Lamp Room", {"Transform": transform((0.0, 8.35, 0.0), scale=(1.3, 1.2, 1.3)),
                                  **self.mesh("Cylinder", self.glass["Lamp"], shadows=False)}, lighthouse)
        self.entity("Roof", {"Transform": transform((0.0, 9.4, 0.0), scale=(1.8, 0.9, 1.8)), **self.mesh("Cone", self.red_paint)}, lighthouse)
        self.entity("Finial", {"Transform": transform((0.0, 9.95, 0.0), scale=(0.22, 0.22, 0.22)), **self.mesh("Sphere", self.brass)}, lighthouse)
        self.entity("Lamp", {"Transform": transform((0.0, 8.3, 0.0), scale=(0.42, 0.42, 0.42)), **self.mesh("Sphere", self.neon["Lamp"], shadows=False),
                             "PointLight": {"Color": [1.0, 0.8, 0.5], "Intensity": 8.0, "Range": 10.0}}, lighthouse)
        beams = self.entity("Beams", {"Transform": transform((0.0, 8.3, 0.0)),
                                      "Script": {"ClassName": "Showcase.Spinner", "Fields": {"Speed": {"Type": "Float", "Value": 36.0}}}},
                            lighthouse)
        for index, yaw in enumerate((0.0, 180.0)):
            # The beams sweep the whole clearing: shadow maps of that reach would cost more than their shadows show.
            beam = self.entity(f"Beam {index + 1}", {"Transform": transform((0.0, 0.0, 0.0), (-5.0, yaw, 0.0)),
                                                     "SpotLight": {"Color": [1.0, 0.85, 0.6], "Intensity": 90.0, "Range": 55.0,
                                                                   "InnerConeAngle": 5.0, "OuterConeAngle": 9.0, "CastShadows": False}}, beams)
            self.light_shaft(beam, 26.0, 2.6, self.beam)
        self.lighthouse = beams

        # The light show: a glossy stage with white columns, a mirror ball and four colored spots on a truss.
        stage = self.entity("Stage", {"Transform": LAYOUT.place((-9.6, 0.0, -7.2), (0.0, 30.0, 0.0))}, lights)
        self.entity("Floor", {"Transform": transform((0.0, 0.18, 0.0), scale=(5.4, 0.36, 5.4)), **self.mesh("Cylinder", self.stage_floor),
                              "RigidBody": {"Type": "Static"}, "MeshCollider": {"Mesh": "builtin://Cylinder", "Convex": True}}, stage)
        for index, (x, z, height) in enumerate(((-0.8, -0.4, 1.6), (0.5, -0.9, 2.3), (0.9, 0.6, 1.1))):
            self.entity(f"Column {index + 1}", {"Transform": transform((x, 0.36 + height / 2.0, z), scale=(0.42, height, 0.42)),
                                                **self.mesh("Cylinder", self.plaster), "RigidBody": {"Type": "Static"},
                                                "MeshCollider": {"Mesh": "builtin://Cylinder", "Convex": True}}, stage)
        self.entity("Statue Sphere", {"Transform": transform((0.9, 0.36 + 1.1 + 0.3, 0.6), scale=(0.6, 0.6, 0.6)), **self.mesh("Sphere", self.plaster)},
                    stage)
        corners = [(-2.1, -2.1), (2.1, -2.1), (2.1, 2.1), (-2.1, 2.1)]
        for index, (x, z) in enumerate(corners):
            self.entity(f"Truss Post {index + 1}", {"Transform": transform((x, 2.3, z), scale=(0.1, 4.6, 0.1)), **self.mesh("Cylinder", self.iron)},
                        stage)
            nx, nz = corners[(index + 1) % 4]
            length = math.hypot(nx - x, nz - z)
            self.entity(f"Truss Beam {index + 1}", {"Transform": transform(((x + nx) / 2.0, 4.6, (z + nz) / 2.0),
                                                                            (0.0, math.degrees(math.atan2(nx - x, nz - z)), 0.0),
                                                                            (0.1, 0.1, length)), **self.mesh("Cube", self.iron)}, stage)
        self.entity("Ball Rod", {"Transform": transform((0.0, 4.3, 0.0), scale=(0.03, 0.6, 0.03)), **self.mesh("Cylinder", self.iron)}, stage)
        self.entity("Mirror Ball", {"Transform": transform((0.0, 3.75, 0.0), scale=(0.9, 0.9, 0.9)),
                                    **self.mesh("asset://Meshes/MirrorBall.gltf", self.chrome),
                                    "Script": {"ClassName": "Showcase.Spinner", "Fields": {"Speed": {"Type": "Float", "Value": 24.0}}}}, stage)
        rig = self.entity("Light Rig", {"Script": {"ClassName": "Showcase.LightShow"}}, stage)
        for index, (x, z) in enumerate(corners):
            # Aimed at the stage's center, a little above the floor.
            dx, dy, dz = -x * 0.8, 0.9 - 4.45, -z * 0.8
            yaw = math.degrees(math.atan2(-dx, -dz))
            pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
            spot = self.entity(f"Spot {index + 1}", {"Transform": transform((x * 0.92, 4.45, z * 0.92), (pitch, yaw, 0.0)),
                                                     "SpotLight": {"Color": [1.0, 1.0, 1.0], "Intensity": 160.0, "Range": 9.0,
                                                                   "InnerConeAngle": 14.0, "OuterConeAngle": 22.0, "CastShadows": True}}, rig)
            self.entity("Housing", {"Transform": transform((0.0, 0.0, 0.12), (90.0, 0.0, 0.0), (0.26, 0.36, 0.26)),
                                    **self.mesh("Cylinder", self.iron, shadows=False)}, spot)
            self.entity("Lens", {"Transform": transform((0.0, 0.0, -0.07), (90.0, 0.0, 0.0), (0.2, 0.02, 0.2)),
                                 **self.mesh("Cylinder", self.neon["Lens"], shadows=False)}, spot)
            self.light_shaft(spot, 4.4, 1.7, self.spot_beam, start=-0.08)
        self.light_show = rig
        # Evening under the truss: a local post-process volume around the stage darkens the exposure, so the spots' colored
        # pools and the beams outshine the sun, cools and deepens the colors and strengthens the glow and the lens; it fades out
        # over 5 m around its box as the camera leaves.
        self.evening_zone = self.entity("Evening Zone", {"Transform": transform((0.0, 3.0, 0.0), scale=(9.0, 6.0, 9.0)), "PostProcess": {
            "Global": False, "BlendDistance": 5.0, "Priority": 1.0,
            "OverrideEV100": True, "EV100": 1.0, "OverrideBloomIntensity": True, "BloomIntensity": 0.34,
            "OverrideContrast": True, "Contrast": 1.24, "OverrideSaturation": True, "Saturation": 1.35,
            "OverrideTemperature": True, "Temperature": -0.55, "OverrideTint": True, "Tint": 0.24,
            "OverrideVignette": True, "Vignette": 0.6, "OverrideChromaticAberration": True, "ChromaticAberration": 0.35}}, stage)
        self.signpost((-12.2, -4.6), "LIGHT & SHADOW", "sun and sky, spot and point lights,\nsoft and colored shadows")

    def light_shaft(self, lamp, length, radius, material, start=0.0, layers=8):
        """The visible shaft of a lamp's light: glowing cones along the lamp's -Z from its lens, nested so that the inner,
        narrower ones add up at the core and the shaft fades towards its edge (and, through the material's texture, along its
        length)."""
        for layer in range(layers):
            width = radius * (1.0 - layer / layers)
            self.entity(f"Shaft {layer + 1}", {"Transform": transform((0.0, 0.0, start), scale=(width, width, length)),
                                               "Mesh": {"Mesh": "asset://Meshes/Beam.gltf", "Materials": [material], "CastShadows": False}},
                        lamp)

    # --- Physics: rigid bodies, joints and impacts ----------------------------------------------------------------------------

    def make_physics(self):
        physics = self.entity("Physics")
        props = self.entity("Props", None, physics)
        (low, high) = self.models[CRATE][1]
        width, height, depth = (high[0] - low[0], high[1] - low[1], high[2] - low[2])
        center = [(low[axis] + high[axis]) / 2.0 for axis in range(3)]
        camera = {"Type": "Entity", "Value": self.camera}

        def crate(name, position, yaw=0.0):
            # The mesh's center, turned with the crate, lands on the position.
            offset = yaw_point((center[0], 0.0, center[2]), yaw)
            self.model_entity(name, CRATE, LAYOUT.place((position[0] - offset[0], position[1] - low[1], position[2] - offset[2]), (0.0, yaw, 0.0)), {
                "RigidBody": {"Type": "Dynamic", "Mass": 9.0, "AngularDamping": 0.1},
                "BoxCollider": {"HalfExtents": [width / 2.0 - 0.002, height / 2.0, depth / 2.0 - 0.002], "Offset": center,
                                "Friction": 0.65, "Restitution": 0.05},
                "AudioSource": {"Clip": "asset://Audio/WoodHit.wav", "Spatial": True, "MinDistance": 2.0, "MaxDistance": 40.0},
                "Script": {"ClassName": "Showcase.Impact", "Fields": {"Volume": {"Type": "Float", "Value": 0.9}, "Camera": camera,
                                                                      "ShakeStrength": {"Type": "Float", "Value": 0.12}}}}, props)

        # The pyramid faces the camera; the tower stands in the wrecking ball's way.
        gap = 0.015
        for row, count in enumerate((4, 3, 2, 1)):
            for column in range(count):
                x = (column - (count - 1) / 2.0) * (width + gap)
                crate(f"Crate {row + 1}-{column + 1}", (3.2 + x, row * height, -8.8))
        wreck_x, wreck_z = -2.6, -9.6
        for level in range(6):
            crate(f"Tower Crate {level + 1}", (wreck_x + 2.4, level * height, wreck_z), 90.0 * (level % 2))
        (barrel_low, barrel_high) = self.models[BARREL][1]
        barrel_height = barrel_high[1] - barrel_low[1]
        barrel_width = max(barrel_high[0] - barrel_low[0], barrel_high[2] - barrel_low[2]) + 0.03
        for index, (x, level) in enumerate(((-barrel_width, 0), (0.0, 0), (barrel_width, 0), (-barrel_width / 2.0, 1), (barrel_width / 2.0, 1))):
            self.model_entity(f"Barrel {index + 1}", BARREL, LAYOUT.place((5.8 + x, level * barrel_height - barrel_low[1] + 0.002, -10.6),
                                                                          (0.0, 37.0 * index, 0.0)), {
                "RigidBody": {"Type": "Dynamic", "Mass": 14.0, "AngularDamping": 0.15},
                "MeshCollider": {"Mesh": self.models[BARREL][0], "Convex": True, "Friction": 0.6, "Restitution": 0.05},
                "AudioSource": {"Clip": "asset://Audio/WoodHit.wav", "Pitch": 0.8, "Spatial": True, "MinDistance": 2.0, "MaxDistance": 40.0},
                "Script": {"ClassName": "Showcase.Impact", "Fields": {"Volume": {"Type": "Float", "Value": 1.0}, "Camera": camera,
                                                                      "ShakeStrength": {"Type": "Float", "Value": 0.15}}}}, props)
        self.props = props

        def shape(name, mesh, position, scale, material, components=None, euler=(0.0, 0.0, 0.0), parent=physics):
            return self.entity(name, {"Transform": LAYOUT.place(position, euler, scale), **self.mesh(mesh, material), **(components or {})}, parent)

        # The wrecking ball hangs from a beam on a ball joint (a rigid rod down to it); its key pushes it into the tower.
        pivot = (wreck_x, 5.2, wreck_z)
        rest = (wreck_x, 1.2, wreck_z)
        ball_size = 0.9
        for name, position, scale in (("Post", (wreck_x, 2.7, wreck_z - 1.4), (0.25, 5.4, 0.25)),
                                      ("Post 2", (wreck_x, 2.7, wreck_z + 1.4), (0.25, 5.4, 0.25)),
                                      ("Beam", (wreck_x, 5.35, wreck_z), (0.3, 0.3, 3.2))):
            shape(f"Wrecking {name}", "Cube", position, scale, self.iron, {"RigidBody": {"Type": "Static"}, "BoxCollider": {}})
        length = pivot[1] - rest[1]
        push = (7.0, 0.0, 0.0)
        self.wrecking_ball = shape("Wrecking Ball", "Sphere", rest, (ball_size, ball_size, ball_size), self.ball_metal, {
            "RigidBody": {"Type": "Dynamic", "Mass": 120.0, "LinearDamping": 0.02, "AngularDamping": 0.3},
            "SphereCollider": {"Radius": 0.5, "Friction": 0.4, "Restitution": 0.2},
            "Joint": {"Type": "Ball", "Anchor": [0.0, length / ball_size, 0.0]},
            "AudioSource": {"Clip": "asset://Audio/MetalHit.wav", "Pitch": 0.6, "Spatial": True, "MinDistance": 3.0, "MaxDistance": 50.0},
            "Script": {"ClassName": "Showcase.WreckingBall", "Fields": {
                # Towards the tower (+X of the layout, turned with it).
                "Push": {"Type": "Vector3", "Value": list(push)}, "Camera": camera, "ShakeStrength": {"Type": "Float", "Value": 0.5},
                "MinimumSpeed": {"Type": "Float", "Value": 1.5}}}})
        # The rod is a child of the ball, from its center up to the beam: it swings with it.
        self.entity("Rod", {"Transform": transform((0.0, length / ball_size / 2.0, 0.0),
                                                   scale=(0.07 / ball_size, length / ball_size, 0.07 / ball_size)),
                            **self.mesh("Cylinder", self.iron)}, self.wrecking_ball)

        # A seesaw on a limited hinge and a paddle on a motorized hinge.
        shape("Seesaw Fulcrum", "Cube", (7.4, 0.18, -13.4), (0.45, 0.45, 0.8), self.iron, {"RigidBody": {"Type": "Static"}, "BoxCollider": {}},
              (0.0, 0.0, 45.0))
        shape("Seesaw", "Cube", (7.4, 0.56, -13.4), (4.0, 0.12, 0.7), self.red_paint, {
            "RigidBody": {"Type": "Dynamic", "Mass": 25.0, "AngularDamping": 0.2}, "BoxCollider": {"Friction": 0.7},
            "Joint": {"Type": "Hinge", "Axis": [0.0, 0.0, 1.0], "LimitsEnabled": True, "MinLimit": -16.0, "MaxLimit": 16.0}})
        shape("Paddle Post", "Cube", (-6.0, 1.3, -12.6), (0.25, 2.6, 0.25), self.iron, {"RigidBody": {"Type": "Static"}, "BoxCollider": {}})
        self.paddle = shape("Paddle", "Cube", (-6.0, 2.6, -12.15), (3.4, 0.22, 0.5), self.red_paint, {
            "RigidBody": {"Type": "Dynamic", "Mass": 30.0}, "BoxCollider": {"Friction": 0.5, "Restitution": 0.3},
            "Joint": {"Type": "Hinge", "Axis": [0.0, 0.0, 1.0], "MotorEnabled": True, "MotorSpeed": 40.0, "MotorMaxForce": 5000.0}})

        self.make_dominoes(physics, camera)
        self.make_bridge(physics)
        # In front of the wrecking ball's posts as the tour sees them, beside its swing.
        self.signpost((-4.6, -7.0), "PHYSICS", "rigid bodies, joints and impacts\n1 wrecking ball   2 dominoes")

    def make_dominoes(self, physics, camera):
        """A run of rainbow dominoes along an S-curve: prefab instances, each with its own color."""
        domino = self.entity("Domino", {
            "Transform": transform((0.0, -100.0, 0.0), scale=(0.11, 0.9, 0.45)),
            "Mesh": {"Mesh": "builtin://Cube", "Materials": [self.domino_colors[0]]},
            "RigidBody": {"Type": "Dynamic", "Mass": 1.2, "AngularDamping": 0.05},
            "BoxCollider": {"Friction": 0.45},
            "AudioSource": {"Clip": "asset://Audio/WoodHit.wav", "Pitch": 1.6, "Spatial": True, "MinDistance": 1.5, "MaxDistance": 30.0},
            "Script": {"ClassName": "Showcase.Impact", "Fields": {"Volume": {"Type": "Float", "Value": 0.35},
                                                                  "MinimumSpeed": {"Type": "Float", "Value": 0.3}}}})
        prefab = self.call("prefab_create", {"entity": domino, "path": "Prefabs/Domino.sprefab"})["reference"]
        self.call("entity_delete", {"entities": [domino]})
        run = self.entity("Domino Run", {"Script": {"ClassName": "Showcase.DominoRun"}}, physics)
        # The path, sampled finely, then walked in steps of the spacing.
        samples = []
        for index in range(400):
            t = index / 399.0
            samples.append((-3.6 + 11.0 * t, -15.8 + 1.0 * math.sin(2.0 * math.pi * t * 1.25)))
        spacing = 0.44
        points = [samples[0]]
        walked = 0.0
        for previous, current in zip(samples, samples[1:]):
            walked += math.hypot(current[0] - previous[0], current[1] - previous[1])
            if walked >= spacing:
                points.append(current)
                walked = 0.0
        for index, (x, z) in enumerate(points):
            before = points[max(0, index - 1)]
            after = points[min(len(points) - 1, index + 1)]
            # Facing along the path: the thin side (local X) points to the next domino.
            yaw = math.degrees(math.atan2(-(after[1] - before[1]), after[0] - before[0]))
            instance = self.call("prefab_instantiate", {"prefab": prefab})["id"]
            self.call("component_set", {"entity": instance, "component": "Transform",
                                        "fields": LAYOUT.place((x, 0.45, z), (0.0, yaw, 0.0), (0.11, 0.9, 0.45))})
            self.call("component_set", {"entity": instance, "component": "Mesh",
                                        "fields": {"Materials": [self.domino_colors[index % len(self.domino_colors)]]}})
            self.call("entity_rename", {"entity": instance, "name": f"Domino {index + 1}"})
            self.call("entity_reparent", {"entity": instance, "parent": run})
        self.dominoes = run
        print("dominoes", len(points))

    def make_bridge(self, physics):
        """A plank bridge between two towers: a chain of hinges that sags under its own weight. The towers hold its ends."""
        bridge_root = self.entity("Bridge", None, physics)
        span_start, span_end, z, deck = -15.4, -10.2, -13.4, 1.8
        count, plank = 14, 0.4
        chain = count * plank
        span = span_end - span_start
        # The planks hang on an arc of the chain's length between the towers' edges: find its radius.
        low_radius, high_radius = chain / (2.0 * math.pi) + 1e-6, 1000.0
        for _ in range(200):
            radius = (low_radius + high_radius) / 2.0
            if 2.0 * radius * math.sin(chain / (2.0 * radius)) < span:
                low_radius = radius
            else:
                high_radius = radius
        half_angle = chain / (2.0 * radius)
        center_x = (span_start + span_end) / 2.0
        center_y = deck + radius * math.cos(half_angle)
        planks = self.entity("Planks", None, bridge_root)
        first = previous = None
        for index in range(count):
            # The plank's edges on the arc, from the west end; the planks are a little shorter than their chords, so they
            # leave gaps for the hinges to bend.
            a0 = -half_angle + chain * index / count / radius
            a1 = -half_angle + chain * (index + 1) / count / radius
            x0, y0 = center_x + radius * math.sin(a0), center_y - radius * math.cos(a0)
            x1, y1 = center_x + radius * math.sin(a1), center_y - radius * math.cos(a1)
            tilt = math.degrees(math.atan2(y1 - y0, x1 - x0))
            chord = math.hypot(x1 - x0, y1 - y0)
            components = {
                "Transform": LAYOUT.place(((x0 + x1) / 2.0, (y0 + y1) / 2.0, z), (0.0, 0.0, tilt), (chord - 0.06, 0.06, 1.1)),
                **self.mesh("Cube", self.textures[PLANKS]),
                "RigidBody": {"Type": "Dynamic", "Mass": 4.0, "AngularDamping": 0.3, "LinearDamping": 0.1},
                "BoxCollider": {"Friction": 0.8}}
            if previous is not None:
                # Hinged to the plank before at the edge they share (the anchor is in the plank's scaled space).
                components["Joint"] = {"Type": "Hinge", "ConnectedEntity": previous, "Anchor": [-0.5 * chord / (chord - 0.06), 0.0, 0.0],
                                       "Axis": [0.0, 0.0, 1.0]}
            entity = self.entity(f"Plank {index + 1}", components, planks)
            first = first or entity
            previous = entity
        # The towers hold the chain's ends: hinges at their top edges facing the bridge.
        for name, x, edge, plank_entity in (("West Tower", span_start - 0.7, 0.5, first), ("East Tower", span_end + 0.7, -0.5, previous)):
            self.entity(name, {"Transform": LAYOUT.place((x, deck / 2.0, z), scale=(1.4, deck, 1.6)), **self.mesh("Cube", self.textures[PLANKS]),
                               "RigidBody": {"Type": "Static"}, "BoxCollider": {},
                               "Joint": {"Type": "Hinge", "ConnectedEntity": plank_entity, "Anchor": [edge, 0.5, 0.0],
                                         "Axis": [0.0, 0.0, 1.0]}}, bridge_root)
        self.bridge = planks

    # --- Triggers: a ball launcher, a goal and a gate -----------------------------------------------------------------------

    def make_triggers(self):
        triggers = self.entity("Triggers")
        camera = {"Type": "Entity", "Value": self.camera}
        launcher_position = (8.0, 0.0, -5.0)
        goal_position = (13.2, 0.0, -11.0)
        # The goal faces the launcher: a glowing rim with a net on a post, a backboard and a scoreboard.
        goal_yaw = math.degrees(math.atan2(launcher_position[0] - goal_position[0], launcher_position[2] - goal_position[2]))
        goal_frame = Frame(goal_position, goal_yaw)
        rim_height = 3.1
        goal = self.entity("Goal", {"Transform": goal_frame.place((0.0, 0.0, 0.0))}, triggers)
        self.entity("Post", {"Transform": transform((0.0, 2.0, -1.0), scale=(0.16, 4.0, 0.16)), **self.mesh("Cylinder", self.iron),
                             "RigidBody": {"Type": "Static"}, "BoxCollider": {}}, goal)
        self.entity("Arm", {"Transform": transform((0.0, rim_height + 0.2, -0.85), scale=(0.08, 0.08, 0.3)), **self.mesh("Cube", self.iron)}, goal)
        self.entity("Backboard", {"Transform": transform((0.0, rim_height + 0.5, -0.72), scale=(1.8, 1.15, 0.04)),
                                  **self.mesh("Cube", self.glass["Backboard"]), "RigidBody": {"Type": "Static"}, "BoxCollider": {}}, goal)
        rim = self.entity("Rim", {"Transform": transform((0.0, rim_height, -0.1), scale=(0.46, 0.46, 0.46)),
                                  **self.mesh("asset://Meshes/Hoop.gltf", self.neon["Amber"]), "RigidBody": {"Type": "Static"},
                                  "MeshCollider": {"Mesh": "asset://Meshes/Hoop.gltf", "Convex": False}}, goal)
        # The net catches the balls and lets them drop straight down onto the plate.
        self.entity("Net", {"Transform": transform((0.0, rim_height - 0.02, -0.1), scale=(0.45, 0.5, 0.45)),
                            **self.mesh("asset://Meshes/Net.gltf", self.net), "RigidBody": {"Type": "Static"},
                            "MeshCollider": {"Mesh": "asset://Meshes/Net.gltf", "Convex": False, "Friction": 0.2}}, goal)
        board = self.entity("Scoreboard", {"Transform": transform((0.0, rim_height + 1.42, -0.76))}, goal)
        self.entity("Panel", {"Transform": transform((0.0, 0.0, -0.05), scale=(1.4, 0.72, 0.08)), **self.mesh("Cube", self.charcoal)}, board)
        self.entity("Label", {"Transform": transform((0.0, 0.3, 0.0)), "Text": world_text("GOALS", 0.13, (1.0, 0.8, 0.5, 1.0))}, board)
        counter = self.entity("Counter", {"Transform": transform((0.0, 0.16, 0.0)), "Text": world_text("0", 0.42, (1.0, 0.95, 0.85, 1.0))}, board)
        burst = self.make_burst_prefab()
        self.goal = self.entity("Goal Trigger", {
            "Transform": transform((0.0, rim_height - 0.3, -0.1)), "RigidBody": {"Type": "Static"},
            "BoxCollider": {"HalfExtents": [0.28, 0.24, 0.28], "IsTrigger": True},
            "AudioSource": {"Clip": "asset://Audio/Goal.wav", "Volume": 0.9, "Spatial": True, "MinDistance": 3.0, "MaxDistance": 50.0},
            "Script": {"ClassName": "Showcase.GoalZone", "Fields": {
                "Counter": {"Type": "Entity", "Value": counter}, "Hoop": {"Type": "Entity", "Value": rim},
                "Burst": {"Type": "Prefab", "Value": burst}, "Camera": camera}}}, goal)
        self.rim = rim

        # The pressure plate under the net: balls that drop through land on it and raise the gate beside it.
        plate_center = goal_frame.point((0.0, 0.0, -0.1))
        plate = self.entity("Plate", {"Transform": LAYOUT.place((plate_center[0], 0.04, plate_center[2]), (0.0, goal_yaw, 0.0), (1.6, 0.08, 1.6)),
                                      **self.mesh("Cube", self.neon["Plate"]), "RigidBody": {"Type": "Static"}, "BoxCollider": {}}, triggers)
        gate_frame = Frame(goal_frame.point((2.6, 0.0, -1.6)), goal_yaw - 35.0)
        gate = self.entity("Gate", {"Transform": gate_frame.place((0.0, 1.35, 0.0)),
                                    "RigidBody": {"Type": "Kinematic"}, "BoxCollider": {"HalfExtents": [1.25, 1.35, 0.08]},
                                    "AudioSource": {"Clip": "asset://Audio/Gate.wav", "Volume": 0.9, "Spatial": True, "MinDistance": 3.0,
                                                    "MaxDistance": 40.0}}, triggers)
        for index in range(7):
            self.entity(f"Bar {index + 1}", {"Transform": transform((-1.08 + index * 0.36, 0.0, 0.0), scale=(0.07, 2.7, 0.07)),
                                             **self.mesh("Cylinder", self.iron)}, gate)
        for index, y in enumerate((-1.0, 0.0, 1.0)):
            self.entity(f"Crossbar {index + 1}", {"Transform": transform((0.0, y, 0.0), scale=(2.4, 0.08, 0.1)), **self.mesh("Cube", self.iron)}, gate)
        for name, local, scale in (("Pillar", (-1.65, 2.8, 0.0), (0.8, 5.6, 0.8)), ("Pillar 2", (1.65, 2.8, 0.0), (0.8, 5.6, 0.8)),
                                   ("Lintel", (0.0, 5.85, 0.0), (4.1, 0.5, 0.9))):
            self.entity(name, {"Transform": gate_frame.place(local, scale=scale), **self.mesh("Cube", self.textures[MARBLE]),
                               "RigidBody": {"Type": "Static"}, "BoxCollider": {}}, triggers)
        self.gate = gate
        self.plate = self.entity("Plate Trigger", {
            "Transform": LAYOUT.place((plate_center[0], 0.42, plate_center[2]), (0.0, goal_yaw, 0.0)), "RigidBody": {"Type": "Static"},
            "BoxCollider": {"HalfExtents": [0.8, 0.36, 0.8], "IsTrigger": True},
            "Script": {"ClassName": "Showcase.PressurePlate", "Fields": {
                "Gate": {"Type": "Entity", "Value": gate}, "Plate": {"Type": "Entity", "Value": plate}}}}, triggers)

        # The ball launcher: a squat mortar that lobs balls through the hoop (its key, or the tour).
        rim_point = goal_frame.point((0.0, rim_height, -0.1))
        aim_yaw = math.degrees(math.atan2(-(rim_point[0] - launcher_position[0]), -(rim_point[2] - launcher_position[2])))
        launch_ball = self.make_launch_prefabs()
        launcher = self.entity("Ball Launcher", {
            "Transform": LAYOUT.place(launcher_position, (0.0, aim_yaw, 0.0)),
            "AudioSource": {"Clip": "asset://Audio/Cannon.wav", "Volume": 1.0, "Spatial": True, "MinDistance": 3.0, "MaxDistance": 60.0}},
            triggers)
        self.entity("Base", {"Transform": transform((0.0, 0.14, 0.0), scale=(1.2, 0.28, 1.2)), **self.mesh("Cylinder", self.iron),
                             "RigidBody": {"Type": "Static"}, "MeshCollider": {"Mesh": "builtin://Cylinder", "Convex": True}}, launcher)
        self.entity("Turntable", {"Transform": transform((0.0, 0.34, 0.0), scale=(0.9, 0.12, 0.9)), **self.mesh("Cylinder", self.brass)}, launcher)
        for side in (-0.34, 0.34):
            self.entity("Cheek", {"Transform": transform((side, 0.66, 0.05), scale=(0.08, 0.62, 0.7)), **self.mesh("Cube", self.iron)}, launcher)
        barrel = self.entity("Barrel", {"Transform": transform((0.0, 0.8, 0.0), (55.0, 0.0, 0.0))}, launcher)
        tube = self.entity("Tube", None, barrel)
        self.entity("Bore", {"Transform": transform((0.0, 0.0, -0.3), (90.0, 0.0, 0.0), (0.5, 1.1, 0.5)), **self.mesh("Cylinder", self.iron)}, tube)
        self.entity("Muzzle Band", {"Transform": transform((0.0, 0.0, -0.82), (90.0, 0.0, 0.0), (0.6, 0.1, 0.6)), **self.mesh("Cylinder", self.brass)},
                    tube)
        self.entity("Breech", {"Transform": transform((0.0, 0.0, 0.25), scale=(0.56, 0.56, 0.56)), **self.mesh("Sphere", self.iron)}, tube)
        muzzle = self.entity("Muzzle", {"Transform": transform((0.0, 0.0, -1.0))}, tube)
        self.call("component_add", {"entity": launcher, "component": "Script", "fields": {
            "ClassName": "Showcase.BallLauncher", "Fields": {
                "Ball": {"Type": "Prefab", "Value": launch_ball}, "Muzzle": {"Type": "Entity", "Value": muzzle},
                "Target": {"Type": "Entity", "Value": rim}, "Flash": {"Type": "Prefab", "Value": "asset://Prefabs/LaunchFlash.sprefab"},
                "Camera": camera, "Recoil": {"Type": "Entity", "Value": tube}}}})
        self.launcher = launcher
        self.signpost((9.8, -3.2), "TRIGGERS", "3 fires the launcher: a goal counts,\nthe plate opens the gate")

    def make_burst_prefab(self):
        burst = self.entity("Goal Burst", {"Transform": transform((0.0, -100.0, 0.0)), "ParticleSystem": {
            "Emitting": False, "Burst": 140, "Shape": "Sphere", "Radius": 0.25, "LifetimeMin": 0.8, "LifetimeMax": 1.6, "SpeedMin": 2.0,
            "SpeedMax": 5.5, "GravityFactor": 0.5, "Drag": 0.9, "StartSize": 0.1, "EndSize": 0.02, "StartColor": [1.0, 0.85, 0.3, 1.0],
            "EndColor": [1.0, 0.2, 0.65, 0.0], "Intensity": 5.0, "DestroyWhenFinished": True}})
        reference = self.call("prefab_create", {"entity": burst, "path": "Prefabs/GoalBurst.sprefab"})["reference"]
        self.call("entity_delete", {"entities": [burst]})
        return reference

    def make_launch_prefabs(self):
        flash = self.entity("Launch Flash", {"Transform": transform((0.0, -100.0, 0.0)), "ParticleSystem": {
            "Emitting": False, "Burst": 26, "Shape": "Sphere", "Radius": 0.15, "LifetimeMin": 1.2, "LifetimeMax": 2.2, "SpeedMin": 0.5,
            "SpeedMax": 1.6, "GravityFactor": -0.05, "Drag": 0.8, "StartSize": 0.3, "EndSize": 1.1, "StartColor": [0.5, 0.5, 0.5, 0.45],
            "EndColor": [0.6, 0.6, 0.6, 0.0], "BlendMode": "Alpha", "DestroyWhenFinished": True}})
        self.entity("Fire", {"Transform": transform((0.0, 0.0, 0.0)), "ParticleSystem": {
            "Emitting": False, "Burst": 34, "Shape": "Sphere", "Radius": 0.1, "LifetimeMin": 0.15, "LifetimeMax": 0.35, "SpeedMin": 2.0,
            "SpeedMax": 4.0, "StartSize": 0.28, "EndSize": 0.05, "StartColor": [1.0, 0.7, 0.3, 1.0], "EndColor": [1.0, 0.2, 0.05, 0.0],
            "Intensity": 8.0, "DestroyWhenFinished": True}}, flash)
        self.call("prefab_create", {"entity": flash, "path": "Prefabs/LaunchFlash.sprefab"})
        self.call("entity_delete", {"entities": [flash]})
        ball = self.entity("Launcher Ball", {
            "Transform": transform((0.0, -50.0, 0.0), scale=(0.26, 0.26, 0.26)),
            "Mesh": {"Mesh": "builtin://Sphere", "Materials": [self.orange_ball]},
            # No drag: the launcher's aim assumes a ballistic flight.
            "RigidBody": {"Type": "Dynamic", "Mass": 2.0, "LinearDamping": 0.0, "AngularDamping": 0.1, "ContinuousCollision": True},
            "SphereCollider": {"Radius": 0.5, "Friction": 0.6, "Restitution": 0.55},
            "AudioSource": {"Clip": "asset://Audio/WoodHit.wav", "Pitch": 1.3, "Spatial": True, "MinDistance": 2.0, "MaxDistance": 40.0},
            "Script": {"ClassName": "Showcase.Impact", "Fields": {"Volume": {"Type": "Float", "Value": 0.6},
                                                                  "Lifetime": {"Type": "Float", "Value": 20.0}}}})
        reference = self.call("prefab_create", {"entity": ball, "path": "Prefabs/LauncherBall.sprefab"})["reference"]
        self.call("entity_delete", {"entities": [ball]})
        return reference

    # --- The yard: where the walker climbs, jumps, rides and pushes ---------------------------------------------------------

    def make_yard(self):
        """The yard (south-east): stairs up to a deck, a ramp down from it, posts to jump along, a lookout a lift rises to, and
        crates to push; and the walker, the character the camera walks as."""
        frame = YARD
        yard = self.entity("Yard")
        planks, marble = self.textures[PLANKS], self.textures[MARBLE]

        def solid(name, local, scale, material, euler=(0.0, 0.0, 0.0), mesh="Cube", parent=yard):
            collider = {"BoxCollider": {}} if mesh == "Cube" else {"MeshCollider": {"Mesh": f"builtin://{mesh}", "Convex": True}}
            return self.entity(name, {"Transform": frame.place(local, euler, scale), **self.mesh(mesh, material), "RigidBody": {"Type": "Static"},
                                      **collider}, parent)

        # The deck on steel posts, and five steps of 25 cm up to it from the front.
        self.deck = solid("Deck", (0.5, DECK_TOP - 0.1, 0.0), (3.0, 0.2, 3.0), planks)
        for index, (x, z) in enumerate(((-0.85, -1.35), (1.85, -1.35), (-0.85, 1.35), (1.85, 1.35))):
            solid(f"Deck Post {index + 1}", (x, (DECK_TOP - 0.2) / 2.0, z), (0.16, DECK_TOP - 0.2, 0.16), self.iron, mesh="Cylinder")
        stairs = self.entity("Stairs", None, yard)
        for step in range(1, 6):
            top = 0.25 * step
            entity = solid(f"Step {step}", (0.2, top / 2.0, 1.5 + (5 - step) * 0.36 + 0.18), (1.6, top, 0.36), planks, parent=stairs)
            if step == 1:
                self.first_step = entity
        # The ramp down from the deck's left edge: 1.5 m over 3 m. Its top face runs from (-4, 0) to (-1, 1.5) (x, height); its
        # local X runs uphill.
        run, rise, thickness = 3.0, DECK_TOP, 0.15
        length = math.hypot(run, rise)
        normal = (-rise / length, run / length)
        center = (-2.5 - normal[0] * thickness / 2.0, rise / 2.0 - normal[1] * thickness / 2.0)
        self.ramp = solid("Ramp", (center[0], center[1], 0.0), (length, thickness, 1.6), planks, (0.0, 0.0, math.degrees(math.atan2(rise, run))))
        # Posts to jump along, up to the deck's front corner.
        for index, (x, z, top) in enumerate(((-3.4, 3.4, 0.5), (-2.6, 2.7, 0.95), (-1.8, 2.0, 1.4))):
            solid(f"Jump Post {index + 1}", (x, top / 2.0, z), (0.7, top, 0.7), marble, mesh="Cylinder")

        # The lookout on the right, 3.4 m up, railed at the back and on the right; the lift rises to its front edge.
        lookout = self.entity("Lookout", None, yard)
        solid("Floor", (3.7, LOOKOUT_TOP - 0.1, -0.5), (2.2, 0.2, 2.2), planks, parent=lookout)
        for index, (x, z) in enumerate(((2.7, -1.5), (4.7, -1.5), (2.7, 0.5), (4.7, 0.5))):
            solid(f"Post {index + 1}", (x, (LOOKOUT_TOP - 0.2) / 2.0, z), (0.18, LOOKOUT_TOP - 0.2, 0.18), self.iron, mesh="Cylinder", parent=lookout)
        for name, local, scale in (("Back Rail", (3.7, LOOKOUT_TOP + 0.95, -1.55), (2.2, 0.06, 0.06)),
                                   ("Right Rail", (4.75, LOOKOUT_TOP + 0.95, -0.5), (0.06, 0.06, 2.2)),
                                   ("Back Rail Post", (2.65, LOOKOUT_TOP + 0.5, -1.55), (0.06, 1.0, 0.06)),
                                   ("Corner Rail Post", (4.75, LOOKOUT_TOP + 0.5, -1.55), (0.06, 1.0, 0.06)),
                                   ("Right Rail Post", (4.75, LOOKOUT_TOP + 0.5, 0.55), (0.06, 1.0, 0.06))):
            solid(name, local, scale, self.iron, parent=lookout)
        # A lantern on the corner post lights the yard at night.
        (lantern_low, _) = self.models[LANTERN][1]
        lantern = self.model_entity("Lantern", LANTERN, frame.place((4.75, LOOKOUT_TOP + 1.0 - lantern_low[1], -1.55)), {
            "Mesh": {"Mesh": self.models[LANTERN][0], "CastShadows": False}}, parent=lookout)
        self.entity("Lantern Light", {"Transform": transform((0.0, 0.14, 0.0)),
                                      "PointLight": {"Color": [1.0, 0.62, 0.3], "Intensity": 10.0, "Range": 13.0},
                                      "Script": {"ClassName": "Showcase.LanternFlicker", "Fields": {"Amount": {"Type": "Float", "Value": 0.12}}}},
                    lantern)
        # The lift: a steel platform between four posts, from the ground to the lookout's floor.
        rest_top = 0.12
        self.lift = self.entity("Lift", {"Transform": frame.place((3.7, rest_top - 0.1, 1.5), scale=(1.5, 0.2, 1.5)),
                                         **self.mesh("Cube", self.textures[PAINTED_METAL]), "RigidBody": {"Type": "Kinematic"}, "BoxCollider": {},
                                         "AudioSource": {"Clip": "asset://Audio/Lift.wav", "Volume": 0.35, "Loop": True, "PlayOnStart": True,
                                                         "Spatial": True, "MinDistance": 2.0, "MaxDistance": 20.0},
                                         "Script": {"ClassName": "Showcase.Lift", "Fields": {
                                             "Rise": {"Type": "Float", "Value": LOOKOUT_TOP - rest_top}}}}, yard)
        # Guide posts at its corners, outside the platform, up past the lookout's rails.
        for index, (x, z) in enumerate(((2.86, 0.66), (4.54, 0.66), (2.86, 2.34), (4.54, 2.34))):
            solid(f"Lift Post {index + 1}", (x, 2.3, z), (0.1, 4.6, 0.1), self.iron)

        # Crates to push, a size larger than the playground's: two on the deck, a stack by the ramp's foot.
        crates = self.entity("Yard Crates", None, yard)
        crate_scale = 1.3
        (low, high) = self.models[CRATE][1]
        crate_size = [(high[axis] - low[axis]) * crate_scale for axis in range(3)]
        center = [(low[axis] + high[axis]) / 2.0 for axis in range(3)]
        for index, (x, base, z, yaw) in enumerate(((1.4, DECK_TOP, -0.9, 10.0), (1.3, DECK_TOP, 0.6, -25.0), (-3.4, 0.0, -2.3, 5.0),
                                                   (-4.6, 0.0, -2.1, -8.0), (-4.0, crate_size[1], -2.2, 3.0))):
            offset = yaw_point((center[0] * crate_scale, 0.0, center[2] * crate_scale), yaw + frame.yaw)
            position = frame.point((x, base - low[1] * crate_scale, z))
            self.model_entity(f"Yard Crate {index + 1}", CRATE, {"Translation": [position[0] - offset[0], position[1], position[2] - offset[2]],
                                                                  "Rotation": frame.rotation((0.0, yaw, 0.0)), "Scale": [crate_scale] * 3}, {
                "RigidBody": {"Type": "Dynamic", "Mass": 9.0, "AngularDamping": 0.1},
                # In the crate's own (unscaled) units: the entity's scale scales the collider too.
                "BoxCollider": {"HalfExtents": [(high[0] - low[0]) / 2.0 - 0.002, (high[1] - low[1]) / 2.0, (high[2] - low[2]) / 2.0 - 0.002],
                                "Offset": center, "Friction": 0.6, "Restitution": 0.05},
                "AudioSource": {"Clip": "asset://Audio/WoodHit.wav", "Spatial": True, "MinDistance": 2.0, "MaxDistance": 40.0},
                "Script": {"ClassName": "Showcase.Impact", "Fields": {"Volume": {"Type": "Float", "Value": 0.8}}}}, crates)
        self.yard_crates = crates

        # The walker: an upright capsule as tall as a person, its feet at the entity, with its footsteps and landings. It waits
        # out of the way until the camera walks (Walker parks it).
        feet = frame.point((0.2, 0.0, 4.4))
        self.walker = self.entity("Walker", {"Transform": transform(feet), "CharacterController": {
            "Radius": 0.32, "Height": 1.8, "Offset": [0.0, 0.9, 0.0], "StepHeight": 0.35, "MaxSlopeAngle": 50.0, "Mass": 75.0, "PushForce": 300.0}})
        footsteps = self.entity("Footsteps", {"AudioSource": {"Clip": "asset://Audio/Step.wav", "Volume": 0.55, "Spatial": False}}, self.walker)
        landing = self.entity("Landing", {"AudioSource": {"Clip": "asset://Audio/Land.wav", "Volume": 0.8, "Spatial": False}}, self.walker)
        self.call("component_add", {"entity": self.walker, "component": "Script", "fields": {"ClassName": "Showcase.Walker", "Fields": {
            "Footsteps": {"Type": "Entity", "Value": footsteps}, "Landing": {"Type": "Entity", "Value": landing}}}})
        sign = frame.point((2.8, 0.0, 4.6))
        self.signpost((sign[0], sign[2]), "CHARACTER", "F walks: stairs, a ramp, posts to jump,\na lift and crates to push")

    # --- Scenery: rocks, stumps, ferns and branches around the exhibits ---------------------------------------------------

    def make_scenery(self):
        scenery = self.entity("Scenery")
        for name, model, position, yaw, size, solid in (
                ("Boulder", BOULDER, (-12.8, 0.0, 8.0), 40.0, 1.8, True),
                ("Mossy Rocks", ROCKS, (0.5, 0.0, -22.5), 8.0, 1.0, True),
                ("Mossy Rocks 2", ROCKS_2, (-19.0, 0.0, -2.0), 70.0, 0.9, True),
                ("Old Trunk", BIG_LOG, (17.5, 0.0, 6.5), -35.0, 1.0, True),
                ("Stump", STUMP, (-7.4, 0.0, 5.2), 20.0, 1.0, True),
                ("Stump 2", STUMP_2, (6.4, 0.0, 9.5), 130.0, 0.9, True),
                ("Fern", FERN, (-2.6, 0.0, 12.4), 15.0, 1.2, False),
                ("Fern 2", FERN, (5.4, 0.0, 6.8), 140.0, 1.3, False),
                ("Fern 3", FERN, (-14.2, 0.0, 4.6), 260.0, 1.4, False),
                ("Fern 4", FERN, (6.0, 0.0, 11.8), 80.0, 1.3, False),
                ("Fern 5", FERN, (-10.4, 0.0, -14.6), 200.0, 1.5, False),
                ("Fern 6", FERN, (18.5, 0.0, -2.5), 300.0, 1.4, False),
                ("Fern 7", FERN, (-6.0, 0.0, 14.5), 45.0, 1.3, False),
                ("Fern 8", FERN, (4.2, 0.0, -5.6), 170.0, 1.1, False),
                ("Branches", BRANCHES, (1.9, 0.0, 11.4), 30.0, 1.0, False),
                ("Branches 2", BRANCHES, (-5.2, 0.0, -2.6), 120.0, 1.0, False),
                ("Branches 3", BRANCHES, (8.0, 0.0, -1.0), 250.0, 0.9, False)):
            (model_low, _) = self.models[model][1]
            place = LAYOUT.place((position[0], -model_low[1] * size, position[2]), (0.0, yaw, 0.0), (size, size, size))
            solid_components = {"RigidBody": {"Type": "Static"}, "MeshCollider": {"Mesh": self.models[model][0], "Convex": False}}
            self.model_entity(name, model, place, solid_components if solid else None, scenery)

        # Shrubs (alpha-tested, two-sided leaves) behind the exhibits.
        (shrub_low, _) = self.models[SHRUBS][1]
        for index, (x, z, yaw, size) in enumerate(((-15.5, 4.0, 30.0, 0.8), (-14.0, -9.5, 150.0, 0.7), (-9.0, -17.5, 80.0, 0.75),
                                                   (9.0, -18.0, 200.0, 0.8), (17.0, -6.0, 290.0, 0.7), (15.5, 7.5, 10.0, 0.75),
                                                   (-11.5, 12.5, 120.0, 0.65), (5.5, 17.5, 250.0, 0.65))):
            self.model_entity(f"Shrubs {index + 1}", SHRUBS, LAYOUT.place((x, -shrub_low[1] * size, z), (0.0, yaw, 0.0), (size, size, size)), None,
                              scenery)

        self.make_trees()

        # Undergrowth in a ring around the clearing (seeded, so every build is the same): it fills the distance and breaks the
        # line where the ground meets the sky's trees.
        undergrowth = self.entity("Undergrowth")
        generator = random.Random(7)
        placed_count = 0
        while placed_count < 90:
            model = generator.choices((FERN, BRANCHES, ROCKS_2, STUMP), weights=(10, 4, 1, 1))[0]
            angle = generator.uniform(0.0, 2.0 * math.pi)
            distance = generator.uniform(22.0, 44.0)
            size = generator.uniform(1.1, 2.0) if model == FERN else generator.uniform(0.45, 0.8) if model == ROCKS_2 else generator.uniform(0.8, 1.3)
            yaw = generator.uniform(0.0, 360.0)
            position = (math.cos(angle) * distance, -self.models[model][1][0][1] * size, math.sin(angle) * distance)
            placed_count += 1
            self.model_entity(f"Undergrowth {placed_count}", model, LAYOUT.place(position, (0.0, yaw, 0.0), (size, size, size)), None, undergrowth)

    def make_trees(self):
        """Bare trees ring the clearing (seeded, so every build is the same), clear of the exhibits at its edge: they stand in
        front of the sky's forest, so it moves behind them as the camera flies, and the low sun lays their shadows across
        the clearing. A capsule around each trunk's foot stops balls."""
        trees = self.entity("Trees")
        # Where the clearing's edge is taken: the lighthouse, the rock sets, the old trunk, the bridge, the goal, the boulder.
        keep_clear = (((-5.5, -21.0), 4.5), ((0.5, -22.5), 5.0), ((-19.0, -2.0), 5.0), ((17.5, 6.5), 3.5), ((-12.8, -13.4), 5.0),
                      ((13.2, -11.0), 4.5), ((-12.8, 8.0), 3.0))
        generator = random.Random(17)
        # The first three stand towards the sun (south-east), so their trunks lay long shadows across the plaza.
        sunward = [(17.1, 10.3), (14.2, 18.6), (21.5, 3.0)]
        placed = []
        attempts = 0
        while len(placed) < 26 and attempts < 2000:
            attempts += 1
            angle = generator.uniform(0.0, 2.0 * math.pi)
            distance = generator.uniform(18.0, 31.0)
            x, z = sunward[len(placed)] if len(placed) < len(sunward) else (math.cos(angle) * distance, math.sin(angle) * distance)
            if any(math.hypot(x - cx, z - cz) < reach for (cx, cz), reach in keep_clear):
                continue
            if any(math.hypot(x - px, z - pz) < 4.5 for px, pz in placed):
                continue
            placed.append((x, z))
            mesh = f"asset://Meshes/Tree{'ABC'[len(placed) % 3]}.gltf"
            size = generator.uniform(0.85, 1.2)
            self.entity(f"Tree {len(placed)}", {
                "Transform": LAYOUT.place((x, 0.0, z), (0.0, generator.uniform(0.0, 360.0), 0.0), (size, size, size)),
                "Mesh": {"Mesh": mesh, "Materials": [self.textures[BARK]]},
                "RigidBody": {"Type": "Static"},
                "CapsuleCollider": {"Radius": 0.45, "HalfHeight": 2.0, "Offset": [0.0, 2.4, 0.0]}}, trees)

    # --- Sound, screen and tour -----------------------------------------------------------------------------------------------

    def make_sound(self):
        self.entity("Music", {"AudioSource": {"Clip": "asset://Audio/Music.wav", "Volume": 0.3, "Loop": True, "PlayOnStart": True, "Spatial": False}})
        self.ambience = self.entity("Forest Ambience", {"AudioSource": {"Clip": "asset://Audio/Ambience.wav", "Volume": 0.5, "Loop": True,
                                                                        "PlayOnStart": True, "Spatial": False}})
        # Crickets, silent until night falls (DayNight turns them up).
        self.night_ambience = self.entity("Night Ambience", {"AudioSource": {"Clip": "asset://Audio/Night.wav", "Volume": 0.0, "Loop": True,
                                                                             "PlayOnStart": True, "Spatial": False}})
        self.call("component_add", {"entity": self.fountain, "component": "AudioSource", "fields": {
            "Clip": "asset://Audio/Fountain.wav", "Volume": 0.8, "Loop": True, "PlayOnStart": True, "Spatial": True, "MinDistance": 2.0,
            "MaxDistance": 26.0}})

    def make_grading(self):
        """The scene's look in a global post-process volume, golden hour: warm, with a little more contrast and color than life,
        a soft vignette and a trace of film grain. A second global volume holds the look of the night (cooler, with the
        exposure of eyes used to the dark), weighted in by DayNight, which also turns the sun into moonlight."""
        grading = self.entity("Post Processing")
        self.look = self.entity("Golden Hour", {"PostProcess": {
            "Global": True, "OverrideContrast": True, "Contrast": 1.1, "OverrideSaturation": True, "Saturation": 1.1,
            "OverrideTemperature": True, "Temperature": 0.1, "OverrideTint": True, "Tint": 0.02, "OverrideVignette": True, "Vignette": 0.35,
            "OverrideVignetteSmoothness": True, "VignetteSmoothness": 0.5, "OverrideFilmGrain": True, "FilmGrain": 0.03,
            "OverrideBloomIntensity": True, "BloomIntensity": 0.12}}, grading)
        night = self.entity("Night", {"PostProcess": {
            "Global": True, "Priority": 2.0, "Weight": 0.0, "OverrideEV100": True, "EV100": -2.0, "OverrideTemperature": True,
            "Temperature": -0.5, "OverrideTint": True, "Tint": 0.04, "OverrideSaturation": True, "Saturation": 0.8,
            "OverrideContrast": True, "Contrast": 1.15, "OverrideBloomIntensity": True, "BloomIntensity": 0.22, "OverrideVignette": True,
            "Vignette": 0.5}}, grading)
        entity = {"Type": "Entity"}
        self.day_night = self.entity("Day and Night", {"Script": {"ClassName": "Showcase.DayNight", "Fields": {
            "Sun": entity | {"Value": self.found["Sun"]}, "Sky": entity | {"Value": self.found["Sky"]}, "NightGrade": entity | {"Value": night},
            "EveningZone": entity | {"Value": self.evening_zone}, "DayAmbience": entity | {"Value": self.ambience},
            "NightAmbience": entity | {"Value": self.night_ambience}}}})

    def make_playground(self):
        """The playground, which counts the props knocked down and puts the exhibits back, the debug view and the thrown ball's
        prefab."""
        ball = self.entity("Ball", {
            "Transform": transform((0.0, -50.0, 0.0), scale=(0.24, 0.24, 0.24)),
            "Mesh": {"Mesh": "builtin://Sphere", "Materials": [self.ball_metal]},
            "RigidBody": {"Type": "Dynamic", "Mass": 5.0, "ContinuousCollision": True, "AngularDamping": 0.2},
            "SphereCollider": {"Radius": 0.5, "Friction": 0.4, "Restitution": 0.35},
            "AudioSource": {"Clip": "asset://Audio/MetalHit.wav", "Spatial": True, "MinDistance": 2.0, "MaxDistance": 40.0},
            "Script": {"ClassName": "Showcase.Impact", "Fields": {"Volume": {"Type": "Float", "Value": 0.7},
                                                                  "Lifetime": {"Type": "Float", "Value": 40.0}}}})
        self.ball_prefab = self.call("prefab_create", {"entity": ball, "path": "Prefabs/Ball.sprefab"})["reference"]
        self.call("entity_delete", {"entities": [ball]})
        self.playground = self.entity("Playground", {"Script": {"ClassName": "Showcase.Playground", "Fields": {
            "Props": {"Type": "Entity", "Value": self.props}, "Movables": {"Type": "Entity", "Value": self.yard_crates},
            "Dominoes": {"Type": "Entity", "Value": self.dominoes},
            "Rig": {"Type": "Entity", "Value": self.rig}, "Launcher": {"Type": "Entity", "Value": self.launcher},
            "WreckingBall": {"Type": "Entity", "Value": self.wrecking_ball}}}})
        self.debug_view = self.entity("Debug View", {"Script": {"ClassName": "Showcase.DebugView", "Fields": {
            "Rig": {"Type": "Entity", "Value": self.rig}, "Launcher": {"Type": "Entity", "Value": self.launcher},
            "Triggers": {"Type": "Entity", "Value": self.find_root("Triggers")}, "Bridge": {"Type": "Entity", "Value": self.bridge}}}})

    def make_hud(self):
        """The screen: a UI canvas, 1920 x 1080 units fitted to any window, with the HUD (the feature list, the status, the
        buttons, the hint line and the crosshair) and over it the tour's letterbox, title, captions and fade."""
        # In the editor's scene view the canvas stands behind the home view, out of the way of the exhibits.
        screen = self.entity("Screen", {"Transform": LAYOUT.place((0.0, 0.0, 24.0)),
                                        "UICanvas": {"ScaleMode": "ScaleWithScreenSize", "ReferenceResolution": [1920.0, 1080.0]}})
        hud = self.entity("HUD", {"UIRect": ui_fill(), "AudioSource": {"Clip": "asset://Audio/UIClick.wav", "Volume": 0.5, "Spatial": False}},
                          screen)
        hover = self.entity("Hover Sound", {"AudioSource": {"Clip": "asset://Audio/UIHover.wav", "Volume": 0.35, "Spatial": False}}, hud)
        entity = {"Type": "Entity"}
        hud_fields = {"Rig": entity | {"Value": self.rig}, "Playground": entity | {"Value": self.playground},
                      "Goal": entity | {"Value": self.goal}, "WreckingBall": entity | {"Value": self.wrecking_ball},
                      "Launcher": entity | {"Value": self.launcher}, "Lighting": entity | {"Value": self.day_night},
                      "HoverSound": entity | {"Value": hover},
                      "SunIcon": {"Type": "Asset", "Value": "asset://Textures/Icons/Sun.png"},
                      "MoonIcon": {"Type": "Asset", "Value": "asset://Textures/Icons/Moon.png"},
                      "WalkIcon": {"Type": "Asset", "Value": "asset://Textures/Icons/Walk.png"},
                      "FlyIcon": {"Type": "Asset", "Value": "asset://Textures/Icons/Fly.png"},
                      "FlyHint": {"Type": "String", "Value": FLY_HINT}, "WalkHint": {"Type": "String", "Value": WALK_HINT},
                      "FreeCursorHint": {"Type": "String", "Value": FREE_CURSOR_HINT}}

        # The feature list, top left: the engine's name, then a row per exhibit area, its name alone until it lights up and
        # opens (Hud stacks the rows; this is the list with every row closed).
        row_height, row_gap = 34.0, 4.0
        rows_height = row_height * len(FEATURES) + row_gap * (len(FEATURES) - 1)
        panel = self.entity("Feature Panel", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (36.0, 36.0), (440.0, 124.0 + rows_height + 16.0)),
                                              "UIImage": {"Color": PANEL, "CornerRadius": 18.0}}, hud)
        self.entity("Brand", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (30.0, 22.0), (380.0, 52.0)),
                              "UIText": ui_text("S T R A D A", 42.0, TEXT)}, panel)
        self.entity("Subtitle", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (32.0, 74.0), (380.0, 24.0)),
                                 "UIText": ui_text("ENGINE SHOWCASE", 17.0, AMBER)}, panel)
        self.entity("Rule", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (30.0, 110.0), (380.0, 2.0)), "UIImage": {"Color": AMBER[:3] + [0.45]}},
                    panel)
        features = self.entity("Features", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (18.0, 124.0), (404.0, rows_height))}, panel)
        for index, (name, detail, center, radius) in enumerate(FEATURES):
            row = self.entity(f"Feature: {name}", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (0.0, index * (row_height + row_gap)),
                                                                     (404.0, row_height))}, features)
            glow = self.entity("Glow", {"UIRect": ui_fill(Opacity=0.0), "UIImage": {"Color": linear(1.0, 0.68, 0.3, 0.13), "CornerRadius": 10.0}}, row)
            bar = self.entity("Bar", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.5), (6.0, 0.0), (4.0, -12.0), (0.0, 1.0), Opacity=0.25),
                                      "UIImage": {"Color": AMBER, "CornerRadius": 2.0}}, row)
            label = self.entity("Name", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (22.0, 4.0), (376.0, 26.0)), "UIText": ui_text(name, 21.0, TEXT)},
                                row)
            line = self.entity("Detail", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (22.0, 31.0), (376.0, 18.0), Opacity=0.0),
                                          "UIText": ui_text(detail, 15.0, DIM)}, row)
            x, z = center if center else (0.0, 0.0)
            self.call("component_add", {"entity": row, "component": "Script", "fields": {"ClassName": "Showcase.HudFeature", "Fields": {
                "Center": {"Type": "Vector3", "Value": [x, 0.0, z]}, "Radius": {"Type": "Float", "Value": radius},
                "Glow": entity | {"Value": glow}, "Bar": entity | {"Value": bar}, "Label": entity | {"Value": label},
                "Detail": entity | {"Value": line}, "ClosedHeight": {"Type": "Float", "Value": row_height},
                "DetailHeight": {"Type": "Float", "Value": 22.0}}}})
        hud_fields["Features"] = entity | {"Value": features}
        hud_fields["FeaturePanel"] = entity | {"Value": panel}
        hud_fields["RowGap"] = {"Type": "Float", "Value": row_gap}

        # The status, top right: the props knocked down and the goals.
        status = self.entity("Status", {"UIRect": ui_rect((1.0, 0.0), (1.0, 0.0), (-36.0, 36.0), (340.0, 92.0)),
                                        "UIImage": {"Color": PANEL, "CornerRadius": 16.0}}, hud)
        for column, (label, field) in enumerate((("KNOCKED DOWN", "KnockedText"), ("GOALS", "GoalsText"))):
            left = 24.0 + 182.0 * column
            self.entity(f"{label.title()} Label", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (left, 14.0), (150.0, 20.0)),
                                                   "UIText": ui_text(label, 14.0, DIM)}, status)
            value = self.entity(f"{label.title()}", {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (left, 36.0), (150.0, 42.0)),
                                                     "UIText": ui_text("0", 34.0, TEXT)}, status)
            hud_fields[field] = entity | {"Value": value}
        self.entity("Divider", {"UIRect": ui_rect((0.0, 0.5), (0.5, 0.5), (190.0, 0.0), (1.0, 56.0)), "UIImage": {"Color": linear(1.0, 1.0, 1.0, 0.14)}},
                    status)

        # The buttons, down the right side under the status, each with its icon and its key.
        buttons = self.entity("Buttons", {"UIRect": ui_rect((1.0, 0.0), (1.0, 0.0), (-36.0, 144.0), (340.0, 64.0 * len(BUTTONS)))}, hud)
        for index, (name, label, icon, key, field) in enumerate(BUTTONS):
            button = self.entity(name, {"UIRect": ui_rect((0.0, 0.0), (0.0, 0.0), (0.0, 64.0 * index), (340.0, 56.0)),
                                        "UIImage": {"CornerRadius": 14.0}, "UIButton": BUTTON}, buttons)
            image = self.entity("Icon", {"UIRect": ui_rect((0.0, 0.5), (0.0, 0.5), (16.0, 0.0), (30.0, 30.0)),
                                         "UIImage": {"Texture": f"asset://Textures/Icons/{icon}.png", "Color": TEXT}}, button)
            text = self.entity("Label", {"UIRect": ui_rect((0.0, 0.5), (0.0, 0.5), (60.0, 0.0), (230.0, 40.0)),
                                         "UIText": ui_text(label, 20.0, TEXT)}, button)
            keycap = self.entity("Key", {"UIRect": ui_rect((1.0, 0.5), (1.0, 0.5), (-14.0, 0.0), (30.0, 30.0)),
                                         "UIImage": {"Color": linear(1.0, 1.0, 1.0, 0.13), "CornerRadius": 7.0}}, button)
            self.entity("Letter", {"UIRect": ui_fill(), "UIText": ui_text(key, 16.0, TEXT, "Center")}, keycap)
            hud_fields[field] = entity | {"Value": button}
            if field == "LightsButton":
                hud_fields["LightsIcon"] = entity | {"Value": image}
            if field == "ModeButton":
                hud_fields["ModeIcon"] = entity | {"Value": image}
                hud_fields["ModeLabel"] = entity | {"Value": text}

        # The hint line at the bottom, and the crosshair, shown while looking around.
        hint = self.entity("Hint", {"UIRect": ui_rect((0.5, 1.0), (0.5, 1.0), (0.0, -26.0), (1300.0, 44.0)),
                                    "UIImage": {"Color": linear(0.0, 0.0, 0.0, 0.42), "CornerRadius": 22.0}}, hud)
        hud_fields["HintText"] = entity | {"Value": self.entity("Text", {"UIRect": ui_fill(),
                                                                          "UIText": ui_text(FLY_HINT, 18.0, linear(0.9, 0.9, 0.88), "Center")},
                                                                hint)}
        self.crosshair = self.entity("Crosshair", {"UIRect": ui_rect((0.5, 0.5), (0.5, 0.5), (0.0, 0.0), (46.0, 46.0), Visible=False),
                                                   "UIImage": {"Texture": "asset://Textures/Crosshair.png", "Color": linear(1.0, 1.0, 1.0, 0.85)}},
                                     hud)
        self.call("component_add", {"entity": hud, "component": "Script", "fields": {"ClassName": "Showcase.Hud", "Fields": hud_fields}})
        self.hud = hud

        # The tour's screen, over the HUD. Its texts and fade are authored transparent and its letterbox flat (Flythrough shows
        # them), so the editor's game view shows the HUD alone.
        tour = self.entity("Tour Screen", {"UIRect": ui_fill()}, screen)
        letterbox = self.entity("Letterbox", {"UIRect": ui_fill()}, tour)
        black = linear(0.0, 0.0, 0.0)
        self.entity("Top Bar", {"UIRect": ui_rect((0.0, 0.0), (0.5, 0.0), (0.0, 0.0), (0.0, 0.0), (1.0, 0.0)), "UIImage": {"Color": black}}, letterbox)
        self.entity("Bottom Bar", {"UIRect": ui_rect((0.0, 1.0), (0.5, 1.0), (0.0, 0.0), (0.0, 0.0), (1.0, 1.0)), "UIImage": {"Color": black}},
                    letterbox)
        self.letterbox = letterbox
        # The title under the neon sign the tour opens on: letter-spaced between two rules, over a soft shadow.
        self.tour_title = self.entity("Tour Title", {"UIRect": ui_rect((0.5, 1.0), (0.5, 0.5), (0.0, -300.0), (1100.0, 120.0), Opacity=0.0)}, tour)
        soft = "asset://Textures/SoftBand.png"
        self.entity("Backdrop", {"UIRect": ui_rect((0.5, 0.5), (0.5, 0.5), (0.0, 0.0), (1400.0, 210.0)),
                                 "UIImage": {"Texture": soft, "Color": linear(0.0, 0.0, 0.0, 0.62)}}, self.tour_title)
        for name, y in (("Rule", -42.0), ("Rule 2", 44.0)):
            self.entity(name, {"UIRect": ui_rect((0.5, 0.5), (0.5, 0.5), (0.0, y), (620.0, 2.0)), "UIImage": {"Color": AMBER[:3] + [0.85]}},
                        self.tour_title)
        title = "   ".join(" ".join(word) for word in ("ENGINE", "SHOWCASE"))
        self.entity("Text", {"UIRect": ui_fill(), "UIText": ui_text(title, 40.0, TEXT, "Center")}, self.tour_title)
        # The caption with its detail line in the lower third, over a soft shadow.
        self.tour_caption = self.entity("Tour Caption", {"UIRect": ui_rect((0.5, 1.0), (0.5, 1.0), (0.0, -136.0), (1500.0, 110.0), Opacity=0.0)},
                                        tour)
        self.entity("Backdrop", {"UIRect": ui_rect((0.5, 0.5), (0.5, 0.5), (0.0, 0.0), (1600.0, 200.0)),
                                 "UIImage": {"Texture": soft, "Color": linear(0.0, 0.0, 0.0, 0.66)}}, self.tour_caption)
        self.tour_caption_text = self.entity("Text", {"UIRect": ui_rect((0.0, 0.0), (0.5, 0.0), (0.0, 0.0), (0.0, 62.0), (1.0, 0.0)),
                                                      "UIText": ui_text("", 46.0, TEXT, "Center", "Bottom")}, self.tour_caption)
        self.tour_detail = self.entity("Detail", {"UIRect": ui_rect((0.0, 0.0), (0.5, 0.0), (0.0, 68.0), (0.0, 34.0), (1.0, 0.0)),
                                                  "UIText": ui_text("", 23.0, AMBER, "Center", "Top")}, self.tour_caption)
        self.tour_hint = self.entity("Tour Hint", {"UIRect": ui_rect((1.0, 1.0), (1.0, 0.5), (-48.0, -55.0), (600.0, 30.0), Opacity=0.0),
                                                   "UIText": ui_text("Press any key to explore", 18.0, linear(0.85, 0.85, 0.85), "Right")}, tour)
        # Over everything: the black the tour fades in from.
        self.tour_fade = self.entity("Tour Fade", {"UIRect": ui_fill(Opacity=0.0), "UIImage": {"Color": black}}, tour)
        self.finish_camera(self.ball_prefab, self.crosshair)

    def find_root(self, name):
        return [entity["id"] for entity in self.call("entity_find", {"name": name})["entities"]][0]

    def make_tour(self):
        """The opening: shots the camera glides through, each a CameraShot with a caption, cueing exhibits on the way."""
        entity = {"Type": "Entity"}
        self.tour_fields = {
            "Rig": entity | {"Value": self.rig}, "Fade": entity | {"Value": self.tour_fade}, "Letterbox": entity | {"Value": self.letterbox},
            "Title": entity | {"Value": self.tour_title}, "Caption": entity | {"Value": self.tour_caption},
            "CaptionText": entity | {"Value": self.tour_caption_text}, "Detail": entity | {"Value": self.tour_detail},
            "Hint": entity | {"Value": self.tour_hint}, "Hud": entity | {"Value": self.hud}}
        tour = self.entity("Camera Tour", {"Script": {"ClassName": "Showcase.Flythrough", "Fields": self.tour_fields}})
        for name, frame, eye, target, seconds, caption, detail, cue, lead in TOUR:
            fields = {"Caption": {"Type": "String", "Value": caption}, "Detail": {"Type": "String", "Value": detail},
                      "Seconds": {"Type": "Float", "Value": seconds}}
            if cue is not None:
                fields["Cue"] = {"Type": "Entity", "Value": getattr(self, cue)}
                fields["CueLead"] = {"Type": "Float", "Value": lead}
            self.entity(f"Shot: {name}", {"Transform": frame.look(eye, target), "Script": {"ClassName": "Showcase.CameraShot", "Fields": fields}},
                        tour)
        self.tour = tour

    # --- The test scene -------------------------------------------------------------------------------------------------------

    def make_test_scene(self):
        call = self.call
        # The test scene: the main scene without the tour playing, with the checks.
        call("scene_save", {"path": "Scenes/Tests.sscene"})
        call("scene_settings", {"name": "Showcase Tests"})
        # A script's fields are set as a whole.
        call("component_set", {"entity": self.tour, "component": "Script", "fields": {"Fields": self.tour_fields | {
            "PlayOnStart": {"Type": "Bool", "Value": False}}}})
        entity = {"Type": "Entity"}
        call("entity_create", {"name": "Tests", "components": {"Script": {"ClassName": "Showcase.ShowcaseTests", "Fields": {
            "Playground": entity | {"Value": self.playground}, "Rig": entity | {"Value": self.rig}, "Camera": entity | {"Value": self.camera},
            "Lantern": entity | {"Value": self.lantern_light}, "WreckingBall": entity | {"Value": self.wrecking_ball},
            "Paddle": entity | {"Value": self.paddle}, "Campfire": entity | {"Value": self.campfire}, "Goal": entity | {"Value": self.goal},
            "Launcher": entity | {"Value": self.launcher}, "Plate": entity | {"Value": self.plate}, "Gate": entity | {"Value": self.gate},
            "Dominoes": entity | {"Value": self.dominoes}, "Bridge": entity | {"Value": self.bridge}, "Tour": entity | {"Value": self.tour},
            "LightShow": entity | {"Value": self.light_show}, "Lighthouse": entity | {"Value": self.lighthouse},
            "Gallery": entity | {"Value": self.gallery_spheres}, "Fountain": entity | {"Value": self.fountain_jets},
            "Fireflies": entity | {"Value": self.fireflies}, "Sparks": entity | {"Value": self.sparks}, "Leaves": entity | {"Value": self.leaves},
            "DebugView": entity | {"Value": self.debug_view}, "Hud": entity | {"Value": self.hud}, "Lighting": entity | {"Value": self.day_night},
            "NeonSign": entity | {"Value": self.neon_sign}, "Look": entity | {"Value": self.look},
            "EveningZone": entity | {"Value": self.evening_zone}, "Walker": entity | {"Value": self.walker},
            "Lift": entity | {"Value": self.lift}, "Deck": entity | {"Value": self.deck}, "FirstStep": entity | {"Value": self.first_step},
            "Ramp": entity | {"Value": self.ramp}}}}})
        call("scene_save")
        call("scene_open", {"path": "Scenes/Main.sscene"})

        run = call("test_run", {"scene": "Scenes/Tests.sscene", "timeout": 60})
        print("test run:", {key: run[key] for key in ("finished", "timedOut", "frames", "passed", "failed", "exceptions")})
        for result in run["results"]:
            print("  ", "PASS" if result["passed"] else "FAIL", result["name"], result.get("message", ""))
        if run["failed"] or run["exceptions"] or not run["finished"]:
            raise SystemExit("the tests failed")


if __name__ == "__main__":
    main()

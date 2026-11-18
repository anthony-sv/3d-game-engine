"""Builds the Moto project through `strada mcp`, the way an agent does: the circuit of a motorcycle time trial made of
visual scripts alone (no C#). In order:

1. the project and its settings;
2. the circuit part: the meshes, textures and sky GenerateAssets.py makes from the layout in Circuit.py, the materials,
   the physics materials of the surfaces (Surfaces/*.sphysmat: asphalt, kerb, gravel and grass), and the circuit's
   entities (the track, the barriers, the ground and the hills, the start gantry, the three sector gates and the
   footbridge), each surface a static body with a triangle-mesh collider, the track's with a physics material per
   surface;
3. the lighting part: the sun, the sky and the scene's renderer settings (shadows over 220 m in four cascades);
4. the camera part: the main camera at the home view over the start, its far plane past the hills, and its tour, a graph
   (Scripts/Tour.sgraph) whose timeline flies it once round the circuit and home (Tour.py makes the path);
5. the props part: 10,000 props placed by seed (Props.py: tyre stacks, cones, braking markers, catch fences, advertising
   boards, grandstands, marshals' posts, floodlights, bushes and trees) on the meshes and textures GenerateAssets.py makes
   (Meshes/Props, Textures/Props) with two materials (Materials/Props), a group under the Props entity for each kind,
   each group made with all its props in one request;
6. the bike part: the motorcycle and its rider on the grid (Bike.py makes them; GenerateAssets.py writes their meshes to
   Meshes/Bike and their livery to Textures/Bike), with their four materials (Materials/Bike), the Bike entity with its
   body, fork, wheels and rider;
7. the input part: the game's input actions (Input/Moto.sinput, Controls.py), which the project file names;
8. the sound part: the sounds GenerateAudio.py makes (Audio/*.wav: the engine's layers on and off load, the wind, a tyre
   squeal, a shift blip and a crowd), the mixer they play through (Audio/Moto.smixer, Sound.py), which the project file
   names, the echo under the footbridge (the Footbridge Echo entity, a reverb zone) and the audio director (the Audio
   Director entity and Scripts/AudioDirector.sgraph: the Pause action blends the mix to the Paused snapshot and back);
9. the ghost part: the ghost bike, a second motorcycle and rider of the bike part's meshes and materials, at the start and
   finish line, its graph (Scripts/Ghost.sgraph), whose timeline rides it round the racing line (RacingLine.py makes
   the line and its keys, Ghost.py the graph), and its engine's sound (EngineSound.py: the Engine Sound entity under it
   with a voice for each layer, Scripts/EngineSound.sgraph and the curve assets it crossfades the layers through);
10. the trackside part: the four trackside cameras and the camera director (Trackside.py: the Trackside entity, whose
    children the cameras are, and Scripts/CameraDirector.sgraph), which cuts between them and the tour on actions;
11. the tests part: the test scene (Scenes/Tests.sscene, the main scene with the checks and without the props) and its
    graphs (Checks.py);
12. checks: spheres dropped on the road, a kerb, a gravel trap, a verge, the ground, a barrier and the footbridge come to
    rest on their surfaces; the test scene's checks pass in a test run; the tour plays in play mode, the camera where its
    path puts it, and so does the ghost, where its line puts it; and nothing logs a warning.

    python Projects/Moto/Tools/Author.py [strada executable]
    python Projects/Moto/Tools/Author.py --part <circuit|lighting|camera|props|bike|input|sound|ghost|trackside|tests> [strada]
    python Projects/Moto/Tools/Author.py --captures <output folder> --player <StradaRuntime executable> [--views ...]
        [--sounds]

The project is rebuilt from scratch: everything but this Tools folder is replaced. Without an executable, strada runs
from the repository (dotnet run).

With --part, one part is built in place instead: the project is opened as it is, the part's root entities are made anew
in the main scene where they were among the roots, each entity with the ID it had (found by its names from the root),
and its files are written (materials, physics materials and graphs keep their asset handles), so that a part run again
with nothing changed writes the same files. The tests part follows every other, since the test scene is the main scene
with the checks and they name its entities; its own entities keep their IDs too. The run compares every file of the
project outside Tools before and after: when a file the part (or the tests part after it) does not own changed, it names
the files and fails, also when the authoring itself failed. After a change to how graphs are arranged, `--part camera`
writes every graph again: the tour's, then those of the parts that have made theirs (the audio director's, the ghost's
and its engine sound's, the camera director's) and the test scene's.

With --captures, nothing is built: the player takes its screenshots of the main scene from the fixed list of cameras in
Captures.py, each from a copy of the project outside the repository, at 1920 by 1080 pixels, into the output folder;
the four trackside cameras' views catch the ghost riding by. With --sounds as well, the player also records the ghost
passing each trackside camera, as its listener hears it, into <view>.wav.
"""
import argparse
import hashlib
import json
import math
import os
import posixpath
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import Checks  # noqa: E402  (the test scene and its graphs)
import Circuit  # noqa: E402  (the layout)
import GenerateAssets  # noqa: E402  (the circuit's meshes, textures and sky)
import Bike  # noqa: E402  (the motorcycle and its rider)
import Captures  # noqa: E402  (the player's screenshots from fixed cameras)
import Controls  # noqa: E402  (the input actions)
import EngineSound  # noqa: E402  (the engine's sound)
import GenerateAudio  # noqa: E402  (the sounds)
import Ghost  # noqa: E402  (the ghost bike)
import Graphs  # noqa: E402  (writing graphs)
import Props  # noqa: E402  (the props' placement)
import RacingLine  # noqa: E402  (the line the ghost rides)
import Sound  # noqa: E402  (the mixer)
import Tour  # noqa: E402  (the camera tour)
import Trackside  # noqa: E402  (the trackside cameras and their director)

PROJECT = os.path.dirname(TOOLS)
ROOT = os.path.dirname(os.path.dirname(PROJECT))
NAME = "Strada Moto"


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
            raise SystemExit(f"{tool} {json.dumps(arguments)[:2000]} failed: {text}")
        return json.loads(text)

    def close(self):
        self.process.stdin.close()
        return self.process.wait(timeout=120)


# --- Parts ---------------------------------------------------------------------------------------------------------------

# Each part: the root entities of the main scene it makes, and the files of the project it may write (paths relative to
# the project folder, forward slashes; "<folder>/*<extension>" takes the folder's own files of that extension). The tests
# part makes no entity of the main scene: its own scene is made from it; nor does the input part, whose files are the
# actions and the project file that names them. The camera part writes every graph again.
PARTS = {
    "circuit": (("Circuit",), ("Assets/Meshes/*.gltf", "Assets/Textures/*.png", "Assets/Environments/*.hdr",
                               "Assets/Materials/*.smat", "Assets/Surfaces/*.sphysmat", "Assets/Scenes/Main.sscene",
                               "Assets/AssetRegistry.sreg")),
    "lighting": (("Sun", "Sky"), ("Assets/Scenes/Main.sscene",)),
    "camera": (("Main Camera",), ("Assets/Scenes/Main.sscene", "Assets/Scripts/Tour.sgraph", "Assets/Scripts/Ghost.sgraph",
                                  "Assets/Scripts/EngineSound.sgraph", "Assets/Scripts/AudioDirector.sgraph",
                                  "Assets/Scripts/CameraDirector.sgraph", "Assets/AssetRegistry.sreg")),
    "props": (("Props",), ("Assets/Meshes/Props/*.gltf", "Assets/Textures/Props/*.png", "Assets/Materials/Props/*.smat",
                           "Assets/Scenes/Main.sscene", "Assets/AssetRegistry.sreg")),
    "bike": (("Bike",), ("Assets/Meshes/Bike/*.gltf", "Assets/Textures/Bike/*.png", "Assets/Materials/Bike/*.smat",
                         "Assets/Scenes/Main.sscene", "Assets/AssetRegistry.sreg")),
    "input": ((), ("Assets/Input/*.sinput", "Moto.sproj", "Assets/AssetRegistry.sreg")),
    "sound": ((Sound.DIRECTOR, Sound.ECHO), ("Assets/Audio/*.wav", "Assets/Audio/Engine/*.wav", "Assets/Audio/*.smixer",
                                             "Moto.sproj", "Assets/Scenes/Main.sscene", "Assets/Scripts/AudioDirector.sgraph",
                                             "Assets/AssetRegistry.sreg")),
    "ghost": ((Ghost.ENTITY,), ("Assets/Scenes/Main.sscene", "Assets/Scripts/Ghost.sgraph", "Assets/Scripts/EngineSound.sgraph",
                                "Assets/Curves/*.scurve", "Assets/AssetRegistry.sreg")),
    "trackside": ((Trackside.ROOT,), ("Assets/Scenes/Main.sscene", "Assets/Scripts/CameraDirector.sgraph",
                                      "Assets/AssetRegistry.sreg")),
    "tests": ((), ("Assets/Scenes/Tests.sscene", "Assets/Scripts/Checks.sgraph", "Assets/Scripts/Tests.sgraph",
                   "Assets/AssetRegistry.sreg")),
}
ORDER = ("circuit", "lighting", "camera", "props", "bike", "input", "sound", "ghost", "trackside", "tests")
# The parts that make entities of the main scene, or the files it needs (the input part's actions, which the sound and
# trackside parts' graphs name).
SCENE_PARTS = ORDER[:-1]


def _ghost_graphs(write):
    """Declares or writes the ghost's graphs: its engine sound's first, since the ghost's sets the engine sound's
    variables."""
    def run(author):
        getattr(EngineSound, write)(author)
        getattr(Ghost, write)(author)
    return run


# The graphs of the parts, each group written together (every graph's declarations before any body names them): the part
# that makes them, the graphs, and how to declare and write them.
GRAPH_GROUPS = (
    ("camera", (Tour.TOUR,), Tour.declare, Tour.write),
    ("sound", (Sound.AUDIO_DIRECTOR,), Sound.declare, Sound.write),
    ("ghost", (EngineSound.ENGINE_SOUND, Ghost.GHOST), _ghost_graphs("declare"), _ghost_graphs("write")),
    ("trackside", (Trackside.CAMERA_DIRECTOR,), Trackside.declare, Trackside.write),
)
# Every graph of the project, in the order they are written.
GRAPHS = tuple(graph for _, graphs, _, _ in GRAPH_GROUPS for graph in graphs) + Checks.GRAPHS


def hash_project():
    """The SHA-256 of every file of the project outside Tools, by its path relative to the project (forward slashes)."""
    hashes = {}
    for folder, folders, files in os.walk(PROJECT):
        if folder == PROJECT:
            # Tools holds this script and Python's caches; .strada is the editor's local state, which git ignores.
            folders[:] = [name for name in folders if name not in ("Tools", ".strada")]
        for name in files:
            path = os.path.join(folder, name)
            with open(path, "rb") as file:
                hashes[os.path.relpath(path, PROJECT).replace(os.sep, "/")] = hashlib.sha256(file.read()).hexdigest()
    return hashes


def owns(owned, path):
    """Whether a path of the project is one of a part's own files (PARTS)."""
    folder, name = posixpath.split(path)
    for own in owned:
        own_folder, own_name = posixpath.split(own)
        if own_name.startswith("*"):
            if folder == own_folder and name.endswith(own_name[1:]):
                return True
        elif path == own:
            return True
    return False


def foreign_changes(before, part):
    """Lists what changed in the project since the hashes were taken, and returns the changed files neither the part nor
    the tests part after it owns."""
    after = hash_project()
    changed = sorted(path for path in set(before) | set(after) if before.get(path) != after.get(path))
    print("changed:", ", ".join(changed) if changed else "nothing")
    owned = PARTS[part][1] + PARTS["tests"][1]
    return [path for path in changed if not owns(owned, path)]


# --- Transforms ----------------------------------------------------------------------------------------------------------

def place(s, lateral=0.0, height=0.0):
    """A transform on the circuit: at a lap distance and a lateral offset (right of travel), its forward along the travel."""
    return {"Translation": list(Circuit.offset_point(s, lateral, height)), "RotationEuler": [0.0, Circuit.yaw(s), 0.0],
            "Scale": [1.0, 1.0, 1.0]}


def look(eye, target):
    """A camera transform at the eye looking at the target (world points): pitch and yaw, no roll."""
    dx, dy, dz = (target[k] - eye[k] for k in range(3))
    pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
    yaw = math.degrees(math.atan2(-dx, -dz))
    return {"Translation": list(eye), "RotationEuler": [pitch, yaw, 0.0], "Scale": [1.0, 1.0, 1.0]}


def sun_euler():
    """The directional light's Euler angles: its forward (-Z) points away from the sun (GenerateAssets.sun_direction), whose
    glow the sky shows."""
    x, _, z = GenerateAssets.sun_direction()
    return [-GenerateAssets.SUN_ELEVATION, math.degrees(math.atan2(x, z)), 0.0]


# --- The authoring session -------------------------------------------------------------------------------------------------

def mesh(name):
    return f"asset://Meshes/{name}.gltf"


def material_reference(name):
    return f"asset://Materials/{name}.smat"


def surface_reference(name):
    return f"asset://Surfaces/{name}.sphysmat"


# The physics materials of the circuit's surfaces (GenerateAssets.SURFACES): how a body slides on each, and how much it
# holds back a wheel that rolls on it. A box sliding on the gravel ploughs into it: the gravel's friction is the larger
# and wins over the other side's (Maximum), so whatever slides into a trap stops sooner than on the asphalt. A tyre grips
# the loose gravel less than the asphalt: how much is the vehicle's to model, from RollingResistance and its own tyres,
# not from this sliding friction.
SURFACE_MATERIALS = {
    "Asphalt": {"Friction": 1.0, "RollingResistance": 0.015, "Surface": "Asphalt"},
    "Kerb": {"Friction": 0.8, "RollingResistance": 0.02, "Surface": "Kerb"},
    "Gravel": {"Friction": 1.5, "FrictionCombine": "Maximum", "RollingResistance": 0.25, "Surface": "Gravel"},
    "Grass": {"Friction": 0.5, "RollingResistance": 0.08, "Surface": "Grass"},
}

# The air's haze over the far ground and the hills: its color (the horizon's), how much of the meadow's own color each
# ring of the ground loses to it, and how bright it is when it has all of it.
HAZE_COLOR = (0.62, 0.72, 0.86)
HAZE_PER_RING = 0.06
HAZE_BRIGHTNESS = 0.4


def entity_ids(entities, path=()):
    """The IDs of a hierarchy's entities (scene_hierarchy's tree) by their names from the root: {(root, ..., name): id}.
    Where siblings share a name, the first keeps it."""
    ids = {}
    for entity in entities:
        key = path + (entity["name"],)
        ids.setdefault(key, entity["id"])
        ids.update({inner: found for inner, found in entity_ids(entity.get("children", []), key).items() if inner not in ids})
    return ids


class Moto:
    def __init__(self, client):
        self.client = client
        self.handles = {}
        # The IDs the entities a part makes again had, by their names from the root, and where its roots stood.
        self.previous_ids = {}
        self.root_indices = {}
        # The names from the root of the entities made in this session, by their IDs.
        self.paths = {}

    def call(self, tool, arguments=None):
        return self.client.call(tool, arguments)

    def entity(self, name, components, parent=None):
        """Creates an entity with the ID it had before the part made it again (a new one when it had none), a root where it
        stood among the roots."""
        path = (self.paths[parent] if parent is not None else ()) + (name,)
        arguments = {"name": name, "components": components}
        if path in self.previous_ids:
            arguments["id"] = self.previous_ids[path]
        if parent is not None:
            arguments["parent"] = parent
        elif name in self.root_indices:
            arguments["siblingIndex"] = self.root_indices[name]
        created = self.call("entity_create", arguments)["id"]
        self.paths[created] = path
        return created

    def entity_with_children(self, name, components, parent, children):
        """Creates an entity under the parent and its children ([(name, components)]) in one request, each with the ID it
        had before the part made it again (a new one when it had none); returns the entity's ID. Siblings' names are
        unique, so that a name from the root finds one entity."""
        path = self.paths[parent] + (name,)
        assert len({child for child, _ in children}) == len(children), f"{name}: children share names"
        arguments = {"name": name, "parent": parent, "components": components, "children": []}
        if path in self.previous_ids:
            arguments["id"] = self.previous_ids[path]
        for child, child_components in children:
            request = {"name": child, "components": child_components}
            if path + (child,) in self.previous_ids:
                request["id"] = self.previous_ids[path + (child,)]
            arguments["children"].append(request)
        created = self.call("entity_create", arguments)
        self.paths[created["id"]] = path
        for (child, _), child_id in zip(children, created["children"]):
            self.paths[child_id] = path + (child,)
        return created["id"]

    def material(self, name, fields):
        """The material at Materials/<name>.smat with the fields: created, or set when it exists (its handle stays). Set
        replaces only the fields given, so every material gives all the fields it changes from the defaults."""
        if os.path.exists(os.path.join(PROJECT, "Assets", "Materials", f"{name}.smat")):
            self.call("material_set", {"material": material_reference(name), "fields": fields})
        else:
            self.call("material_create", {"path": f"Materials/{name}.smat", "fields": fields})
        return material_reference(name)

    def physics_material(self, name, fields):
        """The physics material at Surfaces/<name>.sphysmat with the fields: created, or set when it exists (its handle
        stays). Set replaces only the fields given, so every material gives all the fields it changes from the defaults."""
        if os.path.exists(os.path.join(PROJECT, "Assets", "Surfaces", f"{name}.sphysmat")):
            self.call("physics_material_set", {"physicsMaterial": surface_reference(name), "fields": fields})
        else:
            self.call("physics_material_create", {"path": f"Surfaces/{name}.sphysmat", "fields": fields})
        return surface_reference(name)

    def remove_roots(self, names):
        """Deletes the main scene's root entities of these names (with their children), noting the IDs they and their
        descendants had and where the roots stood, so that the part makes them again as they were."""
        roots = self.call("scene_hierarchy")["entities"]
        removed = [entity for entity in roots if entity["name"] in names]
        self.previous_ids.update(entity_ids(removed))
        self.root_indices.update({entity["name"]: index for index, entity in enumerate(roots) if entity["name"] in names})
        if removed:
            self.call("entity_delete", {"entities": [entity["id"] for entity in removed]})

    # --- The parts -------------------------------------------------------------------------------------------------------

    def build_part(self, part):
        self.remove_roots(PARTS[part][0])
        {"circuit": self.make_circuit, "lighting": self.make_lighting, "camera": self.make_camera, "props": self.make_props,
         "bike": self.make_bike, "input": self.make_input, "sound": self.make_sound, "ghost": self.make_ghost,
         "trackside": self.make_trackside}[part]()

    def write_graphs(self, graphs, declare, write):
        """Writes graphs from scratch, keeping the handles of those that exist: declare(author) adds their interfaces and
        write(author) their regions."""
        for graph in graphs:
            self.handles[graph] = Graphs.prepare(self, PROJECT, graph, graph in Checks.LIBRARIES)
        author = Graphs.Author(self)
        declare(author)
        write(author)
        print(f"wrote {', '.join(graph[len('asset://'):] for graph in graphs)} in {author.batches} batches")

    def write_part_graphs(self, part):
        """Writes the graphs a part makes (GRAPH_GROUPS)."""
        for owner, graphs, declare, write in GRAPH_GROUPS:
            if owner == part:
                def write_all(author, graphs=graphs, write=write):
                    write(author)
                    for graph in graphs:
                        author.finish(graph)

                self.write_graphs(graphs, declare, write_all)

    def data_asset(self, kind, parameter, path, fields):
        """The data asset at Assets/<path> with all its fields: created, or set when it exists (its handle stays)."""
        reference = f"asset://{path}"
        if os.path.exists(os.path.join(PROJECT, "Assets", *path.split("/"))):
            self.call(f"{kind}_set", {parameter: reference, "fields": fields})
        else:
            self.call(f"{kind}_create", {"path": path, "fields": fields})
        return reference

    def root_ids(self):
        """The main scene's root entities' IDs, by their names."""
        return {entity["name"]: entity["id"] for entity in self.call("scene_hierarchy")["entities"]}

    def make_circuit(self):
        GenerateAssets.generate(os.path.join(PROJECT, "Assets"), GenerateAssets.SEED, groups=("circuit",))
        refreshed = self.call("asset_refresh")
        if refreshed.get("warnings"):
            raise SystemExit(f"the assets refreshed with warnings: {refreshed['warnings']}")
        self.make_materials()
        surfaces = {name: self.physics_material(name, fields) for name, fields in SURFACE_MATERIALS.items()}
        static = {"Type": "Static"}
        root = self.entity("Circuit", {"Transform": {"Translation": [0.0, 0.0, 0.0]}})
        track_materials = [material_reference(name) for name in GenerateAssets.TRACK_MATERIALS]
        # The collision mesh has a submesh per surface, each with its physics material.
        self.entity("Track", {
            "Mesh": {"Mesh": mesh("Track"), "Materials": track_materials},
            "RigidBody": static,
            "MeshCollider": {"Mesh": mesh("TrackCollision"), "Convex": False,
                             "Materials": [surfaces[name] for name in GenerateAssets.SURFACES]}}, root)
        self.entity("Barriers", {
            "Mesh": {"Mesh": mesh("Barriers"), "Materials": [material_reference("Concrete")]},
            "RigidBody": static,
            "MeshCollider": {"Mesh": mesh("BarrierCollision"), "Convex": False, "Friction": 0.4}}, root)
        # The ground and the hills lie beyond the shadows' reach and under everything: they cast none.
        self.entity("Ground", {
            "Mesh": {"Mesh": mesh("Ground"), "Materials": [material_reference(name) for name in GenerateAssets.GROUND_MATERIALS],
                     "CastShadows": False},
            "RigidBody": static,
            "MeshCollider": {"Convex": False, "Material": surfaces["Grass"]}}, root)
        hills = [material_reference(name) for name in GenerateAssets.HILLS_MATERIALS]
        self.entity("Hills", {"Mesh": {"Mesh": mesh("Hills"), "Materials": hills, "CastShadows": False}}, root)
        gantry = [material_reference(name) for name in ("GantrySteel", "GantryPanel", "StartLamp", "GantryAccent")]
        self.entity("Start Gantry", {"Transform": place(0.0), "Mesh": {"Mesh": mesh("Gantry"), "Materials": gantry},
                                     "RigidBody": static, "MeshCollider": {"Convex": False}}, root)
        # The timing lines: each gate's forward (-Z) points along the travel, its origin on the center line. The third is
        # the start and finish line, under the gantry.
        for number, s in enumerate(Circuit.SECTOR_GATES, start=1):
            components = {"Transform": place(s)}
            if s != 0.0:
                components.update({"Mesh": {"Mesh": mesh("Gate"), "Materials": [material_reference("GateSteel"),
                                                                                 material_reference(f"SectorBoard{number}")]},
                                   "RigidBody": static, "MeshCollider": {"Convex": False}})
            self.entity(f"Sector Gate {number}", components, root)
        self.entity("Footbridge", {"Transform": place(Circuit.FOOTBRIDGE),
                                   "Mesh": {"Mesh": mesh("Footbridge"), "Materials": [material_reference("Concrete"),
                                                                                      material_reference("BridgePanel")]},
                                   "RigidBody": static, "MeshCollider": {"Convex": False}}, root)

    def make_materials(self):
        material = self.material

        def textured(texture, tile, roughness, color=(1.0, 1.0, 1.0, 1.0)):
            """A material whose texture repeats every tile meters (the meshes' texture coordinates are in meters)."""
            size = tile if isinstance(tile, tuple) else (tile, tile)
            return {"BaseColor": list(color), "BaseColorTexture": f"asset://Textures/{texture}.png", "Roughness": roughness,
                    "UVTiling": [1.0 / size[0], 1.0 / size[1]]}

        material("Asphalt", textured("Asphalt", GenerateAssets.ASPHALT_TILE, 0.82))
        # The road around the timing lines: the asphalt with the lines painted on, mapped as the asphalt is.
        material("TimingLines", textured("TimingLines", GenerateAssets.ASPHALT_TILE, 0.82))
        material("Kerb", textured("Kerb", (Circuit.KERB_WIDTH, 2.0), 0.55))
        material("Grass", textured("Grass", 8.0, 0.95))
        material("Gravel", textured("Gravel", 3.0, 1.0))
        material("Concrete", textured("Concrete", 4.0, 0.9))
        # The meadow beyond the barriers, and its rings farther out: the air between lightens and blues them towards the
        # horizon's color, more with every ring, up to the haze of the hills' foot (there is no fog).
        material("Meadow", textured("Meadow", 16.0, 0.95))
        for ring, name in enumerate(GenerateAssets.GROUND_MATERIALS[1:], start=1):
            haze = HAZE_PER_RING * ring
            hazy = textured("Meadow", 16.0, 0.95, (1.0 - haze, 1.0 - haze, 1.0 - haze, 1.0))
            hazy.update({"EmissiveColor": list(HAZE_COLOR), "EmissiveIntensity": round(HAZE_BRIGHTNESS * haze, 4)})
            material(name, hazy)
        # The hills are kilometers away: the air between lightens and blues them (there is no fog), as the light the
        # material emits in the horizon's color.
        forest = textured("Forest", 60.0, 1.0)
        forest.update({"EmissiveColor": list(HAZE_COLOR), "EmissiveIntensity": 0.22})
        material("Forest", forest)
        material("GantrySteel", {"BaseColor": [0.09, 0.1, 0.12, 1.0], "Roughness": 0.45})
        material("GantryPanel", {"BaseColor": [0.02, 0.02, 0.025, 1.0], "Roughness": 0.35})
        material("GantryAccent", {"BaseColor": [0.72, 0.06, 0.04, 1.0], "Roughness": 0.5})
        # The start lights, unlit: dark red glass.
        material("StartLamp", {"BaseColor": [0.18, 0.01, 0.01, 1.0], "Roughness": 0.15, "EmissiveColor": [1.0, 0.06, 0.02],
                               "EmissiveIntensity": 0.0})
        material("GateSteel", {"BaseColor": [0.62, 0.63, 0.66, 1.0], "Roughness": 0.45})
        # Each sector board shows its cell of the texture ("S1", "S2"), lit from inside a little.
        for number in (1, 2):
            material(f"SectorBoard{number}", {
                "BaseColor": [1.0, 1.0, 1.0, 1.0], "BaseColorTexture": "asset://Textures/SectorBoards.png", "Roughness": 0.4,
                "EmissiveColor": [1.0, 1.0, 1.0], "EmissiveIntensity": 0.6, "EmissiveTexture": "asset://Textures/SectorBoards.png",
                "UVTiling": [1.0 / 3.0, 1.0], "UVOffset": [(number - 1) / 3.0, 0.0]})
        material("BridgePanel", {"BaseColor": [0.05, 0.2, 0.55, 1.0], "Roughness": 0.5})

    def make_props(self):
        """The props (Props.py places them, GenerateAssets.py makes their meshes and textures): their two materials, then a
        group under Props for each kind, made with all its props in one request."""
        GenerateAssets.generate(os.path.join(PROJECT, "Assets"), GenerateAssets.SEED, groups=("props",))
        refreshed = self.call("asset_refresh")
        if refreshed.get("warnings"):
            raise SystemExit(f"the assets refreshed with warnings: {refreshed['warnings']}")
        materials = {
            # The atlas of flat colors, figures and names that every prop but the fence's panels is drawn with.
            "Props": self.material("Props/Props", {"BaseColorTexture": "asset://Textures/Props/Props.png", "Roughness": 0.8}),
            # The chain-link, galvanized: its holes cut out, seen from both sides. Its wires cover about a third of the
            # texture, which is what its smaller images average to; the cutoff stays under that, so a fence far off reads as
            # a grey veil instead of vanishing.
            "Fence": self.material("Props/Fence", {"BaseColorTexture": "asset://Textures/Props/Fence.png", "Roughness": 0.45,
                                                   "Metallic": 0.6, "AlphaMode": "Mask", "AlphaCutoff": 0.3, "DoubleSided": True}),
        }
        props = Props.place()
        print("props:", Props.check(props))
        root = self.entity("Props", {"Transform": {"Translation": [0.0, 0.0, 0.0]}})
        for group, name, meshes in Props.GROUPS:
            members = [prop for prop in props if prop.mesh in meshes]
            children = []
            for number, prop in enumerate(members, start=1):
                transform = {"Translation": list(prop.position), "RotationEuler": [0.0, prop.yaw, 0.0]}
                if prop.scale != (1.0, 1.0, 1.0):
                    transform["Scale"] = list(prop.scale)
                children.append((f"{name} {number}", {
                    "Transform": transform,
                    "Mesh": {"Mesh": mesh(f"Props/{prop.mesh}"), "Materials": [materials[GenerateAssets.prop_material(prop.mesh)]]}}))
            self.entity_with_children(group, {}, root, children)

    def make_bike(self):
        """The motorcycle and its rider on the grid (Bike.py makes them): their materials, then the Bike entity with its
        parts, each where Bike.ENTITIES puts it."""
        GenerateAssets.generate(os.path.join(PROJECT, "Assets"), GenerateAssets.SEED, groups=("bike",))
        refreshed = self.call("asset_refresh")
        if refreshed.get("warnings"):
            raise SystemExit(f"the assets refreshed with warnings: {refreshed['warnings']}")
        print("bike:", Bike.check_grid())
        livery = "asset://Textures/Bike/Livery.png"
        materials = {
            # The bodywork's livery, the rims, the leathers and the helmet: lacquered.
            "Paint": self.material("Bike/Paint", {"BaseColorTexture": livery, "Roughness": 0.28}),
            # Rubber, the seat, the engine's castings, the grips, the gloves and the boots.
            "Dark": self.material("Bike/Dark", {"BaseColorTexture": livery, "Roughness": 0.8}),
            "Metal": self.material("Bike/Metal", {"BaseColorTexture": livery, "Metallic": 1.0, "Roughness": 0.3}),
            # The screen, the lights and the visor: smooth and opaque.
            "Glass": self.material("Bike/Glass", {"BaseColorTexture": livery, "Roughness": 0.05}),
        }
        translation, yaw = Bike.grid_place()
        made = {"Bike": self.entity("Bike", {"Transform": {"Translation": list(translation), "RotationEuler": [0.0, yaw, 0.0]}})}
        for name, mesh_name, parent, offset, angle in Bike.ENTITIES:
            made[name] = self.entity(name, {
                "Transform": {"Translation": list(offset), "RotationEuler": [angle, 0.0, 0.0]},
                "Mesh": {"Mesh": mesh(f"Bike/{mesh_name}"),
                         "Materials": [materials[material] for material in Bike.mesh_materials(mesh_name)]}}, made[parent])

    def make_sound(self):
        """The sounds (GenerateAudio.py) and the mixer they play through (Sound.py), which the project file names."""
        GenerateAudio.generate(os.path.join(PROJECT, "Assets"))
        # Made twice more in memory: the same bytes each time, and what was just written.
        GenerateAudio.check()
        refreshed = self.call("asset_refresh")
        if refreshed.get("warnings"):
            raise SystemExit(f"the assets refreshed with warnings: {refreshed['warnings']}")
        self.call("project_settings", {"settings": {"Mixer": self.data_asset("mixer", "mixer", Sound.MIXER, Sound.mixer())}})
        # The echo under the footbridge, and the audio director, whose graph the Pause action reaches (the input part named
        # the actions before).
        self.entity(Sound.ECHO, Sound.echo())
        self.write_part_graphs("sound")
        self.entity(Sound.DIRECTOR, {"VisualScript": {"Graph": self.handles[Sound.AUDIO_DIRECTOR]}})

    def make_input(self):
        """The game's input actions (Controls.py), which the project file names."""
        actions = self.data_asset("input_actions", "inputActions", Controls.INPUT_ACTIONS, Controls.input_actions())
        self.call("project_settings", {"settings": {"InputActions": actions}})

    def write_ghost_graphs(self):
        """The ghost's graph, whose timeline holds the racing line's keys (Ghost.py, RacingLine.py), and its engine sound's,
        with the curves it crossfades the layers through (EngineSound.py)."""
        print("line:", RacingLine.check())
        print("engine sound:", EngineSound.check())
        for path, fields in EngineSound.curves().items():
            self.data_asset("curve", "curve", path, fields)
        self.write_part_graphs("ghost")

    def make_ghost(self):
        """The ghost bike: the bike part's meshes and materials under the Ghost entity, at the line's pose where it sets
        out, with its graph, which rides it round the racing line, and its engine's sound: the Engine Sound entity under
        it, with a voice for each layer."""
        self.write_ghost_graphs()
        made = {"Bike": self.entity(Ghost.ENTITY, {"Transform": Ghost.transform(),
                                                   "VisualScript": {"Graph": self.handles[Ghost.GHOST]}})}
        for name, mesh_name, parent, offset, angle in Bike.ENTITIES:
            materials = [material_reference(f"Bike/{material}") for material in Bike.mesh_materials(mesh_name)]
            made[name] = self.entity(name, {"Transform": {"Translation": list(offset), "RotationEuler": [angle, 0.0, 0.0]},
                                            "Mesh": {"Mesh": mesh(f"Bike/{mesh_name}"), "Materials": materials}}, made[parent])
        self.entity_with_children(EngineSound.ENTITY, {"Transform": {"Translation": list(EngineSound.OFFSET)},
                                                       "VisualScript": {"Graph": self.handles[EngineSound.ENGINE_SOUND]}},
                                  made["Bike"], [(name, {"AudioSource": EngineSound.voice(clip)}) for name, clip in EngineSound.VOICES])

    def make_trackside(self):
        """The trackside cameras (Trackside.py), children of the Trackside entity, which runs the camera director: it cuts
        between them and the main camera's tour, and they follow the ghost."""
        placed = Trackside.placing()
        print("trackside:", "; ".join(f"{name} at {position}" for name, position in placed))
        self.write_part_graphs("trackside")
        roots = self.root_ids()
        missing = [name for name in ("Main Camera", Ghost.ENTITY) if name not in roots]
        if missing:
            raise SystemExit(f"the trackside part follows the camera and ghost parts: the scene has no {', '.join(missing)}")
        root = self.entity(Trackside.ROOT, {"Transform": {"Translation": [0.0, 0.0, 0.0]}, "VisualScript": {
            "Graph": self.handles[Trackside.CAMERA_DIRECTOR], "Variables": {
                "TourCamera": {"Type": "Entity", "Value": roots["Main Camera"]},
                "Subject": {"Type": "Entity", "Value": roots[Ghost.ENTITY]}}}})
        for name, position in placed:
            self.entity(name, Trackside.camera(position), root)

    def make_lighting(self):
        self.call("scene_settings", {"name": "Main", "settings": {"Renderer": {
            "Tonemapper": "ACES", "EV100": 0.0, "Shadows": True, "SoftShadows": True, "ShadowDistance": 220.0, "CascadeCount": 4,
            "CascadeSplitLambda": 0.85, "ShadowMapSize": 4096, "AmbientOcclusion": True, "AmbientOcclusionRadius": 1.0,
            "AmbientOcclusionIntensity": 1.2, "Bloom": True, "BloomIntensity": 0.03, "FXAA": True}}})
        self.entity("Sun", {"Transform": {"Translation": [0.0, 50.0, 0.0], "RotationEuler": sun_euler()},
                            "DirectionalLight": {"Color": [1.0, 0.95, 0.88], "Intensity": 6.0, "CastShadows": True, "LightSize": 0.53}})
        self.entity("Sky", {"SkyLight": {"Environment": "asset://Environments/Sky.hdr", "Intensity": 1.2, "DrawSkybox": True}})

    def make_camera(self):
        """The main camera at the home view, over the inside of the main straight before the line, looking past the gantry
        down the straight, and its tour. Its near plane suits a rider's view, its far plane takes in the hills (the
        projection itself has none)."""
        print("tour:", Tour.check())

        def write(author):
            Tour.write(author)
            author.finish(Tour.TOUR)

        self.write_graphs((Tour.TOUR,), Tour.declare, write)
        self.entity("Main Camera", {"Transform": look(Tour.HOME_EYE, Tour.HOME_TARGET),
                                    "Camera": {"PerspectiveFOV": 50.0, "PerspectiveNear": 0.1, "PerspectiveFar": 5000.0},
                                    "AudioListener": {}, "VisualScript": {"Graph": self.handles[Tour.TOUR]}})
        # Every graph is written again, those of the parts that have made theirs too (their own files, such as the engine
        # sound's curves, stay theirs): each keeps its handle, so the entities that run it stay as they are.
        for part, graphs, _, _ in GRAPH_GROUPS:
            if part != "camera" and all(os.path.exists(os.path.join(PROJECT, "Assets", *graph[len("asset://"):].split("/")))
                                        for graph in graphs):
                self.write_part_graphs(part)

    def make_tests(self):
        """The test scene and its graphs, from the main scene as saved; then every graph is checked and the checks run."""
        if Tour.TOUR not in self.handles:
            self.handles[Tour.TOUR] = self.call("graph_get", {"graph": Tour.TOUR, "analyze": False})["asset"]
        self.write_graphs(Checks.GRAPHS, Checks.declare, Checks.write)
        Graphs.verify(self, GRAPHS)
        Checks.build_scene(self, self.handles, PROJECT)
        expected = len(Checks.checks())
        run = self.call("test_run", {"scene": "Scenes/Tests.sscene", "timeout": math.ceil(Tour.LENGTH) + 30})
        print("test run:", {key: run[key] for key in ("finished", "timedOut", "frames", "passed", "failed", "exceptions")})
        if not run["finished"] or run["failed"] or run["exceptions"] or run["passed"] != expected:
            for result in run["results"]:
                if not result["passed"]:
                    print(f"  failed: {result['name']}: {result['message']}")
            raise SystemExit(f"the test run passed {run['passed']} of {expected} checks")

    def check_tour(self):
        """Plays the main scene for ten seconds and checks that the tour put the camera where its path is then, and the
        ghost (once the ghost part has made it) where its line is then."""
        frames = 600
        self.call("play_start", {"paused": True})
        try:
            roots = {entity["name"]: entity["id"] for entity in self.call("scene_hierarchy")["entities"]}
            self.call("play_advance", {"frames": frames})

            def translation(name):
                return self.call("component_get", {"entity": roots[name], "component": "Transform"})["fields"]["Translation"]

            at = translation("Main Camera")
            ghost = translation(Ghost.ENTITY) if Ghost.ENTITY in roots else None
        finally:
            self.call("play_stop")
        # The tour's time after the frames is a frame either side of theirs, and so is the ghost's.
        seconds = frames / 60.0
        closest = min(math.dist(at, Tour.camera(seconds + k / 60.0)[0]) for k in (-1, 0, 1))
        print(f"  the tour's camera after {seconds:.0f} s: {closest:.2f} m from its path")
        if closest > 0.05:
            raise SystemExit(f"the tour does not play in play mode: the camera is at {at}, {closest:.2f} m from its path")
        if ghost is not None:
            closest = min(math.dist(ghost, RacingLine.pose(Ghost.START_AT + seconds + k / 60.0)[0]) for k in (-1, 0, 1))
            print(f"  the ghost after {seconds:.0f} s: {closest:.2f} m from its line")
            if closest > 0.1:
                raise SystemExit(f"the ghost does not ride in play mode: it is at {ghost}, {closest:.2f} m from its line")

    # --- Checks ----------------------------------------------------------------------------------------------------------

    def check_surfaces(self):
        """Drops a sphere above each kind of surface while playing and checks that it comes to rest on it: the colliders are
        there, face up and match the drawn surfaces. The spheres live in play mode's copy of the scene only."""
        radius = 0.3
        apex = next(start + 0.5 * (end - start) for corner, start, end, side in Circuit.corner_ranges() if corner.name == "Turn 6")
        inside = 1 if Circuit.curvature(apex) < 0.0 else -1
        kerb = (Circuit.HALF_WIDTH + Circuit.KERB_WIDTH - 0.3) * inside
        trap = Circuit.HALF_WIDTH + Circuit.KERB_WIDTH + Circuit.VERGE_WIDTH + 0.5 * Circuit.gravel(apex, "left" if inside > 0 else "right")
        verge = Circuit.HALF_WIDTH + Circuit.KERB_WIDTH + 1.5
        face = Circuit.barrier_offset(Circuit.FOOTBRIDGE + 30.0, "right")
        bridge = Circuit.offset_point(Circuit.FOOTBRIDGE, 0.0)
        probes = [
            ("road on the main straight", Circuit.offset_point(300.0, 2.0), Circuit.road_height(2.0)),
            ("road at Turn 1", Circuit.offset_point(600.0, -1.0), Circuit.road_height(-1.0)),
            ("kerb at Turn 6", Circuit.offset_point(apex, kerb), Circuit.ROAD_HEIGHT + Circuit.KERB_HEIGHT),
            ("gravel at Turn 6", Circuit.offset_point(apex, -trap * inside), Circuit.GRAVEL_HEIGHT),
            ("verge on the back straight", Circuit.offset_point(Circuit.FOOTBRIDGE - 40.0, -verge), None),
            ("ground beyond the barrier", Circuit.offset_point(Circuit.FOOTBRIDGE + 30.0, face + 6.0), 0.0),
            ("barrier top", Circuit.offset_point(Circuit.FOOTBRIDGE + 30.0, face + 0.3), Circuit.BARRIER_HEIGHT),
            ("footbridge deck", (bridge[0], 0.0, bridge[2]), Circuit.BRIDGE_CLEARANCE + 0.5),
        ]
        self.call("play_start", {"paused": True})
        try:
            ids = []
            for name, point, surface in probes:
                drop = (surface if surface is not None else Circuit.VERGE_HEIGHT) + 1.5
                ids.append(self.entity(f"Probe: {name}", {
                    "Transform": {"Translation": [point[0], drop, point[2]]},
                    "RigidBody": {"Type": "Dynamic", "Mass": 5.0, "LinearDamping": 4.0, "AngularDamping": 4.0},
                    "SphereCollider": {"Radius": radius, "Friction": 1.0}}))
            self.call("play_advance", {"frames": 150})
            failures = []
            for (name, point, surface), entity in zip(probes, ids):
                rest = self.call("component_get", {"entity": entity, "component": "Transform"})["fields"]["Translation"]
                expected = (surface if surface is not None else Circuit.VERGE_HEIGHT) + radius
                drift = math.hypot(rest[0] - point[0], rest[2] - point[2])
                # A resting body sinks up to Jolt's penetration slop (0.02 m) into what it rests on; a verge's height
                # changes across it.
                tolerance = 0.03 if surface is not None else 0.12
                print(f"  {name}: rests at {rest[1]:.3f} m (expected {expected:.3f} m), {drift:.2f} m from where it fell")
                if abs(rest[1] - expected) > tolerance or drift > 1.0:
                    failures.append(name)
        finally:
            self.call("play_stop")
        if failures:
            raise SystemExit("spheres did not rest on their surfaces: " + ", ".join(failures))

    def read_log(self, since, level):
        """The log's entries at or above a level from an index on, all of them; returns (entries, the index after the last)."""
        entries = []
        while True:
            read = self.call("log_read", {"since": since, "minLevel": level, "maxCount": 1000})
            since = read["next"]
            if not read["entries"]:
                return entries, since
            entries += read["entries"]

    def check_log(self, since):
        """Nothing the build did logged a warning or an error."""
        entries, _ = self.read_log(since, "warn")
        if entries:
            raise SystemExit("the build logged problems:\n  " + "\n  ".join(entry["message"] for entry in entries))

    def log_position(self):
        """The index after the log's last entry: what is logged from now on comes after it."""
        return self.read_log(0, "critical")[1]

    # --- The project -----------------------------------------------------------------------------------------------------

    def build(self):
        since = self.log_position()
        self.call("project_settings", {"settings": {"Name": NAME, "Window": {"Title": NAME, "Width": 1600, "Height": 900}}})
        found = [entity["id"] for entity in self.call("scene_hierarchy")["entities"]]
        self.call("entity_delete", {"entities": found})
        self.call("scene_settings", {"name": "Main"})
        for part in SCENE_PARTS:
            self.build_part(part)
        self.finish(since)

    def build_in_place(self, part):
        self.call("project_open", {"path": os.path.join(PROJECT, "Moto.sproj")})
        since = self.log_position()
        self.call("scene_open", {"path": "Scenes/Main.sscene"})
        if part != "tests":
            self.build_part(part)
        self.finish(since)

    def finish(self, since):
        """Saves the main scene, checks its surfaces, makes the tests part, plays the tour and checks the log."""
        self.call("scene_save")
        self.check_surfaces()
        self.make_tests()
        self.check_tour()
        self.check_log(since)


def main():
    parser = argparse.ArgumentParser(description="Builds the Moto project through strada mcp, or one of its parts in place.")
    parser.add_argument("strada", nargs="?", help="the strada executable (default: dotnet run of the repository's StradaTool)")
    parser.add_argument("--part", choices=ORDER, help="build this part (and the tests part after it) in place, leaving every "
                        "file they do not own as it is")
    parser.add_argument("--captures", metavar="FOLDER", help="build nothing: take the player's screenshots from the fixed "
                        "cameras (Captures.py) into this folder")
    parser.add_argument("--player", help="the StradaRuntime executable that takes the captures")
    parser.add_argument("--views", nargs="+", help="with --captures, only these views")
    parser.add_argument("--sounds", action="store_true", help="with --captures, also record the ghost passing each trackside "
                        "camera of the views taken")
    arguments = parser.parse_args()
    if arguments.captures:
        if not arguments.player or arguments.part:
            parser.error("--captures takes --player, and no --part")
        Captures.take(os.path.abspath(arguments.player), os.path.abspath(arguments.captures), arguments.views, arguments.sounds)
        return
    if arguments.player or arguments.views or arguments.sounds:
        parser.error("--player, --views and --sounds go with --captures")
    command = [os.path.abspath(arguments.strada)] if arguments.strada else \
        ["dotnet", "run", "--project", os.path.join(ROOT, "StradaTool"), "-c", "Release", "--no-launch-profile", "--"]
    if arguments.part:
        build_part_in_place(command, arguments.part)
    else:
        rebuild(command)


def build_part_in_place(command, part):
    before = hash_project()
    try:
        client = McpClient(command + ["mcp", "--headless", "--new"])
        try:
            Moto(client).build_in_place(part)
        finally:
            client.close()
    except BaseException:
        # The failure goes on as it is, after the files it changed that the part does not own are named.
        foreign = foreign_changes(before, part)
        if foreign:
            print("building the part changed files it does not own: " + ", ".join(foreign), file=sys.stderr)
        raise
    foreign = foreign_changes(before, part)
    if foreign:
        raise SystemExit("building the part changed files it does not own: " + ", ".join(foreign))


def rebuild(command):
    # The editor starts before anything is touched: a tool that cannot run leaves the project as it is.
    client = McpClient(command + ["mcp", "--headless", "--new"])
    # The editor creates projects in empty directories only: this folder waits outside meanwhile.
    aside = tempfile.mkdtemp(prefix="moto-author-")
    shutil.move(TOOLS, os.path.join(aside, "Tools"))
    try:
        if os.path.isdir(PROJECT):
            shutil.rmtree(PROJECT)
        client.call("project_create", {"directory": PROJECT, "name": "Moto", "gitIgnore": False, "agents": False})
    finally:
        shutil.move(os.path.join(aside, "Tools"), TOOLS)
        os.rmdir(aside)
    try:
        Moto(client).build()
    finally:
        client.close()


if __name__ == "__main__":
    main()

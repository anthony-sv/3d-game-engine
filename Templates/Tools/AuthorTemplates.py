"""Builds the project templates through `strada mcp`, the way an agent does. Both are the game Orb Run: roll a glowing orb
with the keyboard or a gamepad and collect 12 sparks before the time runs out, in the same arena, with the same screen.
graph-starter is made of visual scripts alone; csharp-starter is written in C# (the assembly and namespace Game, whose
sources are kept in Templates/csharp-starter/Scripts/Source and written by hand), and one small graph calls a C# member
marked [VisualNode]. In order:

1. the project (Template.sproj, named Orb Run) and its settings; for csharp-starter, the C# project the editor makes, the
   sources put in its place, the engine's repository-only fallback marked in Game.csproj, and the scripts built; then the
   generated sounds, spark mesh and textures (GenerateAssets.py), and the materials;
2. the graphs (for graph-starter, the Checks function library first), declared first: one graph_edit batch per graph adds
   its variables, custom events and functions, so the graphs that send to, read from and call each other never meet a
   missing event;
3. the bodies, one graph_edit batch per region of a graph: the region's nodes and links, arranged (scope Created, then
   Nodes) and framed by a comment box titled for what it does, in its role's colour. Every batch must answer no error, no
   warning and no hint, or the tool stops;
4. the prefabs, the main scene and the test scene, whose entities run the graphs or the C# scripts;
5. the editor closes, and the IDs the engine drew at random (asset handles and entity IDs) are replaced by IDs derived from
   what they name (an asset's path, an entity's place in its file), so two runs write the same bytes; then the template's
   template.json;
6. checks of the files as written, in a new editor session: every graph analyzes without a diagnostic and every node
   resolves exactly, each box stays arranged, the test scene's checks pass, the keys roll the orb, the clock runs out and
   Space starts a new run, nothing the template ships changes, and saving each scene again writes the same bytes.

What building the scripts writes (Scripts/Binaries, Scripts/Strada.props, Scripts/bin and Scripts/obj, which the project
copy filter leaves out of every new project) is removed at the end.

    python Templates/Tools/AuthorTemplates.py [--template <id>]... [--strada <strada executable>] [--output <folder>]
                                              [--thumbnail]

The template is rebuilt from scratch in Templates/<id> (or --output): everything but its thumbnail and its C# sources is
replaced. --thumbnail takes the thumbnail again with the player's --screenshot. Without --strada, strada runs from the
repository (dotnet run).
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

TOOLS = os.path.dirname(os.path.abspath(__file__))
TEMPLATES = os.path.dirname(TOOLS)
ROOT = os.path.dirname(TEMPLATES)


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


# --- Writing graphs: one graph_edit batch per region ---------------------------------------------------------------------

class Pin(tuple):
    """An output or input of a node: (node reference, pin name)."""


class Node:
    """A node of a batch: added by it (a "$label") or one the document had (an ID)."""

    def __init__(self, reference, output=None):
        self.reference = reference
        self.output = output

    def __getitem__(self, pin):
        return Pin((self.reference, pin))

    @property
    def out(self):
        if self.output is None:
            raise SystemExit(f"node {self.reference} has no single output")
        return Pin((self.reference, self.output))


class Region:
    """The nodes and links of one region: added and connected in one batch, arranged beside what they continue, laid out
    left to right and framed by a comment box."""

    def __init__(self, author, graph, title, color, function=None):
        if len(title) > 72:
            raise SystemExit(f"the region title '{title}' is longer than 72 characters")
        self.author = author
        self.graph = graph
        self.title = title
        self.color = color
        self.function = function
        self.additions = []
        self.connections = []
        self.after = []
        self.framed = []
        self.count = 0
        self.shared = {}
        if function is not None:
            entry, ret = author.declared[graph]["functions"][function]
            self.entry = self.existing(entry, "Then")
            self.ret = self.existing(ret)

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        if kind is None:
            self.author.write_region(self)
        return False

    def existing(self, node_id, output=None):
        self.framed.append(node_id)
        return Node(node_id, output)

    def label(self):
        self.count += 1
        return f"$n{self.count}"

    def link(self, source, target):
        if isinstance(source, Node):
            source = source.out
        self.connections.append({"op": "Connect", "from": list(source), "to": list(target)})

    def add(self, type_id, settings=None, output=None, comment=None, **inputs):
        label = self.label()
        operation = {"op": "AddNode", "type": type_id, "as": label}
        if settings:
            operation["settings"] = settings
        literals = {}
        for pin, value in inputs.items():
            if isinstance(value, (Pin, Node)):
                self.link(value, (label, pin))
            elif value is not None:
                literals[pin] = value
        if literals:
            operation["literals"] = literals
        self.additions.append(operation)
        if comment:
            self.after.append({"op": "SetNodeComment", "nodes": [label], "text": comment})
        self.framed.append(label)
        return Node(label, output)

    # Flow.
    def flow(self, source, target):
        """Links an exec output (a node's Then, or a pin) to an exec input (a node's Execute, or a pin)."""
        if isinstance(source, Node):
            source = source["Then"]
        if isinstance(target, Node):
            target = target["Execute"]
        self.connections.append({"op": "Connect", "from": list(source), "to": list(target)})

    def chain(self, *items):
        """Runs the items one after another; returns the last."""
        for first, second in zip(items, items[1:]):
            self.flow(first, second)
        return items[-1]

    def branch(self, condition, comment=None):
        return self.add("Flow.Branch", comment=comment, Condition=condition)

    # Values.
    def expr(self, formula, **inputs):
        return self.add("Expression", {"Formula": formula}, "Result", **inputs).out

    def get(self, variable, graph=None, target=None):
        settings = {"Variable": variable}
        if graph is not None:
            settings["Graph"] = graph
        return self.add("Variable.Get", settings, "Value", Target=target).out

    def set(self, variable, value=None, graph=None, target=None, comment=None):
        settings = {"Variable": variable}
        if graph is not None:
            settings["Graph"] = graph
        return self.add("Variable.Set", settings, "Value", comment=comment, Value=value, Target=target)

    def lit(self, type_id, value):
        return self.add("Literal", {"Type": type_id, "Value": value}, "Value").out

    def pure(self, type_id, output="Result", **inputs):
        return self.add(type_id, None, output, **inputs).out

    def call(self, type_id, output=None, comment=None, **inputs):
        return self.add(type_id, None, output, comment=comment, **inputs)

    def equal(self, a, b):
        return self.pure("Operator.Equal", A=a, B=b)

    def not_equal(self, a, b):
        return self.pure("Operator.NotEqual", A=a, B=b)

    def both(self, a, b):
        return self.pure("Operator.And", A=a, B=b)

    def negate(self, a):
        return self.pure("Operator.Not", A=a)

    def select(self, condition, when_true, when_false):
        """Select's True and False are named like Python's constants, so they are linked here rather than passed by name."""
        node = self.add("Select", None, "Result", Condition=condition)
        self.link(when_true, node["True"])
        self.link(when_false, node["False"])
        return node.out

    def fmt(self, text, **inputs):
        return self.add("FormatString", {"Format": text}, "Result", **inputs).out

    def self_(self):
        return self.add("Self", None, "Self").out

    def runs(self, graph, target):
        return self.add("RunsGraph", {"Graph": graph}, "Result", Target=target).out

    def element(self, array, index):
        return self.pure("Array.Get", "Element", Array=array, Index=index)

    def length(self, array):
        return self.pure("Array.Length", "Length", Array=array)

    def children(self, entity=None):
        return self.pure("Get:Strada.Entity.Children", "Value", Target=entity)

    def position(self, entity=None):
        return self.pure("Get:Strada.Entity.Translation", "Value", Target=entity)

    # Calls, events and messages.
    def library(self, function, graph, output=None, **inputs):
        return self.add("Function.Call", {"Graph": graph, "Function": function}, output, **inputs)

    def own(self, function, output=None, **inputs):
        return self.add("Function.Call", {"Function": function}, output, **inputs)

    def event(self, type_id, settings=None):
        return self.add(type_id, settings)

    def custom(self, name):
        """The region's custom event: written anew in place of the declare pass's node, so arranging places it with the
        region (the declaration stood for the interface until now; senders name events, not nodes)."""
        node_id, parameters = self.author.declared[self.graph]["events"].pop(name)
        self.additions.insert(0, {"op": "Delete", "elements": [node_id], "bridge": False})
        return self.add("Event.Custom", {"Name": name, "Parameters": parameters})

    def send(self, name, target, graph=None, comment=None, **arguments):
        settings = {"Name": name}
        if graph is not None:
            settings["Graph"] = graph
        return self.add("Event.Send", settings, comment=comment, Target=target, **arguments)

    def broadcast(self, name, graph=None, parameters=None, comment=None, **arguments):
        settings = {"Name": name}
        if graph is not None:
            settings["Graph"] = graph
        if parameters is not None:
            settings["Parameters"] = [{"Name": key, "Type": value_type} for key, value_type in parameters]
        return self.add("Event.Broadcast", settings, "Count", comment=comment, **arguments)

    def timeline(self, length, tracks, loop=False):
        return self.add("Flow.Timeline", {"Timeline": {"Length": length, "Loop": loop, "Tracks": tracks}})

    def operations(self):
        framed = list(dict.fromkeys(self.framed))
        return (self.additions + self.connections + self.after + [
            {"op": "Arrange", "scope": "Created"},
            {"op": "Arrange", "scope": "Nodes", "nodes": framed},
            {"op": "AddComment", "text": self.title, "elements": framed, "color": self.color},
        ])


def track(name, type_id, keys, interpolation="Linear"):
    """A timeline track: keys are (time, value)."""
    return {"Name": name, "Type": type_id, "Keys": [{"Time": time, "Value": value, "Interpolation": interpolation} for time, value in keys]}


class Author:
    """Writes the graphs: their declarations, then their regions, checking every batch."""

    def __init__(self, client):
        self.client = client
        self.handles = {}
        self.declared = {}
        self.pending = {}
        self.batches = 0

    def edit(self, graph, description, operations, function=None):
        arguments = {"graph": graph, "description": description, "operations": operations, "refuseNewErrors": True}
        if function is not None:
            arguments["function"] = function
        result = self.client.call("graph_edit", arguments)
        summary = result["summary"]
        if summary["errors"] or summary["warnings"] or summary["brokenReferrers"] or summary["staleSenders"]:
            problems = [f"{d['Severity']} {d['Code']}: {d['Message']}" for d in result["diagnostics"]]
            raise SystemExit(f"{description} in {graph}: {summary}\n  " + "\n  ".join(problems))
        if result["hints"]:
            hints = [f"{hint['kind']}: {hint['message']}" for hint in result["hints"]]
            raise SystemExit(f"{description} in {graph} has hints:\n  " + "\n  ".join(hints))
        self.batches += 1
        return result

    def declare(self, graph, description, variables=(), events=(), functions=()):
        """The graph's interface: variables (name, type, value, public, tooltip, range), custom events (name, [(parameter,
        type)]) and functions (name, pure, inputs, outputs)."""
        operations = [{"op": "SetDescription", "description": description}]
        for name, type_id, value, is_public, tooltip, value_range in variables:
            if is_public and not tooltip:
                raise SystemExit(f"the public variable {name} of {graph} has no tooltip")
            operation = {"op": "AddVariable", "name": name, "type": type_id, "public": is_public}
            if value is not None:
                operation["value"] = value
            if tooltip:
                operation["tooltip"] = tooltip
            if value_range is not None:
                operation["range"] = list(value_range)
            operations.append(operation)
        for index, (name, parameters) in enumerate(events):
            operations.append({"op": "AddNode", "type": "Event.Custom", "as": f"$event{index}",
                               "settings": {"Name": name, "Parameters": [{"Name": p, "Type": t} for p, t in parameters]}})
        for index, (name, pure, inputs, outputs) in enumerate(functions):
            operations.append({"op": "AddFunction", "name": name, "pure": pure,
                               "inputs": [{"name": p, "type": t} for p, t in inputs],
                               "outputs": [{"name": p, "type": t} for p, t in outputs],
                               "entryAs": f"$entry{index}", "returnAs": f"$return{index}"})
        result = self.edit(graph, "Declare the interface", operations)
        labels = result["labels"]
        self.declared[graph] = {
            "events": {name: (labels[f"$event{index}"], [{"Name": p, "Type": t} for p, t in parameters])
                       for index, (name, parameters) in enumerate(events)},
            "functions": {name: (labels[f"$entry{index}"], labels[f"$return{index}"]) for index, (name, *_) in enumerate(functions)},
        }

    def region(self, graph, title, color, function=None):
        return Region(self, graph, title, color, function)

    def write_region(self, region):
        self.pending.setdefault(region.graph, []).append(region)

    def finish(self, graph):
        """Writes the graph's regions: its functions first, in the order they are declared (each before the regions that
        call it), then the event graph's regions in the order they were written."""
        regions = self.pending.pop(graph, [])
        order = list(self.declared[graph]["functions"])
        functions = sorted((region for region in regions if region.function is not None), key=lambda region: order.index(region.function))
        for region in functions + [region for region in regions if region.function is None]:
            self.edit(region.graph, region.title, region.operations(), region.function)
        left = self.declared[graph]["events"]
        if left:
            raise SystemExit(f"{graph} declares custom events no region handles: {', '.join(left)}")


def public(name, type_id, value, tooltip, value_range=None):
    return (name, type_id, value, True, tooltip, value_range)


def private(name, type_id, value=None):
    return (name, type_id, value, False, None, None)


# --- Orb Run: the game ------------------------------------------------------------------------------------------------------

def script(name):
    return f"asset://Scripts/{name}.sgraph"


CHECKS = script("Checks")
GAME = script("Game")
ORB = script("Orb")
SPARK = script("Spark")
HUD = script("Hud")
CAMERA_RIG = script("CameraRig")
TESTS = script("Tests")
LIBRARIES = (CHECKS,)
# The order the graphs are written in: the library first, then each graph before the graphs that read or call it most.
ORDER = (CHECKS, ORB, SPARK, GAME, HUD, CAMERA_RIG, TESTS)
# csharp-starter's one graph, on every spark: it floats the gem where the spark's C# Hover, a [VisualNode] member, puts it.
HOVER = script("Hover")
HOVER_NODE = "Call:Game.Spark.Hover(float)"

# Comment boxes are coloured by role.
EVENTS = [0.2, 0.42, 0.95, 0.22]
RULES = [0.95, 0.55, 0.15, 0.22]
EFFECTS = [0.62, 0.3, 0.95, 0.22]
CHECK = [0.45, 0.6, 0.7, 0.22]

ENTITY = "Strada.Entity"
IDENTITY = [0.0, 0.0, 0.0, 1.0]

# The arena (metres, y up, the camera looking towards -z): a square floor inside four low walls.
HALF = 11.0
WALL = 0.6
WALL_HEIGHT = 1.4
ORB_START = [0.0, 0.5, 0.0]
SPARK_HEIGHT = 0.9
TIME_LIMIT = 35.0
CAMERA_HOME = [0.0, 17.5, 15.0]
CAMERA_LOOK = [0.0, 0.0, 1.5]


def spark_spots():
    """Twelve spots: eight round the edge of the arena and four nearer the middle, so a run sweeps the whole floor."""
    spots = []
    for index in range(8):
        angle = math.radians(22.5 + 45.0 * index)
        spots.append([round(8.0 * math.cos(angle), 2), SPARK_HEIGHT, round(8.0 * math.sin(angle), 2)])
    for index in range(4):
        angle = math.radians(90.0 * index)
        spots.append([round(4.0 * math.cos(angle), 2), SPARK_HEIGHT, round(4.0 * math.sin(angle), 2)])
    return spots


SPOTS = spark_spots()


def declare_all(author):
    author.declare(CHECKS, "What the test scene's checks share: reporting a check and comparing numbers and points.", functions=(
        ("Check", False, (("Name", "string"), ("Passed", "bool"), ("Detail", "string")), ()),
        ("Near", True, (("A", "float"), ("B", "float"), ("Tolerance", "float")), (("Result", "bool"),)),
        ("NearPoint", True, (("A", "Strada.Vector3"), ("B", "Strada.Vector3"), ("Tolerance", "float")), (("Result", "bool"),)),
    ))
    author.declare(ORB, "The orb: the keys or the left stick roll it during a run, it hums with its speed, and walls thud.",
                   variables=(
        public("Push", "float", 12.0, "The force the controls push the orb with, in newtons", (0.0, 100.0)),
        public("DeadZone", "float", 0.2, "How far the stick must lean before it pushes", (0.0, 0.9)),
        public("Hum", ENTITY, None, "The child whose looping sound hums with the orb's speed"),
        public("Floor", ENTITY, None, "The floor the orb rolls on without a thud"),
        public("Rolling", "bool", True, "Read-out: the controls push the orb only during a run"),
    ), events=(("StateChanged", (("State", "string"),)),), functions=(
        ("Steer", True, (), (("Direction", "Strada.Vector3"),)),
    ))
    author.declare(SPARK, "A spark: it bobs and spins above its spot, and the rolling orb collects it.", variables=(
        public("Burst", "Strada.Prefab", None, "The flash of light a collected spark leaves"),
        public("Spin", "float", 120.0, "Degrees per second the gem turns", (0.0, 720.0)),
        public("Bob", "float", 0.18, "Metres the gem rises and falls", (0.0, 1.0)),
        private("Clock", "float", 0.0),
    ))
    author.declare(GAME, "The run: it places the sparks, runs the clock, counts the sparks the orb collects, and ends in a win "
                   "or a loss that Space, Enter or a gamepad's A or Start plays again.", variables=(
        public("SparkPrefab", "Strada.Prefab", None, "The spark a run places at every spot"),
        public("Sparks", ENTITY, None, "The container of the run's sparks"),
        public("Orb", ENTITY, None, "The orb the player rolls"),
        public("PickupSound", ENTITY, None, "The sound of a collected spark"),
        public("WinSound", ENTITY, None, "The sound of a won run"),
        public("LoseSound", ENTITY, None, "The sound of a run whose time ran out"),
        public("TimeLimit", "float", TIME_LIMIT, "Seconds a run lasts", (5.0, 600.0)),
        public("Start", "Strada.Vector3", ORB_START, "Where the orb starts a run"),
        public("State", "string", "Play", "Read-out: Play during a run, then Won or Lost"),
        public("Collected", "int", 0, "Read-out: the sparks collected in this run", (0, 1000)),
        public("Total", "int", len(SPOTS), "Read-out: the sparks a run places", (0, 1000)),
        public("TimeLeft", "float", TIME_LIMIT, "Read-out: the seconds left in this run", (0.0, 600.0)),
        private("Spots", "Strada.Vector3[]", SPOTS),
    ), events=(("SparkCollected", ()), ("Restart", ())), functions=(
        ("PlaceSparks", False, (), ()),
        ("StartRun", False, (), ()),
        ("Win", False, (), ()),
        ("Lose", False, (), ()),
    ))
    author.declare(HUD, "The screen: the sparks and the seconds left, the hint at the start of a run, and the win and lose "
                   "screens with the prompt to play again.", variables=(
        public("Game", ENTITY, None, "The entity whose Game.sgraph the screen reads"),
        public("SparksText", ENTITY, None, "The text that counts the sparks"),
        public("TimeText", ENTITY, None, "The text that shows the seconds left"),
        public("Hint", ENTITY, None, "The goal and the controls, shown at the start of a run"),
        public("WinScreen", ENTITY, None, "The screen of a won run"),
        public("WinDetail", ENTITY, None, "The win screen's line with the seconds to spare"),
        public("LoseScreen", ENTITY, None, "The screen of a run whose time ran out"),
        public("LoseDetail", ENTITY, None, "The lose screen's line with the sparks collected"),
        public("Prompt", ENTITY, None, "The prompt to play again, under either screen"),
        public("TickSound", ENTITY, None, "The tick of each of the last ten seconds"),
        public("TimeColor", "Strada.Color", [0.96, 0.98, 1.0, 1.0], "The colour of the seconds left"),
        public("UrgentColor", "Strada.Color", [1.0, 0.25, 0.2, 1.0], "The colour of the last ten seconds"),
        private("ShownCollected", "int", -1),
        private("ShownSeconds", "int", -1),
    ), events=(("StateChanged", (("State", "string"),)), ("ShowHint", ())), functions=(
        ("ShowSparks", False, (), ()),
        ("ShowClock", False, (), ()),
        ("PulseClock", False, (), ()),
        ("ShowResult", False, (("State", "string"),), ()),
    ))
    author.declare(CAMERA_RIG, "The camera rig: it drifts after the orb, so the camera leans towards where the orb rolls.",
                   variables=(
        public("Orb", ENTITY, None, "The orb the camera follows"),
        public("Home", "Strada.Vector3", CAMERA_HOME, "Where the rig stands while the orb is in the middle"),
        public("Follow", "float", 0.3, "The share of the orb's way from the middle the camera follows", (0.0, 1.0)),
        public("Ease", "float", 2.5, "How quickly the camera catches up, per second", (0.1, 20.0)),
    ))
    declare_tests(author)


# --- Orb.sgraph -----------------------------------------------------------------------------------------------------------

def write_orb(author):
    with author.region(ORB, "Steer: WASD, the arrows or the left stick give a direction", EVENTS, "Steer") as b:
        def key(name):
            return b.pure("Call:Strada.Input.IsKeyDown(Strada.KeyCode)", Key=name)

        def stick(axis):
            return b.pure("Call:Strada.Input.GetGamepadAxis(Strada.GamepadAxis,int)", Axis=axis)

        dead = b.get("DeadZone")
        across = b.add("Expression", {"Formula": "(d || right ? 1 : 0) - (a || left ? 1 : 0) + (abs(stick) > dead ? stick : 0)"}, "Result",
                       comment="Right is +x", d=key("D"), right=key("Right"), a=key("A"), left=key("Left"), stick=stick("LeftX"),
                       dead=dead).out
        along = b.add("Expression", {"Formula": "(s || down ? 1 : 0) - (w || up ? 1 : 0) + (abs(stick) > dead ? stick : 0)"}, "Result",
                      comment="Up the screen is -z, away from the camera", s=key("S"), down=key("Down"), w=key("W"), up=key("Up"),
                      stick=stick("LeftY"), dead=dead).out
        steer = b.expr("vec3(across, 0, along)", across=across, along=along)
        b.link(b.add("Expression", {"Formula": "length(v) > 1 ? normalize(v) : v"}, "Result",
                     comment="A diagonal is no faster than a straight push", v=steer).out, b.ret["Direction"])
    with author.region(ORB, "Roll: each physics step pushes the orb the way it is steered", RULES) as b:
        step = b.event("Event.FixedUpdate")
        rolling = b.branch(b.get("Rolling"))
        push = b.call("Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)", Mode="Force",
                      Force=b.expr("direction * push", direction=b.own("Steer", "Direction").out, push=b.get("Push")))
        b.chain(step, rolling)
        b.flow(rolling["True"], push)
    with author.region(ORB, "Hum: the faster the orb rolls, the higher and louder it hums", EFFECTS) as b:
        tick = b.event("Event.Update")
        speed = b.expr("length(v)", v=b.pure("Get:Strada.RigidBodyComponent.LinearVelocity", "Value"))
        pitch = b.call("Set:Strada.AudioSourceComponent.Pitch", Target=b.get("Hum"), Value=b.expr("0.7 + speed * 0.08", speed=speed))
        volume = b.call("Set:Strada.AudioSourceComponent.Volume", Target=b.get("Hum"), Value=b.expr("min(0.45, speed * 0.06)", speed=speed))
        b.chain(tick, pitch, volume)
    with author.region(ORB, "Bump: a wall hit hard thuds, louder the harder", EFFECTS) as b:
        touch = b.event("Event.CollisionEnter")
        speed = b.expr("length(v)", v=b.pure("Get:Strada.RigidBodyComponent.LinearVelocity", "Value"))
        hard = b.branch(b.both(b.not_equal(touch["Other"], b.get("Floor")), b.expr("speed > 1.5", speed=speed)))
        volume = b.call("Set:Strada.AudioSourceComponent.Volume", Value=b.expr("min(1, speed / 8)", speed=speed))
        b.chain(touch, hard)
        b.chain(hard["True"], volume, b.call("Call:Strada.AudioSourceComponent.Play()"))
    with author.region(ORB, "Control: the orb rolls only while a run is on", EVENTS) as b:
        changed = b.custom("StateChanged")
        b.chain(changed, b.set("Rolling", b.equal(changed["State"], "Play")))


# --- Spark.sgraph ---------------------------------------------------------------------------------------------------------

def write_spark(author):
    with author.region(SPARK, "Start: each spark bobs in a time of its own", EFFECTS) as b:
        start = b.event("Event.Create")
        b.chain(start, b.set("Clock", b.expr("p.x * 0.7 + p.z * 0.4", p=b.position()),
                             comment="Sparks at different spots start at different points of their bob"))
    with author.region(SPARK, "Float: the gem bobs and spins above its spot", EFFECTS) as b:
        tick = b.event("Event.Update")
        clock = b.set("Clock", b.expr("clock + dt", clock=b.get("Clock"), dt=tick["DeltaTime"]))
        gem = b.element(b.children(), 0)
        bob = b.call("Set:Strada.TransformComponent.Translation", Target=gem,
                     Value=b.expr("vec3(0, sin(clock * 2.4) * bob, 0)", clock=b.get("Clock"), bob=b.get("Bob")))
        spin = b.call("Set:Strada.TransformComponent.EulerAngles", Target=b.element(b.children(), 0),
                      Value=b.expr("vec3(0, (clock * spin) % 360, 0)", clock=b.get("Clock"), spin=b.get("Spin")))
        b.chain(tick, clock, bob, spin)
    with author.region(SPARK, "Collect: the rolling orb takes the spark, which flashes and goes", RULES) as b:
        enter = b.event("Event.TriggerEnter")
        orb = enter["Other"]
        taken = b.branch(b.expr("orb && rolling", orb=b.runs(ORB, orb), rolling=b.get("Rolling", ORB, orb)),
                         comment="Only the orb collects, and only during a run")
        burst = b.call("Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)", "Result",
                       Prefab=b.get("Burst"), Position=b.position(), Rotation=IDENTITY)
        tell = b.broadcast("SparkCollected", GAME, comment="The Game counts it")
        b.chain(enter, taken)
        b.chain(taken["True"], burst, tell, b.call("Call:Strada.Entity.Destroy()"))


# --- Game.sgraph ----------------------------------------------------------------------------------------------------------

def change_state(b, state):
    """Sets the State and tells every graph that listens; returns (first, last) of the chain."""
    store = b.set("State", state)
    tell = b.broadcast("StateChanged", parameters=(("State", "string"),), State=state)
    b.flow(store, tell)
    return store, tell


def write_game(author):
    with author.region(GAME, "Place the sparks: one at every spot, the old ones gone", RULES, "PlaceSparks") as b:
        old = b.add("Flow.ForEach", Array=b.children(b.get("Sparks")))
        remove = b.call("Call:Strada.Entity.Destroy()", Target=old["Element"])
        spots = b.add("Flow.ForEach", Array=b.get("Spots"))
        spark = b.call("Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)", "Result",
                       Prefab=b.get("SparkPrefab"), Position=spots["Element"], Rotation=IDENTITY, Parent=b.get("Sparks"))
        b.chain(b.entry, old)
        b.flow(old["Body"], remove)
        b.flow(old["Completed"], spots)
        b.flow(spots["Body"], spark)
        b.flow(spots["Completed"], b.ret)
    with author.region(GAME, "Start a run: the sparks, a full clock, the orb in the middle", RULES, "StartRun") as b:
        place = b.own("PlaceSparks")
        collected = b.set("Collected", 0)
        total = b.set("Total", b.length(b.get("Spots")))
        clock = b.set("TimeLeft", b.get("TimeLimit"))
        home = b.call("Call:Strada.RigidBodyComponent.Teleport(Strada.Vector3,Strada.Quaternion)", Target=b.get("Orb"),
                      Position=b.get("Start"), Rotation=IDENTITY)
        still = b.call("Set:Strada.RigidBodyComponent.LinearVelocity", Target=b.get("Orb"), Value=[0.0, 0.0, 0.0])
        spin = b.call("Set:Strada.RigidBodyComponent.AngularVelocity", Target=b.get("Orb"), Value=[0.0, 0.0, 0.0])
        state, told = change_state(b, "Play")
        b.chain(b.entry, place, collected, total, clock, home, still, spin, state)
        b.flow(told, b.ret)
    with author.region(GAME, "Win: every spark found before the time ran out", RULES, "Win") as b:
        state, told = change_state(b, "Won")
        b.chain(b.entry, b.call("Call:Strada.AudioSourceComponent.Play()", Target=b.get("WinSound")), state)
        b.flow(told, b.ret)
    with author.region(GAME, "Lose: the time ran out", RULES, "Lose") as b:
        state, told = change_state(b, "Lost")
        b.chain(b.entry, b.call("Call:Strada.AudioSourceComponent.Play()", Target=b.get("LoseSound")), state)
        b.flow(told, b.ret)
    with author.region(GAME, "Start: the first run begins at once", EVENTS) as b:
        b.chain(b.event("Event.Create"), b.own("StartRun"))
    with author.region(GAME, "Clock: the time runs down during a run, and a run that ends loses", RULES) as b:
        tick = b.event("Event.Update")
        playing = b.branch(b.equal(b.get("State"), "Play"))
        down = b.set("TimeLeft", b.expr("max(0, left - dt)", left=b.get("TimeLeft"), dt=tick["DeltaTime"]))
        over = b.branch(b.expr("left <= 0", left=b.get("TimeLeft")))
        b.chain(tick, playing)
        b.chain(playing["True"], down, over)
        b.flow(over["True"], b.own("Lose"))
    with author.region(GAME, "Spark collected: count it, sing a note higher, win at the last", RULES) as b:
        collected = b.custom("SparkCollected")
        playing = b.branch(b.equal(b.get("State"), "Play"))
        count = b.set("Collected", b.expr("collected + 1", collected=b.get("Collected")))
        pitch = b.call("Set:Strada.AudioSourceComponent.Pitch", Target=b.get("PickupSound"),
                       Value=b.expr("pow(2, (collected - 1) / 12.0)", collected=b.get("Collected")),
                       comment="A semitone higher for every spark: the last of twelve rings an octave up")
        ring = b.call("Call:Strada.AudioSourceComponent.Play()", Target=b.get("PickupSound"))
        last = b.branch(b.expr("collected >= total", collected=b.get("Collected"), total=b.get("Total")))
        b.chain(collected, playing)
        b.chain(playing["True"], count, pitch, ring, last)
        b.flow(last["True"], b.own("Win"))
    with author.region(GAME, "Play again: Space, Enter, or a gamepad's A or Start after the end", EVENTS) as b:
        presses = [b.event("Event.KeyPressed", {"Key": "Space"}), b.event("Event.KeyPressed", {"Key": "Enter"}),
                   b.event("Event.GamepadButtonPressed", {"Button": "A"}), b.event("Event.GamepadButtonPressed", {"Button": "Start"})]
        ended = b.branch(b.not_equal(b.get("State"), "Play"))
        for press in presses:
            b.flow(press, ended)
        b.flow(ended["True"], b.own("StartRun"))
    with author.region(GAME, "Restart on request: the tests start a new run", EVENTS) as b:
        b.chain(b.custom("Restart"), b.own("StartRun"))


# --- Hud.sgraph -----------------------------------------------------------------------------------------------------------

def show(b, element, visible):
    return b.call("Set:Strada.UIRectComponent.Visible", Target=b.get(element), Value=visible)


def text(b, element, value):
    return b.call("Set:Strada.UITextComponent.Text", Target=b.get(element), Value=value)


def of_game(b, variable):
    return b.get(variable, GAME, b.get("Game"))


def write_hud(author):
    with author.region(HUD, "Sparks: the counter changes when the Game's count does", RULES, "ShowSparks") as b:
        changed = b.branch(b.not_equal(of_game(b, "Collected"), b.get("ShownCollected")))
        shown = b.set("ShownCollected", of_game(b, "Collected"))
        counter = text(b, "SparksText", b.fmt("{Collected} / {Total}", Collected=b.get("ShownCollected"), Total=of_game(b, "Total")))
        b.chain(b.entry, changed)
        b.chain(changed["True"], shown, counter, b.ret)
        b.flow(changed["False"], b.ret)
    with author.region(HUD, "Clock: whole seconds, red and ticking in the last ten", RULES, "ShowClock") as b:
        shown = b.get("ShownSeconds")
        seconds = b.expr("int(ceil(left))", left=of_game(b, "TimeLeft"))
        changed = b.branch(b.not_equal(seconds, shown))
        store = b.set("ShownSeconds", seconds)
        clock = b.get("TimeText")
        number = b.call("Set:Strada.UITextComponent.Text", Target=clock, Value=b.fmt("{Seconds}", Seconds=shown))
        color = b.call("Set:Strada.UITextComponent.Color", Target=clock,
                       Value=b.select(b.expr("shown <= 10", shown=shown), b.get("UrgentColor"), b.get("TimeColor")))
        tick = b.branch(b.expr("shown <= 10 && shown > 0 && playing", shown=shown, playing=b.equal(of_game(b, "State"), "Play")))
        ring = b.call("Call:Strada.AudioSourceComponent.Play()", Target=b.get("TickSound"))
        b.chain(b.entry, changed)
        b.chain(changed["True"], store, number, color, tick)
        b.chain(tick["True"], ring, b.ret)
        b.flow(tick["False"], b.ret)
        b.flow(changed["False"], b.ret)
    with author.region(HUD, "Pulse: each of the last ten seconds starts large and shrinks", EFFECTS, "PulseClock") as b:
        left = of_game(b, "TimeLeft")
        size = b.call("Set:Strada.UITextComponent.FontSize", Target=b.get("TimeText"),
                      Value=b.expr("72 * (1 + (left <= 10 ? 0.35 * pow(frac(left), 3) : 0))", left=left))
        b.chain(b.entry, size, b.ret)
    with author.region(HUD, "Result: the seconds to spare, or the sparks found", RULES, "ShowResult") as b:
        won = b.branch(b.equal(b.entry["State"], "Won"))
        spare = text(b, "WinDetail", b.fmt("{Seconds} SECONDS TO SPARE", Seconds=b.expr("int(ceil(left))", left=of_game(b, "TimeLeft"))))
        found = text(b, "LoseDetail", b.fmt("{Collected} OF {Total} SPARKS", Collected=of_game(b, "Collected"), Total=of_game(b, "Total")))
        b.chain(b.entry, won)
        b.chain(won["True"], spare, b.ret)
        b.chain(won["False"], found, b.ret)
    with author.region(HUD, "Read the game: each frame, the counter and the clock", EVENTS) as b:
        tick = b.event("Event.Update")
        game = b.branch(b.runs(GAME, b.get("Game")), comment="Reads the Game's variables only when its entity runs Game.sgraph")
        b.chain(tick, game)
        b.chain(game["True"], b.own("ShowSparks"), b.own("ShowClock"), b.own("PulseClock"))
    with author.region(HUD, "Screens: each state of the run shows its screen", EVENTS) as b:
        changed = b.custom("StateChanged")
        state = changed["State"]
        win = show(b, "WinScreen", b.equal(state, "Won"))
        lose = show(b, "LoseScreen", b.equal(state, "Lost"))
        prompt = show(b, "Prompt", b.not_equal(state, "Play"))
        hint = show(b, "Hint", False)
        playing = b.branch(b.equal(state, "Play"))
        fade = b.send("ShowHint", b.self_(), comment="The screen's own event: the hint fades in a region of its own")
        game = b.branch(b.runs(GAME, b.get("Game")))
        result = b.own("ShowResult", State=state)
        b.chain(changed, win, lose, prompt, hint, playing)
        b.flow(playing["True"], fade)
        b.flow(playing["False"], game)
        b.flow(game["True"], result)
    with author.region(HUD, "Hint: the goal and the controls fade in, then out", EFFECTS) as b:
        start = b.custom("ShowHint")
        visible = show(b, "Hint", True)
        fade = b.timeline(4.5, [track("Opacity", "float", [(0.0, 0.0), (0.4, 1.0), (3.5, 1.0), (4.5, 0.0)])])
        opacity = b.call("Set:Strada.UIRectComponent.Opacity", Target=b.get("Hint"), Value=fade["Opacity"])
        hidden = show(b, "Hint", False)
        b.chain(start, visible)
        b.flow(visible, fade["PlayFromStart"])
        b.flow(fade["Update"], opacity)
        b.flow(fade["Finished"], hidden)
    with author.region(HUD, "Prompt: the line to play again breathes", EFFECTS) as b:
        start = b.event("Event.Create")
        breathe = b.timeline(1.4, [track("Alpha", "float", [(0.0, 1.0), (0.7, 0.35), (1.4, 1.0)], "Cubic")], loop=True)
        opacity = b.call("Set:Strada.UIRectComponent.Opacity", Target=b.get("Prompt"), Value=breathe["Alpha"])
        b.flow(start, breathe["PlayFromStart"])
        b.flow(breathe["Update"], opacity)


# --- CameraRig.sgraph -----------------------------------------------------------------------------------------------------

def write_camera_rig(author):
    with author.region(CAMERA_RIG, "Follow: the camera drifts after the orb", EFFECTS) as b:
        tick = b.event("Event.Update")
        target = b.expr("home + vec3(orb.x, 0, orb.z) * follow", home=b.get("Home"), orb=b.position(b.get("Orb")), follow=b.get("Follow"))
        place = b.call("Set:Strada.Entity.Translation",
                       Value=b.expr("lerp(here, target, 1 - exp(-ease * dt))", here=b.position(), target=target, ease=b.get("Ease"),
                                    dt=tick["DeltaTime"]))
        b.chain(tick, place)


# --- Tests.sgraph: the test scene's checks --------------------------------------------------------------------------------

def shared(b, key, make):
    """A value a region reads in several places, made once: a pure node evaluates again for every consumer."""
    if key not in b.shared:
        b.shared[key] = make()
    return b.shared[key]


def var(b, name):
    return shared(b, ("var", name), lambda: b.get(name))


def game(b, variable):
    """A read-out of the Game (a remote Get)."""
    return shared(b, ("game", variable), lambda: b.get(variable, GAME, var(b, "Game")))


def count_sparks(b):
    return shared(b, "sparks", lambda: b.length(b.children(var(b, "Sparks"))))


def visible(b, element):
    return shared(b, ("visible", element), lambda: b.pure("Get:Strada.UIRectComponent.Visible", "Value", Target=var(b, element)))


def shown_text(b, element):
    return shared(b, ("text", element), lambda: b.pure("Get:Strada.UITextComponent.Text", "Value", Target=var(b, element)))


def is_state(b, state):
    return shared(b, ("state", state), lambda: b.equal(game(b, "State"), state))


def rolling(b):
    return shared(b, "rolling", lambda: b.get("Rolling", ORB, var(b, "Orb")))


def whole_seconds(b):
    return shared(b, "seconds", lambda: b.expr("int(ceil(left))", left=game(b, "TimeLeft")))


def wait_until(b, done, limit):
    """Marks the step's start and waits until done holds or limit seconds have passed (without done: the seconds), so a
    check that never comes true fails instead of hanging; returns (first, last) of the chain."""
    now = b.pure("Get:Strada.Time.Elapsed", "Value")
    mark = b.set("StepStart", now)
    inputs = {"now": now, "start": var(b, "StepStart")}
    if done is None:
        condition = f"now - start > {limit}"
    else:
        condition = f"done || now - start > {limit}"
        inputs["done"] = done
    wait = b.add("Flow.WaitUntil", Condition=b.expr(condition, **inputs))
    b.flow(mark, wait)
    return mark, wait["Completed"]


def check(b, name, done, limit, detail, passed=None):
    """Waits for done (at most limit seconds), then reports the check; passed defaults to done."""
    first, waited = wait_until(b, done, limit)
    reported = b.library("Check", CHECKS, Name=f"Orb Run: {name}", Passed=done if passed is None else passed, Detail=detail)
    b.flow(waited, reported)
    return first, reported


def run(b, *steps):
    """Runs a function's steps in order, from its entry to its return: each a node, or (first, last) of a chain."""
    previous = b.entry
    for step in steps:
        first, last = step if isinstance(step, tuple) else (step, step)
        b.flow(previous, first)
        previous = last
    b.flow(previous, b.ret)


def near(b, value, expected, tolerance):
    return b.library("Near", CHECKS, output="Result", A=value, B=expected, Tolerance=tolerance).out


# The checks in the order they run: (function, title of its box, the check's name in the report).
TEST_CHECKS = (
    ("CheckRunStarts", "Start: twelve sparks, a full clock, the orb rolling", "a run starts with twelve sparks and a full clock"),
    ("CheckClock", "Clock: a second of the game takes a second off", "the clock runs down in seconds of the game"),
    ("CheckHudStart", "HUD: 0 / 12, and the whole seconds left", "the HUD shows the sparks and the seconds left"),
    ("CheckCollect", "Collect: the orb rolled onto a spark takes it", "the orb collects a spark it reaches"),
    ("CheckHudCount", "HUD: the counter reads 1 / 12", "the HUD counts the spark"),
    ("CheckUrgent", "Last ten seconds: the clock turns red", "the last ten seconds turn the clock red"),
    ("CheckWin", "Win: the last spark wins the run", "collecting every spark wins"),
    ("CheckWinScreen", "Win screen: the seconds to spare, and the prompt", "the win screen shows the seconds to spare"),
    ("CheckStops", "After the end: the controls push no more", "the orb stops rolling when the run ends"),
    ("CheckRestart", "Restart: twelve sparks, a full clock, the orb home", "a new run starts fresh"),
    ("CheckLose", "Lose: the clock runs out", "running out of time loses"),
    ("CheckLoseScreen", "Lose screen: the sparks found, and the prompt", "the lose screen counts the sparks"),
    ("CheckAgain", "Play again: a run after a loss", "a run after a loss plays again"),
)
TEST_ENTITIES = (
    ("Game", "The entity whose Game.sgraph the checks play"),
    ("Orb", "The orb"),
    ("Sparks", "The container of the run's sparks"),
    ("SparksText", "The HUD's spark counter"),
    ("TimeText", "The HUD's seconds left"),
    ("WinScreen", "The win screen"),
    ("WinDetail", "The win screen's line with the seconds to spare"),
    ("LoseScreen", "The lose screen"),
    ("LoseDetail", "The lose screen's line with the sparks found"),
    ("Prompt", "The prompt to play again"),
)


def declare_tests(author):
    author.declare(TESTS, f"The test scene's checks: {len(TEST_CHECKS)} checks of Orb Run's rules, screen and flow, one after "
                   "another, each a latent function that acts, waits for what it expects and reports it.", variables=(
        tuple(public(name, ENTITY, None, tooltip) for name, tooltip in TEST_ENTITIES) + (
            private("StepStart", "double", 0.0),
            private("Target", ENTITY),
            private("Before", "float", 0.0),
        )), functions=(
        ("RollOnto", False, (("Spark", ENTITY),), ()),
    ) + tuple((name, False, (), ()) for name, _, _ in TEST_CHECKS))


def write_tests(author):
    with author.region(TESTS, "Start: every check in turn, then the report", CHECK) as b:
        start = b.event("Event.Create")
        settle = b.add("Flow.WaitFrames", Frames=1, comment="Every entity has run its Create first")
        steps = b.add("Flow.Sequence", {"Count": len(TEST_CHECKS) + 1})
        b.chain(start, settle)
        b.flow(settle["Completed"], steps)
        for index, (name, _, _) in enumerate(TEST_CHECKS):
            b.flow(steps[f"Then{index}"], b.own(name))
        b.flow(steps[f"Then{len(TEST_CHECKS)}"], b.call("Call:Strada.Testing.TestReporter.Finish()"))
    with author.region(TESTS, "Roll onto: the orb put where a spark stands", CHECK, "RollOnto") as b:
        orb = var(b, "Orb")
        teleport = b.call("Call:Strada.RigidBodyComponent.Teleport(Strada.Vector3,Strada.Quaternion)", Target=orb, Rotation=IDENTITY,
                          Position=b.expr("vec3(p.x, 0.5, p.z)", p=b.position(b.entry["Spark"])))
        still = b.call("Set:Strada.RigidBodyComponent.LinearVelocity", Target=orb, Value=[0.0, 0.0, 0.0])
        run(b, teleport, still)
    for function, title, name in TEST_CHECKS:
        with author.region(TESTS, title, CHECK, function) as b:
            TEST_WRITERS[function](b, name)


def check_run_starts(b, name):
    count = count_sparks(b)
    done = b.expr(f"count == {len(SPOTS)} && total == {len(SPOTS)} && playing && rolling", count=count, total=game(b, "Total"),
                  playing=is_state(b, "Play"), rolling=rolling(b))
    run(b, check(b, name, done, 2, b.fmt("{Count} sparks of {Total}, state {State}", Count=count, Total=game(b, "Total"),
                                         State=game(b, "State"))))


def check_clock(b, name):
    left = game(b, "TimeLeft")
    taken = b.expr("before - left", before=var(b, "Before"), left=left)
    run(b, b.set("Before", left), wait_until(b, None, 1),
        check(b, name, near(b, taken, 1.0, 0.05), 0.1, b.fmt("{Taken} s off the clock in a second", Taken=taken)))


def check_hud_start(b, name):
    sparks = shown_text(b, "SparksText")
    clock = shown_text(b, "TimeText")
    seconds = b.fmt("{Seconds}", Seconds=whole_seconds(b))
    done = b.both(b.equal(sparks, f"0 / {len(SPOTS)}"), b.equal(clock, seconds))
    run(b, check(b, name, done, 1, b.fmt("'{Sparks}' and '{Clock}' with {Seconds} s left", Sparks=sparks, Clock=clock, Seconds=seconds)))


def check_collect(b, name):
    target = var(b, "Target")
    count = count_sparks(b)
    gone = b.negate(b.pure("Get:Strada.Entity.IsValid", "Value", Target=target))
    done = b.expr(f"gone && collected == 1 && count == {len(SPOTS) - 1}", gone=gone, collected=game(b, "Collected"), count=count)
    run(b, b.set("Target", b.element(b.children(var(b, "Sparks")), 0)), b.own("RollOnto", Spark=target),
        check(b, name, done, 2, b.fmt("{Collected} collected, {Count} sparks left", Collected=game(b, "Collected"), Count=count)))


def check_hud_count(b, name):
    sparks = shown_text(b, "SparksText")
    run(b, check(b, name, b.equal(sparks, f"1 / {len(SPOTS)}"), 1, b.fmt("the counter reads '{Sparks}'", Sparks=sparks)))


def check_urgent(b, name):
    color = b.pure("Get:Strada.UITextComponent.Color", "Value", Target=var(b, "TimeText"))
    red = b.expr("r > 0.9 && g < 0.5", r=b.pure("Get:Strada.Color.R", "Value", Target=color),
                 g=b.pure("Get:Strada.Color.G", "Value", Target=color))
    clock = shown_text(b, "TimeText")
    done = b.both(red, b.equal(clock, "10"))
    run(b, b.set("TimeLeft", 9.5, GAME, var(b, "Game")),
        check(b, name, done, 1, b.fmt("'{Clock}' in {Color}", Clock=clock, Color=color)))


def check_win(b, name):
    each = b.add("Flow.ForEach", Array=b.children(var(b, "Sparks")))
    onto = b.own("RollOnto", Spark=each["Element"])
    wait = b.add("Flow.WaitFrames", Frames=4, comment="The orb stays on each spark for a few physics steps")
    b.flow(each["Body"], onto)
    b.flow(onto, wait)
    won = is_state(b, "Won")
    run(b, b.set("TimeLeft", 30.0, GAME, var(b, "Game"), comment="Time enough for every spark"), (each, each["Completed"]),
        check(b, name, won, 2, b.fmt("state {State}, {Collected} collected", State=game(b, "State"), Collected=game(b, "Collected"))))


def check_win_screen(b, name):
    detail = shown_text(b, "WinDetail")
    expected = b.fmt("{Seconds} SECONDS TO SPARE", Seconds=whole_seconds(b))
    done = b.expr("win && !lose && prompt && same", win=visible(b, "WinScreen"), lose=visible(b, "LoseScreen"),
                  prompt=visible(b, "Prompt"), same=b.equal(detail, expected))
    run(b, check(b, name, done, 1, b.fmt("'{Detail}', expected '{Expected}'", Detail=detail, Expected=expected)))


def check_stops(b, name):
    still = b.negate(rolling(b))
    run(b, check(b, name, still, 1, b.fmt("rolling: {Rolling}", Rolling=rolling(b))))


def check_restart(b, name):
    count = count_sparks(b)
    orb = b.position(var(b, "Orb"))
    home = b.library("NearPoint", CHECKS, output="Result", A=orb, B=game(b, "Start"), Tolerance=0.3).out
    full = near(b, game(b, "TimeLeft"), game(b, "TimeLimit"), 0.2)
    done = b.expr(f"playing && count == {len(SPOTS)} && collected == 0 && full && home && !win",
                  playing=is_state(b, "Play"), count=count, collected=game(b, "Collected"), full=full, home=home,
                  win=visible(b, "WinScreen"))
    run(b, b.send("Restart", var(b, "Game"), GAME),
        check(b, name, done, 2, b.fmt("state {State}, {Count} sparks, {Left} s, the orb at {Orb}", State=game(b, "State"),
                                      Count=count, Left=game(b, "TimeLeft"), Orb=orb)))


def check_lose(b, name):
    done = b.both(is_state(b, "Lost"), visible(b, "LoseScreen"))
    run(b, b.set("TimeLeft", 0.3, GAME, var(b, "Game")), check(b, name, done, 2, b.fmt("state {State}", State=game(b, "State"))))


def check_lose_screen(b, name):
    detail = shown_text(b, "LoseDetail")
    done = b.both(b.equal(detail, f"0 OF {len(SPOTS)} SPARKS"), visible(b, "Prompt"))
    run(b, check(b, name, done, 1, b.fmt("'{Detail}'", Detail=detail)))


def check_again(b, name):
    done = b.expr("playing && !lose && !prompt && rolling", playing=is_state(b, "Play"), lose=visible(b, "LoseScreen"),
                  prompt=visible(b, "Prompt"), rolling=rolling(b))
    run(b, b.send("Restart", var(b, "Game"), GAME), check(b, name, done, 2, b.fmt("state {State}", State=game(b, "State"))))


TEST_WRITERS = {
    "CheckRunStarts": check_run_starts, "CheckClock": check_clock, "CheckHudStart": check_hud_start, "CheckCollect": check_collect,
    "CheckHudCount": check_hud_count, "CheckUrgent": check_urgent, "CheckWin": check_win, "CheckWinScreen": check_win_screen,
    "CheckStops": check_stops, "CheckRestart": check_restart, "CheckLose": check_lose, "CheckLoseScreen": check_lose_screen,
    "CheckAgain": check_again,
}


def write_checks(author):
    with author.region(CHECKS, "Check: report a pass, or a failure with its detail", CHECK, "Check") as b:
        passed = b.branch(b.entry["Passed"])
        report_pass = b.call("Call:Strada.Testing.TestReporter.Pass(string)", Name=b.entry["Name"])
        report_fail = b.call("Call:Strada.Testing.TestReporter.Fail(string,string)", Name=b.entry["Name"], Message=b.entry["Detail"])
        b.chain(b.entry, passed)
        b.chain(passed["True"], report_pass, b.ret)
        b.chain(passed["False"], report_fail, b.ret)
    with author.region(CHECKS, "Near: two numbers within a tolerance", CHECK, "Near") as b:
        b.link(b.expr("abs(a - b) <= tolerance", a=b.entry["A"], b=b.entry["B"], tolerance=b.entry["Tolerance"]), b.ret["Result"])
    with author.region(CHECKS, "Near point: two points within a tolerance", CHECK, "NearPoint") as b:
        b.link(b.expr("distance(a, b) <= tolerance", a=b.entry["A"], b=b.entry["B"], tolerance=b.entry["Tolerance"]), b.ret["Result"])


WRITERS = {CHECKS: write_checks, ORB: write_orb, SPARK: write_spark, GAME: write_game, HUD: write_hud, CAMERA_RIG: write_camera_rig,
           TESTS: write_tests}


# --- The scene ------------------------------------------------------------------------------------------------------------

def transform(translation, euler=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    return {"Translation": list(translation), "RotationEuler": list(euler), "Scale": list(scale)}


def material(name):
    return f"asset://Materials/{name}.smat"


def clip(name):
    return f"asset://Audio/{name}.wav"


def entity_value(entity):
    return {"Type": "Entity", "Value": entity}


# Linear colours of the game.
ORB_GLOW = [0.15, 0.7, 1.0]
SPARK_GOLD = [1.0, 0.66, 0.16]
CYAN = [0.1, 0.8, 1.0]
VIOLET = [0.55, 0.25, 1.0]


def make_materials(client):
    def make(name, fields):
        client.call("material_create", {"path": f"Materials/{name}.smat", "fields": fields})

    make("Floor", {"BaseColor": [0.02, 0.025, 0.05, 1.0], "Metallic": 0.2, "Roughness": 0.35, "EmissiveTexture": "asset://Textures/Grid.png",
                   "EmissiveColor": [0.12, 0.32, 0.9], "EmissiveIntensity": 0.9, "UVTiling": [11.0, 11.0]})
    make("Wall", {"BaseColor": [0.14, 0.16, 0.26, 1.0], "Metallic": 0.3, "Roughness": 0.35})
    make("Neon Cyan", {"BaseColor": [0.0, 0.0, 0.0, 1.0], "EmissiveColor": CYAN, "EmissiveIntensity": 7.0})
    make("Neon Violet", {"BaseColor": [0.0, 0.0, 0.0, 1.0], "EmissiveColor": VIOLET, "EmissiveIntensity": 7.0})
    make("Orb", {"BaseColor": [0.1, 0.3, 0.4, 1.0], "Roughness": 0.15, "EmissiveTexture": "asset://Textures/Orb.png",
                 "EmissiveColor": ORB_GLOW, "EmissiveIntensity": 12.0})
    make("Spark", {"BaseColor": [1.0, 0.78, 0.3, 1.0], "Metallic": 0.3, "Roughness": 0.2, "EmissiveColor": SPARK_GOLD,
                   "EmissiveIntensity": 8.0})


def make_prefab(client, name, components, children=()):
    """An entity (with child entities: (name, components)) saved as Prefabs/<name>.sprefab, then removed from the scene."""
    entity = client.call("entity_create", {"name": name, "components": components})["id"]
    for child_name, child_components in children:
        client.call("entity_create", {"name": child_name, "parent": entity, "components": child_components})
    reference = client.call("prefab_create", {"entity": entity, "path": f"Prefabs/{name}.sprefab"})["reference"]
    client.call("entity_delete", {"entities": [entity]})
    return reference


def make_prefabs(client, spark_behaviour):
    """The burst a collected spark leaves and the spark; spark_behaviour(burst) gives the components that make a spark act
    (its visual script, or its C# script and the graph that floats its gem)."""
    burst = make_prefab(client, "Spark Burst", {
        "Transform": transform((0.0, -100.0, 0.0)),
        "ParticleSystem": {"Emitting": False, "Burst": 60, "MaxParticles": 80, "Shape": "Sphere", "Radius": 0.3, "LifetimeMin": 0.35,
                           "LifetimeMax": 0.8, "SpeedMin": 3.0, "SpeedMax": 7.0, "Drag": 2.5, "GravityFactor": 0.3, "StartSize": 0.32,
                           "EndSize": 0.0, "StartColor": [1.0, 0.9, 0.55, 1.0], "EndColor": [1.0, 0.45, 0.1, 0.0], "Intensity": 7.0,
                           "Texture": "asset://Textures/Glow.png", "BlendMode": "Additive", "WorldSpace": True, "DestroyWhenFinished": True},
    })
    spark = make_prefab(client, "Spark", dict({
        "Transform": transform((0.0, -100.0, 0.0)),
        "RigidBody": {"Type": "Static"},
        "SphereCollider": {"Radius": 0.8, "IsTrigger": True},
        "PointLight": {"Color": SPARK_GOLD, "Intensity": 5.0, "Range": 3.5},
        "ParticleSystem": {"Emitting": True, "Rate": 8.0, "MaxParticles": 24, "Shape": "Sphere", "Radius": 0.35, "LifetimeMin": 0.6,
                           "LifetimeMax": 1.1, "SpeedMin": 0.2, "SpeedMax": 0.6, "GravityFactor": -0.15, "StartSize": 0.16, "EndSize": 0.0,
                           "StartColor": [1.0, 0.85, 0.45, 1.0], "EndColor": [1.0, 0.5, 0.1, 0.0], "Intensity": 5.0,
                           "Texture": "asset://Textures/Glow.png", "BlendMode": "Additive", "WorldSpace": True},
    }, **spark_behaviour(burst)), children=(("Gem", {"Transform": transform((0.0, 0.0, 0.0), scale=(1.4, 1.4, 1.4)),
                                                    "Mesh": {"Mesh": "asset://Meshes/Spark.gltf", "Materials": [material("Spark")],
                                                             "CastShadows": False}}),))
    return {"Spark": spark}


# The screen: a 1920 x 1080 design, y down. Colours are linear with straight alpha.
GLASS = [0.004, 0.006, 0.02, 0.72]
CAPTION = [0.45, 0.6, 0.85, 1.0]
VALUE = [0.96, 0.98, 1.0, 1.0]
GOLD_TEXT = [1.0, 0.76, 0.25, 1.0]
LOSE_TEXT = [1.0, 0.32, 0.4, 1.0]


def ui_rect(anchor_min, anchor_max, pivot, position, size, visible=True, opacity=1.0):
    return {"AnchorMin": list(anchor_min), "AnchorMax": list(anchor_max), "Pivot": list(pivot), "Position": list(position),
            "Size": list(size), "Visible": visible, "Opacity": opacity}


def at(anchor, pivot, position, size, **flags):
    """A fixed size, its pivot placed at a point of the parent."""
    return ui_rect(anchor, anchor, pivot, position, size, **flags)


def fill(**flags):
    return ui_rect((0.0, 0.0), (1.0, 1.0), (0.5, 0.5), (0.0, 0.0), (0.0, 0.0), **flags)


def image(color, texture=None, radius=0.0):
    fields = {"Color": list(color), "CornerRadius": radius}
    if texture:
        fields["Texture"] = f"asset://Textures/{texture}.png"
    return fields


def label(value, size, color, alignment="Center", vertical="Middle"):
    return {"Text": value, "FontSize": size, "Color": list(color), "Alignment": alignment, "VerticalAlignment": vertical, "Wrap": False}


def build_screen(client):
    """The HUD's canvas and elements; returns the canvas and the elements Hud.sgraph changes, by its variable names."""
    def element(name, parent, rect, image_fields=None, text_fields=None):
        components = {"UIRect": rect}
        if image_fields is not None:
            components["UIImage"] = image_fields
        if text_fields is not None:
            components["UIText"] = text_fields
        return client.call("entity_create", {"name": name, "parent": parent, "components": components})["id"]

    elements = {}
    screen = client.call("entity_create", {"name": "Screen", "components": {
        "Transform": transform((0.0, 6.0, -13.0)),
        "UICanvas": {"ScaleMode": "ScaleWithScreenSize", "ReferenceResolution": [1920.0, 1080.0]}}})["id"]

    # The sparks on the left, the seconds left on the right: the middle stays clear for the arena the camera follows.
    sparks = element("Sparks Panel", screen, at((0.0, 0.0), (0.0, 0.0), (48.0, 40.0), (400.0, 128.0)), image(GLASS, radius=24.0))
    element("Spark Icon", sparks, at((0.0, 0.5), (0.0, 0.5), (24.0, 0.0), (84.0, 84.0)), image([1.0, 1.0, 1.0, 1.0], "SparkIcon"))
    element("Sparks Caption", sparks, at((0.0, 0.0), (0.0, 0.0), (128.0, 16.0), (250.0, 30.0)), text_fields=label("SPARKS", 24.0, CAPTION, "Left"))
    elements["SparksText"] = element("Sparks Text", sparks, at((0.0, 0.0), (0.0, 0.0), (128.0, 44.0), (260.0, 70.0)),
                                     text_fields=label(f"0 / {len(SPOTS)}", 60.0, VALUE, "Left"))
    clock = element("Time Panel", screen, at((1.0, 0.0), (1.0, 0.0), (-48.0, 40.0), (300.0, 128.0)), image(GLASS, radius=24.0))
    element("Time Caption", clock, at((1.0, 0.0), (1.0, 0.0), (-28.0, 16.0), (240.0, 30.0)), text_fields=label("TIME", 24.0, CAPTION, "Right"))
    elements["TimeText"] = element("Time Text", clock, at((1.0, 0.0), (1.0, 0.0), (-28.0, 40.0), (240.0, 80.0)),
                                   text_fields=label(str(int(TIME_LIMIT)), 72.0, VALUE, "Right"))

    # The hint at the start of a run.
    hint = element("Hint", screen, at((0.5, 1.0), (0.5, 1.0), (0.0, -96.0), (1360.0, 150.0), visible=False, opacity=0.0),
                   image(GLASS, radius=28.0))
    elements["Hint"] = hint
    element("Hint Goal", hint, at((0.5, 0.0), (0.5, 0.0), (0.0, 22.0), (1300.0, 60.0)),
            text_fields=label(f"COLLECT ALL {len(SPOTS)} SPARKS BEFORE THE TIME RUNS OUT", 46.0, GOLD_TEXT))
    element("Hint Controls", hint, at((0.5, 0.0), (0.5, 0.0), (0.0, 90.0), (1300.0, 40.0)),
            text_fields=label("ROLL WITH WASD, THE ARROW KEYS OR A GAMEPAD'S LEFT STICK", 30.0, CAPTION))

    # The win and lose screens, and the prompt under both.
    def result_screen(name, title, title_color, detail):
        shade = element(f"{name} Screen", screen, fill(visible=False), image([0.0, 0.0, 0.02, 0.5]))
        panel = element(f"{name} Panel", shade, at((0.5, 0.5), (0.5, 0.5), (0.0, -40.0), (1000.0, 330.0)), image(GLASS, radius=32.0))
        element(f"{name} Title", panel, at((0.5, 0.0), (0.5, 0.0), (0.0, 50.0), (960.0, 130.0)), text_fields=label(title, 110.0, title_color))
        text_entity = element(f"{name} Detail", panel, at((0.5, 0.0), (0.5, 0.0), (0.0, 205.0), (960.0, 70.0)),
                              text_fields=label(detail, 52.0, VALUE))
        return shade, text_entity

    elements["WinScreen"], elements["WinDetail"] = result_screen("Win", "ALL SPARKS FOUND!", GOLD_TEXT, "0 SECONDS TO SPARE")
    elements["LoseScreen"], elements["LoseDetail"] = result_screen("Lose", "TIME'S UP", LOSE_TEXT, f"0 OF {len(SPOTS)} SPARKS")
    elements["Prompt"] = element("Prompt", screen, at((0.5, 0.5), (0.5, 0.5), (0.0, 210.0), (1400.0, 60.0), visible=False),
                                 text_fields=label("PRESS SPACE OR (A) TO PLAY AGAIN", 40.0, VALUE))
    return screen, elements


def build_scene(client, prefabs, attach):
    """The main scene: the camera rig, the light and the look, the arena, the orb, the Game and the screen. attach(entity,
    role, values) gives the entities of the roles Orb, CameraRig, Game and Hud what makes them act, with these values for
    their variables or fields. Returns the entities the test scene names."""
    found = {entity["name"]: entity["id"] for entity in client.call("entity_find", {})["entities"]}
    client.call("entity_delete", {"entities": [found[name] for name in ("Floor", "Cube", "Sphere", "Capsule") if name in found]})
    client.call("scene_settings", {"name": "Main", "settings": {"Renderer": {
        "Tonemapper": "ACES", "EV100": 0.0, "Shadows": True, "SoftShadows": True, "ShadowDistance": 50.0, "AmbientOcclusion": True,
        "Bloom": True, "BloomIntensity": 0.15, "FXAA": True}}})

    def create(name, components, parent=None):
        arguments = {"name": name, "components": components}
        if parent is not None:
            arguments["parent"] = parent
        return client.call("entity_create", arguments)["id"]

    # The camera rig at its home, the camera on it looking down at the arena.
    rig = create("Camera Rig", {"Transform": transform(CAMERA_HOME)})
    camera = found["Main Camera"]
    client.call("entity_reparent", {"entity": camera, "parent": rig})
    pitch = -math.degrees(math.atan2(CAMERA_HOME[1] - CAMERA_LOOK[1], CAMERA_HOME[2] - CAMERA_LOOK[2]))
    client.call("component_set", {"entity": camera, "component": "Transform", "fields": transform((0.0, 0.0, 0.0), euler=(pitch, 0.0, 0.0))})
    client.call("component_set", {"entity": camera, "component": "Camera", "fields": {"PerspectiveFOV": 50.0, "PerspectiveFar": 200.0}})
    client.call("component_add", {"entity": camera, "component": "AudioListener"})

    # A dim blue moon, a faint sky and a look that makes the glow bloom.
    client.call("component_set", {"entity": found["Sun"], "component": "Transform", "fields": {"RotationEuler": [-58.0, 30.0, 0.0]}})
    client.call("component_set", {"entity": found["Sun"], "component": "DirectionalLight",
                                  "fields": {"Color": [0.6, 0.66, 1.0], "Intensity": 0.6, "CastShadows": True}})
    client.call("component_set", {"entity": found["Sky"], "component": "SkyLight", "fields": {
        "Environment": "builtin://DefaultSky", "Intensity": 0.12, "DrawSkybox": False}})
    create("Look", {"PostProcess": {"Global": True, "OverrideBloom": True, "Bloom": True, "OverrideBloomIntensity": True, "BloomIntensity": 0.3,
                                    "OverrideContrast": True, "Contrast": 1.05, "OverrideSaturation": True, "Saturation": 1.1,
                                    "OverrideVignette": True, "Vignette": 0.35}})

    # The arena: the floor, four walls with neon along their tops, and four lights in the corners.
    arena = create("Arena", {"Transform": transform((0.0, 0.0, 0.0))})

    def block(name, center, size, look, collider=True):
        components = {"Transform": transform(center, scale=size), "Mesh": {"Mesh": "builtin://Cube", "Materials": [material(look)]}}
        if collider:
            components["RigidBody"] = {"Type": "Static"}
            components["BoxCollider"] = {"HalfExtents": [0.5, 0.5, 0.5], "Restitution": 0.4, "Friction": 0.6}
        return create(name, components, arena)

    span = 2.0 * HALF + 2.0 * WALL
    floor = block("Floor", (0.0, -0.1, 0.0), (span, 0.2, span), "Floor")
    middle = HALF + WALL / 2.0
    block("North Wall", (0.0, WALL_HEIGHT / 2.0, -middle), (span, WALL_HEIGHT, WALL), "Wall")
    block("South Wall", (0.0, WALL_HEIGHT / 2.0, middle), (span, WALL_HEIGHT, WALL), "Wall")
    block("West Wall", (-middle, WALL_HEIGHT / 2.0, 0.0), (WALL, WALL_HEIGHT, 2.0 * HALF), "Wall")
    block("East Wall", (middle, WALL_HEIGHT / 2.0, 0.0), (WALL, WALL_HEIGHT, 2.0 * HALF), "Wall")
    top = WALL_HEIGHT + 0.03
    block("North Neon", (0.0, top, -middle), (span, 0.06, 0.12), "Neon Violet", collider=False)
    block("South Neon", (0.0, top, middle), (span, 0.06, 0.12), "Neon Violet", collider=False)
    block("West Neon", (-middle, top, 0.0), (0.12, 0.06, 2.0 * HALF), "Neon Cyan", collider=False)
    block("East Neon", (middle, top, 0.0), (0.12, 0.06, 2.0 * HALF), "Neon Cyan", collider=False)
    for index, (x, z) in enumerate(((-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0))):
        create(f"Corner Light {index + 1}", {"Transform": transform((x * (HALF - 1.0), 2.5, z * (HALF - 1.0))),
                                             "PointLight": {"Color": CYAN if index % 2 == 0 else VIOLET, "Intensity": 6.0, "Range": 12.0}},
               arena)

    # The orb: a glowing ball that rolls, with a trail, a light, a thud and a hum.
    orb = create("Orb", {
        "Transform": transform(ORB_START),
        "Mesh": {"Mesh": "builtin://Sphere", "Materials": [material("Orb")]},
        "RigidBody": {"Type": "Dynamic", "Mass": 1.0, "LinearDamping": 1.2, "AngularDamping": 0.4, "AllowSleep": False},
        "SphereCollider": {"Radius": 0.5, "Restitution": 0.35, "Friction": 0.9},
        "PointLight": {"Color": ORB_GLOW, "Intensity": 12.0, "Range": 7.0},
        "ParticleSystem": {"Emitting": True, "Rate": 45.0, "MaxParticles": 60, "Shape": "Sphere", "Radius": 0.25, "LifetimeMin": 0.35,
                           "LifetimeMax": 0.6, "SpeedMin": 0.0, "SpeedMax": 0.3, "StartSize": 0.35, "EndSize": 0.0,
                           "StartColor": [0.5, 0.9, 1.0, 0.7], "EndColor": [0.45, 0.3, 1.0, 0.0], "Intensity": 3.0,
                           "Texture": "asset://Textures/Glow.png", "BlendMode": "Additive", "WorldSpace": True},
        "AudioSource": {"Clip": clip("Bump"), "Volume": 0.8, "Spatial": False},
    })
    hum = create("Hum", {"AudioSource": {"Clip": clip("Hum"), "Loop": True, "PlayOnStart": True, "Volume": 0.0, "Spatial": False}}, orb)
    attach(orb, "Orb", {"Hum": entity_value(hum), "Floor": entity_value(floor)})
    attach(rig, "CameraRig", {"Orb": entity_value(orb), "Home": {"Type": "Vector3", "Value": CAMERA_HOME}})

    sparks = create("Sparks", {"Transform": transform((0.0, 0.0, 0.0))})
    game = create("Game", {"Transform": transform((0.0, 0.0, 0.0))})
    sounds = create("Sounds", {"Transform": transform((0.0, 0.0, 0.0))}, game)
    sound = {name: create(name, {"AudioSource": {"Clip": clip(name), "Volume": volume, "Spatial": False}}, sounds)
             for name, volume in (("Pickup", 0.8), ("Win", 0.9), ("Lose", 0.9), ("Tick", 0.6))}
    screen, elements = build_screen(client)

    game_variables = {
        "SparkPrefab": {"Type": "Prefab", "Value": prefabs["Spark"]},
        "Sparks": entity_value(sparks),
        "Orb": entity_value(orb),
        "PickupSound": entity_value(sound["Pickup"]),
        "WinSound": entity_value(sound["Win"]),
        "LoseSound": entity_value(sound["Lose"]),
    }
    attach(game, "Game", game_variables)
    hud_variables = {"Game": entity_value(game), "TickSound": entity_value(sound["Tick"])}
    for name, entity in elements.items():
        hud_variables[name] = entity_value(entity)
    attach(screen, "Hud", hud_variables)
    known = {"Game": game, "Orb": orb, "Sparks": sparks}
    known.update(elements)
    return {name: known[name] for name, _ in TEST_ENTITIES}


def build_test_scene(client, checks, entities):
    """The test scene: the main scene saved as Tests (its entities keep their IDs) and the entity whose graph or script runs
    the checks; checks(values) gives its components, with these values for the entities the checks name."""
    client.call("scene_save", {"path": "Scenes/Tests.sscene"})
    client.call("scene_settings", {"name": "Tests"})
    client.call("entity_create", {"name": "Tests", "components": checks({name: entity_value(entity) for name, entity in entities.items()})})
    client.call("scene_save")
    client.call("scene_open", {"path": "Scenes/Main.sscene"})


def author_graph_starter(client, project, sources):
    client.call("project_settings", {"settings": {"Name": "Orb Run", "Window": {"Title": "Orb Run", "Width": 1600, "Height": 900},
                                                  "Physics": {"FixedTimestep": 1.0 / 60.0}}})
    subprocess.run([sys.executable, os.path.join(TOOLS, "GenerateAssets.py"), project], check=True)
    client.call("asset_refresh")
    make_materials(client)

    author = Author(client)
    for graph in ORDER:
        author.handles[graph] = client.call("graph_create", {"path": graph[len("asset://"):], "template": "Library" if graph in LIBRARIES
                                                             else "Empty"})["asset"]
    declare_all(author)
    for graph in ORDER:
        WRITERS[graph](author)
        author.finish(graph)
    print(f"wrote {len(ORDER)} graphs in {author.batches} batches")

    def spark_behaviour(burst):
        return {"VisualScript": {"Graph": author.handles[SPARK], "Variables": {"Burst": {"Type": "Prefab", "Value": burst}}}}

    roles = {"Orb": ORB, "CameraRig": CAMERA_RIG, "Game": GAME, "Hud": HUD}

    def attach(entity, role, values):
        client.call("component_add", {"entity": entity, "component": "VisualScript", "fields": {"Graph": author.handles[roles[role]],
                                                                                               "Variables": values}})

    prefabs = make_prefabs(client, spark_behaviour)
    entities = build_scene(client, prefabs, attach)
    client.call("scene_save")
    build_test_scene(client, lambda values: {"VisualScript": {"Graph": author.handles[TESTS], "Variables": values}}, entities)


# --- csharp-starter: Orb Run in C# ----------------------------------------------------------------------------------------

# The C# classes of the roles the scene's entities play (namespace Game).
CSHARP_CLASSES = {"Orb": "Game.Orb", "CameraRig": "Game.CameraRig", "Game": "Game.OrbRun", "Hud": "Game.Hud", "Spark": "Game.Spark",
                  "Tests": "Game.OrbRunTests"}
SCRIPT_PROJECT = os.path.join("Scripts", "Game.csproj")
SCRIPT_SOURCES = os.path.join("Scripts", "Source")
REPOSITORY_ONLY = """\t<!-- strada:repository-only -->
\t<!-- In the engine's repository, builds without Strada.props (dotnet format, IDEs) use the scripting API from source;
\t     CMake builds pass StradaScriptCoreAssembly. -->
\t<ItemGroup Condition="'$(StradaScriptCoreAssembly)' == ''">
\t\t<ProjectReference Include="../../../Strada-ScriptCore/Strada.ScriptCore.csproj">
\t\t\t<Private>false</Private>
\t\t</ProjectReference>
\t</ItemGroup>
\t<!-- /strada:repository-only -->
"""


def use_engine_sources(csproj):
    """In the engine's repository, builds without the machine's Strada.props (dotnet format in CI, IDEs) take the scripting
    API from its sources; CMake builds pass StradaScriptCoreAssembly. The fallback is marked repository-only, so the projects
    made from the template leave it out."""
    with open(csproj, encoding="utf-8") as file:
        text = file.read()
    reference = '\t<ItemGroup>\n\t\t<Reference Include="Strada.ScriptCore">'
    if text.count(reference) != 1 or text.count("</Project>") != 1:
        raise SystemExit(f"{csproj} does not reference Strada.ScriptCore as expected")
    text = text.replace(reference, '\t<ItemGroup Condition="\'$(StradaScriptCoreAssembly)\' != \'\'">\n\t\t<Reference Include="Strada.ScriptCore">')
    text = text.replace("</Project>", REPOSITORY_ONLY + "</Project>")
    with open(csproj, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)


def declare_hover(author):
    author.declare(HOVER, "A spark's float: its gem bobs where the spark's C# Hover puts it, and spins.", variables=(
        public("Spin", "float", 120.0, "Degrees per second the gem turns", (0.0, 720.0)),
        private("Clock", "float", 0.0),
    ))


def write_hover(author):
    with author.region(HOVER, "Float: the gem bobs where C# Hover puts it, and spins", EFFECTS) as b:
        tick = b.event("Event.Update")
        clock = b.get("Clock")
        count = b.set("Clock", b.expr("clock + dt", clock=clock, dt=tick["DeltaTime"]))
        gem = b.element(b.children(), 0)
        hover = b.add(HOVER_NODE, None, "Result", comment="Spark.Hover in Scripts/Source/Spark.cs, a [VisualNode] member",
                      Time=clock).out
        bob = b.call("Set:Strada.TransformComponent.Translation", Target=gem, Value=hover)
        spin = b.call("Set:Strada.TransformComponent.EulerAngles", Target=gem,
                      Value=b.expr("vec3(0, (clock * spin) % 360, 0)", clock=clock, spin=b.get("Spin")))
        b.chain(tick, count, bob, spin)


def author_csharp_starter(client, project, sources):
    client.call("project_settings", {"settings": {"Name": "Orb Run", "Window": {"Title": "Orb Run", "Width": 1600, "Height": 900},
                                                  "Physics": {"FixedTimestep": 1.0 / 60.0}}})
    # The editor makes the C# project (Scripts/Game.csproj: the default script module, Game) with the first class; the
    # sources replace what it wrote.
    created = client.call("script_create", {"className": "OrbRun"})
    if created["class"] != "Game.OrbRun":
        raise SystemExit(f"the editor made the class {created['class']}, not Game.OrbRun")
    shutil.copytree(sources, os.path.join(project, SCRIPT_SOURCES), dirs_exist_ok=True)
    use_engine_sources(os.path.join(project, SCRIPT_PROJECT))
    built = client.call("script_build")
    if not built["succeeded"] or built["warnings"]:
        raise SystemExit(f"the scripts did not build cleanly: {built}")
    found = [node["Type"] for node in client.call("graph_library", {"query": "Hover"})["nodes"] if node.get("Game")]
    if found != [HOVER_NODE]:
        raise SystemExit(f"the scripts' [VisualNode] members are the nodes {found}, not {HOVER_NODE}")

    subprocess.run([sys.executable, os.path.join(TOOLS, "GenerateAssets.py"), project], check=True)
    client.call("asset_refresh")
    make_materials(client)

    author = Author(client)
    author.handles[HOVER] = client.call("graph_create", {"path": HOVER[len("asset://"):], "template": "Empty"})["asset"]
    declare_hover(author)
    write_hover(author)
    author.finish(HOVER)
    print(f"wrote 1 graph in {author.batches} batches")

    def spark_behaviour(burst):
        return {"Script": {"ClassName": CSHARP_CLASSES["Spark"], "Fields": {"Burst": {"Type": "Prefab", "Value": burst}}},
                "VisualScript": {"Graph": author.handles[HOVER], "Variables": {}}}

    def attach(entity, role, values):
        if role == "Game":
            values = dict(values, TimeLimit={"Type": "Float", "Value": TIME_LIMIT})
        client.call("component_add", {"entity": entity, "component": "Script", "fields": {"ClassName": CSHARP_CLASSES[role],
                                                                                         "Fields": values}})

    prefabs = make_prefabs(client, spark_behaviour)
    entities = build_scene(client, prefabs, attach)
    client.call("scene_save")
    build_test_scene(client, lambda values: {"Script": {"ClassName": CSHARP_CLASSES["Tests"], "Fields": values}}, entities)


# --- IDs that two runs share -------------------------------------------------------------------------------------------------

DOCUMENTS = (".sproj", ".sreg", ".sscene", ".sprefab", ".sgraph", ".smat")
QUOTED_DIGITS = re.compile(r'"(\d+)"')


def project_documents(project):
    """The project's JSON documents, by their paths relative to the project with forward slashes, in path order."""
    found = []
    for folder, _, names in os.walk(project):
        for name in names:
            if name.endswith(DOCUMENTS):
                path = os.path.join(folder, name)
                found.append((os.path.relpath(path, project).replace(os.sep, "/"), path))
    return sorted(found)


def derive_id(template_id, key, taken):
    """A 64-bit ID from a key, above the handles the engine reserves for built-in assets, unique in taken."""
    salt = 0
    while True:
        digest = hashlib.sha256(f"strada-template/{template_id}/{key}/{salt}".encode()).digest()
        value = int.from_bytes(digest[:8], "little")
        if value >= 1 << 32 and value not in taken:
            taken.add(value)
            return str(value)
        salt += 1


def entity_lists(value):
    """Every list of entities (objects with an "ID") in a document."""
    if isinstance(value, dict):
        for key, member in value.items():
            if key == "Entities" and isinstance(member, list):
                yield member
            else:
                yield from entity_lists(member)
    elif isinstance(value, list):
        for member in value:
            yield from entity_lists(member)


def normalize_ids(project, template_id):
    """Replaces the IDs the engine drew at random with IDs derived from what they name: an asset's handle from its path, an
    entity's ID from the first file (in path order) and place it appears at. Only IDs the registry and the entity lists
    define are replaced, wherever they are referred to; a quoted number of 10 digits or more that is neither replaced nor
    derived stops the tool, since it would be an ID this does not know."""
    documents = project_documents(project)
    taken = set()
    mapping = {}
    with open(os.path.join(project, "Assets", "AssetRegistry.sreg"), encoding="utf-8") as file:
        registry = json.load(file)
    for entry in sorted(registry["Assets"], key=lambda asset: asset["Path"]):
        mapping[entry["Handle"]] = derive_id(template_id, f"asset:{entry['Path']}", taken)
    for relative, path in documents:
        if not relative.endswith((".sscene", ".sprefab")):
            continue
        with open(path, encoding="utf-8") as file:
            document = json.load(file)
        for entities in entity_lists(document):
            for index, entity in enumerate(entities):
                if entity["ID"] not in mapping:
                    mapping[entity["ID"]] = derive_id(template_id, f"entity:{relative}:{index}", taken)
    derived = set(mapping.values())
    for relative, path in documents:
        with open(path, encoding="utf-8", newline="") as file:
            content = file.read()
        replaced = QUOTED_DIGITS.sub(lambda match: f'"{mapping.get(match.group(1), match.group(1))}"', content)
        unknown = [number for number in QUOTED_DIGITS.findall(replaced) if len(number) >= 10 and number not in derived]
        if unknown:
            raise SystemExit(f"{relative} holds numbers that look like IDs this tool does not know: {unknown[:5]}")
        if replaced != content:
            with open(path, "w", encoding="utf-8", newline="") as file:
                file.write(replaced)
    print(f"derived {len(mapping)} IDs from the assets' paths and the entities' places")


# --- Checks of the written template -----------------------------------------------------------------------------------------

def contains(box, node):
    """Whether a node's position lies inside a comment box."""
    x, y = node["Position"]
    (left, top), (width, height) = box["Position"], box["Size"]
    return left <= x <= left + width and top <= y <= top + height


def verify_graphs(client, graphs, library_use=None):
    """Every graph analyzes without a diagnostic and every node resolves exactly; every node stands in exactly one comment
    box, and every box stands arranged; library_use, (a library, the path of a graph), is a library that graph uses."""
    for graph in graphs:
        document = client.call("graph_get", {"graph": graph})["document"]
        checked = client.call("graph_check", {"graph": graph, "document": document})
        if not checked["valid"] or not checked["analyzed"] or checked["diagnostics"]:
            raise SystemExit(f"{graph} does not check clean: {checked['issues']} {checked.get('diagnostics')}")
        inexact = [node["ID"] for node in checked["nodes"] if node["Resolved"] != "Exact"]
        if inexact:
            raise SystemExit(f"{graph} has nodes that do not resolve exactly: {inexact}")
        sub_graphs = [(0, document["Graph"])] + [(function["ID"], function["Graph"]) for function in document["Functions"]]
        for function, content in sub_graphs:
            boxes = content["Comments"]
            for node in content["Nodes"]:
                if sum(contains(box, node) for box in boxes) != 1:
                    raise SystemExit(f"node {node['ID']} of {graph} (sub-graph {function}) is not inside exactly one comment box")
            for box in boxes:
                inside = [node["ID"] for node in content["Nodes"] if contains(box, node)]
                arranged = client.call("graph_arrange", {"graph": graph, "function": function, "nodes": inside, "dryRun": True})
                if arranged["changed"]:
                    raise SystemExit(f"the box '{box['Text']}' of {graph} does not stand arranged")
    if library_use is not None:
        library, user = library_use
        used_by = {entry["path"] for entry in client.call("graph_interface", {"graph": library})["usedBy"]}
        if user not in used_by:
            raise SystemExit(f"{library} is used by {sorted(used_by)}, not by {user}")


def read_log(client, since, level):
    """The log's entries at or above a level from an index on; returns (entries, the index after the last)."""
    entries = []
    while True:
        read = client.call("log_read", {"since": since, "minLevel": level, "maxCount": 10000})
        since = read["next"]
        if not read["entries"]:
            return entries, since
        entries += read["entries"]


def graph_variable(client, entity, name):
    """A public variable of an entity's graph as the running game holds it."""
    components = client.call("entity_get", {"entity": entity})["components"]
    return components["VisualScript"]["Variables"][name]["Value"]


def find_entity(client, name):
    matches = [entity["id"] for entity in client.call("entity_find", {"name": name})["entities"] if entity["name"] == name]
    if len(matches) != 1:
        raise SystemExit(f"expected one entity named {name}, found {len(matches)}")
    return matches[0]


def play_graph_starter(client):
    """Plays the main scene: W rolls the orb away from the camera, the clock runs out into a loss, Space starts a new run,
    and no visual script warns meanwhile."""
    _, start = read_log(client, 0, "critical")
    game = find_entity(client, "Game")
    orb = find_entity(client, "Orb")
    client.call("play_start", {"paused": True})
    client.call("play_advance", {"frames": 30})
    before = client.call("entity_get", {"entity": orb})["components"]["Transform"]["Translation"]
    client.call("input_set", {"keys": {"W": True}})
    client.call("play_advance", {"frames": 60})
    client.call("input_set", {"keys": {"W": False}})
    after = client.call("entity_get", {"entity": orb})["components"]["Transform"]["Translation"]
    if before[2] - after[2] < 1.5:
        raise SystemExit(f"W did not roll the orb away from the camera: from {before} to {after}")
    client.call("play_advance", {"frames": int(TIME_LIMIT * 60) + 30})
    if graph_variable(client, game, "State") != "Lost":
        raise SystemExit(f"the run did not end when its time ran out: {graph_variable(client, game, 'State')}")
    client.call("input_set", {"keys": {"Space": True}})
    client.call("play_advance", {"frames": 2})
    client.call("input_set", {"keys": {"Space": False}})
    client.call("play_advance", {"frames": 2})
    if graph_variable(client, game, "State") != "Play" or graph_variable(client, game, "TimeLeft") < TIME_LIMIT - 0.5:
        raise SystemExit("Space after the loss did not start a new run")
    entries, _ = read_log(client, start, "warn")
    client.call("play_stop")
    problems = [entry["message"] for entry in entries if "isual script" in entry["message"] or ".sgraph" in entry["message"]]
    if problems:
        raise SystemExit("playing logged problems of visual scripts:\n  " + "\n  ".join(problems))


def play_csharp_starter(client):
    """Plays the main scene: W rolls the orb away from the camera, the clock runs out into the lose screen, Space starts a
    new run with a full clock, and no script throws and no visual script warns meanwhile."""
    _, start = read_log(client, 0, "critical")
    orb = find_entity(client, "Orb")
    lose_screen = find_entity(client, "Lose Screen")
    time_text = find_entity(client, "Time Text")

    def component(entity, name):
        return client.call("entity_get", {"entity": entity})["components"][name]

    client.call("play_start", {"paused": True})
    client.call("play_advance", {"frames": 30})
    before = component(orb, "Transform")["Translation"]
    client.call("input_set", {"keys": {"W": True}})
    client.call("play_advance", {"frames": 60})
    client.call("input_set", {"keys": {"W": False}})
    after = component(orb, "Transform")["Translation"]
    if before[2] - after[2] < 1.5:
        raise SystemExit(f"W did not roll the orb away from the camera: from {before} to {after}")
    client.call("play_advance", {"frames": int(TIME_LIMIT * 60) + 30})
    if not component(lose_screen, "UIRect")["Visible"]:
        raise SystemExit("the lose screen did not show when the time ran out")
    client.call("input_set", {"keys": {"Space": True}})
    client.call("play_advance", {"frames": 2})
    client.call("input_set", {"keys": {"Space": False}})
    client.call("play_advance", {"frames": 2})
    clock = component(time_text, "UIText")["Text"]
    if component(lose_screen, "UIRect")["Visible"] or clock != str(int(TIME_LIMIT)):
        raise SystemExit(f"Space after the loss did not start a new run: the clock reads {clock}")
    entries, _ = read_log(client, start, "warn")
    client.call("play_stop")
    problems = [entry["message"] for entry in entries
                if "Exception" in entry["message"] or "isual script" in entry["message"] or ".sgraph" in entry["message"]]
    if problems:
        raise SystemExit("playing logged problems of the scripts:\n  " + "\n  ".join(problems))


def is_shipped(relative):
    """Whether a file of a template (a path relative to it, with forward slashes) is part of it: what the project copy filter
    copies into new projects. Building the scripts writes the rest."""
    parts = relative.split("/")
    if relative == "Scripts/Strada.props" or parts[0] in ("Tools", "Build", ".strada", ".git") or "__pycache__" in parts[:-1]:
        return False
    return not (len(parts) > 2 and parts[0] == "Scripts" and parts[1] in ("bin", "obj", "Binaries"))


def remove_build_outputs(folder):
    """Removes what building the scripts wrote into the template."""
    for name in ("Binaries", "bin", "obj"):
        shutil.rmtree(os.path.join(folder, "Scripts", name), ignore_errors=True)
    props = os.path.join(folder, "Scripts", "Strada.props")
    if os.path.isfile(props):
        os.remove(props)


def snapshot(folder):
    """Every file the template ships with its bytes, by its path relative to the folder."""
    files = {}
    for directory, _, names in os.walk(folder):
        for name in names:
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, folder).replace(os.sep, "/")
            if is_shipped(relative):
                with open(path, "rb") as file:
                    files[relative] = file.read()
    return files


def check_template(command, project, graphs, library_use, checks, play):
    """Opens the written template in a new editor and checks it: its graphs, its test scene (checks checks, all passed) and
    a play of its main scene; nothing it ships changes."""
    before = snapshot(project)
    client = McpClient(command + ["mcp", "--headless", "--new"])
    try:
        client.call("project_open", {"path": os.path.join(project, "Template.sproj")})
        if os.path.isfile(os.path.join(project, SCRIPT_PROJECT)):
            built = client.call("script_build")
            if not built["succeeded"] or built["warnings"]:
                raise SystemExit(f"the scripts did not build cleanly: {built}")
        verify_graphs(client, graphs, library_use)
        result = client.call("test_run", {"scene": "Scenes/Tests.sscene", "timeout": 90})
        print("test run:", {key: result[key] for key in ("finished", "timedOut", "frames", "passed", "failed", "exceptions")})
        for entry in result["results"]:
            print("  ", "PASS" if entry["passed"] else "FAIL", entry["name"], entry.get("message", ""))
        if result["failed"] or result["exceptions"] or not result["finished"] or result["passed"] != checks:
            raise SystemExit("the tests failed")
        play(client)
        # Saving each scene again writes what the normalized file holds: nothing in a scene is ordered by its IDs' values.
        for scene in ("Scenes/Tests.sscene", "Scenes/Main.sscene"):
            client.call("scene_open", {"path": scene})
            client.call("scene_save")
    finally:
        client.close()
    after = snapshot(project)
    changed = sorted(path for path in set(before) | set(after) if before.get(path) != after.get(path))
    if changed:
        raise SystemExit(f"checking the template changed its files: {', '.join(changed)}")


# --- The templates ----------------------------------------------------------------------------------------------------------

# The checks of csharp-starter's test scene (Scripts/Source/OrbRunTests.cs).
CSHARP_TEST_CHECKS = 14

TEMPLATES_BUILT = {
    "graph-starter": {
        "author": author_graph_starter,
        "check": lambda command, project: check_template(command, project, ORDER, (CHECKS, "Scripts/Tests.sgraph"), len(TEST_CHECKS),
                                                         play_graph_starter),
        "sources": None,
        "template": {"Id": "graph-starter", "Name": "Graph Starter", "Order": 2,
                     "Description": f"Roll a glowing orb and collect {len(SPOTS)} sparks before time runs out: every rule is a graph.",
                     "Requires": {"Sdk": False}, "Tags": ["3D", "Visual scripts"], "Thumbnail": "thumbnail.png",
                     "ProjectFile": "Template.sproj"},
    },
    "csharp-starter": {
        "author": author_csharp_starter,
        "check": lambda command, project: check_template(command, project, (HOVER,), None, CSHARP_TEST_CHECKS, play_csharp_starter),
        "sources": SCRIPT_SOURCES,
        # Its thumbnail's camera rig stands closer to the orb than the game's (Home is 0, 17.5, 15): both templates play the same
        # game, and their cards on the start screen must not look alike.
        "thumbnail_rig": [0.0, 8.5, 6.6],
        "template": {"Id": "csharp-starter", "Name": "C# Starter", "Order": 3,
                     "Description": f"Roll a glowing orb and collect {len(SPOTS)} sparks before time runs out, written in C#, with a "
                                    "C# member that a graph calls as a node.",
                     "Requires": {"Sdk": True}, "Tags": ["3D", "C#"], "Thumbnail": "thumbnail.png", "ProjectFile": "Template.sproj"},
    },
}
THUMBNAIL = "thumbnail.png"
THUMBNAIL_SIZE = (640, 360)
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def write_template_file(folder, fields):
    document = {"Strada": {"Version": 1, "Type": "ProjectTemplate"}, "Template": fields}
    with open(os.path.join(folder, "template.json"), "w", encoding="utf-8", newline="\n") as file:
        json.dump(document, file, indent="\t", ensure_ascii=False)
        file.write("\n")


def read_png(path):
    """The width, height, channels and rows (lists of 8-bit values) of a non-interlaced 8-bit RGB or RGBA PNG image."""
    with open(path, "rb") as file:
        data = file.read()
    if data[:8] != PNG_SIGNATURE:
        raise SystemExit(f"{path} is not a PNG image")
    position = 8
    compressed = b""
    width = height = channels = 0
    while position < len(data):
        length = int.from_bytes(data[position:position + 4], "big")
        kind = data[position + 4:position + 8]
        body = data[position + 8:position + 8 + length]
        position += 12 + length
        if kind == b"IHDR":
            width, height = int.from_bytes(body[0:4], "big"), int.from_bytes(body[4:8], "big")
            depth, color_type, interlace = body[8], body[9], body[12]
            if depth != 8 or color_type not in (2, 6) or interlace != 0:
                raise SystemExit(f"{path} is not an 8-bit, non-interlaced RGB or RGBA image")
            channels = 3 if color_type == 2 else 4
        elif kind == b"IDAT":
            compressed += body
    raw = zlib.decompress(compressed)
    stride = width * channels
    rows = []
    previous = [0] * stride
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        line = list(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 0xFF
            elif kind == 2:
                line[x] = (line[x] + up) & 0xFF
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 0xFF
            elif kind == 4:
                estimate = left + up - corner
                to_left, to_up, to_corner = abs(estimate - left), abs(estimate - up), abs(estimate - corner)
                predictor = left if to_left <= to_up and to_left <= to_corner else up if to_up <= to_corner else corner
                line[x] = (line[x] + predictor) & 0xFF
        rows.append(line)
        previous = line
    return width, height, channels, rows


def area_weights(source, target):
    """For each target sample, the source samples it covers with their shares (an area average)."""
    scale = source / target
    weights = []
    for index in range(target):
        begin, end = index * scale, (index + 1) * scale
        covered = []
        sample = int(begin)
        while sample < end and sample < source:
            share = min(end, sample + 1) - max(begin, sample)
            if share > 0.0:
                covered.append((sample, share / scale))
            sample += 1
        weights.append(covered)
    return weights


def write_scaled_png(source, target, size):
    """Writes the PNG image source scaled to size (an area average) as an RGB PNG image."""
    width, height, channels, rows = read_png(source)
    columns = area_weights(width, size[0])
    lines = area_weights(height, size[1])
    narrow = [[[sum(row[sample * channels + channel] * share for sample, share in covered) for channel in range(3)] for covered in columns]
              for row in rows]
    pixels = bytearray()
    for covered in lines:
        pixels.append(0)
        pixels += bytes(min(255, round(sum(narrow[sample][x][channel] * share for sample, share in covered)))
                        for x in range(size[0]) for channel in range(3))

    def chunk(kind, body):
        return len(body).to_bytes(4, "big") + kind + body + (zlib.crc32(kind + body) & 0xFFFFFFFF).to_bytes(4, "big")

    header = size[0].to_bytes(4, "big") + size[1].to_bytes(4, "big") + bytes((8, 2, 0, 0, 0))
    with open(target, "wb") as file:
        file.write(PNG_SIGNATURE + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(pixels), 9)) + chunk(b"IEND", b""))


def place_thumbnail_rig(project, home):
    """Moves the camera rig of the copy's main scene, and the Home its CameraRig script keeps it at, to home."""
    scene_file = os.path.join(project, "Assets", "Scenes", "Main.sscene")
    with open(scene_file, encoding="utf-8") as file:
        scene = json.load(file)
    rigs = [entity for entity in scene["Entities"] if entity["Components"].get("Tag", {}).get("Tag") == "Camera Rig"]
    if len(rigs) != 1:
        raise SystemExit(f"{scene_file} has {len(rigs)} entities named Camera Rig, not one")
    components = rigs[0]["Components"]
    components["Transform"]["Translation"] = list(home)
    components["Script"]["Fields"]["Home"]["Value"] = list(home)
    with open(scene_file, "w", encoding="utf-8") as file:
        json.dump(scene, file, indent="\t")


def take_thumbnail(player, folder, rig_home=None):
    """Plays a copy of the template in the player for two and a half seconds and keeps its last frame, scaled to 640 x 360,
    as thumbnail.png. With rig_home, the copy's camera rig stands there (place_thumbnail_rig)."""
    work = tempfile.mkdtemp(prefix="template-thumbnail-")
    try:
        copy = os.path.join(work, "Project")
        shutil.copytree(folder, copy, ignore=shutil.ignore_patterns("template.json", THUMBNAIL))
        project_file = os.path.join(copy, "Template.sproj")
        with open(project_file, encoding="utf-8") as file:
            settings = json.load(file)
        # The copy's window has the thumbnail's shape; the display's scale decides its size in pixels.
        settings["Project"]["Window"]["Width"], settings["Project"]["Window"]["Height"] = (2 * THUMBNAIL_SIZE[0], 2 * THUMBNAIL_SIZE[1])
        with open(project_file, "w", encoding="utf-8") as file:
            json.dump(settings, file, indent="\t")
        if rig_home is not None:
            place_thumbnail_rig(copy, rig_home)
        shot = os.path.join(work, "capture.png")
        subprocess.run([player, "--project", project_file, "--frames", "150", "--screenshot", shot], check=True)
        write_scaled_png(shot, os.path.join(folder, THUMBNAIL), THUMBNAIL_SIZE)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def build_template(command, template_id, folder):
    spec = TEMPLATES_BUILT[template_id]
    # The editor creates projects in new or empty folders only: the thumbnail and the C# sources wait outside meanwhile. The
    # sources are the template's in the repository, also when it is built elsewhere (--output).
    aside = tempfile.mkdtemp(prefix="template-author-")
    kept = os.path.join(folder, THUMBNAIL)
    sources = None
    if spec["sources"] is not None:
        repository_sources = os.path.join(TEMPLATES, template_id, spec["sources"])
        if not os.path.isdir(repository_sources):
            raise SystemExit(f"{repository_sources} holds no C# sources")
        sources = os.path.join(aside, "Source")
        shutil.copytree(repository_sources, sources)
    if os.path.isfile(kept):
        shutil.move(kept, os.path.join(aside, THUMBNAIL))
    authored = False
    try:
        if os.path.isdir(folder):
            shutil.rmtree(folder)
        client = McpClient(command + ["mcp", "--headless", "--new"])
        try:
            client.call("project_create", {"directory": folder, "name": "Template", "gitIgnore": False, "agents": False})
            spec["author"](client, folder, sources)
            authored = True
        finally:
            client.close()
    finally:
        if os.path.isfile(os.path.join(aside, THUMBNAIL)):
            os.makedirs(folder, exist_ok=True)
            shutil.move(os.path.join(aside, THUMBNAIL), kept)
        # The sources are written by hand: a build that failed puts them back as they were.
        if sources is not None and not authored:
            target = os.path.join(folder, spec["sources"])
            shutil.rmtree(target, ignore_errors=True)
            shutil.copytree(sources, target)
        shutil.rmtree(aside, ignore_errors=True)
    normalize_ids(folder, template_id)
    write_template_file(folder, spec["template"])
    spec["check"](command, folder)


def main():
    parser = argparse.ArgumentParser(description="Builds the project templates through strada mcp.")
    parser.add_argument("--template", choices=sorted(TEMPLATES_BUILT), action="append",
                        help="a template to build (repeatable; default: every template this tool builds)")
    parser.add_argument("--strada", help="the strada executable (default: strada from the repository, through dotnet run)")
    parser.add_argument("--output", help="the folder to build into instead of Templates/<id> (with one --template)")
    parser.add_argument("--thumbnail", action="store_true", help="take the thumbnail again with the player beside --strada")
    arguments = parser.parse_args()
    templates = arguments.template or sorted(TEMPLATES_BUILT)
    if arguments.output and len(templates) != 1:
        parser.error("--output builds one template: name it with --template")
    if arguments.thumbnail and not arguments.strada:
        parser.error("--thumbnail needs --strada, whose folder holds the player")
    command = [os.path.abspath(arguments.strada)] if arguments.strada else \
        ["dotnet", "run", "--project", os.path.join(ROOT, "StradaTool"), "-c", "Release", "--no-launch-profile", "--"]
    for template_id in templates:
        folder = os.path.abspath(arguments.output) if arguments.output else os.path.join(TEMPLATES, template_id)
        build_template(command, template_id, folder)
        if arguments.thumbnail:
            player = os.path.join(os.path.dirname(os.path.abspath(arguments.strada)), "StradaRuntime" + (".exe" if os.name == "nt" else ""))
            take_thumbnail(player, folder, TEMPLATES_BUILT[template_id].get("thumbnail_rig"))
        remove_build_outputs(folder)
        if not os.path.isfile(os.path.join(folder, THUMBNAIL)):
            print(f"warning: {folder} has no {THUMBNAIL}: take one with --thumbnail")
        print(f"built {template_id} in {folder}")


if __name__ == "__main__":
    main()

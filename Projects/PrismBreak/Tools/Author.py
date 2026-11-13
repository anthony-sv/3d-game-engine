"""Builds the Prism Break project through `strada mcp`, the way an agent does: a neon brick breaker made of visual scripts
alone (no C#). In order:

1. the project and its settings (window, physics layers), the generated sounds, prism mesh and textures
   (GenerateAssets.py), and the materials;
2. the thirteen graphs (Arcade and Checks, function libraries, first);
3. a declare pass: one graph_edit batch per graph adding its variables, its custom events with their parameters and its
   functions with their inputs and outputs, the interface the other graphs' typed sends, remote variables and calls
   resolve to, so graphs that refer to each other in cycles never see a missing event;
4. the bodies, one graph_edit batch per region of a graph: the region's nodes and links, arranged (scope Created, then
   Nodes) and framed by a comment box titled for what it does, in its role's colour. Every batch must answer no error and
   no warning, or the tool stops;
5. the prefabs, the main scene and the test scene;
6. checks: each graph's document has no diagnostic and every node resolves exactly, each box stays arranged, and the
   Arcade library is used by the graphs that call it;
7. the tests (test_run of Scenes/Tests.sscene), the title's attract mode played for 600 frames without a warning from a
   visual script, and an exported game's smoke test.

    python Projects/PrismBreak/Tools/Author.py [strada executable]

The project is rebuilt from scratch: everything but this Tools folder is replaced, and two runs give the same graphs.
Without an executable, strada runs from the repository (dotnet run).
"""
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(TOOLS)
ROOT = os.path.dirname(os.path.dirname(PROJECT))


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


# --- The graphs' files ------------------------------------------------------------------------------------------------------

def script(name):
    return f"asset://Scripts/{name}.sgraph"


ARCADE = script("Arcade")
CHECKS = script("Checks")
BRICK = script("Brick")
BALL = script("Ball")
SHARD = script("Shard")
POWER_UP = script("PowerUp")
PADDLE = script("Paddle")
DRAIN = script("Drain")
LIGHTS = script("Lights")
CAMERA_RIG = script("CameraRig")
HUD = script("Hud")
GAME = script("Game")
TESTS = script("Tests")
LIBRARIES = (ARCADE, CHECKS)
# The order the bodies are written in: libraries first, then each graph before the graphs that call or read it most.
ORDER = (ARCADE, CHECKS, BRICK, BALL, SHARD, POWER_UP, PADDLE, DRAIN, LIGHTS, CAMERA_RIG, HUD, GAME, TESTS)

# Comment boxes are coloured by role.
EVENTS = [0.2, 0.42, 0.95, 0.22]
RULES = [0.95, 0.55, 0.15, 0.22]
EFFECTS = [0.62, 0.3, 0.95, 0.22]
TITLE = [0.25, 0.78, 0.4, 0.22]
DEVELOPMENT = [0.9, 0.22, 0.22, 0.22]
CHECK = [0.45, 0.6, 0.7, 0.22]

# Linear colours of the game: the prisms by hits left, the neon and the lights.
CYAN = [0.08, 0.85, 1.0, 1.0]
MAGENTA = [1.0, 0.12, 0.72, 1.0]
GOLD = [1.0, 0.7, 0.08, 1.0]
WHITE = [1.0, 1.0, 1.0, 1.0]
BRICK_COLORS = [CYAN, MAGENTA, GOLD, WHITE]
WARNING_RED = [1.0, 0.08, 0.05, 1.0]
IDENTITY = [0.0, 0.0, 0.0, 1.0]

# The arena (metres, y up, the camera looking towards -z).
WALL_X = 8.5
BACK_Z = -12.5
PADDLE_Z = 8.5
PADDLE_WIDTH = 3.2
BALL_RADIUS = 0.25
BRICK_SIZE = (1.35, 0.55, 0.7)
COLUMNS = 11
ORIGIN = [-7.25, 0.5, -10.5]
SPACING = [1.45, 0.85]
# Higher than a 42-degree view of the whole arena needs: from (0, 19, 13.5) the paddle line falls below the frame.
CAMERA_HOME = [0.0, 26.0, 16.0]
CAMERA_LOOK = [0.0, 0.0, -0.5]

# The levels: rows joined by '/', '.' an empty cell, '1' to '4' a prism of that many hits. Level 1 has 33 prisms, level 2
# 34 and level 3 44; they cycle, faster each time round.
LEVELS = [
    "...11111.../..1222221../.123333321./..1222221../...11111...",
    "..11...11../.1221.1221./12..212..21/2..3...3..2/11111111111",
    "4.4.4.4.4.4/44444444444/4.3333333.4/4.3.222.3.4/4.3.212.3.4/4.3.....3.4",
]
LEVEL_BRICKS = [sum(ch.isdigit() for ch in layout) for layout in LEVELS]


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

    def __init__(self, author, graph, title, color, function=None, development=False):
        if len(title) > 72:
            raise SystemExit(f"the region title '{title}' is longer than 72 characters")
        self.author = author
        self.graph = graph
        self.title = title
        self.color = color
        self.function = function
        self.development = development
        self.additions = []
        self.connections = []
        self.after = []
        self.framed = []
        self.count = 0
        self.shared = {}
        declared = author.declared[graph]
        if function is not None:
            entry, ret = declared["functions"][function]
            self.entry = self.existing(entry, "Then")
            self.ret = self.existing(ret)

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        if kind is None:
            self.author.write_region(self)
        return False

    def existing(self, node_id, output=None):
        node = Node(node_id, output)
        self.framed.append(node_id)
        return node

    def label(self):
        self.count += 1
        return f"$n{self.count}"

    def link(self, source, target):
        if isinstance(source, Node):
            source = source.out
        self.connections.append({"op": "Connect", "from": list(source), "to": list(target)})

    def add(self, type_id, settings=None, output=None, comment=None, enabled=None, **inputs):
        label = self.label()
        operation = {"op": "AddNode", "type": type_id, "as": label}
        if settings:
            operation["settings"] = settings
        literals = {}
        for pin, value in inputs.items():
            name = pin.replace("__", ".")
            if isinstance(value, (Pin, Node)):
                self.link(value, (label, name))
            elif value is not None:
                literals[name] = value
        if literals:
            operation["literals"] = literals
        self.additions.append(operation)
        if comment:
            self.after.append({"op": "SetNodeComment", "nodes": [label], "text": comment})
        if enabled:
            self.after.append({"op": "SetEnabled", "nodes": [label], "enabled": enabled})
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

    def sequence(self, count):
        return self.add("Flow.Sequence", {"Count": count})

    # Values.
    def expr(self, formula, **inputs):
        return self.add("Expression", {"Formula": formula}, "Result", **inputs).out

    def get(self, variable, graph=None, target=None):
        settings = {"Variable": variable}
        if graph is not None:
            settings["Graph"] = graph
        return self.add("Variable.Get", settings, "Value", Target=target).out

    def set(self, variable, value=None, graph=None, target=None, comment=None, enabled=None):
        settings = {"Variable": variable}
        if graph is not None:
            settings["Graph"] = graph
        return self.add("Variable.Set", settings, "Value", comment=comment, enabled=enabled, Value=value, Target=target)

    def lit(self, type_id, value):
        return self.add("Literal", {"Type": type_id, "Value": value}, "Value").out

    def pure(self, type_id, output="Result", **inputs):
        return self.add(type_id, None, output, **inputs).out

    def call(self, type_id, output=None, comment=None, enabled=None, **inputs):
        return self.add(type_id, None, output, comment=comment, enabled=enabled, **inputs)

    def equal(self, a, b):
        return self.pure("Operator.Equal", A=a, B=b)

    def not_equal(self, a, b):
        return self.pure("Operator.NotEqual", A=a, B=b)

    def either(self, a, b):
        return self.pure("Operator.Or", A=a, B=b)

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

    def children(self, entity):
        return self.pure("Get:Strada.Entity.Children", "Value", Target=entity)

    def position(self, entity=None):
        return self.pure("Get:Strada.Entity.Translation", "Value", Target=entity)

    # Calls, events and messages.
    def library(self, function, graph=ARCADE, output=None, **inputs):
        return self.add("Function.Call", {"Graph": graph, "Function": function}, output, **inputs)

    def own(self, function, output=None, **inputs):
        return self.add("Function.Call", {"Function": function}, output, **inputs)

    def event(self, type_id, settings=None):
        return self.add(type_id, settings)

    def custom(self, name):
        """The region's custom event: written anew in place of the declare pass's node, so arranging places it with the
        region (the declaration stood for the interface until now; senders name events, not nodes)."""
        declared = self.author.declared[self.graph]
        node_id, parameters = declared["events"].pop(name)
        self.additions.insert(0, {"op": "Delete", "elements": [node_id], "bridge": False})
        return self.add("Event.Custom", {"Name": name, "Parameters": parameters})

    def send(self, name, target, graph=None, comment=None, **arguments):
        settings = {"Name": name}
        if graph is not None:
            settings["Graph"] = graph
        else:
            settings["Parameters"] = [{"Name": key, "Type": value_type} for key, (value_type, _) in arguments.items()]
            arguments = {key: value for key, (_, value) in arguments.items()}
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
        after = list(self.after)
        if self.development:
            # Cheats run in the editor and in projects, never in an exported game.
            after.append({"op": "SetEnabled", "nodes": [node for node in framed if isinstance(node, str)], "enabled": "DevelopmentOnly"})
        return (self.additions + self.connections + after + [
            {"op": "Arrange", "scope": "Created"},
            {"op": "Arrange", "scope": "Nodes", "nodes": framed},
            {"op": "AddComment", "text": self.title, "elements": framed, "color": self.color},
        ])


def track(name, type_id, keys, interpolation="Linear"):
    """A timeline track: keys are (time, value) for curves, times for event tracks."""
    if type_id == "exec":
        return {"Name": name, "Type": "exec", "Keys": [{"Time": time} for time in keys]}
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
        type)]) and functions (name, pure, inputs, outputs, locals)."""
        operations = [{"op": "SetDescription", "description": description}]
        for name, type_id, value, public, tooltip, value_range in variables:
            if public and not tooltip:
                raise SystemExit(f"the public variable {name} of {graph} has no tooltip")
            operation = {"op": "AddVariable", "name": name, "type": type_id, "public": public}
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
        for index, (name, pure, inputs, outputs, local_variables) in enumerate(functions):
            operations.append({"op": "AddFunction", "name": name, "pure": pure,
                               "inputs": [{"name": p, "type": t} for p, t in inputs],
                               "outputs": [{"name": p, "type": t} for p, t in outputs],
                               "entryAs": f"$entry{index}", "returnAs": f"$return{index}"})
            for local_name, local_type, local_value in local_variables:
                local = {"op": "AddLocal", "function": name, "name": local_name, "type": local_type}
                if local_value is not None:
                    local["value"] = local_value
                operations.append(local)
        result = self.edit(graph, "Declare the interface", operations)
        labels = result["labels"]
        self.declared[graph] = {
            "events": {name: (labels[f"$event{index}"], [{"Name": p, "Type": t} for p, t in parameters])
                       for index, (name, parameters) in enumerate(events)},
            "functions": {name: (labels[f"$entry{index}"], labels[f"$return{index}"]) for index, (name, *_) in enumerate(functions)},
        }

    def region(self, graph, title, color, function=None, development=False):
        return Region(self, graph, title, color, function, development)

    def write_region(self, region):
        self.pending.setdefault(region.graph, []).append(region)

    def finish(self, graph):
        """Writes the graph's regions: its functions first, in the order they are declared (each before the functions and
        regions that call it, so a call's node is measured as it stays: a call of a function that waits shows it), then the
        event graph's regions in the order they were written."""
        regions = self.pending.pop(graph, [])
        order = list(self.declared[graph]["functions"])
        functions = sorted((region for region in regions if region.function is not None), key=lambda region: order.index(region.function))
        for region in functions + [region for region in regions if region.function is None]:
            self.edit(region.graph, region.title, region.operations(), region.function)
        left = self.declared[graph]["events"]
        if left:
            raise SystemExit(f"{graph} declares custom events no region handles: {', '.join(left)}")


# --- Interfaces (the declare pass) -------------------------------------------------------------------------------------------

def public(name, type_id, value, tooltip, value_range=None):
    return (name, type_id, value, True, tooltip, value_range)


def private(name, type_id, value=None):
    return (name, type_id, value, False, None, None)


HUD_ELEMENTS = (
    ("GameBar", "The bar across the top that shows the score, the level and the lives during a game"),
    ("ScoreText", "The text that shows the score"),
    ("BestText", "The text that shows the best score of this session"),
    ("LevelText", "The text that shows the level"),
    ("ComboBadge", "The badge that pops up with the combo"),
    ("ComboText", "The text in the combo badge"),
    ("Life1", "The first life icon"),
    ("Life2", "The second life icon"),
    ("Life3", "The third life icon"),
    ("Life4", "The fourth life icon"),
    ("Life5", "The fifth life icon"),
    ("ServePrompt", "The prompt to launch the ball while it waits on the paddle"),
    ("TitleScreen", "The title screen, shown while the game plays itself"),
    ("TitlePrompt", "The title's prompt to start, which blinks"),
    ("Banner", "The banner that shows a cleared level and the next one"),
    ("BannerText", "The text of the level banner"),
    ("PauseScreen", "The pause screen"),
    ("OverScreen", "The game-over screen with the top five"),
    ("FinalScore", "The game-over screen's score"),
    ("TableText", "The game-over screen's table of the top five"),
)
LIVES_SHOWN = 5
ENTITY = "Strada.Entity"


def declare_all(author):
    author.declare(ARCADE, "The arcade rules every graph shares: scores, the prisms' colours, the chimes' notes and pitches, "
                   "digits in groups and the top-five table.", functions=(
        ("ScoreFor", True, (("Points", "int"), ("Combo", "int"), ("Level", "int")), (("Score", "int"),), ()),
        ("BrickColor", True, (("Hits", "int"),), (("Color", "Strada.Color"),), ()),
        ("RowNote", True, (("Row", "int"),), (("Semitones", "int"),), ()),
        ("PitchOf", True, (("Semitones", "float"),), (("Pitch", "float"),), ()),
        ("GroupDigits", False, (("Value", "int"),), (("Text", "string"),), (("Digits", "string", ""), ("Grouped", "string", ""))),
        ("InsertScore", False, (("Scores", "int[]"), ("Score", "int"), ("Keep", "int")), (("Result", "int[]"),),
         (("At", "int", 0), ("Kept", "int[]", []))),
    ))
    author.declare(CHECKS, "What the test scene's checks share: reporting a check and comparing numbers, vectors and tables.",
                   functions=(
        ("Check", False, (("Name", "string"), ("Passed", "bool"), ("Detail", "string")), (), ()),
        ("Near", True, (("A", "float"), ("B", "float"), ("Tolerance", "float")), (("Result", "bool"),), ()),
        ("NearVector", True, (("A", "Strada.Vector3"), ("B", "Strada.Vector3"), ("Tolerance", "float")), (("Result", "bool"),), ()),
        ("SameInts", True, (("A", "int[]"), ("B", "int[]")), (("Result", "bool"),), ()),
    ))
    author.declare(BRICK, "A prism: it glows in the colour of the hits it has left, chimes and flashes when the ball hits it, "
                   "and bursts into shards when it breaks.", variables=(
        public("Hits", "int", 1, "Hits it takes to break: 1 cyan, 2 magenta, 3 gold, 4 white", (1, 4)),
        public("Points", "int", 10, "Points its break scores before the combo and the level", (0, 1000)),
        public("Row", "int", 0, "Its row, counted from the back: it picks the note of its chime", (0, 15)),
        public("Game", ENTITY, None, "The entity whose Game.sgraph runs the match"),
        public("Burst", "Strada.Prefab", None, "The particle burst a break leaves"),
        public("Shard", "Strada.Prefab", None, "A shard of the burst"),
        public("ShardCount", "int", 6, "Shards a break throws out", (0, 16)),
        private("Glow", "Strada.Color", CYAN),
        private("Look", "Strada.Material"),
    ), events=(("Configure", ()),), functions=(
        ("Recolor", False, (), (), ()),
        ("Break", False, (), (), ()),
    ))
    author.declare(BALL, "A ball: it keeps the level's speed and never flies flat, the paddle aims it, and walls tick.", variables=(
        public("Game", ENTITY, None, "The entity whose Game.sgraph sets the ball's speed"),
        public("Paddle", ENTITY, None, "The paddle that aims the ball"),
        public("MinZ", "float", 0.35, "The least share of the speed along the arena's length, so the ball never flies flat", (0.1, 0.9)),
    ))
    author.declare(SHARD, "A shard of a broken prism: it bursts out, then shrinks away.", variables=(
        public("Kick", "float", 2.5, "The impulse that throws the shard out", (0.0, 20.0)),
        public("Life", "float", 1.1, "Seconds before the shard starts to shrink", (0.1, 5.0)),
        public("Fade", "float", 0.6, "Seconds the shard takes to shrink away", (0.05, 3.0)),
        private("Size", "Strada.Vector3", [1.0, 1.0, 1.0]),
    ), events=(("FadeAway", ()),))
    author.declare(POWER_UP, "A capsule that falls from a broken prism: the paddle catches it for its power, or it falls out.",
                   variables=(
        public("Kind", "string", "Widen", "Widen makes the paddle wider, Slow slows the ball"),
        public("Seconds", "float", 8.0, "How long the power lasts", (0.5, 30.0)),
        public("Game", ENTITY, None, "The entity whose Game.sgraph a Slow capsule tells"),
        public("Paddle", ENTITY, None, "The paddle that catches the capsule"),
        public("FallSpeed", "float", 4.5, "Metres per second the capsule falls towards the paddle", (0.5, 20.0)),
        public("Spin", "float", 200.0, "Degrees per second the capsule turns", (0.0, 1000.0)),
        private("Caught", "bool", False),
        private("Angle", "float", 0.0),
    ), functions=(("Catch", False, (), (), ()),))
    author.declare(PADDLE, "The paddle: the mouse, the keys or the autopilot steer it, a Widen capsule makes it wider for a "
                   "while, and it squashes when the ball hits it.", variables=(
        public("Game", ENTITY, None, "The entity whose Game.sgraph runs the match"),
        public("Balls", ENTITY, None, "The container of the live balls, which the autopilot follows"),
        public("Width", "float", PADDLE_WIDTH, "The paddle's width in metres", (1.0, 8.0)),
        public("Limit", "float", WALL_X, "The inner side walls' distance from the middle", (1.0, 20.0)),
        public("Z", "float", PADDLE_Z, "The paddle's line along the arena", (-20.0, 20.0)),
        public("KeySpeed", "float", 16.0, "Metres per second the keys move the paddle", (0.0, 60.0)),
        public("ScreenSpan", "float", 19.0, "Metres of the arena the screen's width spans for the mouse", (1.0, 60.0)),
        public("Autopilot", "bool", False, "Steers the paddle under the ball: the title plays itself"),
        private("X", "float", 0.0),
        private("Target", "float", 0.0),
        private("ColliderWidth", "float", PADDLE_WIDTH),
    ), events=(("Widen", (("Seconds", "float"),)),), functions=(("Follow", False, (), (), ()),))
    author.declare(DRAIN, "The drain behind the paddle: a ball that falls in is lost.", variables=(
        public("Game", ENTITY, None, "The entity whose Game.sgraph counts the lives"),
    ))
    author.declare(LIGHTS, "One of the arena's six lights: it pulses in the colour of every prism the ball hits, runs a rainbow "
                   "chase when a level is cleared and flashes red when a life is lost.", variables=(
        public("Base", "Strada.Color", CYAN, "The light's colour at rest"),
        public("BaseIntensity", "float", 6.0, "The light's intensity at rest", (0.0, 50.0)),
        public("Index", "int", 0, "The light's place in the rainbow chase", (0, 15)),
        private("Flash", "Strada.Color", WHITE),
        private("FlashStrength", "float", 1.0),
    ), events=(("Pulse", (("Tint", "Strada.Color"), ("Strength", "float"))), ("LevelCleared", (("Level", "int"),)),
               ("LifeLost", (("Lives", "int"),))))
    author.declare(CAMERA_RIG, "The camera rig: hits shake it, the shake settles, and it sways while the title plays itself.",
                   variables=(
        public("Home", "Strada.Vector3", CAMERA_HOME, "Where the rig stands at rest"),
        public("ShakeSize", "float", 0.35, "Metres the rig moves at full trauma", (0.0, 2.0)),
        public("Settle", "float", 1.6, "Trauma lost per second", (0.1, 10.0)),
        private("Trauma", "float", 0.0),
        private("Clock", "float", 0.0),
        private("Sway", "bool", False),
    ), events=(("Shake", (("Strength", "float"),)), ("LevelCleared", (("Level", "int"),)), ("StateChanged", (("State", "string"),))))
    author.declare(HUD, "The screen: it reads the game's score, level, combo and lives, shows the title, the prompts, the "
                   "banner, the pause screen and the top five.", variables=(
        public("Game", ENTITY, None, "The entity whose Game.sgraph the screen reads"),
    ) + tuple(public(name, ENTITY, None, tooltip) for name, tooltip in HUD_ELEMENTS) + (
        private("ShownScore", "int", -1),
        private("ShownLevel", "int", -1),
        private("ShownCombo", "int", -1),
        private("Best", "int", 0),
        private("NextLevel", "int", 2),
        private("Lines", "string[]", []),
    ), events=(("StateChanged", (("State", "string"),)), ("LevelCleared", (("Level", "int"),)), ("LifeLost", (("Lives", "int"),)),
               ("GameEnded", (("Score", "int"), ("Scores", "int[]"))), ("Paused", (("On", "bool"),)),
               ("ComboChanged", (("Combo", "int"),))), functions=(
        ("ShowScore", False, (), (), ()),
        ("ShowLives", False, (), (), ()),
    ))
    author.declare(GAME, "The match: it builds the levels, serves and launches the ball, scores the prisms, counts lives and "
                   "combos, clears levels, ends the game with its top five, pauses, and plays itself on the title.", variables=(
        public("BrickPrefab", "Strada.Prefab", None, "The prism the levels are built of"),
        public("BallPrefab", "Strada.Prefab", None, "The ball a serve puts on the paddle"),
        public("PowerUpPrefab", "Strada.Prefab", None, "The capsule a broken prism sometimes drops"),
        public("WidenLook", "Strada.Material", None, "The material of a Widen capsule"),
        public("SlowLook", "Strada.Material", None, "The material of a Slow capsule"),
        public("Bricks", ENTITY, None, "The container of the level's prisms"),
        public("Balls", ENTITY, None, "The container of the live balls"),
        public("Shards", ENTITY, None, "The container of the shards of broken prisms"),
        public("PowerUps", ENTITY, None, "The container of the falling capsules"),
        public("Paddle", ENTITY, None, "The paddle"),
        public("CameraRig", ENTITY, None, "The camera rig that hits shake"),
        public("LevelClearSound", ENTITY, None, "The sound of a cleared level"),
        public("GameOverSound", ENTITY, None, "The sound of the end of a game"),
        public("PowerUpSound", ENTITY, None, "The sound of a capsule dropping"),
        public("LaunchSound", ENTITY, None, "The sound of a launch"),
        public("Columns", "int", COLUMNS, "Prisms in a row of a level's layout", (1, 15)),
        public("Origin", "Strada.Vector3", ORIGIN, "Where the first prism of the back row stands"),
        public("Spacing", "Strada.Vector2", SPACING, "Metres between the prisms' centres, across and along the arena"),
        public("StartLives", "int", 3, "Lives a game starts with", (1, LIVES_SHOWN)),
        public("BaseSpeed", "float", 9.0, "The ball's speed on the first level, in metres per second", (2.0, 30.0)),
        public("SpeedPerLevel", "float", 1.25, "Speed the ball gains with every level", (0.0, 5.0)),
        public("PowerUpChance", "float", 0.12, "The chance that a broken prism drops a capsule", (0.0, 1.0)),
        public("WidenSeconds", "float", 8.0, "How long a Widen capsule widens the paddle", (0.5, 30.0)),
        public("SlowSeconds", "float", 6.0, "How long a Slow capsule slows the ball", (0.5, 30.0)),
        public("Seed", "int", 0, "Seeds the random numbers for a repeatable game; 0 leaves them random", (0, 1000000)),
        public("StartOnTitle", "bool", True, "Starts on the title, which plays itself; otherwise serves at once"),
        public("State", "string", "Title", "Read-out: Title, Serve, Play, Clear or Over"),
        public("Score", "int", 0, "Read-out: the score", (0, 1000000000)),
        public("Lives", "int", 3, "Read-out: the lives left", (0, 99)),
        public("Level", "int", 1, "Read-out: the level", (1, 1000)),
        public("Combo", "int", 0, "Read-out: prisms broken since the ball last touched the paddle", (0, 1000)),
        public("BricksLeft", "int", 0, "Read-out: prisms left in the level", (0, 1000)),
        public("BallSpeed", "float", 9.0, "Read-out: the ball's speed now, in metres per second", (0.0, 100.0)),
        public("LastPulseCount", "int", 0, "Read-out: the lights the last hit pulsed", (0, 100)),
        public("LastBroadcastCount", "int", 0, "Read-out: the graphs the last level clear reached", (0, 100)),
        public("Attract", "bool", False, "Read-out: the title is playing itself; the screen keeps its points out of the best"),
        private("Levels", "string[]", LEVELS),
        private("Scores", "int[]", []),
        private("LiveBalls", "Strada.Entity[]", []),
        private("Building", "bool", False),
        private("Launched", "bool", False),
        private("Paused", "bool", False),
        private("Clearing", "bool", False),
    ), events=(
        ("BrickHit", (("Tint", "Strada.Color"),)),
        ("BrickBroken", (("Points", "int"), ("At", "Strada.Vector3"), ("Row", "int"))),
        ("PaddleHit", ()),
        ("BallLost", (("Ball", ENTITY),)),
        ("SpawnPowerUp", (("Kind", "string"), ("At", "Strada.Vector3"))),
        ("SlowDown", (("Seconds", "float"),)),
        ("Launch", ()),
        ("StartGame", ()),
        ("ClearLevel", ()),
    ), functions=(
        ("BuildLevel", False, (("Number", "int"),), (), ()),
        ("Serve", False, (), (), ()),
        ("LaunchBall", False, (), (), ()),
        ("StartAttractMode", False, (), (), ()),
        ("LevelClear", False, (), (), (("WasAttract", "bool", False),)),
        ("ClearBricks", False, (), (), ()),
        ("BeginGame", False, (), (), ()),
        ("GameOver", False, (), (), ()),
        ("SpawnCapsule", False, (("Kind", "string"), ("At", "Strada.Vector3")), (), ()),
    ))
    declare_tests(author)


# --- Arcade.sgraph: the rules every graph shares --------------------------------------------------------------------------

def write_arcade(author):
    with author.region(ARCADE, "Score: points times the combo factor times the level", RULES, "ScoreFor") as b:
        b.link(b.expr("points * (10 + 5 * min(combo, 10)) * level / 10", points=b.entry["Points"], combo=b.entry["Combo"],
                      level=b.entry["Level"]), b.ret["Score"])
    with author.region(ARCADE, "Brick colour: cyan, magenta, gold, white by hits left", EFFECTS, "BrickColor") as b:
        colors = b.lit("Strada.Color[]", BRICK_COLORS)
        b.link(b.element(colors, b.expr("clamp(hits - 1, 0, 3)", hits=b.entry["Hits"])), b.ret["Color"])
    with author.region(ARCADE, "Row note: the A minor pentatonic, higher towards the back", EFFECTS, "RowNote") as b:
        notes = b.lit("int[]", [16, 14, 12, 9, 7, 4, 2, 0])
        b.link(b.element(notes, b.expr("clamp(row, 0, 7)", row=b.entry["Row"])), b.ret["Semitones"])
    with author.region(ARCADE, "Pitch: twelve semitones make an octave", EFFECTS, "PitchOf") as b:
        b.link(b.expr("pow(2, semitones / 12.0)", semitones=b.entry["Semitones"]), b.ret["Pitch"])
    with author.region(ARCADE, "Group digits: a space before every three from the right", RULES, "GroupDigits") as b:
        value = b.entry["Value"]
        digits = b.set("Digits", b.fmt("{Value}", Value=b.expr("abs(value)", value=value)))
        grouped = b.set("Grouped", b.get("Digits"))
        count = b.pure("String.Length", "Length", Text=b.get("Digits"))
        loop = b.add("Flow.ForLoop", First=1, Last=b.expr("(length - 1) / 3", length=count))
        at = b.expr("length - 3 * group", length=b.pure("String.Length", "Length", Text=b.get("Digits")), group=loop["Index"])
        head = b.pure("String.Substring", Text=b.get("Grouped"), Start=0, Length=at)
        tail = b.pure("String.Substring", Text=b.get("Grouped"), Start=at, Length=-1)
        insert = b.set("Grouped", b.fmt("{Head} {Tail}", Head=head, Tail=tail))
        sign = b.select(b.expr("value < 0", value=value), b.lit("string", "-"), b.lit("string", ""))
        b.link(b.fmt("{Sign}{Digits}", Sign=sign, Digits=b.get("Grouped")), b.ret["Text"])
        b.chain(b.entry, digits, grouped, loop)
        b.flow(loop["Body"], insert)
        b.flow(loop["Completed"], b.ret)
    with author.region(ARCADE, "Insert a score: in order, highest first, keeping the best", RULES, "InsertScore") as b:
        scores = b.entry["Scores"]
        start = b.set("At", b.length(scores))
        each = b.add("Flow.ForEach", Array=scores)
        higher = b.branch(b.expr("score > element", score=b.entry["Score"], element=each["Element"]))
        found = b.set("At", each["Index"])
        insert = b.set("Kept", b.pure("Array.Insert", Array=scores, Index=b.get("At"), Value=b.entry["Score"]))
        trim = b.add("Flow.While", Condition=b.expr("count > keep", count=b.length(b.get("Kept")), keep=b.entry["Keep"]))
        drop = b.set("Kept", b.pure("Array.RemoveAt", Array=b.get("Kept"),
                                    Index=b.expr("count - 1", count=b.length(b.get("Kept")))))
        b.chain(b.entry, start, each)
        b.flow(each["Body"], higher)
        b.flow(higher["True"], found)
        b.flow(found, each["Break"])
        b.flow(each["Completed"], insert)
        b.chain(insert, trim)
        b.flow(trim["Body"], drop)
        b.flow(trim["Completed"], b.ret)
        b.link(b.get("Kept"), b.ret["Result"])


# --- Checks.sgraph: what the checks share -----------------------------------------------------------------------------------

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
    with author.region(CHECKS, "Near vector: two points within a tolerance", CHECK, "NearVector") as b:
        b.link(b.expr("distance(a, b) <= tolerance", a=b.entry["A"], b=b.entry["B"], tolerance=b.entry["Tolerance"]), b.ret["Result"])
    with author.region(CHECKS, "Same ints: two tables element by element", CHECK, "SameInts") as b:
        b.link(b.equal(b.entry["A"], b.entry["B"]), b.ret["Result"])


# --- Brick.sgraph: a prism ------------------------------------------------------------------------------------------------

def chime_pitch(b, row, combo):
    """The pitch of a prism's chime: its row's note, one semitone higher for every prism of the combo, two octaves at most."""
    note = b.library("RowNote", output="Semitones", Row=row).out
    semitones = b.expr("min(note + combo, 24)", note=note, combo=combo)
    return b.library("PitchOf", output="Pitch", Semitones=semitones).out


def write_brick(author):
    with author.region(BRICK, "Start: each prism glows with a material of its own", EFFECTS) as b:
        start = b.event("Event.Create")
        clone = b.call("Call:Strada.Material.Clone()", "Result",
                       Target=b.pure("Call:Strada.MeshComponent.GetMaterial(int)", Index=0))
        keep = b.set("Look", clone.out)
        use = b.call("Call:Strada.MeshComponent.SetMaterial(int,Strada.Material)", Index=0, Material=b.get("Look"))
        b.chain(start, clone, keep, use, b.own("Recolor"))
    with author.region(BRICK, "Configure: the Game set the hits; show them", EVENTS) as b:
        b.chain(b.custom("Configure"), b.own("Recolor"))
    with author.region(BRICK, "Recolor: the glow follows the hits left", EFFECTS, "Recolor") as b:
        glow = b.set("Glow", b.library("BrickColor", output="Color", Hits=b.get("Hits")).out)
        emissive = b.call("Set:Strada.Material.Emissive", Target=b.get("Look"), Value=b.get("Glow"))
        b.chain(b.entry, glow, emissive, b.ret)
    with author.region(BRICK, "Hit: lose a hit, recolor, chime, juice, then flash or break", RULES) as b:
        hit = b.event("Event.CollisionEnter")
        by_ball = b.branch(b.runs(BALL, hit["Other"]))
        lose = b.set("Hits", b.expr("hits - 1", hits=b.get("Hits")))
        combo = b.get("Combo", GAME, b.get("Game"))
        pitch = b.call("Set:Strada.AudioSourceComponent.Pitch", Value=chime_pitch(b, b.get("Row"), combo))
        ring = b.call("Call:Strada.AudioSourceComponent.Play()")
        tell = b.send("BrickHit", b.get("Game"), GAME,
                      Tint=b.library("BrickColor", output="Color", Hits=b.expr("hits + 1", hits=b.get("Hits"))).out,
                      comment="Every hit juices the arena in the colour the prism had when the ball struck it")
        left = b.branch(b.expr("hits > 0", hits=b.get("Hits")))
        flash = b.timeline(0.3, [
            track("Glow", "float", [(0.0, 8.0), (0.3, 1.4)]),
            track("Squash", "Strada.Vector3", [(0.0, [1.12, 0.7, 1.12]), (0.12, [0.95, 1.08, 0.95]), (0.3, [1.0, 1.0, 1.0])]),
        ])
        glow = b.call("Set:Strada.Material.EmissiveIntensity", Target=b.get("Look"), Value=flash["Glow"])
        squash = b.call("Set:Strada.TransformComponent.Scale", Value=flash["Squash"])
        b.chain(hit, by_ball)
        b.chain(by_ball["True"], lose, b.own("Recolor"), pitch, ring, tell, left)
        b.flow(left["True"], flash["PlayFromStart"])
        b.flow(left["False"], b.own("Break"))
        b.chain(flash["Update"], glow, squash)
    with author.region(BRICK, "Break: burst, throw shards, tell the Game, go", EFFECTS, "Break") as b:
        at = b.position()
        burst = b.call("Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)", "Result",
                       Prefab=b.get("Burst"), Position=at, Rotation=IDENTITY)
        tint = b.call("Set:Strada.ParticleSystemComponent.StartColor", Target=burst.out, Value=b.get("Glow"))
        emit = b.call("Call:Strada.ParticleSystemComponent.Emit(uint)", Target=burst.out, Count=48)
        pitch = b.call("Set:Strada.AudioSourceComponent.Pitch", Target=burst.out,
                       Value=chime_pitch(b, b.get("Row"), b.get("Combo", GAME, b.get("Game"))))
        ring = b.call("Call:Strada.AudioSourceComponent.Play()", Target=burst.out)
        shatter = b.call("Call:Strada.AudioSourceComponent.Play()", Target=b.element(b.children(burst.out), 0))
        shards = b.add("Flow.ForLoop", First=1, Last=b.get("ShardCount"))
        shard = b.call("Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)", "Result",
                       Prefab=b.get("Shard"), Rotation=IDENTITY, Parent=b.get("Shards", GAME, b.get("Game")),
                       Position=b.expr("at + spread * 0.3", at=b.position(),
                                       spread=b.pure("Get:Strada.Random.InsideUnitSphere", "Value")))
        tell = b.send("BrickBroken", b.get("Game"), GAME, Points=b.get("Points"), At=b.position(), Row=b.get("Row"))
        go = b.call("Call:Strada.Entity.Destroy()")
        b.chain(b.entry, burst, tint, emit, pitch, ring, shatter, shards)
        b.flow(shards["Body"], shard)
        b.chain(shards["Completed"], tell, go, b.ret)


# --- Ball.sgraph ----------------------------------------------------------------------------------------------------------

def write_ball(author):
    with author.region(BALL, "Keep the speed and the angle: never slower, never flat", RULES) as b:
        step = b.event("Event.FixedUpdate")
        game = b.branch(b.runs(GAME, b.get("Game")))
        velocity = b.pure("Get:Strada.RigidBodyComponent.LinearVelocity", "Value")
        moving = b.branch(b.expr("length(v) > 0.5", v=velocity))
        flat = b.expr("normalize(vec3(v.x, 0, v.z))", v=b.pure("Get:Strada.RigidBodyComponent.LinearVelocity", "Value"))
        steep = b.expr("(abs(d.z) < minZ ? vec3(sign(d.x) * sqrt(1 - minZ * minZ), 0, d.z < 0 ? -minZ : minZ) : d) * speed",
                       d=flat, minZ=b.get("MinZ"), speed=b.get("BallSpeed", GAME, b.get("Game")))
        keep = b.call("Set:Strada.RigidBodyComponent.LinearVelocity", Value=steep)
        b.chain(step, game)
        b.chain(game["True"], moving)
        b.flow(moving["True"], keep)
    with author.region(BALL, "Bounce off the paddle: where it hits aims the ball", RULES) as b:
        touch = b.event("Event.CollisionEnter")
        paddle = b.branch(b.equal(touch["Other"], b.get("Paddle")))
        aim = b.expr("normalize(vec3(clamp((p.x - q.x) / (w * 0.5), -1, 1) * 1.2, 0, -1)) * speed",
                     p=b.position(), q=b.position(b.get("Paddle")), w=b.get("Width", PADDLE, b.get("Paddle")),
                     speed=b.get("BallSpeed", GAME, b.get("Game")))
        bounce = b.call("Set:Strada.RigidBodyComponent.LinearVelocity", Value=aim)
        tell = b.send("PaddleHit", b.get("Game"), GAME)
        b.chain(touch, paddle)
        b.chain(paddle["True"], bounce, tell)
    with author.region(BALL, "Sounds: walls tick as the ball leaves them", EFFECTS) as b:
        leave = b.event("Event.CollisionExit")
        wall = b.branch(b.both(b.negate(b.runs(BRICK, leave["Other"])), b.not_equal(leave["Other"], b.get("Paddle"))))
        b.chain(leave, wall)
        b.chain(wall["True"], b.call("Call:Strada.AudioSourceComponent.Play()"))


# --- Shard.sgraph ---------------------------------------------------------------------------------------------------------

def write_shard(author):
    with author.region(SHARD, "Burst out: a kick and a spin in random directions", EFFECTS) as b:
        start = b.event("Event.Create")
        kick = b.call("Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)", Mode="Impulse",
                      Force=b.expr("(direction + vec3(0, 1.5, 0)) * kick",
                                   direction=b.pure("Get:Strada.Random.OnUnitSphere", "Value"), kick=b.get("Kick")))
        spin = b.call("Call:Strada.RigidBodyComponent.AddTorque(Strada.Vector3,Strada.ForceMode)", Mode="Impulse",
                      Torque=b.expr("axis * 0.05", axis=b.pure("Get:Strada.Random.OnUnitSphere", "Value")))
        fade = b.send("FadeAway", b.self_(), comment="The shard's own event: fading waits, in a region of its own")
        b.chain(start, kick, spin, fade)
    with author.region(SHARD, "Fade and go: wait, shrink, destroy", EFFECTS) as b:
        fade = b.custom("FadeAway")
        size = b.set("Size", b.pure("Get:Strada.Entity.Scale", "Value"))
        wait = b.add("Flow.Delay", Duration=b.get("Life"))
        shrink = b.add("Flow.Tween", {"Easing": "EaseIn"}, Duration=b.get("Fade"))
        scale = b.call("Set:Strada.Entity.Scale", Value=b.expr("size * (1 - alpha)", size=b.get("Size"), alpha=shrink["Alpha"]))
        b.chain(fade, size, wait)
        b.flow(wait["Completed"], shrink)
        b.flow(shrink["Update"], scale)
        b.chain(shrink["Completed"], b.call("Call:Strada.Entity.Destroy()"))


# --- PowerUp.sgraph -------------------------------------------------------------------------------------------------------

def write_power_up(author):
    with author.region(POWER_UP, "Fall and spin: caught by the paddle, or missed", RULES) as b:
        tick = b.event("Event.Update")
        dt = tick["DeltaTime"]
        waiting = b.branch(b.negate(b.get("Caught")))
        fall = b.call("Set:Strada.Entity.Translation",
                      Value=b.expr("p + vec3(0, 0, speed * dt)", p=b.position(), speed=b.get("FallSpeed"), dt=dt))
        turn = b.set("Angle", b.expr("(angle + spin * dt) % 360", angle=b.get("Angle"), spin=b.get("Spin"), dt=dt))
        spin = b.call("Set:Strada.TransformComponent.EulerAngles", Value=b.expr("vec3(angle, 0, 90)", angle=b.get("Angle")))
        paddle = b.get("Paddle")
        caught = b.branch(b.expr("abs(p.x - q.x) < w * 0.5 + 0.35 && abs(p.z - q.z) < 0.45", p=b.position(),
                                 q=b.position(paddle), w=b.get("Width", PADDLE, b.get("Paddle"))))
        missed = b.branch(b.expr("p.z > 12", p=b.position()))
        b.chain(tick, waiting)
        b.chain(waiting["True"], fall, turn, spin, caught)
        b.flow(caught["True"], b.own("Catch"))
        b.flow(caught["False"], missed)
        b.chain(missed["True"], b.call("Call:Strada.Entity.Destroy()"))
    with author.region(POWER_UP, "Catch: the power goes to its owner, the capsule goes", RULES, "Catch") as b:
        mark = b.set("Caught", True)
        kind = b.add("Flow.Switch", {"Type": "string", "Cases": ["Widen", "Slow"]}, Selection=b.get("Kind"))
        widen = b.send("Widen", b.get("Paddle"), PADDLE, Seconds=b.get("Seconds"))
        slow = b.send("SlowDown", b.get("Game"), GAME, Seconds=b.get("Seconds"))
        hide = b.call("Set:Strada.MeshComponent.Visible", Value=False)
        dim = b.call("Set:Strada.PointLightComponent.Intensity", Value=0.0)
        ring = b.call("Call:Strada.AudioSourceComponent.Play()")
        wait = b.add("Flow.Delay", Duration=0.8, comment="The capsule stays until its sound has played")
        go = b.call("Call:Strada.Entity.Destroy()")
        b.chain(b.entry, mark, kind)
        b.flow(kind["Case:Widen"], widen)
        b.flow(kind["Case:Slow"], slow)
        for done in (widen, slow, kind["Default"]):
            b.flow(done, hide)
        b.chain(hide, dim, ring, wait)
        b.flow(wait["Completed"], go)
        b.chain(go, b.ret)


# --- Paddle.sgraph --------------------------------------------------------------------------------------------------------

def paddle_scale(b, squash=None):
    """The paddle's scale: its width across, squashed by a hit."""
    if squash is None:
        return b.expr("vec3(width, 0.4, 0.5)", width=b.get("Width"))
    return b.expr("vec3(width, 0.4 * (1 - 0.35 * sin(alpha * pi)), 0.5 * (1 + 0.2 * sin(alpha * pi)))", width=b.get("Width"),
                  alpha=squash)


def write_paddle(author):
    with author.region(PADDLE, "Steer: the mouse or the keys set a target the paddle eases to", EVENTS) as b:
        tick = b.event("Event.Update")
        dt = tick["DeltaTime"]
        keys = {}
        for name, key in (("a", "A"), ("left", "Left"), ("d", "D"), ("right", "Right")):
            keys[name] = b.pure("Call:Strada.Input.IsKeyDown(Strada.KeyCode)", Key=key)
        direction = b.expr("(d || right ? 1 : 0) - (a || left ? 1 : 0)", **keys)
        manual = b.expr("clamp(length(delta) > 0 ? (mouse.x / screen - 0.5) * span : target + direction * speed * dt, "
                        "-limit + width * 0.5, limit - width * 0.5)",
                        delta=b.pure("Get:Strada.Input.MouseDelta", "Value"), mouse=b.pure("Get:Strada.Input.MousePosition", "Value"),
                        screen=b.pure("Get:Strada.Application.WindowWidth", "Value"), span=b.get("ScreenSpan"),
                        target=b.get("Target"), direction=direction, speed=b.get("KeySpeed"), dt=dt, limit=b.get("Limit"),
                        width=b.get("Width"))
        pilot = b.branch(b.get("Autopilot"))
        steer = b.set("Target", manual, comment="The target stays within the paddle's reach, so the keys turn it round at once at a wall")
        ease = b.set("X", b.expr("clamp(lerp(x, target, 1 - exp(-25 * dt)), -limit + width * 0.5, limit - width * 0.5)",
                                 x=b.get("X"), target=b.get("Target"), dt=dt, limit=b.get("Limit"), width=b.get("Width")))
        b.chain(tick, pilot)
        b.chain(pilot["True"], b.own("Follow"), ease)
        b.chain(pilot["False"], steer, ease)
    with author.region(PADDLE, "Autopilot: follow the ball, a little off centre to aim it", TITLE, "Follow") as b:
        balls = b.children(b.get("Balls"))
        any_ball = b.branch(b.expr("count > 0", count=b.length(balls)))
        ball = b.element(b.children(b.get("Balls")), 0)
        follow = b.set("Target", b.expr("ball.x + sin(x * 0.9) * 0.9", ball=b.position(ball), x=b.get("X")))
        b.chain(b.entry, any_ball)
        b.chain(any_ball["True"], follow, b.ret)
        b.flow(any_ball["False"], b.ret)
    with author.region(PADDLE, "Move the body: each physics step carries the paddle to X", RULES) as b:
        step = b.event("Event.FixedUpdate")
        move = b.call("Call:Strada.RigidBodyComponent.MoveKinematic(Strada.Vector3,Strada.Quaternion)", Rotation=IDENTITY,
                      Position=b.expr("vec3(x, 0.5, z)", x=b.get("X"), z=b.get("Z")))
        b.chain(step, move)
    with author.region(PADDLE, "Widen: a capsule widens the paddle, which settles back", EFFECTS) as b:
        widen = b.custom("Widen")
        wide = b.timeline(8.0, [track("Width", "float", [(0.0, PADDLE_WIDTH), (0.25, 5.0), (0.4, 4.8), (7.6, 4.8), (8.0, PADDLE_WIDTH)],
                                      "Cubic")])
        b.link(b.expr("8 / seconds", seconds=widen["Seconds"]), wide["Rate"])
        width = b.set("Width", wide["Width"])
        scale = b.call("Set:Strada.TransformComponent.Scale", Value=paddle_scale(b))
        grown = b.branch(b.expr("abs(width - collider) > 0.05", width=b.get("Width"), collider=b.get("ColliderWidth")))
        rebuild = b.call("Set:Strada.BoxColliderComponent.HalfExtents", Value=[0.5, 0.5, 0.5],
                         comment="Setting the box again makes the physics rebuild it at the paddle's new width")
        keep = b.set("ColliderWidth", b.get("Width"))
        b.flow(widen, wide["PlayFromStart"])
        b.chain(wide["Update"], width, scale, grown)
        b.chain(grown["True"], rebuild, keep)
    with author.region(PADDLE, "Squash on a hit: the paddle gives and springs back", EFFECTS) as b:
        touch = b.event("Event.CollisionEnter")
        by_ball = b.branch(b.runs(BALL, touch["Other"]))
        ring = b.call("Call:Strada.AudioSourceComponent.Play()")
        squash = b.add("Flow.Tween", {"Easing": "Linear"}, Duration=0.18)
        scale = b.call("Set:Strada.TransformComponent.Scale", Value=paddle_scale(b, squash["Alpha"]))
        b.chain(touch, by_ball)
        b.chain(by_ball["True"], ring, squash)
        b.flow(squash["Update"], scale)


# --- Drain.sgraph ---------------------------------------------------------------------------------------------------------

def write_drain(author):
    with author.region(DRAIN, "A ball falls out: the Game loses it", EVENTS) as b:
        enter = b.event("Event.TriggerEnter")
        ball = b.branch(b.runs(BALL, enter["Other"]))
        lose = b.send("BallLost", b.get("Game"), GAME, Ball=enter["Other"])
        b.chain(enter, ball)
        b.chain(ball["True"], lose, b.call("Call:Strada.AudioSourceComponent.Play()"))


# --- Lights.sgraph --------------------------------------------------------------------------------------------------------

def rest_light(b):
    color = b.call("Set:Strada.PointLightComponent.Color", Value=b.get("Base"))
    intensity = b.call("Set:Strada.PointLightComponent.Intensity", Value=b.get("BaseIntensity"))
    b.chain(color, intensity)
    return color


def write_lights(author):
    with author.region(LIGHTS, "Pulse: flash in the prism's colour, fade back", EFFECTS) as b:
        pulse = b.custom("Pulse")
        flash = b.set("Flash", pulse["Tint"])
        strength = b.set("FlashStrength", pulse["Strength"])
        fade = b.add("Flow.Tween", {"Easing": "EaseOut"}, Duration=0.45)
        color = b.call("Set:Strada.PointLightComponent.Color", Value=b.pure(
            "Call:Strada.Color.Lerp(Strada.Color,Strada.Color,float)", A=b.get("Flash"), B=b.get("Base"), T=fade["Alpha"]))
        intensity = b.call("Set:Strada.PointLightComponent.Intensity", Value=b.expr(
            "base * (1 + 2.5 * strength * (1 - alpha))", base=b.get("BaseIntensity"), strength=b.get("FlashStrength"),
            alpha=fade["Alpha"]))
        b.chain(pulse, flash, strength, fade)
        b.chain(fade["Update"], color, intensity)
        b.flow(fade["Completed"], rest_light(b))
    with author.region(LIGHTS, "Rainbow chase: a cleared level runs the colours round", TITLE) as b:
        cleared = b.custom("LevelCleared")
        wait = b.add("Flow.Delay", Duration=b.expr("index * 0.08", index=b.get("Index")))
        rainbow = b.timeline(1.8, [track("Hue", "Strada.Color", [
            (0.0, [1.0, 0.1, 0.1, 1.0]), (0.3, [1.0, 0.6, 0.05, 1.0]), (0.6, [0.95, 1.0, 0.1, 1.0]), (0.9, [0.1, 1.0, 0.3, 1.0]),
            (1.2, [0.1, 0.8, 1.0, 1.0]), (1.5, [0.4, 0.2, 1.0, 1.0]), (1.8, [1.0, 0.15, 0.75, 1.0])])])
        color = b.call("Set:Strada.PointLightComponent.Color", Value=rainbow["Hue"])
        bright = b.call("Set:Strada.PointLightComponent.Intensity", Value=b.expr("base * 2", base=b.get("BaseIntensity")))
        b.chain(cleared, wait)
        b.flow(wait["Completed"], rainbow["PlayFromStart"])
        b.chain(rainbow["Update"], color, bright)
        b.flow(rainbow["Finished"], rest_light(b))
    with author.region(LIGHTS, "Warning: a lost life flashes the lights red", EFFECTS) as b:
        lost = b.custom("LifeLost")
        fade = b.add("Flow.Tween", {"Easing": "EaseIn"}, Duration=0.9)
        red = b.lit("Strada.Color", WARNING_RED)
        color = b.call("Set:Strada.PointLightComponent.Color", Value=b.pure(
            "Call:Strada.Color.Lerp(Strada.Color,Strada.Color,float)", A=red, B=b.get("Base"), T=fade["Alpha"]))
        intensity = b.call("Set:Strada.PointLightComponent.Intensity", Value=b.expr(
            "base * (1 + 3 * (1 - alpha))", base=b.get("BaseIntensity"), alpha=fade["Alpha"]))
        b.chain(lost, fade)
        b.chain(fade["Update"], color, intensity)
        b.flow(fade["Completed"], rest_light(b))


# --- CameraRig.sgraph -----------------------------------------------------------------------------------------------------

def write_camera_rig(author):
    with author.region(CAMERA_RIG, "Shake: every hit adds trauma", EFFECTS) as b:
        shake = b.custom("Shake")
        b.chain(shake, b.set("Trauma", b.expr("min(1, trauma + strength)", trauma=b.get("Trauma"), strength=shake["Strength"])))
    with author.region(CAMERA_RIG, "Settle: the offset grows with trauma squared and fades", EFFECTS) as b:
        tick = b.event("Event.Update")
        dt = tick["DeltaTime"]
        clock = b.set("Clock", b.expr("clock + dt", clock=b.get("Clock"), dt=dt))
        offset = b.expr("vec3(sin(t * 31.7), sin(t * 27.3 + 1.3), 0) * trauma * trauma * size", t=b.get("Clock"),
                        trauma=b.get("Trauma"), size=b.get("ShakeSize"))
        sway = b.select(b.get("Sway"), b.expr("vec3(sin(t * 0.37) * 0.9, sin(t * 0.23) * 0.35, 0)", t=b.get("Clock")),
                        b.lit("Strada.Vector3", [0.0, 0.0, 0.0]))
        place = b.call("Set:Strada.Entity.Translation", Value=b.expr("home + offset + sway", home=b.get("Home"), offset=offset,
                                                                       sway=sway))
        settle = b.set("Trauma", b.expr("max(0, trauma - dt * settle)", trauma=b.get("Trauma"), dt=dt, settle=b.get("Settle")))
        b.chain(tick, clock, place, settle)
    with author.region(CAMERA_RIG, "Clear: a cleared level shakes the camera hard", EFFECTS) as b:
        b.chain(b.custom("LevelCleared"), b.set("Trauma", 0.8))
    with author.region(CAMERA_RIG, "Title sway: the camera drifts while the title plays", TITLE) as b:
        changed = b.custom("StateChanged")
        b.chain(changed, b.set("Sway", b.equal(changed["State"], "Title")))


# --- Hud.sgraph -----------------------------------------------------------------------------------------------------------

def show(b, element, visible):
    return b.call("Set:Strada.UIRectComponent.Visible", Target=b.get(element), Value=visible)


def text(b, element, value):
    return b.call("Set:Strada.UITextComponent.Text", Target=b.get(element), Value=value)


def write_hud(author):
    with author.region(HUD, "Read the game: a text changes only when its value did", EVENTS) as b:
        tick = b.event("Event.Update")
        game = b.branch(b.runs(GAME, b.get("Game")))
        score = b.branch(b.not_equal(b.get("Score", GAME, b.get("Game")), b.get("ShownScore")))
        level = b.branch(b.not_equal(b.get("Level", GAME, b.get("Game")), b.get("ShownLevel")))
        shown_level = b.set("ShownLevel", b.get("Level", GAME, b.get("Game")))
        level_text = text(b, "LevelText", b.fmt("LEVEL {Level}", Level=b.get("ShownLevel")))
        combo = b.branch(b.not_equal(b.get("Combo", GAME, b.get("Game")), b.get("ShownCombo")))
        shown_combo = b.set("ShownCombo", b.get("Combo", GAME, b.get("Game")))
        tell = b.send("ComboChanged", b.self_(), Combo=("int", b.get("ShownCombo")),
                      comment="The screen's own event: the badge pops in a region of its own")
        b.chain(tick, game)
        b.flow(game["True"], score)
        b.chain(score["True"], b.own("ShowScore"), level)
        b.flow(score["False"], level)
        b.chain(level["True"], shown_level, level_text, combo)
        b.flow(level["False"], combo)
        b.chain(combo["True"], shown_combo, tell)
    with author.region(HUD, "Show the score: in groups of three digits, with the best", RULES, "ShowScore") as b:
        game = b.branch(b.runs(GAME, b.get("Game")))
        shown = b.set("ShownScore", b.get("Score", GAME, b.get("Game")))
        digits = b.library("GroupDigits", output="Text", Value=b.get("ShownScore"))
        score = text(b, "ScoreText", digits.out)
        best = b.set("Best", b.expr("demo ? best : max(best, score)", demo=b.get("Attract", GAME, b.get("Game")), best=b.get("Best"),
                                    score=b.get("ShownScore")),
                     comment="The points the title scores while it plays itself are no player's best")
        best_digits = b.library("GroupDigits", output="Text", Value=b.get("Best"))
        best_text = text(b, "BestText", best_digits.out)
        b.chain(b.entry, game)
        b.chain(game["True"], shown, digits, score, best, best_digits, best_text, b.ret)
        b.flow(game["False"], b.ret)
    with author.region(HUD, "Show the lives: one icon for every life left", RULES, "ShowLives") as b:
        game = b.branch(b.runs(GAME, b.get("Game")))
        icons = b.add("Array.Make", {"Type": ENTITY, "Count": LIVES_SHOWN}, "Array",
                      **{f"Element{index}": b.get(f"Life{index + 1}") for index in range(LIVES_SHOWN)})
        each = b.add("Flow.ForEach", Array=icons.out)
        visible = b.call("Set:Strada.UIRectComponent.Visible", Target=each["Element"],
                         Value=b.expr("index < lives", index=each["Index"], lives=b.get("Lives", GAME, b.get("Game"))))
        b.chain(b.entry, game)
        b.chain(game["True"], each)
        b.flow(each["Body"], visible)
        b.flow(each["Completed"], b.ret)
        b.flow(game["False"], b.ret)
    with author.region(HUD, "Combo pop: the badge pops at every prism, hides at the paddle", EFFECTS) as b:
        changed = b.custom("ComboChanged")
        shown = b.branch(b.expr("combo >= 2", combo=changed["Combo"]))
        label = text(b, "ComboText", b.fmt("×{Combo}", Combo=changed["Combo"]))
        visible = show(b, "ComboBadge", True)
        pop = b.timeline(0.35, [track("Size", "float", [(0.0, 1.6), (0.12, 0.88), (0.24, 1.06), (0.35, 1.0)], "Cubic")])
        size = b.call("Set:Strada.UIRectComponent.Size", Target=b.get("ComboBadge"),
                      Value=b.expr("vec2(200 * s, 92 * s)", s=pop["Size"]))
        font = b.call("Set:Strada.UITextComponent.FontSize", Target=b.get("ComboText"), Value=b.expr("60 * s", s=pop["Size"]))
        hidden = show(b, "ComboBadge", False)
        b.chain(changed, shown)
        b.chain(shown["True"], label, visible)
        b.flow(visible, pop["PlayFromStart"])
        b.chain(pop["Update"], size, font)
        b.flow(shown["False"], hidden)
    with author.region(HUD, "Lives: a lost life takes an icon away", EVENTS) as b:
        b.chain(b.custom("LifeLost"), b.own("ShowLives"))
    with author.region(HUD, "Panels: each state of the game shows its screen", EVENTS) as b:
        changed = b.custom("StateChanged")
        state = changed["State"]
        title = show(b, "TitleScreen", b.equal(state, "Title"))
        bar = show(b, "GameBar", b.not_equal(state, "Title"))
        serve = show(b, "ServePrompt", b.equal(state, "Serve"))
        over = show(b, "OverScreen", b.equal(state, "Over"))
        pause = show(b, "PauseScreen", False)
        b.chain(changed, title, bar, serve, over, pause, b.own("ShowLives"), b.own("ShowScore"))
    with author.region(HUD, "Level banner: LEVEL CLEAR, then the next level's number", TITLE) as b:
        cleared = b.custom("LevelCleared")
        played = b.branch(b.both(b.runs(GAME, b.get("Game")), b.negate(b.get("Attract", GAME, b.get("Game")))),
                          comment="The title stays clear of the banner while it plays itself")
        next_level = b.set("NextLevel", b.expr("level + 1", level=cleared["Level"]))
        clear_text = text(b, "BannerText", "LEVEL CLEAR")
        visible = show(b, "Banner", True)
        banner = b.timeline(2.6, [track("Opacity", "float", [(0.0, 0.0), (0.2, 1.0), (2.2, 1.0), (2.6, 0.0)]),
                                  track("Swap", "exec", [1.1])])
        opacity = b.call("Set:Strada.UIRectComponent.Opacity", Target=b.get("Banner"), Value=banner["Opacity"])
        next_text = text(b, "BannerText", b.fmt("LEVEL {Level}", Level=b.get("NextLevel")))
        hidden = show(b, "Banner", False)
        b.chain(cleared, played)
        b.chain(played["True"], next_level, clear_text, visible)
        b.flow(visible, banner["PlayFromStart"])
        b.flow(banner["Update"], opacity)
        b.flow(banner["Swap"], next_text)
        b.flow(banner["Finished"], hidden)
    with author.region(HUD, "Game over: the final score and the top five", RULES) as b:
        ended = b.custom("GameEnded")
        final_digits = b.library("GroupDigits", output="Text", Value=ended["Score"])
        final = text(b, "FinalScore", final_digits.out)
        clear = b.set("Lines", b.lit("string[]", []))
        each = b.add("Flow.ForEach", Array=ended["Scores"])
        digits = b.library("GroupDigits", output="Text", Value=each["Element"])
        line = b.set("Lines", b.pure("Array.Add", Array=b.get("Lines"),
                                     Value=b.fmt("{Rank}.   {Points}", Rank=b.expr("index + 1", index=each["Index"]), Points=digits.out)))
        table = text(b, "TableText", b.pure("String.Join", Parts=b.get("Lines"), Separator="\n"))
        b.chain(ended, final_digits, final, clear, each)
        b.chain(each["Body"], digits, line)
        b.flow(each["Completed"], table)
    with author.region(HUD, "Pause: the pause screen follows the game", EVENTS) as b:
        paused = b.custom("Paused")
        b.chain(paused, show(b, "PauseScreen", paused["On"]))
    with author.region(HUD, "Prompt blink: the prompts breathe", TITLE) as b:
        start = b.event("Event.Create")
        blink = b.timeline(1.4, [track("Alpha", "float", [(0.0, 1.0), (0.7, 0.25), (1.4, 1.0)], "Cubic")], loop=True)
        title = b.call("Set:Strada.UIRectComponent.Opacity", Target=b.get("TitlePrompt"), Value=blink["Alpha"])
        serve = b.call("Set:Strada.UIRectComponent.Opacity", Target=b.get("ServePrompt"), Value=blink["Alpha"])
        b.flow(start, blink["PlayFromStart"])
        b.chain(blink["Update"], title, serve)


# --- Game.sgraph ----------------------------------------------------------------------------------------------------------

INSTANTIATE = "Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)"


def level_speed(b):
    """The ball's speed on the current level."""
    return b.expr("base + per_level * (level - 1)", base=b.get("BaseSpeed"), per_level=b.get("SpeedPerLevel"), level=b.get("Level"))


def change_state(b, state):
    """Sets the State and tells every graph that listens; returns (first, last) of the chain."""
    store = b.set("State", state)
    tell = b.broadcast("StateChanged", parameters=(("State", "string"),), State=state)
    b.flow(store, tell)
    return store, tell


def destroy_balls(b):
    """Destroys the live balls; returns (first, last) of the chain."""
    each = b.add("Flow.ForEach", Array=b.get("LiveBalls"))
    b.flow(each["Body"], b.call("Call:Strada.Entity.Destroy()", Target=each["Element"]))
    empty = b.set("LiveBalls", b.lit("Strada.Entity[]", []))
    landed = b.set("Launched", False)
    b.flow(each["Completed"], empty)
    b.flow(empty, landed)
    return each, landed


def write_game(author):
    with author.region(GAME, "Start: seed, build the first level, then the title or a serve", EVENTS) as b:
        start = b.event("Event.Create")
        seeded = b.branch(b.not_equal(b.get("Seed"), 0))
        seed = b.call("Call:Strada.Random.Seed(int)", Seed=b.get("Seed"))
        lives = b.set("Lives", b.get("StartLives"))
        level = b.set("Level", 1)
        speed = b.set("BallSpeed", level_speed(b))
        build = b.own("BuildLevel", Number=1)
        title = b.branch(b.get("StartOnTitle"))
        b.chain(start, seeded)
        b.chain(seeded["True"], seed, lives)
        b.flow(seeded["False"], lives)
        b.chain(lives, level, speed, build, title)
        b.flow(title["True"], b.own("StartAttractMode"))
        b.flow(title["False"], b.own("Serve"))
    with author.region(GAME, "Build a level: its layout, row by row, prism by prism", RULES, "BuildLevel") as b:
        building = b.set("Building", True)
        old = b.add("Flow.ForEach", Array=b.children(b.get("Bricks")))
        remove = b.call("Call:Strada.Entity.Destroy()", Target=old["Element"])
        none_left = b.set("BricksLeft", 0)
        layout = b.element(b.get("Levels"), b.expr("(number - 1) % count", number=b.entry["Number"], count=b.length(b.get("Levels"))))
        rows = b.add("Flow.ForEach", Array=b.pure("String.Split", "Parts", Text=layout, Separator="/"))
        columns = b.add("Flow.ForLoop", First=0, Last=b.expr("columns - 1", columns=b.get("Columns")))
        cell = b.add("String.ToInt", Text=b.pure("String.Substring", Text=rows["Element"], Start=columns["Index"], Length=1))
        prism = b.branch(b.expr("ok && hits > 0", ok=cell["Success"], hits=cell["Value"]))
        brick = b.call(INSTANTIATE, "Result", Prefab=b.get("BrickPrefab"), Rotation=IDENTITY, Parent=b.get("Bricks"),
                       Position=b.expr("origin + vec3(column * spacing.x, 0, row * spacing.y)", origin=b.get("Origin"),
                                       column=columns["Index"], spacing=b.get("Spacing"), row=rows["Index"]))
        hits = b.set("Hits", cell["Value"], BRICK, brick.out)
        points = b.set("Points", b.expr("hits * 10", hits=cell["Value"]), BRICK, brick.out)
        row = b.set("Row", rows["Index"], BRICK, brick.out)
        owner = b.set("Game", b.self_(), BRICK, brick.out)
        configure = b.send("Configure", brick.out, BRICK)
        count = b.set("BricksLeft", b.expr("left + 1", left=b.get("BricksLeft")))
        pop = b.add("Flow.Delay", Duration=0.015, comment="The prisms pop in one by one while the loops wait here")
        built = b.set("Building", False)
        b.chain(b.entry, building, old)
        b.flow(old["Body"], remove)
        b.chain(old["Completed"], none_left, rows)
        b.flow(rows["Body"], columns)
        b.flow(columns["Body"], prism)
        b.chain(prism["True"], brick, hits, points, row, owner, configure, count, pop)
        b.chain(rows["Completed"], built, b.ret)
    with author.region(GAME, "Serve: a new ball waits on the paddle", RULES, "Serve") as b:
        first, last = destroy_balls(b)
        ball = b.call(INSTANTIATE, "Result", Prefab=b.get("BallPrefab"), Rotation=IDENTITY, Parent=b.get("Balls"),
                      Position=b.expr("vec3(p.x, 0.5, p.z - 0.6)", p=b.position(b.get("Paddle"))))
        owner = b.set("Game", b.self_(), BALL, ball.out)
        paddle = b.set("Paddle", b.get("Paddle"), BALL, ball.out)
        live = b.set("LiveBalls", b.pure("Array.Add", Array=b.get("LiveBalls"), Value=ball.out))
        speed = b.set("BallSpeed", level_speed(b))
        combo = b.set("Combo", 0)
        state, told = change_state(b, "Serve")
        b.chain(b.entry, first)
        b.chain(last, ball, owner, paddle, live, speed, combo, state)
        b.flow(told, b.ret)
    with author.region(GAME, "Serve: the ball rides the paddle until it is launched", RULES) as b:
        tick = b.event("Event.Update")
        launched = b.branch(b.get("Launched"))
        any_ball = b.branch(b.expr("count > 0", count=b.length(b.get("LiveBalls"))))
        ball = b.element(b.get("LiveBalls"), 0)
        ride = b.call("Call:Strada.RigidBodyComponent.Teleport(Strada.Vector3,Strada.Quaternion)", Target=ball, Rotation=IDENTITY,
                      Position=b.expr("vec3(p.x, 0.5, p.z - 0.6)", p=b.position(b.get("Paddle"))))
        still = b.call("Set:Strada.RigidBodyComponent.LinearVelocity", Target=b.element(b.get("LiveBalls"), 0), Value=[0.0, 0.0, 0.0])
        b.chain(tick, launched)
        b.flow(launched["False"], any_ball)
        b.chain(any_ball["True"], ride, still)
    with author.region(GAME, "Input: Space and a click start, or launch, by state", EVENTS) as b:
        space = b.event("Event.KeyPressed", {"Key": "Space"})
        click = b.event("Event.MouseButtonPressed", {"Button": "Left"})
        busy = b.branch(b.either(b.get("Building"), b.get("Paused")))
        by_state = b.add("Flow.Switch", {"Type": "string", "Cases": ["Title", "Over", "Serve"]}, Selection=b.get("State"))
        begin = b.own("BeginGame")
        b.flow(space, busy)
        b.flow(click, busy)
        b.flow(busy["False"], by_state)
        b.flow(by_state["Case:Title"], begin)
        b.flow(by_state["Case:Over"], begin)
        b.flow(by_state["Case:Serve"], b.own("LaunchBall"))
        b.chain(b.custom("Launch"), b.own("LaunchBall"))
        b.chain(b.custom("StartGame"), b.own("BeginGame"))
    with author.region(GAME, "Launch: the ball leaves the paddle at the level's speed", RULES, "LaunchBall") as b:
        launched = b.branch(b.get("Launched"))
        any_ball = b.branch(b.expr("count > 0", count=b.length(b.get("LiveBalls"))))
        mark = b.set("Launched", True)
        velocity = b.call("Set:Strada.RigidBodyComponent.LinearVelocity", Target=b.element(b.get("LiveBalls"), 0),
                          Value=b.expr("normalize(vec3(spread, 0, -1)) * speed", speed=b.get("BallSpeed"),
                                       spread=b.pure("Call:Strada.Random.Range(float,float)", Min=-0.35, Max=0.35)))
        sound = b.call("Call:Strada.AudioSourceComponent.Play()", Target=b.get("LaunchSound"))
        serving = b.branch(b.equal(b.get("State"), "Serve"))
        state, told = change_state(b, "Play")
        b.chain(b.entry, launched)
        b.flow(launched["False"], any_ball)
        b.chain(any_ball["True"], mark, velocity, sound, serving)
        b.flow(serving["True"], state)
        for done in (told, serving["False"], any_ball["False"], launched["True"]):
            b.flow(done, b.ret)
    with author.region(GAME, "Bricks: points by the combo and the level, then a clear", RULES) as b:
        broken = b.custom("BrickBroken")
        points = b.library("ScoreFor", output="Score", Points=broken["Points"], Combo=b.get("Combo"), Level=b.get("Level"))
        score = b.set("Score", b.expr("score + points", score=b.get("Score"), points=points.out))
        combo = b.set("Combo", b.expr("combo + 1", combo=b.get("Combo")))
        left = b.set("BricksLeft", b.expr("left - 1", left=b.get("BricksLeft")))
        lucky = b.branch(b.expr("roll < chance", roll=b.pure("Get:Strada.Random.Value", "Value"), chance=b.get("PowerUpChance")))
        kind = b.select(b.expr("roll < 0.5", roll=b.pure("Get:Strada.Random.Value", "Value")), b.lit("string", "Widen"),
                        b.lit("string", "Slow"))
        capsule = b.own("SpawnCapsule", Kind=kind, At=broken["At"])
        empty = b.branch(b.expr("left <= 0 && !clearing", left=b.get("BricksLeft"), clearing=b.get("Clearing")))
        b.chain(broken, score, combo, left, lucky)
        b.chain(lucky["True"], capsule, empty)
        b.flow(lucky["False"], empty)
        b.flow(empty["True"], b.own("LevelClear"))
    with author.region(GAME, "Juice: every hit pulses the lights and kicks the camera", EFFECTS) as b:
        hit = b.custom("BrickHit")
        pulse = b.broadcast("Pulse", LIGHTS, Tint=hit["Tint"], Strength=1.0)
        count = b.set("LastPulseCount", pulse.out)
        kick = b.send("Shake", b.get("CameraRig"), CAMERA_RIG, Strength=0.3)
        b.chain(hit, pulse, count, kick)
    with author.region(GAME, "Capsules: a broken prism sometimes drops a power", EFFECTS, "SpawnCapsule") as b:
        widen = b.equal(b.entry["Kind"], "Widen")
        capsule = b.call(INSTANTIATE, "Result", Prefab=b.get("PowerUpPrefab"), Rotation=IDENTITY, Parent=b.get("PowerUps"),
                         Position=b.expr("vec3(at.x, 0.5, at.z)", at=b.entry["At"]))
        kind = b.set("Kind", b.entry["Kind"], POWER_UP, capsule.out)
        seconds = b.set("Seconds", b.select(widen, b.get("WidenSeconds"), b.get("SlowSeconds")), POWER_UP, capsule.out)
        owner = b.set("Game", b.self_(), POWER_UP, capsule.out)
        paddle = b.set("Paddle", b.get("Paddle"), POWER_UP, capsule.out)
        look = b.call("Call:Strada.MeshComponent.SetMaterial(int,Strada.Material)", Target=capsule.out, Index=0,
                      Material=b.select(b.equal(b.entry["Kind"], "Widen"), b.get("WidenLook"), b.get("SlowLook")))
        glow = b.call("Set:Strada.PointLightComponent.Color", Target=capsule.out,
                      Value=b.select(b.equal(b.entry["Kind"], "Widen"), b.lit("Strada.Color", [0.2, 1.0, 0.4, 1.0]),
                                     b.lit("Strada.Color", [1.0, 0.5, 0.08, 1.0])))
        sound = b.call("Call:Strada.AudioSourceComponent.Play()", Target=b.get("PowerUpSound"))
        b.chain(b.entry, capsule, kind, seconds, owner, paddle, look, glow, sound, b.ret)
    with author.region(GAME, "Capsules on request: the tests and the cheats drop one", EVENTS) as b:
        spawn = b.custom("SpawnPowerUp")
        b.chain(spawn, b.own("SpawnCapsule", Kind=spawn["Kind"], At=spawn["At"]))
    with author.region(GAME, "Slow motion: the ball slows, then speeds up again", EFFECTS) as b:
        slow = b.custom("SlowDown")
        timeline = b.timeline(1.0, [track("Factor", "float", [(0.0, 1.0), (0.1, 0.55), (0.85, 0.55), (1.0, 1.0)])])
        b.link(b.expr("1 / seconds", seconds=slow["Seconds"]), timeline["Rate"])
        slowed = b.set("BallSpeed", b.expr("speed * factor", speed=level_speed(b), factor=timeline["Factor"]))
        back = b.set("BallSpeed", level_speed(b))
        b.flow(slow, timeline["PlayFromStart"])
        b.flow(timeline["Update"], slowed)
        b.flow(timeline["Finished"], back)
    with author.region(GAME, "Combo: the paddle ends it", RULES) as b:
        hit = b.custom("PaddleHit")
        b.chain(hit, b.set("Combo", 0, comment="A combo counts the prisms one ball breaks between two touches of the paddle, "
                                               "so a touch of the paddle starts it again"))
    with author.region(GAME, "Balls and lives: a lost ball costs a life, then a serve", RULES) as b:
        lost = b.custom("BallLost")
        ball = lost["Ball"]
        known = b.branch(b.expr("index >= 0", index=b.pure("Array.IndexOf", "Index", Array=b.get("LiveBalls"), Value=ball)))
        remove = b.set("LiveBalls", b.pure("Array.RemoveAt", Array=b.get("LiveBalls"),
                                           Index=b.pure("Array.IndexOf", "Index", Array=b.get("LiveBalls"), Value=ball)))
        destroy = b.call("Call:Strada.Entity.Destroy()", Target=ball)
        last = b.branch(b.expr("count == 0", count=b.length(b.get("LiveBalls"))))
        landed = b.set("Launched", False)
        attract = b.branch(b.get("Attract"))
        again = b.add("Flow.Delay", Duration=0.6)
        still = b.branch(b.get("Attract"), comment="A player who started meanwhile keeps the game: the title's demo ends here")
        life = b.set("Lives", b.expr("lives - 1", lives=b.get("Lives")))
        tell = b.broadcast("LifeLost", parameters=(("Lives", "int"),), Lives=b.get("Lives"))
        wait = b.add("Flow.Delay", Duration=1.0)
        more = b.branch(b.expr("lives > 0", lives=b.get("Lives")))
        b.chain(lost, known)
        b.chain(known["True"], remove, destroy, last)
        b.chain(last["True"], landed, attract)
        b.flow(attract["True"], again)
        b.flow(again["Completed"], still)
        b.flow(still["True"], b.own("StartAttractMode"))
        b.chain(attract["False"], life, tell, wait)
        b.flow(wait["Completed"], more)
        b.flow(more["True"], b.own("Serve"))
        b.flow(more["False"], b.own("GameOver"))
    with author.region(GAME, "Level clear: tell every graph, wait, build the next", RULES, "LevelClear") as b:
        clearing = b.set("Clearing", True)
        was = b.set("WasAttract", b.get("Attract"))
        demo = b.branch(b.get("WasAttract"), comment="The title stays up while its demo clears a level")
        state, told = change_state(b, "Clear")
        first, last = destroy_balls(b)
        cleared = b.broadcast("LevelCleared", parameters=(("Level", "int"),), Level=b.get("Level"))
        count = b.set("LastBroadcastCount", cleared.out)
        sound = b.call("Call:Strada.AudioSourceComponent.Play()", Target=b.get("LevelClearSound"))
        wait = b.add("Flow.Delay", Duration=2.2)
        same = b.branch(b.equal(b.get("Attract"), b.get("WasAttract")),
                        comment="A player who started during the demo's clear has a game of their own: this clear ends here")
        level = b.set("Level", b.expr("level + 1", level=b.get("Level")))
        build = b.own("BuildLevel", Number=b.get("Level"))
        attract = b.branch(b.get("Attract"))
        done = b.set("Clearing", False)
        b.chain(b.entry, clearing, was, demo)
        b.flow(demo["True"], first)
        b.flow(demo["False"], state)
        b.chain(told, first)
        b.chain(last, cleared, count, sound, wait)
        b.chain(wait["Completed"], same)
        b.chain(same["True"], level, build, attract)
        b.flow(same["False"], done)
        b.chain(attract["True"], b.own("StartAttractMode"), done)
        b.chain(attract["False"], b.own("Serve"), done)
        b.flow(done, b.ret)
    with author.region(GAME, "Game over: the score joins the top five", RULES, "GameOver") as b:
        state, told = change_state(b, "Over")
        first, last = destroy_balls(b)
        insert = b.library("InsertScore", output="Result", Scores=b.get("Scores"), Score=b.get("Score"), Keep=5)
        scores = b.set("Scores", insert.out)
        ended = b.broadcast("GameEnded", parameters=(("Score", "int"), ("Scores", "int[]")), Score=b.get("Score"), Scores=b.get("Scores"))
        sound = b.call("Call:Strada.AudioSourceComponent.Play()", Target=b.get("GameOverSound"))
        b.chain(b.entry, first)
        b.chain(last, insert, scores, ended, sound, state)
        b.flow(told, b.ret)
    with author.region(GAME, "A new game: fresh score and lives, the first level", RULES, "BeginGame") as b:
        attract = b.set("Attract", False)
        paused = b.set("Paused", False)
        time = b.call("Set:Strada.Time.TimeScale", Value=1.0)
        score = b.set("Score", 0)
        lives = b.set("Lives", b.get("StartLives"))
        level = b.set("Level", 1)
        combo = b.set("Combo", 0)
        pilot = b.set("Autopilot", False, PADDLE, b.get("Paddle"))
        first, last = destroy_balls(b)
        capsules = b.add("Flow.ForEach", Array=b.children(b.get("PowerUps")))
        remove = b.call("Call:Strada.Entity.Destroy()", Target=capsules["Element"])
        state, told = change_state(b, "Serve")
        build = b.own("BuildLevel", Number=1)
        b.chain(b.entry, attract, paused, time, score, lives, level, combo, pilot, first)
        b.chain(last, capsules)
        b.flow(capsules["Body"], remove)
        b.flow(capsules["Completed"], state)
        b.chain(told, build, b.own("Serve"), b.ret)
    with author.region(GAME, "Pause: Escape freezes the game and shows the pause screen", EVENTS) as b:
        escape = b.event("Event.KeyPressed", {"Key": "Escape"})
        playing = b.branch(b.either(b.equal(b.get("State"), "Serve"), b.equal(b.get("State"), "Play")))
        toggle = b.set("Paused", b.negate(b.get("Paused")))
        time = b.call("Set:Strada.Time.TimeScale", Value=b.select(b.get("Paused"), b.lit("float", 0.0), b.lit("float", 1.0)))
        tell = b.broadcast("Paused", parameters=(("On", "bool"),), On=b.get("Paused"))
        b.chain(escape, playing)
        b.chain(playing["True"], toggle, time, tell)
    with author.region(GAME, "Title: the game plays itself until a player starts", TITLE, "StartAttractMode") as b:
        attract = b.set("Attract", True)
        pilot = b.set("Autopilot", True, PADDLE, b.get("Paddle"))
        state, told = change_state(b, "Title")
        wait = b.add("Flow.Delay", Duration=1.2)
        still = b.branch(b.get("Attract"))
        b.chain(b.entry, attract, pilot, b.own("Serve"), state)
        b.flow(told, wait)
        b.flow(wait["Completed"], still)
        b.chain(still["True"], b.own("LaunchBall"), b.ret)
        b.flow(still["False"], b.ret)
    with author.region(GAME, "Clear on request: the tests empty the level", EVENTS) as b:
        b.chain(b.custom("ClearLevel"), b.own("ClearBricks"))
    with author.region(GAME, "Clear the prisms: all go at once, and the level is clear", RULES, "ClearBricks") as b:
        each = b.add("Flow.ForEach", Array=b.children(b.get("Bricks")))
        remove = b.call("Call:Strada.Entity.Destroy()", Target=each["Element"])
        none_left = b.set("BricksLeft", 0)
        open_level = b.branch(b.negate(b.get("Clearing")))
        b.chain(b.entry, each)
        b.flow(each["Body"], remove)
        b.chain(each["Completed"], none_left, open_level)
        b.chain(open_level["True"], b.own("LevelClear"), b.ret)
        b.flow(open_level["False"], b.ret)
    with author.region(GAME, "Cheats: 1 and 2 drop capsules, N clears the level", DEVELOPMENT, development=True) as b:
        above = "vec3(p.x, 0.5, p.z - 5)"
        for key, kind in (("D1", "Widen"), ("D2", "Slow")):
            pressed = b.event("Event.KeyPressed", {"Key": key})
            b.chain(pressed, b.own("SpawnCapsule", Kind=kind, At=b.expr(above, p=b.position(b.get("Paddle")))))
        b.chain(b.event("Event.KeyPressed", {"Key": "N"}), b.own("ClearBricks"))


# --- Tests.sgraph: the test scene's checks --------------------------------------------------------------------------------

def shared(b, key, make):
    """A value a region reads in several places, made once: a pure node evaluates again for every consumer, so one node
    serves a read before a wait and after it."""
    if key not in b.shared:
        b.shared[key] = make()
    return b.shared[key]


def var(b, name):
    """One of the graph's own variables, read by one node per region."""
    return shared(b, ("var", name), lambda: b.get(name))


def of_game(b, variable):
    """A read-out of the Game (a remote Get)."""
    return shared(b, ("game", variable), lambda: b.get(variable, GAME, var(b, "Game")))


def the_ball(b):
    return shared(b, "ball", lambda: b.element(b.children(var(b, "Balls")), 0))


def position_of(b, name):
    return shared(b, ("position", name), lambda: b.position(var(b, name)))


def count_of(b, container):
    return shared(b, ("count", container), lambda: b.length(b.children(var(b, container))))


def hits_of(b, brick):
    return shared(b, ("hits", brick), lambda: b.get("Hits", BRICK, var(b, brick)))


def glow_of(b, brick):
    """The emissive colour of a prism's own material."""
    look = b.pure("Call:Strada.MeshComponent.GetMaterial(int)", Target=var(b, brick), Index=0)
    return b.pure("Get:Strada.Material.Emissive", "Value", Target=look)


def width_of_paddle(b):
    return b.expr("scale.x", scale=b.pure("Get:Strada.Entity.Scale", "Value", Target=var(b, "Paddle")))


def near(b, value, expected, tolerance):
    return b.library("Near", CHECKS, output="Result", A=value, B=expected, Tolerance=tolerance).out


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


def wait_seconds(b, seconds):
    return wait_until(b, None, seconds)


def check(b, name, done, limit, detail, passed=None):
    """Waits for done (at most limit seconds), then reports the check; passed defaults to done."""
    first, waited = wait_until(b, done, limit)
    reported = b.library("Check", CHECKS, Name=f"Prism Break: {name}", Passed=done if passed is None else passed, Detail=detail)
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


def find_brick(b, row, column, variable="Target"):
    """Stores the prism of a layout's row and column in a variable; returns (first, last) of the chain."""
    found = b.own("FindBrick", output="Brick", Row=row, Column=column)
    keep = b.set(variable, found.out)
    b.flow(found, keep)
    return found, keep


def strike(b, brick):
    return b.own("Strike", At=position_of(b, brick))


def place(b, at, velocity):
    return b.own("Place", At=at, Velocity=velocity)


def onto_paddle(b):
    """Sends the ball onto the paddle from just in front of it."""
    return place(b, shared(b, "onto the paddle", lambda: b.expr("vec3(p.x, 0.5, p.z - 1.0)", p=position_of(b, "Paddle"))),
                 shared(b, "towards the paddle", lambda: b.expr("vec3(0, 0, speed)", speed=of_game(b, "BallSpeed"))))


def into_drain(b):
    return place(b, b.lit("Strada.Vector3", [0.0, 0.5, 10.7]), b.expr("vec3(0, 0, speed)", speed=of_game(b, "BallSpeed")))


def gone(b, variable="Target"):
    return shared(b, ("gone", variable), lambda: b.negate(b.pure("Get:Strada.Entity.IsValid", "Value", Target=var(b, variable))))


def is_state(b, state):
    return shared(b, ("state", state), lambda: b.equal(of_game(b, "State"), state))


# The checks in the order they run: (function, title of its box, the check's name in the report). Each function's body is
# written by its writer in TEST_WRITERS.
TEST_CHECKS = (
    ("CheckLevelIsBuilt", "Level built: 33 prisms once the Game serves", "the first level is built from its layout"),
    ("CheckBricksAreEntities", "Bricks container: one child per prism left",
     "every brick of the level is an entity of the bricks container"),
    ("CheckBrickGlow", "Glow: a gold prism's material glows BrickColor(3)", "bricks glow in the colour of their hits"),
    ("CheckScoreFormula", "Score formula: ScoreFor(50, 3, 2) is 250", "scores grow with the combo and the level"),
    ("CheckPitches", "Pitches: an octave doubles, a fifth is 1.4983", "pitches rise by semitones"),
    ("CheckDigitGroups", "Digit groups: 1 234 567, -45 and 0", "scores show in groups of three digits"),
    ("CheckTopFive", "Top five: sorted, and no longer than kept", "the top five stays sorted and five long"),
    ("CheckHudScore", "HUD score: 12345 shows as 12 345", "the HUD shows the game's score"),
    ("CheckBallWaits", "Serve: the ball waits at its serve point", "the ball waits on the paddle"),
    ("CheckLaunchSpeed", "Launch: two frames later at the level's speed", "a launch sends the ball at the level's speed"),
    ("CheckBallKeepsSpeed", "Speed and angle: a slow, flat ball is put right", "the ball keeps its speed and never goes flat"),
    ("CheckBallBreaksBrick", "Break: the ball aimed at a front prism breaks it", "a ball breaks a one-hit brick"),
    ("CheckBreakScores", "Break score: ScoreFor(10, combo, level) more", "a break scores by the formula"),
    ("CheckStrongBrick", "Strong prism: clear the way, then one touch of gold", "a strong brick loses one hit per touch"),
    ("CheckShards", "Shards: a break throws them, and they fade away", "shards burst out and fade away"),
    ("CheckCombo", "Combo: two breaks make 2, the paddle makes 0", "the combo grows and resets at the paddle"),
    ("CheckAutopilot", "Autopilot: the paddle moves under a resting ball", "the autopilot keeps the paddle under the ball"),
    ("CheckWiden", "Widen: a caught capsule widens the paddle", "W widens the paddle"),
    ("CheckWidenSettles", "Widen ends: the paddle settles back to 3.2 m", "the widening timeline brings the paddle back"),
    ("CheckSlowDown", "Slow motion: 0.55 of the speed, then all of it", "S slows the ball and lets it go again"),
    ("CheckDrain", "Drain: a life less, then a new serve", "the drain takes a life and serves again"),
    ("CheckHudLives", "HUD lives: one icon for each life left", "the HUD shows the lives left"),
    ("CheckClearBroadcast", "Clear: the broadcast reaches eight graphs", "clearing a level reaches every graph that listens"),
    ("CheckNextLevel", "Next level: level 2's 34 prisms, then a serve", "the next level is built"),
    ("CheckBanner", "Banner: LEVEL 2, bright, then gone within 2.5 s", "the level banner plays its timeline"),
    ("CheckCameraShake", "Camera: the clear shakes it, then it rests at home", "the camera shakes on a clear and settles"),
    ("CheckLightsPulse", "Lights: a hit that leaves the prism standing pulses all six", "every hit pulses every light"),
    ("CheckGameOver", "Game over: the last life ends it, the table shows the score",
     "losing the last life ends the game and records the score"),
    ("CheckNewGame", "New game: fresh read-outs, and remote reads guarded", "a new game starts fresh, and remote reads are guarded"),
)
TEST_ENTITIES = (
    ("Game", "The entity whose Game.sgraph the checks play"),
    ("Paddle", "The paddle"),
    ("CameraRig", "The camera rig"),
    ("Bricks", "The container of the level's prisms"),
    ("Balls", "The container of the live balls"),
    ("Shards", "The container of the shards of broken prisms"),
    ("ScoreText", "The HUD's score text"),
    ("TableText", "The game-over screen's table of the top five"),
    ("Banner", "The level banner"),
    ("BannerText", "The level banner's text"),
) + tuple((f"Life{index + 1}", f"Life icon {index + 1} of the HUD") for index in range(LIVES_SHOWN))


def declare_tests(author):
    author.declare(TESTS, "The test scene's checks: 29 checks of Prism Break's rules, physics, screen and flow, one after "
                   "another, each a latent function that acts, waits for what it expects and reports it.", variables=(
        tuple(public(name, ENTITY, None, tooltip) for name, tooltip in TEST_ENTITIES) + (
            private("StepStart", "double", 0.0),
            private("Strong", ENTITY),
            private("Target", ENTITY),
            private("LeftBefore", "int", 0),
            private("ScoreBefore", "int", 0),
            private("ComboBefore", "int", 0),
            private("LevelBefore", "int", 0),
            private("LivesBefore", "int", 0),
            private("Reached", "int", 0),
            private("Base", "float", 0.0),
            private("Slowed", "float", 0.0),
            private("PeakAt", "double", 0.0),
            private("GoneAt", "double", 0.0),
            private("SawLevel", "bool", False),
            private("BannerDone", "bool", False),
            private("ShakeSeen", "bool", False),
        )), events=(("WatchTheBanner", ()), ("WatchTheCamera", ())), functions=(
        ("Place", False, (("At", "Strada.Vector3"), ("Velocity", "Strada.Vector3")), (), ()),
        ("Strike", False, (("At", "Strada.Vector3"),), (), ()),
        ("FindBrick", False, (("Row", "int"), ("Column", "int")), (("Brick", ENTITY),), (("Found", ENTITY, None),)),
    ) + tuple((name, False, (), (), ()) for name, _, _ in TEST_CHECKS))


def write_tests(author):
    with author.region(TESTS, "Start: every check in turn, then the report", CHECK) as b:
        start = b.event("Event.Create")
        settle = b.add("Flow.WaitFrames", Frames=1, comment="Every entity has run its Create first")
        steps = b.sequence(len(TEST_CHECKS) + 1)
        b.chain(start, settle)
        b.flow(settle["Completed"], steps)
        for index, (name, _, _) in enumerate(TEST_CHECKS):
            b.flow(steps[f"Then{index}"], b.own(name))
        b.flow(steps[f"Then{len(TEST_CHECKS)}"], b.call("Call:Strada.Testing.TestReporter.Finish()"))
    with author.region(TESTS, "Watch the banner: bright, LEVEL 2, then faded", CHECK) as b:
        watch = b.custom("WatchTheBanner")
        opacity = b.pure("Get:Strada.UIRectComponent.Opacity", "Value", Target=var(b, "Banner"))
        bright = b.add("Flow.WaitUntil", Condition=b.expr("opacity > 0.9", opacity=opacity))
        peak = b.set("PeakAt", b.pure("Get:Strada.Time.Elapsed", "Value"))
        text_now = b.pure("Get:Strada.UITextComponent.Text", "Value", Target=var(b, "BannerText"))
        level = b.add("Flow.WaitUntil", Condition=b.equal(text_now, "LEVEL 2"))
        saw = b.set("SawLevel", True)
        visible = b.pure("Get:Strada.UIRectComponent.Visible", "Value", Target=var(b, "Banner"))
        faded = b.add("Flow.WaitUntil", Condition=b.expr("opacity <= 0.001 || !visible", opacity=opacity, visible=visible))
        gone_at = b.set("GoneAt", b.pure("Get:Strada.Time.Elapsed", "Value"))
        done = b.set("BannerDone", True)
        b.chain(watch, bright)
        b.chain(bright["Completed"], peak, level)
        b.chain(level["Completed"], saw, faded)
        b.chain(faded["Completed"], gone_at, done)
    with author.region(TESTS, "Watch the camera: the clear moves it off its home", CHECK) as b:
        watch = b.custom("WatchTheCamera")
        moved = b.add("Flow.WaitUntil", Condition=b.expr("distance(p, home) > 0.05", p=position_of(b, "CameraRig"),
                                                         home=b.get("Home", CAMERA_RIG, var(b, "CameraRig"))))
        b.chain(watch, moved)
        b.flow(moved["Completed"], b.set("ShakeSeen", True))

    with author.region(TESTS, "Place: the ball at a point, with a velocity", CHECK, "Place") as b:
        any_ball = b.branch(b.expr("count > 0", count=count_of(b, "Balls")))
        ball = the_ball(b)
        teleport = b.call("Call:Strada.RigidBodyComponent.Teleport(Strada.Vector3,Strada.Quaternion)", Target=ball,
                          Position=b.entry["At"], Rotation=IDENTITY)
        velocity = b.call("Set:Strada.RigidBodyComponent.LinearVelocity", Target=ball, Value=b.entry["Velocity"])
        b.chain(b.entry, any_ball)
        b.chain(any_ball["True"], teleport, velocity, b.ret)
        b.flow(any_ball["False"], b.ret)
    with author.region(TESTS, "Strike: the ball flies at a prism from in front", CHECK, "Strike") as b:
        run(b, place(b, b.expr("vec3(at.x, 0.5, at.z + 1.6)", at=b.entry["At"]),
                     b.expr("vec3(0, 0, -speed)", speed=of_game(b, "BallSpeed"))))
    with author.region(TESTS, "Find a prism: the one at a row and column of the layout", CHECK, "FindBrick") as b:
        each = b.add("Flow.ForEach", Array=b.children(var(b, "Bricks")))
        expected = b.expr("origin + vec3(column * spacing.x, 0, row * spacing.y)", origin=of_game(b, "Origin"),
                          column=b.entry["Column"], spacing=of_game(b, "Spacing"), row=b.entry["Row"])
        here = b.branch(b.library("NearVector", CHECKS, output="Result", A=b.position(each["Element"]), B=expected, Tolerance=0.05).out)
        found = b.set("Found", each["Element"])
        b.chain(b.entry, each)
        b.flow(each["Body"], here)
        b.flow(here["True"], found)
        b.flow(found, each["Break"])
        b.flow(each["Completed"], b.ret)
        b.link(b.get("Found"), b.ret["Brick"])

    for function, title, name in TEST_CHECKS:
        with author.region(TESTS, title, CHECK, function) as b:
            TEST_WRITERS[function](b, name)


def check_level_is_built(b, name):
    left = of_game(b, "BricksLeft")
    serving = is_state(b, "Serve")
    run(b, check(b, name, serving, 5, b.fmt("{Left} prisms once the state is {State}", Left=left, State=of_game(b, "State")),
                 b.both(serving, b.equal(left, LEVEL_BRICKS[0]))))


def check_bricks_are_entities(b, name):
    children = count_of(b, "Bricks")
    left = of_game(b, "BricksLeft")
    run(b, check(b, name, b.equal(children, left), 1, b.fmt("{Children} children for {Left} prisms", Children=children, Left=left)))


def check_brick_glow(b, name):
    hits = hits_of(b, "Strong")
    glow = glow_of(b, "Strong")
    done = b.both(b.equal(hits, 3), b.equal(glow, b.library("BrickColor", ARCADE, output="Color", Hits=3).out))
    run(b, find_brick(b, 2, 5, "Strong"), check(b, name, done, 1, b.fmt("{Hits} hits, glowing {Glow}", Hits=hits, Glow=glow)))


def check_score_formula(b, name):
    score = b.library("ScoreFor", ARCADE, output="Score", Points=50, Combo=3, Level=2).out
    run(b, check(b, name, b.equal(score, 250), 1, b.fmt("ScoreFor(50, 3, 2) is {Score}", Score=score)))


def check_pitches(b, name):
    octave = b.library("PitchOf", ARCADE, output="Pitch", Semitones=12.0).out
    fifth = b.library("PitchOf", ARCADE, output="Pitch", Semitones=7.0).out
    done = b.both(near(b, octave, 2.0, 0.0001), near(b, fifth, 1.4983, 0.0001))
    run(b, check(b, name, done, 1, b.fmt("{Octave} and {Fifth}", Octave=octave, Fifth=fifth)))


def check_digit_groups(b, name):
    large = b.library("GroupDigits", ARCADE, output="Text", Value=1234567)
    negative = b.library("GroupDigits", ARCADE, output="Text", Value=-45)
    zero = b.library("GroupDigits", ARCADE, output="Text", Value=0)
    done = b.both(b.both(b.equal(large.out, "1 234 567"), b.equal(negative.out, "-45")), b.equal(zero.out, "0"))
    run(b, large, negative, zero, check(b, name, done, 1, b.fmt("'{Large}', '{Negative}' and '{Zero}'", Large=large.out,
                                                                 Negative=negative.out, Zero=zero.out)))


def check_top_five(b, name):
    full = b.library("InsertScore", ARCADE, output="Result", Scores=b.lit("int[]", [900, 500, 300, 200, 100]), Score=400, Keep=5)
    short = b.library("InsertScore", ARCADE, output="Result", Scores=b.lit("int[]", [50]), Score=10, Keep=1)
    done = b.both(b.library("SameInts", CHECKS, output="Result", A=full.out, B=b.lit("int[]", [900, 500, 400, 300, 200])).out,
                  b.library("SameInts", CHECKS, output="Result", A=short.out, B=b.lit("int[]", [50])).out)
    run(b, full, short, check(b, name, done, 1, b.fmt("{Full} and {Short}", Full=full.out, Short=short.out)))


def check_hud_score(b, name):
    shown = b.pure("Get:Strada.UITextComponent.Text", "Value", Target=var(b, "ScoreText"))
    run(b, b.set("Score", 12345, GAME, var(b, "Game")),
        check(b, name, b.equal(shown, "12 345"), 1, b.fmt("the score text reads '{Text}'", Text=shown)))


def check_ball_waits(b, name):
    point = b.expr("vec3(p.x, 0.5, p.z - 0.6)", p=position_of(b, "Paddle"))
    ball = b.position(the_ball(b))
    done = b.both(is_state(b, "Serve"), b.library("NearVector", CHECKS, output="Result", A=ball, B=point, Tolerance=0.05).out)
    run(b, check(b, name, done, 1, b.fmt("the ball at {Ball}, its serve point {Point}", Ball=ball, Point=point)))


def check_launch_speed(b, name):
    speed = b.expr("length(v)", v=b.pure("Get:Strada.RigidBodyComponent.LinearVelocity", "Value", Target=the_ball(b)))
    expected = of_game(b, "BallSpeed")
    frames = b.add("Flow.WaitFrames", Frames=2)
    run(b, b.send("Launch", var(b, "Game"), GAME), (frames, frames["Completed"]),
        check(b, name, near(b, speed, expected, 0.05), 1, b.fmt("{Speed} m/s of {Expected}", Speed=speed, Expected=expected)))


def check_ball_keeps_speed(b, name):
    velocity = b.pure("Get:Strada.RigidBodyComponent.LinearVelocity", "Value", Target=the_ball(b))
    speed = of_game(b, "BallSpeed")
    done = b.expr("abs(length(v) - speed) <= 0.05 && abs(v.z) / length(v) >= minZ - 0.001", v=velocity, speed=speed,
                  minZ=b.get("MinZ", BALL, the_ball(b)))
    flat = place(b, b.lit("Strada.Vector3", [0.0, 0.5, 1.0]), b.lit("Strada.Vector3", [5.0, 0.0, -0.2]))
    run(b, flat, wait_seconds(b, 2), check(b, name, done, 0.1, b.fmt("velocity {Velocity} at a speed of {Speed}", Velocity=velocity,
                                                                    Speed=speed)))


def check_ball_breaks_brick(b, name):
    left = of_game(b, "BricksLeft")
    before = var(b, "LeftBefore")
    done = b.both(gone(b), b.equal(left, b.expr("before - 1", before=before)))
    game = var(b, "Game")
    combo = b.set("Combo", 3, GAME, game, comment="A combo and a level above their first values, so that the next check tells "
                                                    "the score's formula from the prism's points")
    run(b, find_brick(b, 4, 5), b.set("LeftBefore", left), b.set("ScoreBefore", of_game(b, "Score")), combo,
        b.set("Level", 2, GAME, game), b.set("ComboBefore", of_game(b, "Combo")), b.set("LevelBefore", of_game(b, "Level")),
        strike(b, "Target"), check(b, name, done, 2, b.fmt("{Left} prisms left of {Before}", Left=left, Before=before)),
        b.set("Level", 1, GAME, game, comment="Back on the first level, which the checks after the next one play"))


def check_break_scores(b, name):
    combo = var(b, "ComboBefore")
    level = var(b, "LevelBefore")
    before = var(b, "ScoreBefore")
    points = b.library("ScoreFor", ARCADE, output="Score", Points=10, Combo=combo, Level=level).out
    score = of_game(b, "Score")
    scored = b.equal(score, b.expr("before + points", before=before, points=points))
    run(b, check(b, name, scored, 1, b.fmt("{Score} after {Before}, with a combo of {Combo} on level {Level}", Score=score,
                                           Before=before, Combo=combo, Level=level),
                 b.both(scored, b.expr("combo > 0 && level > 1", combo=combo, level=level))))


def check_strong_brick(b, name):
    hits = hits_of(b, "Strong")
    glow = glow_of(b, "Strong")
    done = b.both(b.equal(hits, 2), b.equal(glow, b.library("BrickColor", ARCADE, output="Color", Hits=2).out))
    run(b, find_brick(b, 3, 5), strike(b, "Target"), wait_until(b, b.equal(hits_of(b, "Target"), 1), 2),
        strike(b, "Target"), wait_until(b, gone(b), 2), strike(b, "Strong"),
        check(b, name, done, 2, b.fmt("{Hits} hits, glowing {Glow}", Hits=hits, Glow=glow)))


def check_shards(b, name):
    shards = count_of(b, "Shards")
    had = var(b, "Reached")
    none_left = b.equal(shards, 0)
    run(b, find_brick(b, 4, 4), strike(b, "Target"), wait_until(b, b.both(gone(b), b.expr("count > 0", count=shards)), 2),
        b.set("Reached", shards),
        check(b, name, none_left, 2.5, b.fmt("{Had} shards after the break, {Left} 2.5 s later", Had=had, Left=shards),
              b.both(none_left, b.expr("had > 0", had=had))))


def check_combo(b, name):
    combo = of_game(b, "Combo")
    reached = var(b, "Reached")
    reset = b.equal(combo, 0)
    run(b, onto_paddle(b), wait_until(b, reset, 1),
        find_brick(b, 4, 6), strike(b, "Target"), wait_until(b, gone(b), 2),
        find_brick(b, 4, 7), strike(b, "Target"), wait_until(b, b.both(gone(b), b.equal(combo, 2)), 2),
        b.set("Reached", combo), onto_paddle(b),
        check(b, name, reset, 1, b.fmt("{Reached} after two breaks, {Combo} after the paddle", Reached=reached, Combo=combo),
              b.both(reset, b.equal(reached, 2))))


def check_autopilot(b, name):
    off = b.expr("abs(p.x - q.x)", p=position_of(b, "Paddle"), q=b.position(the_ball(b)))
    rest = place(b, b.lit("Strada.Vector3", [3.5, 0.5, 2.0]), b.lit("Strada.Vector3", [0.0, 0.0, 0.0]))
    pilot = b.set("Autopilot", True, PADDLE, var(b, "Paddle"),
                  comment="At x 3.5 the autopilot's aiming offset, sin(0.9 x) * 0.9, is almost 0")
    run(b, rest, pilot, wait_seconds(b, 0.5),
        check(b, name, b.expr("off < 0.4", off=off), 0.1, b.fmt("{Off} m between the paddle and the ball", Off=off)),
        b.set("Autopilot", False, PADDLE, var(b, "Paddle")))


def check_widen(b, name):
    width = width_of_paddle(b)
    spawn = b.send("SpawnPowerUp", var(b, "Game"), GAME, Kind="Widen", At=b.expr("p + vec3(0, 0, -0.2)", p=position_of(b, "Paddle")))
    run(b, spawn, check(b, name, b.expr("width > 4.5", width=width), 0.6, b.fmt("the paddle is {Width} m wide", Width=width)))


def check_widen_settles(b, name):
    width = width_of_paddle(b)
    run(b, check(b, name, near(b, width, PADDLE_WIDTH, 0.01), 2.5, b.fmt("the paddle is {Width} m wide", Width=width)))


def check_slow_down(b, name):
    speed = of_game(b, "BallSpeed")
    base = var(b, "Base")
    slowed = var(b, "Slowed")
    done = b.both(near(b, slowed, b.expr("base * 0.55", base=base), 0.01), near(b, speed, base, 0.01))
    run(b, b.set("Base", speed), b.send("SlowDown", var(b, "Game"), GAME, Seconds=1.5), wait_seconds(b, 0.4),
        b.set("Slowed", speed), wait_seconds(b, 1.4),
        check(b, name, done, 0.1, b.fmt("{Slowed} m/s slowed, {Speed} after, of {Base}", Slowed=slowed, Speed=speed, Base=base)))


def check_drain(b, name):
    lives = of_game(b, "Lives")
    before = var(b, "LivesBefore")
    done = b.both(b.equal(lives, b.expr("before - 1", before=before)), is_state(b, "Serve"))
    run(b, b.set("LivesBefore", lives), into_drain(b),
        check(b, name, done, 1.5, b.fmt("{Lives} lives of {Before}, state {State}", Lives=lives, Before=before,
                                        State=of_game(b, "State"))))


def check_hud_lives(b, name):
    icons = {f"l{index + 1}": b.pure("Get:Strada.UIRectComponent.Visible", "Value", Target=var(b, f"Life{index + 1}"))
             for index in range(LIVES_SHOWN)}
    shown = b.expr(" + ".join(f"({icon} ? 1 : 0)" for icon in icons), **icons)
    lives = of_game(b, "Lives")
    run(b, check(b, name, b.equal(shown, lives), 1, b.fmt("{Shown} icons for {Lives} lives", Shown=shown, Lives=lives)))


def check_clear_broadcast(b, name):
    count = of_game(b, "LastBroadcastCount")
    run(b, b.set("SawLevel", False), b.set("BannerDone", False), b.set("ShakeSeen", False),
        b.send("WatchTheBanner", b.self_(), comment="The banner and the camera are watched from the clear on"),
        b.send("WatchTheCamera", b.self_()), b.send("ClearLevel", var(b, "Game"), GAME),
        check(b, name, b.equal(count, 8), 1, b.fmt("it reached {Count} graphs", Count=count)))


def check_next_level(b, name):
    left = of_game(b, "BricksLeft")
    level = of_game(b, "Level")
    done = b.both(b.both(b.equal(level, 2), b.equal(left, LEVEL_BRICKS[1])), is_state(b, "Serve"))
    run(b, check(b, name, done, 5, b.fmt("level {Level}, {Left} prisms, state {State}", Level=level, Left=left,
                                         State=of_game(b, "State"))))


def check_banner(b, name):
    fade = b.expr("gone - peak", gone=var(b, "GoneAt"), peak=var(b, "PeakAt"))
    saw = var(b, "SawLevel")
    done = var(b, "BannerDone")
    run(b, check(b, name, done, 3, b.fmt("LEVEL 2 shown: {Saw}; faded {Fade} s after it was bright", Saw=saw, Fade=fade),
                 b.expr("done && saw && fade <= 2.5", done=done, saw=saw, fade=fade)))


def check_camera_shake(b, name):
    offset = b.expr("distance(p, home)", p=position_of(b, "CameraRig"), home=b.get("Home", CAMERA_RIG, var(b, "CameraRig")))
    resting = b.expr("offset < 0.001", offset=offset)
    shaken = var(b, "ShakeSeen")
    run(b, check(b, name, resting, 2, b.fmt("shaken: {Shaken}; {Offset} m from home now", Shaken=shaken, Offset=offset),
                 b.both(resting, shaken)))


def check_lights_pulse(b, name):
    count = of_game(b, "LastPulseCount")
    hits = hits_of(b, "Target")
    standing = b.both(b.equal(hits, 1), b.equal(count, 6))
    reset = b.set("LastPulseCount", 0, GAME, var(b, "Game"),
                  comment="Level 2's front row hides its two-hit prisms: one breaks, then the one behind it is hit once")
    run(b, b.send("Launch", var(b, "Game"), GAME), wait_until(b, is_state(b, "Play"), 1), find_brick(b, 4, 4),
        strike(b, "Target"), wait_until(b, gone(b), 2), reset, find_brick(b, 2, 4), strike(b, "Target"),
        check(b, name, standing, 2, b.fmt("it pulsed {Count} lights, and the prism has {Hits} hits left", Count=count, Hits=hits)))


def check_game_over(b, name):
    score = b.library("GroupDigits", ARCADE, output="Text", Value=of_game(b, "Score"))
    rows = b.pure("String.Split", "Parts", Text=b.pure("Get:Strada.UITextComponent.Text", "Value", Target=var(b, "TableText")),
                  Separator="\n")
    first = b.element(rows, 0)
    shows = b.equal(first, b.fmt("1.   {Points}", Points=score.out))
    over = is_state(b, "Over")
    run(b, b.set("Lives", 1, GAME, var(b, "Game")), into_drain(b), wait_until(b, over, 3), score,
        check(b, name, shows, 1, b.fmt("state {State}, the first row '{Row}'", State=of_game(b, "State"), Row=first), b.both(shows, over)))


def check_new_game(b, name):
    score, lives, level, left = (of_game(b, variable) for variable in ("Score", "Lives", "Level", "BricksLeft"))
    fresh = b.expr(f"score == 0 && lives == 3 && level == 1 && left == {LEVEL_BRICKS[0]}", score=score, lives=lives, level=level,
                   left=left)
    done = b.both(fresh, is_state(b, "Serve"))
    guarded = b.both(b.runs(GAME, var(b, "Game")), b.negate(b.runs(GAME, var(b, "Paddle"))))
    run(b, b.send("StartGame", var(b, "Game"), GAME),
        check(b, name, done, 3, b.fmt("score {Score}, {Lives} lives, level {Level}, {Left} prisms", Score=score, Lives=lives,
                                      Level=level, Left=left), b.both(done, guarded)))


TEST_WRITERS = {
    "CheckLevelIsBuilt": check_level_is_built, "CheckBricksAreEntities": check_bricks_are_entities,
    "CheckBrickGlow": check_brick_glow, "CheckScoreFormula": check_score_formula, "CheckPitches": check_pitches,
    "CheckDigitGroups": check_digit_groups, "CheckTopFive": check_top_five, "CheckHudScore": check_hud_score,
    "CheckBallWaits": check_ball_waits, "CheckLaunchSpeed": check_launch_speed, "CheckBallKeepsSpeed": check_ball_keeps_speed,
    "CheckBallBreaksBrick": check_ball_breaks_brick, "CheckBreakScores": check_break_scores, "CheckStrongBrick": check_strong_brick,
    "CheckShards": check_shards, "CheckCombo": check_combo, "CheckAutopilot": check_autopilot, "CheckWiden": check_widen,
    "CheckWidenSettles": check_widen_settles, "CheckSlowDown": check_slow_down, "CheckDrain": check_drain,
    "CheckHudLives": check_hud_lives, "CheckClearBroadcast": check_clear_broadcast, "CheckNextLevel": check_next_level,
    "CheckBanner": check_banner, "CheckCameraShake": check_camera_shake, "CheckLightsPulse": check_lights_pulse,
    "CheckGameOver": check_game_over, "CheckNewGame": check_new_game,
}


WRITERS = {
    ARCADE: write_arcade, CHECKS: write_checks, BRICK: write_brick, BALL: write_ball, SHARD: write_shard, POWER_UP: write_power_up,
    PADDLE: write_paddle, DRAIN: write_drain, LIGHTS: write_lights, CAMERA_RIG: write_camera_rig, HUD: write_hud,
    GAME: write_game, TESTS: write_tests,
}


# --- The scene ------------------------------------------------------------------------------------------------------------

def transform(translation, euler=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    return {"Translation": list(translation), "RotationEuler": list(euler), "Scale": list(scale)}


def material(name):
    return f"asset://Materials/{name}.smat"


def clip(name):
    return f"asset://Audio/{name}.wav"


def entity_value(entity):
    return {"Type": "Entity", "Value": entity}


def float_value(value):
    return {"Type": "Float", "Value": value}


def make_materials(client):
    def make(name, fields):
        client.call("material_create", {"path": f"Materials/{name}.smat", "fields": fields})

    make("Floor", {"BaseColor": [0.012, 0.012, 0.03, 1.0], "Metallic": 0.3, "Roughness": 0.32,
                   "EmissiveTexture": "asset://Textures/Grid.png", "EmissiveColor": [0.32, 0.18, 1.0], "EmissiveIntensity": 0.9,
                   "UVTiling": [19.0, 26.0]})
    make("Wall", {"BaseColor": [0.03, 0.03, 0.06, 1.0], "Metallic": 0.7, "Roughness": 0.28})
    make("Neon Cyan", {"BaseColor": [0.0, 0.0, 0.0, 1.0], "EmissiveColor": CYAN[:3], "EmissiveIntensity": 9.0})
    make("Neon Magenta", {"BaseColor": [0.0, 0.0, 0.0, 1.0], "EmissiveColor": MAGENTA[:3], "EmissiveIntensity": 9.0})
    # Each prism clones this material and sets its glow (Brick.sgraph).
    make("Prism", {"BaseColor": [0.06, 0.06, 0.09, 1.0], "Metallic": 0.1, "Roughness": 0.06, "EmissiveColor": CYAN[:3],
                   "EmissiveIntensity": 1.4})
    make("Ball", {"BaseColor": [1.0, 1.0, 1.0, 1.0], "Roughness": 0.2, "EmissiveColor": [0.75, 0.95, 1.0], "EmissiveIntensity": 7.0})
    make("Paddle", {"BaseColor": [0.04, 0.04, 0.08, 1.0], "Metallic": 0.8, "Roughness": 0.22, "EmissiveColor": [0.15, 0.7, 1.0],
                    "EmissiveIntensity": 2.5})
    make("Shard", {"BaseColor": [0.8, 0.85, 1.0, 1.0], "Roughness": 0.1, "EmissiveColor": [0.8, 0.9, 1.0], "EmissiveIntensity": 5.0})
    make("Capsule W", {"BaseColor": [0.05, 0.2, 0.08, 1.0], "Roughness": 0.15, "EmissiveColor": [0.2, 1.0, 0.4], "EmissiveIntensity": 6.0})
    make("Capsule S", {"BaseColor": [0.2, 0.1, 0.02, 1.0], "Roughness": 0.15, "EmissiveColor": [1.0, 0.5, 0.08], "EmissiveIntensity": 6.0})


def make_prefab(client, name, components, children=(), variables=None, graph=None):
    """An entity (with child entities: (name, components)) saved as Prefabs/<name>.sprefab, then removed from the scene."""
    if graph is not None:
        components = dict(components)
        components["VisualScript"] = {"Graph": graph, "Variables": variables or {}}
    entity = client.call("entity_create", {"name": name, "components": components})["id"]
    for child_name, child_components in children:
        client.call("entity_create", {"name": child_name, "parent": entity, "components": child_components})
    reference = client.call("prefab_create", {"entity": entity, "path": f"Prefabs/{name}.sprefab"})["reference"]
    client.call("entity_delete", {"entities": [entity]})
    return reference


def make_prefabs(client, handles):
    hidden = transform((0.0, -100.0, 0.0))
    burst = make_prefab(client, "Burst", {
        "Transform": hidden,
        "ParticleSystem": {"Emitting": False, "Burst": 0, "MaxParticles": 160, "Shape": "Sphere", "Radius": 0.3, "LifetimeMin": 0.35,
                           "LifetimeMax": 0.85, "SpeedMin": 3.0, "SpeedMax": 8.5, "Drag": 2.6, "GravityFactor": 0.4, "StartSize": 0.34,
                           "EndSize": 0.0, "StartColor": [1.0, 1.0, 1.0, 1.0], "EndColor": [0.55, 0.3, 1.0, 0.0], "Intensity": 6.0,
                           "Texture": "asset://Textures/Spark.png", "BlendMode": "Additive", "WorldSpace": True,
                           "DestroyWhenFinished": True},
        "AudioSource": {"Clip": clip("Chime"), "Volume": 0.7, "Spatial": False},
    }, children=(("Shatter", {"AudioSource": {"Clip": clip("Break"), "Volume": 0.55, "Spatial": False}}),))
    shard = make_prefab(client, "Shard", {
        "Transform": transform((0.0, -100.0, 0.0), scale=(0.16, 0.16, 0.16)),
        "Mesh": {"Mesh": "builtin://Cube", "Materials": [material("Shard")], "CastShadows": False},
        "RigidBody": {"Type": "Dynamic", "Mass": 0.05, "Layer": 2, "LinearDamping": 0.4, "AngularDamping": 0.3},
        "BoxCollider": {"HalfExtents": [0.5, 0.5, 0.5], "Restitution": 0.4, "Friction": 0.5},
    }, graph=handles[SHARD])
    brick = make_prefab(client, "Brick", {
        "Transform": hidden,
        "Mesh": {"Mesh": "asset://Meshes/Prism.gltf", "Materials": [material("Prism")]},
        "RigidBody": {"Type": "Static"},
        "BoxCollider": {"HalfExtents": [BRICK_SIZE[0] / 2.0, BRICK_SIZE[1] / 2.0, BRICK_SIZE[2] / 2.0], "Restitution": 1.0, "Friction": 0.0},
        "AudioSource": {"Clip": clip("Chime"), "Volume": 0.7, "Spatial": False},
    }, graph=handles[BRICK], variables={"Burst": {"Type": "Prefab", "Value": burst}, "Shard": {"Type": "Prefab", "Value": shard}})
    ball = make_prefab(client, "Ball", {
        "Transform": transform((0.0, -100.0, 0.0), scale=(2 * BALL_RADIUS, 2 * BALL_RADIUS, 2 * BALL_RADIUS)),
        "Mesh": {"Mesh": "builtin://Sphere", "Materials": [material("Ball")]},
        "RigidBody": {"Type": "Dynamic", "Layer": 1, "ContinuousCollision": True, "GravityFactor": 0.0, "LinearDamping": 0.0,
                      "AngularDamping": 0.0, "AllowSleep": False, "LockTranslation": [False, True, False], "LockRotation": [True, True, True]},
        "SphereCollider": {"Radius": 0.5, "Restitution": 1.0, "Friction": 0.0},
        "PointLight": {"Color": [0.6, 0.9, 1.0], "Intensity": 5.0, "Range": 4.5},
        "ParticleSystem": {"Emitting": True, "Rate": 90.0, "MaxParticles": 120, "Shape": "Point", "LifetimeMin": 0.22, "LifetimeMax": 0.32,
                           "SpeedMin": 0.0, "SpeedMax": 0.2, "StartSize": 0.42, "EndSize": 0.0, "StartColor": [0.55, 0.9, 1.0, 0.8],
                           "EndColor": [0.7, 0.3, 1.0, 0.0], "Intensity": 4.0, "Texture": "asset://Textures/Spark.png",
                           "BlendMode": "Additive", "WorldSpace": True},
        "AudioSource": {"Clip": clip("Wall"), "Volume": 0.5, "Spatial": False},
    }, graph=handles[BALL])
    power_up = make_prefab(client, "PowerUp", {
        "Transform": transform((0.0, -100.0, 0.0), euler=(0.0, 0.0, 90.0), scale=(0.42, 0.42, 0.42)),
        "Mesh": {"Mesh": "builtin://Capsule", "Materials": [material("Capsule W")]},
        "PointLight": {"Color": [0.2, 1.0, 0.4], "Intensity": 4.0, "Range": 3.0},
        "AudioSource": {"Clip": clip("PowerUp"), "Volume": 0.8, "Spatial": False},
    }, graph=handles[POWER_UP])
    return {"Brick": brick, "Ball": ball, "PowerUp": power_up}


# The screen: a 1920 x 1080 design, y down. Colours are linear with straight alpha.
GLASS = [0.004, 0.005, 0.02, 0.74]
CAPTION = [0.42, 0.5, 0.78, 1.0]
VALUE = [0.96, 0.98, 1.0, 1.0]
GOLD_TEXT = [1.0, 0.74, 0.2, 1.0]
TITLE_LETTERS = "PRISM BREAK"


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


def label(value, size, color, alignment="Center", vertical="Middle", spacing=1.0):
    return {"Text": value, "FontSize": size, "Color": list(color), "Alignment": alignment, "VerticalAlignment": vertical,
            "Wrap": False, "LineSpacing": spacing}


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
        "Transform": transform((0.0, 14.0, -16.0)),
        "UICanvas": {"ScaleMode": "ScaleWithScreenSize", "ReferenceResolution": [1920.0, 1080.0]}}})["id"]

    # The game bar: the score and the best on the left, the level in the middle, the lives on the right.
    bar = element("Game Bar", screen, ui_rect((0.0, 0.0), (1.0, 0.0), (0.5, 0.0), (0.0, 0.0), (0.0, 112.0), visible=False),
                  image(GLASS))
    elements["GameBar"] = bar
    element("Game Bar Line", bar, ui_rect((0.0, 1.0), (1.0, 1.0), (0.5, 1.0), (0.0, 0.0), (0.0, 3.0)), image([0.3, 0.2, 1.0, 0.9]))
    element("Score Caption", bar, at((0.0, 0.0), (0.0, 0.0), (64.0, 14.0), (260.0, 30.0)), text_fields=label("SCORE", 22.0, CAPTION, "Left"))
    elements["ScoreText"] = element("Score Text", bar, at((0.0, 0.0), (0.0, 0.0), (64.0, 40.0), (300.0, 60.0)),
                                    text_fields=label("0", 52.0, VALUE, "Left"))
    element("Best Caption", bar, at((0.0, 0.0), (0.0, 0.0), (400.0, 14.0), (260.0, 30.0)), text_fields=label("BEST", 22.0, CAPTION, "Left"))
    elements["BestText"] = element("Best Text", bar, at((0.0, 0.0), (0.0, 0.0), (400.0, 40.0), (300.0, 60.0)),
                                   text_fields=label("0", 52.0, GOLD_TEXT, "Left"))
    elements["LevelText"] = element("Level Text", bar, at((0.5, 0.0), (0.5, 0.0), (0.0, 26.0), (500.0, 64.0)),
                                    text_fields=label("LEVEL 1", 48.0, VALUE))
    element("Lives Caption", bar, at((1.0, 0.0), (1.0, 0.0), (-64.0, 14.0), (300.0, 30.0)), text_fields=label("LIVES", 22.0, CAPTION, "Right"))
    for index in range(LIVES_SHOWN):
        elements[f"Life{index + 1}"] = element(f"Life {index + 1}", bar, at((1.0, 0.0), (1.0, 0.0), (-64.0 - 58.0 * index, 50.0), (46.0, 46.0)),
                                               image([0.55, 0.9, 1.0, 1.0], "Life"))

    # The combo badge under the bar.
    badge = element("Combo Badge", screen, at((0.5, 0.0), (0.5, 0.5), (0.0, 196.0), (200.0, 92.0), visible=False),
                    image([0.85, 0.08, 0.6, 0.92], radius=46.0))
    elements["ComboBadge"] = badge
    elements["ComboText"] = element("Combo Text", badge, fill(), text_fields=label("×2", 60.0, VALUE))

    # The serve prompt over the paddle.
    elements["ServePrompt"] = element("Serve Prompt", screen, at((0.5, 1.0), (0.5, 1.0), (0.0, -200.0), (1000.0, 60.0), visible=False),
                                      text_fields=label("SPACE OR CLICK TO LAUNCH", 38.0, VALUE))

    # The title: PRISM BREAK in glowing letters, each in a prism's colour, over the arena that plays itself.
    title = element("Title Screen", screen, fill())
    elements["TitleScreen"] = title
    element("Title Shade", title, fill(), image([0.0, 0.0, 0.015, 0.35]))
    element("Title Glow", title, at((0.5, 0.0), (0.5, 0.5), (0.0, 330.0), (1700.0, 560.0)), image([0.9, 0.15, 0.85, 0.5], "Glow"))
    letter_size = 196.0
    advance = 124.0
    width = advance * len(TITLE_LETTERS)
    for index, letter in enumerate(TITLE_LETTERS):
        if letter == " ":
            continue
        color = BRICK_COLORS[index % 3]
        x = -width / 2.0 + advance * (index + 0.5)
        cell = element(f"Title {letter} {index + 1}", title, at((0.5, 0.0), (0.5, 0.5), (x, 330.0), (advance + 40.0, 240.0)))
        element(f"Title {letter} {index + 1} Glow", cell, fill(), text_fields=label(letter, letter_size * 1.08, color[:3] + [0.35]))
        element(f"Title {letter} {index + 1} Face", cell, fill(), text_fields=label(letter, letter_size, [0.6 + 0.4 * c for c in color[:3]] + [1.0]))
    element("Title Subtitle", title, at((0.5, 0.0), (0.5, 0.5), (0.0, 520.0), (1200.0, 50.0)),
            text_fields=label("A GAME MADE OF VISUAL SCRIPTS ALONE", 32.0, [0.7, 0.78, 1.0, 1.0]))
    elements["TitlePrompt"] = element("Title Prompt", title, at((0.5, 0.0), (0.5, 0.5), (0.0, 690.0), (1200.0, 64.0)),
                                      text_fields=label("PRESS SPACE OR CLICK TO PLAY", 48.0, GOLD_TEXT))
    element("Title Controls", title, at((0.5, 0.0), (0.5, 0.5), (0.0, 770.0), (1600.0, 40.0)),
            text_fields=label("MOUSE, A AND D OR THE ARROWS MOVE THE PADDLE      ESC PAUSES", 26.0, CAPTION))

    # The level banner.
    banner = element("Banner", screen, at((0.5, 0.5), (0.5, 0.5), (0.0, -40.0), (1500.0, 260.0), visible=False, opacity=0.0),
                     image([0.1, 0.7, 1.0, 0.45], "Glow"))
    elements["Banner"] = banner
    elements["BannerText"] = element("Banner Text", banner, fill(), text_fields=label("LEVEL CLEAR", 128.0, VALUE))

    # The pause screen.
    pause = element("Pause Screen", screen, fill(visible=False), image([0.0, 0.0, 0.02, 0.62]))
    elements["PauseScreen"] = pause
    element("Pause Title", pause, at((0.5, 0.5), (0.5, 0.5), (0.0, -40.0), (1000.0, 150.0)), text_fields=label("PAUSED", 132.0, VALUE))
    element("Pause Hint", pause, at((0.5, 0.5), (0.5, 0.5), (0.0, 70.0), (1000.0, 50.0)), text_fields=label("ESC TO RESUME", 34.0, CAPTION))

    # The game-over screen: the score and the top five.
    over = element("Over Screen", screen, fill(visible=False), image([0.0, 0.0, 0.02, 0.55]))
    elements["OverScreen"] = over
    panel = element("Over Panel", over, at((0.5, 0.5), (0.5, 0.5), (0.0, 0.0), (760.0, 780.0)), image(GLASS, radius=28.0))
    element("Over Title", panel, at((0.5, 0.0), (0.5, 0.0), (0.0, 40.0), (700.0, 110.0)),
            text_fields=label("GAME OVER", 96.0, [1.0, 0.35, 0.85, 1.0]))
    element("Final Caption", panel, at((0.5, 0.0), (0.5, 0.0), (0.0, 160.0), (700.0, 30.0)), text_fields=label("SCORE", 24.0, CAPTION))
    elements["FinalScore"] = element("Final Score", panel, at((0.5, 0.0), (0.5, 0.0), (0.0, 190.0), (700.0, 90.0)),
                                     text_fields=label("0", 80.0, VALUE))
    element("Table Caption", panel, at((0.5, 0.0), (0.5, 0.0), (0.0, 300.0), (700.0, 30.0)), text_fields=label("TOP FIVE", 24.0, CAPTION))
    elements["TableText"] = element("Table Text", panel, at((0.5, 0.0), (0.5, 0.0), (0.0, 340.0), (520.0, 290.0)),
                                    text_fields=label("", 42.0, GOLD_TEXT, "Center", "Top", 1.25))
    element("Over Prompt", panel, at((0.5, 1.0), (0.5, 1.0), (0.0, -40.0), (700.0, 50.0)),
            text_fields=label("SPACE OR CLICK TO PLAY AGAIN", 32.0, VALUE))
    return screen, elements


def build_scene(client, handles, prefabs):
    """The main scene: the camera rig, the light and the look, the arena, the paddle, the Game, the music and the screen.
    Returns the entities the test scene changes, by name."""
    found = {entity["name"]: entity["id"] for entity in client.call("entity_find", {})["entities"]}
    client.call("entity_delete", {"entities": [found[name] for name in ("Floor", "Cube", "Sphere", "Capsule") if name in found]})
    client.call("scene_settings", {"name": "Main", "settings": {"Renderer": {
        "Tonemapper": "ACES", "EV100": 0.0, "Shadows": True, "SoftShadows": True, "ShadowDistance": 60.0, "AmbientOcclusion": True,
        "Bloom": True, "BloomIntensity": 0.12, "FXAA": True}}})

    def create(name, components, parent=None):
        arguments = {"name": name, "components": components}
        if parent is not None:
            arguments["parent"] = parent
        return client.call("entity_create", arguments)["id"]

    # The camera rig at its home, the camera on it looking at the middle of the arena.
    rig = create("Camera Rig", {"Transform": transform(CAMERA_HOME),
                                "VisualScript": {"Graph": handles[CAMERA_RIG], "Variables": {
                                    "Home": {"Type": "Vector3", "Value": CAMERA_HOME}}}})
    camera = found["Main Camera"]
    client.call("entity_reparent", {"entity": camera, "parent": rig})
    pitch = -math.degrees(math.atan2(CAMERA_HOME[1] - CAMERA_LOOK[1], CAMERA_HOME[2] - CAMERA_LOOK[2]))
    client.call("component_set", {"entity": camera, "component": "Transform", "fields": transform((0.0, 0.0, 0.0), euler=(pitch, 0.0, 0.0))})
    client.call("component_set", {"entity": camera, "component": "Camera", "fields": {"PerspectiveFOV": 42.0, "PerspectiveFar": 200.0}})
    client.call("component_add", {"entity": camera, "component": "AudioListener"})

    # A dim violet sun, a faint sky and the look of neon: bloom, contrast, saturation, a vignette and a touch of fringing.
    client.call("component_set", {"entity": found["Sun"], "component": "Transform", "fields": {"RotationEuler": [-62.0, 25.0, 0.0]}})
    client.call("component_set", {"entity": found["Sun"], "component": "DirectionalLight",
                                  "fields": {"Color": [0.62, 0.45, 1.0], "Intensity": 0.4, "CastShadows": True}})
    client.call("component_set", {"entity": found["Sky"], "component": "SkyLight", "fields": {
        "Environment": "builtin://DefaultSky", "Intensity": 0.08, "DrawSkybox": False}})
    create("Look", {"PostProcess": {"Global": True, "OverrideBloom": True, "Bloom": True, "OverrideBloomIntensity": True, "BloomIntensity": 0.25,
                                    "OverrideContrast": True, "Contrast": 1.1, "OverrideSaturation": True, "Saturation": 1.15,
                                    "OverrideVignette": True, "Vignette": 0.4, "OverrideChromaticAberration": True,
                                    "ChromaticAberration": 0.15}})

    # The arena: the floor, three walls with neon strips along their tops, the drain behind the paddle and six lights.
    arena = create("Arena", {"Transform": transform((0.0, 0.0, 0.0))})
    solid = {"Restitution": 1.0, "Friction": 0.0}

    def block(name, center, size, look, parent=arena, collider=True):
        components = {"Transform": transform(center, scale=size), "Mesh": {"Mesh": "builtin://Cube", "Materials": [material(look)]}}
        if collider:
            components["RigidBody"] = {"Type": "Static"}
            components["BoxCollider"] = dict(solid, HalfExtents=[0.5, 0.5, 0.5])
        return create(name, components, parent)

    depth = 25.0
    middle_z = BACK_Z + depth / 2.0
    block("Floor", (0.0, -0.1, middle_z), (2.0 * WALL_X + 1.0, 0.2, depth), "Floor")
    block("Left Wall", (-WALL_X - 0.25, 0.6, middle_z), (0.5, 1.2, depth), "Wall")
    block("Right Wall", (WALL_X + 0.25, 0.6, middle_z), (0.5, 1.2, depth), "Wall")
    block("Back Wall", (0.0, 0.6, BACK_Z - 0.25), (2.0 * WALL_X + 1.0, 1.2, 0.5), "Wall")
    block("Left Neon", (-WALL_X - 0.02, 1.22, middle_z), (0.08, 0.06, depth), "Neon Cyan", collider=False)
    block("Right Neon", (WALL_X + 0.02, 1.22, middle_z), (0.08, 0.06, depth), "Neon Cyan", collider=False)
    block("Back Neon", (0.0, 1.22, BACK_Z - 0.02), (2.0 * WALL_X, 0.06, 0.08), "Neon Magenta", collider=False)
    create("Drain", {
        "Transform": transform((0.0, 0.5, 11.25)),
        "RigidBody": {"Type": "Static"},
        "BoxCollider": {"HalfExtents": [9.0, 1.0, 0.75], "IsTrigger": True},
        "AudioSource": {"Clip": clip("Lose"), "Volume": 0.9, "Spatial": False},
    }, arena)
    lights = create("Lights", {"Transform": transform((0.0, 0.0, 0.0))}, arena)
    spots = [(-9.5, -10.0), (-9.5, -1.0), (-9.5, 8.0), (9.5, 8.0), (9.5, -1.0), (9.5, -10.0)]
    light_entities = []
    for index, (x, z) in enumerate(spots):
        color = CYAN if index % 2 == 0 else MAGENTA
        light_entities.append(create(f"Light {index + 1}", {
            "Transform": transform((x, 1.2, z)),
            "PointLight": {"Color": color[:3], "Intensity": 6.0, "Range": 8.0},
            "VisualScript": {"Graph": handles[LIGHTS], "Variables": {
                "Base": {"Type": "Color", "Value": color}, "BaseIntensity": float_value(6.0), "Index": {"Type": "Int32", "Value": index}}},
        }, lights))

    containers = {name: create(name, {"Transform": transform((0.0, 0.0, 0.0))}) for name in ("Bricks", "Balls", "Shards", "PowerUps")}

    paddle = create("Paddle", {
        "Transform": transform((0.0, 0.5, PADDLE_Z), scale=(PADDLE_WIDTH, 0.4, 0.5)),
        "Mesh": {"Mesh": "builtin://Cube", "Materials": [material("Paddle")]},
        "RigidBody": {"Type": "Kinematic"},
        "BoxCollider": dict(solid, HalfExtents=[0.5, 0.5, 0.5]),
        "AudioSource": {"Clip": clip("Paddle"), "Volume": 0.8, "Spatial": False},
    })

    game = create("Game", {"Transform": transform((0.0, 0.0, 0.0))})
    sounds = create("Sounds", {"Transform": transform((0.0, 0.0, 0.0))}, game)
    sound = {name: create(name, {"AudioSource": {"Clip": clip(name), "Volume": 0.8, "Spatial": False}}, sounds)
             for name in ("LevelClear", "GameOver", "PowerUp", "Launch")}
    create("Music", {"AudioSource": {"Clip": clip("Music"), "Stream": True, "Loop": True, "Volume": 0.35, "PlayOnStart": True,
                                     "Spatial": False}})
    screen, elements = build_screen(client)

    game_variables = {
        "BrickPrefab": {"Type": "Prefab", "Value": prefabs["Brick"]},
        "BallPrefab": {"Type": "Prefab", "Value": prefabs["Ball"]},
        "PowerUpPrefab": {"Type": "Prefab", "Value": prefabs["PowerUp"]},
        "WidenLook": {"Type": "Asset", "Value": material("Capsule W")},
        "SlowLook": {"Type": "Asset", "Value": material("Capsule S")},
        "Paddle": entity_value(paddle),
        "CameraRig": entity_value(rig),
        "LevelClearSound": entity_value(sound["LevelClear"]),
        "GameOverSound": entity_value(sound["GameOver"]),
        "PowerUpSound": entity_value(sound["PowerUp"]),
        "LaunchSound": entity_value(sound["Launch"]),
    }
    for name, entity in containers.items():
        game_variables[name] = entity_value(entity)
    client.call("component_add", {"entity": game, "component": "VisualScript", "fields": {"Graph": handles[GAME], "Variables": game_variables}})
    client.call("component_add", {"entity": paddle, "component": "VisualScript", "fields": {"Graph": handles[PADDLE], "Variables": {
        "Game": entity_value(game), "Balls": entity_value(containers["Balls"])}}})
    drain = {entity["name"]: entity["id"] for entity in client.call("entity_find", {"name": "Drain"})["entities"]}["Drain"]
    client.call("component_add", {"entity": drain, "component": "VisualScript", "fields": {"Graph": handles[DRAIN], "Variables": {
        "Game": entity_value(game)}}})
    hud_variables = {"Game": entity_value(game)}
    for name, entity in elements.items():
        hud_variables[name] = entity_value(entity)
    client.call("component_add", {"entity": screen, "component": "VisualScript", "fields": {"Graph": handles[HUD], "Variables": hud_variables}})
    return {"Game": game, "GameVariables": game_variables, "Paddle": paddle, "CameraRig": rig, "Screen": screen, "Elements": elements,
            "Containers": containers, "Lights": light_entities}


def build_test_scene(client, handles, scene):
    """The test scene: the main scene saved as Tests (its entities keep their IDs), a repeatable game that serves at once,
    drops no capsule by chance and widens the paddle for 2 s, and the entity whose graph runs the checks."""
    client.call("scene_save", {"path": "Scenes/Tests.sscene"})
    client.call("scene_settings", {"name": "Tests"})
    variables = dict(scene["GameVariables"])
    variables.update({"Seed": {"Type": "Int32", "Value": 7}, "StartOnTitle": {"Type": "Bool", "Value": False},
                      "PowerUpChance": float_value(0.0), "WidenSeconds": float_value(2.0)})
    client.call("component_set", {"entity": scene["Game"], "component": "VisualScript", "fields": {"Graph": handles[GAME],
                                                                                                    "Variables": variables}})
    client.call("entity_create", {"name": "Tests", "components": {"VisualScript": {"Graph": handles[TESTS], "Variables": {
        name: entity_value(entity) for name, entity in test_entities(scene).items()}}}})
    client.call("scene_save")
    client.call("scene_open", {"path": "Scenes/Main.sscene"})


def test_entities(scene):
    """The entities the checks look at, by the names of Tests.sgraph's variables."""
    known = {"Game": scene["Game"], "Paddle": scene["Paddle"], "CameraRig": scene["CameraRig"]}
    known.update(scene["Containers"])
    known.update(scene["Elements"])
    return {name: known[name] for name, _ in TEST_ENTITIES}


# --- Checks ---------------------------------------------------------------------------------------------------------------

def verify(client, handles):
    """Every graph analyzes without a diagnostic and every node resolves exactly; every node stands in exactly one comment
    box, and every box stands arranged; the Arcade library is used by the graphs that call it."""
    for graph, handle in handles.items():
        document = client.call("graph_get", {"graph": handle})["document"]
        checked = client.call("graph_check", {"graph": handle, "document": document})
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
                arranged = client.call("graph_arrange", {"graph": handle, "function": function, "nodes": inside, "dryRun": True})
                if arranged["changed"]:
                    raise SystemExit(f"the box '{box['Text']}' of {graph} does not stand arranged")
    used_by = {entry["path"] for entry in client.call("graph_interface", {"graph": handles[ARCADE]})["usedBy"]}
    expected = {"Scripts/Game.sgraph", "Scripts/Brick.sgraph", "Scripts/Hud.sgraph", "Scripts/Tests.sgraph"}
    if not expected <= used_by:
        raise SystemExit(f"Arcade is used by {sorted(used_by)}, expected at least {sorted(expected)}")


def contains(box, node):
    """Whether a node's position lies inside a comment box."""
    x, y = node["Position"]
    (left, top), (width, height) = box["Position"], box["Size"]
    return left <= x <= left + width and top <= y <= top + height


def read_log(client, since, level):
    """The log's entries at or above a level from an index on, all of them; returns (entries, the index after the last)."""
    entries = []
    while True:
        read = client.call("log_read", {"since": since, "minLevel": level, "maxCount": 10000})
        since = read["next"]
        if not read["entries"]:
            return entries, since
        entries += read["entries"]


def play(client):
    """Plays the title's attract mode for 600 frames: no visual script may warn while it plays."""
    # Only what the attract mode logs counts: the authoring and the test run logged before it.
    _, start = read_log(client, 0, "critical")
    client.call("play_start", {"paused": True})
    client.call("play_advance", {"frames": 600})
    entries, _ = read_log(client, start, "warn")
    client.call("play_stop")
    problems = [entry["message"] for entry in entries if "isual script" in entry["message"] or ".sgraph" in entry["message"]]
    if problems:
        raise SystemExit("the attract mode logged problems of visual scripts:\n  " + "\n  ".join(problems))


def export(client):
    directory = tempfile.mkdtemp(prefix="prismbreak-export-")
    try:
        exported = client.call("project_export", {"directory": directory, "smokeTest": True})
        if not exported["succeeded"] or not exported["smokeTest"]["passed"]:
            raise SystemExit(f"the exported game failed: {exported.get('error')} {exported.get('smokeTest')}")
    finally:
        shutil.rmtree(directory, ignore_errors=True)


# --- The project ------------------------------------------------------------------------------------------------------------

def build(client):
    client.call("project_settings", {"settings": {
        "Name": "Prism Break", "Window": {"Title": "Prism Break", "Width": 1600, "Height": 900},
        "Physics": {"FixedTimestep": 1.0 / 60.0, "Layers": ["Default", "Ball", "Debris"], "IgnoredCollisions": [{"First": 1, "Second": 2}]}}})
    subprocess.run([sys.executable, os.path.join(TOOLS, "GenerateAssets.py")], check=True)
    client.call("asset_refresh")
    make_materials(client)

    author = Author(client)
    graphs = [graph for graph in ORDER if graph in WRITERS or graph in LIBRARIES]
    for graph in graphs:
        path = graph[len("asset://"):]
        template = "Library" if graph in LIBRARIES else "Empty"
        author.handles[graph] = client.call("graph_create", {"path": path, "template": template})["asset"]
    declare_all(author)
    for graph in graphs:
        if graph in WRITERS:
            WRITERS[graph](author)
            author.finish(graph)
    print(f"wrote {len(graphs)} graphs in {author.batches} batches; the levels have {', '.join(map(str, LEVEL_BRICKS))} prisms")

    prefabs = make_prefabs(client, author.handles)
    scene = build_scene(client, author.handles, prefabs)
    client.call("scene_save")
    build_test_scene(client, author.handles, scene)

    verify(client, author.handles)
    run = client.call("test_run", {"scene": "Scenes/Tests.sscene", "timeout": 90})
    print("test run:", {key: run[key] for key in ("finished", "timedOut", "frames", "passed", "failed", "exceptions")})
    for result in run["results"]:
        print("  ", "PASS" if result["passed"] else "FAIL", result["name"], result.get("message", ""))
    if run["failed"] or run["exceptions"] or not run["finished"] or run["passed"] != len(TEST_CHECKS):
        raise SystemExit("the tests failed")
    play(client)
    export(client)


def main():
    command = [os.path.abspath(sys.argv[1])] if len(sys.argv) > 1 else \
        ["dotnet", "run", "--project", os.path.join(ROOT, "StradaTool"), "-c", "Release", "--no-launch-profile", "--"]

    # The editor starts before anything is touched: a tool that cannot run leaves the project as it is.
    client = McpClient(command + ["mcp", "--headless", "--new"])
    # The editor creates projects in empty directories only: this folder waits outside meanwhile.
    aside = tempfile.mkdtemp(prefix="prismbreak-author-")
    shutil.move(TOOLS, os.path.join(aside, "Tools"))
    try:
        if os.path.isdir(PROJECT):
            shutil.rmtree(PROJECT)
        client.call("project_create", {"directory": PROJECT, "name": "PrismBreak", "gitIgnore": False})
    finally:
        shutil.move(os.path.join(aside, "Tools"), TOOLS)
        os.rmdir(aside)
    try:
        build(client)
    finally:
        client.close()


if __name__ == "__main__":
    main()

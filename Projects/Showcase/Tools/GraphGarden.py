"""The Graph Garden: the Showcase's exhibit of visual scripting, as data and as functions that take the authoring session.

A round stone platform south of the plaza: eight tubular chimes hang from an iron ring on three posts around a conductor, a
glowing orb that floats; in front of each chime a glowing pad, around them four lanterns, and a signpost with a live line the
graphs write. When the camera comes near, the conductor plays its score, a looping timeline whose eight event tracks are the
chimes: each note strikes its chime, which rings at its pitch, glows in its note's colour, swings and sparks; every bar the
lanterns pulse together. A ball thrown at a chime rings it as hard as it hit, a pad rings its chime, G (or the tour's cue)
plays a flourish, and at night the conductor glows brighter. Six graphs do all of it:

- Music.sgraph, a function library: a note's pitch and its colour;
- Conductor.sgraph: the score and the flourish (timelines), the conductor's float and glow;
- ChimeTube.sgraph: a chime struck by the score, a pad or a ball;
- ChimePad.sgraph: a pad that rings its chime;
- Lantern.sgraph: a lantern that flares on every bar;
- GardenSign.sgraph: the signpost's live line.

Every graph is written through `graph_edit`, as an agent writes it: a declare pass adds each graph's variables, custom events
and functions, then one batch per region adds its nodes and links, arranges them and frames them in a comment box titled
for what it does, in its role's colour. A batch that answers an error, a warning or a hint stops the tool.

The session is duck-typed (Author.py's Showcase): `call(tool, arguments)`, `entity(name, components, parent)`,
`material(path, fields)` (creates the material, or sets it when it exists), `mesh(mesh, material, extra, shadows)` and
`signpost(position, title, detail, facing)`, with the attributes `iron`, `lamp_glass`, `marble` (materials), `day_night` and
`rig` (entities). This module imports nothing of Author.py, so Author.py imports it.
"""
import math
import os
from dataclasses import dataclass, field

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(TOOLS)

# Where the garden stands: the largest free circle south of the plaza (x, y, z), about 4.4 m clear of any mesh or collider.
CENTER = (-1.8, 0.0, 16.8)
PLATFORM_RADIUS = 3.2
PLATFORM_TOP = 0.16
# The iron ring the chimes hang from, and the chimes' pivots just under it. The first screenshot from the home view decided
# the heights (spec-slice4 section 19.2): at 2.6 m the ring's far side crossed the fountain behind it, at 2.2 m it stays below.
RING_HEIGHT = 2.2
RING_RADIUS = 1.9
PIVOT_HEIGHT = RING_HEIGHT - 0.05
POST_RADIUS = 2.05
POST_ANGLES = (90.0, 210.0, 330.0)
TUBE_RADIUS = 1.6
PAD_RADIUS = 2.7
LANTERN_RADIUS = 3.6
# Square to the tour's view of the garden (from the south-east): the nearest two frame it, none stands in front of it.
LANTERN_ANGLES = (0.0, 90.0, 180.0, 270.0)
LANTERN_HEIGHT = 1.7
CONDUCTOR_HEIGHT = 1.35
# The chimes, from the lowest: their notes (C5 D5 E5 G5 A5 C6 D6 E6, a major pentatonic) as semitones above C5, their
# angles around the ring and the lengths of their bars.
NOTES = ("C5", "D5", "E5", "G5", "A5", "C6", "D6", "E6")
SEMITONES = (0, 2, 4, 7, 9, 12, 14, 16)
TUBE_ANGLES = tuple(22.5 + 45.0 * index for index in range(8))
TUBE_LENGTHS = tuple(round(1.5 - 0.1 * index, 2) for index in range(8))
TUBE_THICKNESS = 0.09
# The score: eight seconds at 120 beats a minute (four bars of four beats), as (time, chime) pairs. Any three seconds of it
# hold at least six notes, and one loop strikes every chime.
SCORE_LENGTH = 8.0
SCORE = ((0.0, 0), (0.5, 2), (1.0, 3), (1.5, 5),
         (2.0, 6), (2.25, 5), (2.5, 4), (3.0, 3), (3.5, 2),
         (4.0, 3), (4.5, 4), (5.0, 5), (5.25, 6), (5.5, 7),
         (6.0, 6), (6.5, 5), (6.75, 4), (7.0, 3), (7.5, 1), (7.75, 2))
DOWNBEATS = (0.0, 2.0, 4.0, 6.0)
# How hard the score strikes swells over its four bars; the conductor's light follows the colour of its hue.
SWELL = ((0.0, 0.3), (2.0, 0.65), (4.0, 0.4), (5.5, 1.0), (8.0, 0.3))
HUE = ((0.0, [0.35, 0.6, 1.0, 1.0]), (2.0, [0.55, 0.35, 1.0, 1.0]), (4.0, [1.0, 0.45, 0.7, 1.0]), (6.0, [1.0, 0.7, 0.35, 1.0]),
       (8.0, [0.35, 0.6, 1.0, 1.0]))
# The flourish: two seconds, an arpeggio up the chimes and down again.
FLOURISH_LENGTH = 2.0
FLOURISH = tuple((0.125 * index, index) for index in range(8)) + tuple((1.0 + 0.125 * index, 6 - index) for index in range(7))
# The colours of the twelve notes of an octave, from C: a colour wheel, a semitone a step.
NOTE_COLORS = tuple([round(c, 4) for c in color] + [1.0] for color in (
    (1.0, 0.12, 0.12), (1.0, 0.38, 0.08), (1.0, 0.62, 0.08), (0.95, 0.88, 0.12), (0.55, 0.95, 0.15), (0.15, 0.95, 0.35),
    (0.1, 0.92, 0.75), (0.1, 0.7, 1.0), (0.18, 0.4, 1.0), (0.45, 0.22, 1.0), (0.75, 0.2, 1.0), (1.0, 0.18, 0.62)))
TEMPO = 120.0
REACH = 10.0

# The exhibit's row in the HUD's feature list (name, what it shows, where on the ground (x, z), and within how many meters
# it is fully lit), and the tour's shot (Author.py's TOUR rows without the frame, which is the layout's own).
FEATURE = ("Visual scripting", "graphs play the chimes; a timeline is the score", (CENTER[0], CENTER[2]), 5.0)
SHOT = ("Graph Garden", (2.4, 2.0, 13.2), (-1.8, 1.4, 16.8), 4.0, "Visual scripting",
        "six graphs play the chimes: a timeline is the score", "graph_garden", 1.2)
# The names of the entities the exhibit adds to the scene beside its root: the signpost (under the signposts).
ROOT = "Graph Garden"
SIGNPOST = "Signpost: Visual Scripting"
# Where the signpost stands (x, z) and the point it faces: beyond the chimes from the tour's shot (in front of it, it would
# hide them), turned between the shot and the home view.
SIGNPOST_PLACE = (CENTER[0] - 3.2, CENTER[2] + 2.7)
SIGNPOST_FACING = (4.0, 20.0)

# Comment boxes are coloured by role.
EVENTS = [0.2, 0.42, 0.95, 0.22]
RULES = [0.95, 0.55, 0.15, 0.22]
EFFECTS = [0.62, 0.3, 0.95, 0.22]


def script(name):
    return f"asset://Scripts/GraphGarden/{name}.sgraph"


MUSIC = script("Music")
CONDUCTOR = script("Conductor")
CHIME_TUBE = script("ChimeTube")
CHIME_PAD = script("ChimePad")
LANTERN = script("Lantern")
GARDEN_SIGN = script("GardenSign")
# In the order they are written: the library first, then each graph before the graphs that send to it or read it.
GRAPHS = (MUSIC, CHIME_TUBE, CHIME_PAD, LANTERN, CONDUCTOR, GARDEN_SIGN)
LIBRARIES = (MUSIC,)
ENTITY = "Strada.Entity"


# --- Transforms (the garden stands in the layout's own frame, which is the world's) -----------------------------------------

def quaternion(euler):
    """The rotation of Euler angles in degrees as [x, y, z, w], as the engine (glm) composes them: Z * Y * X."""
    half = [math.radians(angle) * 0.5 for angle in euler]
    c = [math.cos(angle) for angle in half]
    s = [math.sin(angle) for angle in half]
    return [s[0] * c[1] * c[2] - c[0] * s[1] * s[2], c[0] * s[1] * c[2] + s[0] * c[1] * s[2],
            c[0] * c[1] * s[2] - s[0] * s[1] * c[2], c[0] * c[1] * c[2] + s[0] * s[1] * s[2]]


def transform(translation, euler=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    return {"Translation": list(translation), "Rotation": quaternion(euler), "Scale": list(scale)}


def around(radius, angle, height):
    """A point at the radius and angle (degrees, from +X towards +Z) around the garden's center, at the height."""
    radians = math.radians(angle)
    return (CENTER[0] + radius * math.cos(radians), height, CENTER[2] + radius * math.sin(radians))


def outward_yaw(angle):
    """The yaw that turns an entity's +Z outwards from the center at the angle."""
    radians = math.radians(angle)
    return math.degrees(math.atan2(math.cos(radians), math.sin(radians)))


def entity_value(entity):
    return {"Type": "Entity", "Value": entity}


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
    left to right and framed by a comment box. No link leaves a region: regions share variables, functions and events."""

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
        declared = author.declared[graph]
        if function is not None:
            entry, ret = declared["functions"][function]
            self.entry = self.existing(entry, "Then")
            self.ret = self.existing(ret)

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        if kind is None:
            self.author.pending.setdefault(self.graph, []).append(self)
        return False

    def existing(self, node_id, output=None):
        self.framed.append(node_id)
        return Node(node_id, output)

    def link(self, source, target):
        if isinstance(source, Node):
            source = source.out
        self.connections.append({"op": "Connect", "from": list(source), "to": list(target)})

    def add(self, type_id, settings=None, output=None, comment=None, **inputs):
        self.count += 1
        label = f"$n{self.count}"
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

    def expr(self, formula, **inputs):
        return self.add("Expression", {"Formula": formula}, "Result", **inputs).out

    def get(self, variable, graph=None, target=None):
        settings = {"Variable": variable}
        if graph is not None:
            settings["Graph"] = graph
        return self.add("Variable.Get", settings, "Value", Target=target).out

    def set(self, variable, value=None, comment=None):
        return self.add("Variable.Set", {"Variable": variable}, "Value", comment=comment, Value=value)

    def lit(self, type_id, value):
        return self.add("Literal", {"Type": type_id, "Value": value}, "Value").out

    def pure(self, type_id, output="Result", **inputs):
        return self.add(type_id, None, output, **inputs).out

    def call(self, type_id, output=None, comment=None, **inputs):
        return self.add(type_id, None, output, comment=comment, **inputs)

    def self_(self):
        return self.add("Self", None, "Self").out

    def world_position(self, entity=None):
        return self.pure("Get:Strada.TransformComponent.WorldTranslation", "Value", Target=entity)

    def library(self, function, output, **inputs):
        return self.add("Function.Call", {"Graph": MUSIC, "Function": function}, output, **inputs).out

    def own(self, function, **inputs):
        return self.add("Function.Call", {"Function": function}, **inputs)

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
        """A typed send (graph given: its parameters are the target event's), or one to an event of this graph, whose
        arguments are (type, value) pairs."""
        settings = {"Name": name}
        if graph is not None:
            settings["Graph"] = graph
        else:
            settings["Parameters"] = [{"Name": key, "Type": value_type} for key, (value_type, _) in arguments.items()]
            arguments = {key: value for key, (_, value) in arguments.items()}
        return self.add("Event.Send", settings, comment=comment, Target=target, **arguments)

    def broadcast(self, name, graph, comment=None, **arguments):
        return self.add("Event.Broadcast", {"Name": name, "Graph": graph}, "Count", comment=comment, **arguments)

    def timeline(self, length, tracks, loop=False):
        return self.add("Flow.Timeline", {"Timeline": {"Length": length, "Loop": loop, "Tracks": tracks}})

    def operations(self):
        framed = list(dict.fromkeys(self.framed))
        return (self.additions + self.connections + self.after + [
            {"op": "Arrange", "scope": "Created"},
            {"op": "Arrange", "scope": "Nodes", "nodes": framed},
            {"op": "AddComment", "text": self.title, "elements": framed, "color": self.color},
        ])


def curve(name, type_id, keys, interpolation=None):
    """A timeline's curve track from (time, value) keys (cubic unless an interpolation is given)."""
    written = []
    for time, value in keys:
        key = {"Time": time, "Value": value}
        if interpolation is not None:
            key["Interpolation"] = interpolation
        written.append(key)
    return {"Name": name, "Type": type_id, "Keys": written}


def events(name, times):
    """A timeline's event track: an exec output that fires at each time."""
    return {"Name": name, "Type": "exec", "Keys": [{"Time": time} for time in times]}


def chime_tracks(notes):
    """An event track per chime, named after its note, firing at the times the (time, chime) pairs strike it."""
    return [events(NOTES[chime], [time for time, struck in notes if struck == chime]) for chime in range(len(NOTES))]


class Author:
    """Writes the graphs: their declarations, then their regions, checking every batch."""

    def __init__(self, session):
        self.session = session
        self.declared = {}
        self.pending = {}
        self.batches = 0

    def edit(self, graph, description, operations, function=None):
        arguments = {"graph": graph, "description": description, "operations": operations, "refuseNewErrors": True}
        if function is not None:
            arguments["function"] = function
        result = self.session.call("graph_edit", arguments)
        summary = result["summary"]
        if summary["errors"] or summary["warnings"] or summary["brokenReferrers"] or summary["staleSenders"]:
            problems = [f"{d['Severity']} {d['Code']}: {d['Message']}" for d in result["diagnostics"]]
            raise SystemExit(f"{description} in {graph}: {summary}\n  " + "\n  ".join(problems))
        if result["hints"]:
            hints = [f"{hint['kind']}: {hint['message']}" for hint in result["hints"]]
            raise SystemExit(f"{description} in {graph} has hints:\n  " + "\n  ".join(hints))
        self.batches += 1
        return result

    def declare(self, graph, description, variables=(), declared_events=(), functions=()):
        """The graph's interface: variables (name, type, value, public, tooltip, range), custom events (name, [(parameter,
        type)]) and functions (name, pure, inputs, outputs)."""
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
        for index, (name, parameters) in enumerate(declared_events):
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
                       for index, (name, parameters) in enumerate(declared_events)},
            "functions": {name: (labels[f"$entry{index}"], labels[f"$return{index}"]) for index, (name, *_) in enumerate(functions)},
        }

    def region(self, graph, title, color, function=None):
        return Region(self, graph, title, color, function)

    def finish(self, graph):
        """Writes the graph's regions: its functions first, in the order they are declared (each before the regions that
        call it, so a call's node is measured as it stays), then the event graph's regions in the order they were written."""
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


# --- The declare pass ---------------------------------------------------------------------------------------------------------

def declare_all(author):
    author.declare(MUSIC, "The garden's music: a note's pitch, and the colour every graph shows it in.", functions=(
        ("PitchOf", True, (("Semitone", "int"),), (("Pitch", "float"),)),
        ("NoteColor", True, (("Semitone", "int"),), (("Color", "Strada.Color"),)),
    ))
    author.declare(CHIME_TUBE, "A tubular chime: struck by the score, a pad or a ball, it rings at its note's pitch, as loud as "
                   "the strike, glows in the note's colour, swings and sparks.", variables=(
        public("Semitone", "int", 0, "The chime's note in semitones above C5: its pitch and its colour", (0, 24)),
        public("Bar", ENTITY, None, "The bar that glows and swings (the chime's child, hanging from it)"),
        public("Strikes", "int", 0, "Read-out: times the chime was struck", (0, 1000000000)),
        public("LastVelocity", "float", 0.0, "Read-out: how hard the last strike was, from 0 to 1", (0.0, 1.0)),
        public("Glow", "float", 1.0, "Read-out: how brightly the bar glows, 1 at rest", (0.0, 10.0)),
        private("Look", "Strada.Material"),
        private("Half", "float", 0.5),
    ), declared_events=(("Strike", (("Velocity", "float"),)),))
    author.declare(CHIME_PAD, "A pad in front of a chime: whoever steps on it rings the chime, and the pad flashes.", variables=(
        public("Tube", ENTITY, None, "The chime the pad rings"),
        public("Plate", ENTITY, None, "The plate that flashes (the pad's child)"),
        private("Look", "Strada.Material"),
    ))
    author.declare(LANTERN, "A lantern around the chimes: on every bar of the score it flares in the bar's colour.",
                   declared_events=(("Pulse", (("Bar", "int"),)),))
    author.declare(CONDUCTOR, "The conductor: while the camera is near, its score (a looping timeline) strikes the chimes and "
                   "pulses the lanterns on every bar; G or the tour's cue plays a flourish; the orb floats and glows, brighter at "
                   "night.", variables=(
        public("Tempo", "float", TEMPO, "Beats a minute: the score plays at Tempo / 120 times its speed", (60.0, 200.0)),
        public("Lighting", ENTITY, None, "The day and night switch (a DayNight script): the orb glows brighter at night"),
        public("Chimes", ENTITY, None, "The parent of the eight chimes, from the lowest note to the highest"),
        public("Listener", ENTITY, None, "The camera rig: the score plays while it is within Reach"),
        public("Reach", "float", REACH, "Meters from the conductor within which the camera hears the score", (1.0, 50.0)),
        public("Bars", "int", 0, "Read-out: the bars the score has played", (0, 1000000000)),
        public("Notes", "int", 0, "Read-out: the notes the score and the flourish have struck", (0, 1000000000)),
        public("Playing", "bool", False, "Read-out: the score plays (the camera is within Reach)"),
        private("Home", "Strada.Vector3", [0.0, 0.0, 0.0]),
        private("Clock", "float", 0.0),
        private("Swell", "float", SWELL[0][1]),
        private("Hue", "Strada.Color", HUE[0][1]),
    ), declared_events=(("Perform", ()), ("TourEnded", ())), functions=(
        ("Strike", False, (("Index", "int"),), ()),
    ))
    author.declare(GARDEN_SIGN, "The signpost's live line: the conductor's tempo, bar and notes, read four times a second.",
                   variables=(public("Conductor", ENTITY, None, "The conductor whose score the line reads"),))


# --- The bodies -----------------------------------------------------------------------------------------------------------------

def write_music(author):
    with author.region(MUSIC, "Pitch: twelve semitones make an octave", EFFECTS, "PitchOf") as b:
        b.link(b.expr("pow(2, semitone / 12.0)", semitone=b.entry["Semitone"]), b.ret["Pitch"])
    with author.region(MUSIC, "Colour: a colour wheel, one step a semitone, from C", EFFECTS, "NoteColor") as b:
        colors = b.lit("Strada.Color[]", list(NOTE_COLORS))
        index = b.expr("(semitone % 12 + 12) % 12", semitone=b.entry["Semitone"])
        b.link(b.pure("Array.Get", "Element", Array=colors, Index=index), b.ret["Color"])


def write_chime_tube(author):
    with author.region(CHIME_TUBE, "Start: the bar glows in its note's colour, through its own material", EFFECTS) as b:
        start = b.event("Event.Create")
        clone = b.call("Call:Strada.Material.Clone()", "Result",
                       Target=b.pure("Call:Strada.MeshComponent.GetMaterial(int)", Target=b.get("Bar"), Index=0))
        keep = b.set("Look", clone.out)
        use = b.call("Call:Strada.MeshComponent.SetMaterial(int,Strada.Material)", Target=b.get("Bar"), Index=0, Material=b.get("Look"))
        color = b.library("NoteColor", "Color", Semitone=b.get("Semitone"))
        tint = b.call("Set:Strada.Material.Emissive", Target=b.get("Look"), Value=color)
        glow = b.call("Set:Strada.Material.EmissiveIntensity", Target=b.get("Look"), Value=b.get("Glow"))
        sparks = b.call("Set:Strada.ParticleSystemComponent.StartColor", Value=b.library("NoteColor", "Color", Semitone=b.get("Semitone")))
        half = b.set("Half", b.expr("size.y * 0.5", size=b.pure("Get:Strada.TransformComponent.Scale", "Value", Target=b.get("Bar"))),
                     comment="The bar hangs half its length below the chime's pivot")
        b.chain(start, clone, keep, use, tint, glow, sparks, half)
    with author.region(CHIME_TUBE, "Strike: ring at the note's pitch, as loud as the strike, glow and swing", RULES) as b:
        strike = b.custom("Strike")
        velocity = b.set("LastVelocity", b.expr("clamp(velocity, 0, 1)", velocity=strike["Velocity"]))
        count = b.set("Strikes", b.expr("strikes + 1", strikes=b.get("Strikes")))
        pitch = b.call("Set:Strada.AudioSourceComponent.Pitch", Value=b.library("PitchOf", "Pitch", Semitone=b.get("Semitone")))
        volume = b.call("Set:Strada.AudioSourceComponent.Volume", Value=b.expr("0.4 + 0.6 * velocity", velocity=b.get("LastVelocity")))
        ring = b.call("Call:Strada.AudioSourceComponent.Play()")
        emit = b.call("Call:Strada.ParticleSystemComponent.Emit(uint)", Count=b.expr("uint(8 + 24 * velocity)", velocity=b.get("LastVelocity")))
        swing = b.timeline(0.8, [
            curve("Light", "float", [(0.0, 1.0), (0.8, 0.0)]),
            curve("Swing", "Strada.Vector3", [(0.0, [0.0, 0.0, 0.0]), (0.2, [8.0, 0.0, 0.0]), (0.5, [-5.0, 0.0, 0.0]), (0.8, [0.0, 0.0, 0.0])]),
        ])
        glow = b.set("Glow", b.expr("lerp(1, 8, light)", light=swing["Light"]))
        shine = b.call("Set:Strada.Material.EmissiveIntensity", Target=b.get("Look"), Value=b.get("Glow"))
        turn = b.call("Set:Strada.TransformComponent.EulerAngles", Target=b.get("Bar"), Value=swing["Swing"])
        hang = b.call("Set:Strada.TransformComponent.Translation", Target=b.get("Bar"),
                      Value=b.expr("vec3(0, -half * cos(radians(swing.x)), -half * sin(radians(swing.x)))", half=b.get("Half"),
                                   swing=swing["Swing"]),
                      comment="The bar swings about the pivot at its top")
        b.chain(strike, velocity, count, pitch, volume, ring, emit)
        b.flow(emit, swing["PlayFromStart"])
        b.chain(swing["Update"], glow, shine, turn, hang)
    with author.region(CHIME_TUBE, "Hit: a ball strikes the chime as hard as it flies", EVENTS) as b:
        hit = b.event("Event.CollisionEnter")
        flies = b.add("Call:Strada.Entity.HasComponent<T>()", {"TypeArguments": ["Strada.RigidBodyComponent"]}, "Result", Target=hit["Other"])
        body = b.branch(flies.out, comment="Only bodies that fly strike it: a walker that bumps into it does not")
        speed = b.pure("Get:Strada.RigidBodyComponent.LinearVelocity", "Value", Target=hit["Other"])
        strike = b.send("Strike", b.self_(), Velocity=("float", b.expr("clamp(length(v) / 8, 0.2, 1)", v=speed)))
        b.chain(hit, body)
        b.flow(body["True"], strike)


def write_chime_pad(author):
    with author.region(CHIME_PAD, "Start: the plate flashes through a material of its own", EFFECTS) as b:
        start = b.event("Event.Create")
        clone = b.call("Call:Strada.Material.Clone()", "Result",
                       Target=b.pure("Call:Strada.MeshComponent.GetMaterial(int)", Target=b.get("Plate"), Index=0))
        keep = b.set("Look", clone.out)
        use = b.call("Call:Strada.MeshComponent.SetMaterial(int,Strada.Material)", Target=b.get("Plate"), Index=0, Material=b.get("Look"))
        b.chain(start, clone, keep, use)
    with author.region(CHIME_PAD, "Step: whoever steps on the pad rings its chime, and the plate flashes", EVENTS) as b:
        enter = b.event("Event.TriggerEnter")
        strike = b.send("Strike", b.get("Tube"), CHIME_TUBE, Velocity=0.8)
        flash = b.add("Flow.Tween", {"Easing": "EaseOut"}, Duration=0.6)
        shine = b.call("Set:Strada.Material.EmissiveIntensity", Target=b.get("Look"), Value=b.expr("lerp(9, 1.5, alpha)", alpha=flash["Alpha"]))
        b.chain(enter, strike, flash)
        b.flow(flash["Update"], shine)


def write_lantern(author):
    with author.region(LANTERN, "Pulse: every bar the lantern flares in the bar's colour and fades", EFFECTS) as b:
        pulse = b.custom("Pulse")
        color = b.call("Set:Strada.PointLightComponent.Color",
                       Value=b.library("NoteColor", "Color", Semitone=b.expr("bar * 5", bar=pulse["Bar"])),
                       comment="Each bar a fourth higher round the colour wheel")
        flare = b.add("Flow.Tween", {"Easing": "Linear"}, Duration=0.7)
        light = b.call("Set:Strada.PointLightComponent.Intensity", Value=b.expr("2 + 7 * sin(alpha * pi)", alpha=flare["Alpha"]))
        rest = b.call("Set:Strada.PointLightComponent.Intensity", Value=2.0)
        b.chain(pulse, color, flare)
        b.flow(flare["Update"], light)
        b.flow(flare["Completed"], rest)


def write_conductor(author):
    with author.region(CONDUCTOR, "Strike: a note strikes its chime, as hard as the score swells", RULES, "Strike") as b:
        chime = b.pure("Array.Get", "Element", Array=b.pure("Get:Strada.Entity.Children", "Value", Target=b.get("Chimes")),
                       Index=b.entry["Index"])
        strike = b.send("Strike", chime, CHIME_TUBE, Velocity=b.expr("0.5 + 0.4 * swell", swell=b.get("Swell")))
        count = b.set("Notes", b.expr("notes + 1", notes=b.get("Notes")))
        b.chain(b.entry, strike, count, b.ret)
    with author.region(CONDUCTOR, "Float and glow: the orb bobs, brighter at night, swelling with the score", EFFECTS) as b:
        start = b.event("Event.Create")
        home = b.set("Home", b.world_position())
        tick = b.event("Event.Update")
        clock = b.set("Clock", b.expr("clock + dt", clock=b.get("Clock"), dt=tick["DeltaTime"]))
        bob = b.call("Set:Strada.TransformComponent.WorldTranslation",
                     Value=b.expr("home + vec3(0, sin(clock * 1.7) * 0.08, 0)", home=b.get("Home"), clock=b.get("Clock")))
        night = b.pure("Get:Showcase.DayNight.Blend", "Value", Target=b.get("Lighting"))
        light = b.call("Set:Strada.PointLightComponent.Intensity",
                       Value=b.expr("lerp(2, 7, night) * (0.6 + 0.4 * swell)", night=night, swell=b.get("Swell")),
                       comment="The C# day and night switch's Blend, a game member made a node")
        hue = b.call("Set:Strada.PointLightComponent.Color", Value=b.get("Hue"))
        b.chain(start, home)
        b.chain(tick, clock, bob, light, hue)
    with author.region(CONDUCTOR, "Score: while the camera is near, a looping timeline plays the chimes", RULES) as b:
        step = b.event("Event.FixedUpdate")
        near = b.branch(b.expr("distance(listener, here) < reach", listener=b.world_position(b.get("Listener")),
                               here=b.world_position(), reach=b.get("Reach")))
        playing = b.set("Playing", True)
        resting = b.set("Playing", False)
        score = b.timeline(SCORE_LENGTH, chime_tracks(SCORE) + [
            events("Downbeat", DOWNBEATS),
            curve("Swell", "float", SWELL),
            curve("Hue", "Strada.Color", HUE),
        ], loop=True)
        b.link(b.expr("tempo / 120", tempo=b.get("Tempo")), score["Rate"])
        b.chain(step, near)
        b.chain(near["True"], playing)
        b.flow(playing, score["Play"])
        b.chain(near["False"], resting)
        b.flow(resting, score["Stop"])
        for index, note in enumerate(NOTES):
            b.flow(score[note], b.own("Strike", Index=index))
        pulse = b.broadcast("Pulse", LANTERN, Bar=b.get("Bars"), comment="Every lantern pulses on the bar's downbeat")
        bars = b.set("Bars", b.expr("bars + 1", bars=b.get("Bars")))
        b.flow(score["Downbeat"], pulse)
        b.chain(pulse, bars)
        swell = b.set("Swell", score["Swell"])
        hue = b.set("Hue", score["Hue"])
        b.chain(score["Update"], swell, hue)
    with author.region(CONDUCTOR, "Flourish: G or the tour's cue plays an arpeggio up and down", EVENTS) as b:
        key = b.event("Event.KeyPressed", {"Key": "G"})
        perform = b.custom("Perform")
        ended = b.custom("TourEnded")
        flourish = b.timeline(FLOURISH_LENGTH, chime_tracks(FLOURISH))
        b.flow(key, flourish["PlayFromStart"])
        b.flow(perform, flourish["PlayFromStart"])
        b.flow(ended, flourish["Stop"])
        for index, note in enumerate(NOTES):
            b.flow(flourish[note], b.own("Strike", Index=index))


def write_garden_sign(author):
    with author.region(GARDEN_SIGN, "Read the conductor four times a second: tempo, bar and notes", EFFECTS) as b:
        start = b.event("Event.Create")
        every = b.add("Flow.Interval", Interval=0.25)
        conductor = b.get("Conductor")
        runs = b.branch(b.add("RunsGraph", {"Graph": CONDUCTOR}, "Result", Target=conductor).out,
                        comment="The conductor's variables are read only while it runs its graph")
        line = b.add("FormatString", {"Format": "{Tempo} bpm · bar {Bar} · {Notes} notes"}, "Result",
                     Tempo=b.expr("int(round(tempo))", tempo=b.get("Tempo", CONDUCTOR, b.get("Conductor"))),
                     Bar=b.get("Bars", CONDUCTOR, b.get("Conductor")), Notes=b.get("Notes", CONDUCTOR, b.get("Conductor")))
        show = b.call("Set:Strada.TextComponent.Text", Value=line.out)
        b.flow(start, every["Start"])
        b.chain(every["Tick"], runs)
        b.flow(runs["True"], show)


WRITERS = {MUSIC: write_music, CHIME_TUBE: write_chime_tube, CHIME_PAD: write_chime_pad, LANTERN: write_lantern,
           CONDUCTOR: write_conductor, GARDEN_SIGN: write_garden_sign}


def prepare_graphs(session):
    """Every graph of the garden as an empty document: created when missing, emptied (graph_set) when it exists, so its
    asset handle stays and the batches that follow write it exactly as from scratch."""
    for graph in GRAPHS:
        path = graph[len("asset://"):]
        if os.path.exists(os.path.join(PROJECT, "Assets", *path.split("/"))):
            document = session.call("graph_get", {"graph": graph, "analyze": False})["document"]
            empty = {"Description": "", "Variables": [], "Functions": [], "Graph": {"Nodes": [], "Links": [], "Comments": []}}
            if "Kind" in document:
                empty = {"Kind": document["Kind"], **empty}
            session.call("graph_set", {"graph": graph, "document": empty})
        else:
            session.call("graph_create", {"path": path, "template": "Library" if graph in LIBRARIES else "Empty"})


def write_graphs(session):
    prepare_graphs(session)
    author = Author(session)
    declare_all(author)
    for graph in GRAPHS:
        WRITERS[graph](author)
        author.finish(graph)
    print(f"the Graph Garden: wrote {len(GRAPHS)} graphs in {author.batches} batches")


# --- The entities ---------------------------------------------------------------------------------------------------------------

@dataclass
class GardenIds:
    """The entities of the garden the scene, the tour and the tests refer to."""
    root: str = ""
    conductor: str = ""
    chimes: str = ""
    tubes: list = field(default_factory=list)
    pads: str = ""
    pad_list: list = field(default_factory=list)
    lanterns: str = ""
    signpost: str = ""
    sign: str = ""


def make_materials(session):
    return {
        "Orb": session.material("GraphGarden/Orb", {"BaseColor": [0.85, 0.92, 1.0, 1.0], "Roughness": 0.2,
                                                    "EmissiveColor": [0.55, 0.78, 1.0], "EmissiveIntensity": 7.0}),
        # The chimes' brass: each chime's graph copies it and makes it glow in its note's colour.
        "Tube": session.material("GraphGarden/Tube", {"BaseColor": [0.9, 0.78, 0.55, 1.0], "Metallic": 1.0, "Roughness": 0.22,
                                                      "EmissiveColor": [1.0, 1.0, 1.0], "EmissiveIntensity": 0.0}),
        "Pad": session.material("GraphGarden/Pad", {"BaseColor": [0.04, 0.045, 0.06, 1.0], "Roughness": 0.35,
                                                    "EmissiveColor": [0.45, 0.78, 1.0], "EmissiveIntensity": 1.5}),
        # The lanterns' bulbs, inside their glass.
        "Lamp": session.material("GraphGarden/Lamp", {"BaseColor": [0.02, 0.02, 0.02, 1.0], "Roughness": 0.4,
                                                      "EmissiveColor": [1.0, 0.74, 0.45], "EmissiveIntensity": 8.0}),
    }


def sparks():
    """The sparks a strike throws from the top of a chime, where the score's mallet hits it (its graph tints them)."""
    return {"Emitting": False, "Shape": "Sphere", "Radius": 0.06, "LifetimeMin": 0.35, "LifetimeMax": 0.8, "SpeedMin": 0.8,
            "SpeedMax": 2.2, "ConeAngle": 70.0, "GravityFactor": 0.5, "Drag": 0.6, "StartSize": 0.05, "EndSize": 0.01,
            "StartColor": [1.0, 0.9, 0.7, 1.0], "EndColor": [1.0, 0.6, 0.3, 0.0], "Intensity": 7.0, "MaxParticles": 120}


def build(session):
    """The exhibit: its materials, its six graphs and its entities (under the root, and its signpost under the signposts).
    Returns the entities the rest of the scene refers to."""
    materials = make_materials(session)
    write_graphs(session)
    ids = GardenIds()
    entity = session.entity
    mesh = session.mesh
    ids.root = entity(ROOT)
    x, _, z = CENTER
    entity("Platform", {"Transform": transform((x, PLATFORM_TOP / 2.0, z), scale=(2.0 * PLATFORM_RADIUS, PLATFORM_TOP, 2.0 * PLATFORM_RADIUS)),
                        **mesh("Cylinder", session.marble), "RigidBody": {"Type": "Static"},
                        "MeshCollider": {"Mesh": "builtin://Cylinder", "Convex": True}}, ids.root)
    # The ring (a torus of radius 1, flattened to a band) rests on three posts.
    entity("Ring", {"Transform": transform((x, RING_HEIGHT, z), scale=(RING_RADIUS, 1.0, RING_RADIUS)),
                    **mesh("asset://Meshes/Hoop.gltf", session.iron)}, ids.root)
    for index, angle in enumerate(POST_ANGLES):
        entity(f"Post {index + 1}", {"Transform": transform(around(POST_RADIUS, angle, (RING_HEIGHT + PLATFORM_TOP) / 2.0),
                                                            scale=(0.09, RING_HEIGHT - PLATFORM_TOP, 0.09)),
                                     **mesh("Cylinder", session.iron), "RigidBody": {"Type": "Static"}, "BoxCollider": {}}, ids.root)
    # The conductor: the orb holds its light, so it casts no shadow.
    ids.conductor = entity("Conductor", {
        "Transform": transform((x, CONDUCTOR_HEIGHT, z), scale=(0.42, 0.42, 0.42)), **mesh("Sphere", materials["Orb"], shadows=False),
        "PointLight": {"Color": HUE[0][1][:3], "Intensity": 2.0, "Range": 7.0}}, ids.root)
    # The chimes: each a pivot under the ring (turned to face outwards, so its bar swings out and back) with its collider,
    # sound, sparks and graph; its bar hangs from it, a bracket holds it to the ring.
    ids.chimes = entity("Chimes", None, ids.root)
    for index, (angle, length, semitone) in enumerate(zip(TUBE_ANGLES, TUBE_LENGTHS, SEMITONES)):
        pivot = entity(f"Tube {index + 1}", {
            "Transform": transform(around(TUBE_RADIUS, angle, PIVOT_HEIGHT), (0.0, outward_yaw(angle), 0.0)),
            "RigidBody": {"Type": "Static"},
            "CapsuleCollider": {"Radius": TUBE_THICKNESS, "HalfHeight": length / 2.0 - TUBE_THICKNESS, "Offset": [0.0, -length / 2.0, 0.0]},
            "AudioSource": {"Clip": "asset://Audio/Chime.wav", "Volume": 0.8, "Spatial": True, "MinDistance": 2.0, "MaxDistance": 18.0},
            "ParticleSystem": sparks()}, ids.chimes)
        entity("Bracket", {"Transform": transform((0.0, 0.03, (RING_RADIUS - TUBE_RADIUS) / 2.0), scale=(0.03, 0.03, RING_RADIUS - TUBE_RADIUS + 0.04)),
                           **mesh("Cube", session.iron)}, pivot)
        bar = entity("Bar", {"Transform": transform((0.0, -length / 2.0, 0.0), scale=(TUBE_THICKNESS, length, TUBE_THICKNESS)),
                             **mesh("Cylinder", materials["Tube"])}, pivot)
        session.call("component_add", {"entity": pivot, "component": "VisualScript", "fields": {"Graph": CHIME_TUBE, "Variables": {
            "Semitone": {"Type": "Int32", "Value": semitone}, "Bar": entity_value(bar)}}})
        ids.tubes.append(pivot)
    # The conductor's graph comes once there are chimes to name.
    session.call("component_add", {"entity": ids.conductor, "component": "VisualScript", "fields": {"Graph": CONDUCTOR, "Variables": {
        "Lighting": entity_value(session.day_night), "Chimes": entity_value(ids.chimes), "Listener": entity_value(session.rig)}}})
    # A pad in front of each chime: its trigger rises above it, its plate (a child, so the trigger keeps its size) glows.
    ids.pads = entity("Pads", None, ids.root)
    for index, (angle, tube) in enumerate(zip(TUBE_ANGLES, ids.tubes)):
        pad = entity(f"Pad {index + 1}", {"Transform": transform(around(PAD_RADIUS, angle, PLATFORM_TOP)), "RigidBody": {"Type": "Static"},
                                          "BoxCollider": {"HalfExtents": [0.35, 0.6, 0.35], "Offset": [0.0, 0.6, 0.0], "IsTrigger": True}},
                     ids.pads)
        plate = entity("Plate", {"Transform": transform((0.0, 0.02, 0.0), scale=(0.7, 0.04, 0.7)), **mesh("Cylinder", materials["Pad"])}, pad)
        session.call("component_add", {"entity": pad, "component": "VisualScript", "fields": {"Graph": CHIME_PAD, "Variables": {
            "Tube": entity_value(tube), "Plate": entity_value(plate)}}})
        ids.pad_list.append(pad)
    # Four lanterns around the platform: a post with a glass globe and a bulb, the light and the graph at the globe.
    ids.lanterns = entity("Lanterns", None, ids.root)
    for index, angle in enumerate(LANTERN_ANGLES):
        lantern = entity(f"Lantern {index + 1}", {"Transform": transform(around(LANTERN_RADIUS, angle, LANTERN_HEIGHT)),
                                                  "PointLight": {"Color": [1.0, 0.72, 0.45], "Intensity": 2.0, "Range": 5.0},
                                                  "VisualScript": {"Graph": LANTERN}}, ids.lanterns)
        entity("Post", {"Transform": transform((0.0, -LANTERN_HEIGHT / 2.0, 0.0), scale=(0.07, LANTERN_HEIGHT, 0.07)),
                        **mesh("Cylinder", session.iron), "RigidBody": {"Type": "Static"}, "BoxCollider": {}}, lantern)
        entity("Globe", {"Transform": transform((0.0, 0.12, 0.0), scale=(0.3, 0.3, 0.3)), **mesh("Sphere", session.lamp_glass, shadows=False)},
               lantern)
        entity("Bulb", {"Transform": transform((0.0, 0.12, 0.0), scale=(0.11, 0.11, 0.11)), **mesh("Sphere", materials["Lamp"], shadows=False)},
               lantern)
        entity("Cap", {"Transform": transform((0.0, 0.3, 0.0), scale=(0.16, 0.05, 0.16)), **mesh("Cylinder", session.iron)}, lantern)
    # The signpost, with a live line under its text that the sign's graph writes.
    ids.signpost = session.signpost(SIGNPOST_PLACE, "VISUAL SCRIPTING",
                                    "six graphs play the chimes · a timeline is\nthe score · press G or step on a pad", SIGNPOST_FACING)
    ids.sign = entity("Live", {"Transform": transform((0.0, 1.42, 0.05)),
                               "Text": {"Text": "", "Font": "builtin://DefaultFont", "Color": [1.0, 0.72, 0.35, 1.0], "FontSize": 0.1,
                                        "Alignment": "Center"},
                               "VisualScript": {"Graph": GARDEN_SIGN, "Variables": {"Conductor": entity_value(ids.conductor)}}}, ids.signpost)
    return ids


def test_fields(ids):
    """The fields of the Showcase's test script that name the garden's entities."""
    return {"Conductor": entity_value(ids.conductor), "Chimes": entity_value(ids.chimes), "GardenPads": entity_value(ids.pads),
            "GardenSign": entity_value(ids.sign)}

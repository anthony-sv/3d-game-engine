"""Writing Moto's visual scripts through `graph_edit`, as an agent writes them, and checking that they read well.

A declare pass adds each graph's description, variables, custom events and functions; then one batch per region adds its
nodes and links, arranges them and frames them in a comment box titled for what it does, in its role's colour. A batch
that answers an error, a warning, a broken referrer or a hint stops the tool. verify() checks the written graphs as the
engine's SampleGraphsTests reads them: every node in exactly one titled box, each box arranged.

The session is duck-typed: anything with `call(tool, arguments)` that returns a tool's JSON result.
"""
import os

# Comment boxes are coloured by role.
EVENTS = [0.2, 0.42, 0.95, 0.22]
RULES = [0.95, 0.55, 0.15, 0.22]
EFFECTS = [0.62, 0.3, 0.95, 0.22]
CHECK = [0.45, 0.6, 0.7, 0.22]

ENTITY = "Strada.Entity"
MAX_TITLE = 72


def script(name):
    return f"asset://Scripts/{name}.sgraph"


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
        if len(title) > MAX_TITLE:
            raise SystemExit(f"the region title '{title}' is longer than {MAX_TITLE} characters")
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

    def once(self, key, make):
        """A value the region reads in several places, made once: a pure node evaluates again for every reader, so one node
        serves a read before a wait and after it."""
        if key not in self.shared:
            self.shared[key] = make()
        return self.shared[key]

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

    def var(self, variable):
        """One of the graph's own variables, read by one node per region."""
        return self.once(("var", variable), lambda: self.get(variable))

    def set(self, variable, value=None, comment=None):
        return self.add("Variable.Set", {"Variable": variable}, "Value", comment=comment, Value=value)

    def lit(self, type_id, value):
        return self.add("Literal", {"Type": type_id, "Value": value}, "Value").out

    def pure(self, type_id, output="Result", **inputs):
        return self.add(type_id, None, output, **inputs).out

    def call(self, type_id, output=None, comment=None, **inputs):
        return self.add(type_id, None, output, comment=comment, **inputs)

    def equal(self, a, b):
        return self.pure("Operator.Equal", A=a, B=b)

    def both(self, a, b):
        return self.pure("Operator.And", A=a, B=b)

    def fmt(self, text, **inputs):
        return self.add("FormatString", {"Format": text}, "Result", **inputs).out

    def self_(self):
        return self.add("Self", None, "Self").out

    def runs(self, graph, target):
        return self.add("RunsGraph", {"Graph": graph}, "Result", Target=target).out

    # Calls, events and messages.
    def library(self, graph, function, output=None, **inputs):
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
        """A typed send (graph given: its parameters are the target event's), or one to an event of this graph, whose
        arguments are (type, value) pairs."""
        settings = {"Name": name}
        if graph is not None:
            settings["Graph"] = graph
        else:
            settings["Parameters"] = [{"Name": key, "Type": value_type} for key, (value_type, _) in arguments.items()]
            arguments = {key: value for key, (_, value) in arguments.items()}
        return self.add("Event.Send", settings, comment=comment, Target=target, **arguments)

    def timeline(self, setting, comment=None):
        return self.add("Flow.Timeline", {"Timeline": setting}, comment=comment)

    def operations(self):
        framed = list(dict.fromkeys(self.framed))
        return (self.additions + self.connections + self.after + [
            {"op": "Arrange", "scope": "Created"},
            {"op": "Arrange", "scope": "Nodes", "nodes": framed},
            {"op": "AddComment", "text": self.title, "elements": framed, "color": self.color},
        ])


def public(name, type_id, value, tooltip, value_range=None):
    return (name, type_id, value, True, tooltip, value_range)


def private(name, type_id, value=None):
    return (name, type_id, value, False, None, None)


class Author:
    """Writes graphs: their declarations, then their regions, checking every batch."""

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

    def declare(self, graph, description, variables=(), events=(), functions=()):
        """The graph's interface: variables (name, type, value, public, tooltip, range), custom events (name, [(parameter,
        type)]) and functions (name, pure, inputs, outputs, locals)."""
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
        labels = self.edit(graph, "Declare the interface", operations)["labels"]
        self.declared[graph] = {
            "events": {name: (labels[f"$event{index}"], [{"Name": p, "Type": t} for p, t in parameters])
                       for index, (name, parameters) in enumerate(events)},
            "functions": {name: (labels[f"$entry{index}"], labels[f"$return{index}"]) for index, (name, *_) in enumerate(functions)},
        }

    def region(self, graph, title, color, function=None):
        return Region(self, graph, title, color, function)

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


def prepare(session, project, graph, library=False):
    """A graph as an empty document: created when missing, emptied (graph_set) when it exists, so its asset handle stays
    and the batches that follow write it exactly as from scratch. Returns its handle."""
    path = graph[len("asset://"):]
    if os.path.exists(os.path.join(project, "Assets", *path.split("/"))):
        document = session.call("graph_get", {"graph": graph, "analyze": False})["document"]
        empty = {"Description": "", "Variables": [], "Functions": [], "Graph": {"Nodes": [], "Links": [], "Comments": []}}
        if "Kind" in document:
            empty = {"Kind": document["Kind"], **empty}
        return session.call("graph_set", {"graph": graph, "document": empty})["asset"]
    return session.call("graph_create", {"path": path, "template": "Library" if library else "Empty"})["asset"]


def _contains(box, node):
    """Whether a node's position lies inside a comment box."""
    x, y = node["Position"]
    (left, top), (width, height) = box["Position"], box["Size"]
    return left <= x <= left + width and top <= y <= top + height


def verify(session, graphs):
    """Every graph analyzes without a diagnostic and every node resolves exactly; every node stands in exactly one comment
    box, and every box stands arranged."""
    for graph in graphs:
        document = session.call("graph_get", {"graph": graph})["document"]
        checked = session.call("graph_check", {"graph": graph, "document": document})
        if not checked["valid"] or not checked["analyzed"] or checked["diagnostics"]:
            raise SystemExit(f"{graph} does not check clean: {checked['issues']} {checked.get('diagnostics')}")
        inexact = [node["ID"] for node in checked["nodes"] if node["Resolved"] != "Exact"]
        if inexact:
            raise SystemExit(f"{graph} has nodes that do not resolve exactly: {inexact}")
        sub_graphs = [(0, document["Graph"])] + [(function["ID"], function["Graph"]) for function in document["Functions"]]
        for function, content in sub_graphs:
            boxes = content["Comments"]
            for node in content["Nodes"]:
                if sum(_contains(box, node) for box in boxes) != 1:
                    raise SystemExit(f"node {node['ID']} of {graph} (sub-graph {function}) is not inside exactly one comment box")
            for box in boxes:
                inside = [node["ID"] for node in content["Nodes"] if _contains(box, node)]
                arranged = session.call("graph_arrange", {"graph": graph, "function": function, "nodes": inside, "dryRun": True})
                if arranged["changed"]:
                    raise SystemExit(f"the box '{box['Text']}' of {graph} does not stand arranged")

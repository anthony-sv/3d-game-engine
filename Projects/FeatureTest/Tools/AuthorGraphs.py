"""Writes the FeatureTest project's visual scripts through `strada mcp`, the way an agent does, then runs the project's tests:

- Scripts/Features.sgraph checks every built-in node kind but the contact and click events (on 'Visual Script Tests'),
- Scripts/Contacts.sgraph counts the contacts of the falling box and ball,
- Scripts/Clicks.sgraph counts the clicks that reach the UI canvas.

    python Projects/FeatureTest/Tools/AuthorGraphs.py [strada executable]

Each graph is checked with graph_check (valid, analyzed, no Error diagnostic, every node resolved exactly) and written with
graph_set, which keeps the file canonical and places the nodes. The scene's VisualScript components already reference the
graphs, so only the three graph files change. A new built-in node kind gets a check in build_features, and FeaturesChecks
in Scripts/Source/VisualScriptTests.cs must equal the number of checks Features reports (this tool stops when it does not).
Without an executable, strada runs from the repository (dotnet run).
"""
import json
import os
import re
import subprocess
import sys

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
            raise SystemExit(f"{tool} {json.dumps(arguments)} failed: {text}")
        return json.loads(text)

    def close(self):
        self.process.stdin.close()
        return self.process.wait(timeout=120)


# --- Documents (the content form of .sgraph files) ------------------------------------------------------------------------
# Nodes are made in the order they are needed and numbered in that order; data sources are (node, output pin) pairs.

class Document:
    def __init__(self, description):
        self.description = description
        self.variables = []
        self.functions = []
        self.next_id = 1
        self.graph = Graph(self)

    def new_id(self):
        value = self.next_id
        self.next_id += 1
        return value

    def variable(self, name, type_id, value=None, public=False, tooltip=None):
        variable = {"ID": self.new_id(), "Name": name, "Type": type_id}
        if value is not None:
            variable["Value"] = value
        if public:
            variable["Public"] = True
        if tooltip:
            variable["Tooltip"] = tooltip
        self.variables.append(variable)
        return variable["ID"]

    def function(self, name, pure=False, inputs=(), outputs=(), locals_=()):
        function = {"ID": self.new_id(), "Name": name, "Pure": pure,
                    "Inputs": [{"Name": n, "Type": t} for n, t in inputs],
                    "Outputs": [{"Name": n, "Type": t} for n, t in outputs],
                    "Locals": []}
        for local_name, local_type, local_value in locals_:
            local = {"ID": self.new_id(), "Name": local_name, "Type": local_type}
            if local_value is not None:
                local["Value"] = local_value
            function["Locals"].append(local)
        graph = Graph(self)
        self.functions.append((function, graph))
        return function, graph

    def content(self):
        functions = []
        for function, graph in self.functions:
            entry = dict(function)
            entry["Graph"] = graph.content()
            functions.append(entry)
        return {"Description": self.description, "Variables": self.variables, "Functions": functions,
                "Graph": self.graph.content()}


class Graph:
    def __init__(self, document):
        self.document = document
        self.nodes = []
        self.links = []

    def node(self, type_id, settings=None, inputs=None, **sources):
        """A node whose inputs named in sources are linked from (node, pin) pairs."""
        node = {"ID": self.document.new_id(), "Type": type_id}
        if settings:
            node["Settings"] = settings
        if inputs:
            node["Inputs"] = inputs
        self.nodes.append(node)
        for pin, source in sources.items():
            self.link(source, (node["ID"], pin))
        return node["ID"]

    def link(self, source, target):
        self.links.append({"From": [source[0], source[1]], "To": [target[0], target[1]]})

    def flow(self, source, target, pin="Execute"):
        """Links an exec output (node, pin) or a node's Then to a node's exec input."""
        if not isinstance(source, tuple):
            source = (source, "Then")
        self.link(source, (target, pin))

    def chain(self, first, *nodes):
        """first (a node, whose Then continues, or a (node, pin) exec output) → each node's Execute → ...; returns the last."""
        previous = first
        for node in nodes:
            self.flow(previous, node)
            previous = node
        return previous

    def content(self):
        return {"Nodes": self.nodes, "Links": self.links, "Comments": []}

    # Shorthands.
    def get(self, variable):
        return (self.node("Variable.Get", {"Variable": variable}), "Value")

    def set(self, variable, source=None, literal=None):
        sources = {"Value": source} if source is not None else {}
        return self.node("Variable.Set", {"Variable": variable}, {"Value": literal} if literal is not None else None, **sources)

    def op(self, kind, a=None, b=None, **literals):
        sources = {}
        if a is not None:
            sources["A"] = a
        if b is not None:
            sources["B"] = b
        return (self.node("Operator." + kind, None, literals or None, **sources), "Result")

    def literal(self, type_id, value):
        return (self.node("Literal", {"Type": type_id, "Value": value}), "Value")

    def format(self, text, inputs=None, **placeholders):
        return (self.node("FormatString", {"Format": text}, inputs, **placeholders), "Result")

    def all_of(self, *conditions):
        current = conditions[0]
        for condition in conditions[1:]:
            current = self.op("And", current, condition)
        return current

    def equal(self, source, value):
        return self.op("Equal", source, B=value)

    def increment(self, variable, amount=1):
        """Variable.Set <variable> = <variable> + amount."""
        return self.set(variable, self.op("Add", self.get(variable), B=amount))


# --- Features.sgraph: every built-in node kind but the contact and click events --------------------------------------------

FEATURES_DESCRIPTION = (
    "The feature test's visual script. Event.Create runs a check of every built-in node kind but the contact and click "
    "events (Contacts.sgraph and Clicks.sgraph have those), each reported through TestReporter by the function Check: "
    "Checks counts the passed ones and Done is set after the last. The waiting kinds come last, since each suspends the "
    "checks after it. The input events count into InputEvents (test runs have no input). The custom event Add adds its "
    "Amount to Score: the graph raises it with Event.Send, VisualScriptTests from C#. The custom event Retrigger enters a "
    "RetriggerableDelay, which the graph raises twice.")


def build_features():
    doc = Document(FEATURES_DESCRIPTION)
    checks = doc.variable("Checks", "int", 0, True, "Checks that passed")
    done = doc.variable("Done", "bool", False, True, "Whether every check has run")
    score = doc.variable("Score", "int", 0, True, "What the custom event Add added")
    frames = doc.variable("Frames", "int", 0)
    elapsed = doc.variable("Elapsed", "float", 0.0)
    fixed_steps = doc.variable("FixedSteps", "int", 0)
    fixed_step = doc.variable("FixedStep", "float", 0.0)
    input_events = doc.variable("InputEvents", "int", 0)
    count = doc.variable("Count", "int", 0)
    text = doc.variable("Text", "string", "")
    start = doc.variable("Start", "float", 0.0)
    start_frame = doc.variable("StartFrame", "int", 0)
    ticks = doc.variable("Ticks", "int", 0)
    last_index = doc.variable("LastIndex", "int", 0)
    alpha = doc.variable("Alpha", "float", 0.0)
    first_alpha = doc.variable("FirstAlpha", "float", 0.0)
    first_t = doc.variable("FirstT", "float", 0.0)
    retrigger_entries = doc.variable("RetriggerEntries", "int", 0)
    retrigger_runs = doc.variable("RetriggerRuns", "int", 0)
    retrigger_start = doc.variable("RetriggerStart", "float", 0.0)
    retrigger_waited = doc.variable("RetriggerWaited", "float", 0.0)
    nobody = doc.variable("Nobody", "Strada.Entity")

    # Check(Name, Passed, Message): reports "Features: <Name>" as passed, and counts it, or as failed with the message.
    check, fg = doc.function("Check", False, [("Name", "string"), ("Passed", "bool"), ("Message", "string")])
    entry = fg.node("Function.Entry")
    branch = fg.node("Flow.Branch", Condition=(entry, "Passed"))
    fg.flow(entry, branch)
    name = fg.format("Features: {Name}", Name=(entry, "Name"))
    reported = fg.node("Call:Strada.Testing.TestReporter.Pass(string)", Name=name)
    fg.chain((branch, "True"), reported, fg.increment(checks), fg.node("Function.Return"))
    failed = fg.node("Call:Strada.Testing.TestReporter.Fail(string,string)", Name=name, Message=(entry, "Message"))
    fg.flow((branch, "False"), failed)

    # Average(A, B) -> Mean: a pure function.
    average, ag = doc.function("Average", True, [("A", "float"), ("B", "float")], [("Mean", "float")])
    entry = ag.node("Function.Entry")
    total = ag.op("Add", (entry, "A"), (entry, "B"))
    ag.node("Function.Return", Mean=ag.op("Multiply", total, B=0.5))

    # Triangle(N) -> Total: 1 + 2 + ... + N, summed in a local.
    triangle, tg = doc.function("Triangle", False, [("N", "int")], [("Total", "int")], [("Sum", "int", 0)])
    sum_local = triangle["Locals"][0]["ID"]
    entry = tg.node("Function.Entry")
    loop = tg.node("Flow.ForLoop", None, {"First": 1}, Last=(entry, "N"))
    tg.flow(entry, loop)
    tg.flow((loop, "Body"), tg.set(sum_local, tg.op("Add", tg.get(sum_local), (loop, "Index"))))
    tg.flow((loop, "Completed"), tg.node("Function.Return", Total=tg.get(sum_local)))

    # Pause(Seconds): a latent function (its Delay suspends the caller until it returns).
    pause, pg = doc.function("Pause", False, [("Seconds", "float")])
    entry = pg.node("Function.Entry")
    delay = pg.node("Flow.Delay", Duration=(entry, "Seconds"))
    pg.chain(entry, delay)
    pg.flow((delay, "Completed"), pg.node("Call:Strada.Log.Trace(string)", None, {"Message": "Features: paused"}))

    g = doc.graph

    def call_check(title, passed, message=None, message_text=None):
        inputs = {"Name": title}
        if message_text is not None:
            inputs["Message"] = message_text
        sources = {"Passed": passed}
        if message is not None:
            sources["Message"] = message
        return g.node("Function.Call", {"Function": check["ID"]}, inputs, **sources)

    def report(title, passed, message=None):
        """A call of Check with literal inputs."""
        inputs = {"Name": title, "Passed": passed}
        if message is not None:
            inputs["Message"] = message
        return g.node("Function.Call", {"Function": check["ID"]}, inputs)

    # The events the scene sends every frame count what happened, for the checks.
    update = g.node("Event.Update")
    g.chain(update, g.increment(frames), g.set(elapsed, g.op("Add", g.get(elapsed), (update, "DeltaTime"))))
    fixed = g.node("Event.FixedUpdate")
    g.chain(fixed, g.increment(fixed_steps), g.set(fixed_step, (fixed, "FixedDeltaTime")))
    counted = g.increment(input_events)
    for kind, settings, inputs in [("Event.KeyPressed", {"Key": "Space"}, None), ("Event.KeyReleased", {"Key": "Space"}, None),
                                   ("Event.MouseButtonPressed", {"Button": "Left"}, None),
                                   ("Event.MouseButtonReleased", {"Button": "Left"}, None),
                                   ("Event.GamepadButtonPressed", {"Button": "A"}, {"Gamepad": 0}),
                                   ("Event.GamepadButtonReleased", {"Button": "A"}, {"Gamepad": 0})]:
        g.flow(g.node(kind, settings, inputs), counted)

    # Add(Amount): Score += Amount.
    added = g.node("Event.Custom", {"Name": "Add", "Parameters": [{"Name": "Amount", "Type": "int"}]})
    g.flow(added, g.set(score, g.op("Add", g.get(score), (added, "Amount"))))

    # Retrigger: counts its entries and enters a RetriggerableDelay, whose continuation counts its runs and how long it
    # waited after the latest entry.
    retrigger = g.node("Event.Custom", {"Name": "Retrigger"})
    retriggered = g.node("Flow.RetriggerableDelay", None, {"Duration": 0.2})
    g.chain(retrigger, g.increment(retrigger_entries), g.set(retrigger_start, g.get(elapsed)), retriggered)
    g.chain((retriggered, "Completed"), g.increment(retrigger_runs),
            g.set(retrigger_waited, g.op("Subtract", g.get(elapsed), g.get(retrigger_start))))

    # The checks, in the order Event.Create's Sequence runs them: (node, exec input) of each.
    groups = []

    # Event.Create comes before the first update.
    frames_now = g.get(frames)
    groups.append(call_check("Create runs before the first update", g.equal(frames_now, 0),
                             g.format("{Frames} updates ran first", Frames=frames_now)))

    # Integer arithmetic.
    a = g.literal("int", 17)
    b = g.literal("int", 5)
    results = {"Sum": g.op("Add", a, b), "Difference": g.op("Subtract", a, b), "Product": g.op("Multiply", a, b),
               "Quotient": g.op("Divide", a, b), "Remainder": g.op("Modulo", a, b), "Negated": g.op("Negate", a)}
    expected = {"Sum": 22, "Difference": 12, "Product": 85, "Quotient": 3, "Remainder": 2, "Negated": -17}
    passed = g.all_of(*[g.equal(results[key], expected[key]) for key in results])
    groups.append(call_check("operators compute with integers", passed,
                             g.format("17 and 5: {Sum} {Difference} {Product} {Quotient} {Remainder} {Negated}", **results)))

    # Comparisons, logic, and a division of a float by an int.
    quotient = g.op("Divide", g.literal("float", 7.0), b)
    passed = g.all_of(g.op("Less", b, a), g.op("LessOrEqual", a, a), g.op("Greater", a, b), g.op("GreaterOrEqual", b, b),
                      g.op("NotEqual", a, b), g.op("Not", A=False), g.op("Or", A=False, B=True), g.op("Xor", A=True, B=False),
                      g.equal(quotient, 1.4))
    groups.append(call_check("comparisons and logic hold, and 7.0 / 5 is 1.4", passed,
                             g.format("7.0 / 5 is {Quotient}", Quotient=quotient)))

    # The same operators where they give false, each tested by a Branch of its own: a kind that always gives true fails the
    # check whatever the others do (combining them with And would hide an And that always gives true).
    title = "comparisons and logic are false where they should be"
    falsehoods = [("17 = 5", g.op("Equal", a, b)), ("5 != 5", g.op("NotEqual", b, b)), ("17 < 5", g.op("Less", a, b)),
                  ("17 <= 5", g.op("LessOrEqual", a, b)), ("5 > 17", g.op("Greater", b, a)),
                  ("5 >= 17", g.op("GreaterOrEqual", b, a)), ("true and false", g.op("And", A=True, B=False)),
                  ("false or false", g.op("Or", A=False, B=False)), ("true xor true", g.op("Xor", A=True, B=True)),
                  ("not true", g.op("Not", A=True))]
    previous = None
    for expression, condition in falsehoods:
        tested = g.node("Flow.Branch", Condition=condition)
        if previous is None:
            groups.append(tested)
        else:
            g.flow((previous, "False"), tested)
        g.flow((tested, "True"), report(title, False, f"{expression} is true"))
        previous = tested
    g.flow((previous, "False"), report(title, True))

    # Literal, Make and Break.
    parts = g.node("Break", {"Type": "Strada.Vector3"}, Value=g.literal("Strada.Vector3", [1.0, 2.0, 2.0]))
    swapped = (g.node("Make", {"Type": "Strada.Vector3"}, X=(parts, "Z"), Y=(parts, "Y"), Z=(parts, "X")), "Value")
    length = (g.node("Get:Strada.Vector3.Length", Target=swapped), "Value")
    swapped_parts = g.node("Break", {"Type": "Strada.Vector3"}, Value=swapped)
    passed = g.all_of(g.equal(length, 3.0), g.equal((swapped_parts, "X"), 2.0), g.equal((swapped_parts, "Z"), 1.0))
    groups.append(call_check("Make and Break build and take apart vectors", passed,
                             g.format("(2, 2, 1) has the length {Length}", Length=length)))

    # Select both ways, and Reroute of a value and of the flow.
    def select(condition):
        return (g.node("Select", Condition=condition,
                       **{"True": g.literal("string", "less"), "False": g.literal("string", "not less")}), "Result")

    not_less = (g.node("Reroute", In=select(g.op("Less", a, b))), "Out")
    less = select(g.op("Less", b, a))
    rerouted_flow = g.node("Reroute")
    g.flow((rerouted_flow, "Out"), call_check("Select picks by its condition, and Reroute passes values and the flow on",
                                              g.all_of(g.equal(not_less, "not less"), g.equal(less, "less")),
                                              g.format("17 < 5: {First}, 5 < 17: {Second}", First=not_less, Second=less)))
    groups.append((rerouted_flow, "In"))

    # Convert.
    results = {"Float": (g.node("Convert", {"To": "float"}, Value=g.literal("int", 7)), "Result"),
               "Parsed": (g.node("Convert", {"To": "int"}, Value=g.literal("string", "42")), "Result"),
               "Truncated": (g.node("Convert", {"To": "int"}, Value=g.literal("float", -2.75)), "Result"),
               "Mode": (g.node("Convert", {"To": "int"}, Value=g.literal("Strada.ForceMode", "VelocityChange")), "Result")}
    expected = {"Float": 7.0, "Parsed": 42, "Truncated": -2, "Mode": 3}
    passed = g.all_of(*[g.equal(results[key], expected[key]) for key in results])
    groups.append(call_check("Convert changes numbers, text and enums", passed,
                             g.format("{Float} {Parsed} {Truncated} {Mode}", **results)))

    # FormatString.
    formatted = g.format("{Count} x {Word} = {{done}}", {"Word": "three"}, Count=g.literal("int", 3))
    groups.append(call_check("FormatString fills in its placeholders", g.equal(formatted, "3 x three = {done}"), formatted))

    # Self, IsValid and an entity lookup.
    me = (g.node("Self"), "Self")
    found = (g.node("Call:Strada.Entity.FindByID(ulong)", Id=(g.node("Get:Strada.Entity.ID", Target=me), "Value")), "Result")
    passed = g.all_of((g.node("IsValid", Value=me), "Result"),
                      g.op("Not", (g.node("IsValid", Value=g.get(nobody)), "Result")), g.op("Equal", found, me))
    groups.append(call_check("Self is the entity, which is valid, unlike an unset entity", passed,
                             g.format("Self is {Name}", Name=(g.node("Get:Strada.Entity.Name", Target=me), "Value"))))

    # Event.Send runs the custom event Add on Self at once.
    sent = g.node("Event.Send", {"Name": "Add", "Parameters": [{"Name": "Amount", "Type": "int"}]}, {"Amount": 5})
    score_now = g.get(score)
    g.flow(sent, call_check("Event.Send runs a custom event", g.equal(score_now, 5), g.format("Score is {Score}", Score=score_now)))
    groups.append(sent)

    # Function calls: a pure one, and an impure one with a local.
    mean = (g.node("Function.Call", {"Function": average["ID"]}, {"A": 2.0, "B": 7.0}), "Mean")
    groups.append(call_check("a pure function returns its result", g.equal(mean, 4.5),
                             g.format("the average of 2 and 7 is {Mean}", Mean=mean)))
    summed = g.node("Function.Call", {"Function": triangle["ID"]}, {"N": 4})
    g.flow(summed, call_check("an impure function sums in its local", g.equal((summed, "Total"), 10),
                              g.format("1 + 2 + 3 + 4 is {Total}", Total=(summed, "Total"))))
    groups.append(summed)

    # Switch on an int, a string and an enum.
    on_int = g.node("Flow.Switch", {"Type": "int", "Cases": [1, 2, 3]}, {"Selection": 2})
    on_text = g.node("Flow.Switch", {"Type": "string", "Cases": ["open", "close"]}, {"Selection": "close"})
    on_mode = g.node("Flow.Switch", {"Type": "Strada.ForceMode", "Cases": ["Force", "Impulse"]}, {"Selection": "Impulse"})
    matched = g.set(text, literal="matched")
    missed = g.set(text, literal="missed")
    g.flow((on_int, "Case:2"), on_text)
    g.flow((on_text, "Case:close"), on_mode)
    g.flow((on_mode, "Case:Impulse"), matched)
    for node, pins in [(on_int, ["Case:1", "Case:3", "Default"]), (on_text, ["Case:open", "Default"]),
                       (on_mode, ["Case:Force", "Default"])]:
        for pin in pins:
            g.flow((node, pin), missed)
    text_now = g.get(text)
    switched = call_check("Switch takes the case of an int, a string and an enum", g.equal(text_now, "matched"), text_now)
    g.flow(matched, switched)
    g.flow(missed, switched)
    groups.append(on_int)

    # ForLoop with Break: 1 + 2 + 3 + 4, breaking at 4.
    reset = g.set(count, literal=0)
    loop = g.node("Flow.ForLoop", None, {"First": 1, "Last": 10})
    g.flow(reset, loop)
    summing = g.set(count, g.op("Add", g.get(count), (loop, "Index")))
    stop = g.node("Flow.Branch", Condition=g.equal((loop, "Index"), 4))
    g.chain((loop, "Body"), summing, stop)
    g.flow((stop, "True"), loop, "Break")
    count_now = g.get(count)
    g.flow((loop, "Completed"), call_check("ForLoop counts and Break ends it", g.equal(count_now, 10),
                                           g.format("the sum is {Sum}", Sum=count_now)))
    groups.append(reset)

    # While.
    reset = g.set(count, literal=0)
    loop = g.node("Flow.While", Condition=g.op("Less", g.get(count), B=5))
    g.flow(reset, loop)
    g.flow((loop, "Body"), g.increment(count))
    count_now = g.get(count)
    g.flow((loop, "Completed"), call_check("While repeats while its condition holds", g.equal(count_now, 5),
                                           g.format("it ran {Count} times", Count=count_now)))
    groups.append(reset)

    # ForEach with Break, Array.Length and Array.Get, over the children of the entity's parent.
    parent = (g.node("Get:Strada.Entity.Parent"), "Value")
    guard = g.node("Flow.Branch", Condition=(g.node("IsValid", Value=parent), "Result"))
    reset = g.set(count, literal=0)
    g.flow((guard, "True"), reset)
    siblings = (g.node("Get:Strada.Entity.Children", Target=parent), "Value")
    each = g.node("Flow.ForEach", Array=siblings)
    g.flow(reset, each)
    is_sibling = g.node("Flow.Branch",
                        Condition=g.op("Equal", (g.node("Get:Strada.Entity.Parent", Target=(each, "Element")), "Value"), parent))
    g.flow((each, "Body"), is_sibling)
    counted = g.increment(count)
    g.flow((is_sibling, "True"), counted)
    stop = g.node("Flow.Branch", Condition=g.equal((each, "Index"), 1))
    g.flow(counted, stop)
    g.flow((stop, "True"), each, "Break")
    length = (g.node("Array.Length", Array=siblings), "Length")
    first = (g.node("Array.Get", None, {"Index": 0}, Array=siblings), "Element")
    count_now = g.get(count)
    passed = g.all_of(g.equal(count_now, 2), g.op("GreaterOrEqual", length, B=2),
                      g.op("Equal", (g.node("Get:Strada.Entity.Parent", Target=first), "Value"), parent))
    title = "ForEach visits an array's elements and Break ends it"
    g.flow((each, "Completed"), call_check(title, passed, g.format("{Count} of {Length} children before the break",
                                                                  Count=count_now, Length=length)))
    g.flow((guard, "False"), report(title, False, "the entity has no parent whose children to visit"))
    groups.append(guard)

    # DoOnce: three tries, a Reset and one more run it twice.
    reset = g.set(count, literal=0)
    once = g.node("Flow.DoOnce")
    g.flow((once, "Completed"), g.increment(count))
    tries = g.node("Flow.ForLoop", None, {"First": 1, "Last": 3})
    g.flow((tries, "Body"), once)
    steps = g.node("Flow.Sequence", {"Count": 4})
    g.flow(reset, steps)
    g.flow((steps, "Then0"), tries)
    g.flow((steps, "Then1"), once, "Reset")
    g.flow((steps, "Then2"), once)
    count_now = g.get(count)
    g.flow((steps, "Then3"), call_check("DoOnce runs once until it is reset", g.equal(count_now, 2),
                                        g.format("it ran {Count} times", Count=count_now)))
    groups.append(reset)

    # DoN: four tries of a DoN of 2.
    reset = g.set(count, literal=0)
    twice = g.node("Flow.DoN", None, {"N": 2})
    g.flow((twice, "Exit"), g.increment(count))
    tries = g.node("Flow.ForLoop", None, {"First": 1, "Last": 4})
    g.flow(reset, tries)
    g.flow((tries, "Body"), twice)
    count_now = g.get(count)
    g.flow((tries, "Completed"), call_check("DoN runs N times", g.all_of(g.equal(count_now, 2), g.equal((twice, "Counter"), 2)),
                                            g.format("it ran {Count} times", Count=count_now)))
    groups.append(reset)

    # FlipFlop: A, B, A.
    reset = g.set(text, literal="")
    flip = g.node("Flow.FlipFlop")
    tries = g.node("Flow.ForLoop", None, {"First": 1, "Last": 3})
    g.flow(reset, tries)
    g.flow((tries, "Body"), flip)
    for pin in ["A", "B"]:
        g.flow((flip, pin), g.set(text, g.format("{Text}" + pin, Text=g.get(text))))
    text_now = g.get(text)
    g.flow((tries, "Completed"), call_check("FlipFlop alternates", g.all_of(g.equal(text_now, "ABA"), (flip, "IsA")), text_now))
    groups.append(reset)

    # Gate: entered closed, opened, toggled and closed.
    reset = g.set(count, literal=0)
    gate = g.node("Flow.Gate", {"StartOpen": False})
    g.flow((gate, "Exit"), g.increment(count))
    steps = g.node("Flow.Sequence", {"Count": 9})
    g.flow(reset, steps)
    for index, pin in enumerate(["Enter", "Open", "Enter", "Toggle", "Enter", "Toggle", "Enter", "Close"]):
        g.flow((steps, f"Then{index}"), gate, pin)
    count_now = g.get(count)
    g.flow((steps, "Then8"), call_check("Gate lets the flow through while it is open", g.equal(count_now, 2),
                                        g.format("it let {Count} through", Count=count_now)))
    groups.append(reset)

    # The waiting kinds: each suspends the whole execution of Event.Create, and the checks after it with it.
    def waited(seconds):
        seconds_waited = g.op("Subtract", g.get(elapsed), g.get(start))
        return g.op("GreaterOrEqual", seconds_waited, B=seconds), g.format("it waited {Seconds} s", Seconds=seconds_waited)

    marked = g.set(start, g.get(elapsed))
    delay = g.node("Flow.Delay", None, {"Duration": 0.1})
    g.flow(marked, delay)
    g.flow((delay, "Completed"), call_check("Delay waits its duration", *waited(0.099)))
    groups.append(marked)

    # Retrigger is raised, and again a frame later while its RetriggerableDelay waits: the countdown starts again, and only
    # the latest entry goes on (a plain Delay would go on twice, or after the first entry's duration).
    first_entry = g.node("Event.Send", {"Name": "Retrigger"})
    a_frame = g.node("Flow.WaitFrames", None, {"Frames": 1})
    second_entry = g.node("Event.Send", {"Name": "Retrigger"})
    went_on = g.node("Flow.WaitUntil", Condition=g.op("Greater", g.get(retrigger_runs), B=0))
    settled = g.node("Flow.WaitFrames", None, {"Frames": 3})
    g.flow(first_entry, a_frame)
    g.flow((a_frame, "Completed"), second_entry)
    g.flow(second_entry, went_on)
    g.flow((went_on, "Completed"), settled)
    entries_now = g.get(retrigger_entries)
    runs_now = g.get(retrigger_runs)
    waited_now = g.get(retrigger_waited)
    g.flow((settled, "Completed"),
           call_check("RetriggerableDelay starts again when entered again, and only the latest entry goes on",
                      g.all_of(g.equal(entries_now, 2), g.equal(runs_now, 1), g.op("GreaterOrEqual", waited_now, B=0.199)),
                      g.format("{Runs} of {Entries} entries went on, {Waited} s after the latest", Runs=runs_now,
                               Entries=entries_now, Waited=waited_now)))
    groups.append(first_entry)

    def frames_waited():
        counted_frames = g.op("Subtract", g.get(frames), g.get(start_frame))
        return counted_frames, g.format("it waited {Frames} frames", Frames=counted_frames)

    marked = g.set(start_frame, g.get(frames))
    waiting = g.node("Flow.WaitFrames", None, {"Frames": 3})
    g.flow(marked, waiting)
    counted_frames, message = frames_waited()
    g.flow((waiting, "Completed"), call_check("WaitFrames waits its frames", g.equal(counted_frames, 3), message))
    groups.append(marked)

    marked = g.set(start_frame, g.get(frames))
    waiting = g.node("Flow.WaitUntil", Condition=g.op("GreaterOrEqual", g.get(frames), g.op("Add", g.get(start_frame), B=2)))
    g.flow(marked, waiting)
    counted_frames, message = frames_waited()
    g.flow((waiting, "Completed"), call_check("WaitUntil waits for its condition", g.equal(counted_frames, 2), message))
    groups.append(marked)

    # Tween with EaseOut: its first tick's Alpha is EaseOut (1 - (1 - t)^2) of the part of its duration gone by then, which
    # Linear would not give, and its last is 1.
    reset = g.set(ticks, literal=0)
    marked = g.set(start, g.get(elapsed))
    tween = g.node("Flow.Tween", {"Easing": "EaseOut"}, {"Duration": 0.1})
    g.chain(reset, marked, tween)
    first_tick = g.node("Flow.Branch", Condition=g.equal(g.get(ticks), 0))
    g.flow((tween, "Update"), first_tick)
    ticked = g.set(alpha, (tween, "Alpha"))
    g.chain((first_tick, "True"), g.set(first_alpha, (tween, "Alpha")),
            g.set(first_t, g.op("Divide", g.op("Subtract", g.get(elapsed), g.get(start)), B=0.1)), ticked)
    g.flow((first_tick, "False"), ticked)
    g.flow(ticked, g.increment(ticks))
    alpha_now = g.get(alpha)
    ticks_now = g.get(ticks)
    first_t_now = g.get(first_t)
    first_alpha_now = g.get(first_alpha)
    remaining = g.op("Subtract", None, first_t_now, A=1.0)
    eased = g.op("Subtract", None, g.op("Multiply", remaining, remaining), A=1.0)
    error = (g.node("Call:Strada.Mathf.Abs(float)", Value=g.op("Subtract", first_alpha_now, eased)), "Result")
    passed = g.all_of(g.equal(alpha_now, 1.0), g.op("Greater", ticks_now, B=1), g.op("Less", first_t_now, B=1.0),
                      g.op("Less", error, B=0.0001))
    g.flow((tween, "Completed"), call_check("Tween eases its alpha until it reaches 1", passed,
                                            g.format("{Ticks} ticks, alpha {First} at {Part} of the way, then {Alpha}",
                                                     Ticks=ticks_now, First=first_alpha_now, Part=first_t_now, Alpha=alpha_now)))
    groups.append(reset)

    reset = g.set(ticks, literal=0)
    interval = g.node("Flow.Interval", None, {"Interval": 0.02, "Count": 3})
    g.flow(reset, interval, "Start")
    g.chain((interval, "Tick"), g.increment(ticks), g.set(last_index, (interval, "Index")))
    ticks_now = g.get(ticks)
    index_now = g.get(last_index)
    g.flow((interval, "Completed"), call_check("Interval ticks Count times",
                                               g.all_of(g.equal(ticks_now, 3), g.equal(index_now, 2)),
                                               g.format("{Ticks} ticks, the last {Index}", Ticks=ticks_now, Index=index_now)))
    groups.append(reset)

    marked = g.set(start, g.get(elapsed))
    paused = g.node("Function.Call", {"Function": pause["ID"]}, {"Seconds": 0.05})
    g.flow(marked, paused)
    g.flow(paused, call_check("a latent function suspends its caller", *waited(0.049)))
    groups.append(marked)

    # By now the scene has updated and stepped physics.
    frames_now = g.get(frames)
    steps_now = g.get(fixed_steps)
    passed = g.all_of(g.op("Greater", frames_now, B=0), g.op("Greater", g.get(elapsed), B=0.0), g.op("Greater", steps_now, B=0),
                      g.op("Equal", g.get(fixed_step), (g.node("Get:Strada.Time.FixedDeltaTime"), "Value")))
    groups.append(call_check("Update and FixedUpdate run with their time steps", passed,
                             g.format("{Frames} updates, {Steps} fixed steps", Frames=frames_now, Steps=steps_now)))

    finished = g.set(done, literal=True)
    g.flow(finished, g.node("Call:Strada.Log.Info(string)", Message=g.format("Features: {Checks} checks passed", Checks=g.get(checks))))
    groups.append(finished)

    create = g.node("Event.Create")
    sequence = g.node("Flow.Sequence", {"Count": len(groups)})
    g.flow(create, sequence)
    for index, group in enumerate(groups):
        node, pin = group if isinstance(group, tuple) else (group, "Execute")
        g.flow((sequence, f"Then{index}"), node, pin)

    # When the graph ends, every check has run.
    destroy = g.node("Event.Destroy")
    g.flow(destroy, g.node("Function.Call", {"Function": check["ID"]},
                           {"Name": "the checks ran before the graph ended", "Message": "the scene stopped first"},
                           Passed=g.get(done)))
    # Every group but the last (setting Done) is one check.
    return doc, len(groups) - 1


# --- Contacts.sgraph: the contact events, on the falling box and ball (which also run the C# CollisionProbe) ---------------

CONTACTS_DESCRIPTION = (
    "Counts its entity's contacts: collisions and triggers that begin and end. At the first contact that ends, it checks "
    "that every contact that ended had begun (Checks counts it) and sets Done. The feature test's falling box and ball run "
    "it next to their C# CollisionProbe.")


def build_contacts():
    doc = Document(CONTACTS_DESCRIPTION)
    collisions_entered = doc.variable("CollisionsEntered", "int", 0, True, "Collisions that began")
    collisions_exited = doc.variable("CollisionsExited", "int", 0, True, "Collisions that ended")
    triggers_entered = doc.variable("TriggersEntered", "int", 0, True, "Trigger overlaps that began")
    triggers_exited = doc.variable("TriggersExited", "int", 0, True, "Trigger overlaps that ended")
    checks = doc.variable("Checks", "int", 0, True, "Checks that passed")
    done = doc.variable("Done", "bool", False, True, "Whether a contact has ended")
    last_other = doc.variable("LastOther", "Strada.Entity")
    g = doc.graph

    for kind, counter in [("Event.CollisionEnter", collisions_entered), ("Event.TriggerEnter", triggers_entered)]:
        event = g.node(kind)
        g.chain(event, g.increment(counter), g.set(last_other, (event, "Other")))
    once = g.node("Flow.DoOnce")
    for kind, counter in [("Event.CollisionExit", collisions_exited), ("Event.TriggerExit", triggers_exited)]:
        event = g.node(kind)
        g.chain(event, g.increment(counter), once)

    began = g.op("Add", g.get(collisions_entered), g.get(triggers_entered))
    ended = g.op("Add", g.get(collisions_exited), g.get(triggers_exited))
    passed = g.all_of(g.op("GreaterOrEqual", began, ended), (g.node("IsValid", Value=g.get(last_other)), "Result"))
    branch = g.node("Flow.Branch", Condition=passed)
    g.flow((once, "Completed"), branch)
    me = (g.node("Self"), "Self")
    name = g.format("Contacts: {Name}: every contact that ended had begun",
                    Name=(g.node("Get:Strada.Entity.Name", Target=me), "Value"))
    reported = g.node("Call:Strada.Testing.TestReporter.Pass(string)", Name=name)
    g.chain((branch, "True"), reported, g.increment(checks), g.set(done, literal=True))
    failed = g.node("Call:Strada.Testing.TestReporter.Fail(string,string)", Name=name,
                    Message=g.format("{Began} began, {Ended} ended", Began=began, Ended=ended))
    g.flow((branch, "False"), failed)
    return doc, 1


# --- Clicks.sgraph: Event.Click, on the UI canvas (an ancestor of the button UITests clicks) -------------------------------

CLICKS_DESCRIPTION = (
    "Counts the clicks of the buttons of its entity's UI. At the first, it checks that the clicked entity is a button "
    "(Checks counts it) and sets Done. The feature test's UI canvas runs it next to the C# UITests.")


def build_clicks():
    doc = Document(CLICKS_DESCRIPTION)
    clicks = doc.variable("Clicks", "int", 0, True, "Buttons clicked")
    checks = doc.variable("Checks", "int", 0, True, "Checks that passed")
    done = doc.variable("Done", "bool", False, True, "Whether a button was clicked")
    g = doc.graph

    click = g.node("Event.Click")
    once = g.node("Flow.DoOnce")
    g.chain(click, g.increment(clicks), once)
    is_button = (g.node("Call:Strada.Entity.HasComponent<T>()", {"TypeArguments": ["Strada.UIButtonComponent"]},
                        Target=(click, "Button")), "Result")
    branch = g.node("Flow.Branch", Condition=is_button)
    g.flow((once, "Completed"), branch)
    button_name = (g.node("Get:Strada.Entity.Name", Target=(click, "Button")), "Value")
    name = g.format("Clicks: {Name}: the clicked entity is a button",
                    Name=(g.node("Get:Strada.Entity.Name", Target=(g.node("Self"), "Self")), "Value"))
    reported = g.node("Call:Strada.Testing.TestReporter.Pass(string)", Name=name)
    g.chain((branch, "True"), reported, g.increment(checks), g.set(done, literal=True))
    failed = g.node("Call:Strada.Testing.TestReporter.Fail(string,string)", Name=name,
                    Message=g.format("{Button} has no UIButton component", Button=button_name))
    g.flow((branch, "False"), failed)
    return doc, 1


GRAPHS = [("Scripts/Features.sgraph", build_features), ("Scripts/Contacts.sgraph", build_contacts),
          ("Scripts/Clicks.sgraph", build_clicks)]


def expected_features_checks():
    """FeaturesChecks of the C# companion, which asserts the Features graph's count."""
    with open(os.path.join(PROJECT, "Scripts", "Source", "VisualScriptTests.cs"), encoding="utf-8") as file:
        found = re.search(r"const int FeaturesChecks = (\d+);", file.read())
    if found is None:
        raise SystemExit("VisualScriptTests.cs declares no FeaturesChecks")
    return int(found.group(1))


def write_graphs(client):
    # Every graph is built and checked before the first is written: a tool that stops leaves the project as it was.
    checked_graphs = []
    for path, builder in GRAPHS:
        document, checks = builder()
        content = document.content()
        reference = "asset://" + path
        checked = client.call("graph_check", {"graph": reference, "document": content})
        errors = [diagnostic for diagnostic in checked.get("diagnostics", []) if diagnostic["Severity"] == "Error"]
        inexact = [node for node in checked.get("nodes", []) if node["Resolved"] != "Exact"]
        if not checked["valid"] or not checked["analyzed"] or errors or inexact:
            raise SystemExit(f"{path} does not check: {json.dumps({'issues': checked['issues'], 'errors': errors, 'inexact': inexact})}")
        if path.endswith("Features.sgraph") and checks != expected_features_checks():
            raise SystemExit(f"Features reports {checks} checks: set FeaturesChecks in Scripts/Source/VisualScriptTests.cs to it")
        checked_graphs.append((path, reference, content, checks))
    for path, reference, content, checks in checked_graphs:
        written = client.call("graph_set", {"graph": reference, "document": content})
        print(f"{path}: {checks} checks, {'written' if written['changed'] else 'unchanged'}")


def main():
    # strada runs in the repository's root: a path given relative to where this tool was started is made absolute first.
    command = [os.path.abspath(sys.argv[1])] if len(sys.argv) > 1 else \
        ["dotnet", "run", "--project", os.path.join(ROOT, "StradaTool"), "-c", "Release", "--no-launch-profile", "--"]
    client = McpClient(command + ["mcp", "--headless", "--new"])
    try:
        client.call("project_open", {"path": os.path.join(PROJECT, "FeatureTest.sproj")})
        built = client.call("script_build")
        if not built["succeeded"]:
            raise SystemExit(f"the scripts did not build: {built}")
        write_graphs(client)
        run = client.call("test_run", {"timeout": 60})
        print("test run:", {key: run[key] for key in ("finished", "timedOut", "frames", "passed", "failed", "exceptions")})
        for result in run["results"]:
            if not result["passed"] or result["name"].startswith(("Features", "Contacts", "Clicks", "VisualScriptTests")):
                print("  ", "PASS" if result["passed"] else "FAIL", result["name"], result.get("message", ""))
        if run["failed"] or run["exceptions"] or not run["finished"]:
            raise SystemExit("the tests failed")
    finally:
        client.close()


if __name__ == "__main__":
    main()

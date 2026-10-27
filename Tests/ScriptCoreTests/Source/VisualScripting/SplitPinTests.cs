using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Split struct pins (spec-slice2 section 3.3): a node's Split lists pins shown as one field pin per field, "<Pin>.<Field>",
// which links and literals reach instead of the pin. A split input is made from its field pins as Make makes a struct (an
// unlinked field pin without a literal takes that field of the value the pin takes unconnected), a split output gives its
// fields as Break does, nested entries split field pins, and wildcards split once resolved. Entries that split nothing are
// a Warning UnusedSplit; links and literals of a split pin itself, or of fields of a pin that is not split, reach nothing.
public sealed class SplitPinTests
{
	private const string Tint = "Call:Strada.ScriptCore.Tests.Probe.Tint(Strada.Color,Strada.Matrix4,Strada.Quaternion,Strada.ScriptCore.Tests.ProbeAccess)";
	private const string HitOf = "Call:Strada.ScriptCore.Tests.Probe.HitOf(Strada.Entity,float)";
	private const string Points = "Call:Strada.ScriptCore.Tests.Probe.Points(int)";

	[Fact]
	public void ASplitInputTakesLinksAndLiteralsOnItsFieldPins()
	{
		DocumentBuilder document = new();
		uint position = document.Variable("Position", "Strada.Vector3", "[9.0, 9.0, 9.0]");
		GraphBuilder graph = document.Graph;
		uint set = graph.Node("Variable.Set", Settings(position), """{ "Value.X": 1.0 }""", split: """{ "Inputs": ["Value"] }""");
		graph.Link(graph.Literal("float", "2.5"), "Value", set, "Value.Y");
		graph.Flow(graph.Node("Event.Create"), set);
		AnalyzedDocument analyzed = Analyze(document);

		Assert.Empty(analyzed.Diagnostics);
		// Only the input splits: Variable.Set's output of the same name stays.
		string[] expected = ["In Execute: exec", "In Value.X: float", "In Value.Y: float", "In Value.Z: float", "Out Then: exec", "Out Value: Strada.Vector3"];
		Assert.Equal(expected, ShownPins(analyzed, set));
		Assert.Equal(["Execute", "Value.X", "Value.Y", "Value.Z", "Then", "Value"], WrittenPins(analyzed, set));
		Assert.True(Shown(analyzed, set, "Value.Y", PinDirection.In).IsConnected);
		Assert.Equal(1.0f, Shown(analyzed, set, "Value.X", PinDirection.In).Value);
		// Value.Z takes the Z of what Value takes unconnected: the type default, zero.
		Assert.Equal(0.0f, Shown(analyzed, set, "Value.Z", PinDirection.In).Value);

		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(new Vector3(1.0f, 2.5f, 0.0f), runtime.Variable("Position"));
	}

	[Fact]
	public void ASplitOutputGivesItsFields()
	{
		DocumentBuilder document = new();
		uint position = document.Variable("Position", "Strada.Vector3", "[1.0, 2.0, 3.0]");
		GraphBuilder graph = document.Graph;
		uint get = graph.Node("Variable.Get", Settings(position), split: """{ "Outputs": ["Value"] }""");
		uint y = graph.Write(get, "Value.Y");
		uint z = graph.Write(get, "Value.Z");
		graph.Flow(graph.Node("Event.Create"), y);
		graph.Flow(y, z);
		AnalyzedDocument analyzed = Analyze(document);

		Assert.Empty(analyzed.Diagnostics);
		Assert.Equal(["Out Value.X: float", "Out Value.Y: float", "Out Value.Z: float"], ShownPins(analyzed, get));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["2", "3"], runtime.Records);
	}

	// A split output of an impure node gives the fields of the value the node stored when it ran: they are read each time
	// a node reads them, as a Break node would.
	[Fact]
	public void TheFieldsOfAnImpureNodesOutputAreReadWhenTheyAreUsed()
	{
		DocumentBuilder document = new();
		uint position = document.Variable("Position", "Strada.Vector3");
		GraphBuilder graph = document.Graph;
		uint first = graph.Set(position, "[1.0, 2.0, 3.0]");
		uint second = graph.Node("Variable.Set", Settings(position), """{ "Value": [4.0, 5.0, 6.0] }""", split: """{ "Outputs": ["Value"] }""");
		uint before = graph.Write(second, "Value.Y");
		uint after = graph.Write(second, "Value.Y");
		graph.Flow(graph.Node("Event.Create"), first);
		graph.Flow(first, before);
		graph.Flow(before, second);
		graph.Flow(second, after);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		// Before the Set ran its output holds the type default.
		Assert.Equal(["0", "5"], runtime.Records);
	}

	[Fact]
	public void NestedEntriesSplitFieldPinsOfInputsAndOutputs()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		// A Ray input split, and its Origin split again.
		uint ray = graph.Node("Break", """{ "Type": "Strada.Ray" }""", """{ "Value.Origin.Y": 4.0, "Value.Direction": [0.0, 0.0, 1.0] }""",
			split: """{ "Inputs": ["Value", "Value.Origin"] }""");
		graph.Link(graph.Literal("float", "3.0"), "Value", ray, "Value.Origin.X");
		// A RaycastHit output split, and its Point split again (HitOf is pure: its fields are evaluated with it).
		uint hit = graph.Node(HitOf, inputs: """{ "Distance": 2.5 }""", split: """{ "Outputs": ["Result", "Result.Point"] }""");
		uint origin = graph.Write(ray, "Origin");
		uint direction = graph.Write(ray, "Direction");
		uint pointY = graph.Write(hit, "Result.Point.Y");
		uint distance = graph.Write(hit, "Result.Distance");
		graph.Flow(graph.Node("Event.Create"), origin);
		graph.Flow(origin, direction);
		graph.Flow(direction, pointY);
		graph.Flow(pointY, distance);
		AnalyzedDocument analyzed = Analyze(document);

		Assert.Empty(analyzed.Diagnostics);
		Assert.Equal(["In Value.Origin.X: float", "In Value.Origin.Y: float", "In Value.Origin.Z: float", "In Value.Direction: Strada.Vector3",
			"Out Origin: Strada.Vector3", "Out Direction: Strada.Vector3"], ShownPins(analyzed, ray));
		Assert.Equal(["In Entity: Strada.Entity", "In Distance: float", "Out Result.Entity: Strada.Entity", "Out Result.Point.X: float",
			"Out Result.Point.Y: float", "Out Result.Point.Z: float", "Out Result.Normal: Strada.Vector3", "Out Result.Distance: float"],
			ShownPins(analyzed, hit));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["(3, 4, 0)", "(0, 0, 1)", "1", "2.5"], runtime.Records);
	}

	// An unlinked field pin without a literal takes that field of the value its pin takes unconnected: the pin's default
	// (Tint's structs are "= default", all zeros), else its type's (the identity of a quaternion). So a split pin whose fields
	// are not set passes what the pin would.
	[Fact]
	public void UnlinkedFieldPinsTakeTheFieldsOfTheirPinsUnconnectedValue()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint tint = graph.Node(Tint, inputs: """{ "Color.R": 0.5, "Transform.Column3.X": 5.0, "Rotation.Y": 0.5 }""",
			split: """{ "Inputs": ["Color", "Transform", "Transform.Column3", "Rotation"] }""");
		graph.Flow(graph.Node("Event.Create"), tint);
		AnalyzedDocument analyzed = Analyze(document);

		Assert.Empty(analyzed.Diagnostics);
		Assert.Equal(0.0f, Shown(analyzed, tint, "Color.A", PinDirection.In).Value);
		Assert.Equal(1.0f, Shown(analyzed, tint, "Rotation.W", PinDirection.In).Value);
		Assert.Equal(0.0f, Shown(analyzed, tint, "Transform.Column3.W", PinDirection.In).Value);
		// The analysis JSON gives field pins their defaults, so the editor shows what they take.
		JsonElement rotationW = WrittenPin(analyzed, tint, "Rotation.W");
		Assert.Equal(1.0, rotationW.GetProperty("Default").GetDouble());

		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		(Color color, Matrix4 transform, Quaternion rotation, ProbeAccess access) = Probe.Tinted!.Value;
		Assert.Equal(new Color(0.5f, 0.0f, 0.0f, 0.0f), color);
		Matrix4 expected = default;
		expected.Column3.X = 5.0f;
		Assert.Equal(expected, transform);
		Assert.Equal(new Quaternion(0.0f, 0.5f, 0.0f, 1.0f), rotation);
		Assert.Equal(ProbeAccess.Read, access);
	}

	// Wildcards split once their types are resolved, and links that name their field pins wait for it: a Reroute of a
	// Vector3, a ForEach's Element, and a field pin feeding the wildcard of an operator, which takes its type from it.
	[Fact]
	public void ResolvedWildcardsSplitAndTheirFieldPinsTypeWhatTheyFeed()
	{
		DocumentBuilder document = new();
		uint position = document.Variable("Position", "Strada.Vector3", "[1.0, 2.0, 3.0]");
		GraphBuilder graph = document.Graph;
		uint reroute = graph.Node("Reroute", split: """{ "Outputs": ["Out"] }""");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 0.5 }""");
		uint each = graph.Node("Flow.ForEach", split: """{ "Outputs": ["Element"] }""");
		graph.Link(reroute, "Out.Y", add, "A");
		graph.Link(graph.Get(position), "Value", reroute, "In");
		graph.Link(graph.Node(Points, inputs: """{ "Count": 3 }"""), "Result", each, "Array");
		uint sum = graph.Write(add, "Result");
		uint element = graph.Write(each, "Element.Z");
		graph.Flow(graph.Node("Event.Create"), sum);
		graph.Flow(sum, each);
		graph.Flow(each, element, "Body");
		AnalyzedDocument analyzed = Analyze(document);

		Assert.Empty(analyzed.Diagnostics);
		Assert.Equal(["In In: Strada.Vector3", "Out Out.X: float", "Out Out.Y: float", "Out Out.Z: float"], ShownPins(analyzed, reroute));
		Assert.Equal("float", Shown(analyzed, add, "A", PinDirection.In).Type.Id);
		Assert.Equal("float", Shown(analyzed, add, "Result", PinDirection.Out).Type.Id);
		Assert.Contains("Out Element.Z: float", ShownPins(analyzed, each));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["2.5", "0", "3", "6"], runtime.Records);
	}

	// A field pin of a node that splits late feeds a pin of its own of another node that splits late: the link attaches when
	// the first node splits, before the second resolves, so it types the second's wildcard, which splits in turn. A chain of
	// two split Reroutes, a split Select fed by the fields of a split Reroute, and a For Each's Element fields feeding a
	// Select whose Split lists its float Result. The nodes that resolve later come first, so node order alone would not do.
	[Fact]
	public void FieldPinsOfLateSplitsTypeTheWildcardsOfOtherLateSplits()
	{
		DocumentBuilder chain = new();
		GraphBuilder graph = chain.Graph;
		uint second = graph.Node("Reroute", split: """{ "Outputs": ["Out"] }""");
		uint first = graph.Node("Reroute", split: """{ "Outputs": ["Out"] }""");
		graph.Link(first, "Out.Point", second, "In");
		graph.Link(graph.Node(HitOf, inputs: """{ "Distance": 2.5 }"""), "Result", first, "In");
		uint pointY = graph.Write(second, "Out.Y");
		graph.Flow(graph.Node("Event.Create"), pointY);
		AnalyzedDocument analyzedChain = Analyze(chain);

		Assert.Empty(analyzedChain.Diagnostics);
		Assert.Equal(["In In: Strada.Vector3", "Out Out.X: float", "Out Out.Y: float", "Out Out.Z: float"], ShownPins(analyzedChain, second));
		TestRuntime chainRuntime = TestRuntime.Start(chain);
		chainRuntime.Invoke(ScriptEvent.Create);
		Assert.Equal(["1"], chainRuntime.Records);

		DocumentBuilder select = new();
		graph = select.Graph;
		uint choose = graph.Node("Select", inputs: """{ "Condition": true }""", split: """{ "Outputs": ["Result"] }""");
		uint ray = graph.Node("Reroute", split: """{ "Outputs": ["Out"] }""");
		graph.Link(ray, "Out.Origin", choose, "True");
		graph.Link(ray, "Out.Direction", choose, "False");
		graph.Link(graph.Node("Make", """{ "Type": "Strada.Ray" }""", """{ "Origin": [1.0, 2.0, 3.0], "Direction": [0.0, 0.0, 1.0] }"""), "Value", ray, "In");
		uint originY = graph.Write(choose, "Result.Y");
		graph.Flow(graph.Node("Event.Create"), originY);
		AnalyzedDocument analyzedSelect = Analyze(select);

		Assert.Empty(analyzedSelect.Diagnostics);
		Assert.Equal(["In Condition: bool", "In True: Strada.Vector3", "In False: Strada.Vector3", "Out Result.X: float", "Out Result.Y: float",
			"Out Result.Z: float"], ShownPins(analyzedSelect, choose));
		TestRuntime selectRuntime = TestRuntime.Start(select);
		selectRuntime.Invoke(ScriptEvent.Create);
		Assert.Equal(["2"], selectRuntime.Records);

		DocumentBuilder each = new();
		graph = each.Graph;
		uint pick = graph.Node("Select", inputs: """{ "Condition": true }""", split: """{ "Outputs": ["Result"] }""");
		uint loop = graph.Node("Flow.ForEach", split: """{ "Outputs": ["Element"] }""");
		graph.Link(loop, "Element.Y", pick, "True");
		graph.Link(loop, "Element.Z", pick, "False");
		graph.Link(graph.Node(Points, inputs: """{ "Count": 3 }"""), "Result", loop, "Array");
		uint body = graph.Write(pick, "Result");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, body, "Body");
		AnalyzedDocument analyzedEach = Analyze(each);

		Assert.Equal([(DiagnosticCode.UnusedSplit, pick, "Result")], Findings(analyzedEach));
		Assert.EndsWith("Result is float: outputs split only structs with public fields or readonly structs with get-only properties",
			analyzedEach.Diagnostics[0].Message, StringComparison.Ordinal);
		Assert.Equal("float", Shown(analyzedEach, pick, "True", PinDirection.In).Type.Id);
		TestRuntime eachRuntime = TestRuntime.Start(each);
		eachRuntime.Invoke(ScriptEvent.Create);
		Assert.Equal(["0", "2", "4"], eachRuntime.Records);
	}

	// A disabled node passes its split input on as the struct its fields make, and its split output gives the fields of what
	// it passes.
	[Fact]
	public void ADisabledNodePassesItsSplitPinsOn()
	{
		DocumentBuilder document = new();
		uint position = document.Variable("Position", "Strada.Vector3");
		GraphBuilder graph = document.Graph;
		uint set = graph.Node("Variable.Set", Settings(position), """{ "Value.X": 1.0, "Value.Y": 2.0, "Value.Z": 3.0 }""", "Disabled",
			"""{ "Inputs": ["Value"], "Outputs": ["Value"] }""");
		graph.Flow(graph.Node("Event.Create"), set);
		graph.Flow(set, graph.Write(set, "Value.Y"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["2"], runtime.Records);
		Assert.Equal(Vector3.Zero, runtime.Variable("Position"));
	}

	public static TheoryData<string, string, string, string?, string> UnusedSplits => new()
	{
		// No pin of its direction: none at all, or one of the other direction.
		{ "Flow.Branch", """{ "Inputs": ["Conditon"] }""", "Conditon", null, "Branch has no input pin \"Conditon\"" },
		{ "Variable.Get", """{ "Inputs": ["Value"] }""", "Value", """{ "Variable": 1 }""", "Get Position has no input pin \"Value\"" },
		// A type that cannot split: not a struct, exec, a struct an input cannot make, and a wildcard not resolved.
		{ "Flow.Branch", """{ "Inputs": ["Condition"] }""", "Condition", null, "Condition is bool: inputs split only structs whose public fields are all writable" },
		{ "Flow.Branch", """{ "Outputs": ["True"] }""", "True", null, "True is an exec pin" },
		{ "Break", """{ "Inputs": ["Value"] }""", "Value", """{ "Type": "Strada.RaycastHit" }""", "Value is Strada.RaycastHit: inputs split only structs whose public fields are all writable" },
		{ "Operator.Not", """{ "Inputs": ["A"] }""", "A", null, "A is bool: inputs split only structs whose public fields are all writable" },
		{ "Reroute", """{ "Outputs": ["Out"] }""", "Out", null, "the type of Out is not resolved" },
		// A field pin of an entry that did not split, and one that does not split itself.
		{ "Flow.Branch", """{ "Inputs": ["Condition", "Condition.X"] }""", "Condition.X", null, "Condition.X is a field of Condition, which is not split" },
		{ "Variable.Get", """{ "Outputs": ["Value", "Value.X"] }""", "Value.X", """{ "Variable": 1 }""", "Value.X is float: outputs split only structs with public fields or readonly structs with get-only properties" },
	};

	[Theory]
	[MemberData(nameof(UnusedSplits))]
	public void EntriesThatSplitNothingAreUnusedSplit(string type, string split, string path, string? settings, string why)
	{
		DocumentBuilder document = new();
		document.Variable("Position", "Strada.Vector3");
		uint node = document.Graph.Node(type, settings, split: split);
		AnalyzedDocument analyzed = Analyze(document);

		Diagnostic diagnostic = analyzed.Diagnostics.Single(diagnostic => diagnostic.Code == DiagnosticCode.UnusedSplit && diagnostic.Pin == path);
		Assert.Equal(DiagnosticSeverity.Warning, diagnostic.Severity);
		Assert.Equal(node, diagnostic.Node);
		Assert.Equal(path, diagnostic.Pin);
		Assert.Equal(split.Contains("Inputs", StringComparison.Ordinal) ? PinDirection.In : PinDirection.Out, diagnostic.Direction);
		Assert.EndsWith($"is ignored: {why}", diagnostic.Message, StringComparison.Ordinal);
		Assert.DoesNotContain(ShownPins(analyzed, node), pin => pin.Contains($" {path}.", StringComparison.Ordinal));
	}

	// Split is authoritative: a link to or from a split pin itself, or to a field of a pin that is not split, is UnknownPin,
	// and so is a literal of a field of a pin that is not split; a literal of a split pin itself is not used.
	[Fact]
	public void LinksAndLiteralsOfSplitPinsThemselvesAndOfFieldsOfUnsplitPinsReachNothing()
	{
		DocumentBuilder document = new();
		uint position = document.Variable("Position", "Strada.Vector3");
		GraphBuilder graph = document.Graph;
		uint split = graph.Node("Variable.Set", Settings(position), """{ "Value": [1.0, 2.0, 3.0] }""", split: """{ "Inputs": ["Value"], "Outputs": ["Value"] }""");
		uint whole = graph.Node("Variable.Set", Settings(position), """{ "Value.X": 1.0 }""");
		graph.Link(graph.Get(position), "Value", split, "Value");
		graph.Link(split, "Value", whole, "Value.Y");
		AnalyzedDocument analyzed = Analyze(document);

		Assert.Equal([(DiagnosticCode.UnknownPin, split, "Value"), (DiagnosticCode.UnknownPin, split, "Value"), (DiagnosticCode.UnknownPin, whole, "Value.Y"),
			(DiagnosticCode.UnusedInput, split, "Value"), (DiagnosticCode.UnknownPin, whole, "Value.X")], Findings(analyzed));
		Assert.Equal([PinDirection.In, PinDirection.Out, PinDirection.In, PinDirection.In, PinDirection.In],
			analyzed.Diagnostics.Select(static diagnostic => diagnostic.Direction!.Value));
		Assert.EndsWith("\"Value\": it is split, so links reach its fields", analyzed.Diagnostics[0].Message, StringComparison.Ordinal);
		Assert.EndsWith("\"Value.Y\": Value is not split", analyzed.Diagnostics[2].Message, StringComparison.Ordinal);
		Assert.Equal("Value is split: the stored value is not used (its fields take values of their own)", analyzed.Diagnostics[3].Message);
		// The Value split late (a link named it before it split) reports the same way.
		DocumentBuilder late = new();
		GraphBuilder lateGraph = late.Graph;
		uint reroute = lateGraph.Node("Reroute", split: """{ "Outputs": ["Out"] }""");
		lateGraph.Link(lateGraph.Literal("Strada.Vector3", "[1.0, 2.0, 3.0]"), "Value", reroute, "In");
		lateGraph.Link(reroute, "Out", lateGraph.Write("x"), "Text");
		lateGraph.Link(reroute, "Out.W", lateGraph.Write("y"), "Text");
		AnalyzedDocument lateAnalyzed = Analyze(late);
		Assert.Equal([(DiagnosticCode.UnknownPin, reroute, "Out"), (DiagnosticCode.UnknownPin, reroute, "Out.W")], Findings(lateAnalyzed));
		Assert.Equal("Reroute has no output pin \"Out\": it is split, so links reach its fields", lateAnalyzed.Diagnostics[0].Message);
		Assert.Equal(["Out.X", "Out.Y", "Out.Z"], lateAnalyzed.Diagnostics[0].Suggestions);
	}

	// UnusedSplit's fields in the analysis JSON (ruling R5.4): its Node; Direction In for an entry of Split.Inputs and Out for
	// one of Split.Outputs; Pin the entry exactly as the node's Split list stores it, also when it names no pin or a redirect
	// renamed the pin it names, and the message quotes it so. DisabledIgnored has its Node and neither Pin nor Direction.
	[Fact]
	public void UnusedSplitNamesTheEntryAsStoredInTheAnalysisJson()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint branch = graph.Node("Flow.Branch", split: """{ "Inputs": ["Conditon"], "Outputs": ["True"] }""");
		uint literal = graph.Node("Literal", """{ "Type": "float", "Value": 1.0 }""", enabled: "Disabled");
		JsonElement[] written = WrittenDiagnostics(Analyze(document));

		Assert.Equal(["UnusedSplit", "UnusedSplit", "DisabledIgnored"], written.Select(static diagnostic => diagnostic.GetProperty("Code").GetString()));
		AssertUnusedSplit(written[0], branch, "In", "Conditon", "Split.Inputs \"Conditon\" is ignored: Branch has no input pin \"Conditon\"");
		AssertUnusedSplit(written[1], branch, "Out", "True", "Split.Outputs \"True\" is ignored: True is an exec pin");
		Assert.Equal(literal, written[2].GetProperty("Node").GetUInt32());
		Assert.False(written[2].TryGetProperty("Pin", out _));
		Assert.False(written[2].TryGetProperty("Direction", out _));

		// Get Translation's Value became the stand-in's Result: the entries name Value as the file stores them, and the reasons
		// name the pins as the node now has them. "Result" names the split pin through the rename: it splits nothing again.
		AnalyzedDocument renamed = VisualScriptTesting.Analyze(FuzzLibrary.Library, VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Get:Strada.TransformComponent.Translation", "Split": { "Outputs": ["Value", "Value.X", "Value.Nope", "Result"] } }"""));
		JsonElement[] unused = [.. WrittenDiagnostics(renamed).Where(static diagnostic => diagnostic.GetProperty("Code").GetString() == "UnusedSplit")];
		Assert.Equal(3, unused.Length);
		AssertUnusedSplit(unused[0], 1, "Out", "Value.X",
			"Split.Outputs \"Value.X\" is ignored: Result.X is float: outputs split only structs with public fields or readonly structs with get-only properties");
		AssertUnusedSplit(unused[1], 1, "Out", "Value.Nope",
			$"Split.Outputs \"Value.Nope\" is ignored: {VisualScriptTesting.Node(renamed, 1).Title} has no output pin \"Result.Nope\"");
		AssertUnusedSplit(unused[2], 1, "Out", "Result", "Split.Outputs \"Result\" is ignored: Result is split by an earlier entry");
		Assert.Equal(["Out Result.X: float", "Out Result.Y: float", "Out Result.Z: float"],
			ShownPins(renamed, 1).Where(static pin => pin.StartsWith("Out ", StringComparison.Ordinal)));
	}

	private static void AssertUnusedSplit(JsonElement diagnostic, uint node, string direction, string pin, string message)
	{
		Assert.Equal("Warning", diagnostic.GetProperty("Severity").GetString());
		Assert.Equal(node, diagnostic.GetProperty("Node").GetUInt32());
		Assert.Equal(direction, diagnostic.GetProperty("Direction").GetString());
		Assert.Equal(pin, diagnostic.GetProperty("Pin").GetString());
		Assert.Equal(message, diagnostic.GetProperty("Message").GetString());
	}

	private static JsonElement[] WrittenDiagnostics(AnalyzedDocument document) =>
		[.. VisualScriptTesting.ParseObject(AnalysisWriter.Write(document)).GetProperty("Diagnostics").EnumerateArray()];

	// An exact pin name wins over a split reading: Switch's case "a.b" is the exec output Case:a.b, never a field of Case:a.
	[Fact]
	public void AnExactPinNameWinsOverASplitReading()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint choose = graph.Node("Flow.Switch", """{ "Type": "string", "Cases": ["a", "a.b"] }""", """{ "Selection": "a.b" }""",
			split: """{ "Outputs": ["Case:a"] }""");
		graph.Flow(graph.Node("Event.Create"), choose);
		graph.Writes(choose, "Case:a.b", "a.b");
		graph.Writes(choose, "Case:a", "a");
		AnalyzedDocument analyzed = Analyze(document);

		Assert.Equal([(DiagnosticCode.UnusedSplit, choose, "Case:a")], Findings(analyzed));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["a.b"], runtime.Records);
	}

	// Split paths follow the pin renames of a redirect (Get Translation's Value became the stand-in's Result), and an
	// overload is chosen by the pins its split paths are fields of.
	[Fact]
	public void SplitPathsFollowRenamedPinsAndChooseOverloads()
	{
		AnalyzedDocument renamed = VisualScriptTesting.Analyze(FuzzLibrary.Library, VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Get:Strada.TransformComponent.Translation", "Split": { "Outputs": ["Value"] } }, { "ID": 2, "Type": "Operator.Negate" }""",
			"""{ "From": [1, "Value.Y"], "To": [2, "A"] }"""));
		Assert.Equal([(DiagnosticCode.NodeUpdated, 1u, (string?)null)], Findings(renamed));
		Assert.Contains("Out Result.Y: float", ShownPins(renamed, 1));
		Assert.True(Shown(renamed, 2, "A", PinDirection.In).IsConnected);

		AnalyzedDocument chosen = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Call:Strada.Physics.Raycast", "Split": { "Inputs": ["Ray", "Ray.Origin"] } }, { "ID": 2, "Type": "Literal", "Settings": { "Type": "float" } }""",
			"""{ "From": [2, "Value"], "To": [1, "Ray.Origin.Y"] }"""));
		Assert.Equal([(DiagnosticCode.NodeUpdated, 1u, (string?)null)], Findings(chosen));
		Assert.Equal("Call:Strada.Physics.Raycast(Strada.Ray,float,out Strada.RaycastHit,uint)", VisualScriptTesting.Node(chosen, 1).CanonicalType);
		Assert.True(Shown(chosen, 1, "Ray.Origin.Y", PinDirection.In).IsConnected);
	}

	// Splitting or recombining a pin while the graph runs changes no node: the node keeps its state and stored outputs, which
	// its field pins then give, and a node enabled or disabled in the same edit is new or removed.
	[Fact]
	public void ALiveEditThatSplitsAPinKeepsTheNodesStateAndOutputs()
	{
		DocumentBuilder document = new();
		uint position = document.Variable("Position", "Strada.Vector3");
		GraphBuilder graph = document.Graph;
		uint set = graph.Set(position, "[1.0, 2.0, 3.0]");
		uint once = graph.Node("Flow.DoOnce");
		uint write = graph.Write(set, "Value");
		graph.Flow(graph.Node("Event.Create"), set);
		graph.Flow(graph.Node("Event.Update"), once);
		graph.Flow(once, write, "Completed");
		string built = document.Build();
		TestRuntime runtime = TestRuntime.Start(built);
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.1f);
		Assert.Equal(["(1, 2, 3)"], runtime.Records);

		// The Set's output splits and the Write reads its Y; the DoOnce is unchanged and stays closed.
		string split = built.Replace(FormattableString.Invariant($"\"ID\": {set}, \"Type\": \"Variable.Set\""),
			FormattableString.Invariant($"\"ID\": {set}, \"Type\": \"Variable.Set\", \"Split\": {{ \"Outputs\": [\"Value\"] }}"), StringComparison.Ordinal)
			.Replace(FormattableString.Invariant($"[{set}, \"Value\"]"), FormattableString.Invariant($"[{set}, \"Value.Y\"]"), StringComparison.Ordinal);
		runtime.LoadWorking(split);
		runtime.Invoke(ScriptEvent.Update, 0.1f);
		Assert.Equal(["(1, 2, 3)"], runtime.Records);
		// Opened again, the DoOnce lets one Write through, which reads the Set's stored output by its field.
		runtime.LoadWorking(split.Replace("\"Type\": \"Flow.DoOnce\"", "\"Type\": \"Flow.DoOnce\", \"Enabled\": \"Disabled\"", StringComparison.Ordinal));
		runtime.Invoke(ScriptEvent.Update, 0.1f);
		Assert.Equal(["(1, 2, 3)", "2"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	// A split pin costs the steps of the Make or Break it stands for, each time it is read, and allocates nothing.
	[Fact]
	public void SplitPinsOfVectorsAllocateNothing()
	{
		DocumentBuilder document = new();
		uint position = document.Variable("Position", "Strada.Vector3");
		GraphBuilder graph = document.Graph;
		uint get = graph.Node("Variable.Get", Settings(position), split: """{ "Outputs": ["Value"] }""");
		uint set = graph.Node("Variable.Set", Settings(position), """{ "Value.X": 0.5 }""", split: """{ "Inputs": ["Value"] }""");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1.0 }""");
		graph.Link(get, "Value.Y", add, "A");
		graph.Link(add, "Result", set, "Value.Y");
		graph.Link(get, "Value.Z", set, "Value.Z");
		graph.Flow(graph.Node("Event.Update"), set);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Update, 0.016f);

		Assert.Equal(0, runtime.FewestBytesPerUpdate());
		Assert.Equal(new Vector3(0.5f, 51.0f, 0.0f), runtime.Variable("Position"));
	}

	// The reader keeps what the loader keeps (spec-slice2 section 1.2, repair 15): every entry it would drop is an Error
	// InvalidDocument at its path, and the entry is skipped, with the whole subtree of a dropped parent; a Split or a list of
	// the wrong kind is one. Members of Split other than Inputs and Outputs are not checked.
	[Fact]
	public void TheReaderRefusesSplitEntriesTheLoaderWouldDrop()
	{
		string longPath = new('P', DocumentReader.MaxPinBytes + 1);
		string entries = string.Join(", ", ["1", "\"\"", "\"Value..X\"", "\".Value\"", "\"Value.\"", "\"\\u0001\"", $"\"{longPath}\"", "\"Value\"", "\"Value\"",
			"\"Value.Q.R\"", "\"Other.X\""]);
		AnalyzedDocument analyzed = VisualScriptTesting.Analyze(RuntimeTesting.Library, VisualScriptTesting.Document(
			$$"""{ "ID": 1, "Type": "Variable.Set", "Settings": { "Variable": 9 }, "Split": { "Inputs": [{{entries}}], "Extra": 1 } }, """ +
			"""{ "ID": 2, "Type": "Flow.Branch", "Split": 5 }, { "ID": 3, "Type": "Flow.Branch", "Split": { "Outputs": "True" } }""",
			variables: """{ "ID": 9, "Name": "Position", "Type": "Strada.Vector3" }"""));

		string[] expected =
		[
			"Graph.Nodes[0].Split.Inputs[0]: expected a pin path", "Graph.Nodes[0].Split.Inputs[1]: expected a pin path",
			"Graph.Nodes[0].Split.Inputs[2]: expected a pin path", "Graph.Nodes[0].Split.Inputs[3]: expected a pin path",
			"Graph.Nodes[0].Split.Inputs[4]: expected a pin path", "Graph.Nodes[0].Split.Inputs[5]: expected a pin path",
			"Graph.Nodes[0].Split.Inputs[6]: expected a pin path", "Graph.Nodes[0].Split.Inputs[8]: \"Value\" comes earlier",
			"Graph.Nodes[0].Split.Inputs[9]: \"Value.Q.R\" is a field of \"Value.Q\", which is not an earlier entry",
			"Graph.Nodes[0].Split.Inputs[10]: \"Other.X\" is a field of \"Other\", which is not an earlier entry",
			"Graph.Nodes[1].Split: expected an object", "Graph.Nodes[2].Split.Outputs: expected an array",
		];
		Assert.All(analyzed.Diagnostics, static diagnostic => Assert.Equal(DiagnosticCode.InvalidDocument, diagnostic.Code));
		Assert.Equal(expected.Length, analyzed.Diagnostics.Count);
		for (int index = 0; index < expected.Length; index++)
		{
			Assert.StartsWith(expected[index], analyzed.Diagnostics[index].Message, StringComparison.Ordinal);
		}
		// What is kept splits.
		Assert.Contains("In Value.Z: float", ShownPins(analyzed, 1));

		// At most 64 entries per list are kept, and a field of an entry dropped by the limit goes with it.
		string many = string.Join(", ", Enumerable.Range(0, DocumentSplit.MaxEntries).Select(static index => $"\"Pin{index}\"")) + ", \"Value\", \"Value.X\"";
		AnalyzedDocument limited = VisualScriptTesting.Analyze(RuntimeTesting.Library, VisualScriptTesting.Document(
			$$"""{ "ID": 1, "Type": "Variable.Set", "Settings": { "Variable": 9 }, "Split": { "Inputs": [{{many}}] } }""",
			variables: """{ "ID": 9, "Name": "Position", "Type": "Strada.Vector3" }"""));
		Assert.Equal(["Graph.Nodes[0].Split.Inputs[64]: a list of split pins has at most 64 entries",
			"Graph.Nodes[0].Split.Inputs[65]: a list of split pins has at most 64 entries"],
			limited.Diagnostics.Where(static diagnostic => diagnostic.Code == DiagnosticCode.InvalidDocument).Select(static diagnostic => diagnostic.Message));
		Assert.Contains("In Value: Strada.Vector3", ShownPins(limited, 1));
	}

	private static AnalyzedDocument Analyze(DocumentBuilder document) => VisualScriptTesting.Analyze(RuntimeTesting.Library, document.Build());

	private static string Settings(uint variable) => FormattableString.Invariant($"{{ \"Variable\": {variable} }}");

	// The pins analysis shows, as "In Name: type".
	private static string[] ShownPins(AnalyzedDocument document, uint node) =>
		[.. VisualScriptTesting.Node(document, node).GetShownPins().Select(static pin => $"{(pin.IsInput ? "In" : "Out")} {pin.Name}: {pin.Type.Id}")];

	private static AnalyzedPin Shown(AnalyzedDocument document, uint node, string name, PinDirection direction) =>
		VisualScriptTesting.Node(document, node).FindShownPin(name, direction) ??
		throw new Xunit.Sdk.XunitException($"node {node} shows no {direction} pin {name}");

	private static List<(DiagnosticCode Code, uint? Node, string? Pin)> Findings(AnalyzedDocument document) =>
		[.. document.Diagnostics.Select(static diagnostic => (diagnostic.Code, diagnostic.Node, diagnostic.Pin))];

	// The analysis JSON's pins of a node.
	private static JsonElement WrittenNode(AnalyzedDocument document, uint node) =>
		VisualScriptTesting.ParseObject(AnalysisWriter.Write(document)).GetProperty("Nodes").EnumerateArray()
			.Single(entry => entry.GetProperty("ID").GetUInt32() == node);

	private static string?[] WrittenPins(AnalyzedDocument document, uint node) =>
		[.. WrittenNode(document, node).GetProperty("Pins").EnumerateArray().Select(static pin => pin.GetProperty("Name").GetString())];

	private static JsonElement WrittenPin(AnalyzedDocument document, uint node, string name) =>
		WrittenNode(document, node).GetProperty("Pins").EnumerateArray().Single(pin => pin.GetProperty("Name").GetString() == name);
}

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Text.Json;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The analysis checks the input actions a document names against the project's, when the references bundle tells them
// (spec section 6 of IA2): the Action setting of the two action events and the unlinked Action inputs of Strada.Input's
// members. A name the project lacks is UnknownAction with the nearest names, a project without actions is one NoInputActions
// per document, and a bundle that tells nothing checks nothing.
public sealed class InputActionAnalysisTests
{
	private const string Actions = """{ "Self": "0", "Graphs": [], "InputActions": { "Names": ["Jump", "Move", "Throttle"] } }""";

	private static string Missing(string why) => $$"""{ "InputActions": { "Missing": {{JsonSerializer.Serialize(why)}} } }""";

	private static AnalyzedDocument Analyze(string document, string references, LibraryAnalyses? libraries = null) =>
		GraphAnalyzer.Analyze(VisualScriptTesting.Library, Encoding.UTF8.GetBytes(document), GraphReferences.Parse(Encoding.UTF8.GetBytes(references)),
			libraries ?? new LibraryAnalyses());

	private static string Pressed(uint id, string action, string extra = "") =>
		$$"""{ "ID": {{id}}, "Type": "Event.ActionPressed", "Settings": { "Action": {{JsonSerializer.Serialize(action)}} }{{extra}} }""";

	private static string GetAction(uint id, string? action) => action == null
		? $$"""{ "ID": {{id}}, "Type": "Call:Strada.Input.GetAction(string)" }"""
		: $$"""{ "ID": {{id}}, "Type": "Call:Strada.Input.GetAction(string)", "Inputs": { "Action": {{JsonSerializer.Serialize(action)}} } }""";

	private static IReadOnlyList<Diagnostic> Findings(AnalyzedDocument document) =>
		[.. document.Diagnostics.Where(static diagnostic => diagnostic.Code is DiagnosticCode.UnknownAction or DiagnosticCode.NoInputActions)];

	[Fact]
	public void AnEventsActionTheProjectLacksIsAWarningOnTheSettingWithTheNearestActions()
	{
		AnalyzedDocument document = Analyze(VisualScriptTesting.Document(Pressed(1, "Jmup") + ", " + Pressed(2, "Throttle")), Actions);
		Diagnostic diagnostic = VisualScriptTesting.Single(document, DiagnosticCode.UnknownAction);
		Assert.Equal(DiagnosticSeverity.Warning, diagnostic.Severity);
		Assert.Equal(1u, diagnostic.Node);
		Assert.Equal("Action", diagnostic.Setting);
		Assert.Null(diagnostic.Span);
		Assert.Null(diagnostic.Pin);
		Assert.Equal("Action: the project has no input action \"Jmup\"", diagnostic.Message);
		// However far: a project has few actions, and a renamed one is often far from its former name.
		Assert.Equal(["Jump", "Move", "Throttle"], diagnostic.Suggestions);
		// The node still runs as before (it never starts), so its binding is the name.
		Assert.IsType<ActionEventBinding>(VisualScriptTesting.Node(document, 1).Binding);
		Assert.False(VisualScriptTesting.Has(Analyze(VisualScriptTesting.Document(Pressed(1, "Jump")), Actions), DiagnosticCode.UnknownAction));
	}

	[Fact]
	public void NamesMatchWithTheirLetterCaseAndTheReleasedEventIsCheckedToo()
	{
		AnalyzedDocument document = Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Event.ActionReleased", "Settings": { "Action": "jump" } }"""), Actions);
		Diagnostic diagnostic = VisualScriptTesting.Single(document, DiagnosticCode.UnknownAction);
		Assert.Equal("Jump", diagnostic.Suggestions[0]);
	}

	[Fact]
	public void AnUnlinkedActionInputIsCheckedByTheValueItTakes()
	{
		AnalyzedDocument document = Analyze(VisualScriptTesting.Document(
			GetAction(1, "Throtle") + ", " + GetAction(2, "Move") + ", " + GetAction(3, null) + ", " +
			"""{ "ID": 4, "Type": "Call:Strada.Input.IsActionDown(string)" }, { "ID": 5, "Type": "Literal", "Settings": { "Type": "string", "Value": "Nothing" } }""",
			"""{ "From": [5, "Value"], "To": [4, "Action"] }"""), Actions);
		List<Diagnostic> findings = [.. Findings(document)];
		Assert.Equal(2, findings.Count);
		Diagnostic unknown = findings.Single(static finding => finding.Node == 1);
		Assert.Equal(DiagnosticCode.UnknownAction, unknown.Code);
		Assert.Equal("Action", unknown.Pin);
		Assert.Equal(PinDirection.In, unknown.Direction);
		Assert.Null(unknown.Setting);
		Assert.Equal("Action: the project has no input action \"Throtle\"", unknown.Message);
		Assert.Equal("Throttle", unknown.Suggestions[0]);
		// An input that stores no name takes the empty one, which names no action; a linked one is never checked.
		Diagnostic empty = findings.Single(static finding => finding.Node == 3);
		Assert.Equal("Action is empty: it names none of the project's input actions", empty.Message);
		Assert.Equal(3, empty.Suggestions.Count);
	}

	[Fact]
	public void AStoredValueThatIsNoNameIsOnlyAnInvalidValue()
	{
		AnalyzedDocument document = Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Call:Strada.Input.GetAction(string)", "Inputs": { "Action": 3 } }"""), Actions);
		Assert.True(VisualScriptTesting.Has(document, DiagnosticCode.InvalidValue));
		Assert.Empty(Findings(document));
	}

	[Fact]
	public void EveryMemberOfInputThatNamesAnActionIsChecked()
	{
		string[] types =
		[
			"Call:Strada.Input.GetAction(string)", "Call:Strada.Input.GetActionVector(string)", "Call:Strada.Input.IsActionDown(string)",
			"Call:Strada.Input.IsActionPressed(string)", "Call:Strada.Input.IsActionReleased(string)",
			"Call:Strada.Input.SetActionOverride(string,float)", "Call:Strada.Input.SetActionVectorOverride(string,Strada.Vector2)",
			"Call:Strada.Input.ClearActionOverride(string)", "Call:Strada.Input.StartRebind(string,int,int)",
			"Call:Strada.Input.Rebind(string,int,string,int)", "Call:Strada.Input.GetBindingControls(string,int)",
		];
		for (int index = 0; index < types.Length; index++)
		{
			string node = $$"""{ "ID": 1, "Type": "{{types[index]}}", "Inputs": { "Action": "Jupm" } }""";
			AnalyzedDocument document = Analyze(VisualScriptTesting.Document(node), Actions);
			Assert.True(VisualScriptTesting.Single(document, DiagnosticCode.UnknownAction).Node == 1, types[index]);
		}
		// Rebind's controls are no action's name.
		AnalyzedDocument rebind = Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Call:Strada.Input.Rebind(string,int,string,int)", "Inputs": { "Action": "Jump", "Controls": "Key/J" } }"""), Actions);
		Assert.Empty(Findings(rebind));
	}

	[Fact]
	public void AProjectWithoutActionsIsOneWarningForTheWholeDocument()
	{
		AnalyzedDocument document = Analyze(VisualScriptTesting.Document(Pressed(1, "Jump") + ", " + GetAction(2, "Move") + ", " + Pressed(3, "Move")),
			Missing("the project names no input actions (Project Settings, Input Actions)"));
		Diagnostic diagnostic = Assert.Single(Findings(document));
		Assert.Equal(DiagnosticCode.NoInputActions, diagnostic.Code);
		Assert.Equal(DiagnosticSeverity.Warning, diagnostic.Severity);
		Assert.Equal(1u, diagnostic.Node);
		Assert.Equal("Action", diagnostic.Setting);
		Assert.Equal("the project names no input actions (Project Settings, Input Actions): action names are not checked", diagnostic.Message);
		Assert.Empty(diagnostic.Suggestions);

		// A function's nodes count with the event graph's: still one.
		string function = """{ "ID": 100, "Name": "Read", "Pure": false, "Inputs": [], "Outputs": [], "Locals": [], "Graph": { "Nodes": [ { "ID": 101, "Type": "Function.Entry" }, """ +
			GetAction(102, "Jump") + """ ], "Links": [], "Comments": [] } }""";
		Assert.Single(Findings(Analyze(VisualScriptTesting.Document(Pressed(1, "Jump"), functions: function), Missing("none"))));
	}

	[Fact]
	public void NothingIsCheckedWhenNobodyToldTheActions()
	{
		string document = VisualScriptTesting.Document(Pressed(1, "Jmup") + ", " + GetAction(2, null));
		string[] bundles =
		[
			"",
			"""{ "Self": "0", "Graphs": [] }""",
			"""{ "InputActions": null }""",
			"""{ "InputActions": [] }""",
			"""{ "InputActions": {} }""",
			"""{ "InputActions": { "Names": "Jump" } }""",
			"""{ "InputActions": { "Names": ["Jump", 3] } }""",
			"""{ "InputActions": { "Missing": "" } }""",
			"""{ "InputActions": { "Missing": 1 } }""",
		];
		foreach (string bundle in bundles)
		{
			AnalyzedDocument analyzed = Analyze(document, bundle);
			Assert.True(Findings(analyzed).Count == 0, bundle);
			Assert.Null(GraphReferences.Parse(Encoding.UTF8.GetBytes(bundle)).InputActions);
		}
	}

	[Fact]
	public void ADocumentWithoutActionNodesAnalyzesAlikeWithOrWithoutTheActions()
	{
		string document = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Event.Update" }, { "ID": 2, "Type": "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", "Inputs": { "Key": "Space" } }""");
		byte[] told = AnalysisWriter.Write(Analyze(document, Actions));
		byte[] untold = AnalysisWriter.Write(Analyze(document, string.Empty));
		Assert.Equal(Encoding.UTF8.GetString(untold), Encoding.UTF8.GetString(told));
	}

	[Fact]
	public void TheLibrariesACallLinksAreNotCheckedForTheirCaller()
	{
		DocumentBuilder library = new() { Kind = "Library" };
		FunctionBuilder read = library.Function("Read");
		read.Graph.Flow(read.Entry, read.Graph.Node("Call:Strada.Input.SetActionOverride(string,float)", inputs: """{ "Action": "Jmup", "Value": 1.0 }"""));
		DocumentBuilder script = new();
		uint update = script.Graph.Node("Event.Update");
		script.Graph.Flow(update, script.Graph.Node("Function.Call", CrossGraphTesting.Call(read, CrossGraphTesting.Motion)));
		string references = new BundleBuilder().Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, library).Build();
		references = references[..references.LastIndexOf('}')] + """, "InputActions": { "Names": ["Jump"] } }""";
		Assert.NotNull(GraphReferences.Parse(Encoding.UTF8.GetBytes(references)).InputActions);
		LibraryAnalyses libraries = new();
		AnalyzedDocument caller = GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(script.Build()),
			GraphReferences.Parse(Encoding.UTF8.GetBytes(references)), libraries);
		VisualScriptTesting.AssertNoErrors(caller);
		Assert.Empty(Findings(caller));
		LibraryAnalyses.Entry entry = libraries.Find(CrossGraphTesting.Motion) ?? throw new InvalidOperationException("the library was not analyzed");
		Assert.Empty(Findings(entry.Analysis));
		// Analyzed as a document of its own, told the actions, the library's call is checked.
		Assert.Single(Findings(Analyze(library.Build(), Actions)));
	}

	[Fact]
	public void TheLibraryMarksTheActionNamesOfInputAndTheEventsAndOnlyThose()
	{
		List<string> pins = [];
		List<string> settings = [];
		foreach (NodeDefinition node in VisualScriptTesting.Library.Nodes)
		{
			foreach (PinDescription pin in node.Pins.Where(static pin => pin.NamesInputAction))
			{
				pins.Add($"{node.Type} {pin.Name}");
				Assert.True(pin.IsInput && pin.Type == VisualScriptTesting.Library.String, node.Type);
				// The engine finds the documents to tell the actions by this prefix (VisualScriptEngine::Analyze).
				Assert.StartsWith("Call:Strada.Input.", node.Type, StringComparison.Ordinal);
			}
			foreach (SettingDescription setting in node.Settings.Where(static setting => setting.NamesInputAction))
			{
				settings.Add($"{node.Type} {setting.Name}");
				Assert.Equal(SettingKind.String, setting.Kind);
			}
		}
		Assert.Equal(
		[
			"Call:Strada.Input.ClearActionOverride(string) Action", "Call:Strada.Input.GetAction(string) Action",
			"Call:Strada.Input.GetActionVector(string) Action", "Call:Strada.Input.GetBindingControls(string,int) Action",
			"Call:Strada.Input.IsActionDown(string) Action", "Call:Strada.Input.IsActionPressed(string) Action",
			"Call:Strada.Input.IsActionReleased(string) Action", "Call:Strada.Input.Rebind(string,int,string,int) Action",
			"Call:Strada.Input.SetActionOverride(string,float) Action", "Call:Strada.Input.SetActionVectorOverride(string,Strada.Vector2) Action",
			"Call:Strada.Input.StartRebind(string,int,int) Action",
		], pins.Order(StringComparer.Ordinal));
		Assert.Equal(["Event.ActionPressed Action", "Event.ActionReleased Action"], settings.Order(StringComparer.Ordinal));

		// The library JSON says so: "InputAction" on those pins and settings, nowhere else.
		JsonElement library = VisualScriptTesting.ParseObject(LibraryWriter.Write(VisualScriptTesting.Library));
		int marked = 0;
		foreach (JsonElement node in library.GetProperty("Nodes").EnumerateArray())
		{
			foreach (string list in new[] { "Pins", "Settings" })
			{
				foreach (JsonElement entry in node.GetProperty(list).EnumerateArray())
				{
					if (entry.TryGetProperty("InputAction", out JsonElement flag))
					{
						Assert.Equal(JsonValueKind.True, flag.ValueKind);
						marked++;
					}
				}
			}
		}
		Assert.Equal(pins.Count + settings.Count, marked);
	}

	// A member of the scripting API added with an action parameter is marked in the same change.
	[Fact]
	public void EveryActionParameterOfInputIsMarked()
	{
		foreach (MethodInfo method in typeof(Input).GetMethods(BindingFlags.Public | BindingFlags.Static))
		{
			foreach (ParameterInfo parameter in method.GetParameters())
			{
				bool action = parameter.ParameterType == typeof(string) && parameter.Name == "action";
				Assert.True(action == parameter.IsDefined(typeof(InputActionNameAttribute), false), $"{method.Name}({parameter.Name})");
			}
		}
	}
}

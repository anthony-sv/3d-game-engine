using System.IO;
using System.Linq;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The shared fixtures of Tests/Data/VisualScripting (spec section 11.1) are semantically valid: every document, and the
// content of Example.sgraph, analyzes without an Error diagnostic, and the editor's document does what it says.
public sealed class FixtureAnalysisTests
{
	public static TheoryData<string> Documents => ["Events.json", "Flow.json", "Data.json", "Functions.json", "Variables.json", "Reflected.json", "Editor.json"];

	[Theory]
	[MemberData(nameof(Documents))]
	public void TheDocumentsAnalyzeWithoutErrors(string name)
	{
		string path = Path.Combine(VisualScriptTesting.FixtureDirectory, "Documents", name);
		Assert.True(File.Exists(path), $"{path} is missing: the test project copies Tests/Data/VisualScripting");
		AnalyzedDocument document = GraphAnalyzer.Analyze(VisualScriptTesting.Library, File.ReadAllBytes(path));
		VisualScriptTesting.AssertNoErrors(document);
		Assert.All(document.Graphs.SelectMany(static graph => graph.Nodes), static node => Assert.Equal(NodeResolution.Exact, node.Resolution));
	}

	// Documents/Editor.json (spec-slice2 section 15.1) is what its comments say, run with the fuzz library's stand-ins of the
	// engine members: the raycast's split hit gives the ground's height and the distance, the split translation takes field
	// literals and the height, the disabled log and delay pass the flow on, development-only nodes run, and the disabled
	// Update and the disabled entry of Report (which runs, with the one Warning) change nothing.
	[Fact]
	public void TheEditorDocumentRunsAsItsCommentsSay()
	{
		byte[] text = File.ReadAllBytes(Path.Combine(VisualScriptTesting.FixtureDirectory, "Documents", "Editor.json"));
		AnalyzedDocument document = GraphAnalyzer.Analyze(VisualScriptTesting.Library, text);
		Diagnostic warning = Assert.Single(document.Diagnostics);
		Assert.Equal((DiagnosticCode.DisabledIgnored, 30u), (warning.Code, warning.Node));
		Assert.Equal("Then", VisualScriptTesting.Node(document, 17).PassThrough?.Name);
		Assert.Equal("Completed", VisualScriptTesting.Node(document, 19).PassThrough?.Name);
		Assert.Null(VisualScriptTesting.Node(document, 22).PassThrough);
		Assert.True(VisualScriptTesting.Node(document, 11).FindShownPin("Hit.Point.Y", PinDirection.Out)?.Links.Count == 1);
		Assert.True(VisualScriptTesting.Node(document, 14).FindShownPin("Value.Y", PinDirection.In)?.IsConnected);

		FuzzStandIns.Messages = [];
		try
		{
			FakeEnvironment environment = FuzzLibrary.CreateEnvironment();
			VisualScriptRuntime runtime = new(environment, () => FuzzLibrary.Library);
			runtime.Load(100, "Assets/Editor.sgraph", text);
			Assert.Equal(VisualScriptStatus.Success, runtime.CreateInstance(FuzzLibrary.Hero, 100, []));
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(FuzzLibrary.Hero, ScriptEvent.Create, 0.0f, 0));
			// The ray from the origin down hits 2.5 below it.
			Assert.Equal(new Vector3(1.0f, -2.5f, -2.0f), FuzzStandIns.Translation);
			Assert.Equal(["2.5", "logged: enabled", "logged in development builds only"], FuzzStandIns.Messages);
			// The development-only delay waits 0.25 s, the enabled one 0.125 s.
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(FuzzLibrary.Hero, ScriptEvent.Update, 0.25f, 0));
			Assert.Equal(3, FuzzStandIns.Messages.Count);
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(FuzzLibrary.Hero, ScriptEvent.Update, 0.125f, 0));
			Assert.Equal(["2.5", "logged: enabled", "logged in development builds only", "done"], FuzzStandIns.Messages);
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(FuzzLibrary.Hero, ScriptEvent.Update, 1.0f, 0));
			Assert.Equal(4, FuzzStandIns.Messages.Count);
			Assert.Empty(environment.Logs);
		}
		finally
		{
			FuzzStandIns.Messages = null;
		}
	}

	[Fact]
	public void TheExampleAnalyzesWithoutErrors()
	{
		using JsonDocument file = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(VisualScriptTesting.FixtureDirectory, "Example.sgraph")));
		AnalyzedDocument document = VisualScriptTesting.Analyze(file.RootElement.GetProperty("VisualScript").GetRawText());
		VisualScriptTesting.AssertNoErrors(document);
		Assert.Empty(document.Diagnostics);
		// The reflected node is pure, and the function call is impure (AddScore writes a variable).
		Assert.True(VisualScriptTesting.Node(document, 11).Pure);
		Assert.False(VisualScriptTesting.Node(document, 13).Pure);
		Assert.Equal("Strada.KeyCode", VisualScriptTesting.Pin(document, 11, "Key", PinDirection.In).Type.Id);
		Assert.Equal(KeyCode.Space, VisualScriptTesting.Pin(document, 11, "Key", PinDirection.In).Value);
		Assert.Equal(5, VisualScriptTesting.Pin(document, 13, "Points", PinDirection.In).Value);
	}
}

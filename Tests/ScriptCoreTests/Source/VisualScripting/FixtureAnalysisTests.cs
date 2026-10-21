using System.IO;
using System.Linq;
using System.Text.Json;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The shared fixtures of Tests/Data/VisualScripting (spec section 11.1) are semantically valid: every document, and the
// content of Example.sgraph, analyzes without an Error diagnostic.
public sealed class FixtureAnalysisTests
{
	public static TheoryData<string> Documents => ["Events.json", "Flow.json", "Data.json", "Functions.json", "Variables.json", "Reflected.json"];

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

using System.Linq;
using System.Text.Json;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// How stored node types find their library node (spec section 4.5, decision 13): exact, the redirect table, a unique
// overload by member name, otherwise unknown or ambiguous.
public sealed class NodeResolutionTests
{
	private static NodeLibrary LibraryWithRedirects(params NodeRedirect[] redirects) => NodeLibraryBuilder.Build(new NodeLibrarySources
	{
		ApiTypes = NodeLibrarySources.GetApiTypes(),
		Redirects = redirects,
	});

	// A pure node feeding a Branch, so its Result is used.
	private static string Feeding(string type, string inputs, string resultPin = "Result") => VisualScriptTesting.Document(
		$$"""{ "ID": 1, "Type": "Event.Update" }, { "ID": 2, "Type": "Flow.Branch" }, { "ID": 3, "Type": "{{type}}", "Inputs": { {{inputs}} } }""",
		$$"""{ "From": [1, "Then"], "To": [2, "Execute"] }, { "From": [3, "{{resultPin}}"], "To": [2, "Condition"] }""");

	[Fact]
	public void ExactTypesResolveExactly()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(Feeding("Call:Strada.Input.IsKeyDown(Strada.KeyCode)", "\"Key\": \"Space\""));
		VisualScriptTesting.AssertNoErrors(document);
		Assert.Empty(document.Diagnostics);
		AnalyzedNode node = VisualScriptTesting.Node(document, 3);
		Assert.Equal(NodeResolution.Exact, node.Resolution);
		Assert.Null(node.CanonicalType);
		Assert.Equal(NodeResolution.Exact, VisualScriptTesting.Node(document, 2).Resolution);
	}

	[Fact]
	public void ARedirectRenamesTheTypeAndItsPins()
	{
		NodeLibrary library = LibraryWithRedirects(new NodeRedirect("Call:Strada.Input.IsKeyHeld(Strada.KeyCode)",
			"Call:Strada.Input.IsKeyDown(Strada.KeyCode)", [new PinRename(PinDirection.In, "Button", "Key"), new PinRename(PinDirection.Out, "Held", "Result")]));
		AnalyzedDocument document = VisualScriptTesting.Analyze(library, Feeding("Call:Strada.Input.IsKeyHeld(Strada.KeyCode)", "\"Button\": \"Space\"", "Held"));
		VisualScriptTesting.AssertNoErrors(document);
		AnalyzedNode node = VisualScriptTesting.Node(document, 3);
		Assert.Equal(NodeResolution.Redirect, node.Resolution);
		Assert.Equal("Call:Strada.Input.IsKeyDown(Strada.KeyCode)", node.CanonicalType);
		Assert.Equal([new PinRename(PinDirection.In, "Button", "Key"), new PinRename(PinDirection.Out, "Held", "Result")], node.PinRenames);
		Assert.Equal(KeyCode.Space, node.FindPin("Key", PinDirection.In)!.Value);
		Assert.True(node.FindPin("Result", PinDirection.Out)!.Links.Single().IsValid);
		Diagnostic updated = VisualScriptTesting.Single(document, DiagnosticCode.NodeUpdated);
		Assert.Equal(DiagnosticSeverity.Warning, updated.Severity);
		Assert.Equal(3u, updated.Node);
		Assert.Equal("node updated: Call:Strada.Input.IsKeyHeld(Strada.KeyCode) → Call:Strada.Input.IsKeyDown(Strada.KeyCode)", updated.Message);

		// The analysis JSON reports the canonical type and the renames.
		JsonElement entry = VisualScriptTesting.ParseObject(AnalysisWriter.Write(document)).GetProperty("Nodes").EnumerateArray()
			.Single(static node => node.GetProperty("ID").GetUInt32() == 3);
		Assert.Equal("Redirect", entry.GetProperty("Resolved").GetString());
		Assert.Equal("Call:Strada.Input.IsKeyDown(Strada.KeyCode)", entry.GetProperty("CanonicalType").GetString());
		Assert.Equal("""[{"Direction":"In","From":"Button","To":"Key"},{"Direction":"Out","From":"Held","To":"Result"}]""",
			entry.GetProperty("PinRenames").GetRawText());
	}

	[Fact]
	public void ChainedRedirectsComposeTheirRenames()
	{
		NodeLibrary library = LibraryWithRedirects(
			new NodeRedirect("Call:Strada.Input.IsKeyHeld(Strada.KeyCode)", "Call:Strada.Input.IsKeyPushed(Strada.KeyCode)",
				[new PinRename(PinDirection.In, "Button", "Code")]),
			new NodeRedirect("Call:Strada.Input.IsKeyPushed(Strada.KeyCode)", "Call:Strada.Input.IsKeyDown(Strada.KeyCode)",
				[new PinRename(PinDirection.In, "Code", "Key")]));
		AnalyzedDocument document = VisualScriptTesting.Analyze(library, Feeding("Call:Strada.Input.IsKeyHeld(Strada.KeyCode)", "\"Button\": \"Space\""));
		VisualScriptTesting.AssertNoErrors(document);
		AnalyzedNode node = VisualScriptTesting.Node(document, 3);
		Assert.Equal(NodeResolution.Redirect, node.Resolution);
		Assert.Equal([new PinRename(PinDirection.In, "Button", "Key")], node.PinRenames);
		Assert.Equal(KeyCode.Space, node.FindPin("Key", PinDirection.In)!.Value);
	}

	[Fact]
	public void SignatureLessTypesResolveToTheOnlyOverload()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(Feeding("Call:Strada.Input.IsKeyDown", "\"Key\": \"Space\""));
		VisualScriptTesting.AssertNoErrors(document);
		AnalyzedNode node = VisualScriptTesting.Node(document, 3);
		Assert.Equal(NodeResolution.Unique, node.Resolution);
		Assert.Equal("Call:Strada.Input.IsKeyDown(Strada.KeyCode)", node.CanonicalType);
		Assert.Empty(node.PinRenames);
		Assert.Equal("node updated: Call:Strada.Input.IsKeyDown → Call:Strada.Input.IsKeyDown(Strada.KeyCode)",
			VisualScriptTesting.Single(document, DiagnosticCode.NodeUpdated).Message);
		JsonElement entry = VisualScriptTesting.ParseObject(AnalysisWriter.Write(document)).GetProperty("Nodes").EnumerateArray()
			.Single(static node => node.GetProperty("ID").GetUInt32() == 3);
		Assert.Equal("Unique", entry.GetProperty("Resolved").GetString());
		Assert.Equal("[]", entry.GetProperty("PinRenames").GetRawText());

		// A generic method without its signature, and a re-signed method whose stored pins still fit.
		AnalyzedDocument generic = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Call:Strada.Entity.GetComponent", "Settings": { "TypeArguments": ["Strada.RigidBodyComponent"] } }"""));
		Assert.Equal("Call:Strada.Entity.GetComponent<T>()", VisualScriptTesting.Node(generic, 1).CanonicalType);
		Assert.Equal("Strada.RigidBodyComponent", VisualScriptTesting.Pin(generic, 1, "Result").Type.Id);
		AnalyzedDocument resigned = VisualScriptTesting.Analyze(Feeding("Call:Strada.Mathf.Approximately(int,int)", "\"A\": 1.0, \"B\": 2.0"));
		Assert.Equal("Call:Strada.Mathf.Approximately(float,float)", VisualScriptTesting.Node(resigned, 3).CanonicalType);
	}

	[Fact]
	public void StoredPinsChooseBetweenOverloads()
	{
		AnalyzedDocument scalar = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "New:Strada.Vector3", "Inputs": { "Scalar": 2.0 } }"""));
		Assert.Equal("New:Strada.Vector3(float)", VisualScriptTesting.Node(scalar, 1).CanonicalType);
		AnalyzedDocument components = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "New:Strada.Vector3", "Inputs": { "Y": 2.0 } }"""));
		Assert.Equal("New:Strada.Vector3(float,float,float)", VisualScriptTesting.Node(components, 1).CanonicalType);
		AnalyzedDocument ray = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "New:Strada.Ray" }"""));
		Assert.Equal("New:Strada.Ray(Strada.Vector3,Strada.Vector3)", VisualScriptTesting.Node(ray, 1).CanonicalType);
	}

	[Fact]
	public void SeveralFittingOverloadsAreAmbiguous()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Call:Strada.Random.Range" }"""));
		Diagnostic ambiguous = VisualScriptTesting.Single(document, DiagnosticCode.AmbiguousNode);
		Assert.Equal(DiagnosticSeverity.Error, ambiguous.Severity);
		Assert.Equal(1u, ambiguous.Node);
		Assert.Equal(["Call:Strada.Random.Range(float,float)", "Call:Strada.Random.Range(int,int)"], ambiguous.Suggestions);
		AnalyzedNode node = VisualScriptTesting.Node(document, 1);
		Assert.Equal(NodeResolution.Unknown, node.Resolution);
		Assert.Equal(NodeKind.Unknown, node.Kind);
		Assert.False(VisualScriptTesting.Has(document, DiagnosticCode.NodeUpdated));
	}

	[Fact]
	public void UnknownTypesGetTheNearestTypesAsSuggestions()
	{
		AnalyzedDocument typo = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Brnach" }"""));
		Diagnostic unknown = VisualScriptTesting.Single(typo, DiagnosticCode.UnknownNodeType);
		Assert.Equal("Flow.Branch", unknown.Suggestions[0]);
		Assert.True(unknown.Suggestions.Count <= Suggestions.Count);

		AnalyzedDocument member = VisualScriptTesting.Analyze(Feeding("Call:Strada.Input.IsKeyDwn(Strada.KeyCode)", "\"Key\": \"Space\""));
		Assert.Equal("Call:Strada.Input.IsKeyDown(Strada.KeyCode)", VisualScriptTesting.Single(member, DiagnosticCode.UnknownNodeType).Suggestions[0]);
		// An unknown node keeps the pins its links and inputs name, typed as wildcards, and raises no further errors.
		AnalyzedNode node = VisualScriptTesting.Node(member, 3);
		Assert.Equal(NodeKind.Unknown, node.Kind);
		Assert.Equal("Is Key Dwn", node.Title);
		Assert.Equal(["Out:Result:*", "In:Key:*"], node.Pins.Select(static pin => $"{pin.Direction}:{pin.Name}:{pin.Type.Id}"));
		Assert.Single(VisualScriptTesting.Errors(member));

		// Overloads exist, but none has the stored pins.
		AnalyzedDocument pins = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Call:Strada.Mathf.Clamp", "Inputs": { "Low": 0.0 } }"""));
		Diagnostic noOverload = VisualScriptTesting.Single(pins, DiagnosticCode.UnknownNodeType);
		Assert.Equal(["Call:Strada.Mathf.Clamp(float,float,float)"], noOverload.Suggestions);
	}

	[Fact]
	public void LookupKeysDropSignaturesAndTypeParameters()
	{
		Assert.Equal("Call:Strada.Input.IsKeyDown", NodeResolver.LookupKey("Call:Strada.Input.IsKeyDown(Strada.KeyCode)"));
		Assert.Equal("Call:Strada.Entity.GetComponent", NodeResolver.LookupKey("Call:Strada.Entity.GetComponent<T>()"));
		Assert.Equal("New:Strada.Vector3", NodeResolver.LookupKey("New:Strada.Vector3(float,float,float)"));
		Assert.Equal("Get:Strada.Time.DeltaTime", NodeResolver.LookupKey("Get:Strada.Time.DeltaTime"));
	}
}

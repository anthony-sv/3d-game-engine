using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The read-back of changes of an instance's public variables (VisualScriptDescribeChangedInstanceVariables,
// Docs/Architecture.md §11, "Long lists"): what changed since the instance's last, in the stored form, and the names of the
// others.
public sealed class ChangedVariablesTests
{
	// Samples (float[]) that Event.Update replaces with a copy holding Clock (float) as one more element, and Steady (int).
	private static (DocumentBuilder Document, uint Samples) Grower()
	{
		DocumentBuilder document = new();
		uint samples = document.Variable("Samples", "float[]", "[1.0, 2.0]", isPublic: true);
		uint clock = document.Variable("Clock", "float", "0.5", isPublic: true);
		document.Variable("Steady", "int", "7", isPublic: true);
		document.Variable("Hidden", "int", "1");
		GraphBuilder graph = document.Graph;
		uint add = graph.Node("Array.Add");
		graph.Link(graph.Get(samples), "Value", add, "Array");
		graph.Link(graph.Get(clock), "Value", add, "Value");
		uint set = graph.Set(samples);
		graph.Link(add, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Update"), set);
		return (document, samples);
	}

	private static (string[] Changed, string[] Unchanged, JsonElement Values) Describe(TestRuntime runtime)
	{
		HostReply reply = runtime.Runtime.DescribeChangedVariables(TestRuntime.Hero);
		Assert.Equal(VisualScriptStatus.Success, reply.Status);
		using JsonDocument json = JsonDocument.Parse(reply.Text!);
		JsonElement changed = json.RootElement.GetProperty("Changed").Clone();
		return ([.. changed.EnumerateObject().Select(static property => property.Name)],
			[.. json.RootElement.GetProperty("Unchanged").EnumerateArray().Select(static name => name.GetString()!)], changed);
	}

	[Fact]
	public void TheFirstReadBackAnswersEveryPublicVariableAndTheNextOnlyWhatChanged()
	{
		TestRuntime runtime = TestRuntime.Start(Grower().Document);

		(string[] changed, string[] unchanged, JsonElement values) = Describe(runtime);
		Assert.Equal(["Samples", "Clock", "Steady"], changed);
		Assert.Empty(unchanged);
		Assert.Equal("""{"Type":"Float[]","Value":[1,2]}""", values.GetProperty("Samples").GetRawText());
		// As DescribeInstanceVariables answers them.
		using (JsonDocument all = JsonDocument.Parse(runtime.Runtime.DescribeInstanceVariables(TestRuntime.Hero)!))
		{
			Assert.Equal(all.RootElement.GetRawText(), values.GetRawText());
		}
		(changed, unchanged, _) = Describe(runtime);
		Assert.Empty(changed);
		Assert.Equal(["Samples", "Clock", "Steady"], unchanged);

		// The update gives Samples a new array with one more element: answered whole.
		runtime.Invoke(ScriptEvent.Update);
		(changed, unchanged, values) = Describe(runtime);
		Assert.Equal(["Samples"], changed);
		Assert.Equal(["Clock", "Steady"], unchanged);
		Assert.Equal("""{"Type":"Float[]","Value":[1,2,0.5]}""", values.GetProperty("Samples").GetRawText());
	}

	[Fact]
	public void AVariableGivenAValueIsAnsweredAgainWhateverBecameOfIt()
	{
		TestRuntime runtime = TestRuntime.Start(Grower().Document);
		Describe(runtime);

		// Taken, refused for its type, and set to what it holds already: each is answered at the next read-back.
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.SetInstanceVariables(TestRuntime.Hero, """
			{ "Clock": { "Type": "Float", "Value": 4 }, "Steady": { "Type": "Float", "Value": 1 },
			  "Samples": { "Type": "Float[]", "Value": [1, 2] } }
			"""u8));
		(string[] changed, string[] unchanged, JsonElement values) = Describe(runtime);
		Assert.Equal(["Samples", "Clock", "Steady"], changed);
		Assert.Empty(unchanged);
		Assert.Equal("""{"Type":"Int32","Value":7}""", values.GetProperty("Steady").GetRawText());
		Assert.Equal("""{"Type":"Float","Value":4}""", values.GetProperty("Clock").GetRawText());
		Assert.Empty(Describe(runtime).Changed);
	}

	[Fact]
	public void ALiveEditAnswersEveryVariableOfTheNewProgram()
	{
		(DocumentBuilder document, _) = Grower();
		TestRuntime runtime = TestRuntime.Start(document);
		Describe(runtime);

		document.Variable("Extra", "int", "3", isPublic: true);
		runtime.LoadWorking(document.Build());
		(string[] changed, string[] unchanged, _) = Describe(runtime);
		Assert.Equal(["Samples", "Clock", "Steady", "Extra"], changed);
		Assert.Empty(unchanged);
	}

	[Fact]
	public void AnArrayLongerThanAListIsInNeitherPartAndIsAnsweredOnceItFits()
	{
		(DocumentBuilder document, _) = Grower();
		TestRuntime runtime = TestRuntime.Start(document, $$"""{ "Samples": { "Type": "Float[]", "Value": [{{string.Join(",", Enumerable.Repeat("0", ScriptFieldCodec.MaxListElements))}}] } }""");
		Assert.Contains("Samples", Describe(runtime).Changed);

		// One element past what a list holds: left out, reported once.
		runtime.Invoke(ScriptEvent.Update);
		(string[] changed, string[] unchanged, _) = Describe(runtime);
		Assert.DoesNotContain("Samples", changed);
		Assert.DoesNotContain("Samples", unchanged);
		Assert.Equal(["Clock", "Steady"], unchanged);
		Assert.DoesNotContain("Samples", Describe(runtime).Unchanged);
		Assert.Single(runtime.Environment.Warnings, static warning => warning.Contains("more than a list's 4096", System.StringComparison.Ordinal));

		// Given a list that fits, it is answered again.
		Assert.Equal(VisualScriptStatus.Success,
			runtime.Runtime.SetInstanceVariables(TestRuntime.Hero, """{ "Samples": { "Type": "Float[]", "Value": [3] } }"""u8));
		Assert.Equal(["Samples"], Describe(runtime).Changed);
	}

	[Fact]
	public void AnEntityWithoutAnInstanceIsNotFound()
	{
		TestRuntime runtime = new();
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Runtime.DescribeChangedVariables(TestRuntime.Hero).Status);
		Assert.Null(runtime.Runtime.DescribeChangedVariables(TestRuntime.Hero).Text);
	}
}

using System.Text;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Public-variable overrides and the stored-map encoding of instances' variables (spec sections 2.3 and 5.4).
public sealed class VariableTests
{
	private static DocumentBuilder Variables()
	{
		DocumentBuilder document = new();
		document.Variable("Speed", "float", "4.0", isPublic: true);
		document.Variable("Lives", "int", "3", isPublic: true, "Health", "Hearts");
		document.Variable("Distance", "long", "\"-9000000000\"", isPublic: true);
		document.Variable("Mode", "Strada.ForceMode", "\"Impulse\"", isPublic: true);
		document.Variable("Partner", "Strada.Entity", "\"0\"", isPublic: true);
		document.Variable("Look", "Strada.Material", "\"0\"", isPublic: true);
		document.Variable("Spawn", "Strada.Prefab", "\"0\"", isPublic: true);
		document.Variable("Any", "Strada.AssetHandle", "\"0\"", isPublic: true);
		document.Variable("Turn", "Strada.Quaternion", isPublic: true);
		document.Variable("Tint", "Strada.Color", isPublic: true);
		document.Variable("Secret", "int", "1");
		return document;
	}

	[Fact]
	public void OverridesSetPublicVariablesByNameOrFormerName()
	{
		const string overrides = """
			{
				"Speed": { "Type": "Float", "Value": 2.5 },
				"Hearts": { "Type": "Int32", "Value": 9 },
				"Distance": { "Type": "Int64", "Value": "5000000000" },
				"Mode": { "Type": "Int32", "Value": 2 },
				"Partner": { "Type": "Entity", "Value": "4815162342" },
				"Look": { "Type": "Asset", "Value": "12" },
				"Spawn": { "Type": "Prefab", "Value": "13" },
				"Any": { "Type": "Asset", "Value": "14" },
				"Turn": { "Type": "Quaternion", "Value": [0.0, 0.0, 0.0, 2.0] },
				"Secret": { "Type": "Int32", "Value": 5 },
				"Unknown": { "Type": "Int32", "Value": 5 }
			}
			""";
		TestRuntime runtime = TestRuntime.Start(Variables(), overrides);

		Assert.Equal(2.5f, runtime.Variable("Speed"));
		Assert.Equal(9, runtime.Variable("Lives"));
		Assert.Equal(5000000000L, runtime.Variable("Distance"));
		Assert.Equal(ForceMode.Acceleration, runtime.Variable("Mode"));
		// Entity values hold the ID; lookups through the environment turn them into what the scene has.
		Assert.Equal(4815162342UL, Assert.IsType<Entity>(runtime.Variable("Partner")).ID);
		Assert.Equal(new AssetHandle(12), Assert.IsType<Material>(runtime.Variable("Look")).Handle);
		Assert.Equal(new AssetHandle(13), Assert.IsType<Prefab>(runtime.Variable("Spawn")).Handle);
		Assert.Equal(new AssetHandle(14), runtime.Variable("Any"));
		Assert.Equal(Quaternion.Identity, runtime.Variable("Turn"));
		// Private variables are never overridden, and unknown entries are skipped silently.
		Assert.Equal(1, runtime.Variable("Secret"));
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void OverridesOfTheWrongShapeOrTypeAreSkippedWithAWarning()
	{
		const string overrides = """
			{
				"Speed": { "Type": "Double", "Value": 2.5 },
				"Lives": 7,
				"Mode": { "Type": "Int32", "Value": "Impulse" },
				"Distance": { "Type": "Int64" },
				"Look": { "Type": "Asset", "Value": "asset://Hero.smat" }
			}
			""";
		TestRuntime runtime = TestRuntime.Start(Variables(), overrides);

		Assert.Equal(4.0f, runtime.Variable("Speed"));
		Assert.Equal(3, runtime.Variable("Lives"));
		Assert.Equal(ForceMode.Impulse, runtime.Variable("Mode"));
		Assert.Equal(
		[
			$"{TestRuntime.Name} on Hero (7): Speed: stored \"Double\" value ignored: the variable is a Float now",
			$"{TestRuntime.Name} on Hero (7): Lives: stored value ignored: expected {{ \"Type\", \"Value\" }}",
			$"{TestRuntime.Name} on Hero (7): Mode: stored value ignored: expected a Int32 value, not \"Impulse\"",
			$"{TestRuntime.Name} on Hero (7): Distance: stored value ignored: expected {{ \"Type\", \"Value\" }}",
			$"{TestRuntime.Name} on Hero (7): Look: stored value ignored: expected a Asset value, not \"asset://Hero.smat\"",
		], runtime.Environment.Warnings);
	}

	[Theory]
	[InlineData("[1, 2]", "variable values ignored: expected an object")]
	[InlineData("{ \"Speed\": ", "variable values ignored: they are not JSON")]
	public void OverridesThatAreNotAMapAreIgnoredWithAWarning(string overrides, string warning)
	{
		TestRuntime runtime = TestRuntime.Start(Variables(), overrides);
		Assert.Equal(4.0f, runtime.Variable("Speed"));
		Assert.Equal([$"{TestRuntime.Name} on Hero (7): {warning}"], runtime.Environment.Warnings);
	}

	// Overrides that are not text (bytes that are not UTF-8, an escaped surrogate without its pair) are not JSON either:
	// creating and updating the instance ignore them with a Warning.
	[Fact]
	public void OverridesThatAreNotTextAreIgnoredWithAWarning()
	{
		byte[][] overrides =
		[
			[.. """{ "Speed": { "Type": "Float", "Value": 2.5 }, "Tag": { "Type": "String", "Value": """u8, (byte)'"', 0xFF, .. "\" } }"u8],
			"""{ "Speed": { "Type": "Float", "Value": 2.5 }, "Tag": { "Type": "String", "Value": "\uD800" } }"""u8.ToArray(),
			"""{ "Speed": { "Type": "Float", "Value": 2.5 }, "\uDFFF": 1 }"""u8.ToArray(),
		];
		string warning = $"{TestRuntime.Name} on Hero (7): variable values ignored: they are not JSON";
		foreach (byte[] text in overrides)
		{
			TestRuntime runtime = new();
			runtime.LoadWorking(Variables().Build());
			Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.CreateInstance(TestRuntime.Hero, TestRuntime.Graph, text));
			Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.SetInstanceVariables(TestRuntime.Hero, text));
			Assert.Equal(4.0f, runtime.Variable("Speed"));
			Assert.Equal([warning, warning], runtime.Environment.Warnings);
		}
	}

	[Fact]
	public void DescribeInstanceVariablesWritesThePublicVariablesInDocumentOrder()
	{
		TestRuntime runtime = TestRuntime.Start(Variables(), """{ "Partner": { "Type": "Entity", "Value": "8" }, "Look": { "Type": "Asset", "Value": "12" } }""");
		byte[]? json = runtime.Runtime.DescribeInstanceVariables(TestRuntime.Hero);
		Assert.NotNull(json);
		string expected = """
			{"Speed":{"Type":"Float","Value":4},"Lives":{"Type":"Int32","Value":3},"Distance":{"Type":"Int64","Value":"-9000000000"},
			"Mode":{"Type":"Int32","Value":1},"Partner":{"Type":"Entity","Value":"8"},"Look":{"Type":"Asset","Value":"12"},
			"Spawn":{"Type":"Prefab","Value":"0"},"Any":{"Type":"Asset","Value":"0"},"Turn":{"Type":"Quaternion","Value":[0,0,0,1]},
			"Tint":{"Type":"Color","Value":[1,1,1,1]}}
			""";
		Assert.Equal(expected.ReplaceLineEndings(string.Empty), Encoding.UTF8.GetString(json));
		Assert.Null(runtime.Runtime.DescribeInstanceVariables(8));
	}

	[Fact]
	public void DescribedQuaternionsAlwaysHaveADirection()
	{
		DocumentBuilder document = new();
		uint turn = document.Variable("Turn", "Strada.Quaternion", isPublic: true);
		GraphBuilder graph = document.Graph;
		uint zero = graph.Node("Make", """{ "Type": "Strada.Quaternion" }""");
		uint set = graph.Set(turn);
		graph.Link(zero, "Value", set, "Value");
		graph.Flow(graph.Node("Event.Create"), set);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(new Quaternion(0.0f, 0.0f, 0.0f, 0.0f), runtime.Variable("Turn"));
		Assert.Equal("""{"Turn":{"Type":"Quaternion","Value":[0,0,0,1]}}""", Encoding.UTF8.GetString(runtime.Runtime.DescribeInstanceVariables(TestRuntime.Hero)!));
	}

	[Fact]
	public void SetInstanceVariablesAppliesOverridesToARunningInstance()
	{
		TestRuntime runtime = TestRuntime.Start(Variables());
		Assert.Equal(VisualScriptStatus.Success,
			runtime.Runtime.SetInstanceVariables(TestRuntime.Hero, Encoding.UTF8.GetBytes("""{ "Health": { "Type": "Int32", "Value": 1 } }""")));
		Assert.Equal(1, runtime.Variable("Lives"));
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.SetInstanceVariables(TestRuntime.Hero, []));
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Runtime.SetInstanceVariables(8, []));
		Assert.Empty(runtime.Environment.Logs);
	}
}

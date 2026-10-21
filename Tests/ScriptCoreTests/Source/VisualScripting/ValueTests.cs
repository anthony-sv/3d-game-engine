using System.Runtime.CompilerServices;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Values (spec sections 2 and 5.3): slots, the type defaults, quaternion normalization, enum names and entity literals.
public sealed class ValueTests
{
	[Fact]
	public void SlotsAreTwentyFourBytesWithTheReferenceAtSixteen()
	{
		Assert.Equal(24, Unsafe.SizeOf<Slot>());
		Slot slot = Slot.FromQuaternion(new Quaternion(1.0f, 2.0f, 3.0f, 4.0f));
		Assert.Equal(16, (int)Unsafe.ByteOffset(ref Unsafe.As<Slot, byte>(ref slot), ref Unsafe.As<object?, byte>(ref slot.Object)));
		Assert.Equal(new Quaternion(1.0f, 2.0f, 3.0f, 4.0f), slot.Quaternion);
		Assert.Null(slot.Object);
		Assert.Equal("", Slot.FromString(null).Object);
	}

	[Fact]
	public void VariablesWithoutAValueStartAtTheTypeDefault()
	{
		DocumentBuilder document = new();
		(string Type, object? Expected)[] variables =
		[
			("bool", false), ("int", 0), ("uint", 0u), ("long", 0L), ("ulong", 0UL), ("float", 0.0f), ("double", 0.0), ("string", ""),
			("Strada.Vector2", new Vector2(0.0f, 0.0f)), ("Strada.Vector3", new Vector3(0.0f, 0.0f, 0.0f)),
			("Strada.Vector4", new Vector4(0.0f, 0.0f, 0.0f, 0.0f)), ("Strada.Quaternion", new Quaternion(0.0f, 0.0f, 0.0f, 1.0f)),
			("Strada.Color", new Color(1.0f, 1.0f, 1.0f, 1.0f)), ("Strada.Matrix4", Matrix4.Identity), ("Strada.ForceMode", ForceMode.Force),
			("Strada.KeyCode", (KeyCode)0), ("Strada.AssetHandle", new AssetHandle(0)), ("Strada.Entity", null), ("Strada.Material", null),
			("Strada.Prefab", null), ("Strada.RigidBodyComponent", null), ("Strada.ScriptCore.Tests.Mover", null), ("Strada.ScriptCore.Tests.Holder", null),
			("Strada.RaycastHit", default(RaycastHit)), ("Strada.ScriptCore.Tests.Pair", default(Pair)), ("System.Object", null),
		];
		for (int index = 0; index < variables.Length; index++)
		{
			document.Variable("V" + index, variables[index].Type);
		}
		TestRuntime runtime = TestRuntime.Start(document);
		for (int index = 0; index < variables.Length; index++)
		{
			Assert.Equal(variables[index].Expected, runtime.Variable("V" + index));
		}
	}

	[Fact]
	public void OutputsOfImpureNodesThatNeverRanHoldTheTypeDefault()
	{
		DocumentBuilder document = new();
		uint turn = document.Variable("Turn", "Strada.Quaternion");
		GraphBuilder graph = document.Graph;
		uint set = graph.Set(turn, "[0.0, 1.0, 0.0, 0.0]");
		uint text = graph.Node("FormatString", """{ "Format": "{Value}" }""");
		graph.Link(set, "Value", text, "Value");
		graph.Flow(graph.Node("Event.Create"), graph.Write(text, "Result"));
		graph.Flow(graph.Node("Event.Update"), set);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["(0, 0, 0, 1)", "(0, 1, 0, 0)"], runtime.Records);
	}

	[Fact]
	public void QuaternionLiteralsAreNormalized()
	{
		DocumentBuilder document = new();
		document.Variable("Half", "Strada.Quaternion", "[0.0, 0.0, 3.0, 4.0]");
		document.Variable("Zero", "Strada.Quaternion", "[0.0, 0.0, 0.0, 0.0]");
		TestRuntime runtime = new();
		Diagnostic warning = Assert.Single(runtime.Load(document.Build()));
		Assert.Equal(DiagnosticCode.ZeroQuaternion, warning.Code);
		runtime.Create();
		Assert.Equal(new Quaternion(0.0f, 0.0f, 0.6f, 0.8f), runtime.Variable("Half"));
		Assert.Equal(Quaternion.Identity, runtime.Variable("Zero"));
	}

	[Fact]
	public void EnumLiteralsAreEnumeratorNames()
	{
		DocumentBuilder document = new();
		uint key = document.Variable("Key", "Strada.KeyCode", "\"Space\"");
		document.Variable("Unnamed", "Strada.ForceMode", "9");
		GraphBuilder graph = document.Graph;
		uint pressed = graph.Node("Event.KeyPressed", """{ "Key": "Space" }""");
		uint equal = graph.Node("Operator.Equal", inputs: """{ "B": "Space" }""");
		graph.Link(graph.Get(key), "Value", equal, "A");
		graph.Flow(pressed, graph.Write(equal, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);

		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(KeyCode.Space, runtime.Variable("Key"));
		Assert.Equal((ForceMode)9, runtime.Variable("Unnamed"));
		Assert.Equal(["True"], runtime.Records);
	}

	[Fact]
	public void EntityLiteralsAreNoneOnly()
	{
		DocumentBuilder document = new();
		uint partner = document.Variable("Partner", "Strada.Entity", "\"5\"");
		TestRuntime runtime = new();
		Diagnostic error = Assert.Single(runtime.Load(document.Build()));
		Assert.Equal(DiagnosticCode.EntityValue, error.Code);
		Assert.Equal(VisualScriptStatus.Failure, runtime.Create());
		Assert.Equal(System.FormattableString.Invariant($"{TestRuntime.Name} cannot run: 1 error, first: {error.Message} (variable {partner})"),
			runtime.SingleError());
		Assert.Null(runtime.Runtime.FindInstance(TestRuntime.Hero));
	}

	[Fact]
	public void BoxedStructsAreNeverChangedInPlace()
	{
		DocumentBuilder document = new();
		uint first = document.Variable("First", "Strada.ScriptCore.Tests.Pair");
		uint second = document.Variable("Second", "Strada.ScriptCore.Tests.Pair");
		GraphBuilder graph = document.Graph;
		uint make = graph.Node("Make", """{ "Type": "Strada.ScriptCore.Tests.Pair" }""", """{ "First": 1, "Second": 2 }""");
		uint setFirst = graph.Set(first);
		uint setSecond = graph.Set(second);
		uint changed = graph.Node("Make", """{ "Type": "Strada.ScriptCore.Tests.Pair" }""", """{ "Second": 9 }""");
		uint split = graph.Node("Break", """{ "Type": "Strada.ScriptCore.Tests.Pair" }""");
		uint setChanged = graph.Set(second);
		graph.Link(make, "Value", setFirst, "Value");
		graph.Link(graph.Get(first), "Value", setSecond, "Value");
		graph.Link(graph.Get(second), "Value", split, "Value");
		graph.Link(split, "First", changed, "First");
		graph.Link(changed, "Value", setChanged, "Value");
		graph.Flow(graph.Node("Event.Create"), setFirst);
		graph.Flow(setFirst, setSecond);
		graph.Flow(setSecond, setChanged);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(new Pair { First = 1, Second = 2 }, runtime.Variable("First"));
		Assert.Equal(new Pair { First = 1, Second = 9 }, runtime.Variable("Second"));
	}

	[Fact]
	public void ConstantsOfOperatorsAreStagedAtTheirJoinType()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 4000000000 }""");
		graph.Link(graph.Literal("uint", "1"), "Value", add, "A");
		graph.Flow(graph.Node("Event.Create"), graph.Write(add, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["4000000001"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}
}

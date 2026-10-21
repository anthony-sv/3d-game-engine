using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Reflected members (spec section 4.5) called through their compiled invokers: methods, getters, setters and constructors
// of static members, structs and classes, with out parameters, constants and generic methods.
public sealed class ReflectedNodeTests
{
	[Fact]
	public void StaticMethodsConstantsAndConstructors()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint clamp = graph.Node("Call:Strada.Mathf.Clamp(float,float,float)", inputs: """{ "Value": 5.0, "Min": 0.0, "Max": 3.0 }""");
		uint pi = graph.Node("Get:Strada.Mathf.PI");
		uint vector = graph.Node("New:Strada.Vector3(float,float,float)", inputs: """{ "X": 3.0, "Y": 4.0, "Z": 0.0 }""");
		uint length = graph.Node("Get:Strada.Vector3.Length");
		uint lerp = graph.Node("Call:Strada.Vector3.Lerp(Strada.Vector3,Strada.Vector3,float)", inputs: """{ "B": [2.0, 2.0, 2.0], "T": 0.5 }""");
		uint up = graph.Node("Get:Strada.Vector3.Up");
		uint layers = graph.Node("Get:Strada.Physics.AllLayers");
		uint text = graph.Node("FormatString", """{ "Format": "{Clamp} {Pi} {Length} {Lerp} {Layers}" }""");
		graph.Link(vector, "Value", length, "Target");
		graph.Link(up, "Value", lerp, "A");
		graph.Link(clamp, "Result", text, "Clamp");
		graph.Link(pi, "Value", text, "Pi");
		graph.Link(length, "Value", text, "Length");
		graph.Link(lerp, "Result", text, "Lerp");
		graph.Link(layers, "Value", text, "Layers");
		graph.Flow(graph.Node("Event.Create"), graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["3 3.1415927 5 (1, 1.5, 1) 4294967295"], runtime.Records);
	}

	[Fact]
	public void OutParametersBecomeOutputs()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint split = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Split(int,out int,out int)", inputs: """{ "Value": 7 }""");
		uint text = graph.Node("FormatString", """{ "Format": "{Half} {Rest} {Even}" }""");
		graph.Link(split, "Half", text, "Half");
		graph.Link(split, "Rest", text, "Rest");
		graph.Link(split, "Result", text, "Even");
		graph.Flow(graph.Node("Event.Create"), graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["3 4 False"], runtime.Records);
	}

	[Fact]
	public void ClassTargetsTakeObjectsOfTheirType()
	{
		DocumentBuilder document = new();
		uint holder = document.Variable("Holder", "Strada.ScriptCore.Tests.Holder");
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("New:Strada.ScriptCore.Tests.Holder()");
		uint store = graph.Set(holder);
		uint count = graph.Node("Set:Strada.ScriptCore.Tests.Holder.Count", inputs: """{ "Value": 20 }""");
		uint increment = graph.Node("Call:Strada.ScriptCore.Tests.Holder.Increment()");
		uint twice = graph.Node("Call:Strada.ScriptCore.Tests.Holder.Twice()");
		uint read = graph.Node("Get:Strada.ScriptCore.Tests.Holder.Count");
		uint text = graph.Node("FormatString", """{ "Format": "{Count} {Twice}" }""");
		graph.Flow(graph.Node("Event.Create"), create);
		graph.Flow(create, store);
		graph.Link(create, "Value", store, "Value");
		graph.Flow(store, count);
		graph.Link(graph.Get(holder), "Value", count, "Target");
		graph.Flow(count, increment);
		graph.Link(graph.Get(holder), "Value", increment, "Target");
		graph.Link(graph.Get(holder), "Value", twice, "Target");
		graph.Link(graph.Get(holder), "Value", read, "Target");
		graph.Link(read, "Value", text, "Count");
		graph.Link(twice, "Result", text, "Twice");
		graph.Flow(increment, graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["21 42"], runtime.Records);
		Assert.Equal(21, Assert.IsType<Holder>(runtime.Variable("Holder")).Count);
	}

	[Fact]
	public void GenericMethodsAreClosedOverTheirTypeArguments()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint mover = graph.Node("Call:Strada.Entity.As<T>()", """{ "TypeArguments": ["Strada.ScriptCore.Tests.Mover"] }""");
		graph.Flow(graph.Node("Event.Create"), graph.Write(mover, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		// No C# instance runs on Hero: the script registry has none.
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["None"], runtime.Records);
	}

	[Fact]
	public void UserConversionsAndOperatorMethodsAreInvoked()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint color = graph.Node("Convert", """{ "To": "Strada.Color" }""");
		graph.Link(graph.Literal("Strada.Vector4", "[0.25, 0.5, 0.75, 1.0]"), "Value", color, "Value");
		uint rotate = graph.Node("Operator.Multiply");
		graph.Link(graph.Node("Call:Strada.Quaternion.AngleAxis(float,Strada.Vector3)", inputs: """{ "Degrees": 180.0, "Axis": [0.0, 1.0, 0.0] }"""),
			"Result", rotate, "A");
		graph.Link(graph.Literal("Strada.Vector3", "[1.0, 0.0, 0.0]"), "Value", rotate, "B");
		uint round = graph.Node("Call:Strada.Mathf.Round(float)");
		uint x = graph.Node("Break", """{ "Type": "Strada.Vector3" }""");
		graph.Link(rotate, "Result", x, "Value");
		graph.Link(x, "X", round, "Value");
		uint text = graph.Node("FormatString", """{ "Format": "{Color} {X}" }""");
		graph.Link(color, "Result", text, "Color");
		graph.Link(round, "Result", text, "X");
		graph.Flow(graph.Node("Event.Create"), graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["(0.25, 0.5, 0.75, 1) -1"], runtime.Records);
	}
}

// Members reading process-wide statics of the API (spec section 5.5).
[Collection(ProcessStaticsCollection.Name)]
public sealed class ReflectedStaticsTests
{
	[Fact]
	public void SeededRandomNumbersRepeat()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint seed = graph.Node("Call:Strada.Random.Seed(int)", inputs: """{ "Seed": 1234 }""");
		uint range = graph.Node("Call:Strada.Random.Range(int,int)", inputs: """{ "Min": 0, "Max": 1000000 }""");
		graph.Flow(graph.Node("Event.Update"), seed);
		graph.Flow(seed, graph.Write(range, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		Random.Seed(1234);
		Assert.Equal([Random.Range(0, 1000000).ToString(System.Globalization.CultureInfo.InvariantCulture), runtime.Records[0]], runtime.Records);
	}

	[Fact]
	public void TimeDeltaTimeIsWhatTheEnvironmentSet()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		graph.Flow(graph.Node("Event.Update"), graph.Write(graph.Node("Get:Strada.Time.DeltaTime"), "Value"));
		TestRuntime runtime = TestRuntime.Start(document);
		float previous = Time.DeltaTime;
		try
		{
			Time.DeltaTime = 0.125f;
			runtime.Invoke(ScriptEvent.Update, 0.5f);
			Assert.Equal(["0.125"], runtime.Records);
			Assert.Equal(0.5f, runtime.Environment.DeltaTime);
		}
		finally
		{
			Time.DeltaTime = previous;
		}
	}
}

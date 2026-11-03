using System;
using System.Buffers;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Values (spec-slice4 section 5): the one value encoding with its caps, the currencies of a stop's values, a snapshot apart
// from what the rest of the frame does, values changed elsewhere while stopped, the last frames of watched functions, and the
// values of instances between frames.
public sealed class DebugValueTests
{
	private const ulong Player = 123;

	private static VisualType Type(Type type) => RuntimeTesting.Library.FindType(type) ?? throw new InvalidOperationException(type.FullName);

	// The members of a value's encoding, as an object.
	private static string Encode(object? value, VisualType type, FakeEnvironment? environment = null)
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer, LibraryWriter.CreateOptions()))
		{
			writer.WriteStartObject();
			new DebugValueWriter(writer, environment ?? new FakeEnvironment(), RuntimeTesting.Library).WriteMembers(SlotValues.FromValue(value, type), type);
			writer.WriteEndObject();
		}
		return Encoding.UTF8.GetString(buffer.WrittenSpan);
	}

	private static JsonElement Parse(string text)
	{
		using JsonDocument document = JsonDocument.Parse(text);
		return document.RootElement.Clone();
	}

	public static TheoryData<object?, Type, string> Literals => new()
	{
		{ true, typeof(bool), """{"Type":"bool","Value":true}""" },
		{ -3, typeof(int), """{"Type":"int","Value":-3}""" },
		{ 7u, typeof(uint), """{"Type":"uint","Value":7}""" },
		{ -9000000000L, typeof(long), """{"Type":"long","Value":"-9000000000"}""" },
		{ ulong.MaxValue, typeof(ulong), """{"Type":"ulong","Value":"18446744073709551615"}""" },
		{ 0.5f, typeof(float), """{"Type":"float","Value":0.5}""" },
		{ 2.25, typeof(double), """{"Type":"double","Value":2.25}""" },
		{ new Vector3(1.0f, 2.0f, 3.0f), typeof(Vector3), """{"Type":"Strada.Vector3","Value":[1,2,3]}""" },
		{ new Quaternion(0.0f, 0.0f, 0.0f, 1.0f), typeof(Quaternion), """{"Type":"Strada.Quaternion","Value":[0,0,0,1]}""" },
		{ new Color(1.0f, 0.5f, 0.25f, 1.0f), typeof(Color), """{"Type":"Strada.Color","Value":[1,0.5,0.25,1]}""" },
		{ ProbeAccess.Read | ProbeAccess.Write, typeof(ProbeAccess), """{"Type":"Strada.ScriptCore.Tests.ProbeAccess","Value":["Read","Write"]}""" },
		{ new AssetHandle(42), typeof(AssetHandle), """{"Type":"Strada.AssetHandle","Value":"42"}""" },
	};

	[Theory]
	[MemberData(nameof(Literals))]
	public void ValuesWithALiteralFormCrossAsTheirLiteral(object? value, Type type, string expected)
	{
		Assert.Equal(expected, Encode(value, Type(type)));
	}

	[Fact]
	public void MatricesAndEnumsCrossAsTheirLiterals()
	{
		JsonElement matrix = Parse(Encode(Matrix4.Identity, Type(typeof(Matrix4))));
		Assert.Equal(16, matrix.GetProperty("Value").GetArrayLength());
		Assert.Equal(1.0, matrix.GetProperty("Value")[0].GetDouble());
		Assert.Equal("""{"Type":"Strada.KeyCode","Value":"Space"}""", Encode(KeyCode.Space, Type(typeof(KeyCode))));
	}

	// JSON has no NaN or infinity: numbers and math structs that are not finite cross as their text.
	[Fact]
	public void NumbersThatAreNotFiniteCrossAsTheirText()
	{
		Assert.Equal("""{"Type":"float","Text":"NaN"}""", Encode(float.NaN, Type(typeof(float))));
		Assert.Equal("""{"Type":"double","Text":"-Infinity"}""", Encode(double.NegativeInfinity, Type(typeof(double))));
		Assert.Equal("""{"Type":"Strada.Vector3","Text":"(Infinity, 0, 1)"}""", Encode(new Vector3(float.PositiveInfinity, 0.0f, 1.0f), Type(typeof(Vector3))));
		Assert.Equal("""{"Type":"float[]","Value":[{"Value":1},{"Text":"NaN"}],"Length":2}""", Encode(new[] { 1.0f, float.NaN }, Type(typeof(float[]))));
	}

	// Strings are cut at 256 bytes of UTF-8 on a code point, with their whole length in UTF-16 units; lone surrogates cross
	// as U+FFFD.
	[Fact]
	public void StringsAreCutAtTheirCapOnACodePoint()
	{
		VisualType text = Type(typeof(string));
		Assert.Equal("""{"Type":"string","Value":"Serve"}""", Encode("Serve", text));
		JsonElement cut = Parse(Encode(new string('é', 200), text));
		Assert.Equal(new string('é', 128), cut.GetProperty("Value").GetString());
		Assert.Equal(200, cut.GetProperty("Length").GetInt32());
		Assert.True(cut.GetProperty("Truncated").GetBoolean());
		// A four-byte code point that does not fit whole is left out whole.
		JsonElement emoji = Parse(Encode(new string('a', 254) + "\U0001F600", text));
		Assert.Equal(new string('a', 254), emoji.GetProperty("Value").GetString());
		Assert.Equal(256, emoji.GetProperty("Length").GetInt32());
		Assert.Equal(new string('a', 256), Parse(Encode(new string('a', 256), text)).GetProperty("Value").GetString());
		Assert.Equal("""{"Type":"string","Value":"a�b"}""".Replace("\\uFFFD", "�", StringComparison.Ordinal), Encode("a\uD800b", text));
	}

	// Arrays cross their first 32 elements, each without its Type (strings inside cut at 64 bytes), and their whole length;
	// arrays nest two levels, deeper ones cross as their length.
	[Fact]
	public void ArraysCrossTheirFirstElements()
	{
		JsonElement numbers = Parse(Encode(Enumerable.Range(0, 40).ToArray(), Type(typeof(int[]))));
		Assert.Equal("int[]", numbers.GetProperty("Type").GetString());
		Assert.Equal(32, numbers.GetProperty("Value").GetArrayLength());
		Assert.Equal("""{"Value":31}""", numbers.GetProperty("Value")[31].GetRawText());
		Assert.Equal(40, numbers.GetProperty("Length").GetInt32());
		Assert.True(numbers.GetProperty("Truncated").GetBoolean());
		Assert.Equal("""{"Type":"int[]","Value":[{"Value":1},{"Value":2}],"Length":2}""", Encode(new[] { 1, 2 }, Type(typeof(int[]))));

		JsonElement words = Parse(Encode(new[] { new string('w', 100) }, Type(typeof(string[]))));
		JsonElement word = words.GetProperty("Value")[0];
		Assert.Equal(new string('w', 64), word.GetProperty("Value").GetString());
		Assert.Equal(100, word.GetProperty("Length").GetInt32());
		Assert.True(word.GetProperty("Truncated").GetBoolean());

		JsonElement entities = Parse(Encode(new Entity?[] { new(Player), null }, Type(typeof(Entity[]))));
		Assert.Equal("""[{"Entity":"123","Valid":false},{"Entity":"0"}]""", entities.GetProperty("Value").GetRawText());

		VisualType number = Type(typeof(int));
		VisualType row = new("int[]", "Int Array", TypeKind.Array, typeof(int[])) { Element = number };
		VisualType grid = new("int[][]", "Grid", TypeKind.Array, typeof(int[][])) { Element = row };
		VisualType cube = new("int[][][]", "Cube", TypeKind.Array, typeof(int[][][])) { Element = grid };
		Assert.Equal("""{"Type":"int[][]","Value":[{"Value":[{"Value":4}],"Length":1}],"Length":1}""", Encode(new[] { new[] { 4 } }, grid));
		Assert.Equal("""{"Type":"int[][][]","Value":[{"Value":[{"Length":3}],"Length":1}],"Length":1}""", Encode(new[] { new[] { new[] { 1, 2, 3 } } }, cube));
	}

	// Entities cross by ID ("0" for None; Valid false when the scene no longer has it); script instances and components also
	// by their text, naming their entity.
	[Fact]
	public void EntitiesComponentsAndScriptInstancesCrossByTheirEntity()
	{
		FakeEnvironment environment = new();
		environment.Add(Player, "Player");
		Mover mover = new();
		environment.Add(5678, "Player", mover);
		VisualType entity = Type(typeof(Entity));
		Assert.Equal("""{"Type":"Strada.Entity","Entity":"123"}""", Encode(new Entity(Player), entity, environment));
		Assert.Equal("""{"Type":"Strada.Entity","Entity":"0"}""", Encode(null, entity, environment));
		Assert.Equal("""{"Type":"Strada.Entity","Entity":"99","Valid":false}""", Encode(new Entity(99), entity, environment));
		Assert.Equal("""{"Type":"Strada.ScriptCore.Tests.Mover","Text":"Mover on Player (5678)","Entity":"5678"}""", Encode(mover, Type(typeof(Mover)), environment));

		VisualType rigidBody = Type(typeof(RigidBodyComponent));
		RigidBodyComponent component = (RigidBodyComponent)Activator.CreateInstance(typeof(RigidBodyComponent), nonPublic: true)!;
		component.Entity = new Entity(Player);
		Assert.Equal("""{"Type":"Strada.RigidBodyComponent","Text":"Rigid Body of Player (123)","Entity":"123"}""", Encode(component, rigidBody, environment));
		Assert.Equal("""{"Type":"Strada.RigidBodyComponent","Value":null}""", Encode(null, rigidBody, environment));
	}

	private sealed class Chatty(string text)
	{
		public override string ToString() => text;
	}

	private sealed class Faulty
	{
		public override string ToString() => throw new InvalidOperationException("no text");
	}

	// Other objects cross as their invariant ToString, cut at 256 bytes; what a ToString throws is its text; null is a null
	// Value.
	[Fact]
	public void OtherObjectsCrossAsTheirText()
	{
		VisualType holder = Type(typeof(Holder));
		Assert.Equal("""{"Type":"Strada.ScriptCore.Tests.Holder","Value":null}""", Encode(null, holder));
		Assert.Equal("""{"Type":"Strada.ScriptCore.Tests.Holder","Text":"Strada.ScriptCore.Tests.Holder"}""", Encode(new Holder(), holder));
		VisualType any = new("Chatty", "Chatty", TypeKind.Class, typeof(Chatty));
		Assert.Equal(new string('x', 256), Parse(Encode(new Chatty(new string('x', 300)), any)).GetProperty("Text").GetString());
		Assert.Equal("""{"Type":"Faulty","Text":"Faulty (its ToString threw InvalidOperationException)"}""",
			Encode(new Faulty(), new VisualType("Faulty", "Faulty", TypeKind.Class, typeof(Faulty))));
		Assert.Equal("""{"Type":"Strada.ScriptCore.Tests.Pair","Text":"Strada.ScriptCore.Tests.Pair"}""", Encode(new Pair { First = 1 }, Type(typeof(Pair))));
	}

	// Create sets Created from a Next; Update sets Last from a Next, then Total = Created + Last's output at a breakpoint.
	private static (TestRuntime Runtime, DebugSession Session, uint Set, uint Add, uint Updated) Currencies()
	{
		DocumentBuilder document = new();
		uint total = document.Variable("Total", "int", "0");
		uint created = document.Variable("Created", "int", "0");
		uint last = document.Variable("Last", "int", "0");
		GraphBuilder graph = document.Graph;
		uint keep = graph.Set(created);
		graph.Link(graph.Node(RuntimeTesting.Next), "Result", keep, "Value");
		graph.Flow(graph.Node("Event.Create"), keep);
		uint updated = graph.Set(last);
		graph.Link(graph.Node(RuntimeTesting.Next), "Result", updated, "Value");
		uint add = graph.Node("Operator.Add");
		graph.Link(graph.Get(created), "Value", add, "A");
		graph.Link(updated, "Value", add, "B");
		uint set = graph.Set(total);
		graph.Link(add, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Update"), updated);
		graph.Flow(updated, set);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(set);
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		return (runtime, session, set, add, updated);
	}

	// A value is current when the stopped execution computed it, earlier when another execution of the instance did, not
	// computed when none did in a frame that has stamps; an input of the stopped node is its source's value at an Enter
	// position.
	[Fact]
	public void StampsGiveEveryValueItsCurrency()
	{
		(TestRuntime runtime, DebugSession session, uint set, uint add, uint updated) = Currencies();
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		JsonElement first = session.Describe(1);
		Assert.Equal(set, first.GetProperty("Node").GetProperty("ID").GetUInt32());
		JsonElement value = first.GetProperty("Node").GetProperty("Inputs")[0];
		Assert.Equal("Value", DebugSession.Text(value, "Name"));
		Assert.Equal("NotComputed", DebugSession.Text(value, "Currency"));
		Assert.Equal("NotComputed", DebugSession.Text(first.GetProperty("Node").GetProperty("Outputs")[0], "Currency"));
		// Last's Set and the Next it read (a pure node its plan evaluated).
		JsonElement[] computed = [.. first.GetProperty("Computed").EnumerateArray()];
		Assert.Equal(2, computed.Length);
		JsonElement next = computed.Single(node => node.GetProperty("Node").GetUInt32() == updated).GetProperty("Outputs")[0];
		Assert.Equal("""{"Name":"Value","Type":"int","Value":2,"Currency":"Current"}""", next.GetRawText());
		JsonElement delta = Assert.Single(first.GetProperty("Event").EnumerateArray());
		Assert.Equal("""{"Name":"DeltaTime","Type":"float","Value":0.25,"Currency":"Current"}""", delta.GetRawText());
		Assert.Equal(["Total", "Created", "Last"], first.GetProperty("Variables").EnumerateArray().Select(static variable => DebugSession.Text(variable, "Name")));
		Assert.Equal("""{"Name":"Created","Type":"int","Value":1,"Currency":"Current","ID":2,"Public":false}""", first.GetProperty("Variables")[1].GetRawText());

		session.Continue();
		Assert.Equal(3, runtime.Variable("Total"));
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		JsonElement second = session.Describe(2);
		// The sum and the Set's output come from the Update before: computed earlier, by another execution.
		JsonElement earlier = second.GetProperty("Node").GetProperty("Inputs")[0];
		Assert.Equal("Earlier", DebugSession.Text(earlier, "Currency"));
		Assert.Equal(3, earlier.GetProperty("Value").GetInt32());
		Assert.Equal("Earlier", DebugSession.Text(second.GetProperty("Node").GetProperty("Outputs")[0], "Currency"));
		Assert.DoesNotContain(second.GetProperty("Computed").EnumerateArray(), node => node.GetProperty("Node").GetUInt32() == add);
		Assert.Equal(0, second.GetProperty("Truncated").GetInt32());
	}

	// What the rest of the frame does after a stop never reaches its snapshot; when the stop continues, what changed meanwhile
	// is listed by its next stop: variables by name, event outputs by node and pin, the chain's own writes never.
	[Fact]
	public void ASnapshotKeepsItsValuesAndTheNextStopListsWhatChangedElsewhere()
	{
		DocumentBuilder document = new();
		uint points = document.Variable("Points", "int", "0", isPublic: true);
		uint own = document.Variable("Own", "int", "0");
		GraphBuilder graph = document.Graph;
		uint hit = graph.Node("Event.Custom", """{ "Name": "Hit", "Parameters": [{ "Name": "Row", "Type": "int" }] }""");
		uint first = graph.Write("hit");
		uint write = graph.Set(own, "5");
		uint second = graph.Write("again");
		graph.Flow(hit, first);
		graph.Flow(first, write);
		graph.Flow(write, second);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(first, second);
		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [3]));
		// The next delivery overwrites Row and stops as well; C# sets Points.
		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [4]));
		runtime.Runtime.SetVariable(TestRuntime.Hero, "Points", typeof(int), 9);
		Assert.Equal(2, session.Stops().Length);

		JsonElement snapshot = session.Describe(1);
		Assert.Equal(3, snapshot.GetProperty("Event")[0].GetProperty("Value").GetInt32());
		Assert.Equal(0, snapshot.GetProperty("Variables")[0].GetProperty("Value").GetInt32());
		Assert.False(snapshot.GetProperty("Event")[0].TryGetProperty("Changed", out _));

		JsonElement resumed = session.Continue(1);
		Assert.Equal("Stopped", DebugSession.Text(resumed, "Outcome"));
		JsonElement stop = session.Stops().Single(stop => DebugSession.IdOf(stop) == resumed.GetProperty("Stop").GetInt32());
		Assert.Equal(FormattableString.Invariant($$"""[{"Node":0,"Name":"Points"},{"Node":{{hit}},"Name":"Row"}]"""), stop.GetProperty("Changed").GetRawText());
		Assert.Equal(0, stop.GetProperty("ChangedMore").GetInt32());
		JsonElement described = session.Describe(DebugSession.IdOf(stop));
		JsonElement row = described.GetProperty("Event")[0];
		Assert.Equal(4, row.GetProperty("Value").GetInt32());
		Assert.True(row.GetProperty("Changed").GetBoolean());
		JsonElement[] variables = [.. described.GetProperty("Variables").EnumerateArray()];
		Assert.True(variables[0].GetProperty("Changed").GetBoolean());
		Assert.Equal(points, variables[0].GetProperty("ID").GetUInt32());
		Assert.True(variables[0].GetProperty("Public").GetBoolean());
		// Own was written by the chain itself.
		Assert.Equal(5, variables[1].GetProperty("Value").GetInt32());
		Assert.False(variables[1].TryGetProperty("Changed", out _));
		Assert.Equal(own, variables[1].GetProperty("ID").GetUInt32());
	}

	// At most 64 changes are listed; ChangedMore counts the others.
	[Fact]
	public void ChangesBeyondTheCapAreCounted()
	{
		DocumentBuilder document = new();
		for (int index = 0; index < 70; index++)
		{
			document.Variable(string.Create(CultureInfo.InvariantCulture, $"V{index}"), "int", "0", isPublic: true);
		}
		GraphBuilder graph = document.Graph;
		uint first = graph.Write("first");
		uint second = graph.Write("second");
		graph.Flow(graph.Node("Event.Update"), first);
		graph.Flow(first, second);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(first, second);
		runtime.Invoke(ScriptEvent.Update);
		for (int index = 0; index < 70; index++)
		{
			runtime.Runtime.SetVariable(TestRuntime.Hero, string.Create(CultureInfo.InvariantCulture, $"V{index}"), typeof(int), index + 1);
		}

		session.Continue();
		JsonElement stop = session.SingleStop();
		Assert.Equal(VisualScriptDebugger.MaxChanges, stop.GetProperty("Changed").GetArrayLength());
		Assert.Equal(6, stop.GetProperty("ChangedMore").GetInt32());
	}

	// A watched function's call frame is copied as it returns: Values reads its last call, labelled with its entity; an
	// unwatched function has none.
	[Fact]
	public void WatchedFunctionFramesAreCapturedAtTheirReturn()
	{
		DocumentBuilder document = new();
		FunctionBuilder doubled = document.Function("Doubled", inputs: ["Amount:int"], outputs: ["Twice:int"]);
		uint multiply = doubled.Graph.Node("Operator.Multiply", inputs: """{ "B": 2 }""");
		doubled.Graph.Link(doubled.Entry, "Amount", multiply, "A");
		uint returned = doubled.Graph.Node("Function.Return");
		doubled.Graph.Link(multiply, "Result", returned, "Twice");
		doubled.Graph.Flow(doubled.Entry, returned);
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", doubled.Settings, """{ "Amount": 21 }""");
		graph.Flow(graph.Node("Event.Update"), call);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(8, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));
		DebugSession session = new(runtime);
		string values = FormattableString.Invariant($$"""{ "Command": "Values", "Entity": "{{TestRuntime.Hero}}", "Function": {{doubled.Id}}, "Nodes": [{{multiply}}, 999] }""");
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal("None", DebugSession.Text(session.Expect(values), "Source"));

		session.Configure(watches: [FormattableString.Invariant($$"""{ "Document": "{{TestRuntime.Graph}}", "Function": {{doubled.Id}} }""")]);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update, entity: 8);
		JsonElement reply = session.Expect(values);
		Assert.Equal("LastCall", DebugSession.Text(reply, "Source"));
		Assert.Equal("8", DebugSession.Text(reply, "Entity"));
		JsonElement node = Assert.Single(reply.GetProperty("Nodes").EnumerateArray());
		Assert.Equal(FormattableString.Invariant($$"""{"ID":{{multiply}},"Outputs":[{"Name":"Result","Type":"int","Value":42}]}"""), node.GetRawText());
		Assert.Equal(0L, runtime.FewestBytesPerUpdate(updates: 20));
		// Unwatched, the function's last frame is gone.
		session.Configure();
		Assert.Equal("None", DebugSession.Text(session.Expect(values), "Source"));
	}

	// Values reads an instance between frames without a debugger: its event graph's frame, every variable (public and
	// private) and its timelines.
	[Fact]
	public void ValuesReadTheEventFrameEveryVariableAndTheTimelines()
	{
		DocumentBuilder document = new();
		document.Variable("Speed", "float", "2.5", isPublic: true);
		document.Variable("Secret", "string", "\"hidden\"");
		uint seen = document.Variable("Seen", "int", "0");
		GraphBuilder graph = document.Graph;
		uint timeline = graph.Node("Flow.Timeline", """{ "Timeline": { "Length": 2 } }""");
		graph.Flow(graph.Node("Event.Create"), timeline, "Then", "PlayFromStart");
		uint next = graph.Set(seen);
		graph.Link(graph.Node(RuntimeTesting.Next), "Result", next, "Value");
		graph.Flow(graph.Node("Event.Update"), next);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		DebugSession session = new(runtime);
		session.Send("""{ "Command": "Detach" }""");

		JsonElement reply = session.Expect(FormattableString.Invariant(
			$$"""{ "Command": "Values", "Entity": "{{TestRuntime.Hero}}", "Nodes": [{{next}}], "Variables": true, "Timelines": true }"""));
		Assert.Equal(["Entity", "Graph", "Source", "Variables", "Timelines", "Nodes", "Truncated"], reply.EnumerateObject().Select(static member => member.Name));
		Assert.Equal("Frame", DebugSession.Text(reply, "Source"));
		Assert.Equal("100", DebugSession.Text(reply, "Graph"));
		Assert.Equal("""[{"Name":"Speed","Type":"float","Value":2.5,"ID":1,"Public":true},{"Name":"Secret","Type":"string","Value":"hidden","ID":2,"Public":false},{"Name":"Seen","Type":"int","Value":1,"ID":3,"Public":false}]""",
			reply.GetProperty("Variables").GetRawText());
		Assert.Equal(FormattableString.Invariant($$"""[{"Node":{{timeline}},"Time":0.5,"Length":2,"Playing":true,"Reverse":false}]"""), reply.GetProperty("Timelines").GetRawText());
		Assert.Equal(FormattableString.Invariant($$"""[{"ID":{{next}},"Outputs":[{"Name":"Value","Type":"int","Value":1}]}]"""), reply.GetProperty("Nodes").GetRawText());

		JsonElement bare = session.Expect(FormattableString.Invariant($$"""{ "Command": "Values", "Entity": "{{TestRuntime.Hero}}" }"""));
		Assert.Empty(bare.GetProperty("Variables").EnumerateArray());
		Assert.Empty(bare.GetProperty("Timelines").EnumerateArray());
		Assert.Equal(("NotFound", "Entity: no instance runs on the entity"), session.Refused("""{ "Command": "Values", "Entity": "99" }"""));
		Assert.Equal(("NotFound", "Function: the graph has no function 77"), session.Refused(FormattableString.Invariant(
			$$"""{ "Command": "Values", "Entity": "{{TestRuntime.Hero}}", "Function": 77 }""")));
	}

	// A linked input that converts to text calls the source value's ToString, which is game code: at an Enter stop, Describe
	// shows it unconverted when it throws, and answers (spec-slice4 sections 5.3 and 7.5: the protocol never throws).
	[Fact]
	public void DescribeShowsAnInputWhoseConversionThrowsUnconverted()
	{
		NodeLibrary library = NodeLibraryBuilder.Build(new NodeLibrarySources
		{
			ApiTypes = [.. NodeLibrarySources.GetApiTypes(), typeof(Probe), typeof(Brittle)],
		});
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint make = graph.Node("Call:Strada.ScriptCore.Tests.Brittle.Make()");
		uint write = graph.Write(make, "Result");
		graph.Flow(graph.Node("Event.Create"), make);
		graph.Flow(make, write);
		TestRuntime runtime = new(() => library);
		runtime.LoadWorking(document.Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		DebugSession session = new(runtime);
		session.Break(write);
		runtime.Invoke(ScriptEvent.Create);
		JsonElement stop = session.SingleStop();
		Assert.Equal("Enter", DebugSession.Text(stop, "Place"));

		JsonElement input = Assert.Single(session.Describe(DebugSession.IdOf(stop)).GetProperty("Node").GetProperty("Inputs").EnumerateArray());
		Assert.Equal("Text", DebugSession.Text(input, "Name"));
		Assert.Equal("Strada.ScriptCore.Tests.Brittle", DebugSession.Text(input, "Type"));
		Assert.Equal("Brittle (its ToString threw InvalidOperationException)", DebugSession.Text(input, "Text"));
		Assert.Equal("Current", DebugSession.Text(input, "Currency"));
	}
}

// A class whose ToString throws, as game code may: what the debugger shows of it must not throw.
public sealed class Brittle
{
	public static Brittle Make() => new();

	public override string ToString() => throw new InvalidOperationException("no text");
}

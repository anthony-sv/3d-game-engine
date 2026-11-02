using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.Loader;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The library of a game whose public types are those of the namespace TestGame (VisualNodeProbes.cs), with the API, the
// probes graphs write through and the game's script class Player.
internal static class GameTesting
{
	internal const ulong Villain = 8;

	private static readonly Lazy<NodeLibrary> s_Library = new(static () => NodeLibraryBuilder.Build(Sources()));

	internal static IReadOnlyList<Type> GameTypes { get; } =
		[.. typeof(TestGame.Probe).Assembly.GetExportedTypes().Where(static type => type.Namespace == "TestGame").OrderBy(static type => type.FullName, StringComparer.Ordinal)];

	internal static NodeLibrary Library => s_Library.Value;

	internal static NodeLibrarySources Sources(params Type[] moreGameTypes) => new()
	{
		ApiTypes = [.. NodeLibrarySources.GetApiTypes(), typeof(Probe)],
		ScriptClasses = [typeof(TestGame.Player)],
		GameTypes = [.. GameTypes, .. moreGameTypes],
	};

	internal static MemberNodeDefinition Node(string type) =>
		Library.FindNode(type) as MemberNodeDefinition ?? throw new InvalidOperationException($"the game library has no member node {type}");

	// Hero runs the graph with a Player script, Villain with a Jumper script.
	internal static (VisualScriptRuntime Runtime, FakeEnvironment Environment) Start(string document)
	{
		Probe.Reset();
		TestGame.Probe.Reset();
		FakeEnvironment environment = new();
		environment.Add(TestRuntime.Hero, "Hero", new TestGame.Player());
		environment.Add(Villain, "Villain", new Jumper());
		VisualScriptRuntime runtime = new(environment, () => Library);
		IReadOnlyList<Diagnostic> errors = [.. runtime.Load(TestRuntime.Graph, TestRuntime.Name, System.Text.Encoding.UTF8.GetBytes(document))
			.Where(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error)];
		Assert.True(errors.Count == 0, VisualScriptTesting.Describe(errors));
		return (runtime, environment);
	}
}

// [VisualNode] (spec-slice3 section 3.7): the game's marked members are nodes of every member kind, with what their
// attributes give; marked types and the types marked members use are types of graphs; marked members without a node form
// are notes; former names redirect; instance members of script classes run on the Target entity's script.
public sealed class VisualNodeTests
{
	private static string Pins(NodeDefinition node) =>
		string.Join(", ", node.Pins.Select(static pin => $"{pin.Direction} {pin.Name}: {pin.Type.Id}{(pin.DefaultsToSelf ? " (Self)" : "")}"));

	[Fact]
	public void MembersOfEveryKindAreNodes()
	{
		MemberNodeDefinition add = GameTesting.Node("Call:TestGame.Probe.Add(int,int)");
		Assert.Equal(("Add", "Probe", "Game/Probe", NodeKind.Call, true), (add.Title, add.Subtitle, add.Category, add.Kind, add.Game));
		Assert.Equal("In Execute: exec, In A: int, In B: int, Out Then: exec, Out Result: int", Pins(add));
		Assert.Equal(NodeKind.Pure, GameTesting.Node("Call:TestGame.Probe.Twice(int)").Kind);
		Assert.Equal(NodeKind.Call, GameTesting.Node("Call:TestGame.Probe.Nothing()").Kind);

		Assert.Equal(("Get Counter", NodeKind.Pure), (GameTesting.Node("Get:TestGame.Probe.Counter").Title, GameTesting.Node("Get:TestGame.Probe.Counter").Kind));
		Assert.Equal(("Set Counter", NodeKind.Call), (GameTesting.Node("Set:TestGame.Probe.Counter").Title, GameTesting.Node("Set:TestGame.Probe.Counter").Kind));
		Assert.Equal("Out Value: float", Pins(GameTesting.Node("Get:TestGame.Probe.Gravity")));
		Assert.NotNull(GameTesting.Library.FindNode("Set:TestGame.Probe.Gravity"));
		Assert.NotNull(GameTesting.Library.FindNode("Get:TestGame.Probe.Limit"));
		Assert.Null(GameTesting.Library.FindNode("Set:TestGame.Probe.Limit"));
		Assert.Null(GameTesting.Library.FindNode("Get:TestGame.Probe.Unmarked"));

		// The type's attribute marks every public member with a node form: the constructor, the fields (no setters on a
		// struct's instance fields) and the method; Fill has a ref parameter and is skipped without a note.
		MemberNodeDefinition make = GameTesting.Node("New:TestGame.Damage(float,TestGame.Element)");
		Assert.Equal(("Make Damage (Amount, Element)", "Game/Rules", NodeKind.Pure), (make.Title, make.Category, make.Kind));
		Assert.Equal("In Amount: float, In Element: TestGame.Element, Out Value: TestGame.Damage", Pins(make));
		Assert.NotNull(GameTesting.Library.FindNode("Get:TestGame.Damage.Element"));
		Assert.Null(GameTesting.Library.FindNode("Set:TestGame.Damage.Amount"));
		MemberNodeDefinition doubled = GameTesting.Node("Call:TestGame.Damage.Doubled()");
		Assert.Equal(("Target is Damage", TargetKind.Value), (doubled.Subtitle, doubled.Target));
		Assert.Equal("In Execute: exec, In Target: TestGame.Damage, Out Then: exec, Out Result: float", Pins(doubled));
		Assert.Null(GameTesting.Library.FindNode("Call:TestGame.Damage.Fill(float)"));

		// A member of an unmarked class: its type is a type of graphs because the member's signature uses it.
		Assert.Equal("In Execute: exec, In Target: TestGame.Inventory, In Value: int, Out Then: exec",
			Pins(GameTesting.Node("Set:TestGame.Inventory.Count")));

		// Instance members of script classes take the entity whose script they run on.
		MemberNodeDefinition apply = GameTesting.Node("Call:TestGame.Player.ApplyDamage(TestGame.Damage)");
		Assert.Equal(("Apply Damage", "Target is Player", "Game/Player", TargetKind.Script), (apply.Title, apply.Subtitle, apply.Category, apply.Target));
		Assert.Equal("In Execute: exec, In Target: Strada.Entity (Self), In Damage: TestGame.Damage, Out Then: exec", Pins(apply));
		Assert.Equal(TargetKind.Script, GameTesting.Node("Get:TestGame.Player.Health").Target);
		Assert.Null(GameTesting.Library.FindNode("New:TestGame.Player()"));

		// Members without a node form have none.
		foreach (string type in (string[])["Call:TestGame.Probe.Swap(int,int)", "Call:TestGame.Probe.Wait()", "Call:TestGame.Probe.Hidden()",
			"Call:TestGame.Probe.Twins(int,int)", "Call:TestGame.Probe.Open(TestGame.Window)"])
		{
			Assert.Null(GameTesting.Library.FindNode(type));
		}

		// Only game members are flagged, in the library JSON too.
		Assert.False(GameTesting.Library.FindNode("Call:Strada.Mathf.Abs(float)")!.Game);
		using JsonDocument library = JsonDocument.Parse(LibraryWriter.Write(GameTesting.Library));
		JsonElement[] nodes = [.. library.RootElement.GetProperty("Nodes").EnumerateArray()];
		JsonElement addEntry = nodes.Single(static node => node.GetProperty("Type").GetString() == "Call:TestGame.Probe.Add(int,int)");
		Assert.True(addEntry.GetProperty("Game").GetBoolean());
		Assert.Equal(["Latent", "Game", "Pins"], addEntry.EnumerateObject().Select(static member => member.Name).SkipWhile(static name => name != "Latent").Take(3));
		Assert.False(nodes.Single(static node => node.GetProperty("Type").GetString() == "Call:Strada.Mathf.Abs(float)").TryGetProperty("Game", out _));
	}

	[Fact]
	public void TheAttributeGivesTitlesCategoriesDescriptionsAndKeywords()
	{
		MemberNodeDefinition strike = GameTesting.Node("Call:TestGame.Probe.Strike(TestGame.Damage)");
		Assert.Equal(("Deal Damage", "Game/Combat", "Hurts whatever it hits."), (strike.Title, strike.Category, strike.Description));
		Assert.Equal(["hit", "hurt"], strike.Keywords);
		// Values outside the limits are not used (the notes say so).
		Assert.Equal("Long", GameTesting.Node("Call:TestGame.Probe.Long()").Title);
		Assert.Equal("Game/Probe", GameTesting.Node("Call:TestGame.Probe.Lost()").Category);
		Assert.Equal(["fine"], GameTesting.Node("Call:TestGame.Probe.Findable()").Keywords);
		Assert.Empty(GameTesting.Node("Call:TestGame.Probe.Add(int,int)").Keywords);
		// A game member's node has no description unless its attribute gives one.
		Assert.Equal(string.Empty, GameTesting.Node("Call:TestGame.Probe.Add(int,int)").Description);
	}

	[Fact]
	public void MarkedMembersWithoutANodeAreNotes()
	{
		string[] expected =
		[
			"TestGame.Player | Game/Player | TestGame.Player: script classes are created by components, not by New",
			"TestGame.Probe.Findable | Game/Probe | TestGame.Probe.Findable: the keyword \"\" must have 1 to 64 characters without control characters: it is left out",
			"TestGame.Probe.Hidden | Game/Probe | TestGame.Probe.Hidden: [VisualNode] members must be public",
			"TestGame.Probe.Long | Game/Probe | TestGame.Probe.Long: the Title must have 1 to 128 characters without control characters: the node is titled by the member's name",
			"TestGame.Probe.Lost | Game/Probe | TestGame.Probe.Lost: the Category \"Game//Empty\" is not 1 to 8 names of 1 to 64 characters separated by '/': the nodes are listed in Game/Probe",
			"TestGame.Probe.Measure | Game/Probe | TestGame.Probe.Measure: System.Span<int> is a ref struct: graphs cannot hold its values",
			"TestGame.Probe.Nothing | Game/Probe | TestGame.Probe.Nothing: Pure needs a result or out parameters: the node is impure",
			"TestGame.Probe.Open | Game/Probe | TestGame.Probe.Open: TestGame.Window is a ref struct: graphs cannot hold its values",
			"TestGame.Probe.Sum | Game/Probe | TestGame.Probe.Sum: the former name Add is the type of another node: it is ignored",
			"TestGame.Probe.Swap | Game/Probe | TestGame.Probe.Swap: [VisualNode] member has no node form: ref parameters have no pins",
			"TestGame.Probe.Twins | Game/Probe | TestGame.Probe.Twins: two pins would be named ExecuteParameter",
			"TestGame.Probe.Wait | Game/Probe | TestGame.Probe.Wait: System.Threading.Tasks.Task is not a type of graphs",
			"TestGame.Renamed.Current | Game/Renamed | TestGame.Renamed.Current: the former name \"Old Name\" is not a name of a member or type: it is ignored",
			"TestGame.Window | Game/Window | TestGame.Window is a ref struct: graphs cannot hold its values",
		];
		Assert.Equal(expected, GameTesting.Library.Notes.Select(static note => $"{note.Member} | {note.Category} | {note.Message}"));
	}

	// The library JSON lists the notes after the nodes, each in the form the engine's reader pins (spec-slice3 section 2.3).
	[Fact]
	public void TheNotesAreInTheLibraryJson()
	{
		const string noteText =
			"""{"Member":"TestGame.Probe.Swap","Category":"Game/Probe","Message":"TestGame.Probe.Swap: [VisualNode] member has no node form: ref parameters have no pins"}""";
		using JsonDocument library = JsonDocument.Parse(LibraryWriter.Write(GameTesting.Library));
		Assert.Equal(["Types", "Nodes", "Notes"], library.RootElement.EnumerateObject().Select(static member => member.Name));
		JsonElement[] notes = [.. library.RootElement.GetProperty("Notes").EnumerateArray()];
		Assert.Equal(GameTesting.Library.Notes.Count, notes.Length);
		Assert.Contains(noteText, notes.Select(static note => note.GetRawText()));

		// A library without notes has no Notes member.
		using JsonDocument plain = JsonDocument.Parse(LibraryWriter.Write(VisualScriptTesting.Library));
		Assert.False(plain.RootElement.TryGetProperty("Notes", out _));
	}

	[Fact]
	public void GameEnumsAndStructsAreTypesOfGraphs()
	{
		VisualType element = GameTesting.Library.FindType("TestGame.Element")!;
		Assert.Equal((TypeKind.Enum, ScriptFieldType.Int32, "Element"), (element.Kind, element.FieldType, element.Title));
		Assert.Equal(["Fire", "Water", "Earth"], element.Enumerators.Select(static enumerator => enumerator.Name));
		VisualType damage = GameTesting.Library.FindType("TestGame.Damage")!;
		Assert.Equal((TypeKind.Struct, true, true), (damage.Kind, damage.Makeable, damage.Breakable));
		Assert.Equal(["Amount: float", "Element: TestGame.Element"], damage.Fields.Select(static field => $"{field.Name}: {field.Type.Id}"));
		Assert.Equal(TypeKind.Class, GameTesting.Library.FindType("TestGame.Inventory")!.Kind);
		Assert.NotNull(GameTesting.Library.FindType("TestGame.Element[]"));
		Assert.Null(GameTesting.Library.FindType("TestGame.Window"));
		// Types no marked member uses are not types of graphs.
		Assert.Null(GameTesting.Library.FindType("TestGame.Probe"));

		// A public variable of a game enum is exposable; one switches on it.
		AnalyzedDocument document = VisualScriptTesting.Analyze(GameTesting.Library, VisualScriptTesting.Document(
			"""{ "ID": 2, "Type": "Event.Create" }, { "ID": 3, "Type": "Variable.Get", "Settings": { "Variable": 1 } }, { "ID": 4, "Type": "Flow.Switch", "Settings": { "Type": "TestGame.Element" } }""",
			"""{ "From": [2, "Then"], "To": [4, "Execute"] }, { "From": [3, "Value"], "To": [4, "Selection"] }""",
			"""{ "ID": 1, "Name": "Kind", "Type": "TestGame.Element", "Value": "Water", "Public": true }"""));
		VisualScriptTesting.AssertNoErrors(document);
		Assert.Empty(document.Diagnostics);
		Assert.Equal(["Case:Fire", "Case:Water", "Case:Earth", "Default"],
			VisualScriptTesting.Node(document, 4).Pins.Where(static pin => !pin.IsInput).Select(static pin => pin.Name));
	}

	[Fact]
	public void FormerNamesRedirectToTheMember()
	{
		foreach (string former in (string[])["Call:TestGame.Probe.Cure(int)", "Call:TestGame.Healer.Mend(int)"])
		{
			AnalyzedDocument document = VisualScriptTesting.Analyze(GameTesting.Library, VisualScriptTesting.Document(
				$$"""{ "ID": 1, "Type": "Event.Create" }, { "ID": 2, "Type": "{{former}}", "Inputs": { "Amount": 4 } }""",
				"""{ "From": [1, "Then"], "To": [2, "Execute"] }"""));
			VisualScriptTesting.AssertNoErrors(document);
			AnalyzedNode node = VisualScriptTesting.Node(document, 2);
			Assert.Equal((NodeResolution.Redirect, "Call:TestGame.Probe.Heal(int)"), (node.Resolution, node.CanonicalType));
			Assert.Equal($"node updated: {former} → Call:TestGame.Probe.Heal(int)", VisualScriptTesting.Single(document, DiagnosticCode.NodeUpdated).Message);
		}
		// A former name another node has is ignored: the stored Add stays Add.
		Assert.Null(GameTesting.Library.FindRedirect("Call:TestGame.Probe.Add(int,int)"));
		Assert.Equal(["Call:TestGame.Healer.Mend(int)", "Call:TestGame.Probe.Cure(int)"],
			new[] { "Call:TestGame.Probe.Cure(int)", "Call:TestGame.Healer.Mend(int)", "Call:TestGame.Renamed.Old Name()" }
				.Where(static type => GameTesting.Library.FindRedirect(type) != null).Order(StringComparer.Ordinal));
	}

	// A member whose parameter types changed resolves to its only overload; a removed one is unknown, with suggestions.
	[Fact]
	public void ReSignedMembersResolveAndRemovedOnesAreUnknown()
	{
		AnalyzedDocument resigned = VisualScriptTesting.Analyze(GameTesting.Library, VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Event.Create" }, { "ID": 2, "Type": "Call:TestGame.Probe.Heal(float)", "Inputs": { "Amount": 4 } }""",
			"""{ "From": [1, "Then"], "To": [2, "Execute"] }"""));
		VisualScriptTesting.AssertNoErrors(resigned);
		Assert.Equal((NodeResolution.Unique, "Call:TestGame.Probe.Heal(int)"),
			(VisualScriptTesting.Node(resigned, 2).Resolution, VisualScriptTesting.Node(resigned, 2).CanonicalType));

		AnalyzedDocument removed = VisualScriptTesting.Analyze(GameTesting.Library, VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Event.Create" }, { "ID": 2, "Type": "Call:TestGame.Probe.Heals(int)" }""",
			"""{ "From": [1, "Then"], "To": [2, "Execute"] }"""));
		Diagnostic unknown = VisualScriptTesting.Single(removed, DiagnosticCode.UnknownNodeType);
		Assert.Contains("Call:TestGame.Probe.Heal(int)", unknown.Suggestions);
	}

	// Graphs call the game's members: statics, constructors, inferred generics, and instance members of script classes on
	// the Target entity's script (Self when Target is unlinked).
	[Fact]
	public void GraphsCallTheGamesMembers()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint add = graph.Node("Call:TestGame.Probe.Add(int,int)", inputs: """{ "A": 2, "B": 3 }""");
		uint damage = graph.Node("New:TestGame.Damage(float,TestGame.Element)", inputs: """{ "Amount": 30, "Element": "Water" }""");
		uint strike = graph.Node("Call:TestGame.Probe.Strike(TestGame.Damage)");
		uint apply = graph.Node("Call:TestGame.Player.ApplyDamage(TestGame.Damage)");
		uint health = graph.Node("Get:TestGame.Player.Health");
		uint pick = graph.Node("Call:TestGame.Probe.Pick<T>(T,T,bool)", inputs: """{ "TakeSecond": true }""");
		uint text = graph.Node("FormatString", """{ "Format": "{Sum} {Health} {Picked}" }""");
		graph.Link(damage, "Value", strike, "Damage");
		graph.Link(damage, "Value", apply, "Damage");
		graph.Link(add, "Result", text, "Sum");
		graph.Link(health, "Value", text, "Health");
		graph.Link(add, "Result", pick, "First");
		graph.Link(health, "Value", pick, "Second");
		graph.Link(pick, "Result", text, "Picked");
		graph.Flow(graph.Node("Event.Create"), add);
		graph.Flow(add, strike);
		graph.Flow(strike, apply);
		graph.Flow(apply, graph.Write(text, "Result"));
		(VisualScriptRuntime runtime, FakeEnvironment environment) = GameTesting.Start(document.Build());

		Assert.Equal(VisualScriptStatus.Success, runtime.CreateInstance(TestRuntime.Hero, TestRuntime.Graph, []));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(TestRuntime.Hero, ScriptEvent.Create, 0.0f, 0));
		Assert.Equal(["Add 2 3", "Strike 30 Water"], TestGame.Probe.Calls);
		Assert.Equal(["5 70 70"], Probe.Records);
		Assert.Empty(environment.Logs);

		// On an entity whose script is of another class the call is an aborting error.
		Assert.Equal(VisualScriptStatus.Success, runtime.CreateInstance(GameTesting.Villain, TestRuntime.Graph, []));
		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(GameTesting.Villain, ScriptEvent.Create, 0.0f, 0));
		Assert.Equal(FormattableString.Invariant($"{TestRuntime.Name}: Apply Damage (node {apply}) on Villain ({GameTesting.Villain}): Villain has no Player script"),
			Assert.Single(environment.Errors));
	}

	[Fact]
	public void TwoBuildsGiveTheSameBytes()
	{
		Assert.Equal(LibraryWriter.Write(NodeLibraryBuilder.Build(GameTesting.Sources())), LibraryWriter.Write(NodeLibraryBuilder.Build(GameTesting.Sources())));
	}

	// A game type with the full name of an API type is not a type of graphs: the API's type keeps the name.
	[Fact]
	public void AGameTypeNamedLikeAnApiTypeIsANote()
	{
		AssemblyBuilder assembly = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("CollidingGame"), AssemblyBuilderAccess.Run);
		Type colliding = assembly.DefineDynamicModule("CollidingGame").DefineType("Strada.Vector3", TypeAttributes.Public | TypeAttributes.Sealed).CreateType();
		NodeLibrary library = NodeLibraryBuilder.Build(GameTesting.Sources(colliding));
		Assert.Same(typeof(Vector3), library.FindType("Strada.Vector3")!.ClrType);
		Assert.Contains(new LibraryNote("Strada.Vector3", "Game/Vector3", "Strada.Vector3: the game type has the name of a scripting API type and is not a type of graphs"),
			library.Notes);
	}

	// A game type or member carrying an attribute of a dependency the game did not deploy cannot be read: it is a note, and
	// the library keeps every other node, the API's and the game's (user data never disables visual scripting).
	[Fact]
	public void TypesAndMembersWhoseAttributesCannotBeReadAreNotes()
	{
		AssemblyLoadContext context = new("UnreadableGame", isCollectible: true);
		try
		{
			Assembly game = context.LoadFromStream(UnreadableGame());
			NodeLibrary library = NodeLibraryBuilder.Build(GameTesting.Sources([.. game.GetExportedTypes()]));

			const string missing = "Could not load file or assembly 'UndeployedDependency";
			LibraryNote tagged = Assert.Single(library.Notes, static note => note.Member == "UnreadableGame.Tagged");
			Assert.Equal("Game/Tagged", tagged.Category);
			Assert.StartsWith("UnreadableGame.Tagged: its members cannot be read: " + missing, tagged.Message, StringComparison.Ordinal);
			LibraryNote broken = Assert.Single(library.Notes, static note => note.Member == "UnreadableGame.Mixed.Broken");
			Assert.Equal("Game/Mixed", broken.Category);
			Assert.StartsWith("UnreadableGame.Mixed.Broken: the member cannot be read: " + missing, broken.Message, StringComparison.Ordinal);
			Assert.Null(library.FindType("UnreadableGame.Tagged"));
			Assert.Null(library.FindNode("Call:UnreadableGame.Tagged.Answer()"));
			Assert.Null(library.FindNode("Call:UnreadableGame.Mixed.Broken()"));
			Assert.NotNull(library.FindNode("Call:UnreadableGame.Mixed.Fine()"));
			Assert.NotNull(library.FindNode("Call:UnreadableGame.Plain.Other()"));
			Assert.NotNull(library.FindNode("Call:TestGame.Probe.Add(int,int)"));
			Assert.NotNull(library.FindNode("Call:Strada.Mathf.Abs(float)"));
			Assert.NotNull(library.FindNode("Array.Make"));
		}
		finally
		{
			context.Unload();
		}
	}

	// The image of a game whose static classes Tagged (with the attribute FancyAttribute of the assembly UndeployedDependency,
	// which exists only in this process, so no load context finds it) and Mixed and Plain have methods marked [VisualNode];
	// Mixed.Broken carries FancyAttribute as well.
	private static MemoryStream UnreadableGame()
	{
		AssemblyBuilder dependency = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("UndeployedDependency"), AssemblyBuilderAccess.RunAndCollect);
		TypeBuilder fancyBuilder = dependency.DefineDynamicModule("UndeployedDependency").DefineType("UndeployedDependency.FancyAttribute",
			TypeAttributes.Public | TypeAttributes.Sealed, typeof(Attribute));
		fancyBuilder.DefineDefaultConstructor(MethodAttributes.Public);
		CustomAttributeBuilder fancy = new(fancyBuilder.CreateType().GetConstructor(Type.EmptyTypes)!, []);
		CustomAttributeBuilder visualNode = new(typeof(VisualNodeAttribute).GetConstructor(Type.EmptyTypes)!, []);

		PersistedAssemblyBuilder game = new(new AssemblyName("UnreadableGame"), typeof(object).Assembly);
		ModuleBuilder module = game.DefineDynamicModule("UnreadableGame");
		TypeBuilder StaticClass(string name) => module.DefineType(name, TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed);
		TypeBuilder tagged = StaticClass("UnreadableGame.Tagged");
		tagged.SetCustomAttribute(fancy);
		Answer(tagged, "Answer", 42, visualNode);
		TypeBuilder mixed = StaticClass("UnreadableGame.Mixed");
		Answer(mixed, "Broken", 1, visualNode, fancy);
		Answer(mixed, "Fine", 2, visualNode);
		TypeBuilder plain = StaticClass("UnreadableGame.Plain");
		Answer(plain, "Other", 7, visualNode);
		tagged.CreateType();
		mixed.CreateType();
		plain.CreateType();
		MemoryStream image = new();
		game.Save(image);
		image.Position = 0;
		return image;
	}

	// public static int name() => value, with the attributes given.
	private static void Answer(TypeBuilder type, string name, int value, params CustomAttributeBuilder[] attributes)
	{
		MethodBuilder method = type.DefineMethod(name, MethodAttributes.Public | MethodAttributes.Static, typeof(int), Type.EmptyTypes);
		foreach (CustomAttributeBuilder attribute in attributes)
		{
			method.SetCustomAttribute(attribute);
		}
		ILGenerator code = method.GetILGenerator();
		code.Emit(OpCodes.Ldc_I4, value);
		code.Emit(OpCodes.Ret);
	}
}

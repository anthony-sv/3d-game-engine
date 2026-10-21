using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Threading;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Script classes of a pretend game: types of the type system and the domain of Script type arguments.
public class ProbeEnemy : Script
{
}

public sealed class ProbeBoss : ProbeEnemy
{
}

public sealed class ProbePlayer : Script
{
}

// Members whose parameters clash with the fixed pins of their nodes (spec section 4.5, the Parameter suffix).
public static class SuffixProbe
{
	public static void Write(string execute, string then)
	{
		_ = execute + then;
	}

	public static int Count(int result, out int then)
	{
		then = result;
		return result;
	}

	public static bool Find(int value, out int result)
	{
		result = value;
		return true;
	}
}

public sealed class TargetProbe
{
	public void Aim(Entity target)
	{
		_ = target;
	}
}

// The node library (spec sections 4.5 and 6.1) built over the whole scripting API.
public sealed class NodeLibraryTests
{
	private static NodeLibrary Library => VisualScriptTesting.Library;

	private static NodeLibrary BuildWithProbes() => NodeLibraryBuilder.Build(new NodeLibrarySources
	{
		ApiTypes = [.. NodeLibrarySources.GetApiTypes(), typeof(SuffixProbe), typeof(TargetProbe)],
		ScriptClasses = [typeof(ProbePlayer), typeof(ProbeEnemy), typeof(ProbeBoss)],
	});

	private static IEnumerable<MemberNodeDefinition> Members => Library.Nodes.OfType<MemberNodeDefinition>();

	[Fact]
	public void BuildingOverTheWholeApiSucceeds()
	{
		NodeLibrary library = NodeLibraryBuilder.Build(NodeLibrarySources.ForScriptCore([typeof(ProbePlayer), typeof(ProbeBoss)]));
		byte[] json = LibraryWriter.Write(library);
		JsonElement root = VisualScriptTesting.ParseObject(json);
		Assert.Equal(library.Types.Count, root.GetProperty("Types").GetArrayLength());
		Assert.Equal(library.Nodes.Count, root.GetProperty("Nodes").GetArrayLength());
		// Every built-in kind is in the library, and hundreds of reflected members.
		foreach (BuiltinNode node in Enum.GetValues<BuiltinNode>())
		{
			Assert.Equal(BuiltinNodes.TypeOf(node), library.GetBuiltin(node).Type);
		}
		Assert.True(library.Nodes.OfType<MemberNodeDefinition>().Count() > 500);
	}

	[Fact]
	public void BuildsGiveIdenticalBytesInEveryCulture()
	{
		byte[] first = LibraryWriter.Write(NodeLibraryBuilder.Build(NodeLibrarySources.ForScriptCore([])));
		byte[] second = LibraryWriter.Write(NodeLibraryBuilder.Build(NodeLibrarySources.ForScriptCore([])));
		Assert.Equal(first, second);
		CultureInfo culture = Thread.CurrentThread.CurrentCulture;
		CultureInfo uiCulture = Thread.CurrentThread.CurrentUICulture;
		try
		{
			Thread.CurrentThread.CurrentCulture = new CultureInfo("de-DE");
			Thread.CurrentThread.CurrentUICulture = new CultureInfo("de-DE");
			byte[] german = LibraryWriter.Write(NodeLibraryBuilder.Build(NodeLibrarySources.ForScriptCore([])));
			Assert.Equal(first, german);
		}
		finally
		{
			Thread.CurrentThread.CurrentCulture = culture;
			Thread.CurrentThread.CurrentUICulture = uiCulture;
		}
	}

	[Fact]
	public void NodesAndTypesAreInOrdinalOrder()
	{
		for (int index = 1; index < Library.Nodes.Count; index++)
		{
			NodeDefinition previous = Library.Nodes[index - 1];
			NodeDefinition node = Library.Nodes[index];
			int order = string.CompareOrdinal(previous.Category, node.Category);
			order = order != 0 ? order : string.CompareOrdinal(previous.Title, node.Title);
			order = order != 0 ? order : string.CompareOrdinal(previous.Type, node.Type);
			Assert.True(order < 0, $"{previous.Type} comes before {node.Type}");
		}
		for (int index = 1; index < Library.Types.Count; index++)
		{
			Assert.True(string.CompareOrdinal(Library.Types[index - 1].Id, Library.Types[index].Id) < 0);
		}
	}

	[Fact]
	public void ExcludedMembersHaveNoNodes()
	{
		Assert.Null(Library.FindNode("Set:Strada.Time.DeltaTime"));
		Assert.Null(Library.FindNode("Set:Strada.Entity.ID"));
		Assert.Null(Library.FindNode("Get:Strada.Component.Entity"));
		Assert.Null(Library.FindNode("Set:Strada.Component.Entity"));
		Assert.NotNull(Library.FindNode("Get:Strada.Time.DeltaTime"));
		Assert.NotNull(Library.FindNode("Get:Strada.Entity.ID"));
		string[] forbidden = [".get_", ".set_", ".op_", ".Equals(", ".GetHashCode(", ".ToString(", "Assert.AreEqual", "Assert.AreNotEqual",
			"Assert.Throws", "TestReporter.Run", "Matrix4.Inverse", "Matrix4.Item", "AssertionException", "Attribute", "Strada.Interop",
			"Strada.VisualScripting", "Script.On"];
		foreach (MemberNodeDefinition member in Members)
		{
			foreach (string text in forbidden)
			{
				Assert.False(member.Type.Contains(text, StringComparison.Ordinal), $"{member.Type} should not be a node ({text})");
			}
		}
		Assert.NotNull(Library.FindNode("Get:Strada.Mathf.PI"));
		Assert.Null(Library.FindNode("Set:Strada.Mathf.PI"));
		Assert.NotNull(Library.FindNode("Call:Strada.Testing.TestReporter.Pass(string)"));
		Assert.NotNull(Library.FindNode("Call:Strada.Testing.Assert.IsNull(System.Object,string)"));
	}

	[Fact]
	public void TypesAreTheApiTypesWithValues()
	{
		string[] expected = ["exec", "bool", "int", "uint", "long", "ulong", "float", "double", "string", "System.Object", "Strada.Entity",
			"Strada.Script", "Strada.Component", "Strada.RigidBodyComponent", "Strada.Asset", "Strada.Material", "Strada.AssetHandle",
			"Strada.Vector3", "Strada.Quaternion", "Strada.Color", "Strada.Matrix4", "Strada.RaycastHit", "Strada.Ray", "Strada.KeyCode",
			"Strada.Entity[]"];
		foreach (string id in expected)
		{
			Assert.NotNull(Library.FindType(id));
		}
		// Static classes, attributes and exceptions hold no values; the wildcard is no type.
		foreach (string id in new[] { "*", "Strada.Mathf", "Strada.Input", "Strada.Physics", "Strada.Testing.Assert", "Strada.RangeAttribute",
			"Strada.Testing.AssertionException" })
		{
			Assert.Null(Library.FindType(id));
		}
		Assert.Equal(TypeKind.Entity, Library.FindType("Strada.Script")!.Kind);
		Assert.Equal(TypeKind.Array, Library.FindType("Strada.Entity[]")!.Kind);
		Assert.Equal("Entity Array", Library.FindType("Strada.Entity[]")!.Title);
		Assert.Equal("Rigid Body", Library.FindType("Strada.RigidBodyComponent")!.Title);
	}

	[Fact]
	public void PurityFollowsTheRule()
	{
		foreach (string type in new[] { "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", "Call:Strada.Entity.HasComponent<T>()",
			"Call:Strada.CameraComponent.WorldToScreenPoint(Strada.Vector3)", "Call:Strada.Mathf.Sin(float)", "Get:Strada.Entity.Name",
			"New:Strada.Vector3(float,float,float)", "Call:Strada.Random.Range(int,int)", "Call:Strada.Assets.Load<T>(string)" })
		{
			Assert.True(VisualScriptTesting.LibraryNode(type).Pure, $"{type} is pure");
			Assert.Equal(NodeKind.Pure, VisualScriptTesting.LibraryNode(type).Kind);
		}
		foreach (string type in new[] { "Call:Strada.Entity.Destroy()", "Call:Strada.Random.Seed(int)",
			"Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)", "Set:Strada.Entity.Name",
			"Call:Strada.Entity.Create(string)", "Call:Strada.Log.Info(string)" })
		{
			Assert.False(VisualScriptTesting.LibraryNode(type).Pure, $"{type} is impure");
			Assert.Equal(NodeKind.Call, VisualScriptTesting.LibraryNode(type).Kind);
		}
		// Impure nodes have Execute and Then; pure ones have no exec pins.
		Assert.Contains(VisualScriptTesting.LibraryNode("Call:Strada.Random.Seed(int)").Pins, static pin => pin.Name == "Execute" && pin.IsExec);
		Assert.DoesNotContain(VisualScriptTesting.LibraryNode("Call:Strada.Mathf.Sin(float)").Pins, static pin => pin.IsExec);
	}

	[Fact]
	public void ExactlyTheListedQueriesAreMarkedPure()
	{
		const System.Reflection.BindingFlags declared = System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Instance |
			System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.DeclaredOnly;
		string[] marked = [.. typeof(Entity).Assembly.GetTypes()
			.SelectMany(static type => type.GetMethods(declared))
			.Where(static method => method.IsDefined(typeof(PureNodeAttribute), false))
			.Select(static method => $"{method.DeclaringType!.Name}.{method.Name}")
			.Order(StringComparer.Ordinal)];
		Assert.Equal(
		[
			"Assets.Load", "CameraComponent.ScreenToWorldRay", "CameraComponent.WorldToScreenPoint", "Entity.As", "Entity.FindByID",
			"Entity.FindByName", "Entity.GetComponent", "Entity.HasComponent", "Input.GetGamepadAxis", "Input.IsGamepadButtonDown",
			"Input.IsGamepadButtonPressed", "Input.IsGamepadButtonReleased", "Input.IsGamepadConnected", "Input.IsKeyDown", "Input.IsKeyPressed",
			"Input.IsKeyReleased", "Input.IsMouseButtonDown", "Input.IsMouseButtonPressed", "Input.IsMouseButtonReleased", "MeshComponent.GetMaterial",
			"Ray.GetPoint",
		], marked);
	}

	[Fact]
	public void EasingChoicesAreTheEasingsInOrder()
	{
		Assert.Equal(Enum.GetNames<Easing>(), BuiltinNodes.EasingNames);
		Assert.Equal(BuiltinNodes.EasingNames, VisualScriptTesting.LibraryNode("Flow.Tween").FindSetting("Easing")!.Choices);
	}

	[Fact]
	public void SetNodesExistForStaticMembersAndReferenceTypesOnly()
	{
		Assert.Null(Library.FindNode("Set:Strada.Vector3.X"));
		Assert.NotNull(Library.FindNode("Get:Strada.Vector3.X"));
		Assert.Null(Library.FindNode("Set:Strada.Ray.Origin"));
		Assert.NotNull(Library.FindNode("Set:Strada.Entity.Name"));
		Assert.NotNull(Library.FindNode("Set:Strada.TransformComponent.Translation"));
		Assert.NotNull(Library.FindNode("Set:Strada.Physics.Gravity"));
		Assert.NotNull(Library.FindNode("Set:Strada.Time.TimeScale"));
		foreach (MemberNodeDefinition member in Members.Where(static member => member.MemberKind == MemberNodeKind.Set))
		{
			bool isStatic = member.Target == TargetKind.None;
			Assert.True(isStatic || !member.DeclaringType.IsValueType, $"{member.Type} sets a struct instance");
			Assert.False(member.Pure);
		}
	}

	[Fact]
	public void MakeAndBreakCoverTheStructs()
	{
		// The library lists types by ID.
		Assert.Equal(["Strada.Color", "Strada.Matrix4", "Strada.Quaternion", "Strada.Ray", "Strada.Vector2", "Strada.Vector3", "Strada.Vector4"],
			Library.Types.Where(static type => type.Makeable).Select(static type => type.Id));
		Assert.Equal(["Strada.AssetHandle", "Strada.Color", "Strada.Matrix4", "Strada.Quaternion", "Strada.Ray", "Strada.RaycastHit", "Strada.Vector2",
			"Strada.Vector3", "Strada.Vector4"], Library.Types.Where(static type => type.Breakable).Select(static type => type.Id));
		Assert.Equal(["X", "Y", "Z"], Library.FindType("Strada.Vector3")!.Fields.Select(static field => field.Name));
		Assert.Equal(["R", "G", "B", "A"], Library.FindType("Strada.Color")!.Fields.Select(static field => field.Name));
		Assert.Equal(["Column0", "Column1", "Column2", "Column3"], Library.FindType("Strada.Matrix4")!.Fields.Select(static field => field.Name));
		Assert.Equal(["Entity", "Point", "Normal", "Distance"], Library.FindType("Strada.RaycastHit")!.Fields.Select(static field => field.Name));
		Assert.Equal(["ID", "IsValid"], Library.FindType("Strada.AssetHandle")!.Fields.Select(static field => field.Name));
		Assert.Equal(["Strada.Entity", "Strada.Vector3", "Strada.Vector3", "float"],
			Library.FindType("Strada.RaycastHit")!.Fields.Select(static field => field.Type.Id));
	}

	[Fact]
	public void ImplicitConversionsFollowTheRules()
	{
		static string[] ImplicitTo(NodeLibrary library, string id) => [.. library.FindType(id)!.ImplicitTo.Select(static type => type.Id)];

		Assert.Equal(["long", "float", "double", "System.Object"], ImplicitTo(Library, "int"));
		Assert.Equal(["long", "ulong", "float", "double", "System.Object"], ImplicitTo(Library, "uint"));
		Assert.Equal(["float", "double", "System.Object"], ImplicitTo(Library, "long"));
		Assert.Equal(["float", "double", "System.Object"], ImplicitTo(Library, "ulong"));
		Assert.Equal(["double", "System.Object"], ImplicitTo(Library, "float"));
		Assert.Equal(["System.Object"], ImplicitTo(Library, "double"));
		Assert.Equal(["System.Object"], ImplicitTo(Library, "string"));
		Assert.Equal(["Strada.Asset", "Strada.AssetHandle", "System.Object"], ImplicitTo(Library, "Strada.Material"));
		Assert.Equal(["Strada.Entity", "System.Object"], ImplicitTo(Library, "Strada.RigidBodyComponent"));
		Assert.Equal(["Strada.Entity", "System.Object"], ImplicitTo(Library, "Strada.Script"));
		Assert.Equal(["System.Object"], ImplicitTo(Library, "Strada.Entity"));
		Assert.Equal(["System.Object"], ImplicitTo(Library, "Strada.Entity[]"));
		Assert.Equal(["System.Object"], ImplicitTo(Library, "Strada.Vector3"));
		Assert.Empty(ImplicitTo(Library, "System.Object"));
		Assert.Empty(ImplicitTo(Library, "exec"));

		NodeLibrary game = BuildWithProbes();
		Assert.Equal(["Strada.ScriptCore.Tests.ProbeEnemy", "Strada.Script", "Strada.Entity", "System.Object"],
			ImplicitTo(game, "Strada.ScriptCore.Tests.ProbeBoss"));
		Assert.Equal(TypeKind.Entity, game.FindType("Strada.ScriptCore.Tests.ProbeBoss")!.Kind);
	}

	[Fact]
	public void EveryNodeInputTypeCanBeProduced()
	{
		List<VisualType> outputs = [];
		foreach (NodeDefinition node in Library.Nodes)
		{
			foreach (PinDescription pin in node.Pins)
			{
				if (!pin.IsInput && pin.Type.HasValues && !outputs.Contains(pin.Type))
				{
					outputs.Add(pin.Type);
				}
			}
		}
		foreach (NodeDefinition node in Library.Nodes)
		{
			foreach (PinDescription pin in node.Pins)
			{
				if (!pin.IsInput || !pin.Type.HasValues)
				{
					continue;
				}
				bool produced = pin.Type.HasLiteral || outputs.Exists(output => TypeRules.GetConversion(output, pin.Type) != null);
				Assert.True(produced, $"nothing produces the {pin.Type.Id} input {pin.Name} of {node.Type}");
			}
		}
	}

	[Fact]
	public void ParameterPinsClashingWithFixedPinsTakeTheSuffix()
	{
		NodeLibrary library = BuildWithProbes();
		static string[] Pins(NodeLibrary library, string type) => [.. library.FindNode(type)!.Pins.Select(static pin => $"{pin.Direction}:{pin.Name}")];

		Assert.Equal(["In:Execute", "In:ExecuteParameter", "In:Then", "Out:Then"], Pins(library, "Call:Strada.ScriptCore.Tests.SuffixProbe.Write(string,string)"));
		Assert.Equal(["In:Execute", "In:Result", "Out:Then", "Out:ThenParameter", "Out:Result"],
			Pins(library, "Call:Strada.ScriptCore.Tests.SuffixProbe.Count(int,out int)"));
		Assert.Equal(["In:Execute", "In:Value", "Out:Then", "Out:ResultParameter", "Out:Result"],
			Pins(library, "Call:Strada.ScriptCore.Tests.SuffixProbe.Find(int,out int)"));
		Assert.Equal(["In:Execute", "In:Target", "In:TargetParameter", "Out:Then"], Pins(library, "Call:Strada.ScriptCore.Tests.TargetProbe.Aim(Strada.Entity)"));
		// The input Value of Mathf.Clamp keeps its name: its only output is Result.
		Assert.Equal(["In:Value", "In:Min", "In:Max", "Out:Result"], Pins(library, "Call:Strada.Mathf.Clamp(float,float,float)"));
	}

	[Fact]
	public void ReflectedPinsFollowTheSignature()
	{
		NodeDefinition raycast = VisualScriptTesting.LibraryNode("Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)");
		Assert.Equal(["Execute", "Origin", "Direction", "MaxDistance", "LayerMask", "Then", "Hit", "Result"], raycast.Pins.Select(static pin => pin.Name));
		PinDescription layerMask = raycast.Pins.Single(static pin => pin.Name == "LayerMask");
		Assert.True(layerMask.HasDefault);
		Assert.Equal(Physics.AllLayers, layerMask.Default);
		Assert.Equal(PinDirection.Out, raycast.Pins.Single(static pin => pin.Name == "Hit").Direction);

		NodeDefinition addForce = VisualScriptTesting.LibraryNode("Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)");
		PinDescription target = addForce.Pins.Single(static pin => pin.Name == "Target");
		Assert.True(target.DefaultsToSelf);
		Assert.Equal("Strada.Entity", target.Type.Id);
		Assert.Equal(ForceMode.Force, addForce.Pins.Single(static pin => pin.Name == "Mode").Default);
		Assert.Equal(TargetKind.Component, ((MemberNodeDefinition)addForce).Target);

		PinDescription entityTarget = VisualScriptTesting.LibraryNode("Get:Strada.Entity.Name").Pins.Single(static pin => pin.Name == "Target");
		Assert.True(entityTarget.DefaultsToSelf);
		PinDescription vectorTarget = VisualScriptTesting.LibraryNode("Get:Strada.Vector3.Length").Pins.Single(static pin => pin.Name == "Target");
		Assert.False(vectorTarget.DefaultsToSelf);
		Assert.Equal("Strada.Vector3", vectorTarget.Type.Id);
		Assert.Equal("Strada.Asset", VisualScriptTesting.LibraryNode("Get:Strada.Asset.Handle").Pins.Single(static pin => pin.Name == "Target").Type.Id);
		Assert.DoesNotContain(VisualScriptTesting.LibraryNode("Get:Strada.Time.DeltaTime").Pins, static pin => pin.Name == "Target");
		Assert.Equal(["X", "Y", "Z", "Value"], VisualScriptTesting.LibraryNode("New:Strada.Vector3(float,float,float)").Pins.Select(static pin => pin.Name));
		Assert.Equal(["Execute", "Target", "Value", "Then"], VisualScriptTesting.LibraryNode("Set:Strada.Entity.Name").Pins.Select(static pin => pin.Name));
	}

	[Fact]
	public void GenericMethodsTakeTypeArgumentsFromTheirDomain()
	{
		MemberNodeDefinition getComponent = (MemberNodeDefinition)VisualScriptTesting.LibraryNode("Call:Strada.Entity.GetComponent<T>()");
		Assert.Equal(TypeConstraint.Component, getComponent.GenericDomain);
		SettingDescription setting = Assert.Single(getComponent.Settings);
		Assert.Equal("TypeArguments", setting.Name);
		Assert.Equal(SettingKind.TypeList, setting.Kind);
		Assert.True(setting.Required);
		PinDescription result = getComponent.Pins.Single(static pin => pin.Name == "Result");
		Assert.True(result.Type.IsWildcard);
		Assert.Equal(PinConstraint.Reference, result.Constraint);
		Assert.Equal(TypeConstraint.Asset, ((MemberNodeDefinition)VisualScriptTesting.LibraryNode("Call:Strada.Assets.Load<T>(string)")).GenericDomain);
		Assert.Equal(TypeConstraint.Script, ((MemberNodeDefinition)VisualScriptTesting.LibraryNode("Call:Strada.Entity.As<T>()")).GenericDomain);

		Assert.Contains(Library.Domains.Components, static type => type.Id == "Strada.RigidBodyComponent");
		Assert.Contains(Library.Domains.Components, static type => type.Id == "Strada.TransformComponent");
		Assert.DoesNotContain(Library.Domains.Components, static type => type.Id == "Strada.Component");
		Assert.Equal(["Strada.AudioClip", "Strada.EnvironmentMap", "Strada.Font", "Strada.Material", "Strada.Mesh", "Strada.Prefab", "Strada.Texture"],
			Library.Domains.Assets.Select(static type => type.Id));
		Assert.Empty(Library.Domains.Scripts);
		Assert.Equal(["Strada.ScriptCore.Tests.ProbeBoss", "Strada.ScriptCore.Tests.ProbeEnemy", "Strada.ScriptCore.Tests.ProbePlayer"],
			BuildWithProbes().Domains.Scripts.Select(static type => type.Id));
	}

	[Fact]
	public void TitlesSubtitlesAndCategoriesNameTheMembers()
	{
		NodeDefinition addForce = VisualScriptTesting.LibraryNode("Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)");
		Assert.Equal("Add Force", addForce.Title);
		Assert.Equal("Target is Rigid Body", addForce.Subtitle);
		Assert.Equal("Components/Rigid Body", addForce.Category);
		Assert.Equal("Make Vector3 (X, Y, Z)", VisualScriptTesting.LibraryNode("New:Strada.Vector3(float,float,float)").Title);
		Assert.Equal("Get Delta Time", VisualScriptTesting.LibraryNode("Get:Strada.Time.DeltaTime").Title);
		Assert.Equal("Time", VisualScriptTesting.LibraryNode("Get:Strada.Time.DeltaTime").Subtitle);
		Assert.Equal("Math", VisualScriptTesting.LibraryNode("Call:Strada.Mathf.Sin(float)").Category);
		Assert.Equal("Math/Vector3", VisualScriptTesting.LibraryNode("Call:Strada.Vector3.Lerp(Strada.Vector3,Strada.Vector3,float)").Category);
		Assert.Equal("Testing", VisualScriptTesting.LibraryNode("Call:Strada.Testing.TestReporter.Pass(string)").Category);
		Assert.Equal("Test Reporter", VisualScriptTesting.LibraryNode("Call:Strada.Testing.TestReporter.Pass(string)").Subtitle);
		Assert.Equal("Assets/Material", VisualScriptTesting.LibraryNode("Get:Strada.Material.BaseColor").Category);
		Assert.Equal("Physics", VisualScriptTesting.LibraryNode("Get:Strada.RaycastHit.Point").Category);
		Assert.Equal("Debug", VisualScriptTesting.LibraryNode("Call:Strada.Log.Info(string)").Category);
		Assert.Equal("Events", VisualScriptTesting.LibraryNode("Event.Update").Category);
		Assert.Equal("Math/Operators", VisualScriptTesting.LibraryNode("Operator.Add").Category);
		Assert.Equal("Utilities/Array", VisualScriptTesting.LibraryNode("Array.Get").Category);
		Assert.NotEmpty(VisualScriptTesting.LibraryNode("Flow.Branch").Keywords);
		Assert.Empty(addForce.Keywords);

		Assert.Equal("Add Force", Titles.Words("AddForce"));
		Assert.Equal("UI Button", Titles.Words("UIButton"));
		Assert.Equal("Perspective FOV", Titles.Words("PerspectiveFOV"));
		Assert.Equal("Is Pointer Over UI", Titles.Words("IsPointerOverUI"));
		Assert.Equal("Deg2 Rad", Titles.Words("Deg2Rad"));
		Assert.Equal("Vector3", Titles.Words("Vector3"));
		Assert.Equal("TRS", Titles.Words("TRS"));
	}

	[Fact]
	public void DescriptionsComeFromTheDocumentationFile()
	{
		Assert.NotNull(NodeLibrarySources.GetDocumentationPath());
		NodeDefinition addForce = VisualScriptTesting.LibraryNode("Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)");
		// <see cref="ForceMode"/> is rendered as the member's name.
		Assert.Contains("ForceMode", addForce.Description, StringComparison.Ordinal);
		Assert.DoesNotContain("<", addForce.Description, StringComparison.Ordinal);
		Assert.Equal("The entity's component of type T, or null when it has none.",
			VisualScriptTesting.LibraryNode("Call:Strada.Entity.GetComponent<T>()").Description);

		NodeLibrary undocumented = NodeLibraryBuilder.Build(new NodeLibrarySources { ApiTypes = NodeLibrarySources.GetApiTypes() });
		Assert.Empty(undocumented.FindNode(addForce.Type)!.Description);
		Assert.NotEmpty(undocumented.FindNode("Flow.Branch")!.Description);
	}

	[Fact]
	public void TheJsonDescribesTypesAndNodes()
	{
		JsonElement root = VisualScriptTesting.ParseObject(LibraryWriter.Write(Library));
		Dictionary<string, string> types = root.GetProperty("Types").EnumerateArray()
			.ToDictionary(static type => type.GetProperty("ID").GetString()!, static type => type.GetRawText());
		Assert.Equal("""{"ID":"float","Title":"Float","Kind":"Float","FieldType":"Float","Literal":true,"ImplicitTo":["double","System.Object"]}""",
			types["float"]);
		Assert.Equal("""{"ID":"Strada.RigidBodyComponent","Title":"Rigid Body","Kind":"Component","Base":"Strada.Component","ImplicitTo":["Strada.Entity","System.Object"]}""",
			types["Strada.RigidBodyComponent"]);
		Assert.Equal("""{"ID":"Strada.Entity[]","Title":"Entity Array","Kind":"Array","Element":"Strada.Entity","ImplicitTo":["System.Object"]}""",
			types["Strada.Entity[]"]);
		Assert.Equal("""{"ID":"Strada.Material","Title":"Material","Kind":"Asset","FieldType":"Asset","AssetType":"Material","Literal":true,"Base":"Strada.Asset","ImplicitTo":["Strada.Asset","Strada.AssetHandle","System.Object"]}""",
			types["Strada.Material"]);
		Assert.StartsWith("""{"ID":"Strada.KeyCode","Title":"Key Code","Kind":"Enum","FieldType":"Int32","Literal":true,"Enumerators":[""", types["Strada.KeyCode"],
			StringComparison.Ordinal);
		Assert.Contains("""{"Name":"Space","Value":32}""", types["Strada.KeyCode"], StringComparison.Ordinal);
		Assert.Contains("""
			"Makeable":true,"Breakable":true,"Fields":[{"Name":"X","Type":"float"}
			""", types["Strada.Vector3"], StringComparison.Ordinal);

		Dictionary<string, string> nodes = root.GetProperty("Nodes").EnumerateArray()
			.ToDictionary(static node => node.GetProperty("Type").GetString()!, static node => node.GetRawText());
		Assert.Contains("""
			"Pins":[{"Name":"Execute","Direction":"In","Type":"exec"},{"Name":"Duration","Direction":"In","Type":"float","Default":0.2},{"Name":"Completed","Direction":"Out","Type":"exec","Role":"Continuation"}],"Settings":[]
			""",
			nodes["Flow.Delay"], StringComparison.Ordinal);
		Assert.Contains("""
			"Pins":[{"Name":"A","Direction":"In","Type":"*","Constraint":"Any"},{"Name":"B","Direction":"In","Type":"*","Constraint":"Any"},{"Name":"Result","Direction":"Out","Type":"*","Constraint":"Any"}]
			""",
			nodes["Operator.Add"], StringComparison.Ordinal);
		Assert.Contains("""
			"Settings":[{"Name":"Count","Kind":"Int","Default":2,"Min":2,"Max":64}]
			""", nodes["Flow.Sequence"], StringComparison.Ordinal);
		Assert.Contains("""{"Name":"Mode","Direction":"In","Type":"Strada.ForceMode","Default":"Force"}""",
			nodes["Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)"], StringComparison.Ordinal);
		Assert.Contains("""{"Name":"Target","Direction":"In","Type":"Strada.Entity","DefaultsToSelf":true}""",
			nodes["Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)"], StringComparison.Ordinal);
		Assert.Contains("""{"Name":"Element","Direction":"Out","Type":"*","Constraint":"Any","ElementOf":"Array"}""", nodes["Flow.ForEach"],
			StringComparison.Ordinal);
		Assert.Contains("""{"Name":"In","Direction":"In","Type":"*","Constraint":"Any","Group":"T"}""", nodes["Reroute"], StringComparison.Ordinal);
		Assert.Contains("""{"Name":"TypeArguments","Kind":"TypeList","Required":true,"Constraint":"Component"}""",
			nodes["Call:Strada.Entity.GetComponent<T>()"], StringComparison.Ordinal);
		Assert.Contains("""{"Name":"Key","Kind":"Enum","Required":true,"Type":"Strada.KeyCode"}""", nodes["Event.KeyPressed"], StringComparison.Ordinal);
	}
}

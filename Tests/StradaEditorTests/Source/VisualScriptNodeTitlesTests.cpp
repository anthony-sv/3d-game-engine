#include "Script/ScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptNodeTitles.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	struct BuiltInTitle
	{
		std::string_view Type;
		std::string_view Title;
		// The node library's title where it has more words than the type's last segment; empty when it is Title.
		std::string_view LibraryTitle = {};
	};

	// Every built-in node kind of the scripting runtime (BuiltinNodes.TypeOf) with the title a node of it without settings
	// has. Each equals the node library's title, except where LibraryTitle names the library's.
	constexpr std::array<BuiltInTitle, 90> BuiltInTitles = {{
		{"Event.Create", "Create"},
		{"Event.Update", "Update"},
		{"Event.FixedUpdate", "Fixed Update"},
		{"Event.Destroy", "Destroy"},
		{"Event.CollisionEnter", "Collision Enter"},
		{"Event.CollisionExit", "Collision Exit"},
		{"Event.TriggerEnter", "Trigger Enter"},
		{"Event.TriggerExit", "Trigger Exit"},
		{"Event.Click", "Click"},
		{"Event.KeyPressed", "Key Pressed"},
		{"Event.KeyReleased", "Key Released"},
		{"Event.MouseButtonPressed", "Mouse Button Pressed"},
		{"Event.MouseButtonReleased", "Mouse Button Released"},
		{"Event.GamepadButtonPressed", "Gamepad Button Pressed"},
		{"Event.GamepadButtonReleased", "Gamepad Button Released"},
		{"Event.Custom", "Custom Event"},
		{"Event.Send", "Send Event"},
		{"Flow.Branch", "Branch"},
		{"Flow.Sequence", "Sequence"},
		{"Flow.Switch", "Switch"},
		{"Flow.ForLoop", "For Loop"},
		{"Flow.ForEach", "For Each"},
		{"Flow.While", "While"},
		{"Flow.DoOnce", "Do Once"},
		{"Flow.DoN", "Do N"},
		{"Flow.FlipFlop", "Flip Flop"},
		{"Flow.Gate", "Gate"},
		{"Flow.Delay", "Delay"},
		{"Flow.RetriggerableDelay", "Retriggerable Delay"},
		{"Flow.WaitFrames", "Wait Frames"},
		{"Flow.WaitUntil", "Wait Until"},
		{"Flow.Tween", "Tween"},
		{"Flow.Interval", "Interval"},
		{"Flow.Timeline", "Timeline"},
		{"Variable.Get", "Get Variable"},
		{"Variable.Set", "Set Variable"},
		{"Function.Entry", "Entry", "Function Entry"},
		{"Function.Return", "Return"},
		{"Function.Call", "Call Function"},
		{"Self", "Self"},
		{"Literal", "Literal"},
		{"Reroute", "Reroute"},
		{"Select", "Select"},
		{"IsValid", "Is Valid"},
		{"Convert", "Convert"},
		{"Make", "Make"},
		{"Break", "Break"},
		{"FormatString", "Format String"},
		{"Array.Length", "Length"},
		{"Array.Get", "Get"},
		{"Expression", "Expression"},
		{"String.Length", "Length"},
		{"String.IsEmpty", "Is Empty"},
		{"String.Contains", "Contains"},
		{"String.StartsWith", "Starts With"},
		{"String.EndsWith", "Ends With"},
		{"String.IndexOf", "Index Of"},
		{"String.Substring", "Substring"},
		{"String.Replace", "Replace"},
		{"String.ToUpper", "To Upper"},
		{"String.ToLower", "To Lower"},
		{"String.Trim", "Trim"},
		{"String.Split", "Split"},
		{"String.Join", "Join"},
		{"String.ToInt", "To Int"},
		{"String.ToFloat", "To Float"},
		{"Array.Contains", "Contains"},
		{"Array.IndexOf", "Index Of"},
		{"Array.Make", "Make", "Make Array"},
		{"Array.Set", "Set", "Set Element"},
		{"Array.Add", "Add", "Add Element"},
		{"Array.RemoveAt", "Remove At", "Remove Element"},
		{"Array.Insert", "Insert", "Insert Element"},
		{"Array.Append", "Append", "Append Arrays"},
		{"Operator.Add", "Add"},
		{"Operator.Subtract", "Subtract"},
		{"Operator.Multiply", "Multiply"},
		{"Operator.Divide", "Divide"},
		{"Operator.Modulo", "Modulo"},
		{"Operator.Negate", "Negate"},
		{"Operator.Equal", "Equal"},
		{"Operator.NotEqual", "Not Equal"},
		{"Operator.Less", "Less"},
		{"Operator.LessOrEqual", "Less Or Equal"},
		{"Operator.Greater", "Greater"},
		{"Operator.GreaterOrEqual", "Greater Or Equal"},
		{"Operator.And", "And"},
		{"Operator.Or", "Or"},
		{"Operator.Xor", "Xor"},
		{"Operator.Not", "Not"},
	}};

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = glm::vec2(static_cast<float>(id) * 16.0f, 0.0f);
		node.Settings = std::move(settings);
		return node;
	}

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		return variable;
	}

	VisualScriptLocal MakeLocal(uint32_t id, std::string name, std::string type)
	{
		VisualScriptLocal local;
		local.ID = id;
		local.Name = std::move(name);
		local.Type = std::move(type);
		return local;
	}

	std::string Title(std::string type, Json settings = Json::object())
	{
		return DeriveVisualScriptNodeTitle(MakeNode(1, std::move(type), std::move(settings)), VisualScriptDocument());
	}

	// A document whose variables, locals, functions and custom events title nodes, and nodes whose settings name none.
	VisualScriptDocument MakeNamingDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Speed", "float"));
		document.Variables.push_back(MakeVariable(2, "Score", "int"));

		VisualScriptFunction addScore;
		addScore.ID = 10;
		addScore.Name = "AddScore";
		addScore.Inputs.push_back(VisualScriptParameter{"Amount", "int"});
		addScore.Locals.push_back(MakeLocal(11, "Total", "int"));
		addScore.Graph.Nodes = {MakeNode(12, "Function.Entry"), MakeNode(13, "Variable.Get", Json::object({{"Variable", 11}})),
		                        MakeNode(14, "Variable.Set", Json::object({{"Variable", 2}})),
		                        MakeNode(15, "Variable.Get", Json::object({{"Variable", 1}}))};
		document.Functions.push_back(std::move(addScore));

		VisualScriptFunction reset;
		reset.ID = 20;
		reset.Name = "Reset";
		reset.Locals.push_back(MakeLocal(21, "Counter", "int"));
		// A local of another function, and a local of its own.
		reset.Graph.Nodes = {MakeNode(22, "Variable.Get", Json::object({{"Variable", 11}})),
		                     MakeNode(23, "Variable.Set", Json::object({{"Variable", 21}}))};
		document.Functions.push_back(std::move(reset));

		document.Graph.Nodes = {
			MakeNode(30, "Variable.Get", Json::object({{"Variable", 1}})),
			MakeNode(31, "Variable.Set", Json::object({{"Variable", 2}})),
			// A local is no variable of the event graph.
			MakeNode(32, "Variable.Get", Json::object({{"Variable", 11}})),
			MakeNode(33, "Variable.Get", Json::object({{"Variable", 99}})),
			MakeNode(34, "Variable.Set"),
			MakeNode(35, "Variable.Get", Json::object({{"Variable", "1"}})),
			MakeNode(36, "Variable.Get", Json::object({{"Variable", 1.0}})),
			MakeNode(37, "Variable.Set", Json::object({{"Variable", -1}})),
			MakeNode(38, "Variable.Get", Json::object({{"Variable", int64_t(4294967297)}})),
			MakeNode(39, "Function.Call", Json::object({{"Function", 10}})),
			MakeNode(40, "Function.Call", Json::object({{"Function", 11}})),
			MakeNode(41, "Function.Call"),
			MakeNode(42, "Event.Custom", Json::object({{"Name", "Hit"}})),
			MakeNode(43, "Event.Send", Json::object({{"Name", "Hit"}})),
			MakeNode(44, "Event.Custom", Json::object({{"Name", "Hit.Back"}})),
			MakeNode(45, "Event.Send", Json::object({{"Name", " Hit"}})),
			MakeNode(46, "Event.Custom", Json::object({{"Name", ""}})),
			MakeNode(47, "Event.Send", Json::object({{"Name", 3}})),
			MakeNode(48, "Event.Custom", Json::object({{"Name", "Line\nBreak"}})),
			MakeNode(49, "Event.Send", Json::object({{"Name", std::string(65, 'x')}})),
			MakeNode(50, "Event.Send", Json::object({{"Name", std::string(64, 'x')}})),
		};
		return document;
	}

	// What the titles of MakeNamingDocument's nodes are, by node.
	std::vector<std::pair<uint32_t, std::string>> GetNamingTitles()
	{
		return {
			{12, "Entry"},        {13, "Get Total"},     {14, "Set Score"},
			{15, "Get Speed"},    {22, "Get Variable"},  {23, "Set Counter"},
			{30, "Get Speed"},    {31, "Set Score"},     {32, "Get Variable"},
			{33, "Get Variable"}, {34, "Set Variable"},  {35, "Get Variable"},
			{36, "Get Variable"}, {37, "Set Variable"},  {38, "Get Variable"},
			{39, "AddScore"},     {40, "Call Function"}, {41, "Call Function"},
			{42, "Hit"},          {43, "Send Hit"},      {44, "Custom Event"},
			{45, "Send Event"},   {46, "Custom Event"},  {47, "Send Event"},
			{48, "Custom Event"}, {49, "Send Event"},    {50, "Send " + std::string(64, 'x')},
		};
	}

	VisualScriptNode const* FindNode(VisualScriptDocument const& document, uint32_t id)
	{
		auto const findIn = [id](VisualScriptGraph const& graph) -> VisualScriptNode const*
		{
			auto const node = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
			                               [id](VisualScriptNode const& candidate)
			                               {
											   return candidate.ID == id;
										   });
			return node != graph.Nodes.end() ? &*node : nullptr;
		};
		for (VisualScriptFunction const& function : document.Functions)
		{
			if (VisualScriptNode const* node = findIn(function.Graph))
			{
				return node;
			}
		}
		return findIn(document.Graph);
	}

	// The members of the scripting API in a library are titled as it titles them; a constructor's title is followed by
	// its inputs there ("Make Vector3 (X, Y, Z)"). Built-in kinds without settings are titled as BuiltInTitles says.
	// Returns how many nodes of each form it checked: calls, getters and setters, constructors, built-in kinds.
	std::array<size_t, 4> CheckLibraryTitles(VisualScriptLibrary const& library)
	{
		std::array<size_t, 4> checked = {};
		for (VisualScriptNodeTemplate const& node : library.GetNodes())
		{
			CAPTURE(node.Type);
			std::string const derived = Title(node.Type);
			if (node.Type.starts_with("Call:") || node.Type.starts_with("Get:") || node.Type.starts_with("Set:"))
			{
				CHECK(derived == node.Title);
				checked[node.Type.starts_with("Call:") ? 0 : 1]++;
			}
			else if (node.Type.starts_with("New:"))
			{
				CHECK(node.Title.starts_with(derived + " ("));
				checked[2]++;
			}
			else
			{
				auto const builtIn = std::find_if(BuiltInTitles.begin(), BuiltInTitles.end(),
				                                  [&](BuiltInTitle const& entry)
				                                  {
													  return entry.Type == node.Type;
												  });
				REQUIRE_MESSAGE(builtIn != BuiltInTitles.end(), "a built-in node kind the titles do not list");
				CHECK(derived == builtIn->Title);
				CHECK(node.Title == (builtIn->LibraryTitle.empty() ? builtIn->Title : builtIn->LibraryTitle));
				checked[3]++;
			}
		}
		return checked;
	}
}

TEST_CASE("VisualScriptEditor: nodes of built-in kinds are titled by the words of their type's last segment")
{
	for (BuiltInTitle const& entry : BuiltInTitles)
	{
		CAPTURE(entry.Type);
		CHECK(Title(std::string(entry.Type)) == entry.Title);
	}
	// Settings that title no built-in kind change nothing.
	CHECK(Title("Make", Json::object({{"Type", "Strada.Vector3"}})) == "Make");
	CHECK(Title("Literal", Json::object({{"Type", "float"}, {"Value", 2.5}})) == "Literal");
	CHECK(Title("Event.KeyPressed", Json::object({{"Key", "Space"}})) == "Key Pressed");
}

TEST_CASE("VisualScriptEditor: nodes are titled by the variables, locals, functions and custom events their settings name")
{
	VisualScriptDocument const document = MakeNamingDocument();
	for (auto const& [id, title] : GetNamingTitles())
	{
		CAPTURE(id);
		VisualScriptNode const* const node = FindNode(document, id);
		REQUIRE(node != nullptr);
		CHECK(DeriveVisualScriptNodeTitle(*node, document) == title);
	}

	// A node of no graph sees the document's variables, but no function's locals.
	CHECK(DeriveVisualScriptNodeTitle(MakeNode(60, "Variable.Get", Json::object({{"Variable", 21}})), document) == "Get Variable");
	CHECK(DeriveVisualScriptNodeTitle(MakeNode(61, "Variable.Set", Json::object({{"Variable", 1}})), document) == "Set Speed");
	// Settings that are no object name nothing.
	VisualScriptNode broken = MakeNode(62, "Event.Custom");
	broken.Settings = Json::array({"Hit"});
	CHECK(DeriveVisualScriptNodeTitle(broken, document) == "Custom Event");
	broken.Type = "Variable.Get";
	broken.Settings = Json();
	CHECK(DeriveVisualScriptNodeTitle(broken, document) == "Get Variable");
}

TEST_CASE("VisualScriptEditor: nodes of the scripting API's members are titled by the member's name in words")
{
	// The four forms of reflected nodes.
	CHECK(Title("Call:Strada.Input.IsKeyDown(Strada.KeyCode)") == "Is Key Down");
	CHECK(Title("Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)") == "Add Force");
	CHECK(Title("Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)") == "Raycast");
	CHECK(Title("Call:Strada.Entity.GetComponent<T>()", Json::object({{"TypeArguments", Json::array({"Strada.CameraComponent"})}})) ==
	      "Get Component");
	CHECK(Title("Call:Strada.Input.IsKeyDown") == "Is Key Down");
	CHECK(Title("Get:Strada.TransformComponent.Translation") == "Get Translation");
	CHECK(Title("Get:Strada.Time.DeltaTime") == "Get Delta Time");
	CHECK(Title("Set:Strada.TransformComponent.Translation") == "Set Translation");
	CHECK(Title("Set:Strada.CameraComponent.PerspectiveFOV") == "Set Perspective FOV");
	CHECK(Title("New:Strada.Vector3(float,float,float)") == "Make Vector3");
	CHECK(Title("New:Strada.Vector3(Strada.Vector2,float)") == "Make Vector3");
	// Types are titled as the runtime titles them: nested types by their own name, components without "Component",
	// arrays by their element.
	CHECK(Title("Call:Strada.Physics+Settings.Apply(int)") == "Apply");
	CHECK(Title("New:Strada.Physics+QueryOptions(float)") == "Make Query Options");
	CHECK(Title("New:Strada.RigidBodyComponent()") == "Make Rigid Body");
	CHECK(Title("New:Strada.Component()") == "Make Component");
	CHECK(Title("New:float[](int)") == "Make Float Array");
	CHECK(Title("New:Strada.UIButtonComponent[][](int)") == "Make UI Button Array Array");

	// Words, as the runtime splits names (Titles.Words): acronyms keep together, digits end words, '_' separates them.
	CHECK(Title("Call:Strada.UI.UIButton()") == "UI Button");
	CHECK(Title("Call:Strada.Mathf.Deg2Rad(float)") == "Deg2 Rad");
	CHECK(Title("Call:Strada.Camera.WorldToUV(Strada.Vector3)") == "World To UV");
	CHECK(Title("Get:Strada.Display.VSync") == "Get V Sync");
	CHECK(Title("Get:Game.Player.max_speed") == "Get max speed");
	CHECK(Title("Get:Game.Player.Max__Speed") == "Get Max Speed");
	CHECK(Title("Call:Game.Player.ÜberSchall()") == "Über Schall");
	CHECK(Title("Call:Game.Player.GrößeÄndern()") == "GrößeÄndern");

	// Types the runtime does not know: the words of their last name, as the runtime titles them; the type itself when it
	// ends with none.
	CHECK(Title("Plugin.Nodes.DoSomething") == "Do Something");
	CHECK(Title("Custom:Plugin.Nodes.Run(int)") == "Run");
	CHECK(Title("Flow.") == "Flow.");
	CHECK(Title("Call:") == "Call:");
	CHECK(Title("Get:") == "Get:");
	CHECK(Title("New:") == "New:");
	CHECK(Title("New:(int)") == "New:(int)");
	CHECK(Title("New:Strada.(int)") == "New:Strada.(int)");
	CHECK(Title("").empty());
}

TEST_CASE("VisualScriptEditor: the fixture library's nodes are titled as the library titles them")
{
	Result<std::string> const text =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Editor" / "Library.json");
	REQUIRE_MESSAGE(text.IsOk(), text.GetError());
	Result<Scope<VisualScriptLibrary>> const library = VisualScriptLibrary::Parse(text.GetValue());
	REQUIRE_MESSAGE(library.IsOk(), library.GetError());
	std::array<size_t, 4> const checked = CheckLibraryTitles(*library.GetValue());
	// Every form is there to check.
	CHECK(checked[0] > 0);
	CHECK(checked[1] > 0);
	CHECK(checked[2] > 0);
	CHECK(checked[3] > 0);
}

TEST_CASE("VisualScriptEditor: titles without an analysis are the runtime's, for every node kind and every node a setting names")
{
	Testing::ScriptRuntimeScope const runtime;
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);

	// Every built-in kind of the library, every member of the scripting API.
	std::array<size_t, 4> const checked = CheckLibraryTitles(*library);
	CHECK(checked[3] == BuiltInTitles.size());
	CHECK(checked[0] > 100);
	CHECK(checked[1] > 100);
	CHECK(checked[2] > 0);

	// The nodes settings title, as the analysis titles them.
	VisualScriptDocument const document = MakeNamingDocument();
	Result<VisualScriptAnalysis> const analysis = VisualScriptEngine::Analyze(document);
	REQUIRE_MESSAGE(analysis.IsOk(), analysis.GetError());
	for (auto const& [id, title] : GetNamingTitles())
	{
		CAPTURE(id);
		VisualScriptNode const* const node = FindNode(document, id);
		VisualScriptAnalyzedNode const* const analyzed = analysis.GetValue().FindNode(id);
		REQUIRE(node != nullptr);
		REQUIRE(analyzed != nullptr);
		std::string const derived = DeriveVisualScriptNodeTitle(*node, document);
		CHECK(derived == title);
		// Function.Entry is the one built-in kind the rule titles in fewer words than the library.
		CHECK(derived == (node->Type == "Function.Entry" ? std::string("Entry") : analyzed->Title));
	}
}

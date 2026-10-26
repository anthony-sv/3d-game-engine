#include "Editor/VisualScripting/VisualScriptPalette.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	// Kinds the editor's fixture library lacks, as the scripting runtime describes them: mouse and gamepad events (whose enum
	// settings expand like Key Pressed's) and Flow.Interval, the other node with a tick.
	constexpr std::string_view ExtraTypes = R"json([
		{ "ID": "Strada.MouseButton", "Title": "Mouse Button", "Kind": "Enum", "FieldType": "Int32", "Literal": true, "Flags": false,
		  "Enumerators": [ { "Name": "Left", "Value": 0 }, { "Name": "Right", "Value": 1 }, { "Name": "Middle", "Value": 2 } ],
		  "ImplicitTo": [ "System.Object" ] },
		{ "ID": "Strada.GamepadButton", "Title": "Gamepad Button", "Kind": "Enum", "FieldType": "Int32", "Literal": true, "Flags": false,
		  "Enumerators": [ { "Name": "A", "Value": 0 }, { "Name": "B", "Value": 1 } ], "ImplicitTo": [ "System.Object" ] }
	])json";
	constexpr std::string_view ExtraNodes = R"json([
		{ "Type": "Event.MouseButtonPressed", "Title": "Mouse Button Pressed", "Category": "Events",
		  "Description": "Runs in the frame the mouse button goes down.", "Keywords": [ "input", "press", "release", "mouse" ],
		  "Kind": "Event", "Pure": false, "Latent": false, "Pins": [ { "Name": "Then", "Direction": "Out", "Type": "exec" } ],
		  "Settings": [ { "Name": "Button", "Kind": "Enum", "Required": true, "Type": "Strada.MouseButton" } ] },
		{ "Type": "Event.GamepadButtonPressed", "Title": "Gamepad Button Pressed", "Category": "Events",
		  "Description": "Runs in the frame the button of the gamepad (0 to 15) goes down.", "Keywords": [ "input", "press", "gamepad" ],
		  "Kind": "Event", "Pure": false, "Latent": false,
		  "Pins": [ { "Name": "Gamepad", "Direction": "In", "Type": "int", "Default": 0 }, { "Name": "Then", "Direction": "Out", "Type": "exec" } ],
		  "Settings": [ { "Name": "Button", "Kind": "Enum", "Required": true, "Type": "Strada.GamepadButton" } ] },
		{ "Type": "Flow.Interval", "Title": "Interval", "Category": "Flow", "Description": "Runs Tick every Interval seconds until stopped.",
		  "Keywords": [ "repeat", "timer", "every" ], "Kind": "Latent", "Pure": false, "Latent": true,
		  "Pins": [ { "Name": "Execute", "Direction": "In", "Type": "exec" }, { "Name": "Stop", "Direction": "In", "Type": "exec" },
		            { "Name": "Interval", "Direction": "In", "Type": "float", "Default": 1 },
		            { "Name": "Tick", "Direction": "Out", "Type": "exec", "Role": "Tick" },
		            { "Name": "Completed", "Direction": "Out", "Type": "exec", "Role": "Continuation" } ],
		  "Settings": [] }
	])json";

	// The editor's node library with the kinds above, and as many more plain nodes as asked for.
	Scope<VisualScriptLibrary> ParseLibrary(uint32_t extraNodes = 0)
	{
		Result<std::string> text =
			FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Editor" / "Library.json");
		REQUIRE_MESSAGE(text.IsOk(), text.GetError());
		Result<Json> library = ParseJson(text.GetValue());
		REQUIRE(library.IsOk());
		Result<Json> types = ParseJson(ExtraTypes);
		Result<Json> nodes = ParseJson(ExtraNodes);
		REQUIRE(types.IsOk());
		REQUIRE(nodes.IsOk());
		for (Json const& type : types.GetValue())
		{
			library.GetValue()["Types"].push_back(type);
		}
		for (Json const& node : nodes.GetValue())
		{
			library.GetValue()["Nodes"].push_back(node);
		}
		for (uint32_t index = 0; index < extraNodes; index++)
		{
			std::string const name = "Generated" + std::to_string(index);
			library.GetValue()["Nodes"].push_back({{"Type", "Call:Strada.Generated." + name + "(float)"},
			                                       {"Title", "Generated Step " + std::to_string(index)},
			                                       {"Subtitle", "Generated"},
			                                       {"Category", "Generated/Group " + std::to_string(index % 10)},
			                                       {"Description", ""},
			                                       {"Keywords", Json::array({"synthetic"})},
			                                       {"Kind", "Call"},
			                                       {"Pure", false},
			                                       {"Latent", false},
			                                       {"Pins", Json::array({{{"Name", "Execute"}, {"Direction", "In"}, {"Type", "exec"}},
			                                                             {{"Name", "Amount"}, {"Direction", "In"}, {"Type", "float"}},
			                                                             {{"Name", "Then"}, {"Direction", "Out"}, {"Type", "exec"}}})},
			                                       {"Settings", Json::array()}});
		}
		Result<Scope<VisualScriptLibrary>> parsed = VisualScriptLibrary::Parse(library.GetValue().dump());
		REQUIRE_MESSAGE(parsed.IsOk(), parsed.GetError());
		return parsed.TakeValue();
	}

	VisualScriptDocument ParseDocument(std::string_view text)
	{
		Result<Json> json = ParseJson(text);
		REQUIRE_MESSAGE(json.IsOk(), json.GetError());
		DeserializationContext context;
		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> document = VisualScriptSerializer::DeserializeDocument(json.GetValue(), context, &issues);
		REQUIRE_MESSAGE(document.IsOk(), document.GetError());
		REQUIRE_MESSAGE(issues.empty(), (issues.empty() ? std::string() : issues.front().Path + ": " + issues.front().Message));
		return document.TakeValue();
	}

	// Variables, an impure function with a local, a pure function, a function without an entry, and an event graph that
	// handles Update, the Space key and the custom event Hit, which it also sends.
	constexpr std::string_view PlayerText = R"json({
		"Variables": [
			{ "ID": 1, "Name": "Speed", "Type": "float", "Tooltip": "How fast the player runs" },
			{ "ID": 2, "Name": "Score", "Type": "int" }
		],
		"Functions": [
			{
				"ID": 10, "Name": "AddScore", "Inputs": [ { "Name": "Amount", "Type": "int" } ], "Outputs": [ { "Name": "Total", "Type": "int" } ],
				"Locals": [ { "ID": 11, "Name": "Sum", "Type": "int" } ],
				"Graph": { "Nodes": [ { "ID": 12, "Type": "Function.Entry" }, { "ID": 13, "Type": "Function.Return" } ] }
			},
			{
				"ID": 20, "Name": "Half", "Pure": true, "Inputs": [ { "Name": "Value", "Type": "float" } ],
				"Outputs": [ { "Name": "Result", "Type": "float" } ],
				"Graph": { "Nodes": [ { "ID": 21, "Type": "Function.Entry" } ] }
			},
			{ "ID": 30, "Name": "Empty", "Graph": { "Nodes": [] } }
		],
		"Graph": {
			"Nodes": [
				{ "ID": 40, "Type": "Event.Update" },
				{ "ID": 41, "Type": "Event.KeyPressed", "Settings": { "Key": "Space" } },
				{ "ID": 42, "Type": "Event.Custom", "Settings": { "Name": "Hit", "Parameters": [ { "Name": "Damage", "Type": "float" } ] } },
				{ "ID": 43, "Type": "Event.MouseButtonPressed", "Settings": { "Button": 1 } }
			]
		}
	})json";

	// Every member of each pin, so pin lists compare whole (VisualScriptPin has no operator==).
	std::string DescribePins(std::vector<VisualScriptPin> const& pins)
	{
		std::string text;
		for (VisualScriptPin const& pin : pins)
		{
			text += fmt::format("{} {} {} label={} default={} self={} role={} constraint={} group={} element={} exec={};", pin.Name,
			                    pin.Direction == VisualScriptPinDirection::In ? "In" : "Out", pin.Type, pin.Label, pin.Default.dump(),
			                    pin.DefaultsToSelf, static_cast<int>(pin.Role), static_cast<int>(pin.Constraint), pin.Group, pin.ElementOf,
			                    pin.CarriesExec);
		}
		return text;
	}

	VisualScriptPaletteIndex MakeIndex(VisualScriptLibrary const& library, VisualScriptDocument const& document, uint32_t function)
	{
		VisualScriptPaletteIndex index;
		index.Rebuild(library);
		index.SetDocument(document, function);
		return index;
	}

	std::optional<size_t> FindEntry(VisualScriptPaletteIndex const& index, std::string_view title)
	{
		std::span<VisualScriptPaletteEntry const> const entries = index.GetEntries();
		auto const found = std::find_if(entries.begin(), entries.end(),
		                                [title](VisualScriptPaletteEntry const& entry)
		                                {
											return entry.Title == title;
										});
		return found != entries.end() ? std::optional<size_t>(static_cast<size_t>(found - entries.begin())) : std::nullopt;
	}

	VisualScriptPaletteEntry const& GetEntry(VisualScriptPaletteIndex const& index, std::string_view title)
	{
		std::optional<size_t> const found = FindEntry(index, title);
		REQUIRE_MESSAGE(found.has_value(), title);
		return index.GetEntries()[*found];
	}

	VisualScriptPin const* FindPin(VisualScriptPaletteEntry const& entry, std::string_view name, VisualScriptPinDirection direction)
	{
		auto const found = std::find_if(entry.Pins.begin(), entry.Pins.end(),
		                                [&](VisualScriptPin const& pin)
		                                {
											return pin.Name == name && pin.Direction == direction;
										});
		return found != entry.Pins.end() ? &*found : nullptr;
	}

	std::vector<std::string> RankedTitles(VisualScriptPaletteIndex const& index, std::vector<VisualScriptPaletteMatch> const& matches)
	{
		std::vector<std::string> titles;
		for (VisualScriptPaletteMatch const& match : matches)
		{
			titles.push_back(index.GetEntries()[match.Entry].Title);
		}
		return titles;
	}

	std::vector<VisualScriptPaletteMatch> Rank(VisualScriptPaletteIndex const& index, VisualScriptLibrary const& library,
	                                           std::string_view query, std::vector<std::string> const& recents = {},
	                                           VisualScriptPaletteContext const* context = nullptr)
	{
		return RankVisualScriptPalette(index, query, recents, context, library);
	}

	VisualScriptPin MakePin(std::string name, VisualScriptPinDirection direction, std::string type)
	{
		VisualScriptPin pin;
		pin.Name = std::move(name);
		pin.Direction = direction;
		pin.Type = std::move(type);
		return pin;
	}

	// The pins whose wildcard only the settings of a generic kind type, as spec section 5.9 lists them, and every wildcard of
	// a kind that takes TypeArguments.
	bool IsSettingsTypedPin(VisualScriptLibrary const& library, VisualScriptPaletteEntry const& entry, VisualScriptPin const& pin)
	{
		if (pin.Type != VisualScriptWildcardType || !entry.Settings.empty())
		{
			return false;
		}
		VisualScriptNodeTemplate const* node = library.FindNode(entry.NodeType);
		if (node != nullptr && std::any_of(node->Settings.begin(), node->Settings.end(),
		                                   [](VisualScriptSetting const& setting)
		                                   {
											   return setting.Name == "TypeArguments";
										   }))
		{
			return true;
		}
		std::string_view const type = entry.NodeType;
		bool const in = pin.Direction == VisualScriptPinDirection::In;
		return (type == "Variable.Get" && pin.Name == "Value") || (type == "Variable.Set" && pin.Name == "Value") ||
		       (type == "Literal" && pin.Name == "Value") || (type == "Convert" && pin.Name == "Result") ||
		       (type == "Make" && pin.Name == "Value") || (type == "Break" && pin.Name == "Value") ||
		       (type == "Flow.Switch" && in && pin.Name == "Selection");
	}

	bool ArePerformanceTestsEnabled()
	{
		std::optional<std::string> const enabled = Platform::ReadEnvironmentVariable("STRADA_PERFORMANCE_TESTS");
		return enabled.has_value() && *enabled == "1";
	}
}

TEST_CASE("VisualScriptPalette: the index offers every library kind with its typed expansions, the document's elements and the actions")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument const document = ParseDocument(PlayerText);
	VisualScriptPaletteIndex const index = MakeIndex(*library, document, 0);
	std::span<VisualScriptPaletteEntry const> const entries = index.GetEntries();

	// Every node kind once (Function.Entry and Function.Return only in functions' graphs), in library order, as it is.
	size_t previous = 0;
	for (VisualScriptNodeTemplate const& node : library->GetNodes())
	{
		auto const found = std::find_if(entries.begin(), entries.end(),
		                                [&node](VisualScriptPaletteEntry const& entry)
		                                {
											return entry.NodeType == node.Type && entry.Settings.empty() &&
			                                       entry.Action != VisualScriptPaletteAction::AddReroute &&
			                                       entry.Action != VisualScriptPaletteAction::AddCustomEvent;
										});
		if (node.Type == "Function.Entry" || node.Type == "Function.Return")
		{
			CHECK(found == entries.end());
			continue;
		}
		REQUIRE_MESSAGE(found != entries.end(), node.Type);
		size_t const position = static_cast<size_t>(found - entries.begin());
		CHECK(position >= previous);
		previous = position;
		CHECK(found->Subtitle == node.Subtitle);
		CHECK(found->Category == node.Category);
		CHECK(found->Description == node.Description);
		CHECK(found->Keywords == node.Keywords);
		CHECK(found->Kind == node.Kind);
		CHECK(DescribePins(found->Pins) == DescribePins(node.Pins));
		CHECK_FALSE(found->FromDocument);
		CHECK(found->RecentKey == node.Type + "{}");
	}
	CHECK(GetEntry(index, "Branch").RecentKey == "Flow.Branch{}");

	// Input events: one per key or button.
	VisualScriptTypeInfo const* keys = library->FindType("Strada.KeyCode");
	REQUIRE(keys != nullptr);
	size_t const keyEntries = static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
	                                                            [](VisualScriptPaletteEntry const& entry)
	                                                            {
																	return entry.NodeType == "Event.KeyPressed" && !entry.Settings.empty();
																}));
	CHECK(keyEntries == keys->Enumerators.size());
	VisualScriptPaletteEntry const& keyW = GetEntry(index, "Key Pressed: W");
	CHECK(keyW.Settings == Json::object({{"Key", "W"}}));
	CHECK(DescribePins(keyW.Pins) == DescribePins(library->FindNode("Event.KeyPressed")->Pins));
	CHECK(keyW.RecentKey == R"(Event.KeyPressed{"Key":"W"})");
	CHECK(GetEntry(index, "Mouse Button Pressed: Left").Settings == Json::object({{"Button", "Left"}}));
	CHECK(GetEntry(index, "Gamepad Button Pressed: A").Settings == Json::object({{"Button", "A"}}));

	// Generic calls: one per component or asset class, never the abstract bases.
	VisualScriptPaletteEntry const& rigidBody = GetEntry(index, "Get Component: Rigid Body");
	CHECK(rigidBody.Settings == Json::object({{"TypeArguments", Json::array({"Strada.RigidBodyComponent"})}}));
	CHECK(rigidBody.Subtitle == "Target is Entity");
	VisualScriptPin const* result = FindPin(rigidBody, "Result", VisualScriptPinDirection::Out);
	REQUIRE(result != nullptr);
	CHECK(result->Type == "Strada.RigidBodyComponent");
	CHECK(FindEntry(index, "Get Component: Transform").has_value());
	CHECK(FindEntry(index, "Get Component: Particle System").has_value());
	CHECK_FALSE(FindEntry(index, "Get Component: Component").has_value());
	VisualScriptPaletteEntry const& loadPrefab = GetEntry(index, "Load: Prefab");
	CHECK(loadPrefab.Settings == Json::object({{"TypeArguments", Json::array({"Strada.Prefab"})}}));
	CHECK_FALSE(FindEntry(index, "Load: Asset").has_value());

	// Make and Break: one per makeable or breakable struct, with a pin per field.
	VisualScriptPaletteEntry const& makeVector = GetEntry(index, "Make Vector3");
	CHECK(makeVector.NodeType == "Make");
	CHECK(makeVector.Settings == Json::object({{"Type", "Strada.Vector3"}}));
	CHECK(DescribePins(makeVector.Pins) ==
	      DescribePins(std::vector<VisualScriptPin>{
			  MakePin("X", VisualScriptPinDirection::In, "float"), MakePin("Y", VisualScriptPinDirection::In, "float"),
			  MakePin("Z", VisualScriptPinDirection::In, "float"), MakePin("Value", VisualScriptPinDirection::Out, "Strada.Vector3")}));
	VisualScriptPaletteEntry const& breakHit = GetEntry(index, "Break Raycast Hit");
	CHECK(DescribePins(breakHit.Pins) ==
	      DescribePins(std::vector<VisualScriptPin>{MakePin("Value", VisualScriptPinDirection::In, "Strada.RaycastHit"),
	                                                MakePin("Entity", VisualScriptPinDirection::Out, "Strada.Entity"),
	                                                MakePin("Point", VisualScriptPinDirection::Out, "Strada.Vector3"),
	                                                MakePin("Normal", VisualScriptPinDirection::Out, "Strada.Vector3"),
	                                                MakePin("Distance", VisualScriptPinDirection::Out, "float")}));
	CHECK_FALSE(FindEntry(index, "Make Raycast Hit").has_value());
	CHECK(FindEntry(index, "Make Matrix4").has_value());

	// Switch: on int, on string and on each enum.
	VisualScriptPaletteEntry const& switchInt = GetEntry(index, "Switch on Int");
	CHECK(switchInt.Settings == Json::object({{"Type", "int"}}));
	VisualScriptPin const* selection = FindPin(switchInt, "Selection", VisualScriptPinDirection::In);
	REQUIRE(selection != nullptr);
	CHECK(selection->Type == "int");
	CHECK(FindEntry(index, "Switch on String").has_value());
	CHECK(FindEntry(index, "Switch on Force Mode").has_value());
	CHECK(FindEntry(index, "Switch on Key Code").has_value());
	CHECK_FALSE(FindEntry(index, "Switch on Float").has_value());

	// Literal: one per type of the kinds with plain literals.
	for (std::string_view const title : {"Bool Literal", "Int Literal", "Long Literal", "Float Literal", "Double Literal", "String Literal",
	                                     "Vector3 Literal", "Vector4 Literal", "Quaternion Literal", "Color Literal"})
	{
		CHECK_MESSAGE(FindEntry(index, title).has_value(), title);
	}
	VisualScriptPaletteEntry const& floatLiteral = GetEntry(index, "Float Literal");
	CHECK(floatLiteral.Settings == Json::object({{"Type", "float"}}));
	CHECK(DescribePins(floatLiteral.Pins) ==
	      DescribePins(std::vector<VisualScriptPin>{MakePin("Value", VisualScriptPinDirection::Out, "float")}));
	CHECK_FALSE(FindEntry(index, "Entity Literal").has_value());
	CHECK_FALSE(FindEntry(index, "Force Mode Literal").has_value());
	CHECK_FALSE(FindEntry(index, "Matrix4 Literal").has_value());

	// Convert: to text and to each numeric type, the conversions every value and every number has.
	VisualScriptPaletteEntry const& toInt = GetEntry(index, "To Int");
	CHECK(toInt.Settings == Json::object({{"To", "int"}}));
	VisualScriptPin const* converted = FindPin(toInt, "Result", VisualScriptPinDirection::Out);
	REQUIRE(converted != nullptr);
	CHECK(converted->Type == "int");
	CHECK(FindEntry(index, "To String").has_value());
	CHECK(FindEntry(index, "To ULong").has_value());
	CHECK_FALSE(FindEntry(index, "To Vector3").has_value());

	// The document's variables, functions and custom events.
	VisualScriptPaletteEntry const& getSpeed = GetEntry(index, "Get Speed");
	CHECK(getSpeed.Category == "Variables");
	CHECK(getSpeed.Subtitle == "Float");
	CHECK(getSpeed.Description == "How fast the player runs");
	CHECK(getSpeed.NodeType == "Variable.Get");
	CHECK(getSpeed.Settings == Json::object({{"Variable", 1}}));
	CHECK(getSpeed.Kind == VisualScriptNodeKind::Variable);
	CHECK(getSpeed.FromDocument);
	CHECK(DescribePins(getSpeed.Pins) ==
	      DescribePins(std::vector<VisualScriptPin>{MakePin("Value", VisualScriptPinDirection::Out, "float")}));
	CHECK(getSpeed.RecentKey == R"(Variable.Get{"Variable":1})");
	VisualScriptPaletteEntry const& setScore = GetEntry(index, "Set Score");
	CHECK(DescribePins(setScore.Pins) ==
	      DescribePins(std::vector<VisualScriptPin>{
			  MakePin("Execute", VisualScriptPinDirection::In, "exec"), MakePin("Value", VisualScriptPinDirection::In, "int"),
			  MakePin("Then", VisualScriptPinDirection::Out, "exec"), MakePin("Value", VisualScriptPinDirection::Out, "int")}));
	// Locals only in their function's graph.
	CHECK_FALSE(FindEntry(index, "Get Sum").has_value());

	VisualScriptPaletteEntry const& addScore = GetEntry(index, "AddScore");
	CHECK(addScore.Category == "Functions");
	CHECK(addScore.Subtitle == "(Int Amount) returns Int Total");
	CHECK(addScore.NodeType == "Function.Call");
	CHECK(addScore.Settings == Json::object({{"Function", 10}}));
	CHECK(addScore.Kind == VisualScriptNodeKind::Function);
	CHECK(DescribePins(addScore.Pins) ==
	      DescribePins(std::vector<VisualScriptPin>{
			  MakePin("Execute", VisualScriptPinDirection::In, "exec"), MakePin("Amount", VisualScriptPinDirection::In, "int"),
			  MakePin("Then", VisualScriptPinDirection::Out, "exec"), MakePin("Total", VisualScriptPinDirection::Out, "int")}));
	VisualScriptPaletteEntry const& half = GetEntry(index, "Half");
	CHECK(half.Subtitle == "(Float Value) returns Float Result");
	CHECK(DescribePins(half.Pins) == DescribePins(std::vector<VisualScriptPin>{MakePin("Value", VisualScriptPinDirection::In, "float"),
	                                                                           MakePin("Result", VisualScriptPinDirection::Out, "float")}));
	CHECK(GetEntry(index, "Empty").Subtitle == "()");

	VisualScriptPaletteEntry const& sendHit = GetEntry(index, "Send Hit");
	CHECK(sendHit.Category == "Events");
	CHECK(sendHit.Subtitle == "(Float Damage)");
	CHECK(sendHit.NodeType == "Event.Send");
	CHECK(sendHit.Settings ==
	      Json::object({{"Name", "Hit"}, {"Parameters", Json::array({Json::object({{"Name", "Damage"}, {"Type", "float"}})})}}));
	CHECK(sendHit.FromDocument);
	REQUIRE(sendHit.Pins.size() == 4);
	CHECK(sendHit.Pins[1].Name == "Target");
	CHECK(sendHit.Pins[1].DefaultsToSelf);
	CHECK(DescribePins({sendHit.Pins[2]}) == DescribePins({MakePin("Damage", VisualScriptPinDirection::In, "float")}));

	// The actions come last.
	std::vector<VisualScriptPaletteAction> actions;
	for (size_t at = entries.size() - 4; at < entries.size(); at++)
	{
		actions.push_back(entries[at].Action);
	}
	CHECK(actions == std::vector<VisualScriptPaletteAction>{VisualScriptPaletteAction::AddCommentBox, VisualScriptPaletteAction::AddReroute,
	                                                        VisualScriptPaletteAction::AddCustomEvent,
	                                                        VisualScriptPaletteAction::AddVariable});
	CHECK(entries[entries.size() - 4].Title == "Add Comment Box");
	CHECK(entries[entries.size() - 4].RecentKey == "Action:AddCommentBox");
	CHECK(entries[entries.size() - 3].NodeType == "Reroute");
	CHECK(DescribePins(entries[entries.size() - 3].Pins) == DescribePins(library->FindNode("Reroute")->Pins));
	CHECK(entries[entries.size() - 2].NodeType == "Event.Custom");

	// The document's entries follow the library's.
	CHECK(*FindEntry(index, "Get Speed") > *FindEntry(index, "To ULong"));
	CHECK(*FindEntry(index, "Send Hit") < entries.size() - 4);

	// Without a library, only the document's entries and the actions.
	VisualScriptPaletteIndex bare;
	bare.SetDocument(document, 0);
	CHECK(bare.GetEntries().size() == 2 * 2 + 3 + 1 + 4);
	CHECK(bare.GetEntries().front().Title == "Get Speed");
	CHECK(bare.GetEntries().back().Title == "Add Variable");
}

TEST_CASE("VisualScriptPalette: each graph offers what it can use, and unique events it handles already are gone to")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument const document = ParseDocument(PlayerText);

	// The event graph: no function ends; the events it handles are offered as GoToNode.
	VisualScriptPaletteIndex index = MakeIndex(*library, document, 0);
	CHECK_FALSE(FindEntry(index, "Function Entry").has_value());
	CHECK_FALSE(FindEntry(index, "Return").has_value());
	CHECK_FALSE(FindEntry(index, "Update").has_value());
	VisualScriptPaletteEntry const& update = GetEntry(index, "Update (go to)");
	CHECK(update.Action == VisualScriptPaletteAction::GoToNode);
	CHECK(update.ExistingNode == 40);
	CHECK(update.RecentKey == "Event.Update{}");
	CHECK(GetEntry(index, "Key Pressed: Space (go to)").ExistingNode == 41);
	// An input event's value as an integer names the same button.
	CHECK(GetEntry(index, "Mouse Button Pressed: Right (go to)").ExistingNode == 43);
	CHECK(GetEntry(index, "Mouse Button Pressed: Left").Action == VisualScriptPaletteAction::AddNode);
	CHECK(GetEntry(index, "Key Pressed: W").Action == VisualScriptPaletteAction::AddNode);
	CHECK(GetEntry(index, "Create").Action == VisualScriptPaletteAction::AddNode);
	// Custom events are unique by name: the generic one stays.
	CHECK(GetEntry(index, "Custom Event").Action == VisualScriptPaletteAction::AddNode);
	CHECK(FindEntry(index, "Tween").has_value());
	CHECK(FindEntry(index, "Add Custom Event").has_value());

	// An impure function's graph: no events, nothing that ticks, no second entry; returns, flow and its locals.
	index.SetDocument(document, 10);
	std::span<VisualScriptPaletteEntry const> entries = index.GetEntries();
	CHECK(std::none_of(entries.begin(), entries.end(),
	                   [](VisualScriptPaletteEntry const& entry)
	                   {
						   return entry.Kind == VisualScriptNodeKind::Event;
					   }));
	CHECK_FALSE(FindEntry(index, "Tween").has_value());
	CHECK_FALSE(FindEntry(index, "Interval").has_value());
	CHECK_FALSE(FindEntry(index, "Function Entry").has_value());
	CHECK_FALSE(FindEntry(index, "Add Custom Event").has_value());
	CHECK_FALSE(FindEntry(index, "Key Pressed: W").has_value());
	CHECK(FindEntry(index, "Return").has_value());
	CHECK(FindEntry(index, "Delay").has_value());
	CHECK(FindEntry(index, "Send Hit").has_value());
	CHECK(FindEntry(index, "Set Speed").has_value());
	VisualScriptPaletteEntry const& getSum = GetEntry(index, "Get Sum");
	CHECK(getSum.Subtitle == "Int (local)");
	CHECK(getSum.Settings == Json::object({{"Variable", 11}}));
	CHECK(FindEntry(index, "Set Sum").has_value());
	CHECK(FindEntry(index, "AddScore").has_value());

	// A function without an entry is offered one.
	index.SetDocument(document, 30);
	CHECK(FindEntry(index, "Function Entry").has_value());
	CHECK_FALSE(FindEntry(index, "Get Sum").has_value());

	// A pure function's graph: pure nodes only, and one Return while it has none.
	index.SetDocument(document, 20);
	entries = index.GetEntries();
	CHECK(FindEntry(index, "Return").has_value());
	CHECK_FALSE(FindEntry(index, "Function Entry").has_value());
	for (std::string_view const impure : {"Branch", "Delay", "Set Speed", "Send Hit", "AddScore", "Info", "Add Force", "Sequence"})
	{
		CHECK_MESSAGE(!FindEntry(index, impure).has_value(), impure);
	}
	for (std::string_view const pure : {"Get Speed", "Half", "Add", "Make Vector3", "Is Key Down", "Add Comment Box", "Add Reroute"})
	{
		CHECK_MESSAGE(FindEntry(index, pure).has_value(), pure);
	}
	VisualScriptDocument withReturn = document;
	withReturn.Functions[1].Graph.Nodes.push_back({});
	withReturn.Functions[1].Graph.Nodes.back().ID = 22;
	withReturn.Functions[1].Graph.Nodes.back().Type = "Function.Return";
	index.SetDocument(withReturn, 20);
	CHECK_FALSE(FindEntry(index, "Return").has_value());

	// A function the document does not have shows the event graph's entries, without locals.
	index.SetDocument(document, 999);
	CHECK(FindEntry(index, "Update (go to)").has_value());
	CHECK_FALSE(FindEntry(index, "Return").has_value());

	// Rebuilding with another library keeps the document.
	index.SetDocument(document, 10);
	index.Rebuild(*library);
	CHECK(FindEntry(index, "Get Sum").has_value());
}

TEST_CASE("VisualScriptPalette: queries rank by how each word matches, recency, the document and title length")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument const document = ParseDocument(PlayerText);
	VisualScriptPaletteIndex const index = MakeIndex(*library, document, 0);

	// No query and no context: nothing ranked (the palette shows recents and categories).
	CHECK(Rank(index, *library, "").empty());
	CHECK(Rank(index, *library, "   ").empty());

	// Initials.
	std::vector<VisualScriptPaletteMatch> const addForce = Rank(index, *library, "af");
	REQUIRE_FALSE(addForce.empty());
	CHECK(index.GetEntries()[addForce.front().Entry].Title == "Add Force");
	CHECK(addForce.front().Score == 500 - 2 * 9);
	CHECK(addForce.front().Highlights == std::vector<uint16_t>{0, 4});
	CHECK(RankedTitles(index, Rank(index, *library, "gc")).front() == "Get Component");

	// The whole title.
	std::vector<VisualScriptPaletteMatch> const branch = Rank(index, *library, "branch");
	REQUIRE_FALSE(branch.empty());
	CHECK(index.GetEntries()[branch.front().Entry].Title == "Branch");
	CHECK(branch.front().Score == 1000 + 700 - 2 * 6);
	CHECK(branch.front().Highlights == std::vector<uint16_t>{0, 1, 2, 3, 4, 5});
	// Case and extra spaces do not matter.
	CHECK(Rank(index, *library, "  BRANCH ").front().Score == branch.front().Score);

	// The document's variable above the library's getter with the same words.
	std::vector<VisualScriptPaletteMatch> const getSpeed = Rank(index, *library, "get speed");
	std::vector<std::string> const getSpeedTitles = RankedTitles(index, getSpeed);
	REQUIRE(getSpeedTitles.size() >= 2);
	CHECK(getSpeedTitles[0] == "Get Speed");
	CHECK(getSpeed[0].Score == 1000 + 700 + 600 + 50 - 2 * 9);
	auto const speedMax = std::find(getSpeedTitles.begin(), getSpeedTitles.end(), "Get Speed Max");
	REQUIRE(speedMax != getSpeedTitles.end());
	CHECK(getSpeed[static_cast<size_t>(speedMax - getSpeedTitles.begin())].Score == 700 + 600 - 2 * 13);

	// The node type.
	std::vector<VisualScriptPaletteMatch> const isKeyDown = Rank(index, *library, "iskeydown");
	REQUIRE(isKeyDown.size() == 1);
	CHECK(index.GetEntries()[isKeyDown.front().Entry].Title == "Is Key Down");
	CHECK(isKeyDown.front().Score == 150 - 2 * 11);
	CHECK(isKeyDown.front().Highlights.empty());

	// Keywords, subtitles and categories.
	CHECK(Rank(index, *library, "timer").front().Score == 350 - 2 * 5);
	CHECK(RankedTitles(index, Rank(index, *library, "timer")).front() == "Delay");
	std::vector<VisualScriptPaletteMatch> const rigid = Rank(index, *library, "rigid");
	CHECK(RankedTitles(index, rigid).front() == "Get Component: Rigid Body");
	std::vector<std::string> const rigidTitles = RankedTitles(index, rigid);
	auto const addForceByCategory = std::find(rigidTitles.begin(), rigidTitles.end(), "Add Force");
	REQUIRE(addForceByCategory != rigidTitles.end());
	CHECK(rigid[static_cast<size_t>(addForceByCategory - rigidTitles.begin())].Score == 250 - 2 * 9);

	// A subsequence of three characters or more, with a gap cost.
	std::vector<VisualScriptPaletteMatch> const scattered = Rank(index, *library, "adfrc");
	REQUIRE_FALSE(scattered.empty());
	CHECK(index.GetEntries()[scattered.front().Entry].Title == "Add Force");
	CHECK(scattered.front().Highlights == std::vector<uint16_t>{0, 1, 4, 6, 7});
	CHECK(scattered.front().Score == 100 - 5 * 2 - 2 * 9);
	CHECK(Rank(index, *library, "zzz").empty());
	// Every word must match.
	CHECK(Rank(index, *library, "branch zzz").empty());
	// At most 8 words: the ninth is not looked for.
	CHECK_FALSE(Rank(index, *library, "b b b b b b b b zzz").empty());

	// Equal scores keep the index's order.
	std::vector<VisualScriptPaletteMatch> const keys = Rank(index, *library, "key pressed");
	for (size_t at = 1; at < keys.size(); at++)
	{
		CHECK(keys[at - 1].Score >= keys[at].Score);
		if (keys[at - 1].Score == keys[at].Score)
		{
			CHECK(keys[at - 1].Entry < keys[at].Entry);
		}
	}
	std::vector<std::string> const keyTitles = RankedTitles(index, keys);
	auto const keyA = std::find(keyTitles.begin(), keyTitles.end(), "Key Pressed: A");
	auto const keyB = std::find(keyTitles.begin(), keyTitles.end(), "Key Pressed: B");
	REQUIRE(keyA != keyTitles.end());
	REQUIRE(keyB != keyTitles.end());
	CHECK(keyA < keyB);

	// A recent entry outranks an equal match, by its place among the recents.
	std::string const recentB = GetEntry(index, "Key Pressed: B").RecentKey;
	std::vector<VisualScriptPaletteMatch> const recent = Rank(index, *library, "key pressed", {"Flow.Branch{}", recentB});
	std::vector<std::string> const recentTitles = RankedTitles(index, recent);
	auto const recentA = std::find(recentTitles.begin(), recentTitles.end(), "Key Pressed: A");
	auto const recentKeyB = std::find(recentTitles.begin(), recentTitles.end(), "Key Pressed: B");
	REQUIRE(recentA != recentTitles.end());
	REQUIRE(recentKeyB != recentTitles.end());
	CHECK(recentKeyB < recentA);
	CHECK(recent[static_cast<size_t>(recentKeyB - recentTitles.begin())].Score ==
	      keys[static_cast<size_t>(keyB - keyTitles.begin())].Score + 300 - 15);
	// Only the 20 most recent count.
	std::vector<std::string> many(20, "Flow.Branch{}");
	many.push_back(recentB);
	std::vector<VisualScriptPaletteMatch> const stale = Rank(index, *library, "key pressed", many);
	CHECK(RankedTitles(index, stale) == keyTitles);
}

TEST_CASE("VisualScriptPalette: a dragged pin lists what CanConnect connects to it, never through a settings-typed wildcard")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument const document = ParseDocument(PlayerText);
	VisualScriptPaletteIndex const index = MakeIndex(*library, document, 10);
	std::span<VisualScriptPaletteEntry const> const entries = index.GetEntries();

	std::vector<VisualScriptPin> dragged = {
		MakePin("Value", VisualScriptPinDirection::Out, "float"),
		MakePin("Amount", VisualScriptPinDirection::In, "int"),
		MakePin("Then", VisualScriptPinDirection::Out, "exec"),
		MakePin("Execute", VisualScriptPinDirection::In, "exec"),
		MakePin("Self", VisualScriptPinDirection::Out, "Strada.Entity"),
		MakePin("Force", VisualScriptPinDirection::In, "Strada.Vector3"),
		MakePin("Hit", VisualScriptPinDirection::Out, "Strada.RaycastHit"),
		MakePin("Message", VisualScriptPinDirection::In, "string"),
		MakePin("Body", VisualScriptPinDirection::Out, "Strada.RigidBodyComponent"),
	};
	for (VisualScriptPin const& pin : dragged)
	{
		CAPTURE(pin.Name);
		CAPTURE(pin.Type);
		VisualScriptPaletteContext const context{pin};
		std::vector<VisualScriptPaletteMatch> const matches = Rank(index, *library, "", {}, &context);

		// Brute force over every entry and pin, Convert's rows apart.
		std::set<uint32_t> expected;
		bool anyExact = false;
		for (size_t at = 0; at < entries.size(); at++)
		{
			VisualScriptPaletteEntry const& entry = entries[at];
			bool const convertRow = entry.NodeType == "Convert" && !entry.Settings.empty();
			if (entry.Action == VisualScriptPaletteAction::GoToNode || convertRow)
			{
				continue;
			}
			for (VisualScriptPin const& candidate : entry.Pins)
			{
				if (candidate.Direction == pin.Direction || IsSettingsTypedPin(*library, entry, candidate))
				{
					continue;
				}
				bool const connects = pin.Direction == VisualScriptPinDirection::Out ? library->CanConnect(pin, candidate)
				                                                                     : library->CanConnect(candidate, pin);
				if (connects)
				{
					expected.insert(static_cast<uint32_t>(at));
					anyExact = anyExact || candidate.Type == pin.Type;
				}
			}
		}
		std::set<uint32_t> listed;
		for (VisualScriptPaletteMatch const& match : matches)
		{
			VisualScriptPaletteEntry const& entry = entries[match.Entry];
			if (entry.NodeType == "Convert" && !entry.Settings.empty())
			{
				// Convert's rows only when nothing takes the type as it is.
				CHECK_FALSE(anyExact);
				continue;
			}
			listed.insert(match.Entry);
			// The pin it connects is one that connects, and the score says how.
			VisualScriptPin const* connected =
				FindPin(entry, match.ConnectPin,
			            pin.Direction == VisualScriptPinDirection::Out ? VisualScriptPinDirection::In : VisualScriptPinDirection::Out);
			REQUIRE_MESSAGE(connected != nullptr, entry.Title);
			bool const exact = connected->Type == pin.Type;
			bool const converts = !exact && connected->Type != VisualScriptWildcardType && pin.Type != VisualScriptExecType;
			CHECK(match.Converts == converts);
			int32_t const expectedScore =
				(exact ? 250 : (converts ? 100 : 0)) + (entry.FromDocument ? 50 : 0) - 2 * static_cast<int32_t>(entry.Title.size());
			CHECK_MESSAGE(match.Score == expectedScore, entry.Title);
		}
		CHECK(listed == expected);
	}

	// Settings-typed wildcards connect nothing; their typed expansions do.
	VisualScriptPaletteContext const floatOutput{MakePin("Value", VisualScriptPinDirection::Out, "float")};
	std::vector<std::string> const floatTitles = RankedTitles(index, Rank(index, *library, "", {}, &floatOutput));
	CHECK(std::find(floatTitles.begin(), floatTitles.end(), "Set Variable") == floatTitles.end());
	CHECK(std::find(floatTitles.begin(), floatTitles.end(), "Set Speed") != floatTitles.end());
	CHECK(std::find(floatTitles.begin(), floatTitles.end(), "Break") == floatTitles.end());
	CHECK(std::find(floatTitles.begin(), floatTitles.end(), "Make Vector3") != floatTitles.end());
	// Wildcards with meaning stay: operators, Select, Reroute and Convert's input.
	CHECK(std::find(floatTitles.begin(), floatTitles.end(), "Add") != floatTitles.end());
	CHECK(std::find(floatTitles.begin(), floatTitles.end(), "Select") != floatTitles.end());
	CHECK(std::find(floatTitles.begin(), floatTitles.end(), "Reroute") != floatTitles.end());
	CHECK(std::find(floatTitles.begin(), floatTitles.end(), "Convert") != floatTitles.end());
	VisualScriptPaletteContext const intInput{MakePin("Amount", VisualScriptPinDirection::In, "int")};
	std::vector<std::string> const intTitles = RankedTitles(index, Rank(index, *library, "", {}, &intInput));
	CHECK(std::find(intTitles.begin(), intTitles.end(), "Get Variable") == intTitles.end());
	CHECK(std::find(intTitles.begin(), intTitles.end(), "Literal") == intTitles.end());
	CHECK(std::find(intTitles.begin(), intTitles.end(), "Int Literal") != intTitles.end());
	CHECK(std::find(intTitles.begin(), intTitles.end(), "Get Sum") != intTitles.end());
	// Exec reaches exec and reroutes only; the best fit comes first.
	VisualScriptPaletteContext const execOutput{MakePin("Then", VisualScriptPinDirection::Out, "exec")};
	std::vector<VisualScriptPaletteMatch> const execMatches = Rank(index, *library, "", {}, &execOutput);
	std::vector<std::string> const execTitles = RankedTitles(index, execMatches);
	CHECK(std::find(execTitles.begin(), execTitles.end(), "Add Reroute") != execTitles.end());
	CHECK(std::find(execTitles.begin(), execTitles.end(), "Get Speed") == execTitles.end());
	auto const info = std::find(execTitles.begin(), execTitles.end(), "Info");
	REQUIRE(info != execTitles.end());
	CHECK(execMatches[static_cast<size_t>(info - execTitles.begin())].ConnectPin == "Execute");
	// A query narrows the context's list.
	std::vector<std::string> const queried = RankedTitles(index, Rank(index, *library, "set", {}, &floatOutput));
	CHECK(queried.front() == "Set Speed");
	// GoToNode entries are not listed: they connect nothing.
	VisualScriptPaletteIndex const eventGraph = MakeIndex(*library, document, 0);
	VisualScriptPaletteContext const execInput{MakePin("Execute", VisualScriptPinDirection::In, "exec")};
	std::vector<std::string> const eventTitles = RankedTitles(eventGraph, Rank(eventGraph, *library, "", {}, &execInput));
	CHECK(std::find(eventTitles.begin(), eventTitles.end(), "Update (go to)") == eventTitles.end());
	CHECK(std::find(eventTitles.begin(), eventTitles.end(), "Create") != eventTitles.end());
}

TEST_CASE("VisualScriptPalette: Convert's rows bridge to text and between numbers, unasked only when nothing takes the type as it is")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument const document = ParseDocument(PlayerText);
	VisualScriptPaletteIndex const index = MakeIndex(*library, document, 0);
	auto const convertRows = [&index](std::vector<VisualScriptPaletteMatch> const& matches)
	{
		std::vector<std::string> titles;
		for (VisualScriptPaletteMatch const& match : matches)
		{
			VisualScriptPaletteEntry const& entry = index.GetEntries()[match.Entry];
			if (entry.NodeType == "Convert" && !entry.Settings.empty())
			{
				titles.push_back(entry.Title);
			}
		}
		return titles;
	};

	// A float output: many inputs take a float, so no row without a query; asked for, numbers and text.
	VisualScriptPaletteContext const floatOutput{MakePin("Value", VisualScriptPinDirection::Out, "float")};
	CHECK(convertRows(Rank(index, *library, "", {}, &floatOutput)).empty());
	std::vector<VisualScriptPaletteMatch> const toInt = Rank(index, *library, "to int", {}, &floatOutput);
	REQUIRE_FALSE(toInt.empty());
	CHECK(index.GetEntries()[toInt.front().Entry].Title == "To Int");
	CHECK(toInt.front().ConnectPin == "Value");
	CHECK_FALSE(toInt.front().Converts);
	CHECK(toInt.front().Score == 1000 + 700 + 600 - 2 * 6);
	CHECK(convertRows(toInt) == std::vector<std::string>{"To Int", "To UInt"});
	std::vector<VisualScriptPaletteMatch> const toText = Rank(index, *library, "to string", {}, &floatOutput);
	CHECK(convertRows(toText) == std::vector<std::string>{"To String"});
	CHECK(index.GetEntries()[toText.front().Entry].Title == "To String");

	// A struct converts to text only.
	VisualScriptPaletteContext const matrix{MakePin("Value", VisualScriptPinDirection::Out, "Strada.Matrix4")};
	CHECK(convertRows(Rank(index, *library, "to string", {}, &matrix)) == std::vector<std::string>{"To String"});
	CHECK(convertRows(Rank(index, *library, "to int", {}, &matrix)).empty());
	// Nothing takes a rigid body as it is: the row to text comes unasked.
	VisualScriptPaletteContext const body{MakePin("Body", VisualScriptPinDirection::Out, "Strada.RigidBodyComponent")};
	CHECK(convertRows(Rank(index, *library, "", {}, &body)) == std::vector<std::string>{"To String"});
	// Into an input: the row of the input's type, for text and numbers.
	VisualScriptPaletteContext const message{MakePin("Message", VisualScriptPinDirection::In, "string")};
	std::vector<VisualScriptPaletteMatch> const intoText = Rank(index, *library, "to str", {}, &message);
	REQUIRE_FALSE(intoText.empty());
	CHECK(index.GetEntries()[intoText.front().Entry].Title == "To String");
	CHECK(intoText.front().ConnectPin == "Result");
	CHECK(convertRows(intoText) == std::vector<std::string>{"To String"});
	VisualScriptPaletteContext const amount{MakePin("Amount", VisualScriptPinDirection::In, "int")};
	CHECK(convertRows(Rank(index, *library, "to", {}, &amount)) == std::vector<std::string>{"To Int"});
	VisualScriptPaletteContext const force{MakePin("Force", VisualScriptPinDirection::In, "Strada.Vector3")};
	CHECK(convertRows(Rank(index, *library, "to", {}, &force)).empty());
	// Exec has no conversions.
	VisualScriptPaletteContext const exec{MakePin("Then", VisualScriptPinDirection::Out, "exec")};
	CHECK(convertRows(Rank(index, *library, "to", {}, &exec)).empty());
}

TEST_CASE("VisualScriptPalette: ranking 1500 entries scores each entry once, and fast")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary(900);
	VisualScriptDocument document = ParseDocument(PlayerText);
	for (uint32_t index = 0; index < 300; index++)
	{
		VisualScriptVariable variable;
		variable.ID = 1000 + index;
		variable.Name = "Stamina" + std::to_string(index);
		variable.Type = index % 2 == 0 ? "float" : "int";
		document.Variables.push_back(std::move(variable));
	}
	VisualScriptPaletteIndex const index = MakeIndex(*library, document, 0);
	size_t const entryCount = index.GetEntries().size();
	MESSAGE("palette entries: " << entryCount);
	REQUIRE(entryCount >= 1500);

	VisualScriptPaletteContext const floatOutput{MakePin("Value", VisualScriptPinDirection::Out, "float")};
	struct Query
	{
		std::string_view Text;
		VisualScriptPaletteContext const* Context;
	};
	std::vector<Query> const queries = {
		{"get st", nullptr}, {"adfrc", nullptr}, {"generated step 4", nullptr}, {"st", &floatOutput}, {"", &floatOutput}};
	std::vector<std::string> const recents = {"Flow.Branch{}", R"(Variable.Get{"Variable":1003})"};
	std::vector<std::vector<VisualScriptPaletteMatch>> first;
	for (Query const& query : queries)
	{
		CAPTURE(query.Text);
		VisualScriptPaletteRankStats stats;
		first.push_back(RankVisualScriptPalette(index, query.Text, recents, query.Context, *library, &stats));
		CHECK(stats.EntriesScored == entryCount);
		size_t words = 0;
		bool inWord = false;
		for (char const character : query.Text)
		{
			words += character != ' ' && !inWord ? 1 : 0;
			inWord = character != ' ';
		}
		CHECK(stats.SubsequenceSearches <= stats.EntriesScored * words);
		// The same ranking every time.
		CHECK(RankVisualScriptPalette(index, query.Text, recents, query.Context, *library) == first.back());
	}
	// Words every title starts a word with need no subsequence search.
	VisualScriptPaletteRankStats prefix;
	CHECK_FALSE(RankVisualScriptPalette(index, "generated step", {}, nullptr, *library, &prefix).empty());
	CHECK(prefix.SubsequenceSearches <= entryCount - 900);
	CHECK(first[2].size() >= 100);

	// Wall clock: the median of 21 rounds of the queries.
	std::vector<double> milliseconds;
	for (int round = 0; round < 21; round++)
	{
		auto const start = std::chrono::steady_clock::now();
		size_t found = 0;
		for (Query const& query : queries)
		{
			found += RankVisualScriptPalette(index, query.Text, recents, query.Context, *library).size();
		}
		auto const end = std::chrono::steady_clock::now();
		CHECK(found > 0);
		milliseconds.push_back(std::chrono::duration<double, std::milli>(end - start).count() / static_cast<double>(queries.size()));
	}
	std::sort(milliseconds.begin(), milliseconds.end());
	double const median = milliseconds[milliseconds.size() / 2];
	MESSAGE("ranking " << entryCount << " palette entries: median " << median << " ms per query");
	if (ArePerformanceTestsEnabled())
	{
		// Three times the median of three runs measured on the machine that wrote this (an AMD Ryzen 5 7535HS laptop, Windows 11,
		// MSVC 19.51): 1723 entries, the five queries above. The target is 2 ms in Release.
#if defined(ST_DEBUG)
		constexpr double MeasuredMilliseconds = 12.0;
#else
		constexpr double MeasuredMilliseconds = 0.29;
#endif
		CHECK(median <= 3.0 * MeasuredMilliseconds);
	}
}

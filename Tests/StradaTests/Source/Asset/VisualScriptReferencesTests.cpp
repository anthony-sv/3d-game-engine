#include "Strada/Asset/VisualScriptReferences.h"

#include <glm/glm.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	VisualScriptNode MakeNode(Json settings)
	{
		VisualScriptNode node;
		node.ID = 7;
		node.Type = "Function.Call";
		node.Settings = std::move(settings);
		return node;
	}

	AssetHandle MakeHandle(uint64_t value)
	{
		return AssetHandle(UUID(value));
	}
}

TEST_CASE("VisualScript: a node's Graph setting is a string holding a decimal handle other than 0")
{
	struct Case
	{
		Json Graph;
		std::optional<AssetHandle> Expected;
	};
	std::vector<Case> const cases = {
		{"8146390251172604461", MakeHandle(8146390251172604461ull)},
		{"18446744073709551615", MakeHandle(18446744073709551615ull)},
		{"42", MakeHandle(42)},
		// Leading zeros are digits, as the runtime reads handles.
		{"007", MakeHandle(7)},
		// "0" is the literal of no graph.
		{"0", std::nullopt},
		{"00", std::nullopt},
		{"", std::nullopt},
		{"-5", std::nullopt},
		{"+5", std::nullopt},
		{" 5", std::nullopt},
		{"5 ", std::nullopt},
		{"0x10", std::nullopt},
		{"18446744073709551616", std::nullopt},
		{"asset://Scripts/Motion.sgraph", std::nullopt},
		{"Motion", std::nullopt},
		// Handles are strings: numbers are not, whatever their value.
		{42, std::nullopt},
		{8146390251172604461ull, std::nullopt},
		{1.5, std::nullopt},
		{nullptr, std::nullopt},
		{true, std::nullopt},
		{Json::array({"42"}), std::nullopt},
		{Json::object({{"Graph", "42"}}), std::nullopt},
	};
	for (Case const& tested : cases)
	{
		CAPTURE(tested.Graph.dump());
		VisualScriptNode const node = MakeNode(Json::object({{"Graph", tested.Graph}, {"Function", 3}}));
		CHECK(ReadVisualScriptGraphSetting(node) == tested.Expected);
		// A Graph member makes the node's other settings name another graph's elements, malformed or not.
		CHECK_FALSE(IsLocalVisualScriptReference(node));
	}
}

TEST_CASE("VisualScript: a node without a Graph setting refers to its own document")
{
	VisualScriptNode const call = MakeNode(Json::object({{"Function", 3}}));
	CHECK_FALSE(ReadVisualScriptGraphSetting(call).has_value());
	CHECK(IsLocalVisualScriptReference(call));

	VisualScriptNode const empty = MakeNode(Json::object());
	CHECK_FALSE(ReadVisualScriptGraphSetting(empty).has_value());
	CHECK(IsLocalVisualScriptReference(empty));

	// Settings of a node built in memory that are no object hold no Graph member.
	for (Json const& settings : {Json(), Json::array({"Graph", "42"}), Json("Graph")})
	{
		CAPTURE(settings.dump());
		VisualScriptNode const node = MakeNode(settings);
		CHECK_FALSE(ReadVisualScriptGraphSetting(node).has_value());
		CHECK(IsLocalVisualScriptReference(node));
	}

	// Names are case-sensitive, as every key of the format.
	VisualScriptNode const lowercase = MakeNode(Json::object({{"graph", "42"}}));
	CHECK_FALSE(ReadVisualScriptGraphSetting(lowercase).has_value());
	CHECK(IsLocalVisualScriptReference(lowercase));
}

TEST_CASE("VisualScript: a resolver answers the documents of other graphs, and none for the graphs it does not know")
{
	VisualScriptDocument library;
	library.Kind = VisualScriptDocumentKind::Library;
	AssetHandle const known = MakeHandle(5550000000000000001ull);
	VisualScriptGraphResolver const resolve = [&library, known](AssetHandle graph) -> VisualScriptDocument const*
	{
		return graph == known ? &library : nullptr;
	};
	VisualScriptNode const call = MakeNode(Json::object({{"Graph", "5550000000000000001"}, {"Function", 3}}));
	std::optional<AssetHandle> const graph = ReadVisualScriptGraphSetting(call);
	REQUIRE(graph.has_value());
	CHECK(resolve(*graph) == &library);
	CHECK(resolve(MakeHandle(42)) == nullptr);
}

namespace
{
	VisualScriptNode MakeReference(uint32_t id, std::string type, Json settings)
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Settings = std::move(settings);
		return node;
	}

	Json GraphSetting(uint64_t graph, Json settings = Json::object())
	{
		settings["Graph"] = std::to_string(graph);
		return settings;
	}

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value, bool isPublic)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Value = std::move(value);
		variable.Public = isPublic;
		return variable;
	}

	// A script with two variables, a function, a link, a comment box and, in its event graph, two custom events among other
	// nodes, laid out and commented.
	VisualScriptDocument MakeDoorDocument()
	{
		VisualScriptDocument document;
		document.Description = "A door";
		document.Variables.push_back(MakeVariable(1, "Health", "float", 10.0, true));
		document.Variables.back().Tooltip = "Hit points";
		document.Variables.back().Range = glm::vec2(0.0f, 100.0f);
		document.Variables.back().FormerNames = {"Hp"};
		document.Variables.push_back(MakeVariable(2, "Secret", "int", 3, false));
		VisualScriptFunction open;
		open.ID = 3;
		open.Name = "Open";
		open.Graph.Nodes.push_back(MakeReference(4, "Function.Entry", Json::object()));
		document.Functions.push_back(std::move(open));

		Json const damage = Json::object({{"Name", "Damage"}, {"Type", "float"}});
		VisualScriptNode hit = MakeReference(10, "Event.Custom", Json::object({{"Name", "Hit"}, {"Parameters", Json::array({damage})}}));
		hit.Position = glm::vec2(0.0f, 0.0f);
		hit.Comment = "When struck";
		hit.Collapsed = true;
		hit.Split.Outputs = {"Damage"};
		hit.Inputs = Json::object({{"Unused", 1}});
		document.Graph.Nodes.push_back(hit);
		VisualScriptNode knock = MakeReference(11, "Event.Custom", Json::object({{"Name", "Knock"}}));
		knock.Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
		document.Graph.Nodes.push_back(knock);
		document.Graph.Nodes.push_back(MakeReference(12, "Event.Create", Json::object()));
		document.Graph.Nodes.push_back(MakeReference(13, "Variable.Set", Json::object({{"Variable", 1}})));
		document.Graph.Links.push_back({{10, "Then"}, {13, "Execute"}});
		VisualScriptComment box;
		box.ID = 14;
		box.Text = "Events";
		document.Graph.Comments.push_back(box);
		return document;
	}
}

TEST_CASE("VisualScript: a document's references name each graph once, ascending, with how many nodes name it and its strongest use")
{
	VisualScriptDocument document;
	// The event graph: a Send and a Call naming 900, a Get of 300's variable, a Broadcast to 700.
	document.Graph.Nodes.push_back(MakeReference(10, "Event.Send", GraphSetting(900, Json::object({{"Name", "Hit"}}))));
	document.Graph.Nodes.push_back(MakeReference(11, "Function.Call", GraphSetting(900, Json::object({{"Function", 1}}))));
	document.Graph.Nodes.push_back(MakeReference(12, "Variable.Get", GraphSetting(300, Json::object({{"Variable", 1}}))));
	document.Graph.Nodes.push_back(MakeReference(13, "Event.Broadcast", GraphSetting(700, Json::object({{"Name", "Hit"}}))));
	// Disabled nodes refer to graphs too.
	VisualScriptNode disabled = MakeReference(14, "RunsGraph", GraphSetting(500));
	disabled.Enabled = VisualScriptNodeEnabled::Disabled;
	document.Graph.Nodes.push_back(disabled);
	// Nodes that name no graph: a local reference, malformed settings, and kinds that refer to no graph.
	document.Graph.Nodes.push_back(MakeReference(15, "Function.Call", Json::object({{"Function", 2}})));
	document.Graph.Nodes.push_back(MakeReference(16, "Variable.Set", Json::object({{"Variable", 1}, {"Graph", "0"}})));
	document.Graph.Nodes.push_back(MakeReference(17, "Event.Send", Json::object({{"Name", "Hit"}, {"Graph", 300}})));
	document.Graph.Nodes.push_back(MakeReference(18, "Event.Custom", GraphSetting(100, Json::object({{"Name", "Hit"}}))));
	document.Graph.Nodes.push_back(MakeReference(19, "Sample.Unknown", GraphSetting(100)));
	// A function's graph: a Set of 300's variable, a Send to 700 and a RunsGraph of 700, which makes 700 an Interface use.
	VisualScriptFunction function;
	function.ID = 2;
	function.Name = "Act";
	function.Graph.Nodes.push_back(MakeReference(20, "Variable.Set", GraphSetting(300, Json::object({{"Variable", 1}}))));
	function.Graph.Nodes.push_back(MakeReference(21, "Event.Send", GraphSetting(700, Json::object({{"Name", "Hit"}}))));
	function.Graph.Nodes.push_back(MakeReference(22, "RunsGraph", GraphSetting(700)));
	document.Functions.push_back(std::move(function));

	using Use = VisualScriptReferenceUse;
	std::vector<VisualScriptGraphReference> const expected = {{MakeHandle(300), Use::Interface, 2},
	                                                          {MakeHandle(500), Use::Interface, 1},
	                                                          {MakeHandle(700), Use::Interface, 3},
	                                                          {MakeHandle(900), Use::Library, 2}};
	CHECK(CollectVisualScriptReferences(document) == expected);

	// Each kind alone.
	struct Case
	{
		std::string Type;
		Use Expected;
	};
	std::vector<Case> const cases = {{"Function.Call", Use::Library},   {"Variable.Get", Use::Interface},
	                                 {"Variable.Set", Use::Interface},  {"RunsGraph", Use::Interface},
	                                 {"Event.Send", Use::Verification}, {"Event.Broadcast", Use::Verification}};
	for (Case const& tested : cases)
	{
		CAPTURE(tested.Type);
		VisualScriptDocument single;
		single.Graph.Nodes.push_back(MakeReference(1, tested.Type, GraphSetting(42)));
		CHECK(CollectVisualScriptReferences(single) == std::vector<VisualScriptGraphReference>{{MakeHandle(42), tested.Expected, 1}});
	}
	CHECK(CollectVisualScriptReferences(VisualScriptDocument{}).empty());
}

TEST_CASE("VisualScript: a document's interface is its kind, its variables and its custom events, without layout or anything else")
{
	VisualScriptDocument const door = MakeDoorDocument();
	VisualScriptDocument const linked = MakeVisualScriptInterface(door);
	CHECK(linked.Kind == VisualScriptDocumentKind::Script);
	CHECK(linked.Description.empty());
	CHECK(linked.Variables == door.Variables);
	CHECK(linked.Functions.empty());
	CHECK(linked.Graph.Links.empty());
	CHECK(linked.Graph.Comments.empty());
	REQUIRE(linked.Graph.Nodes.size() == 2);
	VisualScriptNode const& hit = linked.Graph.Nodes[0];
	CHECK(hit.ID == 10);
	CHECK(hit.Type == "Event.Custom");
	CHECK(hit.Settings == door.Graph.Nodes[0].Settings);
	CHECK(hit.Enabled == VisualScriptNodeEnabled::Enabled);
	CHECK_FALSE(hit.Position.has_value());
	CHECK(hit.Inputs == Json::object());
	CHECK(hit.Split.IsEmpty());
	CHECK(hit.Comment.empty());
	CHECK_FALSE(hit.Collapsed);
	VisualScriptNode const& knock = linked.Graph.Nodes[1];
	CHECK(knock.ID == 11);
	CHECK(knock.Enabled == VisualScriptNodeEnabled::DevelopmentOnly);
	// An interface is its own interface.
	CHECK(MakeVisualScriptInterface(linked) == linked);

	VisualScriptDocument library;
	library.Kind = VisualScriptDocumentKind::Library;
	library.Functions = door.Functions;
	VisualScriptDocument const libraryInterface = MakeVisualScriptInterface(library);
	CHECK(libraryInterface.Kind == VisualScriptDocumentKind::Library);
	CHECK(libraryInterface.Functions.empty());
}

TEST_CASE("VisualScript: two documents have the same link interface when their kind and variables give the same program")
{
	VisualScriptDocument const door = MakeDoorDocument();
	CHECK(HaveSameVisualScriptLinkInterface(door, door));
	CHECK(HaveSameVisualScriptLinkInterface(door, MakeVisualScriptInterface(door)));

	// Events, functions, layout, the description and what programs never read of variables do not matter.
	VisualScriptDocument other = door;
	other.Graph.Nodes.clear();
	other.Graph.Links.clear();
	other.Functions.clear();
	other.Description = "Another door";
	other.Variables[0].Tooltip = "Life";
	other.Variables[0].Range.reset();
	CHECK(HaveSameVisualScriptLinkInterface(door, other));

	// The kind and every variable's ID, name, type, value, access and former names do, order included.
	using Change = void (*)(VisualScriptDocument&);
	std::vector<Change> const changes = {[](VisualScriptDocument& document)
	                                     {
											 document.Kind = VisualScriptDocumentKind::Library;
										 },
	                                     [](VisualScriptDocument& document)
	                                     {
											 document.Variables[0].ID = 5;
										 },
	                                     [](VisualScriptDocument& document)
	                                     {
											 document.Variables[0].Name = "Life";
										 },
	                                     [](VisualScriptDocument& document)
	                                     {
											 document.Variables[1].Type = "long";
										 },
	                                     [](VisualScriptDocument& document)
	                                     {
											 document.Variables[1].Value = 4;
										 },
	                                     // The int literal 3.0 is no int to the runtime, although Json takes it for 3.
	                                     [](VisualScriptDocument& document)
	                                     {
											 document.Variables[1].Value = 3.0;
										 },
	                                     [](VisualScriptDocument& document)
	                                     {
											 document.Variables[1].Public = true;
										 },
	                                     [](VisualScriptDocument& document)
	                                     {
											 document.Variables[0].FormerNames.clear();
										 },
	                                     [](VisualScriptDocument& document)
	                                     {
											 std::swap(document.Variables[0], document.Variables[1]);
										 },
	                                     [](VisualScriptDocument& document)
	                                     {
											 document.Variables.pop_back();
										 }};
	for (size_t index = 0; index < changes.size(); index++)
	{
		CAPTURE(index);
		VisualScriptDocument changed = door;
		changes[index](changed);
		CHECK_FALSE(HaveSameVisualScriptLinkInterface(door, changed));
		CHECK_FALSE(HaveSameVisualScriptLinkInterface(changed, door));
	}
}

#include "VisualScriptTestDocuments.h"

#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptLanguageEditing.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = glm::vec2(0.0f);
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	template<typename T>
	T RequireOk(Result<T> result)
	{
		REQUIRE_MESSAGE(result.IsOk(), result.GetError());
		return result.TakeValue();
	}

	// What every edit must leave: a document that saves and loads back as it is, addressing fields only of split pins.
	void CheckDocument(VisualScriptDocument const& document)
	{
		Testing::ExpectStrictRoundTrip(document);
		Testing::ExpectSplitConsistency(document);
	}

	// The operation fails with an error that says the message, and leaves the document as it was.
	template<typename TOperation>
	void CheckRefused(VisualScriptDocument& document, TOperation const& operation, std::string_view message)
	{
		VisualScriptDocument const before = document;
		auto const result = operation(document);
		REQUIRE_FALSE(result.IsOk());
		CHECK_MESSAGE(result.GetError().find(message) != std::string::npos, result.GetError());
		CHECK(document == before);
	}

	// Values of Strada.Vector3 feed a Make Array whose elements are split, linked and given literals.
	VisualScriptDocument MakeArrayDocument()
	{
		VisualScriptDocument document;
		VisualScriptGraph& graph = document.Graph;
		VisualScriptNode make = MakeNode(1, "Array.Make", {{"Type", "Strada.Vector3"}, {"Count", 4}},
		                                 {{"Element0", Json::array({1, 2, 3})}, {"Element2.X", 5}, {"Element3", Json::array({7, 8, 9})}});
		make.Split.Inputs = {"Element2"};
		graph.Nodes = {make, MakeNode(2, "Literal", {{"Type", "Strada.Vector3"}, {"Value", Json::array({0, 0, 1})}}),
		               MakeNode(3, "Literal", {{"Type", "float"}, {"Value", 4}}), MakeNode(4, "Array.Length")};
		graph.Links = {MakeLink(2, "Value", 1, "Element1"), MakeLink(3, "Value", 1, "Element2.Y"), MakeLink(1, "Array", 4, "Array"),
		               // Element7 is no element of a Make Array of 4: its link is an orphan, which stays as it is.
		               MakeLink(2, "Value", 1, "Element7")};
		return document;
	}
}

TEST_CASE("VisualScriptLanguageEditing: Count of Make Array and Formula of an Expression name the operations that set them")
{
	CHECK(VisualScriptEditing::FindVisualScriptSettingOperation(MakeNode(1, "Array.Make"), "Count") == "SetArrayElements");
	CHECK(VisualScriptEditing::FindVisualScriptSettingOperation(MakeNode(1, "Expression"), "Formula") == "SetFormula");
	CHECK(VisualScriptEditing::FindVisualScriptSettingOperation(MakeNode(1, "Array.Make"), "Type").empty());
	CHECK(VisualScriptEditing::FindVisualScriptSettingOperation(MakeNode(1, "Flow.Branch"), "Formula").empty());
	CHECK(VisualScriptEditing::FindVisualScriptSettingOperation(MakeNode(1, "Array.Get"), "Count").empty());

	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(1, "Array.Make"), MakeNode(2, "Expression", {{"Formula", "a"}})};
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::SetSetting(edited, 1, "Count", 3);
		},
		"Count of node 1 (Array.Make) changes its pins or its senders: set it with SetArrayElements");
	CheckRefused(
		document,
		[](VisualScriptDocument& edited)
		{
			return VisualScriptEditing::SetSetting(edited, 2, "Formula", "b");
		},
		"Formula of node 2 (Expression) changes its pins or its senders: set it with SetFormula");
}

TEST_CASE("VisualScriptLanguageEditing: a Make Array's elements are counted, inserted and removed as a Sequence's outputs")
{
	VisualScriptDocument document = MakeArrayDocument();
	VisualScriptGraph& graph = document.Graph;
	VisualScriptNode const& make = graph.Nodes[0];

	SUBCASE("lowering the count removes the links, literals and split pins of the elements that go")
	{
		CHECK(RequireOk(VisualScriptEditing::SetArrayElements(document, 1, 2, std::nullopt, std::nullopt)) == 1);
		CHECK(make.Settings == Json({{"Type", "Strada.Vector3"}, {"Count", 2}}));
		CHECK(make.Inputs == Json({{"Element0", Json::array({1, 2, 3})}}));
		CHECK(make.Split.Inputs.empty());
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(2, "Value", 1, "Element1"), MakeLink(1, "Array", 4, "Array"),
		                                                   MakeLink(2, "Value", 1, "Element7")});
		CHECK(RequireOk(VisualScriptEditing::SetArrayElements(document, 1, 5, std::nullopt, std::nullopt)) == 0);
		CHECK(make.Settings["Count"] == 5);
		CheckDocument(document);
	}

	SUBCASE("inserting elements moves the later ones up, with their literals and split pins")
	{
		CHECK(RequireOk(VisualScriptEditing::SetArrayElements(document, 1, 6, 1, std::nullopt)) == 0);
		CHECK(make.Settings["Count"] == 6);
		CHECK(make.Inputs == Json({{"Element0", Json::array({1, 2, 3})}, {"Element4.X", 5}, {"Element5", Json::array({7, 8, 9})}}));
		CHECK(make.Split.Inputs == std::vector<std::string>{"Element4"});
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(2, "Value", 1, "Element3"), MakeLink(3, "Value", 1, "Element4.Y"),
		                                                   MakeLink(1, "Array", 4, "Array"), MakeLink(2, "Value", 1, "Element7")});
		CheckDocument(document);
	}

	SUBCASE("removing elements removes theirs and moves the later ones down")
	{
		CHECK(RequireOk(VisualScriptEditing::SetArrayElements(document, 1, 2, std::nullopt, 1)) == 2);
		CHECK(make.Inputs == Json({{"Element0", Json::array({1, 2, 3})}, {"Element1", Json::array({7, 8, 9})}}));
		CHECK(make.Split.Inputs.empty());
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Array", 4, "Array"), MakeLink(2, "Value", 1, "Element7")});
		CheckDocument(document);
	}

	SUBCASE("removing the first element moves every other one down")
	{
		CHECK(RequireOk(VisualScriptEditing::SetArrayElements(document, 1, 3, std::nullopt, 0)) == 0);
		CHECK(make.Inputs == Json({{"Element1.X", 5}, {"Element2", Json::array({7, 8, 9})}}));
		CHECK(make.Split.Inputs == std::vector<std::string>{"Element1"});
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(2, "Value", 1, "Element0"), MakeLink(3, "Value", 1, "Element1.Y"),
		                                                   MakeLink(1, "Array", 4, "Array"), MakeLink(2, "Value", 1, "Element7")});
		CheckDocument(document);
	}

	SUBCASE("zero elements are allowed, and a Make Array without Count has two")
	{
		CHECK(RequireOk(VisualScriptEditing::SetArrayElements(document, 1, 0, std::nullopt, std::nullopt)) == 2);
		CHECK(make.Inputs == Json::object());
		graph.Nodes[0].Settings = Json::object();
		graph.Nodes[0].Inputs = {{"Element1", Json::array({1, 1, 1})}};
		CHECK(RequireOk(VisualScriptEditing::SetArrayElements(document, 1, 3, 0, std::nullopt)) == 0);
		CHECK(make.Inputs == Json({{"Element2", Json::array({1, 1, 1})}}));
		CheckDocument(document);
	}

	SUBCASE("a Count that is no count takes a new count only, and every element from it on goes")
	{
		graph.Nodes[0].Settings["Count"] = "four";
		CheckRefused(
			document,
			[](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SetArrayElements(edited, 1, 5, 0, std::nullopt);
			},
			"node 1 has no Count of 0 to 64");
		CHECK(RequireOk(VisualScriptEditing::SetArrayElements(document, 1, 1, std::nullopt, std::nullopt)) == 3);
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Array", 4, "Array")});
	}

	SUBCASE("what a Make Array cannot have is refused")
	{
		auto const setElements = [](uint32_t node, int count, std::optional<int> insertAt, std::optional<int> removeAt)
		{
			return [node, count, insertAt, removeAt](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SetArrayElements(edited, node, count, insertAt, removeAt);
			};
		};
		CheckRefused(document, setElements(1, 65, std::nullopt, std::nullopt), "a Make Array has 0 to 64 elements");
		CheckRefused(document, setElements(1, -1, std::nullopt, std::nullopt), "a Make Array has 0 to 64 elements");
		CheckRefused(document, setElements(1, 5, 1, 1), "inserted or removed, not both");
		CheckRefused(document, setElements(1, 4, 1, std::nullopt), "inserting elements at 1 takes a count above 4");
		CheckRefused(document, setElements(1, 5, 5, std::nullopt), "an index from 0 to 4");
		CheckRefused(document, setElements(1, 4, std::nullopt, 0), "removing elements at 0 takes a count below 4");
		CheckRefused(document, setElements(1, 2, std::nullopt, 3), "an index from 0 to 2");
		CheckRefused(document, setElements(4, 2, std::nullopt, std::nullopt), "node 4 is no Array.Make: its type is Array.Length");
		CheckRefused(document, setElements(9, 2, std::nullopt, std::nullopt), "the document has no node 9");
	}
}

TEST_CASE("VisualScriptLanguageEditing: formula inputs are the ones the scripting runtime gives, by the shared corpus")
{
	Result<std::string> text =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Formulas.json");
	REQUIRE_MESSAGE(text.IsOk(), text.GetError());
	Result<Json> corpus = ParseJson(text.GetValue());
	REQUIRE_MESSAGE(corpus.IsOk(), corpus.GetError());
	REQUIRE(corpus.GetValue().is_array());
	REQUIRE(corpus.GetValue().size() >= 500);
	for (Json const& entry : corpus.GetValue())
	{
		std::string const formula = entry.at("Formula").get<std::string>();
		CAPTURE(formula);
		CHECK(VisualScriptEditing::ListVisualScriptFormulaInputs(formula) == entry.at("Inputs").get<std::vector<std::string>>());
	}
}

TEST_CASE("VisualScriptLanguageEditing: the input rule's cases")
{
	using Inputs = std::vector<std::string>;
	auto const list = [](std::string_view formula)
	{
		return VisualScriptEditing::ListVisualScriptFormulaInputs(formula);
	};
	CHECK(list("a * b + sin(t)") == Inputs{"a", "b", "t"});
	// Function and cast names are inputs where they are not called, components are not, nor are the constants.
	CHECK(list("distance / speed") == Inputs{"distance", "speed"});
	CHECK(list("exp + 1 + float (x)") == Inputs{"exp", "x"});
	CHECK(list("v.X + v . y + w.Z") == Inputs{"v", "w"});
	CHECK(list("true || pi > tau && false") == Inputs{});
	// Numbers take only their complete parts: 1e is 1 and the input e, 3dx is 3d and x, 2.x is 2, '.' and the component x.
	CHECK(list("1e + 3dx + 2.x + 1e+5 + 0x10") == Inputs{"e", "x", "x10"});
	// Characters that start no token are passed over, so their neighbours still decide (a '(' after an unknown character).
	CHECK(list("a # (b) + c\xC3\xA9(d) + \xE2\x82\xAC e") == Inputs{"b", "d", "e"});
	CHECK(list("a = b & c | d == e") == Inputs{"a", "b", "c", "d", "e"});
	// Reserved pin names and Result are refused, names over 64 bytes are none, and repeated names count once.
	CHECK(list("Self + Result + Then + x + x") == Inputs{"x"});
	CHECK(list(std::string(65, 'a') + " + " + std::string(64, 'b')) == Inputs{std::string(64, 'b')});
	// At most 32 inputs.
	std::string many;
	Inputs first;
	for (int index = 0; index < 40; index++)
	{
		std::string const name = "n" + std::to_string(index);
		many += (index > 0 ? " + " : "") + name;
		if (index < 32)
		{
			first.push_back(name);
		}
	}
	CHECK(list(many) == first);
	// A formula over 4096 bytes has none.
	CHECK(list("a" + std::string(4095, ' ')) == Inputs{"a"});
	CHECK(list("a" + std::string(4096, ' ')).empty());
	CHECK(list("").empty());
}

TEST_CASE("VisualScriptLanguageEditing: a formula keeps the links and values of the inputs it keeps, renames in order and removes the rest")
{
	VisualScriptDocument document;
	VisualScriptGraph& graph = document.Graph;
	VisualScriptNode expression = MakeNode(1, "Expression", {{"Formula", "speed * dt"}}, {{"dt", 0.5}});
	graph.Nodes = {expression, MakeNode(2, "Literal", {{"Type", "float"}, {"Value", 3}}), MakeNode(3, "Variable.Set")};
	graph.Links = {MakeLink(2, "Value", 1, "speed"), MakeLink(1, "Result", 3, "Value")};
	VisualScriptNode const& node = graph.Nodes[0];

	SUBCASE("retyping speed * dt as velocity * dt is one rename that keeps the link and the literal")
	{
		VisualScriptEditing::VisualScriptFormulaChange const change =
			RequireOk(VisualScriptEditing::SetFormula(document, 1, "velocity * dt"));
		CHECK(change.RenamedInputs == std::vector<std::pair<std::string, std::string>>{{"speed", "velocity"}});
		CHECK(change.RemovedLinks == 0);
		CHECK(node.Settings == Json({{"Formula", "velocity * dt"}}));
		CHECK(node.Inputs == Json({{"dt", 0.5}}));
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(2, "Value", 1, "velocity"), MakeLink(1, "Result", 3, "Value")});
		CheckDocument(document);
	}

	SUBCASE("a*b to a*b*c adds one unlinked input and changes nothing else")
	{
		graph.Nodes[0].Settings["Formula"] = "speed*dt";
		VisualScriptEditing::VisualScriptFormulaChange const change =
			RequireOk(VisualScriptEditing::SetFormula(document, 1, "speed*dt*scale"));
		CHECK(change.RenamedInputs.empty());
		CHECK(change.RemovedLinks == 0);
		CHECK(node.Inputs == Json({{"dt", 0.5}}));
		CHECK(graph.Links.size() == 2);
	}

	SUBCASE("an input that went has its link and literal removed when the list's length changed")
	{
		VisualScriptEditing::VisualScriptFormulaChange const change = RequireOk(VisualScriptEditing::SetFormula(document, 1, "2 * pi"));
		CHECK(change.RenamedInputs.empty());
		CHECK(change.RemovedLinks == 1);
		CHECK(node.Inputs == Json::object());
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(1, "Result", 3, "Value")});
	}

	SUBCASE("split inputs follow their renames, and an unchanged list changes nothing")
	{
		graph.Nodes[0].Settings["Formula"] = "pos + dir";
		graph.Nodes[0].Inputs = {{"dir.X", 1}, {"dir.Y", 0}, {"pos", Json::array({0, 0, 0})}};
		graph.Nodes[0].Split.Inputs = {"dir"};
		graph.Links = {MakeLink(2, "Value", 1, "dir.Z")};
		VisualScriptEditing::VisualScriptFormulaChange const change =
			RequireOk(VisualScriptEditing::SetFormula(document, 1, "pos + direction * 2"));
		CHECK(change.RenamedInputs == std::vector<std::pair<std::string, std::string>>{{"dir", "direction"}});
		CHECK(node.Split.Inputs == std::vector<std::string>{"direction"});
		CHECK(node.Inputs == Json({{"direction.X", 1}, {"direction.Y", 0}, {"pos", Json::array({0, 0, 0})}}));
		CHECK(graph.Links == std::vector<VisualScriptLink>{MakeLink(2, "Value", 1, "direction.Z")});
		CheckDocument(document);
		VisualScriptDocument const before = document;
		CHECK(RequireOk(VisualScriptEditing::SetFormula(document, 1, "pos+direction*3")).RenamedInputs.empty());
		CHECK(graph.Links == before.Graph.Links);
		CHECK(node.Inputs == before.Graph.Nodes[0].Inputs);
	}

	SUBCASE("a node without a formula has no inputs, and a broken formula still names its inputs")
	{
		graph.Nodes[0].Settings = Json::object();
		CHECK(RequireOk(VisualScriptEditing::SetFormula(document, 1, "speed * (")).RenamedInputs.empty());
		CHECK(node.Settings == Json({{"Formula", "speed * ("}}));
		CHECK(graph.Links.size() == 2);
		CHECK(RequireOk(VisualScriptEditing::SetFormula(document, 1, "")).RemovedLinks == 1);
		CHECK(node.Settings == Json({{"Formula", ""}}));
	}

	SUBCASE("what an Expression cannot have is refused")
	{
		auto const setFormula = [](uint32_t id, std::string formula)
		{
			return [id, formula](VisualScriptDocument& edited)
			{
				return VisualScriptEditing::SetFormula(edited, id, formula);
			};
		};
		CheckRefused(document, setFormula(2, "a"), "node 2 is no Expression: its type is Literal");
		CheckRefused(document, setFormula(1, std::string(4097, 'a')), "a formula has at most 4096 bytes");
		CheckRefused(document, setFormula(1, "a\xFF"), "a formula must be UTF-8");
		CheckRefused(document, setFormula(7, "a"), "the document has no node 7");
		graph.Nodes[0].Inputs = Json::array();
		CheckRefused(document, setFormula(1, "a"), "node 1 has Settings or Inputs that are not objects");
	}
}

#include "VisualScriptTestDocuments.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptBatch.h"
#include "Editor/VisualScripting/VisualScriptClipboard.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace Strada;
using namespace Strada::Testing;

namespace
{
	constexpr std::string_view LogType = "Call:Strada.Log.Info(string)";
	constexpr std::string_view AddForceType = "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)";
	constexpr std::string_view InstantiateType =
		"Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)";

	VisualScriptNode MakeNode(uint32_t id, std::string_view type, std::optional<glm::vec2> position, Json settings = Json::object(),
	                          Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::string(type);
		node.Position = position;
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	// The event graph: Update (1) at (0, 0), its flow into a Log (2) at (320, 0), then into a Delay (3) at (640, 0); the
	// variable Speed (10, float 2.5); the function Push (20), its entry (21) linked to its return (22), with the local Count (23).
	VisualScriptDocument MakeDocument()
	{
		VisualScriptDocument document;
		VisualScriptVariable speed;
		speed.ID = 10;
		speed.Name = "Speed";
		speed.Type = "float";
		speed.Value = 2.5;
		document.Variables.push_back(std::move(speed));
		document.Graph.Nodes.push_back(MakeNode(1, "Event.Update", glm::vec2(0.0f, 0.0f)));
		document.Graph.Nodes.push_back(MakeNode(2, LogType, glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"Message", "a"}})));
		document.Graph.Nodes.push_back(MakeNode(3, "Flow.Delay", glm::vec2(640.0f, 0.0f)));
		document.Graph.Links.push_back(MakeLink(1, "Then", 2, "Execute"));
		document.Graph.Links.push_back(MakeLink(2, "Then", 3, "Execute"));
		VisualScriptFunction push;
		push.ID = 20;
		push.Name = "Push";
		VisualScriptLocal count;
		count.ID = 23;
		count.Name = "Count";
		count.Type = "int";
		push.Locals.push_back(std::move(count));
		push.Graph.Nodes.push_back(MakeNode(21, "Function.Entry", glm::vec2(0.0f, 0.0f)));
		push.Graph.Nodes.push_back(MakeNode(22, "Function.Return", glm::vec2(480.0f, 0.0f)));
		push.Graph.Links.push_back(MakeLink(21, "Then", 22, "Execute"));
		document.Functions.push_back(std::move(push));
		return document;
	}

	// A test's semantics and the batch's context with it.
	struct BatchFixture
	{
		FixtureVisualScriptSemantics Semantics;
		VisualScriptBatchContext Context{Semantics};
	};

	VisualScriptBatchResult ExpectApplied(VisualScriptDocument const& document, Json const& operations,
	                                      VisualScriptBatchContext const& context)
	{
		std::variant<VisualScriptBatchResult, VisualScriptBatchError> applied = ApplyVisualScriptBatch(document, operations, context);
		VisualScriptBatchError const* const error = std::get_if<VisualScriptBatchError>(&applied);
		REQUIRE_MESSAGE(error == nullptr, (error != nullptr ? error->Message : std::string()));
		VisualScriptBatchResult result = std::get<VisualScriptBatchResult>(std::move(applied));
		ExpectStrictRoundTrip(result.Document);
		ExpectSplitConsistency(result.Document);
		return result;
	}

	VisualScriptBatchError ExpectRefused(VisualScriptDocument const& document, Json const& operations,
	                                     VisualScriptBatchContext const& context)
	{
		VisualScriptDocument const before = document;
		std::variant<VisualScriptBatchResult, VisualScriptBatchError> applied = ApplyVisualScriptBatch(document, operations, context);
		REQUIRE(std::holds_alternative<VisualScriptBatchError>(applied));
		// A failure never touches the document given.
		CHECK(document == before);
		return std::get<VisualScriptBatchError>(std::move(applied));
	}

	VisualScriptNode const* FindNode(VisualScriptDocument const& document, uint32_t id)
	{
		auto const search = [id](VisualScriptGraph const& graph) -> VisualScriptNode const*
		{
			auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
			                                [id](VisualScriptNode const& node)
			                                {
												return node.ID == id;
											});
			return found != graph.Nodes.end() ? &*found : nullptr;
		};
		if (VisualScriptNode const* node = search(document.Graph))
		{
			return node;
		}
		for (VisualScriptFunction const& function : document.Functions)
		{
			if (VisualScriptNode const* node = search(function.Graph))
			{
				return node;
			}
		}
		return nullptr;
	}

	bool HasLink(VisualScriptGraph const& graph, VisualScriptLink const& link)
	{
		return std::find(graph.Links.begin(), graph.Links.end(), link) != graph.Links.end();
	}

	VisualScriptVariable const* FindVariable(VisualScriptDocument const& document, std::string_view name)
	{
		auto const found = std::find_if(document.Variables.begin(), document.Variables.end(),
		                                [name](VisualScriptVariable const& variable)
		                                {
											return variable.Name == name;
										});
		return found != document.Variables.end() ? &*found : nullptr;
	}

	uint32_t GetLabel(VisualScriptBatchResult const& result, std::string_view label)
	{
		auto const found = std::find_if(result.Labels.begin(), result.Labels.end(),
		                                [label](auto const& entry)
		                                {
											return entry.first == label;
										});
		REQUIRE(found != result.Labels.end());
		return found->second;
	}
}

TEST_CASE("VisualScriptBatch: every operation kind is listed once")
{
	std::span<std::string_view const> const kinds = GetVisualScriptBatchOperations();
	std::set<std::string_view> const unique(kinds.begin(), kinds.end());
	CHECK(unique.size() == kinds.size());
	for (std::string_view const kind : {"AddNode", "Delete", "Connect", "SetSetting", "AddVariable", "AddFunction", "Arrange", "Fix"})
	{
		CHECK(unique.contains(kind));
	}
}

TEST_CASE("VisualScriptBatch: an empty batch changes nothing, and a failure names its operation and changes nothing")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	VisualScriptBatchResult const empty = ExpectApplied(document, Json::array(), fixture.Context);
	CHECK(empty.Document == document);
	CHECK(empty.Results.empty());
	CHECK(empty.Analyses == 0);

	Json const operations =
		Json::array({Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}}),
	                 Json::object({{"op", "Connect"}, {"from", Json::array({1, "Then"})}, {"to", Json::array({2, "Nope"})}})});
	VisualScriptBatchError const error = ExpectRefused(document, operations, fixture.Context);
	CHECK(error.Failure == VisualScriptBatchFailure::Invalid);
	CHECK(error.Operation == 1);
	CHECK(error.Path == "operations[1].to[1]");
	CHECK(error.Message == "operations[1] (Connect): to[1]: node 2 has no input Nope");
	REQUIRE(error.Data.contains("pins"));
	CHECK(error.Data["pins"].size() == 3);
	CHECK(error.Data["pins"][1] == Json::object({{"name", "Message"}, {"direction", "In"}, {"type", "string"}}));
}

TEST_CASE("VisualScriptBatch: fields an operation does not take are refused, and its required ones demanded")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	VisualScriptBatchError const extra =
		ExpectRefused(document, Json::array({Json::object({{"op", "AddNode"}, {"type", "Flow.Branch"}, {"offset", Json::array({1, 2})}})}),
	                  fixture.Context);
	CHECK(extra.Path == "operations[0].offset");
	CHECK(extra.Message == "operations[0] (AddNode): offset: AddNode takes no offset");
	VisualScriptBatchError const missing =
		ExpectRefused(document, Json::array({Json::object({{"op", "Move"}, {"elements", Json::array({2})}})}), fixture.Context);
	CHECK(missing.Path == "operations[0].offset");
	VisualScriptBatchError const unknown = ExpectRefused(document, Json::array({Json::object({{"op", "Conect"}})}), fixture.Context);
	CHECK(unknown.Path == "operations[0].op");
	CHECK(unknown.Data["names"][0] == "Connect");
}

TEST_CASE("VisualScriptBatch: AddNode adds placed and unplaced nodes with settings, literals, states and labels")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	Json const operations = Json::array({
		Json::object({{"op", "AddNode"},
	                  {"type", std::string(LogType)},
	                  {"position", Json::array({960, 0})},
	                  {"literals", Json::object({{"Message", "hi"}})},
	                  {"collapsed", true},
	                  {"enabled", "Disabled"},
	                  {"as", "$log"}}),
		Json::object({{"op", "AddNode"}, {"type", "Variable.Get"}, {"settings", Json::object({{"Variable", "Speed"}})}, {"as", "$speed"}}),
		Json::object(
			{{"op", "AddNode"}, {"type", "Variable.Get"}, {"function", "Push"}, {"settings", Json::object({{"Variable", "Count"}})}}),
	});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	REQUIRE(result.Results.size() == 3);
	CHECK(result.Results[0] == Json::object({{"id", 24}}));
	CHECK(result.Results[1] == Json::object({{"id", 25}}));
	CHECK(result.Results[2] == Json::object({{"id", 26}}));
	CHECK(result.Labels == std::vector<std::pair<std::string, uint32_t>>{{"$log", 24}, {"$speed", 25}});
	CHECK(result.Created == std::vector<uint32_t>{24, 25, 26});
	VisualScriptNode const* const log = FindNode(result.Document, 24);
	REQUIRE(log != nullptr);
	CHECK(log->Position == glm::vec2(960.0f, 0.0f));
	CHECK(log->Inputs == Json::object({{"Message", "hi"}}));
	CHECK(log->Collapsed);
	CHECK(log->Enabled == VisualScriptNodeEnabled::Disabled);
	// Without a position the node is unplaced: the commit places it, or an Arrange does.
	VisualScriptNode const* const speed = FindNode(result.Document, 25);
	REQUIRE(speed != nullptr);
	CHECK_FALSE(speed->Position.has_value());
	// Names in settings are stored as IDs: in a function, its locals first.
	CHECK(speed->Settings == Json::object({{"Variable", 10}}));
	CHECK(FindNode(result.Document, 26)->Settings == Json::object({{"Variable", 23}}));
	CHECK(result.Document.Functions[0].Graph.Nodes.back().ID == 26);
	// Adding nodes reads no pins.
	CHECK(result.Analyses == 0);
	CHECK(fixture.Semantics.GetAnalyzeCallCount() == 0);
}

TEST_CASE("VisualScriptBatch: labels are defined once, before their use, and name the elements they were defined for")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	auto const add = [](std::string_view label)
	{
		return Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}, {"as", std::string(label)}});
	};
	auto const connect = [](Json from, Json to)
	{
		return Json::object(
			{{"op", "Connect"}, {"from", Json::array({std::move(from), "Then"})}, {"to", Json::array({std::move(to), "Execute"})}});
	};

	SUBCASE("A label used before its definition")
	{
		VisualScriptBatchError const error =
			ExpectRefused(document, Json::array({add("$speed"), connect(3, "$branch"), add("$branch")}), fixture.Context);
		CHECK(error.Path == "operations[1].to[0]");
		CHECK(error.Message == "operations[1] (Connect): to[0]: $branch is not defined by an earlier operation; defined: $speed");
		CHECK(error.Data["labels"] == Json::array({"$speed"}));
	}
	SUBCASE("A label defined twice")
	{
		VisualScriptBatchError const error = ExpectRefused(document, Json::array({add("$a"), add("$a")}), fixture.Context);
		CHECK(error.Path == "operations[1].as");
	}
	SUBCASE("Not a label")
	{
		VisualScriptBatchError const error = ExpectRefused(document, Json::array({add("speed")}), fixture.Context);
		CHECK(error.Path == "operations[0].as");
	}
	SUBCASE("A label of a variable where a node is expected")
	{
		Json const operations = Json::array({Json::object({{"op", "AddVariable"}, {"name", "Health"}, {"type", "int"}, {"as", "$health"}}),
		                                     Json::object({{"op", "Delete"}, {"elements", Json::array({"$health"})}})});
		VisualScriptBatchError const error = ExpectRefused(document, operations, fixture.Context);
		CHECK(error.Path == "operations[1].elements[0]");
		CHECK(error.Message.find("$health names a variable") != std::string::npos);
	}
	SUBCASE("A variable named like a label is reached by its name until a label of that name is defined")
	{
		VisualScriptDocument named = document;
		named.Variables[0].Name = "$speed";
		Json const byName = Json::array({Json::object({{"op", "SetVariable"}, {"variable", "$speed"}, {"value", 4.0}})});
		VisualScriptBatchResult const result = ExpectApplied(named, byName, fixture.Context);
		CHECK(result.Document.Variables[0].Value == 4.0);

		Json const byLabel = Json::array(
			{Json::object({{"op", "AddVariable"}, {"name", "Other"}, {"type", "float"}, {"as", "$speed"}}),
		     Json::object({{"op", "SetVariable"}, {"variable", "$speed"}, {"value", 4.0}}),
		     Json::object({{"op", "AddNode"}, {"type", "Variable.Set"}, {"settings", Json::object({{"Variable", "$speed"}})}})});
		VisualScriptBatchResult const labelled = ExpectApplied(named, byLabel, fixture.Context);
		VisualScriptVariable const* const other = FindVariable(labelled.Document, "Other");
		REQUIRE(other != nullptr);
		CHECK(other->Value == 4.0);
		CHECK(labelled.Document.Variables[0].Value == 2.5);
		// A label of a variable in a Variable setting names that variable.
		CHECK(FindNode(labelled.Document, labelled.Results[2]["id"].get<uint32_t>())->Settings["Variable"] == other->ID);
	}
}

TEST_CASE("VisualScriptBatch: integers name the document as the batch found it, never an ID a new element took again")
{
	BatchFixture fixture;
	// The largest ID is a node: deleting it frees its ID, and the next new element takes it again.
	VisualScriptDocument document;
	document.Graph.Nodes.push_back(MakeNode(1, "Event.Update", glm::vec2(0.0f)));
	document.Graph.Nodes.push_back(MakeNode(2, LogType, glm::vec2(320.0f, 0.0f)));
	document.Graph.Links.push_back(MakeLink(1, "Then", 2, "Execute"));
	Json const operations = Json::array({Json::object({{"op", "Delete"}, {"elements", Json::array({2})}}),
	                                     Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}, {"as", "$again"}}),
	                                     Json::object({{"op", "SetNodeComment"}, {"nodes", Json::array({2})}, {"text", "x"}})});
	VisualScriptBatchError const error = ExpectRefused(document, operations, fixture.Context);
	CHECK(error.Path == "operations[2].nodes[0]");
	CHECK(error.Message == "operations[2] (SetNodeComment): nodes[0]: node 2 was deleted by operations[0]");

	// The new element is reached by its label.
	Json labelled = operations;
	labelled[2]["nodes"] = Json::array({"$again"});
	VisualScriptBatchResult const result = ExpectApplied(document, labelled, fixture.Context);
	CHECK(result.Results[1] == Json::object({{"id", 2}}));
	CHECK(FindNode(result.Document, 2)->Comment == "x");

	// An ID the document never had.
	VisualScriptBatchError const absent = ExpectRefused(
		document, Json::array({Json::object({{"op", "SetNodeComment"}, {"nodes", Json::array({7})}, {"text", "x"}})}), fixture.Context);
	CHECK(absent.Path == "operations[0].nodes[0]");
}

TEST_CASE("VisualScriptBatch: names resolve against the working document, and a Graph naming the edited script too")
{
	BatchFixture fixture;
	fixture.Context.Graph = AssetHandle(77);
	VisualScriptDocument const document = MakeDocument();
	Json const operations = Json::array({
		Json::object({{"op", "AddVariable"}, {"name", "Health"}, {"type", "int"}, {"public", true}}),
		Json::object({{"op", "AddFunction"}, {"name", "Heal"}, {"as", "$heal"}}),
		Json::object({{"op", "AddNode"}, {"type", "Function.Call"}, {"settings", Json::object({{"Function", "Heal"}})}}),
		Json::object({{"op", "AddNode"}, {"type", "Variable.Get"}, {"settings", Json::object({{"Graph", "77"}, {"Variable", "Health"}})}}),
		Json::object({{"op", "RenameFunction"}, {"function", "$heal"}, {"name", "Cure"}}),
		Json::object({{"op", "AddNode"}, {"type", "Function.Call"}, {"settings", Json::object({{"Function", "Cure"}})}}),
	});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	uint32_t const health = result.Results[0]["id"].get<uint32_t>();
	uint32_t const heal = result.Results[1]["id"].get<uint32_t>();
	CHECK(FindNode(result.Document, result.Results[2]["id"].get<uint32_t>())->Settings["Function"] == heal);
	CHECK(FindNode(result.Document, result.Results[3]["id"].get<uint32_t>())->Settings ==
	      Json::object({{"Graph", "77"}, {"Variable", health}}));
	CHECK(FindNode(result.Document, result.Results[5]["id"].get<uint32_t>())->Settings["Function"] == heal);

	// Another graph's names are looked up in its document; without one at hand they cannot be.
	VisualScriptDocument other;
	VisualScriptVariable lives;
	lives.ID = 4;
	lives.Name = "Lives";
	lives.Type = "int";
	lives.Public = true;
	other.Variables.push_back(lives);
	Json const remote = Json::array({Json::object(
		{{"op", "AddNode"}, {"type", "Variable.Get"}, {"settings", Json::object({{"Graph", "88"}, {"Variable", "Lives"}})}})});
	VisualScriptBatchError const unresolved = ExpectRefused(document, remote, fixture.Context);
	CHECK(unresolved.Path == "operations[0].settings.Variable");
	fixture.Context.FindDocument = [&other](AssetHandle graph) -> VisualScriptDocument const*
	{
		return graph == AssetHandle(88) ? &other : nullptr;
	};
	VisualScriptBatchResult const found = ExpectApplied(document, remote, fixture.Context);
	CHECK(FindNode(found.Document, found.Results[0]["id"].get<uint32_t>())->Settings["Variable"] == 4);

	// An unknown name names the closest ones.
	VisualScriptBatchError const unknown = ExpectRefused(
		document,
		Json::array({Json::object({{"op", "AddNode"}, {"type", "Variable.Get"}, {"settings", Json::object({{"Variable", "Sped"}})}})}),
		fixture.Context);
	CHECK(unknown.Path == "operations[0].settings.Variable");
	CHECK(unknown.Data["names"][0] == "Speed");
}

TEST_CASE("VisualScriptBatch: asset texts are stored as handles for asset-typed pins and values only")
{
	BatchFixture fixture;
	fixture.Context.FindAsset = [](std::string_view reference) -> std::optional<AssetHandle>
	{
		return reference == "asset://Prefabs/Crate.sprefab" ? std::optional<AssetHandle>(AssetHandle(1234)) : std::nullopt;
	};
	VisualScriptDocument const document = MakeDocument();
	Json const operations = Json::array({
		Json::object({{"op", "AddNode"},
	                  {"type", std::string(InstantiateType)},
	                  {"literals", Json::object({{"Prefab", "asset://Prefabs/Crate.sprefab"}})}}),
		Json::object({{"op", "SetInput"}, {"node", 2}, {"pin", "Message"}, {"value", "asset://Prefabs/Crate.sprefab"}}),
		Json::object({{"op", "AddVariable"}, {"name", "Crate"}, {"type", "Strada.Prefab"}, {"value", "asset://Prefabs/Crate.sprefab"}}),
	});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	CHECK(FindNode(result.Document, result.Results[0]["id"].get<uint32_t>())->Inputs == Json::object({{"Prefab", "1234"}}));
	// A string pin keeps the text.
	CHECK(FindNode(result.Document, 2)->Inputs == Json::object({{"Message", "asset://Prefabs/Crate.sprefab"}}));
	CHECK(FindVariable(result.Document, "Crate")->Value == "1234");
	// The asset literal needed the new node's pin type: one analysis.
	CHECK(result.Analyses == 1);

	VisualScriptBatchError const missing =
		ExpectRefused(document,
	                  Json::array({Json::object({{"op", "AddNode"},
	                                             {"type", std::string(InstantiateType)},
	                                             {"literals", Json::object({{"Prefab", "asset://Gone.sprefab"}})}})}),
	                  fixture.Context);
	CHECK(missing.Failure == VisualScriptBatchFailure::AssetNotFound);
	CHECK(missing.Path == "operations[0].literals.Prefab");
}

TEST_CASE("VisualScriptBatch: Connect, Relink and Splice list the links they replaced; Disconnect removes one link or a set")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	Json const connect =
		Json::array({Json::object({{"op", "Connect"}, {"from", Json::array({1, "Then"})}, {"to", Json::array({3, "Execute"})}})});
	VisualScriptBatchResult const connected = ExpectApplied(document, connect, fixture.Context);
	CHECK(connected.Results[0] ==
	      Json::object(
			  {{"replaced", Json::array({Json::object({{"from", Json::array({1, "Then"})}, {"to", Json::array({2, "Execute"})}})})}}));
	CHECK(HasLink(connected.Document.Graph, MakeLink(1, "Then", 3, "Execute")));

	Json const relink = Json::array(
		{Json::object({{"op", "Relink"},
	                   {"remove", Json::array({Json::object({{"from", Json::array({2, "Then"})}, {"to", Json::array({3, "Execute"})}})})},
	                   {"add", Json::array({Json::object({{"from", Json::array({1, "Then"})}, {"to", Json::array({3, "Execute"})}})})}})});
	VisualScriptBatchResult const relinked = ExpectApplied(document, relink, fixture.Context);
	CHECK(relinked.Results[0]["replaced"] ==
	      Json::array({Json::object({{"from", Json::array({1, "Then"})}, {"to", Json::array({2, "Execute"})}})}));

	Json const splice =
		Json::array({Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}, {"as", "$log"}}),
	                 Json::object({{"op", "Splice"},
	                               {"node", "$log"},
	                               {"link", Json::object({{"from", Json::array({1, "Then"})}, {"to", Json::array({2, "Execute"})}})}})});
	VisualScriptBatchResult const spliced = ExpectApplied(document, splice, fixture.Context);
	uint32_t const log = GetLabel(spliced, "$log");
	CHECK(spliced.Results[1] == Json::object({{"replaced", Json::array()}}));
	CHECK(HasLink(spliced.Document.Graph, MakeLink(1, "Then", log, "Execute")));
	CHECK(HasLink(spliced.Document.Graph, MakeLink(log, "Then", 2, "Execute")));
	CHECK_FALSE(HasLink(spliced.Document.Graph, MakeLink(1, "Then", 2, "Execute")));

	// Disconnect: one exact link (an error when absent), every link of an output, or of a node.
	Json const exact =
		Json::array({Json::object({{"op", "Disconnect"}, {"from", Json::array({1, "Then"})}, {"to", Json::array({2, "Execute"})}})});
	CHECK(ExpectApplied(document, exact, fixture.Context).Results[0] == Json::object({{"removed", 1}}));
	Json const absent =
		Json::array({Json::object({{"op", "Disconnect"}, {"from", Json::array({1, "Then"})}, {"to", Json::array({3, "Execute"})}})});
	CHECK(ExpectRefused(document, absent, fixture.Context).Path == "operations[0]");
	Json const ofNode = Json::array({Json::object({{"op", "Disconnect"}, {"node", 2}})});
	VisualScriptBatchResult const cut = ExpectApplied(document, ofNode, fixture.Context);
	CHECK(cut.Results[0] == Json::object({{"removed", 2}}));
	CHECK(cut.Document.Graph.Links.empty());
	Json const ofInput = Json::array({Json::object({{"op", "Disconnect"}, {"to", Json::array({1, "Execute"})}})});
	CHECK(ExpectApplied(document, ofInput, fixture.Context).Results[0] == Json::object({{"removed", 0}}));
}

TEST_CASE("VisualScriptBatch: pins are analyzed again only when earlier operations may have changed them")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	auto const add = [](std::string_view label)
	{
		return Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}, {"as", std::string(label)}});
	};
	auto const connect = [](Json from, Json to)
	{
		return Json::object(
			{{"op", "Connect"}, {"from", Json::array({std::move(from), "Then"})}, {"to", Json::array({std::move(to), "Execute"})}});
	};
	// Add, connect, add, connect: the second connection needs the second node's pins.
	VisualScriptBatchResult const interleaved =
		ExpectApplied(document, Json::array({add("$a"), connect(2, "$a"), add("$b"), connect("$a", "$b")}), fixture.Context);
	CHECK(interleaved.Analyses == 2);
	CHECK(fixture.Semantics.GetAnalyzeCallCount() == 2);
	// Adds, then connects: one analysis.
	VisualScriptBatchResult const grouped =
		ExpectApplied(document, Json::array({add("$a"), add("$b"), connect(2, "$a"), connect("$a", "$b")}), fixture.Context);
	CHECK(grouped.Analyses == 1);
	// A link changed the program since the last analysis, so the result has none the caller could reuse.
	CHECK(grouped.Analysis == nullptr);

	// A cached analysis of the stored document serves the first operation that needs pins.
	Result<VisualScriptAnalysis> stored = fixture.Semantics.Analyze(document, AssetHandle());
	REQUIRE(stored.IsOk());
	fixture.Context.StoredAnalysis = CreateRef<VisualScriptAnalysis const>(stored.TakeValue());
	fixture.Context.StoredAnalysisDocument = CreateRef<VisualScriptDocument const>(document);
	VisualScriptBatchResult const cached = ExpectApplied(document, Json::array({connect(1, 3)}), fixture.Context);
	CHECK(cached.Analyses == 0);
	// Moves leave the analysis describing the result, so the caller reuses it.
	VisualScriptBatchResult const layout =
		ExpectApplied(document, Json::array({Json::object({{"op", "SetPosition"}, {"element", 3}, {"position", Json::array({704, 0})}})}),
	                  fixture.Context);
	CHECK(layout.Analyses == 0);
	CHECK(layout.Analysis == fixture.Context.StoredAnalysis);
	// Another document than the one analyzed: the cache is not used.
	VisualScriptDocument other = document;
	other.Description = "Other";
	VisualScriptBatchResult const uncached = ExpectApplied(other, Json::array({connect(1, 3)}), fixture.Context);
	CHECK(uncached.Analyses == 1);
}

TEST_CASE("VisualScriptBatch: without the runtime pins are unavailable, while deletions and measured layout run degraded")
{
	BatchFixture fixture;
	fixture.Semantics.SetAvailable(false);
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(4, "Variable.Set", glm::vec2(960.0f, 0.0f), Json::object({{"Variable", 10}})));
	document.Graph.Links.push_back(MakeLink(3, "Completed", 4, "Execute"));

	VisualScriptBatchError const connect = ExpectRefused(
		document, Json::array({Json::object({{"op", "Connect"}, {"from", Json::array({1, "Then"})}, {"to", Json::array({3, "Execute"})}})}),
		fixture.Context);
	CHECK(connect.Failure == VisualScriptBatchFailure::NeedsRuntime);
	CHECK(connect.Path == "operations[0]");
	CHECK(connect.Message.find("needs the scripting runtime") != std::string::npos);

	Json const operations = Json::array({
		Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}, {"as", "$log"}}),
		Json::object({{"op", "Arrange"}}),
		Json::object({{"op", "Align"}, {"elements", Json::array({1, 2})}, {"alignment", "Top"}}),
		Json::object({{"op", "Distribute"}, {"elements", Json::array({1, 2, 3})}, {"axis", "Horizontal"}}),
		Json::object({{"op", "Straighten"}, {"nodes", Json::array({1, 2})}}),
		Json::object({{"op", "Delete"}, {"elements", Json::array({2})}}),
		Json::object({{"op", "DeleteVariable"}, {"variable", "Speed"}}),
		Json::object({{"op", "DeleteLocal"}, {"function", "Push"}, {"local", "Count"}}),
		Json::object({{"op", "DeleteFunction"}, {"function", 20}}),
	});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	CHECK(result.Analyses == 0);
	CHECK(result.Analysis == nullptr);
	// Without pins only reroutes bridge: the flow through the deleted Log is not kept.
	CHECK(result.Results[5] == Json::object({{"nodes", 1}, {"comments", 0}, {"notes", Json::array()}}));
	CHECK_FALSE(HasLink(result.Document.Graph, MakeLink(1, "Then", 3, "Execute")));
	CHECK(result.Results[6] == Json::object({{"removedNodes", 1}}));
	CHECK(result.Document.Variables.empty());
	CHECK(result.Document.Functions.empty());
	CHECK(result.Results[1] == Json::object({{"arranged", 5}}));
}

TEST_CASE("VisualScriptBatch: a runtime that fails to analyze makes the operations that need pins unavailable")
{
	BatchFixture fixture;
	fixture.Semantics.SetAnalyzeFails(true);
	VisualScriptDocument const document = MakeDocument();
	VisualScriptBatchError const error = ExpectRefused(
		document, Json::array({Json::object({{"op", "Connect"}, {"from", Json::array({1, "Then"})}, {"to", Json::array({3, "Execute"})}})}),
		fixture.Context);
	CHECK(error.Failure == VisualScriptBatchFailure::AnalysisFailed);
	CHECK(error.Message.find("see the log") != std::string::npos);
	// What needs no pins still runs; the result has no analysis.
	VisualScriptBatchResult const result =
		ExpectApplied(document, Json::array({Json::object({{"op", "AddNode"}, {"type", "Flow.Branch"}})}), fixture.Context);
	CHECK(result.Analysis == nullptr);
}

TEST_CASE("VisualScriptBatch: Delete bridges flow as the canvas does; Duplicate and Paste label what they add")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	VisualScriptBatchResult const bridged =
		ExpectApplied(document, Json::array({Json::object({{"op", "Delete"}, {"elements", Json::array({2})}})}), fixture.Context);
	CHECK(HasLink(bridged.Document.Graph, MakeLink(1, "Then", 3, "Execute")));
	VisualScriptBatchResult const cut = ExpectApplied(
		document, Json::array({Json::object({{"op", "Delete"}, {"elements", Json::array({2})}, {"bridge", false}})}), fixture.Context);
	CHECK(cut.Document.Graph.Links.empty());
	// A function's entry stays, with a note.
	VisualScriptBatchResult const entry =
		ExpectApplied(document, Json::array({Json::object({{"op", "Delete"}, {"elements", Json::array({21, 22})}})}), fixture.Context);
	CHECK(entry.Results[0]["nodes"] == 1);
	CHECK(entry.Results[0]["notes"].size() == 1);

	VisualScriptBatchResult const duplicated = ExpectApplied(
		document,
		Json::array(
			{Json::object({{"op", "Duplicate"}, {"elements", Json::array({2, 3})}, {"labels", Json::array({"$log", "$delay"})}}),
	         Json::object({{"op", "Connect"}, {"from", Json::array({"$log", "Then"})}, {"to", Json::array({"$delay", "Execute"})}})}),
		fixture.Context);
	uint32_t const log = GetLabel(duplicated, "$log");
	CHECK(FindNode(duplicated.Document, log)->Type == LogType);
	CHECK(FindNode(duplicated.Document, log)->Position == glm::vec2(352.0f, 32.0f));
	CHECK(duplicated.Results[0]["ids"] == Json::array({log, GetLabel(duplicated, "$delay")}));

	std::string const clipboard = WriteVisualScriptClipboard(document, 0, std::vector<uint32_t>{2}, AssetHandle(), nullptr);
	VisualScriptBatchResult const pasted = ExpectApplied(document,
	                                                     Json::array({Json::object({{"op", "Paste"},
	                                                                                {"clipboard", clipboard},
	                                                                                {"function", "Push"},
	                                                                                {"at", Json::array({0, 320})},
	                                                                                {"labels", Json::array({"$pasted"})}})}),
	                                                     fixture.Context);
	VisualScriptNode const* const copy = FindNode(pasted.Document, GetLabel(pasted, "$pasted"));
	REQUIRE(copy != nullptr);
	CHECK(copy->Position == glm::vec2(0.0f, 320.0f));
	CHECK(pasted.Document.Functions[0].Graph.Nodes.back().ID == copy->ID);
	// By default below the sub-graph's nodes.
	VisualScriptBatchResult const below =
		ExpectApplied(document, Json::array({Json::object({{"op", "Paste"}, {"clipboard", clipboard}})}), fixture.Context);
	VisualScriptNode const* const placed = FindNode(below.Document, below.Results[0]["ids"][0].get<uint32_t>());
	CHECK(placed->Position->x == 0.0f);
	CHECK(placed->Position->y > 112.0f);
	VisualScriptBatchError const damaged =
		ExpectRefused(document, Json::array({Json::object({{"op", "Paste"}, {"clipboard", "{ nothing"}})}), fixture.Context);
	CHECK(damaged.Path == "operations[0].clipboard");
}

TEST_CASE("VisualScriptBatch: SetSetting routes the settings that change pins to their operations")
{
	BatchFixture fixture;
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(4, "Flow.Sequence", glm::vec2(0.0f, 320.0f), Json::object({{"Count", 3}})));
	document.Graph.Links.push_back(MakeLink(4, "Then2", 2, "Execute"));
	document.Graph.Nodes.push_back(MakeNode(5, "Event.Custom", glm::vec2(0.0f, 640.0f), Json::object({{"Name", "Hit"}})));
	document.Graph.Nodes.push_back(MakeNode(6, "Event.Send", glm::vec2(320.0f, 640.0f), Json::object({{"Name", "Hit"}})));
	Json const operations = Json::array({
		Json::object({{"op", "SetSetting"}, {"node", 4}, {"name", "Count"}, {"value", 2}}),
		Json::object({{"op", "SetSetting"}, {"node", 5}, {"name", "Name"}, {"value", "Struck"}}),
		Json::object({{"op", "SetSetting"},
	                  {"node", 5},
	                  {"name", "Parameters"},
	                  {"value", Json::array({Json::object({{"Name", "Damage"}, {"Type", "float"}})})}}),
		Json::object({{"op", "SetSetting"}, {"node", 2}, {"name", "Note"}, {"value", "kept"}}),
		Json::object({{"op", "SetSequenceOutputs"}, {"node", 4}, {"count", 4}, {"insertAt", 0}}),
		Json::object({{"op", "RenameCustomEvent"}, {"node", 5}, {"name", "Blow"}}),
		Json::object({{"op", "SetEventParameters"},
	                  {"node", 6},
	                  {"parameters", Json::array({Json::object({{"name", "Damage"}, {"type", "float"}})})}}),
	});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	CHECK(result.Results[0] == Json::object({{"removedLinks", 1}}));
	CHECK(result.Results[1] == Json::object({{"renamedSenders", 1}}));
	CHECK(result.Results[2] == Json::object({{"removedLinks", 0}}));
	CHECK(result.Results[3] == Json::object());
	CHECK(result.Results[4] == Json::object({{"removedLinks", 0}}));
	CHECK(result.Results[5] == Json::object({{"renamedSenders", 1}}));
	CHECK(FindNode(result.Document, 4)->Settings["Count"] == 4);
	CHECK(FindNode(result.Document, 6)->Settings["Name"] == "Blow");
	CHECK(FindNode(result.Document, 2)->Settings["Note"] == "kept");
	CHECK(FindNode(result.Document, 6)->Settings["Parameters"] == Json::array({Json::object({{"Name", "Damage"}, {"Type", "float"}})}));
}

TEST_CASE("VisualScriptBatch: pins named as the analysis shows them are stored under the document's names")
{
	// A runtime whose analysis renamed the Log's Message to Text (a redirect).
	class RenamingSemantics final : public VisualScriptSemantics
	{
	public:
		Ref<VisualScriptLibrary const> GetLibrary() override { return m_Fixture.GetLibrary(); }
		[[nodiscard]] Result<VisualScriptAnalysis> Analyze(VisualScriptDocument const& document, AssetHandle self) override
		{
			Result<VisualScriptAnalysis> analysis = m_Fixture.Analyze(document, self);
			if (analysis)
			{
				for (VisualScriptAnalyzedNode& node : analysis.GetValue().Nodes)
				{
					if (node.ID != 2)
					{
						continue;
					}
					node.Resolved = VisualScriptResolution::Redirect;
					node.CanonicalType = node.Type;
					node.PinRenames.push_back({VisualScriptPinDirection::In, "Message", "Text"});
					for (VisualScriptPin& pin : node.Pins)
					{
						pin.Name = pin.Name == "Message" ? "Text" : pin.Name;
					}
				}
			}
			return analysis;
		}

	private:
		FixtureVisualScriptSemantics m_Fixture;
	};

	RenamingSemantics semantics;
	VisualScriptBatchContext context(semantics);
	VisualScriptDocument const document = MakeDocument();
	Json const operations = Json::array({
		Json::object({{"op", "AddNode"}, {"type", "Get:Strada.Time.DeltaTime"}, {"as", "$time"}}),
		Json::object({{"op", "Connect"}, {"from", Json::array({"$time", "Value"})}, {"to", Json::array({2, "Text"})}}),
		Json::object({{"op", "SetInput"}, {"node", 2}, {"pin", "Text"}, {"value", "b"}}),
	});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, context);
	uint32_t const time = GetLabel(result, "$time");
	CHECK(HasLink(result.Document.Graph, MakeLink(time, "Value", 2, "Message")));
	CHECK(FindNode(result.Document, 2)->Inputs == Json::object({{"Message", "b"}}));
}

TEST_CASE("VisualScriptBatch: variables, locals and functions are added, changed and removed by name and label")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	Json const operations = Json::array({
		Json::object({{"op", "AddVariable"},
	                  {"name", "Lives"},
	                  {"type", "int"},
	                  {"value", 3},
	                  {"public", true},
	                  {"tooltip", "How many"},
	                  {"range", Json::array({0, 9})},
	                  {"as", "$lives"}}),
		Json::object({{"op", "SetVariable"}, {"variable", "$lives"}, {"name", "Hearts"}, {"type", "float"}}),
		Json::object({{"op", "DuplicateVariable"}, {"variable", "Speed"}, {"name", "MaxSpeed"}}),
		Json::object({{"op", "ReorderVariables"}, {"variables", Json::array({"MaxSpeed", "$lives", 10})}}),
		Json::object({{"op", "AddLocal"}, {"function", "Push"}, {"name", "Step"}, {"type", "float"}, {"value", 0.5}, {"as", "$step"}}),
		Json::object({{"op", "SetLocal"}, {"function", "Push"}, {"local", "$step"}, {"name", "Stride"}}),
		Json::object({{"op", "AddFunction"},
	                  {"name", "Clamp"},
	                  {"pure", true},
	                  {"inputs", Json::array({Json::object({{"name", "Value"}, {"type", "float"}})})},
	                  {"outputs", Json::array({Json::object({{"name", "Result"}, {"type", "float"}})})},
	                  {"as", "$clamp"},
	                  {"entryAs", "$entry"},
	                  {"returnAs", "$return"}}),
		Json::object({{"op", "Connect"}, {"from", Json::array({"$entry", "Value"})}, {"to", Json::array({"$return", "Result"})}}),
		Json::object({{"op", "SetFunctionSignature"},
	                  {"function", "$clamp"},
	                  {"inputs", Json::array({Json::object({{"name", "Amount"}, {"type", "float"}})})},
	                  {"outputs", Json::array({Json::object({{"name", "Result"}, {"type", "float"}})})}}),
		Json::object({{"op", "DuplicateFunction"}, {"function", "Push"}, {"name", "Pull"}}),
		Json::object({{"op", "SetFunctionPure"}, {"function", "Pull"}, {"pure", true}}),
		Json::object({{"op", "DeleteLocal"}, {"function", "Push"}, {"local", "Count"}}),
		Json::object({{"op", "DeleteFunction"}, {"function", "Push"}}),
	});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	VisualScriptVariable const* const hearts = FindVariable(result.Document, "Hearts");
	REQUIRE(hearts != nullptr);
	CHECK(hearts->Type == "float");
	CHECK(hearts->Public);
	CHECK(hearts->Tooltip == "How many");
	CHECK(result.Results[1].contains("valueCleared"));
	REQUIRE(result.Document.Variables.size() == 3);
	CHECK(result.Document.Variables[0].Name == "MaxSpeed");
	CHECK(result.Document.Variables[1].Name == "Hearts");
	CHECK(result.Document.Variables[2].Name == "Speed");
	uint32_t const clamp = GetLabel(result, "$clamp");
	CHECK(result.Results[6] == Json::object({{"id", clamp}, {"entry", clamp + 1}, {"return", clamp + 2}}));
	auto const function = std::find_if(result.Document.Functions.begin(), result.Document.Functions.end(),
	                                   [clamp](VisualScriptFunction const& candidate)
	                                   {
										   return candidate.ID == clamp;
									   });
	REQUIRE(function != result.Document.Functions.end());
	CHECK(function->Pure);
	CHECK(function->Inputs[0].Name == "Amount");
	// The signature edit renamed the entry's pin and kept its link.
	CHECK(HasLink(function->Graph, MakeLink(clamp + 1, "Amount", clamp + 2, "Result")));
	// Push is gone; its copy Pull keeps the locals it had then.
	REQUIRE(result.Document.Functions.size() == 2);
	VisualScriptFunction const& pull = result.Document.Functions[0];
	CHECK(pull.Name == "Pull");
	CHECK(pull.Pure);
	REQUIRE(pull.Locals.size() == 2);
	CHECK(pull.Locals[1].Name == "Stride");
}

TEST_CASE("VisualScriptBatch: promotions, splits and the quick fixes the analysis offers")
{
	BatchFixture fixture;
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(4, AddForceType, glm::vec2(960.0f, 0.0f)));
	document.Graph.Links.push_back(MakeLink(3, "Completed", 4, "Execute"));
	Json const operations = Json::array({
		Json::object(
			{{"op", "PromoteToVariable"}, {"node", 3}, {"pin", "Duration"}, {"direction", "In"}, {"name", "Wait"}, {"as", "$wait"}}),
		Json::object({{"op", "SplitPin"}, {"node", 4}, {"pin", "Force"}, {"direction", "In"}}),
		Json::object({{"op", "SetInput"}, {"node", 4}, {"pin", "Force.Y"}, {"value", 9.5}}),
	});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	VisualScriptVariable const* const wait = FindVariable(result.Document, "Wait");
	REQUIRE(wait != nullptr);
	CHECK(result.Results[0] == Json::object({{"variable", wait->ID}, {"node", GetLabel(result, "$wait")}}));
	CHECK(HasLink(result.Document.Graph, MakeLink(GetLabel(result, "$wait"), "Value", 3, "Duration")));
	CHECK(FindNode(result.Document, 4)->Split.Inputs == std::vector<std::string>{"Force"});
	CHECK(FindNode(result.Document, 4)->Inputs == Json::object({{"Force.Y", 9.5}}));
	// A field of the split pin recombines it.
	VisualScriptBatchResult const recombined = ExpectApplied(
		result.Document, Json::array({Json::object({{"op", "RecombinePin"}, {"node", 4}, {"pin", "Force.X"}, {"direction", "In"}})}),
		fixture.Context);
	CHECK(FindNode(recombined.Document, 4)->Split.IsEmpty());

	// A function input promoted from an unlinked input of its graph, named.
	VisualScriptDocument withCall = MakeDocument();
	withCall.Functions[0].Graph.Nodes.push_back(
		MakeNode(24, LogType, glm::vec2(240.0f, 160.0f), Json::object(), Json::object({{"Message", "m"}})));
	VisualScriptBatchResult const input = ExpectApplied(
		withCall, Json::array({Json::object({{"op", "PromoteToFunctionInput"}, {"node", 24}, {"pin", "Message"}, {"name", "Text"}})}),
		fixture.Context);
	REQUIRE(input.Document.Functions[0].Inputs.size() == 1);
	CHECK(input.Document.Functions[0].Inputs[0].Name == "Text");
	CHECK(HasLink(input.Document.Functions[0].Graph, MakeLink(21, "Text", 24, "Message")));

	// Fixes: an unused input renamed to the pin the analysis suggests, and a navigating fix refused naming the edit.
	VisualScriptDocument misnamed = MakeDocument();
	misnamed.Graph.Nodes[1].Inputs = Json::object({{"Mesage", "x"}});
	VisualScriptDiagnostic unused;
	unused.Severity = VisualScriptSeverity::Warning;
	unused.Code = "UnusedInput";
	unused.Node = 2u;
	unused.Pin = "Mesage";
	unused.Direction = VisualScriptPinDirection::In;
	unused.Message = "Log has no input Mesage";
	unused.Suggestions = {"Message"};
	fixture.Semantics.InjectDiagnostic(unused);
	Result<VisualScriptAnalysis> analysis = fixture.Semantics.Analyze(misnamed, AssetHandle());
	REQUIRE(analysis.IsOk());
	VisualScriptDiagnostic const& injected = analysis.GetValue().Diagnostics.back();
	Json const fixes = GetVisualScriptFixOperations(misnamed, analysis.GetValue(), injected, fixture.Semantics.GetFixtureLibrary());
	REQUIRE(fixes.size() == 2);
	CHECK(fixes[1]["label"] == "Use Message");
	CHECK(fixes[1]["operation"] == Json::object({{"op", "Fix"},
	                                             {"code", "UnusedInput"},
	                                             {"fix", "Use Message"},
	                                             {"function", 0},
	                                             {"node", 2},
	                                             {"pin", "Mesage"},
	                                             {"direction", "In"}}));
	VisualScriptBatchResult const fixed = ExpectApplied(misnamed, Json::array({fixes[1]["operation"]}), fixture.Context);
	CHECK(FindNode(fixed.Document, 2)->Inputs == Json::object({{"Message", "x"}}));
	CHECK(fixed.Results[0] == Json::object({{"fix", "Use Message"}}));
	VisualScriptBatchError const unmatched =
		ExpectRefused(misnamed, Json::array({Json::object({{"op", "Fix"}, {"code", "UnusedInput"}, {"fix", "Use Message"}, {"node", 3}})}),
	                  fixture.Context);
	CHECK(unmatched.Data["candidates"].size() == 1);

	misnamed.Graph.Nodes.push_back(MakeNode(5, "Event.Update", glm::vec2(0.0f, 320.0f)));
	VisualScriptDiagnostic duplicate;
	duplicate.Code = "DuplicateEvent";
	duplicate.Node = 5u;
	duplicate.Message = "a second Update";
	fixture.Semantics.InjectDiagnostic(duplicate);
	Result<VisualScriptAnalysis> twice = fixture.Semantics.Analyze(misnamed, AssetHandle());
	REQUIRE(twice.IsOk());
	std::vector<VisualScriptQuickFix> const offered =
		GetVisualScriptQuickFixes(misnamed, twice.GetValue(), twice.GetValue().Diagnostics.back(), fixture.Semantics.GetFixtureLibrary());
	REQUIRE(offered.size() == 2);
	REQUIRE(offered[0].Navigates);
	// Navigating fixes are not operations.
	CHECK(
		GetVisualScriptFixOperations(misnamed, twice.GetValue(), twice.GetValue().Diagnostics.back(), fixture.Semantics.GetFixtureLibrary())
			.size() == 1);
	VisualScriptBatchError const navigating = ExpectRefused(
		misnamed, Json::array({Json::object({{"op", "Fix"}, {"code", "DuplicateEvent"}, {"fix", offered[0].Label}, {"node", 5}})}),
		fixture.Context);
	CHECK(navigating.Path == "operations[0].fix");
	CHECK(navigating.Message.find("make the edit with Delete") != std::string::npos);
}

TEST_CASE("VisualScriptBatch: comment boxes, positions, node states and the description")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	Json const operations = Json::array({
		Json::object({{"op", "AddComment"},
	                  {"text", "Flow"},
	                  {"elements", Json::array({1, 2})},
	                  {"color", Json::array({1, 0, 0, 0.5})},
	                  {"as", "$box"}}),
		Json::object({{"op", "SetComment"}, {"comment", "$box"}, {"text", "Main flow"}, {"size", Json::array({800, 300})}}),
		Json::object({{"op", "AddComment"}, {"text", "Alone"}, {"position", Json::array({0, 800})}}),
		Json::object({{"op", "Move"}, {"elements", Json::array({3, 21})}, {"offset", Json::array({16, 32})}}),
		Json::object({{"op", "SetPosition"}, {"element", 2}, {"position", Json::array({320, 64})}}),
		Json::object({{"op", "SetNodeComment"}, {"nodes", Json::array({2, 3})}, {"text", "note"}}),
		Json::object({{"op", "SetCollapsed"}, {"nodes", Json::array({3})}, {"collapsed", true}}),
		Json::object({{"op", "SetEnabled"}, {"nodes", Json::array({2})}, {"enabled", "DevelopmentOnly"}}),
		Json::object({{"op", "SetDescription"}, {"description", "Moves"}}),
		Json::object({{"op", "InsertReroute"},
	                  {"link", Json::object({{"from", Json::array({1, "Then"})}, {"to", Json::array({2, "Execute"})}})},
	                  {"position", Json::array({200, 100})},
	                  {"as", "$reroute"}}),
	});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	REQUIRE(result.Document.Graph.Comments.size() == 2);
	VisualScriptComment const& box = result.Document.Graph.Comments[0];
	CHECK(box.Text == "Main flow");
	CHECK(box.Size == glm::vec2(800.0f, 300.0f));
	CHECK(box.Color == glm::vec4(1.0f, 0.0f, 0.0f, 0.5f));
	CHECK(result.Document.Graph.Comments[1].Position == glm::vec2(0.0f, 800.0f));
	CHECK(FindNode(result.Document, 3)->Position == glm::vec2(656.0f, 32.0f));
	CHECK(FindNode(result.Document, 21)->Position == glm::vec2(16.0f, 32.0f));
	CHECK(FindNode(result.Document, 2)->Position == glm::vec2(320.0f, 64.0f));
	CHECK(FindNode(result.Document, 2)->Comment == "note");
	CHECK(FindNode(result.Document, 3)->Collapsed);
	CHECK(FindNode(result.Document, 2)->Enabled == VisualScriptNodeEnabled::DevelopmentOnly);
	CHECK(result.Document.Description == "Moves");
	uint32_t const reroute = GetLabel(result, "$reroute");
	CHECK(HasLink(result.Document.Graph, MakeLink(1, "Then", reroute, "In")));
	CHECK(HasLink(result.Document.Graph, MakeLink(reroute, "Out", 2, "Execute")));
	// Layout alone needs no analysis.
	CHECK(fixture.Semantics.GetAnalyzeCallCount() == 1);
}

TEST_CASE("VisualScriptBatch: Arrange's Created scope places new nodes beside the nodes they continue")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	auto const arrange = Json::object({{"op", "Arrange"}, {"scope", "Created"}, {"extents", "Uniform"}});

	SUBCASE("A chain extended from its last node continues right of it")
	{
		Json const operations = Json::array(
			{Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}, {"as", "$next"}}),
		     Json::object({{"op", "Connect"}, {"from", Json::array({3, "Completed"})}, {"to", Json::array({"$next", "Execute"})}}),
		     arrange});
		VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
		CHECK(result.Results[2] == Json::object({{"arranged", 1}}));
		CHECK(FindNode(result.Document, GetLabel(result, "$next"))->Position == glm::vec2(960.0f, 0.0f));
	}
	SUBCASE("A template's new Update chain starts right of Event.Update")
	{
		VisualScriptDocument const events = MakeVisualScriptTemplate(VisualScriptTemplate::Events);
		VisualScriptNode const* update = nullptr;
		for (VisualScriptNode const& node : events.Graph.Nodes)
		{
			update = node.Type == "Event.Update" ? &node : update;
		}
		REQUIRE(update != nullptr);
		Json const operations = Json::array(
			{Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}, {"as", "$log"}}),
		     Json::object({{"op", "Connect"}, {"from", Json::array({update->ID, "Then"})}, {"to", Json::array({"$log", "Execute"})}}),
		     arrange});
		VisualScriptBatchResult const result = ExpectApplied(events, operations, fixture.Context);
		glm::vec2 const position = *FindNode(result.Document, GetLabel(result, "$log"))->Position;
		CHECK(position.x == std::ceil((update->Position->x + 256.0f + 64.0f) / 16.0f) * 16.0f);
		CHECK(position.y >= update->Position->y);
	}
	SUBCASE("A group linked only into a placed node stands left of it, below what it would overlap")
	{
		Json const operations = Json::array(
			{Json::object({{"op", "AddNode"}, {"type", "Get:Strada.Time.DeltaTime"}, {"as", "$time"}}),
		     Json::object({{"op", "Connect"}, {"from", Json::array({"$time", "Value"})}, {"to", Json::array({3, "Duration"})}}), arrange});
		VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
		// Its right edge 64 left of the Delay, at its top; then down past the Log it would overlap.
		CHECK(FindNode(result.Document, GetLabel(result, "$time"))->Position == glm::vec2(320.0f, 160.0f));
	}
	SUBCASE("An unconnected group goes below everything, and steps down past a comment box")
	{
		Json const operations = Json::array({Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}, {"as", "$alone"}}), arrange});
		VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
		CHECK(FindNode(result.Document, GetLabel(result, "$alone"))->Position == glm::vec2(0.0f, 240.0f));

		VisualScriptDocument boxed = document;
		VisualScriptComment box;
		box.ID = 30;
		box.Position = glm::vec2(0.0f, 200.0f);
		box.Size = glm::vec2(400.0f, 200.0f);
		boxed.Graph.Comments.push_back(box);
		VisualScriptBatchResult const stepped = ExpectApplied(boxed, operations, fixture.Context);
		CHECK(FindNode(stepped.Document, GetLabel(stepped, "$alone"))->Position == glm::vec2(0.0f, 432.0f));
	}
	SUBCASE("Scopes Graph and Nodes")
	{
		Json const graph = Json::array({Json::object({{"op", "Arrange"}, {"function", "Push"}, {"extents", "Uniform"}})});
		CHECK(ExpectApplied(document, graph, fixture.Context).Results[0] == Json::object({{"arranged", 2}}));
		Json const nodes = Json::array({Json::object({{"op", "Arrange"}, {"scope", "Nodes"}, {"nodes", Json::array({2, 3})}})});
		CHECK(ExpectApplied(document, nodes, fixture.Context).Results[0] == Json::object({{"arranged", 2}}));
		Json const mixed = Json::array({Json::object({{"op", "Arrange"}, {"nodes", Json::array({2, 3})}})});
		CHECK(ExpectRefused(document, mixed, fixture.Context).Path == "operations[0].nodes");
	}
}

TEST_CASE("VisualScriptBatch: hints name nodes that never run, outputs read by nothing and formula inputs left at 0")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	// A Connect that replaces Update's link into the Log leaves the Log without flow.
	Json const operations = Json::array(
		{Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}, {"position", Json::array({320, 320})}, {"as", "$log"}}),
	     Json::object({{"op", "Connect"}, {"from", Json::array({1, "Then"})}, {"to", Json::array({"$log", "Execute"})}}),
	     Json::object({{"op", "AddNode"},
	                   {"type", "Variable.Get"},
	                   {"settings", Json::object({{"Variable", 10}})},
	                   {"position", Json::array({0, 320})}})});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	Result<VisualScriptAnalysis> analysis = fixture.Semantics.Analyze(result.Document, AssetHandle());
	REQUIRE(analysis.IsOk());
	uint32_t const get = result.Results[2]["id"].get<uint32_t>();
	std::vector<uint32_t> const nodes{2, GetLabel(result, "$log"), get, 1};
	std::vector<VisualScriptHint> const hints = FindVisualScriptHints(result.Document, analysis.GetValue(), nodes);
	REQUIRE(hints.size() == 2);
	CHECK(hints[0].Node == 2);
	CHECK(hints[0].Kind == "NeverRuns");
	CHECK(hints[0].Message.find("(node 2) has no flow into Execute: it never runs") != std::string::npos);
	CHECK(hints[1].Node == get);
	CHECK(hints[1].Kind == "Unused");

	// An Expression's input with neither a link nor a value.
	VisualScriptDocument formula;
	formula.Graph.Nodes.push_back(
		MakeNode(6, "Expression", glm::vec2(0.0f), Json::object({{"Formula", "speed * dt"}}), Json::object({{"speed", 2.0}})));
	VisualScriptAnalysis built;
	VisualScriptAnalyzedNode node;
	node.ID = 6;
	node.Type = "Expression";
	node.Title = "Expression";
	node.Kind = VisualScriptNodeKind::Data;
	node.Pure = true;
	for (auto const& [name, direction] : {std::pair<char const*, VisualScriptPinDirection>{"speed", VisualScriptPinDirection::In},
	                                      {"dt", VisualScriptPinDirection::In},
	                                      {"Result", VisualScriptPinDirection::Out}})
	{
		VisualScriptPin pin;
		pin.Name = name;
		pin.Direction = direction;
		pin.Type = "float";
		node.Pins.push_back(std::move(pin));
	}
	built.Nodes.push_back(node);
	std::vector<uint32_t> const expression{6};
	std::vector<VisualScriptHint> const unlinked = FindVisualScriptHints(formula, built, expression);
	REQUIRE(unlinked.size() == 2);
	CHECK(unlinked[0].Kind == "Unused");
	CHECK(unlinked[1] == VisualScriptHint{6, "UnlinkedInput", "Expression (node 6) reads dt as 0: link it or store a value"});
}

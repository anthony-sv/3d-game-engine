#include "Fuzzing.h"
#include "VisualScriptTestDocuments.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptArrange.h"
#include "Editor/VisualScripting/VisualScriptBatch.h"
#include "Editor/VisualScripting/VisualScriptClipboard.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptHeadlessCanvas.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"
#include "Editor/VisualScripting/VisualScriptTimeline.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
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
	Result<Ref<VisualScriptAnalysis const>> stored = fixture.Semantics.Analyze(document, AssetHandle());
	REQUIRE(stored.IsOk());
	fixture.Context.StoredAnalysis = stored.TakeValue();
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
		[[nodiscard]] Result<Ref<VisualScriptAnalysis const>> Analyze(VisualScriptDocument const& document, AssetHandle self) override
		{
			Result<Ref<VisualScriptAnalysis const>> shared = m_Fixture.Analyze(document, self);
			if (!shared)
			{
				return shared;
			}
			VisualScriptAnalysis analysis = *shared.GetValue();
			for (VisualScriptAnalyzedNode& node : analysis.Nodes)
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
			return CreateRef<VisualScriptAnalysis const>(std::move(analysis));
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
	Result<Ref<VisualScriptAnalysis const>> analysis = fixture.Semantics.Analyze(misnamed, AssetHandle());
	REQUIRE(analysis.IsOk());
	VisualScriptDiagnostic const& injected = analysis.GetValue()->Diagnostics.back();
	Json const fixes = GetVisualScriptFixOperations(misnamed, *analysis.GetValue(), injected, fixture.Semantics.GetFixtureLibrary());
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
	Result<Ref<VisualScriptAnalysis const>> twice = fixture.Semantics.Analyze(misnamed, AssetHandle());
	REQUIRE(twice.IsOk());
	std::vector<VisualScriptQuickFix> const offered =
		GetVisualScriptQuickFixes(misnamed, *twice.GetValue(), twice.GetValue()->Diagnostics.back(), fixture.Semantics.GetFixtureLibrary());
	REQUIRE(offered.size() == 2);
	REQUIRE(offered[0].Navigates);
	// Navigating fixes are not operations.
	CHECK(GetVisualScriptFixOperations(misnamed, *twice.GetValue(), twice.GetValue()->Diagnostics.back(),
	                                   fixture.Semantics.GetFixtureLibrary())
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
	Result<Ref<VisualScriptAnalysis const>> analysis = fixture.Semantics.Analyze(result.Document, AssetHandle());
	REQUIRE(analysis.IsOk());
	uint32_t const get = result.Results[2]["id"].get<uint32_t>();
	std::vector<uint32_t> const nodes{2, GetLabel(result, "$log"), get, 1};
	std::vector<VisualScriptHint> const hints = FindVisualScriptHints(result.Document, *analysis.GetValue(), nodes);
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

TEST_CASE("VisualScriptBatch: SetSwitchCases and SetSetting of Cases rename the links of changed cases and remove those of cases gone")
{
	BatchFixture fixture;
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(
		MakeNode(4, "Flow.Switch", glm::vec2(0.0f, 320.0f), Json::object({{"Type", "int"}, {"Cases", Json::array({1, 2, 3})}})));
	document.Graph.Links.push_back(MakeLink(4, "Case:2", 3, "Execute"));
	document.Graph.Links.push_back(MakeLink(4, "Case:3", 2, "Execute"));
	Json const operations = Json::array({
		// The same length: the case that changed is renamed, and so is its link.
		Json::object({{"op", "SetSwitchCases"}, {"node", 4}, {"cases", Json::array({1, 5, 3})}}),
		// Through SetSetting, routed to SetSwitchCases: the links of the cases gone go.
		Json::object({{"op", "SetSetting"}, {"node", 4}, {"name", "Cases"}, {"value", Json::array({1})}}),
		// null removes the setting: a Switch on int then has no cases.
		Json::object({{"op", "SetSwitchCases"}, {"node", 4}, {"cases", nullptr}}),
	});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	CHECK(result.Results[0] == Json::object({{"removedLinks", 0}}));
	CHECK(result.Results[1] == Json::object({{"removedLinks", 2}}));
	CHECK(result.Results[2] == Json::object({{"removedLinks", 0}}));
	CHECK_FALSE(FindNode(result.Document, 4)->Settings.contains("Cases"));

	VisualScriptBatchResult const renamed = ExpectApplied(document, Json::array({operations[0]}), fixture.Context);
	CHECK(HasLink(renamed.Document.Graph, MakeLink(4, "Case:5", 3, "Execute")));
	CHECK(HasLink(renamed.Document.Graph, MakeLink(4, "Case:3", 2, "Execute")));
	CHECK(FindNode(renamed.Document, 4)->Settings["Cases"] == Json::array({1, 5, 3}));
	VisualScriptBatchResult const routed = ExpectApplied(document, Json::array({operations[1]}), fixture.Context);
	CHECK(routed.Results[0] == Json::object({{"removedLinks", 2}}));
	CHECK(FindNode(routed.Document, 4)->Settings["Cases"] == Json::array({1}));
	CHECK(routed.Document.Graph.Links.size() == document.Graph.Links.size() - 2);
}

TEST_CASE("VisualScriptBatch: SetArrayElements and SetFormula renumber and rename pins with their links, also through SetSetting")
{
	BatchFixture fixture;
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(
		MakeNode(4, "Array.Make", glm::vec2(0.0f, 320.0f), Json::object({{"Count", 3}}), Json::object({{"Element2", "c"}})));
	document.Graph.Nodes.push_back(
		MakeNode(5, "Expression", glm::vec2(0.0f, 640.0f), Json::object({{"Formula", "speed * dt"}}), Json::object({{"dt", 0.5}})));
	document.Graph.Nodes.push_back(MakeNode(6, "Variable.Get", glm::vec2(-320.0f, 640.0f), Json::object({{"Variable", 10}})));
	document.Graph.Links.push_back(MakeLink(6, "Value", 4, "Element0"));
	document.Graph.Links.push_back(MakeLink(6, "Value", 5, "speed"));

	SUBCASE("the operations")
	{
		Json const operations = Json::array({
			Json::object({{"op", "SetArrayElements"}, {"node", 4}, {"count", 4}, {"insertAt", 0}}),
			Json::object({{"op", "SetFormula"}, {"node", 5}, {"formula", "velocity * dt"}}),
			Json::object({{"op", "SetArrayElements"}, {"node", 4}, {"count", 1}}),
			Json::object({{"op", "SetFormula"}, {"node", 5}, {"formula", "2 * pi"}}),
		});
		VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
		CHECK(result.Results[0] == Json::object({{"removedLinks", 0}}));
		CHECK(result.Results[1] ==
		      Json::object({{"renamedInputs", Json::array({Json::object({{"old", "speed"}, {"new", "velocity"}})})}, {"removedLinks", 0}}));
		// Element0 moved to Element1 first, and goes with the elements from 1 on.
		CHECK(result.Results[2] == Json::object({{"removedLinks", 1}}));
		CHECK(result.Results[3] == Json::object({{"renamedInputs", Json::array()}, {"removedLinks", 1}}));
		CHECK(FindNode(result.Document, 4)->Settings["Count"] == 1);
		CHECK(FindNode(result.Document, 4)->Inputs == Json::object());
		CHECK(FindNode(result.Document, 5)->Settings["Formula"] == "2 * pi");
		CHECK(FindNode(result.Document, 5)->Inputs == Json::object());
		CHECK(result.Document.Graph.Links.size() == document.Graph.Links.size() - 2);
	}

	SUBCASE("SetSetting routes Count and Formula to them")
	{
		Json const operations = Json::array({
			Json::object({{"op", "SetSetting"}, {"node", 4}, {"name", "Count"}, {"value", 2}}),
			Json::object({{"op", "SetSetting"}, {"node", 5}, {"name", "Formula"}, {"value", "velocity * dt"}}),
		});
		VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
		CHECK(result.Results[0] == Json::object({{"removedLinks", 0}}));
		CHECK(result.Results[1] ==
		      Json::object({{"renamedInputs", Json::array({Json::object({{"old", "speed"}, {"new", "velocity"}})})}, {"removedLinks", 0}}));
		CHECK(FindNode(result.Document, 4)->Inputs == Json::object());
		CHECK(HasLink(result.Document.Graph, MakeLink(6, "Value", 4, "Element0")));
		CHECK(HasLink(result.Document.Graph, MakeLink(6, "Value", 5, "velocity")));
		CHECK(FindNode(result.Document, 5)->Inputs == Json::object({{"dt", 0.5}}));
	}

	SUBCASE("what they cannot do is refused, naming the operation and the field")
	{
		struct Refusal
		{
			Json Operation;
			std::string Path;
			std::string Message;
		};
		std::vector<Refusal> const refusals = {
			{Json::object({{"op", "SetArrayElements"}, {"node", 4}, {"count", 4}, {"insertAt", 0}, {"removeAt", 1}}),
		     "operations[0].removeAt", "operations[0] (SetArrayElements): removeAt: SetArrayElements takes insertAt or removeAt, not both"},
			{Json::object({{"op", "SetArrayElements"}, {"node", 5}, {"count", 1}}), "operations[0]",
		     "operations[0] (SetArrayElements): node 5 is no Array.Make: its type is Expression"},
			{Json::object({{"op", "SetFormula"}, {"node", 4}, {"formula", "a"}}), "operations[0]",
		     "operations[0] (SetFormula): node 4 is no Expression: its type is Array.Make"},
			{Json::object({{"op", "SetSetting"}, {"node", 4}, {"name", "Count"}, {"value", "two"}}), "operations[0].value",
		     "operations[0] (SetSetting): value: the Count of a Make Array is an integer from 0 to 64"},
			{Json::object({{"op", "SetSetting"}, {"node", 5}, {"name", "Formula"}, {"value", 3}}), "operations[0].value",
		     "operations[0] (SetSetting): value: the Formula of an Expression is a string"},
		};
		for (Refusal const& refusal : refusals)
		{
			CAPTURE(refusal.Operation.dump());
			VisualScriptBatchError const error = ExpectRefused(document, Json::array({refusal.Operation}), fixture.Context);
			CHECK(error.Failure == VisualScriptBatchFailure::Invalid);
			CHECK(error.Kind == refusal.Operation["op"].get<std::string>());
			CHECK(error.Path == refusal.Path);
			CHECK(error.Message == refusal.Message);
		}
	}
}

TEST_CASE("VisualScriptBatch: SetTimeline sets a Timeline's tracks, renamed tracks keeping their links, also through SetSetting")
{
	BatchFixture fixture;
	VisualScriptDocument document = MakeDocument();
	Json const timeline = Json::parse(R"({"Length": 2, "Loop": false, "Tracks": [
		{"Name": "Lift", "Type": "float", "Keys": [{"Time": 0, "Value": 0}, {"Time": 2, "Value": 1}]},
		{"Name": "Step", "Type": "exec", "Keys": [{"Time": 1}]}]})");
	document.Graph.Nodes.push_back(MakeNode(4, "Flow.Timeline", glm::vec2(0.0f, 320.0f), Json::object({{"Timeline", timeline}})));
	document.Graph.Links.push_back(MakeLink(4, "Lift", 2, "Message"));
	document.Graph.Links.push_back(MakeLink(4, "Step", 3, "Execute"));

	SUBCASE("the operation")
	{
		Json renamed = timeline;
		renamed["Tracks"][0]["Name"] = "Height";
		renamed["Loop"] = true;
		Json removed = renamed;
		removed["Tracks"].erase(1);
		Json const operations = Json::array({
			Json::object({{"op", "SetTimeline"},
		                  {"node", 4},
		                  {"timeline", renamed},
		                  {"renames", Json::array({Json::object({{"old", "Lift"}, {"new", "Height"}})})}}),
			Json::object({{"op", "SetTimeline"}, {"node", 4}, {"timeline", removed}}),
		});
		VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
		CHECK(result.Results[0] == Json::object({{"removedLinks", 0}}));
		CHECK(result.Results[1] == Json::object({{"removedLinks", 1}}));
		CHECK(HasLink(result.Document.Graph, MakeLink(4, "Height", 2, "Message")));
		CHECK_FALSE(HasLink(result.Document.Graph, MakeLink(4, "Step", 3, "Execute")));
		// Written in the canonical form.
		Json const stored = FindNode(result.Document, 4)->Settings["Timeline"];
		CHECK(stored == WriteVisualScriptTimeline(ParseVisualScriptTimeline(removed).GetValue()));
		CHECK(stored["Loop"] == true);
	}

	SUBCASE("SetSetting routes Timeline to it, renaming no track; null sets the default timeline")
	{
		Json retyped = timeline;
		retyped["Tracks"][0]["Type"] = "Strada.Vector2";
		retyped["Tracks"][0]["Keys"] = Json::array();
		VisualScriptBatchResult const routed = ExpectApplied(
			document, Json::array({Json::object({{"op", "SetSetting"}, {"node", 4}, {"name", "Timeline"}, {"value", retyped}})}),
			fixture.Context);
		CHECK(routed.Results[0] == Json::object({{"removedLinks", 1}}));
		CHECK(HasLink(routed.Document.Graph, MakeLink(4, "Step", 3, "Execute")));
		VisualScriptBatchResult const cleared = ExpectApplied(
			document, Json::array({Json::object({{"op", "SetSetting"}, {"node", 4}, {"name", "Timeline"}, {"value", nullptr}})}),
			fixture.Context);
		CHECK(cleared.Results[0] == Json::object({{"removedLinks", 2}}));
		CHECK(FindNode(cleared.Document, 4)->Settings["Timeline"] == WriteVisualScriptTimeline(VisualScriptTimeline{}));
	}

	SUBCASE("what it cannot do is refused, naming the operation and the field")
	{
		struct Refusal
		{
			Json Operation;
			std::string Path;
			std::string Message;
		};
		std::vector<Refusal> const refusals = {
			{Json::object({{"op", "SetTimeline"}, {"node", 3}, {"timeline", timeline}}), "operations[0]",
		     "operations[0] (SetTimeline): node 3 is no Flow.Timeline: its type is Flow.Delay"},
			{Json::object({{"op", "SetTimeline"}, {"node", 4}, {"timeline", Json::object({{"Length", 0}})}}), "operations[0].timeline",
		     "operations[0] (SetTimeline): timeline: Timeline.Length: expected a number greater than 0 and at most 3600"},
			{Json::object({{"op", "SetTimeline"}, {"node", 4}, {"timeline", timeline}, {"renames", Json::array({"Lift"})}}),
		     "operations[0].renames[0]", "operations[0] (SetTimeline): renames[0]: expected { \"old\", \"new\" }, both track names"},
			{Json::object({{"op", "SetTimeline"},
		                   {"node", 4},
		                   {"timeline", timeline},
		                   {"renames", Json::array({Json::object({{"old", "Drift"}, {"new", "Lift"}})})}}),
		     "operations[0]", "operations[0] (SetTimeline): the timeline of node 4 has no track Drift to rename"},
			{Json::object({{"op", "SetSetting"}, {"node", 4}, {"name", "Timeline"}, {"value", 3}}), "operations[0].value",
		     "operations[0] (SetSetting): value: Timeline: expected an object with Length, Loop and Tracks, not 3"},
		};
		for (Refusal const& refusal : refusals)
		{
			CAPTURE(refusal.Operation.dump());
			VisualScriptBatchError const error = ExpectRefused(document, Json::array({refusal.Operation}), fixture.Context);
			CHECK(error.Failure == VisualScriptBatchFailure::Invalid);
			CHECK(error.Kind == refusal.Operation["op"].get<std::string>());
			CHECK(error.Path == refusal.Path);
			CHECK(error.Message == refusal.Message);
		}
	}
}

TEST_CASE("VisualScriptBatch: the executor's own refusals name the operation, its kind and the field")
{
	BatchFixture fixture;
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(4, "Flow.Sequence", glm::vec2(0.0f, 320.0f), Json::object({{"Count", 3}})));
	struct Refusal
	{
		Json Operation;
		std::string Kind;
		std::string Path;
		// Empty: a point out of range, whose message names the bound.
		std::string Message;
	};
	std::vector<Refusal> const refusals = {
		{Json::object({{"op", "SetSequenceOutputs"}, {"node", 4}, {"count", 4}, {"insertAt", 0}, {"removeAt", 1}}), "SetSequenceOutputs",
	     "operations[0].removeAt", "operations[0] (SetSequenceOutputs): removeAt: SetSequenceOutputs takes insertAt or removeAt, not both"},
		{Json::object({{"op", "AddFunction"}, {"name", "Jump"}, {"as", "$jump"}, {"returnAs", "$jump"}}), "AddFunction", "operations[0]",
	     "operations[0] (AddFunction): as, entryAs and returnAs must be different labels"},
		{Json::object({{"op", "Align"}, {"elements", Json::array({2, 21})}, {"alignment", "Top"}}), "Align", "operations[0].elements[1]",
	     "operations[0] (Align): elements[1]: 21 is in another graph than 2: the elements must be of one graph"},
		{Json::object({{"op", "PromoteToFunctionInput"}, {"node", 2}, {"pin", "Message"}}), "PromoteToFunctionInput", "operations[0].node",
	     "operations[0] (PromoteToFunctionInput): node: node 2 is in the event graph: function inputs are added in a function's graph"},
		{Json::object({{"op", "Disconnect"}, {"node", 2}, {"from", Json::array({1, "Then"})}}), "Disconnect", "operations[0].node",
	     "operations[0] (Disconnect): node: Disconnect takes node alone, or from and/or to"},
		{Json::object({{"op", "Relink"}, {"remove", Json::array()}, {"add", Json::array()}}), "Relink", "operations[0]",
	     "operations[0] (Relink): Relink takes links to remove or to add"},
		{Json::object({{"op", "SetSetting"}, {"node", 4}, {"name", "Count"}, {"value", "four"}}), "SetSetting", "operations[0].value",
	     "operations[0] (SetSetting): value: the Count of a Sequence is an integer from 2 to 64"},
		{Json::object({{"op", "AddComment"}, {"text", "Far"}, {"position", Json::array({0, -2.0e7})}}), "AddComment",
	     "operations[0].position", ""},
		{Json::object({{"op", "SetPosition"}, {"element", 2}, {"position", Json::array({1.0e7 + 1.0, 0})}}), "SetPosition",
	     "operations[0].position", ""},
		{Json::object({{"op", "AddComment"}, {"text", "Bright"}, {"color", Json::array({1.5, 0, 0, 1})}}), "AddComment",
	     "operations[0].color", "operations[0] (AddComment): color: expected four numbers, linear [r, g, b, a] within 0 to 1"},
	};
	for (Refusal const& refusal : refusals)
	{
		CAPTURE(refusal.Operation.dump());
		VisualScriptBatchError const error = ExpectRefused(document, Json::array({refusal.Operation}), fixture.Context);
		CHECK(error.Failure == VisualScriptBatchFailure::Invalid);
		CHECK(error.Operation == 0);
		CHECK(error.Kind == refusal.Kind);
		CHECK(error.Path == refusal.Path);
		CHECK(error.Data.is_null());
		if (refusal.Message.empty())
		{
			// Points are finite, within the coordinates documents allow.
			CHECK(error.Message.starts_with(fmt::format("operations[0] ({}): position: expected finite numbers within +-", refusal.Kind)));
		}
		else
		{
			CHECK(error.Message == refusal.Message);
		}
	}
	// A refusal of an operation whose kind is unknown names no kind.
	CHECK(ExpectRefused(document, Json::array({Json::object({{"op", "Conect"}})}), fixture.Context).Kind.empty());
}

TEST_CASE("VisualScriptBatch: SetSetting reports a name or an asset that does not resolve at its value")
{
	BatchFixture fixture;
	fixture.Context.FindAsset = [](std::string_view) -> std::optional<AssetHandle>
	{
		return std::nullopt;
	};
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes.push_back(MakeNode(4, "Variable.Get", glm::vec2(0.0f, 320.0f), Json::object({{"Variable", 10}})));
	VisualScriptBatchError const name =
		ExpectRefused(document, Json::array({Json::object({{"op", "SetSetting"}, {"node", 4}, {"name", "Variable"}, {"value", "Sped"}})}),
	                  fixture.Context);
	CHECK(name.Path == "operations[0].value");
	CHECK(name.Message == "operations[0] (SetSetting): value: the visual script has no variable named Sped");
	CHECK(name.Data["names"][0] == "Speed");
	VisualScriptBatchError const asset = ExpectRefused(
		document,
		Json::array({Json::object({{"op", "SetSetting"}, {"node", 4}, {"name", "Graph"}, {"value", "asset://Scripts/Missing.sgraph"}})}),
		fixture.Context);
	CHECK(asset.Failure == VisualScriptBatchFailure::AssetNotFound);
	CHECK(asset.Path == "operations[0].value");
	CHECK(asset.Data["path"] == "operations[0].value");
	// AddNode's settings name the setting.
	VisualScriptBatchError const added = ExpectRefused(
		document,
		Json::array({Json::object({{"op", "AddNode"}, {"type", "Variable.Get"}, {"settings", Json::object({{"Variable", "Sped"}})}})}),
		fixture.Context);
	CHECK(added.Path == "operations[0].settings.Variable");
}

TEST_CASE("VisualScriptBatch: nodes added without a position are moved, aligned, framed and promoted once Arrange placed them")
{
	BatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	Json const add = Json::object({{"op", "AddNode"}, {"type", std::string(LogType)}, {"as", "$log"}});
	std::vector<std::pair<Json, std::string>> const refused = {
		{Json::object({{"op", "Move"}, {"elements", Json::array({"$log"})}, {"offset", Json::array({16, 0})}}), "elements"},
		{Json::object({{"op", "AddComment"}, {"text", "Log"}, {"elements", Json::array({"$log", 2})}}), "elements"},
		{Json::object({{"op", "Align"}, {"elements", Json::array({2, "$log"})}, {"alignment", "Top"}}), "elements"},
		{Json::object({{"op", "Distribute"}, {"elements", Json::array({1, 2, "$log"})}, {"axis", "Vertical"}}), "elements"},
		{Json::object({{"op", "Straighten"}, {"nodes", Json::array({1, "$log"})}}), "nodes"},
		{Json::object({{"op", "PromoteToVariable"}, {"node", "$log"}, {"pin", "Message"}, {"direction", "In"}}), "node"},
	};
	for (auto const& [operation, field] : refused)
	{
		CAPTURE(operation.dump());
		VisualScriptBatchError const error = ExpectRefused(document, Json::array({add, operation}), fixture.Context);
		CHECK(error.Path == "operations[1]." + field);
		CHECK(error.Message.find(" has no position yet: Arrange the nodes added without one (scope Created) or give it a position") !=
		      std::string::npos);
	}
	// After an Arrange, each applies.
	Json operations = Json::array({add, Json::object({{"op", "Arrange"}, {"scope", "Created"}})});
	for (auto const& [operation, field] : refused)
	{
		operations.push_back(operation);
	}
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	CHECK(FindNode(result.Document, GetLabel(result, "$log"))->Position.has_value());
	// A position given is enough too.
	Json placed = Json::array({add, refused[0].first});
	placed[0]["position"] = Json::array({0, 480});
	VisualScriptBatchResult const moved = ExpectApplied(document, placed, fixture.Context);
	CHECK(FindNode(moved.Document, GetLabel(moved, "$log"))->Position == glm::vec2(16.0f, 480.0f));
}

TEST_CASE("VisualScriptBatch: a rename analyzes again before measured layout, which sizes the renamed nodes by their new titles")
{
	BatchFixture fixture;
	VisualScriptDocument document = MakeDocument();
	// A call of Push, then a Log right of it, linked.
	document.Graph.Nodes.push_back(MakeNode(4, "Function.Call", glm::vec2(0.0f, 320.0f), Json::object({{"Function", 20}})));
	document.Graph.Nodes.push_back(MakeNode(5, LogType, glm::vec2(192.0f, 320.0f)));
	document.Graph.Links.push_back(MakeLink(4, "Then", 5, "Execute"));
	Json const arrange = Json::object({{"op", "Arrange"}});
	VisualScriptDocument const arranged = ExpectApplied(document, Json::array({arrange}), fixture.Context).Document;
	auto const cache = [&fixture](VisualScriptDocument const& stored)
	{
		Result<Ref<VisualScriptAnalysis const>> analysis = fixture.Semantics.Analyze(stored, AssetHandle());
		REQUIRE(analysis.IsOk());
		fixture.Context.StoredAnalysis = analysis.TakeValue();
		fixture.Context.StoredAnalysisDocument = CreateRef<VisualScriptDocument const>(stored);
	};

	// The stored document's analysis is at hand (graph.edit's cache): no operation before the rename analyzed.
	cache(arranged);
	std::string const longName = "ThisIsAVeryLongFunctionNameThatMakesTheCallNodeWide";
	auto const rename = [&longName](std::string_view op, std::string_view field, Json target)
	{
		return Json::object({{"op", std::string(op)}, {std::string(field), std::move(target)}, {"name", longName}});
	};
	VisualScriptBatchResult const renamed =
		ExpectApplied(arranged, Json::array({rename("RenameFunction", "function", 20), arrange}), fixture.Context);
	CHECK(renamed.Analyses == 1);
	// Arranging the result again, measured from a fresh analysis, moves nothing: the batch sized the call by its new title.
	fixture.Context.StoredAnalysis = nullptr;
	fixture.Context.StoredAnalysisDocument = nullptr;
	VisualScriptBatchResult const again = ExpectApplied(renamed.Document, Json::array({arrange}), fixture.Context);
	CHECK(again.Document == renamed.Document);
	CHECK(FindNode(renamed.Document, 5)->Position->x > FindNode(arranged, 5)->Position->x);

	// A variable's or a local's new name analyzes again before measured layout too; the same name does not.
	cache(arranged);
	CHECK(ExpectApplied(arranged, Json::array({rename("SetVariable", "variable", 10), arrange}), fixture.Context).Analyses == 1);
	Json local = rename("SetLocal", "local", 23);
	local["function"] = 20;
	CHECK(ExpectApplied(arranged, Json::array({local, arrange}), fixture.Context).Analyses == 1);
	Json const same = Json::object({{"op", "SetVariable"}, {"variable", 10}, {"name", "Speed"}});
	CHECK(ExpectApplied(arranged, Json::array({same, arrange}), fixture.Context).Analyses == 0);
}

TEST_CASE("VisualScriptBatch: an unknown name of a megabyte is refused with the closest names, compared by its start only")
{
	BatchFixture fixture;
	VisualScriptDocument document = MakeDocument();
	for (uint32_t index = 0; index < 1000; index++)
	{
		VisualScriptVariable variable;
		variable.ID = 1000 + index;
		variable.Name = fmt::format("Variable{}", index);
		variable.Type = "int";
		document.Variables.push_back(std::move(variable));
	}
	// Compared in full against a thousand names, a megabyte would take hours; it is refused at once.
	std::string const name = "Speed" + std::string(size_t{1} << 20, 'x');
	std::vector<Json> const operations = {
		Json::object({{"op", "SetVariable"}, {"variable", name}}),
		Json::object({{"op", "AddNode"}, {"type", "Variable.Get"}, {"settings", Json::object({{"Variable", name}})}}),
	};
	for (Json const& operation : operations)
	{
		VisualScriptBatchError const error = ExpectRefused(document, Json::array({operation}), fixture.Context);
		REQUIRE(error.Data.contains("names"));
		CHECK(error.Data["names"].size() == 3);
	}
}

TEST_CASE("VisualScriptBatch: the links one list lacks are found in time linear in both, at the format's link limit")
{
	std::vector<VisualScriptLink> before;
	for (uint32_t index = 0; index < static_cast<uint32_t>(VisualScriptSerializer::MaxLinksPerGraph); index++)
	{
		before.push_back(MakeLink(index + 1, "Result", index + 2, "A"));
	}
	std::vector<VisualScriptLink> after(before.begin() + 1, before.end());
	after.erase(after.begin() + 20000);
	after.push_back(MakeLink(1, "Result", 3, "B"));
	std::vector<VisualScriptLink> const gone = FindVisualScriptLinksGone(before, after);
	REQUIRE(gone.size() == 2);
	CHECK(gone[0] == before[0]);
	CHECK(gone[1] == before[20001]);
	CHECK(FindVisualScriptLinksGone(after, before) == std::vector<VisualScriptLink>{MakeLink(1, "Result", 3, "B")});
	CHECK(FindVisualScriptLinksGone(before, before).empty());
	// A link is told apart by each of its four parts.
	std::vector<VisualScriptLink> const link = {MakeLink(1, "Then", 2, "Execute")};
	std::vector<VisualScriptLink> const others = {MakeLink(9, "Then", 2, "Execute"), MakeLink(1, "Else", 2, "Execute"),
	                                              MakeLink(1, "Then", 9, "Execute"), MakeLink(1, "Then", 2, "Other")};
	CHECK(FindVisualScriptLinksGone(link, others) == link);
}

namespace
{
	// 2,000 additions in rows of 50, each feeding the next one; the variable Speed (5000); 64 functions, each with an entry and
	// a Get of Speed.
	VisualScriptDocument MakeChainDocument()
	{
		VisualScriptDocument document;
		for (uint32_t index = 0; index < 2000; index++)
		{
			glm::vec2 const position(static_cast<float>(index % 50) * 320.0f, static_cast<float>(index / 50) * 160.0f);
			document.Graph.Nodes.push_back(MakeNode(index + 1, "Operator.Add", position, Json::object(), Json::object({{"B", 1.5}})));
			if (index > 0)
			{
				document.Graph.Links.push_back(MakeLink(index, "Result", index + 1, "A"));
			}
		}
		VisualScriptVariable speed;
		speed.ID = 5000;
		speed.Name = "Speed";
		speed.Type = "float";
		document.Variables.push_back(speed);
		for (uint32_t index = 0; index < 64; index++)
		{
			VisualScriptFunction function;
			function.ID = 6000 + index * 4;
			function.Name = fmt::format("Function{}", index);
			function.Graph.Nodes.push_back(MakeNode(function.ID + 1, "Function.Entry", glm::vec2(0.0f)));
			function.Graph.Nodes.push_back(
				MakeNode(function.ID + 2, "Variable.Get", glm::vec2(320.0f, 0.0f), Json::object({{"Variable", 5000}})));
			document.Functions.push_back(std::move(function));
		}
		return document;
	}

	// Connections of the additions' results into the B inputs two further along the chain, from the first addition on.
	Json MakeChainConnections(uint32_t count)
	{
		Json operations = Json::array();
		for (uint32_t index = 1; index <= count; index++)
		{
			operations.push_back(
				Json::object({{"op", "Connect"}, {"from", Json::array({index, "Result"})}, {"to", Json::array({index + 2, "B"})}}));
		}
		return operations;
	}
}

TEST_CASE("VisualScriptBatch: canvases share one copy of the document per change, whatever the number of sub-graphs")
{
	// Connects read rows, which links do not change: the event graph's canvas lays out once, from the copy the analysis took
	// (spec-slice4 §9.3). Deleting a variable with its nodes then reads the rows of every sub-graph: the functions' canvases
	// share one copy of the document as the Connects left it (a copy per sub-graph would make 65), and the event graph's
	// canvas still serves.
	BatchFixture fixture;
	VisualScriptDocument const document = MakeChainDocument();
	constexpr uint32_t Connects = 100;
	Json operations = MakeChainConnections(Connects);
	operations.push_back(Json::object({{"op", "DeleteVariable"}, {"variable", "Speed"}}));
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	CHECK(result.Analyses == 1);
	CHECK(result.Displays == 2);
	CHECK(result.CanvasSyncs == 1 + 64);
	CHECK(result.Results.back() == Json::object({{"removedNodes", 64}}));
	CHECK(result.Document.Graph.Links.size() == document.Graph.Links.size() + Connects);
}

TEST_CASE("VisualScriptBatch: a thousand connections on a graph of 2,000 nodes lay its canvas out once, in their time on request")
{
	// Spec-slice4 §9.3 and §21: a Connect reads the rows of its sub-graph's canvas, which a link changes for no node the analysis
	// describes, so a thousand of them after one analysis take one copy of the document and one layout. The counts hold
	// always; the time is asserted with STRADA_PERFORMANCE_TESTS=1.
	BatchFixture fixture;
	VisualScriptDocument const document = MakeChainDocument();
	constexpr uint32_t Connects = 1000;
	Json const operations = MakeChainConnections(Connects);
	auto const start = std::chrono::steady_clock::now();
	std::variant<VisualScriptBatchResult, VisualScriptBatchError> const applied =
		ApplyVisualScriptBatch(document, operations, fixture.Context);
	double const milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	VisualScriptBatchResult const* const result = std::get_if<VisualScriptBatchResult>(&applied);
	REQUIRE(result != nullptr);
	CHECK(result->Analyses == 1);
	CHECK(result->Displays == 1);
	CHECK(result->CanvasSyncs <= 2);
	CHECK(result->Document.Graph.Links.size() == document.Graph.Links.size() + Connects);
	if (!ArePerformanceTestsEnabled())
	{
		return;
	}
	MESSAGE("1,000 connections on 2,000 nodes and 1,999 links: " << milliseconds << " ms, " << milliseconds / Connects
	                                                             << " ms per connection");
	// Three times what track I measured (an AMD Ryzen 5 7535HS laptop, Windows 11, MSVC 19.51); track G's figures before the
	// Rows canvases were 1,974 ms (Release) and 91,400 ms (Debug).
#if defined(ST_DEBUG)
	constexpr double MeasuredMilliseconds = 6288.6;
#else
	constexpr double MeasuredMilliseconds = 108.7;
#endif
	CHECK(milliseconds <= 3.0 * MeasuredMilliseconds);
}

namespace
{
	// A random operation of those that read rows, change rows, change geometry or only move, on the document as the batch
	// found it (IDs of its event graph's nodes, its links), now and then naming what is not there.
	Json MakeRowsOperation(Testing::FuzzRandom& random, VisualScriptDocument const& document, VisualScriptLibrary const& library)
	{
		std::vector<VisualScriptNode> const& nodes = document.Graph.Nodes;
		auto const node = [&]() -> Json
		{
			return nodes.empty() || random.Pick(12) == 0 ? Json(999999) : Json(nodes[random.Pick(nodes.size())].ID);
		};
		// A pin of a node as the library's template of its type names it; a name it may not have now and then.
		auto const pin = [&](Json const& id, VisualScriptPinDirection direction) -> std::string
		{
			std::vector<std::string> names;
			for (VisualScriptNode const& candidate : nodes)
			{
				if (!id.is_number_unsigned() || candidate.ID != id.get<uint32_t>())
				{
					continue;
				}
				if (VisualScriptNodeTemplate const* const kind = library.FindNode(candidate.Type))
				{
					for (VisualScriptPin const& entry : kind->Pins)
					{
						if (entry.Direction == direction)
						{
							names.push_back(entry.Name);
						}
					}
				}
			}
			if (names.empty() || random.Pick(10) == 0)
			{
				names = direction == VisualScriptPinDirection::In ? std::vector<std::string>{"Execute", "A", "B", "Message"}
				                                                  : std::vector<std::string>{"Then", "Result", "Completed"};
			}
			return names[random.Pick(names.size())];
		};
		auto const link = [&]() -> Json
		{
			std::vector<VisualScriptLink> const& links = document.Graph.Links;
			if (links.empty() || random.Pick(8) == 0)
			{
				return Json::object({{"from", Json::array({node(), "Then"})}, {"to", Json::array({node(), "Execute"})}});
			}
			VisualScriptLink const& chosen = links[random.Pick(links.size())];
			return Json::object(
				{{"from", Json::array({chosen.From.Node, chosen.From.Pin})}, {"to", Json::array({chosen.To.Node, chosen.To.Pin})}});
		};
		auto const point = [&]()
		{
			return Json::array({static_cast<double>(random.Pick(1600)) - 800.0, static_cast<double>(random.Pick(1600)) - 800.0});
		};
		switch (random.Pick(14))
		{
			case 0:
			case 1:
			case 2:
			{
				Json const from = node();
				Json const to = node();
				return Json::object({{"op", "Connect"},
				                     {"from", Json::array({from, pin(from, VisualScriptPinDirection::Out)})},
				                     {"to", Json::array({to, pin(to, VisualScriptPinDirection::In)})}});
			}
			case 3:
				return Json::object({{"op", "Disconnect"}, {"from", link()["from"]}});
			case 4:
			{
				Json const target = node();
				return Json::object({{"op", "SetInput"},
				                     {"node", target},
				                     {"pin", pin(target, VisualScriptPinDirection::In)},
				                     {"value", random.Pick(2) == 0 ? Json(1.5) : Json("Rows")}});
			}
			case 5:
				return Json::object({{"op", "Move"}, {"elements", Json::array({node()})}, {"offset", point()}});
			case 6:
			{
				std::array<std::string_view, 5> const types = {LogType, "Operator.Add", "Flow.Branch", "Flow.Brnach", "Flow.Sequence"};
				return Json::object({{"op", "AddNode"}, {"type", std::string(types[random.Pick(types.size())])}, {"position", point()}});
			}
			case 7:
				return Json::object({{"op", "Delete"}, {"elements", Json::array({node()})}});
			case 8:
			{
				Json const from = node();
				Json const to = node();
				return Json::object(
					{{"op", "Relink"},
				     {"remove", Json::array({link()})},
				     {"add", Json::array({Json::object({{"from", Json::array({from, pin(from, VisualScriptPinDirection::Out)})},
				                                        {"to", Json::array({to, pin(to, VisualScriptPinDirection::In)})}})})}});
			}
			case 9:
				return Json::object({{"op", "Splice"}, {"node", node()}, {"link", link()}});
			case 10:
				return Json::object({{"op", "SetCollapsed"}, {"nodes", Json::array({node()})}, {"collapsed", random.Pick(2) == 0}});
			case 11:
				return Json::object({{"op", "SetSequenceOutputs"}, {"node", node()}, {"count", 2 + random.Pick(3)}});
			case 12:
				return Json::object({{"op", "Arrange"}, {"extents", random.Pick(2) == 0 ? "Measured" : "Uniform"}});
			default:
				return Json::object({{"op", "SplitPin"}, {"node", node()}, {"pin", "Position"}, {"direction", "In"}});
		}
	}

	// A document whose event graph mixes what the analysis describes with what it cannot (a misspelled type, whose rows come
	// from its links and literals): MakeDocument's, an addition chain, a Sequence and an unknown node linked into the chain.
	VisualScriptDocument MakeRowsDocument()
	{
		VisualScriptDocument document = MakeDocument();
		for (uint32_t index = 0; index < 6; index++)
		{
			document.Graph.Nodes.push_back(MakeNode(100 + index, "Operator.Add", glm::vec2(static_cast<float>(index) * 320.0f, 400.0f),
			                                        Json::object(), Json::object({{"B", 1.5}})));
			if (index > 0)
			{
				document.Graph.Links.push_back(MakeLink(99 + index, "Result", 100 + index, "A"));
			}
		}
		document.Graph.Nodes.push_back(MakeNode(120, "Flow.Sequence", glm::vec2(0.0f, 800.0f), Json::object({{"Count", 2}})));
		document.Graph.Nodes.push_back(
			MakeNode(121, "Flow.Brnach", glm::vec2(320.0f, 800.0f), Json::object(), Json::object({{"Strength", 2}})));
		document.Graph.Links.push_back(MakeLink(120, "Then0", 121, "Execute"));
		document.Graph.Links.push_back(MakeLink(121, "Out", 102, "B"));
		return document;
	}
}

TEST_CASE("VisualScriptBatch: the rows operations read equal a freshly synced canvas's after every operation, anchors aside")
{
	// Spec-slice4 §9.3's property: a Rows canvas syncs again only when rows may have changed, so what every operation that
	// reads rows takes equals what a canvas synced with the working document now gives; every anchor it gives is NaN.
	BatchFixture fixture;
	Ref<VisualScriptLibrary const> const library = fixture.Semantics.GetLibrary();
	size_t inspected = 0;
	size_t mismatches = 0;
	std::string first;
	fixture.Context.InspectRows = [&](VisualScriptBatchRows const& rows)
	{
		inspected++;
		VisualScriptHeadlessCanvas fresh;
		fresh.Sync(rows.Document, rows.Function, rows.Analysis, rows.AnalyzedDocument != nullptr ? rows.AnalyzedDocument : &rows.Document,
		           library.get(), {});
		VisualScriptNodePins const expected = fresh.GetPins();
		VisualScriptGraph const* graph = &rows.Document.Graph;
		for (VisualScriptFunction const& function : rows.Document.Functions)
		{
			if (function.ID == rows.Function)
			{
				graph = &function.Graph;
			}
		}
		for (VisualScriptNode const& node : graph->Nodes)
		{
			std::vector<VisualScriptNodePin> taken = rows.Rows(node.ID);
			std::vector<VisualScriptNodePin> synced = expected(node.ID);
			bool anchored = false;
			for (VisualScriptNodePin& row : taken)
			{
				anchored = anchored || !std::isnan(row.Anchor.x) || !std::isnan(row.Anchor.y);
				row.Anchor = glm::vec2(0.0f);
			}
			for (VisualScriptNodePin& row : synced)
			{
				row.Anchor = glm::vec2(0.0f);
			}
			if (anchored || taken != synced)
			{
				if (mismatches++ == 0)
				{
					first = fmt::format("node {} ({}) in sub-graph {}: {} rows taken, {} synced{}", node.ID, node.Type, rows.Function,
					                    taken.size(), synced.size(), anchored ? ", with an anchor" : "");
				}
			}
		}
	};
	VisualScriptDocument const document = MakeRowsDocument();
	Testing::FuzzRandom random(11250);
	size_t applied = 0;
	int const rounds = Testing::GetFuzzRounds(60);
	for (int round = 0; round < rounds; round++)
	{
		CAPTURE(round);
		Json operations = Json::array();
		for (size_t count = 2 + random.Pick(8); count > 0; count--)
		{
			operations.push_back(MakeRowsOperation(random, document, *library));
		}
		CAPTURE(operations);
		std::variant<VisualScriptBatchResult, VisualScriptBatchError> const result =
			ApplyVisualScriptBatch(document, operations, fixture.Context);
		applied += std::holds_alternative<VisualScriptBatchResult>(result) ? 1 : 0;
		REQUIRE_MESSAGE(mismatches == 0, first);
	}
	CHECK(applied > 0);
	CHECK(inspected > static_cast<size_t>(rounds) / 2);
}

TEST_CASE("VisualScriptBatch: operations that read rows see no anchors, while Straighten and Promote to Variable read real ones")
{
	BatchFixture fixture;
	std::vector<bool> anchored;
	fixture.Context.InspectRows = [&anchored](VisualScriptBatchRows const& rows)
	{
		for (VisualScriptNodePin const& row : rows.Rows(1))
		{
			anchored.push_back(!std::isnan(row.Anchor.x) && !std::isnan(row.Anchor.y));
		}
	};
	VisualScriptDocument document = MakeDocument();
	document.Graph.Nodes[2].Position = glm::vec2(640.0f, 75.0f);
	VisualScriptBatchResult const result = ExpectApplied(
		document,
		Json::array({Json::object({{"op", "Connect"}, {"from", Json::array({1, "Then"})}, {"to", Json::array({3, "Execute"})}}),
	                 Json::object({{"op", "Straighten"}, {"nodes", Json::array({1, 3})}})}),
		fixture.Context);
	REQUIRE_FALSE(anchored.empty());
	CHECK(std::none_of(anchored.begin(), anchored.end(),
	                   [](bool value)
	                   {
						   return value;
					   }));
	// Straighten lines the flow up by the pins' anchors: the Delay moved level with the Update's Then.
	CHECK(FindNode(result.Document, 3)->Position->y != 75.0f);
}

namespace
{
	AssetHandle const BatchDoor(UUID(7500001ull));
	AssetHandle const BatchMotion(UUID(7500002ull));
	AssetHandle const BatchSelf(UUID(7500003ull));

	// Door: the public Health (5) and Armor (9), and the custom event Hit (Damage: float); Motion: the library function Clamp01
	// (7, Height: float).
	struct CrossGraphBatchFixture : BatchFixture
	{
		CrossGraphBatchFixture()
		{
			VisualScriptVariable health;
			health.ID = 5;
			health.Name = "Health";
			health.Type = "float";
			health.Public = true;
			VisualScriptVariable armor = health;
			armor.ID = 9;
			armor.Name = "Armor";
			armor.Type = "int";
			Door.Variables = {health, armor};
			VisualScriptNode hit;
			hit.ID = 1;
			hit.Type = "Event.Custom";
			hit.Position = glm::vec2(0.0f);
			hit.Settings = Json::object({{"Name", "Hit"}, {"Parameters", Damage}});
			VisualScriptNode open = hit;
			open.ID = 2;
			open.Settings = Json::object({{"Name", "Open"}});
			Door.Graph.Nodes = {hit, open};
			Motion = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
			VisualScriptFunction clamp;
			clamp.ID = 7;
			clamp.Name = "Clamp01";
			clamp.Inputs = {{"Height", "float"}};
			Motion.Functions = {clamp};
			Context.Graph = BatchSelf;
			Context.FindDocument = [this](AssetHandle graph) -> VisualScriptDocument const*
			{
				return graph == BatchDoor ? &Door : graph == BatchMotion ? &Motion : nullptr;
			};
			Context.FindGraphName = [](AssetHandle graph) -> std::string
			{
				return graph == BatchDoor ? "Door" : graph == BatchMotion ? "Motion" : std::string();
			};
		}

		Json const Damage = Json::array({{{"Name", "Damage"}, {"Type", "float"}}});
		VisualScriptDocument Door;
		VisualScriptDocument Motion;
	};
}

TEST_CASE("VisualScriptBatch: AddNode of a typed Send or Broadcast without Parameters takes its event's, and refuses an unknown Name")
{
	CrossGraphBatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	std::string const door = BatchDoor.ToString();
	Json const operations = Json::array(
		{Json::object(
			 {{"op", "AddNode"}, {"type", "Event.Send"}, {"settings", Json::object({{"Name", "Hit"}, {"Graph", door}})}, {"as", "$send"}}),
	     Json::object({{"op", "AddNode"},
	                   {"type", "Event.Broadcast"},
	                   {"settings", Json::object({{"Name", "Hit"}, {"Graph", door}, {"Parameters", Json::array()}})},
	                   {"as", "$broadcast"}})});
	VisualScriptBatchResult const result = ExpectApplied(document, operations, fixture.Context);
	VisualScriptNode const* const send = FindNode(result.Document, GetLabel(result, "$send"));
	REQUIRE(send != nullptr);
	CHECK(send->Settings == Json::object({{"Name", "Hit"}, {"Graph", door}, {"Parameters", fixture.Damage}}));
	// Parameters given stay as given (the analysis says when they differ).
	CHECK(FindNode(result.Document, GetLabel(result, "$broadcast"))->Settings["Parameters"] == Json::array());

	VisualScriptBatchError const unknown = ExpectRefused(
		document,
		Json::array(
			{Json::object({{"op", "AddNode"}, {"type", "Event.Send"}, {"settings", Json::object({{"Name", "Hti"}, {"Graph", door}})}})}),
		fixture.Context);
	CHECK(unknown.Path == "operations[0].settings.Name");
	CHECK(unknown.Data["names"] == Json::array({"Hit", "Open"}));
	// An untyped Send, and a graph not at hand, take nothing.
	VisualScriptBatchResult const untyped = ExpectApplied(
		document,
		Json::array({Json::object({{"op", "AddNode"}, {"type", "Event.Send"}, {"settings", Json::object({{"Name", "Hti"}})}, {"as", "$a"}}),
	                 Json::object({{"op", "AddNode"},
	                               {"type", "Event.Send"},
	                               {"settings", Json::object({{"Name", "Hti"}, {"Graph", "77"}})},
	                               {"as", "$b"}})}),
		fixture.Context);
	CHECK_FALSE(FindNode(untyped.Document, GetLabel(untyped, "$a"))->Settings.contains("Parameters"));
	CHECK_FALSE(FindNode(untyped.Document, GetLabel(untyped, "$b"))->Settings.contains("Parameters"));
}

TEST_CASE("VisualScriptBatch: Retarget and SetSetting's routes give a node what another graph declares, keeping links by pin name")
{
	CrossGraphBatchFixture fixture;
	std::string const door = BatchDoor.ToString();
	std::string const motion = BatchMotion.ToString();
	VisualScriptDocument document = MakeDocument();
	VisualScriptNode get;
	get.ID = 40;
	get.Type = "Variable.Get";
	get.Position = glm::vec2(0.0f, 640.0f);
	get.Settings = Json::object({{"Variable", 5}, {"Graph", door}});
	VisualScriptNode call = get;
	call.ID = 41;
	call.Type = "Function.Call";
	call.Settings = Json::object({{"Function", 7}, {"Graph", motion}});
	VisualScriptNode send = get;
	send.ID = 42;
	send.Type = "Event.Send";
	send.Settings = Json::object({{"Name", "Open"}});
	document.Graph.Nodes.insert(document.Graph.Nodes.end(), {get, call, send});
	document.Graph.Links.push_back(MakeLink(40, "Value", 41, "Height"));

	// A variable named in the other graph by its name; the link from Value stays.
	VisualScriptBatchResult const renamed = ExpectApplied(
		document, Json::array({Json::object({{"op", "Retarget"}, {"node", 40}, {"settings", Json::object({{"Variable", "Armor"}})}})}),
		fixture.Context);
	CHECK(FindNode(renamed.Document, 40)->Settings == Json::object({{"Variable", 9}, {"Graph", door}}));
	CHECK(renamed.Results[0] == Json::object({{"removedLinks", 0}}));
	CHECK(HasLink(renamed.Document.Graph, MakeLink(40, "Value", 41, "Height")));
	VisualScriptBatchError const missing = ExpectRefused(
		document, Json::array({Json::object({{"op", "Retarget"}, {"node", 40}, {"settings", Json::object({{"Variable", "Armour"}})}})}),
		fixture.Context);
	CHECK(missing.Path == "operations[0].settings.Variable");
	CHECK(missing.Data["names"][0] == "Armor");

	// SetSetting of Graph routes to Retarget: a Send naming a graph not at hand takes no parameters, and the call goes local.
	VisualScriptBatchResult const routed =
		ExpectApplied(document,
	                  Json::array({Json::object({{"op", "SetSetting"}, {"node", 42}, {"name", "Graph"}, {"value", "77"}}),
	                               Json::object({{"op", "SetSetting"}, {"node", 41}, {"name", "Graph"}, {"value", nullptr}})}),
	                  fixture.Context);
	CHECK(FindNode(routed.Document, 42)->Settings == Json::object({{"Name", "Open"}, {"Graph", "77"}}));
	CHECK(FindNode(routed.Document, 41)->Settings == Json::object({{"Function", 7}}));
	// The call names the document's function 7, which it lacks: no pins, so the link went.
	CHECK(routed.Results[1] == Json::object({{"removedLinks", 1}}));
	VisualScriptBatchResult const typed = ExpectApplied(
		document, Json::array({Json::object({{"op", "SetSetting"}, {"node", 42}, {"name", "Graph"}, {"value", door}})}), fixture.Context);
	CHECK(FindNode(typed.Document, 42)->Settings == Json::object({{"Name", "Open"}, {"Graph", door}, {"Parameters", Json::array()}}));
	// Its Name, once typed, routes too, and takes the new event's parameters only when it has none.
	VisualScriptBatchResult const named =
		ExpectApplied(typed.Document, Json::array({Json::object({{"op", "SetSetting"}, {"node", 42}, {"name", "Name"}, {"value", "Hit"}})}),
	                  fixture.Context);
	CHECK(FindNode(named.Document, 42)->Settings == Json::object({{"Name", "Hit"}, {"Graph", door}, {"Parameters", Json::array()}}));
	// A node of no cross-graph kind is refused.
	VisualScriptBatchError const branch = ExpectRefused(
		document, Json::array({Json::object({{"op", "Retarget"}, {"node", 2}, {"settings", Json::object({{"Graph", door}})}})}),
		fixture.Context);
	CHECK(branch.Message.find("names no other graph") != std::string::npos);
}

TEST_CASE("VisualScriptBatch: SetKind makes the script a library and back")
{
	CrossGraphBatchFixture fixture;
	VisualScriptDocument const document = MakeDocument();
	VisualScriptBatchResult const library =
		ExpectApplied(document, Json::array({Json::object({{"op", "SetKind"}, {"kind", "Library"}})}), fixture.Context);
	CHECK(library.Document.Kind == VisualScriptDocumentKind::Library);
	// Nothing else changes: its variables and event graph show as errors until removed.
	VisualScriptDocument expected = document;
	expected.Kind = VisualScriptDocumentKind::Library;
	CHECK(library.Document == expected);
	CHECK(ExpectApplied(library.Document, Json::array({Json::object({{"op", "SetKind"}, {"kind", "Script"}})}), fixture.Context).Document ==
	      document);
	CHECK(ExpectRefused(document, Json::array({Json::object({{"op", "SetKind"}, {"kind", "Module"}})}), fixture.Context).Path ==
	      "operations[0].kind");
}

TEST_CASE("VisualScriptBatch: Fix applies the quick fixes of cross-graph findings, reading the other graphs")
{
	CrossGraphBatchFixture fixture;
	std::string const door = BatchDoor.ToString();
	VisualScriptDocument document = MakeDocument();
	VisualScriptNode send;
	send.ID = 40;
	send.Type = "Event.Send";
	send.Position = glm::vec2(0.0f, 640.0f);
	send.Settings = Json::object({{"Name", "Hti"}, {"Graph", door}});
	document.Graph.Nodes.push_back(send);
	VisualScriptDiagnostic unknown;
	unknown.Severity = VisualScriptSeverity::Warning;
	unknown.Code = "UnknownEvent";
	unknown.Node = 40;
	unknown.Graph = BatchDoor;
	unknown.Message = "Scripts/Door.sgraph has no custom event Hti";
	unknown.Suggestions = {"Hit"};
	fixture.Semantics.InjectDiagnostic(unknown);

	VisualScriptBatchResult const fixed =
		ExpectApplied(document, Json::array({Json::object({{"op", "Fix"}, {"code", "UnknownEvent"}, {"fix", "Use Hit"}, {"node", 40}})}),
	                  fixture.Context);
	CHECK(FindNode(fixed.Document, 40)->Settings == Json::object({{"Name", "Hit"}, {"Graph", door}, {"Parameters", fixture.Damage}}));
	// The fix operations of the diagnostic, as graph.edit lists them, read the other graph too.
	Result<Ref<VisualScriptAnalysis const>> const analysis = fixture.Semantics.Analyze(document, BatchSelf);
	REQUIRE(analysis.IsOk());
	VisualScriptQuickFixGraphs const graphs{fixture.Context.FindDocument, fixture.Context.FindGraphName};
	Json const operations = GetVisualScriptFixOperations(document, *analysis.GetValue(), analysis.GetValue()->Diagnostics.back(),
	                                                     fixture.Semantics.GetFixtureLibrary(), &graphs);
	REQUIRE(operations.size() == 1);
	CHECK(operations[0]["label"] == "Use Hit");
	CHECK(GetVisualScriptFixOperations(document, *analysis.GetValue(), analysis.GetValue()->Diagnostics.back(),
	                                   fixture.Semantics.GetFixtureLibrary())
	          .empty());
	// A fix that opens the other graph changes nothing here.
	unknown.Code = "PrivateVariable";
	VisualScriptNode get = send;
	get.ID = 41;
	get.Type = "Variable.Get";
	get.Settings = Json::object({{"Variable", 5}, {"Graph", door}});
	document.Graph.Nodes.push_back(get);
	unknown.Node = 41;
	fixture.Semantics.ClearInjectedDiagnostics();
	fixture.Semantics.InjectDiagnostic(unknown);
	VisualScriptBatchError const navigating = ExpectRefused(
		document, Json::array({Json::object({{"op", "Fix"}, {"code", "PrivateVariable"}, {"fix", "Open Health in Door"}, {"node", 41}})}),
		fixture.Context);
	CAPTURE(navigating.Message);
	CHECK(navigating.Message.find("a graph.edit of the graph it names") != std::string::npos);
}

TEST_CASE("VisualScriptBatch: Duplicate and Paste of an Event.Broadcast in a function graph keep it, with no note")
{
	// A Broadcast sends: it is no event, so neither operation leaves it out of a function graph (spec-slice3 §6.4.4), with
	// the runtime's library and without it.
	for (bool const available : {true, false})
	{
		CAPTURE(available);
		BatchFixture fixture;
		fixture.Semantics.SetAvailable(available);
		VisualScriptDocument document = MakeDocument();
		document.Functions[0].Graph.Nodes.push_back(
			MakeNode(24, "Event.Broadcast", glm::vec2(240.0f, 160.0f), Json::object({{"Name", "Hit"}})));
		VisualScriptLibrary const* const library = available ? fixture.Semantics.GetLibrary().get() : nullptr;
		std::string const clipboard = WriteVisualScriptClipboard(document, 20, std::vector<uint32_t>{24}, AssetHandle(), library);
		VisualScriptBatchResult const result = ExpectApplied(
			document,
			Json::array(
				{Json::object({{"op", "Duplicate"}, {"elements", Json::array({24})}, {"labels", Json::array({"$copy"})}}),
		         Json::object({{"op", "Paste"}, {"clipboard", clipboard}, {"function", "Push"}, {"labels", Json::array({"$pasted"})}})}),
			fixture.Context);
		REQUIRE(result.Results.size() == 2);
		for (size_t index = 0; index < result.Results.size(); index++)
		{
			CAPTURE(index);
			Json const& done = result.Results[index];
			CHECK(done["ids"].size() == 1);
			CHECK((!done.contains("notes") || done["notes"].empty()));
		}
		std::vector<VisualScriptNode> const& nodes = result.Document.Functions[0].Graph.Nodes;
		for (std::string_view const label : {"$copy", "$pasted"})
		{
			CAPTURE(label);
			uint32_t const id = GetLabel(result, label);
			auto const found = std::find_if(nodes.begin(), nodes.end(),
			                                [id](VisualScriptNode const& node)
			                                {
												return node.ID == id;
											});
			REQUIRE(found != nodes.end());
			CHECK(found->Type == "Event.Broadcast");
			CHECK(found->Settings["Name"] == "Hit");
		}
	}
}

TEST_CASE("VisualScriptBatch: without the runtime a measured Arrange sizes a remote Get by its target's variable")
{
	// Spec-slice4 §16.3: the headless canvas resolves other graphs as a window does, so a degraded canvas titles a Get of
	// Door's Health by Health (its width is that title's), never by the edited document's variable of that ID.
	CrossGraphBatchFixture fixture;
	fixture.Semantics.SetAvailable(false);
	VisualScriptDocument document;
	VisualScriptVariable own;
	own.ID = 5;
	own.Name = "AVariableOfThisGraphWithAMuchLongerName";
	own.Type = "float";
	document.Variables.push_back(own);
	document.Graph.Nodes.push_back(
		MakeNode(40, "Variable.Get", glm::vec2(0.0f), Json::object({{"Variable", 5}, {"Graph", BatchDoor.ToString()}})));
	document.Graph.Nodes.push_back(MakeNode(41, LogType, glm::vec2(0.0f, 400.0f)));
	document.Graph.Links.push_back(MakeLink(40, "Value", 41, "Message"));

	// What a window shows of the Get: its target's variable.
	VisualScriptGraphResolver const resolve = [&fixture, &document](AssetHandle graph) -> VisualScriptDocument const*
	{
		return graph == BatchSelf ? &document : fixture.Context.FindDocument(graph);
	};
	VisualScriptHeadlessCanvas canvas;
	canvas.Sync(document, 0, nullptr, nullptr, nullptr, resolve);
	VisualScriptCanvasModel const& model = canvas.GetCanvas();
	REQUIRE(model.FindNode(40));
	CHECK(model.GetText(model.GetNodes()[*model.FindNode(40)].Title) == "Health");

	VisualScriptBatchResult const result =
		ExpectApplied(document, Json::array({Json::object({{"op", "Arrange"}, {"extents", "Measured"}})}), fixture.Context);
	VisualScriptDocument expected = document;
	std::vector<uint32_t> const ids = {40, 41};
	REQUIRE(VisualScriptEditing::ArrangeNodes(expected, 0, ids, canvas.GetExtents()).IsOk());
	CHECK(*FindNode(result.Document, 41)->Position == *FindNode(expected, 41)->Position);
	CHECK(*FindNode(result.Document, 40)->Position == *FindNode(expected, 40)->Position);
}

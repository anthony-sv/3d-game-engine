#include "TestUtilities.h"
#include "VisualScriptTestDocuments.h"

#include "Editor/Commands/AssetCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/VisualScripting/VisualScriptCommit.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value = Json())
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Value = std::move(value);
		variable.Public = true;
		return variable;
	}

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = glm::vec2(static_cast<float>(id) * 16.0f, 0.0f);
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	VisualScriptAnalyzedNode MakeAnalyzedNode(uint32_t id, std::string type,
	                                          VisualScriptResolution resolved = VisualScriptResolution::Exact,
	                                          std::string canonicalType = {}, std::vector<VisualScriptPinRename> renames = {})
	{
		VisualScriptAnalyzedNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Resolved = resolved;
		node.CanonicalType = std::move(canonicalType);
		node.PinRenames = std::move(renames);
		return node;
	}

	VisualScriptNode const& FindNode(VisualScriptGraph const& graph, uint32_t id)
	{
		auto const node = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
		                               [id](VisualScriptNode const& candidate)
		                               {
										   return candidate.ID == id;
									   });
		REQUIRE(node != graph.Nodes.end());
		return *node;
	}

	// A function whose graph calls a member that was renamed, and an event graph with a member whose pin Force (split, with
	// a split field) became Impulse, a member resolved as its only overload whose pin Data became Input, and a node the
	// analysis describes as another type.
	VisualScriptDocument MakeOutdatedDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Speed", "float", 4.0));
		VisualScriptFunction function;
		function.ID = 2;
		function.Name = "Helper";
		function.Graph.Nodes.push_back(MakeNode(3, "Call:Old.Scale", Json::object(), Json::object({{"Factor", 2}})));
		document.Functions.push_back(std::move(function));

		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		nodes.push_back(MakeNode(10, "Event.Update"));
		VisualScriptNode push = MakeNode(11, "Call:Old.Push", Json::object(), Json::object({{"Force.X", 1.5}, {"Force.Offset.Y", 0.5}}));
		push.Split.Inputs = {"Force", "Force.Offset"};
		nodes.push_back(std::move(push));
		nodes.push_back(MakeNode(12, "Variable.Get", Json::object({{"Variable", 1}})));
		nodes.push_back(MakeNode(13, "Call:Old.Store"));
		nodes.push_back(MakeNode(14, "Call:Changed.Member"));
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		links.push_back(MakeLink(10, "Then", 11, "Execute"));
		links.push_back(MakeLink(12, "Value", 11, "Force.Z"));
		// Both end at Input once Data is renamed: the first stays.
		links.push_back(MakeLink(12, "Value", 13, "Input"));
		links.push_back(MakeLink(12, "Value", 13, "Data"));
		links.push_back(MakeLink(11, "Then", 13, "Execute"));
		return document;
	}

	VisualScriptAnalysis MakeOutdatedAnalysis()
	{
		VisualScriptAnalysis analysis;
		analysis.Nodes.push_back(MakeAnalyzedNode(3, "Call:Old.Scale", VisualScriptResolution::Redirect, "Call:New.Scale(int)"));
		analysis.Nodes.push_back(MakeAnalyzedNode(10, "Event.Update"));
		analysis.Nodes.push_back(MakeAnalyzedNode(11, "Call:Old.Push", VisualScriptResolution::Redirect, "Call:New.Push(Strada.Vector3)",
		                                          {{VisualScriptPinDirection::In, "Force", "Impulse"}}));
		analysis.Nodes.push_back(MakeAnalyzedNode(12, "Variable.Get"));
		analysis.Nodes.push_back(MakeAnalyzedNode(13, "Call:Old.Store", VisualScriptResolution::Unique, "Call:Old.Store(int)",
		                                          {{VisualScriptPinDirection::In, "Data", "Input"}}));
		// The node changed since this analysis: it is left as it is.
		analysis.Nodes.push_back(MakeAnalyzedNode(14, "Call:Other.Member", VisualScriptResolution::Redirect, "Call:Other.Member(int)"));
		return analysis;
	}

	std::string CommitError(VisualScriptDocument const& stored, VisualScriptDocument edited)
	{
		Result<VisualScriptCommit> const commit = PrepareVisualScriptCommit(stored, std::move(edited), nullptr);
		REQUIRE(commit.IsError());
		return commit.GetError();
	}

	// Additions in rows of 50, each feeding the next one, and the first half also the one after it: 1.5 links per node (2,000
	// nodes, 3,000 links).
	VisualScriptDocument MakeLargeDocument(uint32_t nodeCount)
	{
		size_t const linkCount = static_cast<size_t>(nodeCount) * 3 / 2;
		VisualScriptDocument document;
		document.Graph.Nodes.reserve(nodeCount);
		for (uint32_t index = 0; index < nodeCount; index++)
		{
			VisualScriptNode node;
			node.ID = index + 1;
			node.Type = "Operator.Add";
			node.Position = glm::vec2(static_cast<float>(index % 50) * 320.0f, static_cast<float>(index / 50) * 160.0f);
			node.Inputs = Json::object({{"B", 1.5}});
			document.Graph.Nodes.push_back(std::move(node));
		}
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		links.reserve(linkCount);
		for (uint32_t id = 1; id < nodeCount; id++)
		{
			links.push_back(MakeLink(id, "Result", id + 1, "A"));
		}
		for (uint32_t id = 1; links.size() < linkCount; id++)
		{
			links.push_back(MakeLink(id, "Result", id + 2, "B"));
		}
		return document;
	}

	// Wall-clock bounds are checked only when asked (STRADA_PERFORMANCE_TESTS=1): shared machines make them noise.
	bool ArePerformanceTestsEnabled()
	{
		std::optional<std::string> const enabled = Platform::ReadEnvironmentVariable("STRADA_PERFORMANCE_TESTS");
		return enabled.has_value() && *enabled == "1";
	}

	double Median(std::vector<double> values)
	{
		std::sort(values.begin(), values.end());
		return values[values.size() / 2];
	}

	// The median of one phase over commits.
	double MedianPhase(std::vector<VisualScriptCommitTimings> const& timings, double VisualScriptCommitTimings::* phase)
	{
		std::vector<double> values;
		for (VisualScriptCommitTimings const& commit : timings)
		{
			values.push_back(commit.*phase);
		}
		return Median(std::move(values));
	}
}

TEST_CASE("VisualScriptCommit: renamed variables keep their stored names as former names, never a name the document uses, at most 16")
{
	VisualScriptDocument stored;
	stored.Variables.push_back(MakeVariable(1, "Speed", "float"));
	stored.Variables.push_back(MakeVariable(2, "Score", "int"));
	stored.Variables.push_back(MakeVariable(3, "Health", "int"));
	stored.Variables.push_back(MakeVariable(4, "Old", "int"));

	VisualScriptDocument edited = stored;
	edited.Variables[0].Name = "Velocity";
	// Swapped: both names stay current names.
	edited.Variables[1].Name = "Health";
	edited.Variables[2].Name = "Score";
	// Variable 5 has variable 4's stored name as a former name already: it is not added twice.
	edited.Variables[3].Name = "New";
	edited.Variables.push_back(MakeVariable(5, "Other", "int"));
	edited.Variables[4].FormerNames = {"Old"};

	Result<VisualScriptCommit> const commit = PrepareVisualScriptCommit(stored, edited, nullptr);
	REQUIRE(commit.IsOk());
	CHECK(commit.GetValue().Warnings == std::vector<std::string>{"variable renamed: Speed → Velocity (Speed is kept in FormerNames)"});
	CHECK(commit.GetValue().RenameCount == 1);
	std::vector<VisualScriptVariable> const& variables = commit.GetValue().Document.Variables;
	CHECK(variables[0].FormerNames == std::vector<std::string>{"Speed"});
	CHECK(variables[1].FormerNames.empty());
	CHECK(variables[2].FormerNames.empty());
	CHECK(variables[3].FormerNames.empty());
	CHECK(variables[4].FormerNames == std::vector<std::string>{"Old"});
	Testing::ExpectStrictRoundTrip(commit.GetValue().Document);

	// Up to 16, the oldest dropped.
	VisualScriptDocument full = stored;
	full.Variables[0].Name = "Latest";
	for (int name = 0; name < 16; name++)
	{
		full.Variables[0].FormerNames.push_back("Former" + std::to_string(name));
	}
	Result<VisualScriptCommit> const capped = PrepareVisualScriptCommit(stored, full, nullptr);
	REQUIRE(capped.IsOk());
	std::vector<std::string> const& formerNames = capped.GetValue().Document.Variables[0].FormerNames;
	REQUIRE(formerNames.size() == VisualScriptSerializer::MaxFormerNames);
	CHECK(formerNames.front() == "Former1");
	CHECK(formerNames.back() == "Speed");

	// Without a stored document that loads, nothing was renamed.
	Result<VisualScriptCommit> const unstored = PrepareVisualScriptCommit(VisualScriptDocument(), edited, nullptr);
	REQUIRE(unstored.IsOk());
	CHECK(unstored.GetValue().Warnings.empty());
	CHECK(unstored.GetValue().Document == edited);
}

TEST_CASE("VisualScriptCommit: a variable added and named in one undo step gets no former name, since each commit of a step passes the "
          "document from before it")
{
	// Add Variable commits a variable under the name it is given (NewVar), then the name typed for it, under one merge key.
	VisualScriptDocument before;
	before.Variables.push_back(MakeVariable(1, "Speed", "float", 4.0));
	before.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
	VisualScriptDocument created = before;
	created.Variables.push_back(MakeVariable(2, "NewVar", "float"));
	Result<VisualScriptCommit> const creation = PrepareVisualScriptCommit(before, created, nullptr);
	REQUIRE(creation.IsOk());
	CHECK(creation.GetValue().Warnings.empty());
	VisualScriptDocument named = creation.GetValue().Document;
	named.Variables[1].Name = "Jump";
	Result<VisualScriptCommit> const naming = PrepareVisualScriptCommit(before, named, nullptr);
	REQUIRE(naming.IsOk());
	CHECK(naming.GetValue().Warnings.empty());
	CHECK(naming.GetValue().Document.Variables[1].FormerNames.empty());
	// Against the document the first commit wrote, the name it was added under would be kept, as a rename of its own keeps it.
	Result<VisualScriptCommit> const renaming = PrepareVisualScriptCommit(creation.GetValue().Document, named, nullptr);
	REQUIRE(renaming.IsOk());
	CHECK(renaming.GetValue().Document.Variables[1].FormerNames == std::vector<std::string>{"NewVar"});

	// Committed as a graph window commits the run: one step that writes the variable without a former name, and whose undo
	// gives the file its bytes back.
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	EditorContext context;
	EditorOperations operations(context);
	REQUIRE(operations.CreateProject(directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	Result<AssetHandle> const script = operations.CreateVisualScript("Scripts/Player.sgraph", VisualScriptTemplate::Empty);
	REQUIRE(script.IsOk());
	AssetHandle const graph = script.GetValue();
	REQUIRE(operations.SetVisualScriptDocument(graph, before).IsOk());
	Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
	REQUIRE(asset != nullptr);
	Ref<VisualScriptDocument const> const start = asset->GetSharedDocument();
	std::filesystem::path const file = AssetManager::GetAbsolutePath(graph);
	Result<Buffer> const startBytes = FileSystem::ReadBinaryFile(file);
	REQUIRE(startBytes.IsOk());
	CommandHistory const& history = context.GetHistory();
	size_t const steps = history.GetUndoCount();
	// "VSNM" and a counter.
	constexpr uint64_t Key = 0x56534E4D00000001ull;

	VisualScriptDocument adding = *start;
	adding.Variables.push_back(MakeVariable(2, "NewVar", "float"));
	Result<VisualScriptCommitRequest> add =
		MakeVisualScriptCommitRequest(*start, std::move(adding), nullptr, "Add Variable", "Player", Key);
	REQUIRE(add.IsOk());
	REQUIRE(operations.EditVisualScript(graph, add.GetValue().Document, add.GetValue().Description, Key, add.GetValue().Text).IsOk());
	VisualScriptDocument typed = *asset->GetSharedDocument();
	typed.Variables.back().Name = "Jump";
	Result<VisualScriptCommitRequest> name =
		MakeVisualScriptCommitRequest(*start, std::move(typed), nullptr, "Add Variable", "Player", Key);
	REQUIRE(name.IsOk());
	REQUIRE(operations.EditVisualScript(graph, name.GetValue().Document, name.GetValue().Description, Key, name.GetValue().Text).IsOk());
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(history.GetUndoDescription() == "Add Variable in Player");
	VisualScriptVariable const jump = asset->GetDocument().Variables.back();
	CHECK(jump.Name == "Jump");
	CHECK(jump.FormerNames.empty());
	Result<Buffer> const written = FileSystem::ReadBinaryFile(file);
	REQUIRE(written.IsOk());
	CHECK(std::string(written.GetValue().As<char>(), static_cast<size_t>(written.GetValue().GetSize())) == name.GetValue().Text);

	REQUIRE(operations.Undo().IsOk());
	CHECK(asset->GetSharedDocument().get() == start.get());
	Result<Buffer> const undone = FileSystem::ReadBinaryFile(file);
	REQUIRE(undone.IsOk());
	CHECK(std::string(undone.GetValue().As<char>(), static_cast<size_t>(undone.GetValue().GetSize())) ==
	      std::string(startBytes.GetValue().As<char>(), static_cast<size_t>(startBytes.GetValue().GetSize())));
}

TEST_CASE("VisualScriptCommit: nodes resolved inexactly take their canonical types, their pins' links, literals and split paths following")
{
	VisualScriptDocument const stored = MakeOutdatedDocument();
	VisualScriptDocument edited = stored;
	edited.Variables[0].Name = "Velocity";
	VisualScriptAnalysis const analysis = MakeOutdatedAnalysis();

	Result<VisualScriptCommit> const prepared = PrepareVisualScriptCommit(stored, edited, &analysis);
	REQUIRE(prepared.IsOk());
	VisualScriptCommit const& commit = prepared.GetValue();
	// The renames first, then the canonical rewrites, functions before the event graph.
	CHECK(commit.Warnings == std::vector<std::string>{"variable renamed: Speed → Velocity (Speed is kept in FormerNames)",
	                                                  "node updated: Call:Old.Scale → Call:New.Scale(int)",
	                                                  "node updated: Call:Old.Push → Call:New.Push(Strada.Vector3)",
	                                                  "node updated: Call:Old.Store → Call:Old.Store(int)"});
	CHECK(commit.RenameCount == 1);

	VisualScriptDocument const& document = commit.Document;
	CHECK(document.Variables[0].FormerNames == std::vector<std::string>{"Speed"});
	CHECK(FindNode(document.Functions[0].Graph, 3).Type == "Call:New.Scale(int)");
	CHECK(FindNode(document.Functions[0].Graph, 3).Inputs == Json::object({{"Factor", 2}}));
	VisualScriptNode const& push = FindNode(document.Graph, 11);
	CHECK(push.Type == "Call:New.Push(Strada.Vector3)");
	CHECK(push.Split.Inputs == std::vector<std::string>{"Impulse", "Impulse.Offset"});
	CHECK(push.Inputs == Json::object({{"Impulse.X", 1.5}, {"Impulse.Offset.Y", 0.5}}));
	CHECK(FindNode(document.Graph, 13).Type == "Call:Old.Store(int)");
	CHECK(FindNode(document.Graph, 14).Type == "Call:Changed.Member");
	CHECK(document.Graph.Links == std::vector<VisualScriptLink>{MakeLink(10, "Then", 11, "Execute"), MakeLink(12, "Value", 11, "Impulse.Z"),
	                                                            MakeLink(12, "Value", 13, "Input"), MakeLink(11, "Then", 13, "Execute")});
	Testing::ExpectStrictRoundTrip(document);
	Testing::ExpectSplitConsistency(document);

	// Without an analysis, node types are written as they are.
	Result<VisualScriptCommit> const unanalyzed = PrepareVisualScriptCommit(stored, edited, nullptr);
	REQUIRE(unanalyzed.IsOk());
	CHECK(unanalyzed.GetValue().Warnings.size() == 1);
	CHECK(unanalyzed.GetValue().Document.Graph == stored.Graph);
	CHECK(unanalyzed.GetValue().Document.Functions == stored.Functions);
}

TEST_CASE("VisualScriptCommit: a library keeps a parameter renamed in place as a former name; a script and a list whose length changed "
          "record nothing")
{
	VisualScriptDocument stored;
	stored.Kind = VisualScriptDocumentKind::Library;
	stored.Variables.push_back(MakeVariable(9, "Speed", "float"));
	VisualScriptFunction clamp;
	clamp.ID = 1;
	clamp.Name = "Clamp01";
	clamp.Pure = true;
	clamp.Inputs = {{"Value", "float"}, {"Low", "float"}};
	clamp.Outputs = {{"Result", "float"}};
	VisualScriptFunction mix;
	mix.ID = 2;
	mix.Name = "Mix";
	mix.Inputs = {{"A", "float"}, {"B", "float"}};
	stored.Functions = {clamp, mix};

	VisualScriptDocument edited = stored;
	edited.Variables[0].Name = "Pace";
	edited.Functions[0].Inputs[0].Name = "Amount";
	edited.Functions[0].Outputs[0].Name = "Clamped";
	// A list whose length changed: SetFunctionSignature removed the links of its renamed parameters too.
	edited.Functions[1].Inputs = {{"First", "float"}, {"B", "float"}, {"C", "float"}};
	// A function the stored document does not have records nothing.
	VisualScriptFunction added;
	added.ID = 3;
	added.Name = "Added";
	added.Inputs = {{"P", "int"}};
	edited.Functions.push_back(added);

	Result<VisualScriptCommit> const commit = PrepareVisualScriptCommit(stored, edited, nullptr);
	REQUIRE_MESSAGE(commit.IsOk(), commit.GetError());
	// The variables' renames, then the parameters', all counted as renames.
	CHECK(commit.GetValue().Warnings ==
	      std::vector<std::string>{"variable renamed: Speed → Pace (Speed is kept in FormerNames)",
	                               "parameter renamed: Clamp01's Value → Amount (Value is kept in FormerNames)",
	                               "parameter renamed: Clamp01's Result → Clamped (Result is kept in FormerNames)"});
	CHECK(commit.GetValue().RenameCount == 3);
	VisualScriptDocument const& renamed = commit.GetValue().Document;
	CHECK(renamed.Functions[0].Inputs == std::vector<VisualScriptParameter>{{"Amount", "float", {"Value"}}, {"Low", "float"}});
	CHECK(renamed.Functions[0].Outputs == std::vector<VisualScriptParameter>{{"Clamped", "float", {"Result"}}});
	CHECK(renamed.Functions[1].Inputs == edited.Functions[1].Inputs);
	CHECK(renamed.Functions[2].Inputs == edited.Functions[2].Inputs);
	Testing::ExpectStrictRoundTrip(renamed);

	// A script's functions are called from its own graphs only, which the rename relinks: nothing is recorded.
	VisualScriptDocument storedScript = stored;
	storedScript.Kind = VisualScriptDocumentKind::Script;
	VisualScriptDocument editedScript = edited;
	editedScript.Kind = VisualScriptDocumentKind::Script;
	Result<VisualScriptCommit> const script = PrepareVisualScriptCommit(storedScript, editedScript, nullptr);
	REQUIRE(script.IsOk());
	CHECK(script.GetValue().Warnings == std::vector<std::string>{"variable renamed: Speed → Pace (Speed is kept in FormerNames)"});
	CHECK(script.GetValue().Document.Functions == editedScript.Functions);

	// Renamed back: the name in use leaves the former names, and the name left behind joins them.
	VisualScriptDocument back = renamed;
	back.Functions[0].Inputs[0].Name = "Value";
	Result<VisualScriptCommit> const restored = PrepareVisualScriptCommit(renamed, back, nullptr);
	REQUIRE(restored.IsOk());
	CHECK(restored.GetValue().Warnings ==
	      std::vector<std::string>{"parameter renamed: Clamp01's Amount → Value (Amount is kept in FormerNames)"});
	CHECK(restored.GetValue().Document.Functions[0].Inputs[0].FormerNames == std::vector<std::string>{"Amount"});
	Testing::ExpectStrictRoundTrip(restored.GetValue().Document);

	// Swapped names stay current names: nothing is recorded.
	VisualScriptDocument swapped = stored;
	swapped.Functions[0].Inputs = {{"Low", "float"}, {"Value", "float"}};
	Result<VisualScriptCommit> const swap = PrepareVisualScriptCommit(stored, swapped, nullptr);
	REQUIRE(swap.IsOk());
	CHECK(swap.GetValue().Warnings.empty());
	CHECK(swap.GetValue().Document == swapped);

	// Up to 16, the oldest dropped.
	VisualScriptDocument full = stored;
	full.Functions[0].Inputs[0].Name = "Latest";
	for (int name = 0; name < 16; name++)
	{
		full.Functions[0].Inputs[0].FormerNames.push_back("Former" + std::to_string(name));
	}
	Result<VisualScriptCommit> const capped = PrepareVisualScriptCommit(stored, full, nullptr);
	REQUIRE(capped.IsOk());
	std::vector<std::string> const& formerNames = capped.GetValue().Document.Functions[0].Inputs[0].FormerNames;
	REQUIRE(formerNames.size() == VisualScriptSerializer::MaxFormerNames);
	CHECK(formerNames.front() == "Former1");
	CHECK(formerNames.back() == "Value");

	// A parameter added under a former name of its list (a length change, so no rename) takes it over, in any document.
	VisualScriptDocument history = storedScript;
	history.Functions[0].Inputs[0].FormerNames = {"X", "Y"};
	VisualScriptDocument reused = history;
	reused.Functions[0].Inputs.push_back({"X", "float"});
	Result<VisualScriptCommit> const taken = PrepareVisualScriptCommit(history, reused, nullptr);
	REQUIRE_MESSAGE(taken.IsOk(), taken.GetError());
	CHECK(taken.GetValue().Warnings.empty());
	CHECK(taken.GetValue().Document.Functions[0].Inputs[0].FormerNames == std::vector<std::string>{"Y"});
	Testing::ExpectStrictRoundTrip(taken.GetValue().Document);
}

TEST_CASE("VisualScriptCommit: a library records renamed parameters by SetFunctionSignature's in-order rule, wherever the kept ones "
          "moved")
{
	auto const makeLibrary = [](std::vector<std::string> const& names)
	{
		VisualScriptDocument document;
		document.Kind = VisualScriptDocumentKind::Library;
		VisualScriptFunction function;
		function.ID = 1;
		function.Name = "F";
		function.Pure = true;
		for (std::string const& name : names)
		{
			function.Inputs.push_back({name, "float"});
		}
		document.Functions.push_back(std::move(function));
		return document;
	};
	struct Case
	{
		std::vector<std::string> Stored;
		std::vector<std::string> Edited;
		std::vector<VisualScriptParameter> Expected;
		std::vector<std::string> Warnings;
	};
	std::vector<Case> const cases = {
		// A kept parameter moved and another renamed: the name that went is the one that came's, not the one at its index.
		{{"A", "B"}, {"C", "A"}, {{"C", "float", {"B"}}, {"A", "float"}}, {"parameter renamed: F's B → C (B is kept in FormerNames)"}},
		{{"A", "B"}, {"B", "C"}, {{"B", "float"}, {"C", "float", {"A"}}}, {"parameter renamed: F's A → C (A is kept in FormerNames)"}},
		{{"A", "B", "C"},
	     {"A", "C", "D"},
	     {{"A", "float"}, {"C", "float"}, {"D", "float", {"B"}}},
	     {"parameter renamed: F's B → D (B is kept in FormerNames)"}},
		{{"A", "B"}, {"B", "A2"}, {{"B", "float"}, {"A2", "float", {"A"}}}, {"parameter renamed: F's A → A2 (A is kept in FormerNames)"}},
		{{"A", "B", "C"},
	     {"B", "D", "A"},
	     {{"B", "float"}, {"D", "float", {"C"}}, {"A", "float"}},
	     {"parameter renamed: F's C → D (C is kept in FormerNames)"}},
		// Two renamed: in order, the first that went to the first that came.
		{{"A", "B"},
	     {"D", "C"},
	     {{"D", "float", {"A"}}, {"C", "float", {"B"}}},
	     {"parameter renamed: F's A → D (A is kept in FormerNames)", "parameter renamed: F's B → C (B is kept in FormerNames)"}},
	};
	for (Case const& entry : cases)
	{
		std::string names;
		for (std::string const& name : entry.Stored)
		{
			names += name + " ";
		}
		names += "→";
		for (std::string const& name : entry.Edited)
		{
			names += " " + name;
		}
		CAPTURE(names);
		Result<VisualScriptCommit> const commit = PrepareVisualScriptCommit(makeLibrary(entry.Stored), makeLibrary(entry.Edited), nullptr);
		REQUIRE_MESSAGE(commit.IsOk(), commit.GetError());
		CHECK(commit.GetValue().Warnings == entry.Warnings);
		CHECK(commit.GetValue().Document.Functions[0].Inputs == entry.Expected);
		Testing::ExpectStrictRoundTrip(commit.GetValue().Document);
	}

	// What SetFunctionSignature relinks and what the commit records agree: the call's link to B now ends at C, which keeps
	// B for the calls of other files.
	VisualScriptDocument stored = makeLibrary({"A", "B"});
	stored.Graph.Nodes = {MakeNode(12, "Self"), MakeNode(20, "Function.Call", Json::object({{"Function", 1}}), Json::object({{"A", 0.5}}))};
	stored.Graph.Links = {MakeLink(12, "Value", 20, "B")};
	VisualScriptDocument edited = stored;
	Result<uint32_t> const signature = VisualScriptEditing::SetFunctionSignature(edited, 1, {{"C", "float"}, {"A", "float"}}, {});
	REQUIRE_MESSAGE(signature.IsOk(), signature.GetError());
	CHECK(edited.Graph.Links == std::vector<VisualScriptLink>{MakeLink(12, "Value", 20, "C")});
	CHECK(FindNode(edited.Graph, 20).Inputs == Json::object({{"A", 0.5}}));
	Result<VisualScriptCommit> const commit = PrepareVisualScriptCommit(stored, edited, nullptr);
	REQUIRE_MESSAGE(commit.IsOk(), commit.GetError());
	CHECK(commit.GetValue().Warnings == std::vector<std::string>{"parameter renamed: F's B → C (B is kept in FormerNames)"});
	CHECK(commit.GetValue().Document.Functions[0].Inputs == std::vector<VisualScriptParameter>{{"C", "float", {"B"}}, {"A", "float"}});
	Testing::ExpectStrictRoundTrip(commit.GetValue().Document);
}

TEST_CASE("VisualScriptCommit: a node resolved exactly whose pins the analysis renamed stores their current names, its links, literals "
          "and split paths following")
{
	// A call of a library function whose input X became Value, whose split input Offset became Shift and whose output R
	// became Result.
	VisualScriptDocument stored;
	VisualScriptNode call =
		MakeNode(20, "Function.Call", Json::object({{"Function", 1}, {"Graph", "5550"}}), Json::object({{"X", 0.5}, {"Offset.Y", 1.0}}));
	call.Split.Inputs = {"Offset"};
	stored.Graph.Nodes = {MakeNode(10, "Event.Update"), MakeNode(12, "Self"), call, MakeNode(21, "Call:Strada.Log.Info(string)")};
	stored.Graph.Links = {MakeLink(10, "Then", 20, "Execute"), MakeLink(12, "Value", 20, "Offset.X"), MakeLink(20, "Then", 21, "Execute"),
	                      MakeLink(20, "R", 21, "Message")};

	VisualScriptAnalysis analysis;
	analysis.Nodes.push_back(MakeAnalyzedNode(10, "Event.Update"));
	analysis.Nodes.push_back(MakeAnalyzedNode(12, "Self"));
	VisualScriptAnalyzedNode clamp = MakeAnalyzedNode(20, "Function.Call", VisualScriptResolution::Exact, {},
	                                                  {{VisualScriptPinDirection::In, "X", "Value"},
	                                                   {VisualScriptPinDirection::In, "Offset", "Shift"},
	                                                   {VisualScriptPinDirection::Out, "R", "Result"},
	                                                   {VisualScriptPinDirection::In, "Unused", "Other"}});
	clamp.Title = "Clamp01";
	analysis.Nodes.push_back(clamp);
	analysis.Nodes.push_back(MakeAnalyzedNode(21, "Call:Strada.Log.Info(string)"));

	Result<VisualScriptCommit> const prepared = PrepareVisualScriptCommit(stored, stored, &analysis);
	REQUIRE_MESSAGE(prepared.IsOk(), prepared.GetError());
	// One warning per pin the node used under its former name; none are renames of the document's own names.
	CHECK(prepared.GetValue().Warnings == std::vector<std::string>{"pin renamed: Clamp01 (node 20) X → Value",
	                                                               "pin renamed: Clamp01 (node 20) Offset → Shift",
	                                                               "pin renamed: Clamp01 (node 20) R → Result"});
	CHECK(prepared.GetValue().RenameCount == 0);
	VisualScriptDocument const& document = prepared.GetValue().Document;
	VisualScriptNode const& renamed = FindNode(document.Graph, 20);
	CHECK(renamed.Type == "Function.Call");
	CHECK(renamed.Settings == call.Settings);
	CHECK(renamed.Inputs == Json::object({{"Value", 0.5}, {"Shift.Y", 1.0}}));
	CHECK(renamed.Split.Inputs == std::vector<std::string>{"Shift"});
	CHECK(document.Graph.Links == std::vector<VisualScriptLink>{MakeLink(10, "Then", 20, "Execute"), MakeLink(12, "Value", 20, "Shift.X"),
	                                                            MakeLink(20, "Then", 21, "Execute"),
	                                                            MakeLink(20, "Result", 21, "Message")});
	Testing::ExpectStrictRoundTrip(document);
	Testing::ExpectSplitConsistency(document);

	// The commit request notes them, as it notes canonical rewrites.
	Result<VisualScriptCommitRequest> const request = MakeVisualScriptCommitRequest(stored, stored, &analysis, "Move Node", "Player", 0);
	REQUIRE(request.IsOk());
	CHECK(request.GetValue().Notes == prepared.GetValue().Warnings);

	// An analysis of another version of the node, or a node that names its pins as they are now, changes nothing.
	VisualScriptAnalysis stale = analysis;
	stale.Nodes[2].Type = "Function.Entry";
	Result<VisualScriptCommit> const unchanged = PrepareVisualScriptCommit(stored, stored, &stale);
	REQUIRE(unchanged.IsOk());
	CHECK(unchanged.GetValue().Warnings.empty());
	CHECK(unchanged.GetValue().Document.Graph == stored.Graph);
	Result<VisualScriptCommit> const current = PrepareVisualScriptCommit(document, document, &analysis);
	REQUIRE(current.IsOk());
	CHECK(current.GetValue().Warnings.empty());
	CHECK(current.GetValue().Document.Graph == document.Graph);

	// The analysis of the document before an edit describes the nodes the stored document has: an edit that moves the node
	// keeps it the one analyzed, ...
	VisualScriptDocument moved = stored;
	moved.Graph.Nodes[2].Position = glm::vec2(800.0f, 480.0f);
	Result<VisualScriptCommit> const afterMove = PrepareVisualScriptCommit(stored, moved, &analysis);
	REQUIRE(afterMove.IsOk());
	CHECK(afterMove.GetValue().Warnings == prepared.GetValue().Warnings);
	// ... one that points it at another function (whose input may well be named X) does not, nor does a node the stored
	// document lacks: their pins keep their names until an analysis of them renames them.
	for (char const* const setting : {"Function", "Graph"})
	{
		CAPTURE(setting);
		VisualScriptDocument retargeted = stored;
		retargeted.Graph.Nodes[2].Settings[setting] = std::string_view(setting) == "Function" ? Json(4) : Json("5551");
		Result<VisualScriptCommit> const elsewhere = PrepareVisualScriptCommit(stored, retargeted, &analysis);
		REQUIRE(elsewhere.IsOk());
		CHECK(elsewhere.GetValue().Warnings.empty());
		CHECK(elsewhere.GetValue().Document.Graph == retargeted.Graph);
	}
	VisualScriptDocument without = stored;
	without.Graph.Nodes.erase(without.Graph.Nodes.begin() + 2);
	std::erase_if(without.Graph.Links,
	              [](VisualScriptLink const& link)
	              {
					  return link.From.Node == 20 || link.To.Node == 20;
				  });
	Result<VisualScriptCommit> const added = PrepareVisualScriptCommit(without, stored, &analysis);
	REQUIRE(added.IsOk());
	CHECK(added.GetValue().Warnings.empty());
	CHECK(added.GetValue().Document.Graph == stored.Graph);
}

TEST_CASE("VisualScriptCommit: unplaced nodes are laid out, and a document that would not load back as written is never committed")
{
	// Unplaced nodes are placed as the loader's layout places them; placed ones stay.
	VisualScriptDocument stored;
	stored.Graph.Nodes.push_back(MakeNode(1, "Event.Update"));
	VisualScriptDocument edited = stored;
	edited.Graph.Nodes.push_back(MakeNode(2, "Flow.Branch"));
	edited.Graph.Nodes.push_back(MakeNode(3, "Log.Info"));
	edited.Graph.Nodes[1].Position.reset();
	edited.Graph.Nodes[2].Position.reset();
	edited.Graph.Links.push_back(MakeLink(1, "Then", 2, "Execute"));
	edited.Graph.Links.push_back(MakeLink(2, "True", 3, "Execute"));
	VisualScriptDocument expected = edited;
	PlaceUnplacedNodes(expected);
	Result<VisualScriptCommit> const placed = PrepareVisualScriptCommit(stored, edited, nullptr);
	REQUIRE(placed.IsOk());
	CHECK(placed.GetValue().Document == expected);
	CHECK(placed.GetValue().Warnings.empty());
	// The text to write is the file's canonical text of what was placed.
	CHECK(placed.GetValue().Text == DumpJson(VisualScriptSerializer::Serialize(placed.GetValue().Document)));
	for (VisualScriptNode const& node : placed.GetValue().Document.Graph.Nodes)
	{
		CHECK(node.Position.has_value());
	}
	CHECK(placed.GetValue().Document.Graph.Nodes[0].Position == stored.Graph.Nodes[0].Position);

	// What the loader would repair: a repeated ID, a link from a node the graph does not have, a field of a pin that is
	// not split, a name with '.'. Each fails, naming the first repair.
	std::string const prefix = "the edit would write an invalid document: ";
	VisualScriptDocument repeated = stored;
	repeated.Graph.Nodes.push_back(MakeNode(1, "Flow.Branch"));
	CHECK(CommitError(stored, repeated).starts_with(prefix + "Graph.Nodes[1].ID: "));
	VisualScriptDocument dangling = stored;
	dangling.Graph.Links.push_back(MakeLink(99, "Then", 1, "Execute"));
	CHECK(CommitError(stored, dangling).starts_with(prefix + "Graph.Links[0]"));
	VisualScriptDocument unsplit = stored;
	unsplit.Graph.Nodes[0].Split.Outputs = {"Hit.Point"};
	CHECK(CommitError(stored, unsplit).starts_with(prefix + "Graph.Nodes[0].Split.Outputs[0]: "));
	VisualScriptDocument dotted = stored;
	dotted.Variables.push_back(MakeVariable(5, "Max.Speed", "float"));
	CHECK(CommitError(stored, dotted).starts_with(prefix + "Variables[0].Name: "));
	// A value no file can hold (a NaN would be written as null): the loader refuses it in memory, naming it.
	VisualScriptDocument notANumber = stored;
	notANumber.Variables.push_back(MakeVariable(5, "Speed", "float", std::numeric_limits<double>::quiet_NaN()));
	CHECK(CommitError(stored, notANumber).starts_with(prefix + "Variables[0].Value: "));
}

TEST_CASE("VisualScriptCommit: a commit request names its action in its script, and notes the canonical rewrites but not the renames")
{
	VisualScriptDocument const stored = MakeOutdatedDocument();
	VisualScriptDocument edited = stored;
	edited.Variables[0].Name = "Velocity";
	edited.Graph.Nodes[0].Position = glm::vec2(64.0f, 32.0f);
	VisualScriptAnalysis const analysis = MakeOutdatedAnalysis();

	Result<VisualScriptCommitRequest> const request =
		MakeVisualScriptCommitRequest(stored, edited, &analysis, "Move Node", "Player", 0x5653454400000007ull);
	REQUIRE(request.IsOk());
	CHECK(request.GetValue().Description == "Move Node in Player");
	CHECK(request.GetValue().MergeKey == 0x5653454400000007ull);
	CHECK(request.GetValue().Select.empty());
	CHECK(request.GetValue().Notes == std::vector<std::string>{"node updated: Call:Old.Scale → Call:New.Scale(int)",
	                                                           "node updated: Call:Old.Push → Call:New.Push(Strada.Vector3)",
	                                                           "node updated: Call:Old.Store → Call:Old.Store(int)"});
	// The document is the prepared one, with its file's text.
	Result<VisualScriptCommit> const prepared = PrepareVisualScriptCommit(stored, edited, &analysis);
	REQUIRE(prepared.IsOk());
	REQUIRE(request.GetValue().Document != nullptr);
	CHECK(*request.GetValue().Document == prepared.GetValue().Document);
	CHECK(request.GetValue().Text == prepared.GetValue().Text);
	CHECK(request.GetValue().Text == DumpJson(VisualScriptSerializer::Serialize(*request.GetValue().Document)));

	// Without a script name the action alone; without anything to note, no notes.
	Result<VisualScriptCommitRequest> const plain = MakeVisualScriptCommitRequest(stored, stored, nullptr, "Update Nodes", "", 0);
	REQUIRE(plain.IsOk());
	CHECK(plain.GetValue().Description == "Update Nodes");
	CHECK(plain.GetValue().Notes.empty());

	// A document that cannot be committed gives no request.
	VisualScriptDocument repeated = stored;
	repeated.Graph.Nodes.push_back(MakeNode(10, "Flow.Branch"));
	Result<VisualScriptCommitRequest> const refused = MakeVisualScriptCommitRequest(stored, repeated, nullptr, "Add Branch", "Player", 0);
	REQUIRE(refused.IsError());
	CHECK(refused.GetError().starts_with("the edit would write an invalid document: Graph.Nodes[5].ID: "));
}

TEST_CASE("VisualScriptCommit: documents are the same only when every value is written alike")
{
	VisualScriptDocument document = MakeOutdatedDocument();
	document.Graph.Nodes.push_back(MakeNode(20, "Flow.Sequence", Json::object({{"Count", 3}})));
	CHECK(IsSameVisualScriptDocument(document, document));
	VisualScriptDocument copy = document;
	CHECK(IsSameVisualScriptDocument(document, copy));

	// operator== takes an integer for the float of the same value; the runtime does not (a count must be an integer).
	VisualScriptDocument floating = document;
	floating.Graph.Nodes.back().Settings["Count"] = 3.0;
	REQUIRE(floating == document);
	CHECK_FALSE(IsSameVisualScriptDocument(document, floating));
	CHECK_FALSE(IsSameVisualScriptDocument(floating, document));

	// What the runtime never reads still makes another document.
	VisualScriptDocument moved = document;
	moved.Graph.Nodes[0].Position = glm::vec2(1.0f, 2.0f);
	REQUIRE(HaveSameVisualScriptProgram(moved, document));
	CHECK_FALSE(IsSameVisualScriptDocument(document, moved));
	VisualScriptDocument described = document;
	described.Description = "Pushes";
	CHECK_FALSE(IsSameVisualScriptDocument(document, described));
}

TEST_CASE("VisualScriptCommit: one edit of a 2,000-node graph is prepared, serialized once, written and taken by the asset as one step, "
          "in its time on request")
{
	// The graph of 2,000 nodes always; with STRADA_PERFORMANCE_TESTS=1 also one of 200, and the time of each phase (spec
	// §5.2).
	bool const performance = ArePerformanceTestsEnabled();
	std::vector<uint32_t> sizes = {2000};
	if (performance)
	{
		sizes.push_back(200);
	}
	for (uint32_t const nodeCount : sizes)
	{
		CAPTURE(nodeCount);
		size_t const linkCount = static_cast<size_t>(nodeCount) * 3 / 2;
		Testing::AssetManagerScope assets;
		Testing::TemporaryDirectory directory;
		EditorContext context;
		EditorOperations operations(context);
		REQUIRE(operations.CreateProject(directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
		Result<AssetHandle> const created = operations.CreateVisualScript("Scripts/Large.sgraph", VisualScriptTemplate::Empty);
		REQUIRE(created.IsOk());
		AssetHandle const graph = created.GetValue();
		REQUIRE(operations.SetVisualScriptDocument(graph, MakeLargeDocument(nodeCount)).IsOk());
		Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
		REQUIRE(asset != nullptr);
		REQUIRE(asset->GetDocument().Graph.Nodes.size() == nodeCount);
		REQUIRE(asset->GetDocument().Graph.Links.size() == linkCount);
		Testing::ExpectStrictRoundTrip(asset->GetDocument());

		// The analysis a graph window keeps: every node resolved exactly.
		VisualScriptAnalysis analysis;
		for (VisualScriptNode const& node : asset->GetDocument().Graph.Nodes)
		{
			analysis.Nodes.push_back(MakeAnalyzedNode(node.ID, node.Type));
		}

		// A node moved per commit, each its own step: the pipeline, the canonical text, the file and the asset.
		constexpr size_t Commits = 7;
		std::vector<double> milliseconds;
		std::vector<VisualScriptCommitTimings> phases;
		for (size_t commit = 0; commit < Commits; commit++)
		{
			Ref<VisualScriptDocument const> const stored = asset->GetSharedDocument();
			// The copy the canvas edits is not part of the commit.
			VisualScriptDocument edited = *stored;
			edited.Graph.Nodes[commit].Position->x += 16.0f;
			uint64_t const version = asset->GetVersion();
			size_t const steps = context.GetHistory().GetUndoCount();

			std::chrono::steady_clock::time_point const start = std::chrono::steady_clock::now();
			Result<VisualScriptCommitRequest> made =
				MakeVisualScriptCommitRequest(*stored, std::move(edited), &analysis, "Move Node", "Large", 0);
			REQUIRE_MESSAGE(made.IsOk(), (made ? std::string() : made.GetError()));
			VisualScriptCommitRequest request = made.TakeValue();
			Ref<VisualScriptDocument const> const committed = request.Document;
			auto const text = reinterpret_cast<std::uintptr_t>(request.Text.data());
			REQUIRE(operations.EditVisualScript(graph, committed, request.Description, request.MergeKey, std::move(request.Text)).IsOk());
			milliseconds.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());

			// Always: one step, one new version, and the asset holds the committed document itself.
			CHECK(context.GetHistory().GetUndoCount() == steps + 1);
			CHECK(asset->GetVersion() == version + 1);
			CHECK(asset->GetSharedDocument().get() == committed.get());
			// The document was serialized once: the text the strict check read back is the one written and remembered, moved
			// along without a copy.
			std::string const* const remembered = context.FindVisualScriptText(graph, committed.get());
			REQUIRE(remembered != nullptr);
			CHECK(reinterpret_cast<std::uintptr_t>(remembered->data()) == text);

			// The phases: the preparation's, then the step's.
			auto const* const step = dynamic_cast<SetVisualScriptCommand const*>(context.GetHistory().GetUndoCommand());
			REQUIRE(step != nullptr);
			VisualScriptCommitTimings const& executed = step->GetLastTimings();
			VisualScriptCommitTimings timings = request.Timings;
			timings.ReadBack = executed.ReadBack;
			timings.Compare = executed.Compare;
			timings.Write = executed.Write;
			timings.SetDocument = executed.SetDocument;
			phases.push_back(timings);
		}
		Result<Buffer> const written = FileSystem::ReadBinaryFile(AssetManager::GetAbsolutePath(graph));
		REQUIRE(written.IsOk());
		std::string const text = DumpJson(VisualScriptSerializer::Serialize(asset->GetDocument()));
		CHECK(std::string(written.GetValue().As<char>(), static_cast<size_t>(written.GetValue().GetSize())) == text);

		std::vector<double> sorted = milliseconds;
		std::sort(sorted.begin(), sorted.end());
		double const median = sorted[sorted.size() / 2];
		double const percentile95 = sorted[static_cast<size_t>(std::ceil(0.95 * static_cast<double>(sorted.size()))) - 1];
		MESSAGE("committing one edit of " << nodeCount << " nodes and " << linkCount << " links: median " << median << " ms, p95 "
		                                  << percentile95 << " ms");
		if (performance)
		{
			MESSAGE("phases of the commit of " << nodeCount << " nodes (medians, ms): Normalize "
			                                   << MedianPhase(phases, &VisualScriptCommitTimings::Normalize) << ", Check "
			                                   << MedianPhase(phases, &VisualScriptCommitTimings::Check) << ", Text "
			                                   << MedianPhase(phases, &VisualScriptCommitTimings::Text) << ", ReadBack "
			                                   << MedianPhase(phases, &VisualScriptCommitTimings::ReadBack) << ", Compare "
			                                   << MedianPhase(phases, &VisualScriptCommitTimings::Compare) << ", Write "
			                                   << MedianPhase(phases, &VisualScriptCommitTimings::Write) << ", SetDocument "
			                                   << MedianPhase(phases, &VisualScriptCommitTimings::SetDocument));
			if (nodeCount == 2000)
			{
				// Three times the median of five runs measured with no build running (an AMD Ryzen 5 7535HS laptop, Windows 11,
				// MSVC 19.51).
#if defined(ST_DEBUG)
				constexpr double MeasuredMilliseconds = 371.0;
#else
				constexpr double MeasuredMilliseconds = 21.9;
#endif
				CHECK(median <= 3.0 * MeasuredMilliseconds);
			}
		}
	}
}

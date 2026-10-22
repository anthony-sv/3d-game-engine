#include "TestUtilities.h"
#include "VisualScriptTestDocuments.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/VisualScripting/VisualScriptCommit.h"

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

	// 2,000 additions in rows of 50, each feeding the next one, and the first 1,001 also the one after it: 3,000 links.
	VisualScriptDocument MakeLargeDocument()
	{
		constexpr uint32_t NodeCount = 2000;
		constexpr size_t LinkCount = 3000;
		VisualScriptDocument document;
		document.Graph.Nodes.reserve(NodeCount);
		for (uint32_t index = 0; index < NodeCount; index++)
		{
			VisualScriptNode node;
			node.ID = index + 1;
			node.Type = "Operator.Add";
			node.Position = glm::vec2(static_cast<float>(index % 50) * 320.0f, static_cast<float>(index / 50) * 160.0f);
			node.Inputs = Json::object({{"B", 1.5}});
			document.Graph.Nodes.push_back(std::move(node));
		}
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		links.reserve(LinkCount);
		for (uint32_t id = 1; id < NodeCount; id++)
		{
			links.push_back(MakeLink(id, "Result", id + 1, "A"));
		}
		for (uint32_t id = 1; links.size() < LinkCount; id++)
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
	// A value no file can hold: what would be written does not load back as it is.
	VisualScriptDocument notANumber = stored;
	notANumber.Variables.push_back(MakeVariable(5, "Speed", "float", std::numeric_limits<double>::quiet_NaN()));
	CHECK(CommitError(stored, notANumber) == prefix + "it does not load back as written");
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
	// The document is the prepared one.
	Result<VisualScriptCommit> const prepared = PrepareVisualScriptCommit(stored, edited, &analysis);
	REQUIRE(prepared.IsOk());
	REQUIRE(request.GetValue().Document != nullptr);
	CHECK(*request.GetValue().Document == prepared.GetValue().Document);

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

TEST_CASE(
	"VisualScriptCommit: one edit of a 2,000-node graph is prepared, written and taken by the asset as one step, in its time on request")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory directory;
	EditorContext context;
	EditorOperations operations(context);
	REQUIRE(operations.CreateProject(directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
	Result<AssetHandle> const created = operations.CreateVisualScript("Scripts/Large.sgraph", VisualScriptTemplate::Empty);
	REQUIRE(created.IsOk());
	AssetHandle const graph = created.GetValue();
	REQUIRE(operations.SetVisualScriptDocument(graph, MakeLargeDocument()).IsOk());
	Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
	REQUIRE(asset != nullptr);
	REQUIRE(asset->GetDocument().Graph.Nodes.size() == 2000);
	REQUIRE(asset->GetDocument().Graph.Links.size() == 3000);
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
	for (size_t commit = 0; commit < Commits; commit++)
	{
		Ref<VisualScriptDocument const> const stored = asset->GetSharedDocument();
		// The copy the canvas edits is not part of the commit.
		VisualScriptDocument edited = *stored;
		edited.Graph.Nodes[commit].Position->x += 16.0f;
		uint64_t const version = asset->GetVersion();
		size_t const steps = context.GetHistory().GetUndoCount();

		std::chrono::steady_clock::time_point const start = std::chrono::steady_clock::now();
		Result<VisualScriptCommitRequest> request =
			MakeVisualScriptCommitRequest(*stored, std::move(edited), &analysis, "Move Node", "Large", 0);
		REQUIRE_MESSAGE(request.IsOk(), (request ? std::string() : request.GetError()));
		Ref<VisualScriptDocument const> const committed = request.GetValue().Document;
		REQUIRE(operations.EditVisualScript(graph, committed, request.GetValue().Description).IsOk());
		milliseconds.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());

		// Always: one step, one new version, and the asset holds the committed document itself.
		CHECK(context.GetHistory().GetUndoCount() == steps + 1);
		CHECK(asset->GetVersion() == version + 1);
		CHECK(asset->GetSharedDocument().get() == committed.get());
	}
	Result<Buffer> const written = FileSystem::ReadBinaryFile(AssetManager::GetAbsolutePath(graph));
	REQUIRE(written.IsOk());
	std::string const text = DumpJson(VisualScriptSerializer::Serialize(asset->GetDocument()));
	CHECK(std::string(written.GetValue().As<char>(), static_cast<size_t>(written.GetValue().GetSize())) == text);

	std::sort(milliseconds.begin(), milliseconds.end());
	double const median = milliseconds[milliseconds.size() / 2];
	double const percentile95 = milliseconds[static_cast<size_t>(std::ceil(0.95 * static_cast<double>(milliseconds.size()))) - 1];
	MESSAGE("committing one edit of 2,000 nodes and 3,000 links: median " << median << " ms, p95 " << percentile95 << " ms");
	if (ArePerformanceTestsEnabled())
	{
		// Three times the median measured when the test was written (an AMD Ryzen 5 7535HS laptop, Windows 11, MSVC 19.51).
#if defined(ST_DEBUG)
		constexpr double MeasuredMilliseconds = 993.0;
#else
		constexpr double MeasuredMilliseconds = 36.1;
#endif
		CHECK(median <= 3.0 * MeasuredMilliseconds);
	}
}

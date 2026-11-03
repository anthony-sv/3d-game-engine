#include "TestUtilities.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/VisualScripting/VisualScriptProjectIndex.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = glm::vec2(static_cast<float>(id) * 16.0f, 0.0f);
		node.Settings = std::move(settings);
		return node;
	}

	// A script whose nodes name the graph: a Get of its variable 1 and two typed Sends.
	VisualScriptDocument MakeReferrer(AssetHandle graph)
	{
		VisualScriptDocument document;
		document.Graph.Nodes = {MakeNode(1, "Variable.Get", Json::object({{"Variable", 1}, {"Graph", graph.ToString()}})),
		                        MakeNode(2, "Event.Send", Json::object({{"Name", "Hit"}, {"Graph", graph.ToString()}})),
		                        MakeNode(3, "Event.Send", Json::object({{"Name", "Open"}, {"Graph", graph.ToString()}}))};
		return document;
	}

	std::vector<std::string> ListPaths(VisualScriptProjectIndex const& index)
	{
		std::vector<std::string> paths;
		for (VisualScriptProjectGraph const& graph : index.GetGraphs())
		{
			paths.push_back(graph.Path);
		}
		return paths;
	}

	// The entries logged since firstIndex whose message holds the text.
	size_t CountLogged(uint64_t firstIndex, std::string_view text)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(firstIndex);
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
		                                         [text](LogEntry const& entry)
		                                         {
													 return entry.Message.find(text) != std::string::npos;
												 }));
	}

	struct ProjectFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};

		ProjectFixture() { REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk()); }

		AssetHandle Create(std::string const& path, VisualScriptTemplate kind = VisualScriptTemplate::Events)
		{
			Result<AssetHandle> const created = Operations.CreateVisualScript(path, kind);
			REQUIRE_MESSAGE(created.IsOk(), created.GetError());
			return created.GetValue();
		}
	};
}

TEST_CASE(
	"VisualScriptEditor: the project index's refreshes follow commits, moves, deletions and load failures, and idle ones change nothing")
{
	ProjectFixture fixture;
	VisualScriptProjectIndex index;
	CHECK(index.GetGraphs().empty());
	uint64_t const empty = index.GetRevision();
	index.Refresh();
	CHECK(index.GetRevision() == empty);

	AssetHandle const door = fixture.Create("Scripts/Door.sgraph");
	AssetHandle const motion = fixture.Create("Libraries/Motion.sgraph", VisualScriptTemplate::Library);
	AssetHandle const player = fixture.Create("Scripts/Player.sgraph");
	index.Refresh();
	uint64_t revision = index.GetRevision();
	CHECK(revision != empty);
	CHECK(ListPaths(index) == std::vector<std::string>{"Libraries/Motion.sgraph", "Scripts/Door.sgraph", "Scripts/Player.sgraph"});
	VisualScriptProjectGraph const* found = index.Find(motion);
	REQUIRE(found != nullptr);
	CHECK(found->DisplayName == "Motion");
	CHECK(found->Kind == VisualScriptDocumentKind::Library);
	REQUIRE(found->Document != nullptr);
	CHECK(found->LoadError.empty());
	CHECK(found->References.empty());
	CHECK(index.Find(AssetHandle(UUID(5ull))) == nullptr);

	// An idle refresh: nothing changes, nothing is counted.
	index.Refresh();
	CHECK(index.GetRevision() == revision);

	// A commit in another graph: its new document and what it refers to.
	REQUIRE(fixture.Operations.SetVisualScriptDocument(player, MakeReferrer(door)).IsOk());
	index.Refresh();
	CHECK(index.GetRevision() != revision);
	revision = index.GetRevision();
	found = index.Find(player);
	REQUIRE(found != nullptr);
	CHECK(found->Document == AssetManager::GetAsset<VisualScriptAsset>(player)->GetSharedDocument());
	CHECK(found->References == std::vector<VisualScriptGraphReference>{{door, VisualScriptReferenceUse::Interface, 3}});
	CHECK(index.FindReferrers(door) == std::vector<VisualScriptGraphReference>{{player, VisualScriptReferenceUse::Interface, 3}});
	CHECK(index.FindReferrers(player).empty());
	CHECK(DescribeVisualScriptReferrers(index, door) == "Player.sgraph (3 nodes)");
	CHECK(DescribeVisualScriptReferrers(index, player).empty());

	// A graph's references to itself are no referrers of it.
	REQUIRE(fixture.Operations.SetVisualScriptDocument(door, MakeReferrer(door)).IsOk());
	index.Refresh();
	CHECK(index.FindReferrers(door) == std::vector<VisualScriptGraphReference>{{player, VisualScriptReferenceUse::Interface, 3}});
	revision = index.GetRevision();

	// A move keeps the handle, with the new path.
	REQUIRE(fixture.Operations.MoveAsset(player, "Actors/Hero.sgraph").IsOk());
	index.Refresh();
	CHECK(index.GetRevision() != revision);
	revision = index.GetRevision();
	CHECK(ListPaths(index) == std::vector<std::string>{"Actors/Hero.sgraph", "Libraries/Motion.sgraph", "Scripts/Door.sgraph"});
	CHECK(index.Find(player)->DisplayName == "Hero");
	CHECK(DescribeVisualScriptReferrers(index, door) == "Hero.sgraph (3 nodes)");

	// A file that stops loading: no document, the error, no references.
	REQUIRE(FileSystem::WriteTextFile(AssetManager::GetAbsolutePath(player), "{ \"Strada\": ").IsOk());
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	index.Refresh();
	CHECK(index.GetRevision() != revision);
	revision = index.GetRevision();
	found = index.Find(player);
	REQUIRE(found != nullptr);
	CHECK(found->Document == nullptr);
	CHECK(found->LoadError.find("failed to load 'Actors/Hero.sgraph'") != std::string::npos);
	CHECK(found->References.empty());
	CHECK(index.FindReferrers(door).empty());
	index.Refresh();
	CHECK(index.GetRevision() == revision);

	// A deletion.
	REQUIRE(fixture.Operations.DeleteAsset(player).IsOk());
	index.Refresh();
	CHECK(index.GetRevision() != revision);
	CHECK(index.Find(player) == nullptr);
	CHECK(ListPaths(index) == std::vector<std::string>{"Libraries/Motion.sgraph", "Scripts/Door.sgraph"});
}

TEST_CASE("VisualScriptEditor: without assets the project index is empty")
{
	VisualScriptProjectIndex index;
	{
		ProjectFixture fixture;
		fixture.Create("Scripts/Door.sgraph");
		index.Refresh();
		CHECK(index.GetGraphs().size() == 1);
	}
	REQUIRE_FALSE(AssetManager::IsInitialized());
	uint64_t const revision = index.GetRevision();
	index.Refresh();
	CHECK(index.GetGraphs().empty());
	CHECK(index.GetRevision() != revision);
	index.Refresh();
	CHECK(index.GetRevision() == revision + 1);
}

TEST_CASE("VisualScriptEditor: the project index's resolver gives the window's working document for its own graph, the index's for others")
{
	ProjectFixture fixture;
	AssetHandle const door = fixture.Create("Scripts/Door.sgraph");
	AssetHandle const player = fixture.Create("Scripts/Player.sgraph", VisualScriptTemplate::Empty);
	VisualScriptProjectIndex index;
	index.Refresh();

	VisualScriptDocument working = MakeReferrer(door);
	VisualScriptGraphResolver const resolver = index.MakeResolver(player, &working);
	CHECK(resolver(player) == &working);
	CHECK(resolver(door) == index.Find(door)->Document.get());
	CHECK(resolver(AssetHandle(UUID(5ull))) == nullptr);
	// Without a working document the graph itself resolves to nothing, never to its stored asset; without self, to the
	// index's document.
	CHECK(index.MakeResolver(player)(player) == nullptr);
	CHECK(index.MakeResolver()(player) == index.Find(player)->Document.get());
}

TEST_CASE("VisualScriptEditor: the referrers named for the delete confirmation and the inspector are counted past five")
{
	ProjectFixture fixture;
	AssetHandle const door = fixture.Create("Door.sgraph");
	for (char const* const name : {"A", "B", "C", "D", "E", "F", "G"})
	{
		AssetHandle const script = fixture.Create(std::string(name) + ".sgraph", VisualScriptTemplate::Empty);
		VisualScriptDocument document;
		document.Graph.Nodes = {MakeNode(1, "RunsGraph", Json::object({{"Graph", door.ToString()}}))};
		REQUIRE(fixture.Operations.SetVisualScriptDocument(script, document).IsOk());
	}
	VisualScriptProjectIndex index;
	index.Refresh();
	CHECK(DescribeVisualScriptReferrers(index, door) == "A.sgraph, B.sgraph, C.sgraph, D.sgraph, E.sgraph and 2 more (7 nodes)");
	REQUIRE(fixture.Operations.DeleteAsset(AssetManager::FindByPath("A.sgraph")).IsOk());
	REQUIRE(fixture.Operations.DeleteAsset(AssetManager::FindByPath("B.sgraph")).IsOk());
	REQUIRE(fixture.Operations.DeleteAsset(AssetManager::FindByPath("C.sgraph")).IsOk());
	REQUIRE(fixture.Operations.DeleteAsset(AssetManager::FindByPath("D.sgraph")).IsOk());
	REQUIRE(fixture.Operations.DeleteAsset(AssetManager::FindByPath("E.sgraph")).IsOk());
	REQUIRE(fixture.Operations.DeleteAsset(AssetManager::FindByPath("F.sgraph")).IsOk());
	index.Refresh();
	CHECK(DescribeVisualScriptReferrers(index, door) == "G.sgraph (1 node)");
}

TEST_CASE("VisualScriptEditor: the project index reads a file that does not load once until it changes, across asset refreshes")
{
	ProjectFixture fixture;
	AssetHandle const door = fixture.Create("Scripts/Door.sgraph");
	AssetHandle const broken = fixture.Create("Scripts/Broken.sgraph");
	REQUIRE(FileSystem::WriteTextFile(AssetManager::GetAbsolutePath(broken), "{ \"Strada\": ").IsOk());
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	VisualScriptProjectIndex index;

	// Read once: one error logged, and the index says why it does not load.
	uint64_t const firstLog = Log::GetNextEntryIndex();
	index.Refresh();
	CHECK(CountLogged(firstLog, "failed to load 'Scripts/Broken.sgraph'") == 1);
	VisualScriptProjectGraph const* found = index.Find(broken);
	REQUIRE(found != nullptr);
	CHECK(found->Document == nullptr);
	CHECK(found->LoadError.find("failed to load 'Scripts/Broken.sgraph'") != std::string::npos);
	uint64_t const revision = index.GetRevision();

	// An asset refresh forgets the failed load (as the editor's does whenever it regains focus): the unchanged file is not
	// read again, so nothing more is logged and nothing changes.
	for (int round = 0; round < 3; round++)
	{
		REQUIRE(fixture.Operations.RefreshAssets().IsOk());
		REQUIRE_FALSE(AssetManager::HasLoadFailure(broken));
		index.Refresh();
		index.Refresh();
	}
	CHECK(CountLogged(firstLog, "failed to load 'Scripts/Broken.sgraph'") == 1);
	CHECK(index.GetRevision() == revision);
	CHECK(index.Find(broken)->LoadError.find("failed to load") != std::string::npos);

	// Changed, it is read again: broken another way, then fixed.
	REQUIRE(FileSystem::WriteTextFile(AssetManager::GetAbsolutePath(broken), "{ \"Strada\": 1 ").IsOk());
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	index.Refresh();
	CHECK(CountLogged(firstLog, "failed to load 'Scripts/Broken.sgraph'") == 2);
	REQUIRE(VisualScriptSerializer::SaveToFile(MakeReferrer(door), AssetManager::GetAbsolutePath(broken)).IsOk());
	REQUIRE(fixture.Operations.RefreshAssets().IsOk());
	index.Refresh();
	CHECK(CountLogged(firstLog, "failed to load 'Scripts/Broken.sgraph'") == 2);
	found = index.Find(broken);
	REQUIRE(found != nullptr);
	REQUIRE(found->Document != nullptr);
	CHECK(found->LoadError.empty());
	CHECK(index.FindReferrers(door) == std::vector<VisualScriptGraphReference>{{broken, VisualScriptReferenceUse::Interface, 3}});
	CHECK(index.GetRevision() != revision);
}

TEST_CASE("VisualScriptEditor: the project index keeps its entries while assets of other types come and go")
{
	ProjectFixture fixture;
	AssetHandle const door = fixture.Create("Scripts/Door.sgraph");
	AssetHandle const player = fixture.Create("Scripts/Player.sgraph");
	VisualScriptProjectIndex index;
	index.Refresh();
	uint64_t const revision = index.GetRevision();
	std::span<VisualScriptProjectGraph const> const graphs = index.GetGraphs();
	VisualScriptProjectGraph const* const found = index.Find(door);
	REQUIRE(found != nullptr);
	char const* const path = found->Path.data();

	// Another asset registered and unregistered: the registry's version changes, the scripts listed do not, and what
	// GetGraphs and Find gave stays (the revision too).
	uint64_t const version = AssetManager::GetVersion();
	AssetHandle const paint = AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(), "Paint");
	index.Refresh();
	AssetManager::RemoveMemoryAsset(paint);
	index.Refresh();
	REQUIRE(AssetManager::GetVersion() != version);
	CHECK(index.GetRevision() == revision);
	CHECK(index.GetGraphs().data() == graphs.data());
	CHECK(index.GetGraphs().size() == graphs.size());
	CHECK(index.Find(door) == found);
	CHECK(found->Path.data() == path);

	// A script moved where it keeps its place in the list: the entry is updated where it is, and the revision says so.
	REQUIRE(fixture.Operations.MoveAsset(door, "Scripts/Gate.sgraph").IsOk());
	index.Refresh();
	CHECK(index.GetRevision() != revision);
	CHECK(index.Find(door) == found);
	CHECK(found->Path == "Scripts/Gate.sgraph");
	CHECK(found->DisplayName == "Gate");
	CHECK(ListPaths(index) == std::vector<std::string>{"Scripts/Gate.sgraph", "Scripts/Player.sgraph"});
	CHECK(index.Find(player) != nullptr);
}

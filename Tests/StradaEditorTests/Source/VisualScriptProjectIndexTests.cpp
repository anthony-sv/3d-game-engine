#include "TestUtilities.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/VisualScripting/VisualScriptProjectIndex.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <string>
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

TEST_CASE("VisualScriptProjectIndex: refreshes follow commits, moves, deletions and load failures, and idle ones change nothing")
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

TEST_CASE("VisualScriptProjectIndex: without assets the index is empty")
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

TEST_CASE("VisualScriptProjectIndex: the resolver gives the window's working document for its own graph, the index's for others")
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

TEST_CASE("VisualScriptProjectIndex: the referrers named for the delete confirmation and the inspector are counted past five")
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

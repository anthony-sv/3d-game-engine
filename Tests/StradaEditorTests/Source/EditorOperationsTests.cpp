#include "TestUtilities.h"

#include "Editor/EditorOperations.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

using namespace Strada;

namespace
{
	Json Dump(EditorContext const& context)
	{
		return SceneSerializer::Serialize(context.GetScene());
	}

	EntityCreateInfo Named(std::string name, UUID parent = UUID::Invalid())
	{
		EntityCreateInfo info;
		info.Name = std::move(name);
		info.Parent = parent;
		return info;
	}
}

TEST_CASE("EditorOperations: every modification is undoable and redoable")
{
	EditorContext context;
	EditorOperations operations(context);
	Json const empty = Dump(context);

	Result<UUID> parent = operations.CreateEntity(Named("Parent"));
	REQUIRE(parent.IsOk());
	EntityCreateInfo childInfo = Named("Child", parent.GetValue());
	childInfo.Components = Json::object({{"PointLight", Json::object({{"Range", 7.0}})}});
	Result<UUID> child = operations.CreateEntity(childInfo);
	REQUIRE(child.IsOk());
	REQUIRE(operations.SetComponentFields(child.GetValue(), "Transform", Json::object({{"Translation", {1, 2, 3}}})).IsOk());
	REQUIRE(operations.RenameEntity(child.GetValue(), "Lamp").IsOk());
	REQUIRE(operations.AddComponent(parent.GetValue(), "SpotLight").IsOk());
	REQUIRE(operations.ReparentEntity(child.GetValue(), UUID::Invalid()).IsOk());
	Result<std::vector<UUID>> copies = operations.DuplicateEntities(std::vector<UUID>{child.GetValue()});
	REQUIRE(copies.IsOk());
	REQUIRE(operations.RemoveComponent(parent.GetValue(), "SpotLight").IsOk());
	REQUIRE(operations.DeleteEntities(std::vector<UUID>{parent.GetValue()}).IsOk());

	Json const final = Dump(context);
	size_t const steps = context.GetHistory().GetUndoCount();
	CHECK(steps == 9);

	std::vector<Json> states;
	for (size_t i = 0; i < steps; i++)
	{
		states.push_back(Dump(context));
		REQUIRE(operations.Undo().IsOk());
	}
	CHECK(Dump(context) == empty);
	CHECK(operations.Undo().IsError());

	for (size_t i = 0; i < steps; i++)
	{
		REQUIRE(operations.Redo().IsOk());
		CHECK(Dump(context) == states[steps - 1 - i]);
	}
	CHECK(Dump(context) == final);
	CHECK(operations.Redo().IsError());
}

TEST_CASE("EditorOperations: deleting and undoing restores IDs, components, hierarchy and order")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = operations.CreateEntity(Named("A")).GetValue();
	UUID const b = operations.CreateEntity(Named("B")).GetValue();
	UUID const c = operations.CreateEntity(Named("C")).GetValue();
	UUID const b1 = operations.CreateEntity(Named("B1", b)).GetValue();
	REQUIRE(operations.AddComponent(b1, "Camera", Json::object({{"PerspectiveFOV", 75.0}})).IsOk());
	Json const before = Dump(context);

	REQUIRE(operations.DeleteEntities(std::vector<UUID>{b}).IsOk());
	CHECK_FALSE(operations.FindEntity(b1));
	REQUIRE(operations.Undo().IsOk());
	CHECK(Dump(context) == before);
	CHECK(context.GetScene().GetSiblingIndex(operations.FindEntity(b)) == 1);
	(void)a;
	(void)c;
}

TEST_CASE("EditorOperations: invalid requests fail without changing the scene or history")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const parent = operations.CreateEntity(Named("Parent")).GetValue();
	UUID const child = operations.CreateEntity(Named("Child", parent)).GetValue();
	Json const before = Dump(context);
	size_t const steps = context.GetHistory().GetUndoCount();

	CHECK(operations.ReparentEntity(parent, child).IsError());
	CHECK(operations.SetComponentFields(child, "Transform", Json::object({{"Translation", "up"}})).IsError());
	CHECK(operations.SetComponentFields(child, "Transform", Json::object({{"Typo", 1}})).IsError());
	CHECK(operations.AddComponent(child, "NoSuchComponent").IsError());
	CHECK(operations.RemoveComponent(child, "Transform").IsError());
	CHECK(operations.RenameEntity(child, "").IsError());
	CHECK(operations.DeleteEntities(std::vector<UUID>{UUID(123)}).IsError());

	CHECK(Dump(context) == before);
	CHECK(context.GetHistory().GetUndoCount() == steps);
}

TEST_CASE("EditorOperations: continuous edits merge and dirty state follows the save point")
{
	Testing::TemporaryDirectory directory;
	EditorContext context;
	EditorOperations operations(context);
	CHECK_FALSE(context.IsDirty());

	UUID const entity = operations.CreateEntity(Named("Box")).GetValue();
	CHECK(context.IsDirty());
	std::filesystem::path const path = directory.GetPath() / "Level.sscene";
	REQUIRE(operations.SaveScene(path).IsOk());
	CHECK_FALSE(context.IsDirty());
	CHECK(context.GetScenePath() == path);

	constexpr uint64_t DragKey = 42;
	for (int i = 1; i <= 5; i++)
	{
		REQUIRE(operations.SetComponentFields(entity, "Transform", Json::object({{"Translation", {i, 0, 0}}}), DragKey).IsOk());
	}
	// The first edit after a save starts a new step; the drag then merges into it.
	CHECK(context.GetHistory().GetUndoCount() == 2);
	CHECK(context.IsDirty());
	REQUIRE(operations.Undo().IsOk());
	CHECK_FALSE(context.IsDirty());
	REQUIRE(operations.Redo().IsOk());
	CHECK(context.IsDirty());

	context.GetHistory().BreakMerge();
	REQUIRE(operations.SetComponentFields(entity, "Transform", Json::object({{"Translation", {9, 0, 0}}}), DragKey).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == 3);

	// Opening the saved file replaces the scene and clears the history.
	Result<std::vector<std::string>> opened = operations.OpenScene(path);
	REQUIRE(opened.IsOk());
	CHECK(context.GetHistory().GetUndoCount() == 0);
	CHECK_FALSE(context.IsDirty());
	CHECK(context.GetScene().FindEntityByName("Box"));
	CHECK(operations.OpenScene(directory.GetPath() / "Missing.sscene").IsError());
}

TEST_CASE("EditorOperations: the history is bounded")
{
	EditorContext context;
	EditorOperations operations(context);
	context.GetHistory().SetMaxSize(3);
	for (int i = 0; i < 5; i++)
	{
		REQUIRE(operations.CreateEntity(Named("E" + std::to_string(i))).IsOk());
	}
	CHECK(context.GetHistory().GetUndoCount() == 3);
	while (context.GetHistory().CanUndo())
	{
		REQUIRE(operations.Undo().IsOk());
	}
	CHECK(context.GetScene().GetEntityCount() == 2);
}

TEST_CASE("EditorOperations: selection follows the scene")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = operations.CreateEntity(Named("A")).GetValue();
	UUID const b = operations.CreateEntity(Named("B")).GetValue();

	REQUIRE(operations.Select(std::vector<UUID>{a, b}).IsOk());
	CHECK(context.GetSelection().GetPrimary() == b);
	REQUIRE(operations.Select(std::vector<UUID>{a}, SelectionMode::Toggle).IsOk());
	CHECK(context.GetSelection().GetEntities() == std::vector<UUID>{b});
	CHECK(operations.Select(std::vector<UUID>{UUID(99)}).IsError());

	REQUIRE(operations.DeleteEntities(std::vector<UUID>{b}).IsOk());
	CHECK(context.GetSelection().IsEmpty());
}

#include "Editor/EditorOperations.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;

namespace
{
	Json Dump(EditorContext const& context)
	{
		return SceneSerializer::Serialize(context.GetScene());
	}

	UUID Create(EditorOperations& operations, std::string name, UUID parent = UUID::Invalid(), Json components = Json::object())
	{
		EntityCreateInfo info;
		info.Name = std::move(name);
		info.Parent = parent;
		info.Components = std::move(components);
		Result<UUID> entity = operations.CreateEntity(info);
		REQUIRE(entity.IsOk());
		return entity.GetValue();
	}

	std::vector<UUID> RootOrder(EditorContext const& context)
	{
		return context.GetScene().GetRootEntityIDs();
	}

	std::vector<UUID> ChildOrder(EditorContext& context, UUID parent)
	{
		std::vector<UUID> children;
		for (Entity const child : context.GetScene().GetChildren(context.GetScene().GetEntityByUUID(parent)))
		{
			children.push_back(child.GetUUID());
		}
		return children;
	}
}

TEST_CASE("EditorOperations: moving entities reorders and reparents them as one undo step")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = Create(operations, "A", UUID::Invalid(), Json::object({{"Transform", {{"Translation", {1, 0, 0}}}}}));
	UUID const b = Create(operations, "B");
	UUID const c = Create(operations, "C");
	UUID const d = Create(operations, "D", UUID::Invalid(), Json::object({{"Transform", {{"Translation", {10, 0, 0}}}}}));
	Json const initial = Dump(context);
	size_t const steps = context.GetHistory().GetUndoCount();

	std::vector<UUID> const moveD = {d};
	REQUIRE(operations.MoveEntities(moveD, UUID::Invalid(), a).IsOk());
	CHECK(RootOrder(context) == std::vector<UUID>{d, a, b, c});
	CHECK(context.GetHistory().GetUndoDescription() == "Reorder Entity");
	REQUIRE(operations.Undo().IsOk());
	CHECK(Dump(context) == initial);
	REQUIRE(operations.Redo().IsOk());
	CHECK(RootOrder(context) == std::vector<UUID>{d, a, b, c});

	// An entity that precedes the anchor among the same siblings lands right before it.
	std::vector<UUID> const moveAC = {a, c};
	REQUIRE(operations.MoveEntities(moveAC, UUID::Invalid(), b).IsOk());
	CHECK(RootOrder(context) == std::vector<UUID>{d, a, c, b});
	CHECK(context.GetHistory().GetUndoDescription() == "Reorder Entities");

	// Reparenting keeps world transforms by default.
	std::vector<UUID> const moveAB = {a, b};
	REQUIRE(operations.MoveEntities(moveAB, d).IsOk());
	CHECK(RootOrder(context) == std::vector<UUID>{d, c});
	CHECK(ChildOrder(context, d) == std::vector<UUID>{a, b});
	CHECK(context.GetScene().GetEntityByUUID(a).GetComponent<TransformComponent>().Translation == glm::vec3(-9.0f, 0.0f, 0.0f));
	CHECK(context.GetHistory().GetUndoDescription() == "Reparent Entities");
	CHECK(context.GetHistory().GetUndoCount() == steps + 3);

	// A parent moved together with its child keeps the child.
	std::vector<UUID> const moveDA = {a, d};
	REQUIRE(operations.MoveEntities(moveDA, UUID::Invalid()).IsOk());
	CHECK(RootOrder(context) == std::vector<UUID>{c, d});
	CHECK(ChildOrder(context, d) == std::vector<UUID>{a, b});
	CHECK(context.GetHistory().GetUndoCount() == steps + 4);

	for (int i = 0; i < 4; i++)
	{
		REQUIRE(operations.Undo().IsOk());
	}
	CHECK(Dump(context) == initial);
	for (int i = 0; i < 4; i++)
	{
		REQUIRE(operations.Redo().IsOk());
	}
	CHECK(ChildOrder(context, d) == std::vector<UUID>{a, b});
	CHECK(context.GetScene().GetEntityByUUID(a).GetComponent<TransformComponent>().Translation == glm::vec3(-9.0f, 0.0f, 0.0f));
}

TEST_CASE("EditorOperations: invalid moves change nothing")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const parent = Create(operations, "Parent");
	UUID const child = Create(operations, "Child", parent);
	UUID const other = Create(operations, "Other");
	Json const initial = Dump(context);
	size_t const steps = context.GetHistory().GetUndoCount();

	std::vector<UUID> const moveParent = {parent};
	CHECK(operations.MoveEntities(moveParent, child).IsError());
	CHECK(operations.MoveEntities(moveParent, parent).IsError());
	// The anchor must be a child of the new parent, and cannot be moved itself.
	CHECK(operations.MoveEntities(moveParent, UUID::Invalid(), child).IsError());
	std::vector<UUID> const moveBoth = {other, parent};
	CHECK(operations.MoveEntities(moveBoth, UUID::Invalid(), other).IsError());
	std::vector<UUID> const missing = {UUID(12345)};
	CHECK(operations.MoveEntities(missing, UUID::Invalid()).IsError());
	CHECK(operations.MoveEntities(moveParent, UUID(54321)).IsError());
	CHECK(operations.MoveEntities(std::vector<UUID>{}, UUID::Invalid()).IsError());

	// Moving an entity to where it already is records nothing.
	CHECK(operations.MoveEntities(moveParent, UUID::Invalid(), other).IsOk());
	CHECK(Dump(context) == initial);
	CHECK(context.GetHistory().GetUndoCount() == steps);

	// Neither does moving several entities whose individual moves cancel out.
	UUID const last = Create(operations, "Last");
	Json const withLast = Dump(context);
	size_t const stepsWithLast = context.GetHistory().GetUndoCount();
	std::vector<UUID> const both = {parent, other};
	CHECK(operations.MoveEntities(both, UUID::Invalid(), last).IsOk());
	CHECK(Dump(context) == withLast);
	CHECK(context.GetHistory().GetUndoCount() == stepsWithLast);
}

TEST_CASE("EditorOperations: components are added to and removed from several entities as one undo step")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = Create(operations, "A");
	UUID const b = Create(operations, "B", UUID::Invalid(), Json::object({{"SpotLight", Json::object()}}));
	Json const initial = Dump(context);
	size_t const steps = context.GetHistory().GetUndoCount();

	std::vector<UUID> const both = {a, b};
	REQUIRE(operations.AddComponent(both, "PointLight", Json::object({{"Range", 7.0}})).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(context.GetHistory().GetUndoDescription() == "Add PointLight Component");
	CHECK(context.GetScene().GetEntityByUUID(b).GetComponent<PointLightComponent>().Range == 7.0f);
	REQUIRE(operations.Undo().IsOk());
	CHECK(Dump(context) == initial);
	REQUIRE(operations.Redo().IsOk());

	// All or nothing: B already has a spot light, so A does not get one either.
	CHECK(operations.AddComponent(both, "SpotLight").IsError());
	CHECK_FALSE(context.GetScene().GetEntityByUUID(a).HasComponent<SpotLightComponent>());
	CHECK(operations.AddComponent(both, "Transform").IsError());
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);

	REQUIRE(operations.RemoveComponent(both, "PointLight").IsOk());
	CHECK(context.GetHistory().GetUndoDescription() == "Remove PointLight Component");
	CHECK_FALSE(context.GetScene().GetEntityByUUID(a).HasComponent<PointLightComponent>());
	CHECK(operations.RemoveComponent(both, "SpotLight").IsError());
	CHECK(context.GetScene().GetEntityByUUID(b).HasComponent<SpotLightComponent>());
	REQUIRE(operations.Undo().IsOk());
	CHECK(context.GetScene().GetEntityByUUID(a).GetComponent<PointLightComponent>().Range == 7.0f);
}

TEST_CASE("EditorOperations: several entities are renamed as one undo step")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = Create(operations, "A");
	UUID const b = Create(operations, "B");
	std::vector<UUID> const both = {a, b};
	size_t const steps = context.GetHistory().GetUndoCount();

	REQUIRE(operations.RenameEntities(both, "Lamp").IsOk());
	CHECK(context.GetScene().GetEntityByUUID(a).GetName() == "Lamp");
	CHECK(context.GetScene().GetEntityByUUID(b).GetName() == "Lamp");
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(context.GetHistory().GetUndoDescription() == "Rename Entities");
	CHECK(operations.RenameEntities(both, "").IsError());

	REQUIRE(operations.Undo().IsOk());
	CHECK(context.GetScene().GetEntityByUUID(a).GetName() == "A");
	CHECK(context.GetScene().GetEntityByUUID(b).GetName() == "B");
}

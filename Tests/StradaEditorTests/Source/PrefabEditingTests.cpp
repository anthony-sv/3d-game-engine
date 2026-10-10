#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/EditorOperations.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Math/Math.h"
#include "Strada/Scene/Entity.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	// A fresh project open in an editor model, with the editor commands registered.
	struct PrefabFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		CommandRegistry Registry;

		PrefabFixture()
		{
			// As the editor layer does: entity fields may name assets by reference.
			Context.SetAssetReferenceResolver(
				[](std::string_view reference)
				{
					return AssetManager::ResolveReference(reference);
				});
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			REQUIRE(RegisterEditorCommands(Registry, Operations, EditorCommandEnvironment()).IsOk());
		}

		// A crate with a child and a light, rotated and scaled.
		UUID CreateCrate()
		{
			EntityCreateInfo crate;
			crate.Name = "Crate";
			crate.Components = Json::object({{"Transform", Json::object({{"Translation", {1.0, 2.0, 3.0}},
			                                                             {"Rotation", {0.0, 0.7071068, 0.0, 0.7071068}},
			                                                             {"Scale", {2.0, 2.0, 2.0}}})},
			                                 {"Mesh", Json::object({{"Mesh", "builtin://Cube"}})}});
			Result<UUID> root = Operations.CreateEntity(crate);
			REQUIRE_MESSAGE(root.IsOk(), (root ? std::string() : root.GetError()));
			EntityCreateInfo lamp;
			lamp.Name = "Lamp";
			lamp.Parent = root.GetValue();
			lamp.Components = Json::object({{"PointLight", Json::object({{"Range", 4.0}})}});
			REQUIRE(Operations.CreateEntity(lamp).IsOk());
			return root.GetValue();
		}

		Json Run(std::string_view name, Json params, std::optional<AutomationErrorCode> expectedError = std::nullopt)
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			if (expectedError)
			{
				REQUIRE(result->IsError());
				CHECK(result->GetError().Code == *expectedError);
				return Json();
			}
			REQUIRE_MESSAGE(result->IsOk(), (result->IsError() ? result->GetError().Message : std::string()));
			return result->TakeValue();
		}
	};
}

TEST_CASE("EditorOperations: prefabs are written from entities without changing the scene")
{
	PrefabFixture fixture;
	EditorOperations& operations = fixture.Operations;
	UUID const crate = fixture.CreateCrate();
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();

	Result<AssetHandle> prefab = operations.CreatePrefab(crate, "Prefabs/Crate.sprefab");
	REQUIRE_MESSAGE(prefab.IsOk(), (prefab ? std::string() : prefab.GetError()));
	CHECK(AssetManager::GetAssetType(prefab.GetValue()) == AssetType::Prefab);
	CHECK(AssetManager::FindByPath("Prefabs/Crate.sprefab") == prefab.GetValue());
	CHECK(FileSystem::IsRegularFile(fixture.Context.GetProject()->GetAssetDirectory() / "Prefabs" / "Crate.sprefab"));
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	CHECK_FALSE(fixture.Context.GetScene().GetEntityByUUID(crate).HasComponent<PrefabComponent>());

	CHECK(operations.CreatePrefab(crate, "Prefabs/Crate.sprefab").IsError());
	CHECK(operations.CreatePrefab(crate, "Prefabs/Crate.smat").IsError());
	CHECK(operations.CreatePrefab(UUID(), "Prefabs/Nothing.sprefab").IsError());
	CHECK(operations.CreatePrefab(crate, "../Outside.sprefab").IsError());

	// Named after the entity, numbered when the name is taken.
	Result<AssetHandle> named = operations.CreatePrefabInFolder(crate, "Prefabs");
	REQUIRE(named.IsOk());
	CHECK(AssetManager::GetMetadata(named.GetValue())->Path == "Prefabs/Crate 1.sprefab");
	Result<AssetHandle> rooted = operations.CreatePrefabInFolder(crate, "");
	REQUIRE(rooted.IsOk());
	CHECK(AssetManager::GetMetadata(rooted.GetValue())->Path == "Crate.sprefab");
}

TEST_CASE("EditorOperations: prefab instances are undoable copies placed where asked")
{
	PrefabFixture fixture;
	EditorOperations& operations = fixture.Operations;
	Scene& scene = fixture.Context.GetScene();
	UUID const crate = fixture.CreateCrate();
	Result<AssetHandle> prefab = operations.CreatePrefab(crate, "Prefabs/Crate.sprefab");
	REQUIRE(prefab.IsOk());

	// Without a position the instance keeps the prefab's transform.
	Result<UUID> first = operations.InstantiatePrefab(prefab.GetValue());
	REQUIRE_MESSAGE(first.IsOk(), (first ? std::string() : first.GetError()));
	Entity const instance = scene.GetEntityByUUID(first.GetValue());
	REQUIRE(instance);
	CHECK(first.GetValue() != crate);
	CHECK(instance.GetName() == "Crate");
	CHECK(instance.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(instance.GetComponent<PrefabComponent>().Prefab == prefab.GetValue());
	std::vector<Entity> const children = scene.GetChildren(instance);
	REQUIRE(children.size() == 1);
	CHECK(children[0].GetName() == "Lamp");
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Instantiate Prefab");

	// Undo removes the whole instance; redo brings back the same entities.
	UUID const lamp = children[0].GetUUID();
	REQUIRE(fixture.Context.GetHistory().Undo(fixture.Context).IsOk());
	CHECK_FALSE(scene.HasEntity(first.GetValue()));
	CHECK_FALSE(scene.HasEntity(lamp));
	REQUIRE(fixture.Context.GetHistory().Redo(fixture.Context).IsOk());
	CHECK(scene.HasEntity(first.GetValue()));
	CHECK(scene.HasEntity(lamp));

	// A position moves the root there in world space; its world rotation and scale stay: the prefab's own as a root
	// entity, combined with the parent's under a parent (the crate is turned a quarter about Y and scaled by 2).
	auto const checkPlacement = [&scene](UUID id, glm::vec3 const& expectedPosition, float expectedTurns, float expectedScale)
	{
		glm::vec3 translation(0.0f);
		glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 scale(1.0f);
		REQUIRE(Math::DecomposeTransform(scene.GetWorldTransform(scene.GetEntityByUUID(id)), translation, rotation, scale));
		CHECK(translation.x == doctest::Approx(expectedPosition.x));
		CHECK(translation.y == doctest::Approx(expectedPosition.y));
		CHECK(translation.z == doctest::Approx(expectedPosition.z));
		CHECK(scale.x == doctest::Approx(expectedScale));
		glm::quat const expectedRotation = glm::angleAxis(expectedTurns * glm::two_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
		CHECK(std::abs(glm::dot(rotation, expectedRotation)) == doctest::Approx(1.0f));
	};
	Result<UUID> moved = operations.InstantiatePrefab(prefab.GetValue(), UUID::Invalid(), glm::vec3(5.0f, 0.0f, -1.0f));
	REQUIRE(moved.IsOk());
	checkPlacement(moved.GetValue(), glm::vec3(5.0f, 0.0f, -1.0f), 0.25f, 2.0f);
	Result<UUID> placed = operations.InstantiatePrefab(prefab.GetValue(), crate, glm::vec3(-4.0f, 0.5f, 6.0f));
	REQUIRE(placed.IsOk());
	CHECK(scene.GetParent(scene.GetEntityByUUID(placed.GetValue())).GetUUID() == crate);
	checkPlacement(placed.GetValue(), glm::vec3(-4.0f, 0.5f, 6.0f), 0.5f, 4.0f);

	CHECK(operations.InstantiatePrefab(AssetHandle()).IsError());
	CHECK(operations.InstantiatePrefab(AssetManager::FindBuiltIn("Cube")).IsError());
	CHECK(operations.InstantiatePrefab(prefab.GetValue(), UUID()).IsError());
}

TEST_CASE("EditorCommands: prefab.create and prefab.instantiate")
{
	PrefabFixture fixture;
	UUID const crate = fixture.CreateCrate();

	Json const created = fixture.Run("prefab.create", Json::object({{"entity", crate.ToString()}, {"path", "Prefabs/Crate.sprefab"}}));
	CHECK(created["type"] == "Prefab");
	CHECK(created["path"] == "Prefabs/Crate.sprefab");
	fixture.Run("prefab.create", Json::object({{"entity", crate.ToString()}, {"path", "Prefabs/Crate.sprefab"}}),
	            AutomationErrorCode::InvalidParams);
	fixture.Run("prefab.create", Json::object({{"entity", "12345"}, {"path", "Prefabs/Other.sprefab"}}),
	            AutomationErrorCode::EntityNotFound);

	Json const instance =
		fixture.Run("prefab.instantiate", Json::object({{"prefab", "asset://Prefabs/Crate.sprefab"}, {"position", {0.0, 1.0, 0.0}}}));
	std::optional<UUID> const root = UUID::FromString(instance["id"].get<std::string>());
	REQUIRE(root.has_value());
	CHECK(fixture.Context.GetScene().GetEntityByUUID(*root).GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 1.0f, 0.0f));
	Json const child = fixture.Run("prefab.instantiate", Json::object({{"prefab", created["id"]}, {"parent", crate.ToString()}}));
	CHECK(fixture.Context.GetScene()
	          .GetParent(fixture.Context.GetScene().GetEntityByUUID(*UUID::FromString(child["id"].get<std::string>())))
	          .GetUUID() == crate);

	fixture.Run("prefab.instantiate", Json::object({{"prefab", "asset://Prefabs/Missing.sprefab"}}), AutomationErrorCode::AssetNotFound);
	fixture.Run("prefab.instantiate", Json::object({{"prefab", "builtin://Cube"}}), AutomationErrorCode::InvalidOperation);
	fixture.Run("prefab.instantiate", Json::object({{"prefab", created["id"]}, {"position", {0.0, 1.0}}}),
	            AutomationErrorCode::InvalidParams);
}

#include "TestUtilities.h"

#include "Editor/AssetBrowsing.h"
#include "Editor/AssetDrops.h"
#include "Editor/EditorOperations.h"
#include "Editor/EntityPresets.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Scene/Entity.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;

namespace
{
	// Initializes the AssetManager for as long as it lives (also when the owner's constructor fails).
	class AssetManagerScope
	{
	public:
		AssetManagerScope() { AssetManager::Init(); }
		~AssetManagerScope() { AssetManager::Shutdown(); }

		AssetManagerScope(AssetManagerScope const&) = delete;
		AssetManagerScope& operator=(AssetManagerScope const&) = delete;
	};

	// A fresh project open in an editor model.
	struct ProjectFixture
	{
		AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};

		ProjectFixture() { REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk()); }

		std::filesystem::path GetAssetDirectory() const { return Context.GetProject()->GetAssetDirectory(); }
	};

	void WritePng(std::filesystem::path const& path)
	{
		Image image(2, 2, 4);
		REQUIRE(image.WritePNG(path).IsOk());
	}

	float Roughness(AssetHandle material)
	{
		return AssetManager::GetAsset<MaterialAsset>(material)->GetData().Roughness;
	}
}

TEST_CASE("EditorOperations: asset folders and materials are created, moved and deleted")
{
	ProjectFixture fixture;
	EditorOperations& operations = fixture.Operations;

	REQUIRE(operations.CreateAssetFolder("Materials").IsOk());
	CHECK(FileSystem::IsDirectory(fixture.GetAssetDirectory() / "Materials"));
	CHECK(operations.CreateAssetFolder("Materials").IsError());
	CHECK(operations.CreateAssetFolder("../Outside").IsError());
	CHECK(operations.CreateAssetFolder("").IsError());

	Result<AssetHandle> material = operations.CreateMaterial("Materials/Rock.smat", Json::object({{"Roughness", 0.9}}));
	REQUIRE(material.IsOk());
	CHECK(AssetManager::FindByPath("Materials/Rock.smat") == material.GetValue());
	CHECK(Roughness(material.GetValue()) == doctest::Approx(0.9f));
	CHECK(operations.CreateMaterial("Materials/Rock.smat").IsError());
	CHECK(operations.CreateMaterial("Materials/Rock.png").IsError());
	CHECK(operations.CreateMaterial("Materials/Bad.smat", Json::object({{"Roughness", 2.0}})).IsError());
	CHECK_FALSE(FileSystem::Exists(fixture.GetAssetDirectory() / "Materials" / "Bad.smat"));

	REQUIRE(operations.MoveAsset(material.GetValue(), "Materials/Stone.smat").IsOk());
	REQUIRE(operations.MoveAssetFolder("Materials", "Art/Materials").IsOk());
	CHECK(AssetManager::FindByPath("Art/Materials/Stone.smat") == material.GetValue());

	REQUIRE(operations.DeleteAsset(material.GetValue()).IsOk());
	CHECK_FALSE(AssetManager::IsValid(material.GetValue()));
	REQUIRE(operations.DeleteAssetFolder("Art").IsOk());
	CHECK_FALSE(FileSystem::Exists(fixture.GetAssetDirectory() / "Art"));

	Result<AssetRefreshResult> refreshed = operations.RefreshAssets();
	REQUIRE(refreshed.IsOk());
	CHECK(refreshed.GetValue().Added.empty());

	// Without a project every asset operation fails.
	REQUIRE(operations.CloseProject().IsOk());
	CHECK(operations.CreateAssetFolder("Textures").IsError());
	CHECK(operations.CreateMaterial("New.smat").IsError());
	CHECK(operations.RefreshAssets().IsError());
}

TEST_CASE("EditorOperations: imported files are copied, numbered when taken, and registered all or nothing")
{
	ProjectFixture fixture;
	EditorOperations& operations = fixture.Operations;
	std::filesystem::path const outside = fixture.Directory.GetPath() / "Downloads";
	WritePng(outside / "Wood.png");
	WritePng(outside / "Stone.png");
	REQUIRE(FileSystem::WriteTextFile(outside / "Notes.txt", "not an asset").IsOk());

	std::vector<std::filesystem::path> const both = {outside / "Wood.png", outside / "Stone.png"};
	Result<std::vector<AssetHandle>> imported = operations.ImportAssets(both, "Textures");
	REQUIRE(imported.IsOk());
	REQUIRE(imported.GetValue().size() == 2);
	CHECK(AssetManager::FindByPath("Textures/Wood.png") == imported.GetValue()[0]);
	CHECK(FileSystem::IsRegularFile(outside / "Wood.png"));

	std::vector<std::filesystem::path> const again = {outside / "Wood.png"};
	Result<std::vector<AssetHandle>> numbered = operations.ImportAssets(again, "Textures");
	REQUIRE(numbered.IsOk());
	CHECK(AssetManager::FindByPath("Textures/Wood 1.png") == numbered.GetValue()[0]);

	// Files already in the asset directory are registered in place.
	WritePng(fixture.GetAssetDirectory() / "Loose.png");
	std::vector<std::filesystem::path> const loose = {fixture.GetAssetDirectory() / "Loose.png"};
	Result<std::vector<AssetHandle>> inPlace = operations.ImportAssets(loose, "Textures");
	REQUIRE(inPlace.IsOk());
	CHECK(AssetManager::FindByPath("Loose.png") == inPlace.GetValue()[0]);

	std::vector<std::filesystem::path> const mixed = {outside / "Stone.png", outside / "Notes.txt"};
	CHECK(operations.ImportAssets(mixed, "Other").IsError());
	std::vector<std::filesystem::path> const missing = {outside / "Missing.png"};
	CHECK(operations.ImportAssets(missing, "Other").IsError());
	CHECK_FALSE(FileSystem::Exists(fixture.GetAssetDirectory() / "Other" / "Stone.png"));
	CHECK(operations.ImportAssets(both, "../Outside").IsError());
}

TEST_CASE("EditorOperations: material edits are undoable without marking the scene modified")
{
	ProjectFixture fixture;
	EditorOperations& operations = fixture.Operations;
	EditorContext& context = fixture.Context;
	Result<AssetHandle> created = operations.CreateMaterial("Rock.smat");
	REQUIRE(created.IsOk());
	AssetHandle const material = created.GetValue();
	CHECK_FALSE(context.IsDirty());
	size_t const steps = context.GetHistory().GetUndoCount();

	// One drag: merged into one step; the file follows every change.
	REQUIRE(operations.SetMaterialFields(material, Json::object({{"Roughness", 0.2}}), 77).IsOk());
	REQUIRE(operations.SetMaterialFields(material, Json::object({{"Roughness", 0.3}}), 77).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(context.GetHistory().GetUndoDescription() == "Edit Material");
	CHECK_FALSE(context.IsDirty());
	Result<MaterialData> onDisk =
		MaterialSerializer::LoadFromFile(fixture.GetAssetDirectory() / "Rock.smat", AssetManager::CreateDeserializationContext());
	REQUIRE(onDisk.IsOk());
	CHECK(onDisk.GetValue().Roughness == doctest::Approx(0.3f));

	// Scene edits after an asset edit still track the scene's saved state.
	UUID const entity = operations.CreateEntity(EntityCreateInfo{}).GetValue();
	CHECK(context.IsDirty());
	REQUIRE(operations.Undo().IsOk());
	CHECK_FALSE(context.GetScene().HasEntity(entity));
	CHECK_FALSE(context.IsDirty());

	REQUIRE(operations.Undo().IsOk());
	CHECK(Roughness(material) == doctest::Approx(0.5f));
	CHECK(MaterialSerializer::LoadFromFile(fixture.GetAssetDirectory() / "Rock.smat", AssetManager::CreateDeserializationContext())
	          .GetValue()
	          .Roughness == doctest::Approx(0.5f));
	CHECK_FALSE(context.IsDirty());
	REQUIRE(operations.Redo().IsOk());
	CHECK(Roughness(material) == doctest::Approx(0.3f));

	// Invalid values change nothing; materials without a file of their own are read-only.
	CHECK(operations.SetMaterialFields(material, Json::object({{"Roughness", 3.0}})).IsError());
	CHECK(Roughness(material) == doctest::Approx(0.3f));
	CHECK(operations.SetMaterialFields(GetBuiltInHandle(BuiltInAsset::DefaultMaterial), Json::object({{"Roughness", 0.1}})).IsError());
	CHECK(operations.SetMaterialFields(AssetHandle(UUID(424242)), Json::object()).IsError());
	// Setting a value it already has records nothing.
	size_t const recorded = context.GetHistory().GetUndoCount();
	CHECK(operations.SetMaterialFields(material, Json::object({{"Roughness", 0.3}})).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == recorded);
}

TEST_CASE("AssetBrowsing: paths split and join with forward slashes")
{
	CHECK(AssetBrowsing::JoinPath("", "Oak.png") == "Oak.png");
	CHECK(AssetBrowsing::JoinPath("Textures/Wood", "Oak.png") == "Textures/Wood/Oak.png");
	CHECK(AssetBrowsing::GetParentFolder("Textures/Wood/Oak.png") == "Textures/Wood");
	CHECK(AssetBrowsing::GetParentFolder("Oak.png").empty());
	CHECK(AssetBrowsing::GetFileName("Textures/Wood/Oak.png") == "Oak.png");
	CHECK(AssetBrowsing::GetFileName("Oak.png") == "Oak.png");
}

TEST_CASE("AssetBrowsing: folders and assets are listed per folder, sorted ignoring case, and searched")
{
	ProjectFixture fixture;
	EditorOperations& operations = fixture.Operations;
	REQUIRE(operations.CreateAssetFolder("textures").IsOk());
	REQUIRE(operations.CreateAssetFolder("Audio").IsOk());
	REQUIRE(operations.CreateAssetFolder("textures/Wood").IsOk());
	REQUIRE(FileSystem::CreateDirectories(fixture.GetAssetDirectory() / ".cache").IsOk());
	Result<AssetHandle> const rock = operations.CreateMaterial("textures/rock.smat");
	Result<AssetHandle> const bark = operations.CreateMaterial("textures/Bark.smat");
	Result<AssetHandle> const oak = operations.CreateMaterial("textures/Wood/Oak.smat");
	REQUIRE(rock.IsOk());
	REQUIRE(bark.IsOk());
	REQUIRE(oak.IsOk());

	// Hidden folders are skipped; the project's start scene lives in Scenes/.
	CHECK(AssetBrowsing::ListFolders("") == std::vector<std::string>{"Audio", "Scenes", "textures"});
	CHECK(AssetBrowsing::ListFolders("textures") == std::vector<std::string>{"textures/Wood"});
	CHECK(AssetBrowsing::ListFolders("Missing").empty());

	std::vector<AssetMetadata> const listed = AssetBrowsing::ListAssets("textures");
	REQUIRE(listed.size() == 2);
	CHECK(listed[0].Handle == bark.GetValue());
	CHECK(listed[1].Handle == rock.GetValue());
	CHECK(AssetBrowsing::ListAssets("").empty());

	std::vector<AssetMetadata> const found = AssetBrowsing::SearchAssets("OAK");
	REQUIRE(found.size() == 1);
	CHECK(found[0].Handle == oak.GetValue());
	// Built-in assets are not part of the asset directory.
	CHECK(AssetBrowsing::SearchAssets("builtin").empty());
	CHECK(AssetBrowsing::SearchAssets("smat").size() == 3);
}

TEST_CASE("AssetBrowsing: unique names skip files on disk and registered assets")
{
	ProjectFixture fixture;
	EditorOperations& operations = fixture.Operations;
	CHECK(AssetBrowsing::MakeUniqueName("", "New Material", ".smat") == "New Material.smat");
	REQUIRE(operations.CreateMaterial("New Material.smat").IsOk());
	CHECK(AssetBrowsing::MakeUniqueName("", "New Material", ".smat") == "New Material 1.smat");
	REQUIRE(FileSystem::WriteTextFile(fixture.GetAssetDirectory() / "New Material 1.smat", "{}").IsOk());
	CHECK(AssetBrowsing::MakeUniqueName("", "New Material", ".smat") == "New Material 2.smat");

	REQUIRE(operations.CreateAssetFolder("Art/New Folder").IsOk());
	CHECK(AssetBrowsing::MakeUniqueName("Art", "New Folder", "") == "New Folder 1");
	CHECK(AssetBrowsing::MakeUniqueName("", "New Folder", "") == "New Folder");
}

TEST_CASE("EditorContext: selecting an asset and selecting entities replace each other")
{
	ProjectFixture fixture;
	EditorContext& context = fixture.Context;
	EditorOperations& operations = fixture.Operations;
	Result<AssetHandle> const material = operations.CreateMaterial("Rock.smat");
	REQUIRE(material.IsOk());
	EntityCreateInfo info;
	info.Name = "Entity";
	Result<UUID> const entity = operations.CreateEntity(info);
	REQUIRE(entity.IsOk());
	std::vector<UUID> const entities = {entity.GetValue()};

	REQUIRE(operations.Select(entities).IsOk());
	context.SelectAsset(material.GetValue());
	CHECK(context.GetSelectedAsset() == material.GetValue());
	CHECK(context.GetSelection().IsEmpty());

	REQUIRE(operations.Select(entities).IsOk());
	CHECK_FALSE(context.GetSelectedAsset().IsValid());

	context.SelectAsset(material.GetValue());
	context.ClearSelection();
	CHECK_FALSE(context.GetSelectedAsset().IsValid());

	// Selecting no asset keeps the entity selection.
	REQUIRE(operations.Select(entities).IsOk());
	context.SelectAsset(AssetHandle());
	CHECK(context.GetSelection().Contains(entity.GetValue()));

	context.SelectAsset(material.GetValue());
	REQUIRE(operations.CloseProject().IsOk());
	CHECK_FALSE(context.GetSelectedAsset().IsValid());
}

TEST_CASE("EntityPresets: mesh assets become entities named after the file")
{
	ProjectFixture fixture;
	EditorContext& context = fixture.Context;
	EditorOperations& operations = fixture.Operations;
	AssetHandle const cube = GetBuiltInHandle(BuiltInAsset::CubeMesh);

	Result<UUID> const root = EntityPresets::CreateFromMesh(operations, cube, UUID::Invalid(), glm::vec3(1.0f, 0.0f, -2.0f));
	REQUIRE(root.IsOk());
	Entity const rootEntity = context.GetScene().GetEntityByUUID(root.GetValue());
	CHECK(rootEntity.GetName() == "Cube");
	CHECK(rootEntity.GetComponent<MeshComponent>().Mesh == cube);
	CHECK(rootEntity.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 0.0f, -2.0f));

	// Children start at their parent's origin.
	Result<UUID> const child = EntityPresets::CreateFromMesh(operations, cube, root.GetValue(), glm::vec3(5.0f));
	REQUIRE(child.IsOk());
	Entity const childEntity = context.GetScene().GetEntityByUUID(child.GetValue());
	CHECK(context.GetScene().GetParent(childEntity).GetUUID() == root.GetValue());
	CHECK(childEntity.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));

	size_t const steps = context.GetHistory().GetUndoCount();
	CHECK(EntityPresets::CreateFromMesh(operations, GetBuiltInHandle(BuiltInAsset::DefaultMaterial), UUID::Invalid(), glm::vec3(0.0f))
	          .IsError());
	CHECK(EntityPresets::CreateFromMesh(operations, AssetHandle(), UUID::Invalid(), glm::vec3(0.0f)).IsError());
	CHECK(context.GetHistory().GetUndoCount() == steps);
}

TEST_CASE("AssetDrops: materials are assigned to every submesh as one undo step")
{
	ProjectFixture fixture;
	EditorContext& context = fixture.Context;
	EditorOperations& operations = fixture.Operations;
	Result<AssetHandle> const material = operations.CreateMaterial("Rock.smat");
	REQUIRE(material.IsOk());
	EntityCreateInfo meshInfo;
	meshInfo.Name = "Cube";
	meshInfo.Components = Json::object({{"Mesh", {{"Mesh", GetBuiltInHandle(BuiltInAsset::CubeMesh).ToString()}}}});
	Result<UUID> const mesh = operations.CreateEntity(meshInfo);
	EntityCreateInfo emptyInfo;
	emptyInfo.Name = "Empty";
	Result<UUID> const empty = operations.CreateEntity(emptyInfo);
	REQUIRE(mesh.IsOk());
	REQUIRE(empty.IsOk());

	size_t const steps = context.GetHistory().GetUndoCount();
	REQUIRE(AssetDrops::AssignMaterial(operations, mesh.GetValue(), material.GetValue()).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);
	auto const materials = [&context, &mesh]
	{
		return context.GetScene().GetEntityByUUID(mesh.GetValue()).GetComponent<MeshComponent>().Materials;
	};
	CHECK(materials() == std::vector<AssetHandle>{material.GetValue()});

	CHECK(AssetDrops::AssignMaterial(operations, empty.GetValue(), material.GetValue()).IsError());
	CHECK(AssetDrops::AssignMaterial(operations, UUID(12345), material.GetValue()).IsError());
	CHECK(AssetDrops::AssignMaterial(operations, mesh.GetValue(), GetBuiltInHandle(BuiltInAsset::WhiteTexture)).IsError());
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);

	REQUIRE(operations.Undo().IsOk());
	CHECK(materials().empty());
}

TEST_CASE("AssetDrops: environments go to the first sky light, or a new one")
{
	ProjectFixture fixture;
	EditorContext& context = fixture.Context;
	EditorOperations& operations = fixture.Operations;
	AssetHandle const sky = GetBuiltInHandle(BuiltInAsset::DefaultSky);
	auto const countSkyLights = [&context]
	{
		size_t count = 0;
		context.GetScene().ForEachEntityInHierarchyOrder(
			[&count](Entity entity)
			{
				count += entity.HasComponent<SkyLightComponent>() ? 1 : 0;
			});
		return count;
	};
	REQUIRE(countSkyLights() == 0);
	CHECK(AssetDrops::SetSkyEnvironment(operations, GetBuiltInHandle(BuiltInAsset::CubeMesh)).IsError());
	Result<UUID> const created = AssetDrops::SetSkyEnvironment(operations, sky);
	REQUIRE(created.IsOk());
	CHECK(countSkyLights() == 1);
	Entity const light = context.GetScene().GetEntityByUUID(created.GetValue());
	CHECK(light.GetName() == "Sky Light");
	CHECK(light.GetComponent<SkyLightComponent>().Environment == sky);
	REQUIRE(operations.Undo().IsOk());
	CHECK(countSkyLights() == 0);

	EntityCreateInfo first;
	first.Name = "First";
	first.Components = Json::object({{"SkyLight", Json::object({{"Environment", UUID::Invalid().ToString()}})}});
	EntityCreateInfo second = first;
	second.Name = "Second";
	Result<UUID> const firstLight = operations.CreateEntity(first);
	Result<UUID> const secondLight = operations.CreateEntity(second);
	REQUIRE(firstLight.IsOk());
	REQUIRE(secondLight.IsOk());
	Result<UUID> const updated = AssetDrops::SetSkyEnvironment(operations, sky);
	REQUIRE(updated.IsOk());
	CHECK(updated.GetValue() == firstLight.GetValue());
	CHECK(context.GetScene().GetEntityByUUID(firstLight.GetValue()).GetComponent<SkyLightComponent>().Environment == sky);
	CHECK_FALSE(context.GetScene().GetEntityByUUID(secondLight.GetValue()).GetComponent<SkyLightComponent>().Environment.IsValid());
}

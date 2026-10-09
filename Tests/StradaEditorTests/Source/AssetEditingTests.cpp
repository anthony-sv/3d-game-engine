#include "TestUtilities.h"

#include "Editor/EditorOperations.h"

#include "Strada/Asset/AssetManager.h"
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
	operations.CloseProject();
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

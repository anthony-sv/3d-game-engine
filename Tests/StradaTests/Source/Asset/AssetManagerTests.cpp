#include "Asset/GltfTestBuilder.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Asset/AudioClipAsset.h"
#include "Strada/Asset/EnvironmentAsset.h"
#include "Strada/Asset/FontAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <chrono>

using namespace Strada;
using Strada::Testing::GltfTestBuilder;

namespace
{
	// Initializes the AssetManager for one test and always shuts it down.
	class AssetManagerScope
	{
	public:
		AssetManagerScope() { AssetManager::Init(); }
		~AssetManagerScope()
		{
			if (AssetManager::IsInitialized())
			{
				AssetManager::Shutdown();
			}
		}

		AssetManagerScope(AssetManagerScope const&) = delete;
		AssetManagerScope& operator=(AssetManagerScope const&) = delete;
	};

	void WritePng(std::filesystem::path const& path, uint8_t red)
	{
		Image image(2, 2, 4);
		image.GetPixel(0, 0)[0] = red;
		REQUIRE(image.WritePNG(path).IsOk());
	}

	void WriteBytes(std::filesystem::path const& path, std::vector<uint8_t> const& bytes)
	{
		REQUIRE(FileSystem::WriteBinaryFile(path, bytes).IsOk());
	}

	std::vector<uint8_t> MakeWav()
	{
		return {'R', 'I',  'F',  'F', 37, 0,    0,    0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16,  0, 0, 0, 1, 0,  1,
		        0,   0x44, 0xAC, 0,   0,  0x44, 0xAC, 0, 0,   1,   0,   8,   0,   'd', 'a', 't', 'a', 1, 0, 0, 0, 128};
	}

	// A small project asset directory with one asset of every loadable type.
	struct TestProject
	{
		Testing::TemporaryDirectory Directory;
		std::filesystem::path Assets;

		TestProject()
			: Assets(Directory.GetPath() / "Assets")
		{
			for (char const* folder : {"Textures", "Materials", "Environments", "Audio", "Fonts", "Scenes", ".hidden"})
			{
				REQUIRE(FileSystem::CreateDirectories(Assets / folder).IsOk());
			}
			WritePng(Assets / "Textures" / "Wood.png", 120);
			WritePng(Assets / ".hidden" / "Secret.png", 1);
			REQUIRE(FileSystem::WriteTextFile(Assets / "Readme.txt", "not an asset").IsOk());

			Json material = Json::object();
			material["Strada"] = MakeFileHeader("Material", MaterialSerializer::FormatVersion);
			material["Material"] = Json::object({{"BaseColorTexture", "asset://Textures/Wood.png"}, {"Roughness", 0.8}});
			REQUIRE(FileSystem::WriteTextFile(Assets / "Materials" / "Wood.smat", DumpJson(material)).IsOk());

			HdrImage sky(4, 2, 3);
			REQUIRE(sky.WriteHDR(Assets / "Environments" / "Sky.hdr").IsOk());
			WriteBytes(Assets / "Audio" / "Click.wav", MakeWav());
			REQUIRE(FileSystem::Copy(STRADA_TEST_FONT_PATH, Assets / "Fonts" / "Karla.ttf", false).IsOk());

			Scene scene("Level");
			scene.CreateEntity("Camera").AddComponent<CameraComponent>();
			REQUIRE(SceneSerializer::SaveToFile(scene, Assets / "Scenes" / "Level.sscene").IsOk());
		}
	};

	AssetHandle Find(std::string_view path)
	{
		AssetHandle const handle = AssetManager::FindByPath(path);
		REQUIRE_MESSAGE(handle.IsValid(), path);
		return handle;
	}
}

TEST_CASE("AssetManager: built-in assets are always available")
{
	AssetManagerScope scope;
	for (BuiltInAsset const asset : GetDefaultBuiltInAssets())
	{
		AssetHandle const handle = GetBuiltInHandle(asset);
		std::string const name(GetBuiltInAssetName(asset));
		CAPTURE(name);
		CHECK(handle.IsReserved());
		CHECK(AssetManager::IsValid(handle));
		CHECK(AssetManager::IsLoaded(handle));
		CHECK(AssetManager::FindBuiltIn(name) == handle);
		CHECK(AssetManager::ResolveReference("builtin://" + name).GetValue() == handle);
		CHECK(AssetManager::GetReference(handle) == "builtin://" + name);
		std::optional<AssetMetadata> const metadata = AssetManager::GetMetadata(handle);
		REQUIRE(metadata);
		CHECK(metadata->IsBuiltIn());
		CHECK(metadata->GetDisplayName() == name);
		CHECK(AssetManager::LoadAsset(handle).IsOk());
	}

	Ref<MeshSource> const cube = AssetManager::GetAsset<MeshSource>(GetBuiltInHandle(BuiltInAsset::CubeMesh));
	REQUIRE(cube);
	CHECK(cube->GetMaterials()[0] == GetBuiltInHandle(BuiltInAsset::DefaultMaterial));
	CHECK(AssetManager::GetAsset<MaterialAsset>(GetBuiltInHandle(BuiltInAsset::DefaultMaterial)));
	CHECK(AssetManager::GetAsset<TextureAsset>(GetBuiltInHandle(BuiltInAsset::FlatNormalTexture))->Decode().GetValue().GetPixel(0, 0)[2] ==
	      255);
	CHECK(AssetManager::GetAssets(AssetType::Mesh).size() == 7);
	CHECK(AssetManager::GetAssets().size() == GetDefaultBuiltInAssets().size());

	CHECK(AssetManager::RegisterBuiltInAsset("Cube", AssetHandle(UUID(900)), CreateRef<MaterialAsset>()).IsError());
	CHECK(AssetManager::RegisterBuiltInAsset("Extra", GetBuiltInHandle(BuiltInAsset::CubeMesh), CreateRef<MaterialAsset>()).IsError());
	CHECK(AssetManager::RegisterBuiltInAsset("Extra", AssetHandle(UUID(5000)), CreateRef<MaterialAsset>()).IsError());
	REQUIRE(AssetManager::RegisterBuiltInAsset("Extra", AssetHandle(UUID(900)), CreateRef<MaterialAsset>()).IsOk());
	CHECK(AssetManager::ResolveReference("builtin://Extra").GetValue() == AssetHandle(UUID(900)));
	CHECK(AssetManager::ResolveReference("builtin://Nope").IsError());

	// Shutdown leaves nothing behind; the manager can be initialized again.
	AssetManager::Shutdown();
	CHECK_FALSE(AssetManager::IsInitialized());
	AssetManager::Init();
	CHECK_FALSE(AssetManager::FindBuiltIn("Extra").IsValid());
}

TEST_CASE("AssetManager: opening a directory registers files and keeps handles stable")
{
	AssetManagerScope scope;
	TestProject project;

	Result<AssetRefreshResult> opened = AssetManager::OpenAssetDirectory(project.Assets);
	REQUIRE_MESSAGE(opened.IsOk(), opened.GetError());
	CHECK(AssetManager::HasAssetDirectory());
	CHECK(opened.GetValue().Added.size() == 6);
	CHECK(opened.GetValue().Missing.empty());
	CHECK(FileSystem::IsRegularFile(project.Assets / AssetRegistry::FileName));
	CHECK_FALSE(AssetManager::FindByPath(".hidden/Secret.png").IsValid());

	AssetHandle const wood = Find("Textures/Wood.png");
	CHECK(AssetManager::GetAssetType(wood) == AssetType::Texture);
	CHECK(AssetManager::GetAssetType(Find("Scenes/Level.sscene")) == AssetType::Scene);
	CHECK(AssetManager::GetReference(wood) == "asset://Textures/Wood.png");
	CHECK(AssetManager::GetAbsolutePath(wood) == project.Assets / "Textures" / "Wood.png");
	CHECK(AssetManager::GetAssets(AssetType::Texture).size() == 4);

	AssetManager::CloseAssetDirectory();
	CHECK_FALSE(AssetManager::HasAssetDirectory());
	CHECK_FALSE(AssetManager::IsValid(wood));

	Result<AssetRefreshResult> reopened = AssetManager::OpenAssetDirectory(project.Assets);
	REQUIRE(reopened.IsOk());
	CHECK(reopened.GetValue().Added.empty());
	CHECK(AssetManager::FindByPath("Textures/Wood.png") == wood);
	CHECK(AssetManager::FindByPath("Textures\\Wood.png") == wood);
}

TEST_CASE("AssetManager: assets load on first use with their types")
{
	AssetManagerScope scope;
	TestProject project;
	REQUIRE(AssetManager::OpenAssetDirectory(project.Assets).IsOk());

	AssetHandle const wood = Find("Textures/Wood.png");
	CHECK_FALSE(AssetManager::IsLoaded(wood));
	Ref<TextureAsset> const texture = AssetManager::GetAsset<TextureAsset>(wood);
	REQUIRE(texture);
	CHECK(texture->Handle == wood);
	CHECK(AssetManager::IsLoaded(wood));
	CHECK(AssetManager::GetAsset<TextureAsset>(wood) == texture);

	Ref<MaterialAsset> const material = AssetManager::GetAsset<MaterialAsset>(Find("Materials/Wood.smat"));
	REQUIRE(material);
	CHECK(material->GetData().BaseColorTexture == wood);
	CHECK(material->GetData().Roughness == doctest::Approx(0.8f));

	CHECK(AssetManager::GetAsset<EnvironmentAsset>(Find("Environments/Sky.hdr")));
	CHECK(AssetManager::GetAsset<AudioClipAsset>(Find("Audio/Click.wav")));
	CHECK(AssetManager::GetAsset<FontAsset>(Find("Fonts/Karla.ttf")));

	CHECK_FALSE(AssetManager::GetAsset<MaterialAsset>(wood));
	Result<Ref<MaterialAsset>> const wrongType = AssetManager::TryGetAsset<MaterialAsset>(wood);
	REQUIRE(wrongType.IsError());
	CHECK(wrongType.GetError().find("is a Texture, not a Material") != std::string::npos);
	CHECK(AssetManager::LoadAsset(Find("Scenes/Level.sscene")).IsError());
	CHECK(AssetManager::LoadAsset(AssetHandle()).IsError());
	CHECK(AssetManager::LoadAsset(AssetHandle(UUID(123456789))).IsError());

	AssetManager::UnloadAsset(wood);
	CHECK_FALSE(AssetManager::IsLoaded(wood));
	CHECK(AssetManager::GetAsset<TextureAsset>(wood) != texture);
}

TEST_CASE("AssetManager: meshes create sub-assets and share registered textures")
{
	AssetManagerScope scope;
	TestProject project;
	REQUIRE(FileSystem::CreateDirectories(project.Assets / "Models").IsOk());

	GltfTestBuilder builder;
	int const shared = builder.AddTexture(builder.AddImageUri("../Textures/Wood.png"));
	Image normalImage(1, 1, 4);
	REQUIRE(normalImage.WritePNG(project.Directory.GetPath() / "Normal.png").IsOk());
	Result<Buffer> const normalFile = FileSystem::ReadBinaryFile(project.Directory.GetPath() / "Normal.png");
	REQUIRE(normalFile.IsOk());
	std::span<uint8_t const> const normalBytes = normalFile.GetValue().GetSpan();
	int const embedded = builder.AddTexture(builder.AddEmbeddedImage(std::vector<uint8_t>(normalBytes.begin(), normalBytes.end())));
	Json material = Json::object();
	material["name"] = "Crate";
	material["pbrMetallicRoughness"] = Json::object({{"baseColorTexture", Json::object({{"index", shared}})}});
	material["normalTexture"] = Json::object({{"index", embedded}});

	GltfTestBuilder::Primitive quad;
	quad.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	quad.TexCoords = {{0, 0}, {1, 0}, {0, 1}};
	quad.Material = builder.AddMaterial(material);
	builder.SetScene({builder.AddNode("Crate", builder.AddMesh("Crate", {quad}))});
	REQUIRE(builder.WriteGltf(project.Assets / "Models" / "Crate.gltf", true).IsOk());

	REQUIRE(AssetManager::OpenAssetDirectory(project.Assets).IsOk());
	AssetHandle const crate = Find("Models/Crate.gltf");
	Ref<MeshSource> const mesh = AssetManager::GetAsset<MeshSource>(crate);
	REQUIRE(mesh);
	REQUIRE(mesh->GetMaterials().size() == 1);

	AssetHandle const materialHandle = mesh->GetMaterials()[0];
	std::optional<AssetMetadata> const materialMetadata = AssetManager::GetMetadata(materialHandle);
	REQUIRE(materialMetadata);
	CHECK(materialMetadata->IsSubAsset());
	CHECK(materialMetadata->Parent == crate);
	CHECK(materialMetadata->Name == "Crate.gltf/Materials/Crate");

	Ref<MaterialAsset> const crateMaterial = AssetManager::GetAsset<MaterialAsset>(materialHandle);
	REQUIRE(crateMaterial);
	CHECK(crateMaterial->GetData().BaseColorTexture == Find("Textures/Wood.png"));
	AssetHandle const normalHandle = crateMaterial->GetData().NormalTexture;
	std::optional<AssetMetadata> const normalMetadata = AssetManager::GetMetadata(normalHandle);
	REQUIRE(normalMetadata);
	CHECK(normalMetadata->Parent == crate);
	CHECK(AssetManager::GetAsset<TextureAsset>(normalHandle));

	// Reloading hands out the same sub-asset handles with fresh objects.
	REQUIRE(AssetManager::ReloadAsset(materialHandle).IsOk());
	Ref<MeshSource> const reloaded = AssetManager::GetAsset<MeshSource>(crate);
	CHECK(reloaded != mesh);
	CHECK(reloaded->GetMaterials()[0] == materialHandle);
	CHECK(AssetManager::GetAsset<MaterialAsset>(materialHandle) != crateMaterial);

	AssetManager::UnloadAsset(crate);
	CHECK_FALSE(AssetManager::IsValid(materialHandle));
	CHECK_FALSE(AssetManager::IsValid(normalHandle));
}

TEST_CASE("AssetManager: refresh detects new, missing and modified files")
{
	AssetManagerScope scope;
	TestProject project;
	REQUIRE(AssetManager::OpenAssetDirectory(project.Assets).IsOk());

	WritePng(project.Assets / "Textures" / "Stone.png", 5);
	AssetHandle const wood = Find("Textures/Wood.png");
	REQUIRE(AssetManager::GetAsset<TextureAsset>(wood));

	// Rewrite the loaded texture with a different size and a later timestamp.
	Image larger(4, 4, 4);
	REQUIRE(larger.WritePNG(project.Assets / "Textures" / "Wood.png").IsOk());
	std::error_code errorCode;
	std::filesystem::path const woodPath = project.Assets / "Textures" / "Wood.png";
	std::filesystem::last_write_time(woodPath, std::filesystem::last_write_time(woodPath) + std::chrono::seconds(5), errorCode);
	REQUIRE_FALSE(errorCode);

	AssetHandle const clip = Find("Audio/Click.wav");
	REQUIRE(FileSystem::Remove(project.Assets / "Audio" / "Click.wav").IsOk());

	Result<AssetRefreshResult> refreshed = AssetManager::Refresh();
	REQUIRE(refreshed.IsOk());
	REQUIRE(refreshed.GetValue().Added.size() == 1);
	CHECK(refreshed.GetValue().Added[0] == Find("Textures/Stone.png"));
	CHECK(refreshed.GetValue().Missing == std::vector<AssetHandle>{clip});
	CHECK(refreshed.GetValue().Modified == std::vector<AssetHandle>{wood});

	CHECK(AssetManager::IsMissing(clip));
	CHECK(AssetManager::IsValid(clip));
	Result<Ref<Asset>> const missing = AssetManager::LoadAsset(clip);
	REQUIRE(missing.IsError());
	CHECK(missing.GetError().find("missing") != std::string::npos);
	CHECK_FALSE(AssetManager::IsLoaded(wood));
	CHECK(AssetManager::GetAsset<TextureAsset>(wood)->GetWidth() == 4);

	// The file coming back restores the same handle.
	WriteBytes(project.Assets / "Audio" / "Click.wav", MakeWav());
	REQUIRE(AssetManager::Refresh().IsOk());
	CHECK_FALSE(AssetManager::IsMissing(clip));
	CHECK(AssetManager::GetAsset<AudioClipAsset>(clip));
}

TEST_CASE("AssetManager: failed loads are remembered until a refresh")
{
	AssetManagerScope scope;
	TestProject project;
	WriteBytes(project.Assets / "Textures" / "Broken.png", {1, 2, 3, 4, 5});
	REQUIRE(AssetManager::OpenAssetDirectory(project.Assets).IsOk());

	AssetHandle const broken = Find("Textures/Broken.png");
	Result<Ref<Asset>> const first = AssetManager::LoadAsset(broken);
	REQUIRE(first.IsError());
	CHECK(first.GetError().find("Textures/Broken.png") != std::string::npos);

	WritePng(project.Assets / "Textures" / "Broken.png", 9);
	CHECK(AssetManager::LoadAsset(broken).GetError() == first.GetError());
	REQUIRE(AssetManager::Refresh().IsOk());
	CHECK(AssetManager::GetAsset<TextureAsset>(broken));
}

TEST_CASE("AssetManager: moving, deleting and importing keep the registry consistent")
{
	AssetManagerScope scope;
	TestProject project;
	REQUIRE(AssetManager::OpenAssetDirectory(project.Assets).IsOk());
	AssetHandle const wood = Find("Textures/Wood.png");

	// Materials saved by the engine reference textures by handle.
	MaterialData byHandle;
	byHandle.BaseColorTexture = wood;
	REQUIRE(MaterialSerializer::SaveToFile(byHandle, project.Assets / "Materials" / "Saved.smat").IsOk());
	Result<AssetHandle> const saved = AssetManager::ImportFile("Materials/Saved.smat");
	REQUIRE(saved.IsOk());

	REQUIRE(AssetManager::MoveAsset(wood, "Textures/Hardwood/Oak.png").IsOk());
	CHECK(FileSystem::IsRegularFile(project.Assets / "Textures" / "Hardwood" / "Oak.png"));
	CHECK_FALSE(FileSystem::Exists(project.Assets / "Textures" / "Wood.png"));
	CHECK(AssetManager::FindByPath("Textures/Hardwood/Oak.png") == wood);
	CHECK_FALSE(AssetManager::FindByPath("Textures/Wood.png").IsValid());

	// References by handle survive the move; path references ("asset://Textures/Wood.png") do not.
	Ref<MaterialAsset> const savedMaterial = AssetManager::GetAsset<MaterialAsset>(saved.GetValue());
	REQUIRE(savedMaterial);
	CHECK(savedMaterial->GetData().BaseColorTexture == wood);
	CHECK(AssetManager::LoadAsset(Find("Materials/Wood.smat")).IsError());

	CHECK(AssetManager::MoveAsset(wood, "Textures/Oak.hdr").IsError());
	CHECK(AssetManager::MoveAsset(wood, "Materials/Wood.smat").IsError());
	CHECK(AssetManager::MoveAsset(wood, "../Outside.png").IsError());
	CHECK(AssetManager::MoveAsset(GetBuiltInHandle(BuiltInAsset::WhiteTexture), "White.png").IsError());

	// The registry on disk follows.
	AssetManager::CloseAssetDirectory();
	REQUIRE(AssetManager::OpenAssetDirectory(project.Assets).IsOk());
	CHECK(AssetManager::FindByPath("Textures/Hardwood/Oak.png") == wood);

	REQUIRE(AssetManager::DeleteAsset(wood).IsOk());
	CHECK_FALSE(AssetManager::IsValid(wood));
	CHECK_FALSE(FileSystem::Exists(project.Assets / "Textures" / "Hardwood" / "Oak.png"));
	CHECK(AssetManager::DeleteAsset(wood).IsError());

	WritePng(project.Assets / "Textures" / "New.png", 3);
	Result<AssetHandle> const imported = AssetManager::ImportFile("Textures/New.png");
	REQUIRE(imported.IsOk());
	CHECK(AssetManager::ImportFile(project.Assets / "Textures" / "New.png").GetValue() == imported.GetValue());
	CHECK(AssetManager::ImportFile(project.Directory.GetPath() / "Outside.png").IsError());
	CHECK(AssetManager::ImportFile("Readme.txt").IsError());
	CHECK(AssetManager::ImportFile("Textures/DoesNotExist.png").IsError());
}

TEST_CASE("AssetManager: memory assets live until removed or the directory closes")
{
	AssetManagerScope scope;
	TestProject project;
	REQUIRE(AssetManager::OpenAssetDirectory(project.Assets).IsOk());

	MaterialData data;
	data.BaseColor = {0.0f, 1.0f, 0.0f, 1.0f};
	AssetHandle const handle = AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(data), "Runtime Green");
	CHECK(handle.IsValid());
	CHECK_FALSE(handle.IsReserved());
	CHECK(AssetManager::GetMetadata(handle)->IsMemoryAsset());
	CHECK(AssetManager::GetMetadata(handle)->GetDisplayName() == "Runtime Green");
	CHECK(AssetManager::GetAsset<MaterialAsset>(handle)->GetData().BaseColor.g == 1.0f);
	CHECK(AssetManager::GetReference(handle) == handle.ToString());
	CHECK(AssetManager::ResolveReference(handle.ToString()).GetValue() == handle);
	CHECK(AssetManager::ReloadAsset(handle).IsError());

	AssetManager::RemoveMemoryAsset(handle);
	CHECK_FALSE(AssetManager::IsValid(handle));
	// Built-ins are not memory assets and cannot be removed this way.
	AssetManager::RemoveMemoryAsset(GetBuiltInHandle(BuiltInAsset::CubeMesh));
	CHECK(AssetManager::IsValid(GetBuiltInHandle(BuiltInAsset::CubeMesh)));

	AssetHandle const another = AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(), "Temporary");
	AssetManager::CloseAssetDirectory();
	CHECK_FALSE(AssetManager::IsValid(another));
	CHECK(AssetManager::IsValid(GetBuiltInHandle(BuiltInAsset::CubeMesh)));
}

TEST_CASE("AssetManager: references resolve in component data")
{
	AssetManagerScope scope;
	TestProject project;
	REQUIRE(AssetManager::OpenAssetDirectory(project.Assets).IsOk());

	CHECK(AssetManager::ResolveReference("asset://Textures/Wood.png").GetValue() == Find("Textures/Wood.png"));
	CHECK(AssetManager::ResolveReference("asset://Textures/Missing.png").IsError());
	CHECK(AssetManager::ResolveReference("asset://../Escape.png").IsError());
	CHECK(AssetManager::ResolveReference("98765").IsError());
	CHECK(AssetManager::ResolveReference("not a reference").IsError());

	Scene scene;
	Entity entity = scene.CreateEntity();
	Json mesh = Json::object({{"Mesh", "builtin://Sphere"}, {"Materials", Json::array({"asset://Materials/Wood.smat"})}});
	Result<void> const result = ComponentRegistry::Get<MeshComponent>().Deserialize(scene.GetRegistry(), entity.GetHandle(), mesh,
	                                                                                AssetManager::CreateDeserializationContext());
	REQUIRE_MESSAGE(result.IsOk(), result.GetError());
	CHECK(entity.GetComponent<MeshComponent>().Mesh == GetBuiltInHandle(BuiltInAsset::SphereMesh));
	CHECK(entity.GetComponent<MeshComponent>().Materials == std::vector<AssetHandle>{Find("Materials/Wood.smat")});
}

TEST_CASE("AssetManager: invalid directories and corrupt registries fail to open")
{
	AssetManagerScope scope;
	TestProject project;
	CHECK(AssetManager::OpenAssetDirectory(project.Directory.GetPath() / "Nope").IsError());
	CHECK(AssetManager::Refresh().IsError());

	REQUIRE(FileSystem::WriteTextFile(project.Assets / AssetRegistry::FileName, "{ corrupt").IsOk());
	Result<AssetRefreshResult> const result = AssetManager::OpenAssetDirectory(project.Assets);
	REQUIRE(result.IsError());
	CHECK(result.GetError().find("asset registry") != std::string::npos);
	CHECK_FALSE(AssetManager::HasAssetDirectory());
}

#include "Asset/GltfTestBuilder.h"
#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Strada/Asset/MeshImporter.h"
#include "Strada/Core/Image.h"

#include <doctest/doctest.h>

#include <array>
#include <span>
#include <string>
#include <vector>

using namespace Strada;
using Strada::Testing::GltfTestBuilder;

namespace
{
	struct ModelSeed
	{
		std::string Extension;
		std::vector<uint8_t> Data;
		// An OBJ's material library, written next to it as Model.mtl.
		std::vector<uint8_t> MaterialLibrary;
	};

	std::vector<uint8_t> ReadFile(std::filesystem::path const& path)
	{
		Result<Buffer> data = FileSystem::ReadBinaryFile(path);
		REQUIRE_MESSAGE(data.IsOk(), FileSystem::PathToUtf8(path));
		std::span<uint8_t const> const bytes = data.GetValue().GetSpan();
		return std::vector<uint8_t>(bytes.begin(), bytes.end());
	}

	// A GLB with every glTF feature the importer reads: indexed and non-indexed primitives, a strip, a material with an
	// embedded texture, and a node hierarchy.
	std::vector<uint8_t> MakeGlbSeed(Testing::TemporaryDirectory const& directory)
	{
		Image image(2, 2, 4);
		Result<std::vector<uint8_t>> png = image.EncodePNG();
		REQUIRE(png.IsOk());

		GltfTestBuilder builder;
		int const texture = builder.AddTexture(builder.AddEmbeddedImage(png.GetValue()));
		Json material = Json::object();
		material["pbrMetallicRoughness"] = Json::object({{"baseColorTexture", Json::object({{"index", texture}})}});
		int const materialIndex = builder.AddMaterial(std::move(material));

		GltfTestBuilder::Primitive quad;
		quad.Positions = {{-0.5f, 0.5f, 0.0f}, {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}};
		quad.Normals.assign(4, glm::vec3(0.0f, 0.0f, 1.0f));
		quad.TexCoords = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
		quad.Indices = {0, 1, 2, 0, 2, 3};
		quad.Material = materialIndex;
		GltfTestBuilder::Primitive strip;
		strip.Positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
		strip.Mode = 5;

		int const child = builder.AddNode("Child", builder.AddMesh("Strip", {strip}), glm::vec3(1.0f, 2.0f, 3.0f));
		int const root = builder.AddNode("Root", builder.AddMesh("Quad", {quad}), glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
		                                 glm::vec3(2.0f), {child});
		builder.SetScene({root});
		std::filesystem::path const path = directory.GetPath() / "Seed.glb";
		REQUIRE(builder.WriteGlb(path).IsOk());
		return ReadFile(path);
	}

	void Damage(std::vector<uint8_t>& data, Testing::FuzzRandom& random)
	{
		for (size_t count = 1 + random.Pick(4); count > 0 && data.size() > 1; count--)
		{
			switch (random.Pick(5))
			{
				case 0:
					data.resize(1 + random.Pick(data.size()));
					break;
				case 1:
					data[random.Pick(data.size())] = static_cast<uint8_t>(random.Next());
					break;
				case 2:
					// Binary headers: magic numbers, versions, chunk sizes.
					data[random.Pick(std::min<size_t>(data.size(), 64))] = std::array<uint8_t, 4>{0x00, 0x7F, 0x80, 0xFF}[random.Pick(4)];
					break;
				case 3:
					// Numbers in text formats (OBJ, ASCII FBX, glTF JSON): counts, indices and sizes.
					data[random.Pick(data.size())] = std::array<uint8_t, 4>{'9', '-', '0', ' '}[random.Pick(4)];
					break;
				default:
					data[random.Pick(data.size())] ^= static_cast<uint8_t>(1u << random.Pick(8));
					break;
			}
		}
	}

	// What every consumer of an import relies on: MeshSource accepts the geometry, and every reference is in range.
	void CheckImportedModel(ImportedModel const& model)
	{
		REQUIRE(!model.Submeshes.empty());
		REQUIRE(!model.Materials.empty());
		std::vector<AssetHandle> materials;
		for (size_t index = 0; index < model.Materials.size(); index++)
		{
			materials.push_back(AssetHandle(UUID(index + 1)));
			ImportedMaterial const& material = model.Materials[index];
			for (int32_t const texture : {material.BaseColorTexture, material.NormalTexture, material.MetallicRoughnessTexture,
			                              material.OcclusionTexture, material.EmissiveTexture})
			{
				CHECK(texture >= -1);
				CHECK(texture < static_cast<int32_t>(model.Textures.size()));
			}
		}
		CHECK(model.Vertices.size() <= MeshImporter::MaxVertices);
		Result<Ref<MeshSource>> mesh = MeshSource::Create(model.Vertices, model.Indices, model.Submeshes, materials);
		CHECK_MESSAGE(mesh.IsOk(), (mesh ? std::string() : mesh.GetError()));
	}
}

TEST_CASE("MeshImporter: damaged model files are refused or import to valid meshes")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const ufbxData = FileSystem::PathFromUtf8(STRADA_TEST_UFBX_DATA_DIR);
	std::vector<ModelSeed> const seeds = {
		{".glb", MakeGlbSeed(directory), {}},
		{".glb", ReadFile(FileSystem::PathFromUtf8(STRADA_TEST_CGLTF_DATA_DIR) / "Box.glb"), {}},
		{".fbx", ReadFile(ufbxData / "blender_279_ball_7400_binary.fbx"), {}},
		{".fbx", ReadFile(ufbxData / "maya_cube_7500_ascii.fbx"), {}},
		{".obj", ReadFile(ufbxData / "blender_279_ball_0_obj.obj"), ReadFile(ufbxData / "blender_279_ball_0_obj.mtl")},
	};
	for (ModelSeed const& seed : seeds)
	{
		std::filesystem::path const path = directory.GetPath() / ("Seed" + seed.Extension);
		REQUIRE(FileSystem::WriteBinaryFile(path, seed.Data).IsOk());
		if (!seed.MaterialLibrary.empty())
		{
			// The seed OBJ names its library after the original file.
			REQUIRE(FileSystem::WriteBinaryFile(directory.GetPath() / "blender_279_ball_0_obj.mtl", seed.MaterialLibrary).IsOk());
		}
		Result<ImportedModel> imported = MeshImporter::Import(path);
		REQUIRE_MESSAGE(imported.IsOk(), seed.Extension, ": ", (imported ? std::string() : imported.GetError()));
		CheckImportedModel(imported.GetValue());
	}

	Testing::FuzzRandom random(8000);
	int imported = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(200); round < rounds; round++)
	{
		CAPTURE(round);
		ModelSeed seed = seeds[random.Pick(seeds.size())];
		// OBJ rounds damage the model or its material library.
		bool const damageLibrary = !seed.MaterialLibrary.empty() && random.Pick(3) == 0;
		Damage(damageLibrary ? seed.MaterialLibrary : seed.Data, random);
		std::filesystem::path const path = directory.GetPath() / ("Model" + seed.Extension);
		REQUIRE(FileSystem::WriteBinaryFile(path, seed.Data).IsOk());
		if (!seed.MaterialLibrary.empty())
		{
			REQUIRE(FileSystem::WriteBinaryFile(directory.GetPath() / "blender_279_ball_0_obj.mtl", seed.MaterialLibrary).IsOk());
		}

		Result<ImportedModel> model = MeshImporter::Import(path);
		if (!model)
		{
			refused++;
			continue;
		}
		imported++;
		CheckImportedModel(model.GetValue());
	}
	CHECK(imported > 0);
	CHECK(refused > 0);
}

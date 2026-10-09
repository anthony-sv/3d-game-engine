#include "Asset/GltfTestBuilder.h"
#include "TestUtilities.h"

#include "Strada/Asset/MeshImporter.h"
#include "Strada/Core/Image.h"

#include <doctest/doctest.h>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

using namespace Strada;
using Strada::Testing::GltfTestBuilder;

namespace
{
	// Unit quad in XY facing +Z (counter-clockwise), UV origin at the top-left.
	GltfTestBuilder::Primitive MakeQuad(bool withNormals = true, bool withTexCoords = true)
	{
		GltfTestBuilder::Primitive primitive;
		primitive.Positions = {{-0.5f, 0.5f, 0.0f}, {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}};
		if (withNormals)
		{
			primitive.Normals.assign(4, glm::vec3(0.0f, 0.0f, 1.0f));
		}
		if (withTexCoords)
		{
			primitive.TexCoords = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
		}
		primitive.Indices = {0, 1, 2, 0, 2, 3};
		return primitive;
	}

	std::vector<uint8_t> MakePng(Testing::TemporaryDirectory const& directory, uint8_t red)
	{
		Image image(2, 2, 4);
		for (uint32_t y = 0; y < 2; y++)
		{
			for (uint32_t x = 0; x < 2; x++)
			{
				uint8_t* pixel = image.GetPixel(x, y);
				pixel[0] = red;
				pixel[3] = 255;
			}
		}
		std::filesystem::path const path = directory.GetPath() / "Encoded.png";
		REQUIRE(image.WritePNG(path).IsOk());
		Result<Buffer> data = FileSystem::ReadBinaryFile(path);
		REQUIRE(data.IsOk());
		std::span<uint8_t const> const bytes = data.GetValue().GetSpan();
		return std::vector<uint8_t>(bytes.begin(), bytes.end());
	}

	glm::vec3 GetFaceNormal(ImportedModel const& model, Submesh const& submesh, uint32_t triangle)
	{
		auto const position = [&](uint32_t corner)
		{
			return model.Vertices[submesh.BaseVertex + model.Indices[submesh.BaseIndex + triangle * 3 + corner]].Position;
		};
		return glm::normalize(glm::cross(position(1) - position(0), position(2) - position(0)));
	}

	bool NearlyEqual(glm::vec3 const& a, glm::vec3 const& b, float epsilon = 1e-4f)
	{
		return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(epsilon)));
	}

	void WriteText(std::filesystem::path const& path, std::string_view text)
	{
		REQUIRE(FileSystem::WriteTextFile(path, text).IsOk());
	}
}

TEST_CASE("MeshImporter: glTF geometry, materials and node transforms")
{
	Testing::TemporaryDirectory directory;
	GltfTestBuilder builder;
	Json material = Json::object();
	material["name"] = "Painted";
	material["pbrMetallicRoughness"] =
		Json::object({{"baseColorFactor", {0.5, 0.25, 1.0, 0.75}}, {"metallicFactor", 0.0}, {"roughnessFactor", 0.3}});
	material["emissiveFactor"] = {1.0, 0.5, 0.0};
	material["extensions"] = Json::object({{"KHR_materials_emissive_strength", Json::object({{"emissiveStrength", 5.0}})}});
	material["alphaMode"] = "MASK";
	material["alphaCutoff"] = 0.3;
	material["doubleSided"] = true;

	GltfTestBuilder::Primitive quad = MakeQuad();
	quad.Material = builder.AddMaterial(material);
	int const mesh = builder.AddMesh("Panel", {quad});
	builder.SetScene({builder.AddNode("Panel", mesh, {0.0f, 2.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(2.0f))});
	std::filesystem::path const path = directory.GetPath() / "Panel.gltf";
	REQUIRE(builder.WriteGltf(path, true).IsOk());

	Result<ImportedModel> imported = MeshImporter::Import(path);
	REQUIRE_MESSAGE(imported.IsOk(), imported.GetError());
	ImportedModel const& model = imported.GetValue();
	REQUIRE(model.Submeshes.size() == 1);
	CHECK(model.Submeshes[0].Name == "Panel#0");
	CHECK(model.Submeshes[0].IndexCount == 6);
	CHECK(model.Vertices.size() == 4);

	AABB bounds;
	for (Vertex const& vertex : model.Vertices)
	{
		bounds.Expand(vertex.Position);
		CHECK(NearlyEqual(vertex.Normal, {0.0f, 0.0f, 1.0f}));
		CHECK(NearlyEqual(glm::vec3(vertex.Tangent), {1.0f, 0.0f, 0.0f}));
		CHECK(vertex.Tangent.w == 1.0f);
	}
	CHECK(NearlyEqual(bounds.Min, {-1.0f, 1.0f, 0.0f}));
	CHECK(NearlyEqual(bounds.Max, {1.0f, 3.0f, 0.0f}));

	REQUIRE(model.Materials.size() == 1);
	ImportedMaterial const& imported0 = model.Materials[0];
	CHECK(imported0.Name == "Painted");
	CHECK(imported0.Data.BaseColor == glm::vec4(0.5f, 0.25f, 1.0f, 0.75f));
	CHECK(imported0.Data.Metallic == 0.0f);
	CHECK(imported0.Data.Roughness == doctest::Approx(0.3f));
	CHECK(imported0.Data.EmissiveColor == glm::vec3(1.0f, 0.5f, 0.0f));
	CHECK(imported0.Data.EmissiveIntensity == doctest::Approx(5.0f));
	CHECK(imported0.Data.AlphaMode == MaterialAlphaMode::Mask);
	CHECK(imported0.Data.AlphaCutoff == doctest::Approx(0.3f));
	CHECK(imported0.Data.DoubleSided);
	CHECK(imported0.BaseColorTexture == -1);
	CHECK(model.Warnings.empty());
}

TEST_CASE("MeshImporter: GLB files with embedded images")
{
	Testing::TemporaryDirectory directory;
	GltfTestBuilder builder;
	int const texture = builder.AddTexture(builder.AddEmbeddedImage(MakePng(directory, 250)));
	Json material = Json::object();
	material["pbrMetallicRoughness"] = Json::object({{"baseColorTexture", Json::object({{"index", texture}})}});
	material["normalTexture"] = Json::object({{"index", texture}, {"scale", 0.5}});
	material["occlusionTexture"] = Json::object({{"index", texture}, {"strength", 0.25}});

	GltfTestBuilder::Primitive quad = MakeQuad();
	quad.Material = builder.AddMaterial(material);
	builder.SetScene({builder.AddNode("Quad", builder.AddMesh("Quad", {quad}))});
	std::filesystem::path const path = directory.GetPath() / "Quad.glb";
	REQUIRE(builder.WriteGlb(path).IsOk());

	Result<ImportedModel> imported = MeshImporter::Import(path);
	REQUIRE_MESSAGE(imported.IsOk(), imported.GetError());
	ImportedModel const& model = imported.GetValue();
	// One image shared by three slots is imported once.
	REQUIRE(model.Textures.size() == 1);
	CHECK(model.Textures[0].EncodedData.GetSize() > 0);
	CHECK(model.Textures[0].FilePath.empty());
	ImportedMaterial const& material0 = model.Materials[0];
	CHECK(material0.BaseColorTexture == 0);
	CHECK(material0.NormalTexture == 0);
	CHECK(material0.OcclusionTexture == 0);
	CHECK(material0.Data.NormalStrength == doctest::Approx(0.5f));
	CHECK(material0.Data.OcclusionStrength == doctest::Approx(0.25f));
	// glTF defaults when only textures are given.
	CHECK(material0.Data.Metallic == 1.0f);
	CHECK(material0.Data.Roughness == 1.0f);
}

TEST_CASE("MeshImporter: external buffers and images resolve relative to the file")
{
	Testing::TemporaryDirectory directory;
	REQUIRE(FileSystem::CreateDirectories(directory.GetPath() / "Model").IsOk());
	GltfTestBuilder builder;
	int const external = builder.AddTexture(builder.AddImageUri("textures/albedo%20map.png"));
	std::vector<uint8_t> const png = MakePng(directory, 10);
	int const embedded = builder.AddTexture(builder.AddImageUri("data:image/png;base64," + Base64Encode(png)));
	Json material = Json::object();
	material["pbrMetallicRoughness"] = Json::object(
		{{"baseColorTexture", Json::object({{"index", external}})}, {"metallicRoughnessTexture", Json::object({{"index", embedded}})}});
	GltfTestBuilder::Primitive quad = MakeQuad();
	quad.Material = builder.AddMaterial(material);
	builder.SetScene({builder.AddNode("Quad", builder.AddMesh("Quad", {quad}))});
	std::filesystem::path const path = directory.GetPath() / "Model" / "Quad.gltf";
	REQUIRE(builder.WriteGltf(path, false).IsOk());
	CHECK(FileSystem::IsRegularFile(directory.GetPath() / "Model" / "Quad.bin"));

	Result<ImportedModel> imported = MeshImporter::Import(path);
	REQUIRE_MESSAGE(imported.IsOk(), imported.GetError());
	ImportedModel const& model = imported.GetValue();
	REQUIRE(model.Textures.size() == 2);
	CHECK(model.Textures[0].FilePath == (directory.GetPath() / "Model" / "textures" / "albedo map.png").lexically_normal());
	CHECK(model.Textures[0].Name == "albedo map.png");
	CHECK(model.Textures[0].EncodedData.GetSize() == 0);
	CHECK(model.Textures[1].EncodedData.GetSize() == png.size());

	// A missing buffer file is an error.
	REQUIRE(FileSystem::Remove(directory.GetPath() / "Model" / "Quad.bin").IsOk());
	CHECK(MeshImporter::Import(path).IsError());
}

TEST_CASE("MeshImporter: hierarchies compose and mirrored nodes keep front faces")
{
	Testing::TemporaryDirectory directory;
	GltfTestBuilder builder;
	int const mesh = builder.AddMesh("Quad", {MakeQuad()});
	int const child = builder.AddNode("Child", mesh, {1.0f, 0.0f, 0.0f});
	// The parent mirrors X and moves everything up.
	int const parent = builder.AddNode("Parent", -1, {0.0f, 5.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {-1.0f, 1.0f, 1.0f}, {child});
	builder.SetScene({parent});
	std::filesystem::path const path = directory.GetPath() / "Mirrored.gltf";
	REQUIRE(builder.WriteGltf(path, true).IsOk());

	Result<ImportedModel> imported = MeshImporter::Import(path);
	REQUIRE_MESSAGE(imported.IsOk(), imported.GetError());
	ImportedModel const& model = imported.GetValue();
	REQUIRE(model.Submeshes.size() == 1);
	AABB bounds;
	for (Vertex const& vertex : model.Vertices)
	{
		bounds.Expand(vertex.Position);
		CHECK(NearlyEqual(vertex.Normal, {0.0f, 0.0f, 1.0f}));
		// Mirroring flips the bitangent sign.
		CHECK(vertex.Tangent.w == -1.0f);
	}
	CHECK(NearlyEqual(bounds.Min, {-1.5f, 4.5f, 0.0f}));
	CHECK(NearlyEqual(bounds.Max, {-0.5f, 5.5f, 0.0f}));
	for (uint32_t triangle = 0; triangle < 2; triangle++)
	{
		CHECK(NearlyEqual(GetFaceNormal(model, model.Submeshes[0], triangle), {0.0f, 0.0f, 1.0f}));
	}
}

TEST_CASE("MeshImporter: missing normals become flat, strips and fans become lists")
{
	Testing::TemporaryDirectory directory;
	GltfTestBuilder builder;

	GltfTestBuilder::Primitive strip = MakeQuad(false, false);
	strip.Positions = {{-0.5f, 0.5f, 0.0f}, {-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}};
	strip.Indices.clear();
	strip.Mode = 5;

	GltfTestBuilder::Primitive fan = MakeQuad(false, false);
	fan.Positions = {{-0.5f, -0.5f, 1.0f}, {0.5f, -0.5f, 1.0f}, {0.5f, 0.5f, 1.0f}, {-0.5f, 0.5f, 1.0f}};
	fan.Indices.clear();
	fan.Mode = 6;

	GltfTestBuilder::Primitive points = MakeQuad();
	points.Mode = 0;

	builder.SetScene({builder.AddNode("Shapes", builder.AddMesh("Shapes", {strip, fan, points}))});
	std::filesystem::path const path = directory.GetPath() / "Shapes.gltf";
	REQUIRE(builder.WriteGltf(path, true).IsOk());

	Result<ImportedModel> imported = MeshImporter::Import(path);
	REQUIRE_MESSAGE(imported.IsOk(), imported.GetError());
	ImportedModel const& model = imported.GetValue();
	REQUIRE(model.Submeshes.size() == 2);
	for (Submesh const& submesh : model.Submeshes)
	{
		CHECK(submesh.IndexCount == 6);
		for (uint32_t triangle = 0; triangle < 2; triangle++)
		{
			CHECK(NearlyEqual(GetFaceNormal(model, submesh, triangle), {0.0f, 0.0f, 1.0f}));
		}
		for (uint32_t i = 0; i < submesh.VertexCount; i++)
		{
			CHECK(NearlyEqual(model.Vertices[submesh.BaseVertex + i].Normal, {0.0f, 0.0f, 1.0f}));
		}
	}
	// Primitives without a material share one default material slot.
	REQUIRE(model.Materials.size() == 1);
	CHECK(model.Materials[0].Name == "Default");
	CHECK(model.Warnings.size() == 1);
}

TEST_CASE("MeshImporter: authored tangents are kept")
{
	Testing::TemporaryDirectory directory;
	GltfTestBuilder builder;
	GltfTestBuilder::Primitive quad = MakeQuad();
	quad.Tangents.assign(4, glm::vec4(0.0f, 1.0f, 0.0f, -1.0f));
	builder.SetScene({builder.AddNode("Quad", builder.AddMesh("Quad", {quad}))});
	std::filesystem::path const path = directory.GetPath() / "Tangents.gltf";
	REQUIRE(builder.WriteGltf(path, true).IsOk());

	Result<ImportedModel> imported = MeshImporter::Import(path);
	REQUIRE_MESSAGE(imported.IsOk(), imported.GetError());
	for (Vertex const& vertex : imported.GetValue().Vertices)
	{
		CHECK(vertex.Tangent == glm::vec4(0.0f, 1.0f, 0.0f, -1.0f));
	}
}

TEST_CASE("MeshImporter: invalid glTF files report errors")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "Invalid.gltf";

	SUBCASE("out-of-range index")
	{
		GltfTestBuilder builder;
		GltfTestBuilder::Primitive quad = MakeQuad();
		quad.Indices = {0, 1, 7};
		builder.SetScene({builder.AddNode("Quad", builder.AddMesh("Quad", {quad}))});
		REQUIRE(builder.WriteGltf(path, true).IsOk());
		Result<ImportedModel> const result = MeshImporter::Import(path);
		REQUIRE(result.IsError());
		CHECK(result.GetError().find("out of range") != std::string::npos);
	}
	SUBCASE("unsupported required extension")
	{
		GltfTestBuilder builder;
		builder.SetScene({builder.AddNode("Quad", builder.AddMesh("Quad", {MakeQuad()}))});
		builder.GetDocument()["extensionsUsed"] = Json::array({"KHR_draco_mesh_compression"});
		builder.GetDocument()["extensionsRequired"] = Json::array({"KHR_draco_mesh_compression"});
		REQUIRE(builder.WriteGltf(path, true).IsOk());
		Result<ImportedModel> const result = MeshImporter::Import(path);
		REQUIRE(result.IsError());
		CHECK(result.GetError().find("KHR_draco_mesh_compression") != std::string::npos);
	}
	SUBCASE("no meshes")
	{
		GltfTestBuilder builder;
		builder.SetScene({builder.AddNode("Empty", -1)});
		REQUIRE(builder.WriteGltf(path, true).IsOk());
		Result<ImportedModel> const result = MeshImporter::Import(path);
		REQUIRE(result.IsError());
		CHECK(result.GetError().find("contains no triangle meshes") != std::string::npos);
	}
	SUBCASE("not glTF")
	{
		WriteText(path, "{ this is not json");
		CHECK(MeshImporter::Import(path).IsError());
		CHECK(MeshImporter::Import(directory.GetPath() / "Missing.glb").IsError());
		CHECK(MeshImporter::Import(directory.GetPath() / "Model.blend").IsError());
	}
}

TEST_CASE("MeshImporter: OBJ files with material libraries")
{
	Testing::TemporaryDirectory directory;
	std::vector<uint8_t> const png = MakePng(directory, 30);
	REQUIRE(FileSystem::WriteBinaryFile(directory.GetPath() / "albedo.png", png).IsOk());
	WriteText(directory.GetPath() / "Quad.mtl", "newmtl Red\nKd 1.0 0.0 0.0\nd 1.0\nmap_Kd albedo.png\n");
	WriteText(directory.GetPath() / "Quad.obj", "mtllib Quad.mtl\n"
	                                            "o Quad\n"
	                                            "v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\n"
	                                            "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
	                                            "vn 0 0 1\n"
	                                            "usemtl Red\n"
	                                            "f 1/1/1 2/2/1 3/3/1 4/4/1\n");

	Result<ImportedModel> imported = MeshImporter::Import(directory.GetPath() / "Quad.obj");
	REQUIRE_MESSAGE(imported.IsOk(), imported.GetError());
	ImportedModel const& model = imported.GetValue();
	REQUIRE(model.Submeshes.size() == 1);
	CHECK(model.Submeshes[0].IndexCount == 6);
	for (Vertex const& vertex : model.Vertices)
	{
		CHECK(NearlyEqual(vertex.Normal, {0.0f, 0.0f, 1.0f}));
		// OBJ's bottom-left UV origin is converted to top-left.
		float const expectedV = vertex.Position.y < 0.0f ? 1.0f : 0.0f;
		CHECK(vertex.TexCoord.y == doctest::Approx(expectedV));
	}
	for (uint32_t triangle = 0; triangle < 2; triangle++)
	{
		CHECK(NearlyEqual(GetFaceNormal(model, model.Submeshes[0], triangle), {0.0f, 0.0f, 1.0f}));
	}

	REQUIRE(model.Materials.size() == 1);
	CHECK(model.Materials[0].Name == "Red");
	CHECK(glm::vec3(model.Materials[0].Data.BaseColor) == glm::vec3(1.0f, 0.0f, 0.0f));
	REQUIRE(model.Materials[0].BaseColorTexture == 0);
	CHECK(model.Textures[0].FilePath == (directory.GetPath() / "albedo.png").lexically_normal());
}

TEST_CASE("MeshImporter: ASCII FBX files are converted to meters and Y-up")
{
	Testing::TemporaryDirectory directory;
	// One quad in centimeters (UnitScaleFactor 1 = 1 cm per unit), translated 1 m up, with a green Lambert material.
	WriteText(directory.GetPath() / "Quad.fbx", R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
	Creator: "Strada tests"
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
	Geometry: 1001, "Geometry::Quad", "Mesh" {
		Vertices: *12 {
			a: -50,0,50,50,0,50,50,0,-50,-50,0,-50
		}
		PolygonVertexIndex: *4 {
			a: 0,1,2,-4
		}
		GeometryVersion: 124
		LayerElementNormal: 0 {
			Version: 102
			Name: ""
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Normals: *12 {
				a: 0,1,0,0,1,0,0,1,0,0,1,0
			}
		}
		LayerElementUV: 0 {
			Version: 101
			Name: "UVMap"
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "IndexToDirect"
			UV: *8 {
				a: 0,0,1,0,1,1,0,1
			}
			UVIndex: *4 {
				a: 0,1,2,3
			}
		}
		LayerElementMaterial: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "AllSame"
			ReferenceInformationType: "IndexToDirect"
			Materials: *1 {
				a: 0
			}
		}
		Layer: 0 {
			Version: 100
			LayerElement:  {
				Type: "LayerElementNormal"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementUV"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementMaterial"
				TypedIndex: 0
			}
		}
	}
	Model: 2001, "Model::Quad", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,100,0
		}
		Shading: T
		Culling: "CullingOff"
	}
	Material: 3001, "Material::Green", "" {
		Version: 102
		ShadingModel: "lambert"
		MultiLayer: 0
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0,1,0
		}
	}
}
Connections:  {
	C: "OO",2001,0
	C: "OO",1001,2001
	C: "OO",3001,2001
}
)");

	Result<ImportedModel> imported = MeshImporter::Import(directory.GetPath() / "Quad.fbx");
	REQUIRE_MESSAGE(imported.IsOk(), imported.GetError());
	ImportedModel const& model = imported.GetValue();
	REQUIRE(model.Submeshes.size() == 1);
	CHECK(model.Submeshes[0].IndexCount == 6);

	AABB bounds;
	for (Vertex const& vertex : model.Vertices)
	{
		bounds.Expand(vertex.Position);
		CHECK(NearlyEqual(vertex.Normal, {0.0f, 1.0f, 0.0f}));
	}
	CHECK(NearlyEqual(bounds.Min, {-0.5f, 1.0f, -0.5f}));
	CHECK(NearlyEqual(bounds.Max, {0.5f, 1.0f, 0.5f}));
	for (uint32_t triangle = 0; triangle < 2; triangle++)
	{
		CHECK(NearlyEqual(GetFaceNormal(model, model.Submeshes[0], triangle), {0.0f, 1.0f, 0.0f}));
	}

	REQUIRE(model.Materials.size() == 1);
	CHECK(model.Materials[0].Name == "Green");
	CHECK(glm::vec3(model.Materials[0].Data.BaseColor) == glm::vec3(0.0f, 1.0f, 0.0f));
}

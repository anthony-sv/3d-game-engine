#include "Strada/Asset/MeshProcessing.h"
#include "Strada/Asset/MeshSource.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace Strada;

namespace
{
	Vertex MakeVertex(glm::vec3 const& position, glm::vec2 const& texCoord = glm::vec2(0.0f))
	{
		Vertex vertex;
		vertex.Position = position;
		vertex.TexCoord = texCoord;
		return vertex;
	}

	std::vector<Vertex> MakeQuadCorners()
	{
		// Two counter-clockwise triangles in the XY plane facing +Z, UV origin at the top-left.
		Vertex const topLeft = MakeVertex({-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f});
		Vertex const bottomLeft = MakeVertex({-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f});
		Vertex const bottomRight = MakeVertex({1.0f, -1.0f, 0.0f}, {1.0f, 1.0f});
		Vertex const topRight = MakeVertex({1.0f, 1.0f, 0.0f}, {1.0f, 0.0f});
		return {topLeft, bottomLeft, bottomRight, topLeft, bottomRight, topRight};
	}

	Submesh MakeSubmesh(uint32_t vertexCount, uint32_t indexCount, uint32_t materialIndex = 0)
	{
		Submesh submesh;
		submesh.VertexCount = vertexCount;
		submesh.IndexCount = indexCount;
		submesh.MaterialIndex = materialIndex;
		return submesh;
	}
}

TEST_CASE("MeshSource: valid data creates a mesh with bounds")
{
	std::vector<Vertex> vertices = {MakeVertex({0, 0, 0}), MakeVertex({1, 0, 0}), MakeVertex({0, 2, 0}),
	                                MakeVertex({5, 5, 5}), MakeVertex({6, 5, 5}), MakeVertex({5, 6, 5})};
	std::vector<uint32_t> indices = {0, 1, 2, 0, 1, 2};
	Submesh second = MakeSubmesh(3, 3, 1);
	second.BaseVertex = 3;
	second.BaseIndex = 3;

	Result<Ref<MeshSource>> mesh = MeshSource::Create(vertices, indices, {MakeSubmesh(3, 3), second}, {AssetHandle(), AssetHandle()});
	REQUIRE(mesh.IsOk());
	MeshSource const& source = *mesh.GetValue();
	CHECK(source.GetAssetType() == AssetType::Mesh);
	CHECK(source.GetTriangleCount() == 2);
	CHECK(source.GetSubmeshes()[0].Bounds.Max == glm::vec3(1.0f, 2.0f, 0.0f));
	CHECK(source.GetSubmeshes()[1].Bounds.Min == glm::vec3(5.0f, 5.0f, 5.0f));
	CHECK(source.GetBounds().Min == glm::vec3(0.0f));
	CHECK(source.GetBounds().Max == glm::vec3(6.0f, 6.0f, 5.0f));
}

TEST_CASE("MeshSource: invalid data is rejected with a reason")
{
	std::vector<Vertex> const vertices = {MakeVertex({0, 0, 0}), MakeVertex({1, 0, 0}), MakeVertex({0, 1, 0})};
	std::vector<uint32_t> const indices = {0, 1, 2};
	std::vector<AssetHandle> const materials = {AssetHandle()};

	CHECK(MeshSource::Create(vertices, indices, {}, materials).GetError() == "a mesh needs at least one submesh");
	CHECK(MeshSource::Create(vertices, indices, {MakeSubmesh(3, 3)}, {}).GetError() == "a mesh needs at least one material slot");
	CHECK(MeshSource::Create(vertices, {0, 1, 3}, {MakeSubmesh(3, 3)}, materials).GetError() == "submesh 0 index 3 is out of range");
	CHECK(MeshSource::Create(vertices, {0, 1}, {MakeSubmesh(3, 2)}, materials).IsError());
	CHECK(MeshSource::Create(vertices, indices, {MakeSubmesh(3, 6)}, materials).IsError());
	CHECK(MeshSource::Create(vertices, indices, {MakeSubmesh(4, 3)}, materials).IsError());
	CHECK(MeshSource::Create(vertices, indices, {MakeSubmesh(3, 3, 1)}, materials).GetError() ==
	      "submesh 0 uses material slot 1 but the mesh has 1");

	std::vector<Vertex> invalid = vertices;
	invalid[1].Position.x = std::numeric_limits<float>::quiet_NaN();
	CHECK(MeshSource::Create(invalid, indices, {MakeSubmesh(3, 3)}, materials).IsError());
}

TEST_CASE("MeshProcessing: flat normals follow counter-clockwise winding")
{
	std::vector<Vertex> corners = MakeQuadCorners();
	MeshProcessing::ComputeFlatNormals(corners);
	for (Vertex const& corner : corners)
	{
		CHECK(corner.Normal == glm::vec3(0.0f, 0.0f, 1.0f));
	}

	std::vector<Vertex> degenerate = {MakeVertex({0, 0, 0}), MakeVertex({0, 0, 0}), MakeVertex({0, 0, 0})};
	MeshProcessing::ComputeFlatNormals(degenerate);
	CHECK(degenerate[0].Normal == glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("MeshProcessing: tangents point along +U and the bitangent towards the top of the image")
{
	std::vector<Vertex> corners = MakeQuadCorners();
	MeshProcessing::ComputeFlatNormals(corners);
	MeshProcessing::GenerateTangents(corners);
	for (Vertex const& corner : corners)
	{
		glm::vec3 const tangent(corner.Tangent);
		CHECK(tangent.x == doctest::Approx(1.0f));
		CHECK(std::abs(tangent.y) < 1e-5f);
		CHECK(corner.Tangent.w == 1.0f);
		// Bitangent = cross(N, T) * w points up (+Y), where V decreases.
		glm::vec3 const bitangent = glm::cross(corner.Normal, tangent) * corner.Tangent.w;
		CHECK(bitangent.y == doctest::Approx(1.0f));
	}
}

TEST_CASE("MeshProcessing: degenerate UVs still produce orthonormal tangents")
{
	std::vector<Vertex> corners = MakeQuadCorners();
	for (Vertex& corner : corners)
	{
		corner.TexCoord = glm::vec2(0.0f);
	}
	MeshProcessing::ComputeFlatNormals(corners);
	MeshProcessing::GenerateTangents(corners);
	for (Vertex const& corner : corners)
	{
		glm::vec3 const tangent(corner.Tangent);
		CHECK(glm::length(tangent) == doctest::Approx(1.0f));
		CHECK(std::abs(glm::dot(tangent, corner.Normal)) < 1e-4f);
		CHECK(std::abs(corner.Tangent.w) == 1.0f);
	}
}

TEST_CASE("MeshProcessing: orthogonal tangents are unit length for every direction")
{
	for (glm::vec3 const normal : {glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0),
	                               glm::normalize(glm::vec3(1, 2, -3)), glm::normalize(glm::vec3(-0.001f, 0.0f, -1.0f))})
	{
		glm::vec3 const tangent = MeshProcessing::ComputeOrthogonalTangent(normal);
		CHECK(glm::length(tangent) == doctest::Approx(1.0f));
		CHECK(std::abs(glm::dot(tangent, normal)) < 1e-5f);
	}
}

TEST_CASE("MeshProcessing: welding merges identical corners in first-occurrence order")
{
	std::vector<Vertex> const corners = MakeQuadCorners();
	MeshProcessing::IndexedGeometry const geometry = MeshProcessing::Weld(corners);
	CHECK(geometry.Vertices.size() == 4);
	CHECK(geometry.Indices == std::vector<uint32_t>{0, 1, 2, 0, 2, 3});
	CHECK(MeshProcessing::Unweld(geometry.Vertices, geometry.Indices) == corners);
}

TEST_CASE("MeshProcessing: finalize generates what the source lacks")
{
	std::vector<Vertex> corners = MakeQuadCorners();
	MeshProcessing::IndexedGeometry const welded = MeshProcessing::Weld(corners);

	MeshProcessing::IndexedGeometry const generated = MeshProcessing::Finalize(welded.Vertices, welded.Indices, false, false);
	REQUIRE(generated.Indices.size() == 6);
	for (Vertex const& vertex : generated.Vertices)
	{
		CHECK(vertex.Normal == glm::vec3(0.0f, 0.0f, 1.0f));
		CHECK(vertex.Tangent.x == doctest::Approx(1.0f));
	}

	// Complete sources are passed through untouched.
	std::vector<Vertex> authored = welded.Vertices;
	for (Vertex& vertex : authored)
	{
		vertex.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		vertex.Tangent = glm::vec4(0.0f, 0.0f, 1.0f, -1.0f);
	}
	MeshProcessing::IndexedGeometry const kept = MeshProcessing::Finalize(authored, welded.Indices, true, true);
	CHECK(kept.Vertices == authored);
	CHECK(kept.Indices == welded.Indices);
}

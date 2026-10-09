#include "Strada/Asset/MeshFactory.h"

#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <tuple>

using namespace Strada;

namespace
{
	using PositionKey = std::tuple<int64_t, int64_t, int64_t>;

	PositionKey Quantize(glm::vec3 const& position)
	{
		return {std::llround(position.x * 1e4), std::llround(position.y * 1e4), std::llround(position.z * 1e4)};
	}

	glm::vec3 GetTrianglePosition(MeshSource const& mesh, Submesh const& submesh, uint32_t triangle, uint32_t corner)
	{
		uint32_t const index = mesh.GetIndices()[submesh.BaseIndex + triangle * 3 + corner];
		return mesh.GetVertices()[submesh.BaseVertex + index].Position;
	}

	// Every directed edge (by position) of a closed, consistently wound surface has exactly one opposite edge.
	bool IsClosedAndConsistentlyWound(MeshSource const& mesh)
	{
		Submesh const& submesh = mesh.GetSubmeshes()[0];
		std::map<std::pair<PositionKey, PositionKey>, int> directedEdges;
		for (uint32_t triangle = 0; triangle < submesh.IndexCount / 3; triangle++)
		{
			for (uint32_t corner = 0; corner < 3; corner++)
			{
				PositionKey const from = Quantize(GetTrianglePosition(mesh, submesh, triangle, corner));
				PositionKey const to = Quantize(GetTrianglePosition(mesh, submesh, triangle, (corner + 1) % 3));
				directedEdges[{from, to}]++;
			}
		}
		for (auto const& [edge, count] : directedEdges)
		{
			auto const opposite = directedEdges.find({edge.second, edge.first});
			if (count != 1 || opposite == directedEdges.end() || opposite->second != 1)
			{
				return false;
			}
		}
		return true;
	}

	void CheckPrimitive(Ref<MeshSource> const& mesh, bool isSolid, glm::vec3 const& expectedMin, glm::vec3 const& expectedMax)
	{
		REQUIRE(mesh);
		REQUIRE(mesh->GetSubmeshes().size() == 1);
		REQUIRE(mesh->GetMaterials().size() == 1);
		CHECK(mesh->GetMaterials()[0] == AssetHandle(UUID(4242)));

		for (int axis = 0; axis < 3; axis++)
		{
			CHECK(mesh->GetBounds().Min[axis] == doctest::Approx(expectedMin[axis]).epsilon(1e-4));
			CHECK(mesh->GetBounds().Max[axis] == doctest::Approx(expectedMax[axis]).epsilon(1e-4));
		}

		for (Vertex const& vertex : mesh->GetVertices())
		{
			CHECK(glm::length(vertex.Normal) == doctest::Approx(1.0f).epsilon(1e-4));
			CHECK(glm::length(glm::vec3(vertex.Tangent)) == doctest::Approx(1.0f).epsilon(1e-4));
			CHECK(std::abs(glm::dot(vertex.Normal, glm::vec3(vertex.Tangent))) < 1e-3f);
			CHECK(std::abs(vertex.Tangent.w) == 1.0f);
			CHECK(vertex.TexCoord.x >= 0.0f);
			CHECK(vertex.TexCoord.x <= 1.0f);
			CHECK(vertex.TexCoord.y >= 0.0f);
			CHECK(vertex.TexCoord.y <= 1.0f);
		}

		// Triangles face the same way as their vertex normals and, for the (convex) solids, away from the center.
		Submesh const& submesh = mesh->GetSubmeshes()[0];
		for (uint32_t triangle = 0; triangle < submesh.IndexCount / 3; triangle++)
		{
			glm::vec3 const a = GetTrianglePosition(*mesh, submesh, triangle, 0);
			glm::vec3 const b = GetTrianglePosition(*mesh, submesh, triangle, 1);
			glm::vec3 const c = GetTrianglePosition(*mesh, submesh, triangle, 2);
			glm::vec3 const faceNormal = glm::cross(b - a, c - a);
			REQUIRE(glm::length(faceNormal) > 0.0f);

			uint32_t const index = mesh->GetIndices()[submesh.BaseIndex + triangle * 3];
			CHECK(glm::dot(faceNormal, mesh->GetVertices()[index].Normal) > 0.0f);
			if (isSolid)
			{
				CHECK(glm::dot(faceNormal, (a + b + c) / 3.0f - mesh->GetBounds().GetCenter()) > 0.0f);
			}
		}

		if (isSolid)
		{
			CHECK(IsClosedAndConsistentlyWound(*mesh));
		}
	}
}

TEST_CASE("MeshFactory: solid primitives are closed, outward facing and correctly sized")
{
	AssetHandle const material(UUID(4242));
	CheckPrimitive(MeshFactory::CreateCube(material), true, glm::vec3(-0.5f), glm::vec3(0.5f));
	CheckPrimitive(MeshFactory::CreateSphere(material), true, glm::vec3(-0.5f), glm::vec3(0.5f));
	CheckPrimitive(MeshFactory::CreateSphere(material, 3, 2), true, glm::vec3(-0.5f * std::sqrt(3.0f) / 2.0f, -0.5f, -0.25f),
	               glm::vec3(0.5f * std::sqrt(3.0f) / 2.0f, 0.5f, 0.5f));
	CheckPrimitive(MeshFactory::CreateCylinder(material), true, glm::vec3(-0.5f), glm::vec3(0.5f));
	CheckPrimitive(MeshFactory::CreateCapsule(material), true, glm::vec3(-0.5f, -1.0f, -0.5f), glm::vec3(0.5f, 1.0f, 0.5f));
	CheckPrimitive(MeshFactory::CreateCone(material), true, glm::vec3(-0.5f), glm::vec3(0.5f));
}

TEST_CASE("MeshFactory: flat primitives face their documented direction")
{
	AssetHandle const material(UUID(4242));
	Ref<MeshSource> const plane = MeshFactory::CreatePlane(material);
	CheckPrimitive(plane, false, glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 0.0f, 0.5f));
	for (Vertex const& vertex : plane->GetVertices())
	{
		CHECK(vertex.Normal == glm::vec3(0.0f, 1.0f, 0.0f));
	}

	Ref<MeshSource> const quad = MeshFactory::CreateQuad(material);
	CheckPrimitive(quad, false, glm::vec3(-0.5f, -0.5f, 0.0f), glm::vec3(0.5f, 0.5f, 0.0f));
	for (Vertex const& vertex : quad->GetVertices())
	{
		CHECK(vertex.Normal == glm::vec3(0.0f, 0.0f, 1.0f));
		CHECK(vertex.Tangent.x == doctest::Approx(1.0f));
		// UV origin at the top-left: the top edge has v = 0.
		CHECK(vertex.TexCoord.y == doctest::Approx(vertex.Position.y > 0.0f ? 0.0f : 1.0f));
	}
}

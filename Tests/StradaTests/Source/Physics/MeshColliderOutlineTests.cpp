#include "Physics/PhysicsTestUtilities.h"

#include "Strada/Asset/MeshFactory.h"
#include "Strada/Physics/MeshColliderOutline.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	Ref<MeshSource> CreateMesh(std::vector<glm::vec3> const& positions, std::vector<uint32_t> indices)
	{
		std::vector<Vertex> vertices;
		for (glm::vec3 const& position : positions)
		{
			Vertex vertex;
			vertex.Position = position;
			vertices.push_back(vertex);
		}
		Submesh submesh;
		submesh.VertexCount = static_cast<uint32_t>(vertices.size());
		submesh.IndexCount = static_cast<uint32_t>(indices.size());
		Result<Ref<MeshSource>> mesh = MeshSource::Create(std::move(vertices), std::move(indices), {submesh}, {AssetHandle()});
		REQUIRE(mesh.IsOk());
		return mesh.GetValue();
	}

	using Point = std::tuple<float, float, float>;

	Point ToPoint(glm::vec3 const& position)
	{
		// Rounded so the same corner compares equal however it was computed.
		auto const round = [](float value)
		{
			return std::round(value * 1000.0f) / 1000.0f + 0.0f;
		};
		return {round(position.x), round(position.y), round(position.z)};
	}

	// The edges of an outline as pairs of rounded points, each pair in order.
	std::vector<std::pair<Point, Point>> GetEdges(std::vector<glm::vec3> const& outline)
	{
		REQUIRE(outline.size() % 2 == 0);
		std::vector<std::pair<Point, Point>> edges;
		for (size_t point = 0; point < outline.size(); point += 2)
		{
			Point const from = ToPoint(outline[point]);
			Point const to = ToPoint(outline[point + 1]);
			edges.emplace_back(std::min(from, to), std::max(from, to));
		}
		return edges;
	}

	bool HasDuplicates(std::vector<std::pair<Point, Point>> const& edges)
	{
		return std::set<std::pair<Point, Point>>(edges.begin(), edges.end()).size() != edges.size();
	}

	// Whether every edge runs along one axis between corners of the box with these half extents around the origin.
	bool AreBoxEdges(std::vector<std::pair<Point, Point>> const& edges, glm::vec3 const& halfExtents)
	{
		auto const isCorner = [&](Point const& point)
		{
			return std::abs(std::abs(std::get<0>(point)) - halfExtents.x) < 1e-3f &&
			       std::abs(std::abs(std::get<1>(point)) - halfExtents.y) < 1e-3f &&
			       std::abs(std::abs(std::get<2>(point)) - halfExtents.z) < 1e-3f;
		};
		return std::all_of(edges.begin(), edges.end(),
		                   [&](std::pair<Point, Point> const& edge)
		                   {
							   int const axes = (std::get<0>(edge.first) != std::get<0>(edge.second) ? 1 : 0) +
			                                    (std::get<1>(edge.first) != std::get<1>(edge.second) ? 1 : 0) +
			                                    (std::get<2>(edge.first) != std::get<2>(edge.second) ? 1 : 0);
							   return axes == 1 && isCorner(edge.first) && isCorner(edge.second);
						   });
	}
}

TEST_CASE("MeshColliderOutline: triangle meshes show each edge once, without the diagonals of flat faces")
{
	// The cube's faces repeat their corners, yet each of its 12 edges appears once and no face shows its diagonal.
	std::vector<std::pair<Point, Point>> const cube =
		GetEdges(MeshColliderOutline::BuildTriangleMesh(*MeshFactory::CreateCube(AssetHandle())));
	CHECK(cube.size() == 12);
	CHECK_FALSE(HasDuplicates(cube));
	CHECK(AreBoxEdges(cube, glm::vec3(0.5f)));

	// Open edges always show: a flat square is its border.
	CHECK(GetEdges(MeshColliderOutline::BuildTriangleMesh(*MeshFactory::CreatePlane(AssetHandle()))).size() == 4);

	// Two triangles folded along their shared edge show it; in one plane they do not.
	std::vector<uint32_t> const twoTriangles = {0, 1, 2, 0, 2, 3};
	CHECK(
		GetEdges(MeshColliderOutline::BuildTriangleMesh(*CreateMesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, twoTriangles))).size() ==
		4);
	CHECK(
		GetEdges(MeshColliderOutline::BuildTriangleMesh(*CreateMesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 1}, {0, 1, 0}}, twoTriangles))).size() ==
		5);

	// Curved surfaces show their edges once, also along seams whose corners were computed a rounding error apart (the
	// sphere's first and last meridian); triangles without area, which physics drops, show none.
	Ref<MeshSource> const sphere = MeshFactory::CreateSphere(AssetHandle(), 12, 6);
	std::vector<std::pair<Point, Point>> const sphereEdges = GetEdges(MeshColliderOutline::BuildTriangleMesh(*sphere));
	CHECK_FALSE(HasDuplicates(sphereEdges));
	// Its quads are flat: 12 meridians of 6 edges and 5 rings of 12 edges.
	CHECK(sphereEdges.size() == 12 * 6 + 5 * 12);
	CHECK(MeshColliderOutline::BuildTriangleMesh(*CreateMesh({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}, {0, 1, 2})).empty());

	// Every submesh counts, each with its own base vertex and index.
	Ref<MeshSource> const cubeMesh = MeshFactory::CreateCube(AssetHandle());
	std::vector<Vertex> vertices = cubeMesh->GetVertices();
	std::vector<uint32_t> indices = cubeMesh->GetIndices();
	uint32_t const firstVertex = static_cast<uint32_t>(vertices.size());
	uint32_t const firstIndex = static_cast<uint32_t>(indices.size());
	for (Vertex vertex : cubeMesh->GetVertices())
	{
		vertex.Position.y += 2.0f;
		vertices.push_back(vertex);
	}
	indices.insert(indices.end(), cubeMesh->GetIndices().begin(), cubeMesh->GetIndices().end());
	Submesh lower;
	lower.VertexCount = firstVertex;
	lower.IndexCount = firstIndex;
	Submesh upper = lower;
	upper.BaseVertex = firstVertex;
	upper.BaseIndex = firstIndex;
	Result<Ref<MeshSource>> stacked = MeshSource::Create(std::move(vertices), std::move(indices), {lower, upper}, {AssetHandle()});
	REQUIRE(stacked.IsOk());
	CHECK(GetEdges(MeshColliderOutline::BuildTriangleMesh(*stacked.GetValue())).size() == 24);
}

TEST_CASE("MeshColliderOutline: large triangle meshes keep their sharpest edges")
{
	// A bumpy 120 x 120 grid: about 43,000 edges where the surface bends, more than are drawn.
	constexpr uint32_t Cells = 120;
	std::vector<glm::vec3> positions;
	for (uint32_t row = 0; row <= Cells; row++)
	{
		for (uint32_t column = 0; column <= Cells; column++)
		{
			float const height = 0.05f * std::sin(static_cast<float>(row) * 1.3f) * std::cos(static_cast<float>(column) * 0.7f);
			positions.emplace_back(static_cast<float>(column), height, static_cast<float>(row));
		}
	}
	std::vector<uint32_t> indices;
	for (uint32_t row = 0; row < Cells; row++)
	{
		for (uint32_t column = 0; column < Cells; column++)
		{
			uint32_t const corner = row * (Cells + 1) + column;
			indices.insert(indices.end(), {corner, corner + Cells + 1, corner + 1, corner + 1, corner + Cells + 1, corner + Cells + 2});
		}
	}
	std::vector<std::pair<Point, Point>> const edges = GetEdges(MeshColliderOutline::BuildTriangleMesh(*CreateMesh(positions, indices)));
	CHECK(edges.size() == MeshColliderOutline::MaxTriangleMeshEdges);
	CHECK_FALSE(HasDuplicates(edges));
	// Open edges count as the sharpest: the border is always there.
	size_t const border = static_cast<size_t>(std::count_if(edges.begin(), edges.end(),
	                                                        [](std::pair<Point, Point> const& edge)
	                                                        {
																auto const onBorder = [](Point const& point)
																{
																	float const x = std::get<0>(point);
																	float const z = std::get<2>(point);
																	return x == 0.0f || z == 0.0f || x == static_cast<float>(Cells) ||
			                                                               z == static_cast<float>(Cells);
																};
																return onBorder(edge.first) && onBorder(edge.second);
															}));
	CHECK(border == 4 * Cells);
}

TEST_CASE("MeshColliderOutline: convex hulls are those physics builds")
{
	Ref<MeshSource> const cube = MeshFactory::CreateCube(AssetHandle());
	CHECK(MeshColliderOutline::BuildConvexHull(*cube).IsError());

	Testing::PhysicsSystemScope physics;
	// Flat sides are whole faces: the cube's twelve edges.
	Result<std::vector<glm::vec3>> const cubeHull = MeshColliderOutline::BuildConvexHull(*cube);
	REQUIRE(cubeHull.IsOk());
	std::vector<std::pair<Point, Point>> const cubeEdges = GetEdges(cubeHull.GetValue());
	CHECK(cubeEdges.size() == 12);
	CHECK_FALSE(HasDuplicates(cubeEdges));
	CHECK(AreBoxEdges(cubeEdges, glm::vec3(0.5f)));

	// Points inside the hull and dents in the surface are not part of it: the hull of two cubes side by side is one box.
	std::vector<glm::vec3> positions;
	std::vector<uint32_t> indices;
	for (float const offset : {-1.0f, 1.0f})
	{
		uint32_t const first = static_cast<uint32_t>(positions.size());
		for (Vertex const& vertex : cube->GetVertices())
		{
			positions.push_back(vertex.Position + glm::vec3(offset, 0.0f, 0.0f));
		}
		for (uint32_t const index : cube->GetIndices())
		{
			indices.push_back(first + index);
		}
	}
	positions.emplace_back(0.0f, 0.0f, 0.0f);
	Result<std::vector<glm::vec3>> const pairHull = MeshColliderOutline::BuildConvexHull(*CreateMesh(positions, indices));
	REQUIRE(pairHull.IsOk());
	std::vector<std::pair<Point, Point>> const pairEdges = GetEdges(pairHull.GetValue());
	CHECK(pairEdges.size() == 12);
	CHECK(AreBoxEdges(pairEdges, glm::vec3(1.5f, 0.5f, 0.5f)));

	// Hulls of finely divided meshes keep at most 256 corners, all on the surface.
	Result<std::vector<glm::vec3>> const sphereHull =
		MeshColliderOutline::BuildConvexHull(*MeshFactory::CreateSphere(AssetHandle(), 64, 32));
	REQUIRE(sphereHull.IsOk());
	std::set<Point> corners;
	for (glm::vec3 const& point : sphereHull.GetValue())
	{
		corners.insert(ToPoint(point));
		CHECK(glm::length(point) == doctest::Approx(0.5f).epsilon(0.01));
	}
	CHECK(corners.size() > 32);
	CHECK(corners.size() <= 256);

	// Flat meshes have a flat hull: their border. Physics refuses points on a line, and so does the outline.
	Result<std::vector<glm::vec3>> const planeHull = MeshColliderOutline::BuildConvexHull(*MeshFactory::CreatePlane(AssetHandle()));
	REQUIRE(planeHull.IsOk());
	std::vector<std::pair<Point, Point>> const planeEdges = GetEdges(planeHull.GetValue());
	CHECK(planeEdges.size() == 4);
	CHECK(AreBoxEdges(planeEdges, glm::vec3(0.5f, 0.0f, 0.5f)));
	CHECK(MeshColliderOutline::BuildConvexHull(*CreateMesh({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}, {0, 1, 2})).IsError());
}

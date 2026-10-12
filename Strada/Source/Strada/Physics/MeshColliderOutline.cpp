#include "stpch.h"
#include "Strada/Physics/MeshColliderOutline.h"

#include "Strada/Asset/MeshSource.h"
#include "Strada/Physics/JoltContext.h"
#include "Strada/Physics/PhysicsSystem.h"

#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace Strada::MeshColliderOutline
{
	namespace
	{
		// Adjacent triangles whose normals differ by less (about 0.8 degrees) lie in one plane.
		constexpr float CoplanarCosine = 0.9999f;

		// Corners closer than this share a place, relative to the size of the mesh: meshes repeat a corner for every face
		// that meets there, sometimes a rounding error apart (seams computed twice).
		constexpr double WeldTolerance = 1.0e-6;

		// A cell of the grid places are welded in.
		struct PlaceKey
		{
			std::array<int64_t, 3> Cells{};

			bool operator==(PlaceKey const&) const = default;
		};

		struct PlaceKeyHash
		{
			size_t operator()(PlaceKey const& key) const
			{
				// FNV-1a.
				uint64_t hash = 14695981039346656037ull;
				for (int64_t const cell : key.Cells)
				{
					hash = (hash ^ static_cast<uint64_t>(cell)) * 1099511628211ull;
				}
				return static_cast<size_t>(hash);
			}
		};

		bool IsFinite(glm::vec3 const& position)
		{
			return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
		}

		uint64_t EdgeKey(uint32_t a, uint32_t b)
		{
			return (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
		}
	}

	Result<std::vector<glm::vec3>> BuildConvexHull(MeshSource const& mesh)
	{
		if (!PhysicsSystem::IsInitialized())
		{
			return Error{"the physics system is not initialized"};
		}
		std::vector<Vertex> const& vertices = mesh.GetVertices();
		if (vertices.empty())
		{
			return Error{"the mesh has no vertices"};
		}
		JPH::Array<JPH::Vec3> points;
		points.reserve(vertices.size());
		for (Vertex const& vertex : vertices)
		{
			points.push_back(Jolt::ToJolt(vertex.Position));
		}
		// The settings PhysicsScene builds convex mesh colliders with.
		JPH::ConvexHullShapeSettings const settings(points);
		JPH::ShapeSettings::ShapeResult const result = settings.Create();
		if (result.HasError())
		{
			return MakeError("the convex hull cannot be built: {}", result.GetError().c_str());
		}
		JPH::RefConst<JPH::Shape> const shape = result.Get();
		if (shape->GetSubType() != JPH::EShapeSubType::ConvexHull)
		{
			return Error{"the convex hull cannot be built"};
		}
		auto const& hull = static_cast<JPH::ConvexHullShape const&>(*shape);

		// Jolt keeps the corners relative to the hull's center of mass.
		glm::vec3 const center = Jolt::ToGlm(hull.GetCenterOfMass());
		std::vector<glm::vec3> outline;
		std::unordered_set<uint64_t> edges;
		std::array<JPH::uint, JPH::ConvexHullShape::cMaxPointsInHull> face{};
		for (JPH::uint faceIndex = 0; faceIndex < hull.GetNumFaces(); faceIndex++)
		{
			JPH::uint const corners = std::min(hull.GetFaceVertices(faceIndex, static_cast<JPH::uint>(face.size()), face.data()),
			                                   static_cast<JPH::uint>(face.size()));
			for (JPH::uint corner = 0; corner < corners; corner++)
			{
				JPH::uint const from = face[corner];
				JPH::uint const to = face[(corner + 1) % corners];
				// Every edge borders two faces.
				if (from != to && edges.insert(EdgeKey(from, to)).second)
				{
					outline.push_back(center + Jolt::ToGlm(hull.GetPoint(from)));
					outline.push_back(center + Jolt::ToGlm(hull.GetPoint(to)));
				}
			}
		}
		return outline;
	}

	std::vector<glm::vec3> BuildTriangleMesh(MeshSource const& mesh)
	{
		std::vector<Vertex> const& vertices = mesh.GetVertices();
		std::vector<uint32_t> const& indices = mesh.GetIndices();

		// One corner per place, so the triangles of neighboring faces share their edges. Places are cells of a grid that
		// starts at the mesh's lowest corner (in double precision, so meshes far from the origin weld as well).
		glm::dvec3 lowest(std::numeric_limits<double>::max());
		glm::dvec3 highest(std::numeric_limits<double>::lowest());
		for (Vertex const& vertex : vertices)
		{
			if (IsFinite(vertex.Position))
			{
				lowest = glm::min(lowest, glm::dvec3(vertex.Position));
				highest = glm::max(highest, glm::dvec3(vertex.Position));
			}
		}
		glm::dvec3 const extent = glm::max(highest - lowest, glm::dvec3(0.0));
		double const size = std::max({extent.x, extent.y, extent.z});
		double const cell = size > 0.0 ? size * WeldTolerance : 1.0;

		std::vector<glm::vec3> corners;
		std::vector<uint32_t> cornerOfVertex(vertices.size());
		std::unordered_map<PlaceKey, uint32_t, PlaceKeyHash> cornerOfPlace;
		cornerOfPlace.reserve(vertices.size());
		for (size_t vertex = 0; vertex < vertices.size(); vertex++)
		{
			glm::vec3 const& position = vertices[vertex].Position;
			uint32_t const next = static_cast<uint32_t>(corners.size());
			if (!IsFinite(position))
			{
				// Its triangles are skipped below.
				corners.push_back(position);
				cornerOfVertex[vertex] = next;
				continue;
			}
			PlaceKey key;
			for (glm::length_t axis = 0; axis < 3; axis++)
			{
				key.Cells[static_cast<size_t>(axis)] = std::llround((static_cast<double>(position[axis]) - lowest[axis]) / cell);
			}
			auto const [it, added] = cornerOfPlace.try_emplace(key, next);
			if (added)
			{
				corners.push_back(position);
			}
			cornerOfVertex[vertex] = it->second;
		}

		// Each edge once, with the normal of its first triangle and the smallest cosine to the others: how much the surface
		// bends there.
		struct Edge
		{
			uint32_t From = 0;
			uint32_t To = 0;
			glm::vec3 Normal = glm::vec3(0.0f);
			uint32_t Triangles = 0;
			float Cosine = 1.0f;
		};
		std::vector<Edge> edges;
		std::unordered_map<uint64_t, size_t> edgeOfKey;
		for (Submesh const& submesh : mesh.GetSubmeshes())
		{
			// Indices are relative to the submesh's first vertex (MeshSource validated the ranges).
			for (uint32_t index = 0; index + 2 < submesh.IndexCount; index += 3)
			{
				std::array<uint32_t, 3> triangle{};
				for (size_t corner = 0; corner < 3; corner++)
				{
					triangle[corner] = cornerOfVertex[submesh.BaseVertex + indices[submesh.BaseIndex + index + corner]];
				}
				glm::vec3 const cross =
					glm::cross(corners[triangle[1]] - corners[triangle[0]], corners[triangle[2]] - corners[triangle[0]]);
				float const length = glm::length(cross);
				// Degenerate triangles (and non-finite ones) have no surface to collide with.
				if (!(length > 0.0f) || !std::isfinite(length))
				{
					continue;
				}
				glm::vec3 const normal = cross / length;
				for (size_t corner = 0; corner < 3; corner++)
				{
					uint32_t const from = triangle[corner];
					uint32_t const to = triangle[(corner + 1) % 3];
					auto const [it, added] = edgeOfKey.try_emplace(EdgeKey(from, to), edges.size());
					if (added)
					{
						edges.push_back({from, to, normal, 1, 1.0f});
					}
					else
					{
						Edge& edge = edges[it->second];
						edge.Triangles++;
						edge.Cosine = std::min(edge.Cosine, glm::dot(edge.Normal, normal));
					}
				}
			}
		}

		// Edges inside flat faces are left out; open edges and those of more than two triangles always count as sharp.
		auto const sharpness = [](Edge const& edge)
		{
			return edge.Triangles == 2 ? edge.Cosine : -2.0f;
		};
		std::vector<size_t> shown;
		for (size_t edge = 0; edge < edges.size(); edge++)
		{
			if (sharpness(edges[edge]) < CoplanarCosine)
			{
				shown.push_back(edge);
			}
		}
		if (shown.size() > MaxTriangleMeshEdges)
		{
			std::nth_element(shown.begin(), shown.begin() + MaxTriangleMeshEdges, shown.end(),
			                 [&](size_t a, size_t b)
			                 {
								 return sharpness(edges[a]) < sharpness(edges[b]);
							 });
			shown.resize(MaxTriangleMeshEdges);
			// In the mesh's order again.
			std::sort(shown.begin(), shown.end());
		}

		std::vector<glm::vec3> outline;
		outline.reserve(shown.size() * 2);
		for (size_t const edge : shown)
		{
			outline.push_back(corners[edges[edge].From]);
			outline.push_back(corners[edges[edge].To]);
		}
		return outline;
	}
}

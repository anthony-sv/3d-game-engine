#pragma once

#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Strada
{
	class MeshSource;

	// Outlines of the shapes physics makes of meshes for mesh colliders, for editors to draw: the two ends of each edge in
	// turn, in the mesh's space. Main thread only.
	namespace MeshColliderOutline
	{
		// Triangle meshes with more edges keep their sharpest ones.
		inline constexpr uint32_t MaxTriangleMeshEdges = 16384;

		// The convex hull of the mesh's vertices that PhysicsScene collides with for convex mesh colliders (and for the
		// triangle meshes of dynamic bodies), as Jolt builds it: at most 256 corners, and faces are whole polygons, so flat
		// sides show no diagonals. Requires the PhysicsSystem; fails where physics cannot build the hull either.
		[[nodiscard]] Result<std::vector<glm::vec3>> BuildConvexHull(MeshSource const& mesh);
		// The edges of the mesh's triangles (every submesh), each once, without those between two triangles in one plane, so
		// flat faces read as polygons. Of meshes with more than MaxTriangleMeshEdges such edges, the edges where the surface
		// bends most.
		std::vector<glm::vec3> BuildTriangleMesh(MeshSource const& mesh);
	}
}

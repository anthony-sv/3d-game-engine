#pragma once

#include "Strada/Asset/MeshSource.h"

#include <cstdint>
#include <vector>

namespace Strada::MeshProcessing
{
	struct IndexedGeometry
	{
		std::vector<Vertex> Vertices;
		std::vector<uint32_t> Indices;
	};

	// Expands an indexed triangle list into one vertex per triangle corner.
	std::vector<Vertex> Unweld(std::vector<Vertex> const& vertices, std::vector<uint32_t> const& indices);

	// Sets every corner's normal to its triangle's face normal (counter-clockwise front faces). Degenerate triangles get +Y.
	void ComputeFlatNormals(std::vector<Vertex>& corners);

	// Generates MikkTSpace tangents for an unwelded triangle list (normals and UVs must be set). Triangles whose UVs are
	// degenerate get an arbitrary tangent perpendicular to the normal.
	void GenerateTangents(std::vector<Vertex>& corners);

	// Merges bitwise-identical corners into an indexed triangle list (first-occurrence order).
	IndexedGeometry Weld(std::vector<Vertex> const& corners);

	// Import pipeline for one submesh: flat normals when the source has none, MikkTSpace tangents when the source has
	// none (or normals were generated), then welding.
	IndexedGeometry Finalize(std::vector<Vertex> vertices, std::vector<uint32_t> indices, bool hasNormals, bool hasTangents);

	// A unit tangent perpendicular to the (unit) normal, stable for every direction.
	glm::vec3 ComputeOrthogonalTangent(glm::vec3 const& normal);
}

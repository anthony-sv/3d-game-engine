#pragma once

// Internal to the Physics module: the world's table of collider surfaces and the lookup from a Jolt sub-shape to its
// surface, which the contact listener, the queries and the vehicles' tyres share (Docs/Architecture.md §8, "Physics
// materials" and "Vehicles"). Jolt types stay out of every other header.

#include "Strada/Physics/PhysicsTypes.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>

#include <cstdint>
#include <vector>

namespace Strada
{
	// Collider properties referenced by the user data of their shapes (index + 1; 0 = none): whether the collider is a
	// trigger, its surface and, for a triangle mesh with submesh surfaces, those. Such a mesh's triangles carry Jolt
	// material indices: 0 for the collider's surface, i > 0 for Submeshes[i - 1] (its material list holds placeholders,
	// so that the indices are valid). The main thread writes the table between steps only; Jolt's worker threads read it
	// during steps.
	struct ColliderMaterial
	{
		bool IsTrigger = false;
		ColliderSurface Surface;
		std::vector<ColliderSurface> Submeshes;
	};

	// What a sub-shape of a body is: its collider's entry and its surface (the submesh's on a triangle mesh with submesh
	// surfaces). Both null for a shape without an entry (a character's inner body).
	struct SubShapeMaterial
	{
		ColliderMaterial const* Collider = nullptr;
		ColliderSurface const* Surface = nullptr;
	};

	// Any thread, while the table does not change. Compound and decorated shapes pass the ID on to their leaf
	// (CompoundShape::GetLeafShape, DecoratedShape::GetLeafShape); a leaf's user data names its collider, and a mesh
	// shape's remaining ID its triangle.
	SubShapeMaterial FindSubShapeMaterial(std::vector<ColliderMaterial> const& materials, JPH::Shape const& shape,
	                                      JPH::SubShapeID const& subShape);

	// Lets queries, characters and vehicles' wheels pass through trigger colliders (the user data of a collider's shapes
	// indexes its entry). Any thread, while the table does not change.
	class SolidShapeFilter final : public JPH::ShapeFilter
	{
	public:
		explicit SolidShapeFilter(std::vector<ColliderMaterial> const& materials)
			: m_Materials(materials)
		{
		}

		bool ShouldCollide(JPH::Shape const* shape, JPH::SubShapeID const&) const override { return !IsTrigger(*shape); }

		bool ShouldCollide(JPH::Shape const*, JPH::SubShapeID const&, JPH::Shape const* shape, JPH::SubShapeID const&) const override
		{
			return !IsTrigger(*shape);
		}

	private:
		bool IsTrigger(JPH::Shape const& shape) const
		{
			uint64_t const index = shape.GetUserData();
			return index > 0 && index <= m_Materials.size() && m_Materials[index - 1].IsTrigger;
		}

		std::vector<ColliderMaterial> const& m_Materials;
	};
}

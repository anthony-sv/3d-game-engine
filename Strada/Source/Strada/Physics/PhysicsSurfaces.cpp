#include "stpch.h"
#include "Strada/Physics/PhysicsSurfaces.h"

#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace Strada
{
	SubShapeMaterial FindSubShapeMaterial(std::vector<ColliderMaterial> const& materials, JPH::Shape const& shape,
	                                      JPH::SubShapeID const& subShape)
	{
		JPH::SubShapeID triangle;
		JPH::Shape const* leaf = shape.GetLeafShape(subShape, triangle);
		uint64_t const index = leaf != nullptr ? leaf->GetUserData() : 0;
		if (index == 0 || index > materials.size())
		{
			return {};
		}
		ColliderMaterial const& collider = materials[index - 1];
		ColliderSurface const* surface = &collider.Surface;
		if (!collider.Submeshes.empty() && leaf->GetSubType() == JPH::EShapeSubType::Mesh)
		{
			JPH::uint const material = static_cast<JPH::MeshShape const*>(leaf)->GetMaterialIndex(triangle);
			if (material > 0 && material <= collider.Submeshes.size())
			{
				surface = &collider.Submeshes[material - 1];
			}
		}
		return {&collider, surface};
	}
}

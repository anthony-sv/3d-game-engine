#pragma once

#include "Strada/Core/UUID.h"
#include "Strada/Math/AABB.h"

#include <span>

namespace Strada
{
	class Scene;

	// The world-space bounds of the entities' meshes; an entity without a mesh that loads counts as its origin. Entities the
	// scene does not have are skipped: the bounds are invalid when it has none of them.
	AABB ComputeEntityBounds(Scene& scene, std::span<UUID const> entities);
}

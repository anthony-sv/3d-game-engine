#include "stpch.h"
#include "Strada/Math/AABB.h"

namespace Strada
{
	AABB AABB::Transform(glm::mat4 const& transform) const
	{
		if (!IsValid())
		{
			return {};
		}

		// Arvo's method: the transformed extents are the absolute linear part applied to the extents.
		glm::vec3 const center = glm::vec3(transform * glm::vec4(GetCenter(), 1.0f));
		glm::mat3 const linear(transform);
		glm::mat3 absolute;
		for (int column = 0; column < 3; column++)
		{
			absolute[column] = glm::abs(linear[column]);
		}
		glm::vec3 const extents = absolute * GetExtents();
		return {center - extents, center + extents};
	}
}

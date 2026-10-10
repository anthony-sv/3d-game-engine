#include "stpch.h"
#include "Strada/Math/Frustum.h"

#include <cmath>

namespace Strada
{
	namespace
	{
		// glm stores columns: row i holds the coefficients of the matrix's output component i.
		glm::vec4 Row(glm::mat4 const& matrix, int index)
		{
			return glm::vec4(matrix[0][index], matrix[1][index], matrix[2][index], matrix[3][index]);
		}
	}

	Frustum::Frustum(glm::mat4 const& viewProjection)
	{
		glm::vec4 const x = Row(viewProjection, 0);
		glm::vec4 const y = Row(viewProjection, 1);
		glm::vec4 const z = Row(viewProjection, 2);
		glm::vec4 const w = Row(viewProjection, 3);
		// The last plane keeps every point unless a distance limit replaces it.
		m_Planes = {w + x, w - x, w + y, w - y, z, w - z, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)};
	}

	Frustum::Frustum(glm::mat4 const& view, glm::mat4 const& projection, float maxDistance)
		: Frustum(projection * view)
	{
		if (std::isfinite(maxDistance))
		{
			// View-space z + maxDistance >= 0.
			m_Planes[6] = Row(view, 2) + maxDistance * Row(view, 3);
		}
	}

	bool Frustum::Intersects(AABB const& box) const
	{
		if (!box.IsValid())
		{
			return false;
		}
		for (glm::vec4 const& plane : m_Planes)
		{
			// The corner furthest along the plane's normal: when even it is outside, the whole box is.
			glm::vec3 const corner(plane.x >= 0.0f ? box.Max.x : box.Min.x, plane.y >= 0.0f ? box.Max.y : box.Min.y,
			                       plane.z >= 0.0f ? box.Max.z : box.Min.z);
			if (glm::dot(glm::vec3(plane), corner) + plane.w < 0.0f)
			{
				return false;
			}
		}
		return true;
	}

	bool Frustum::Contains(glm::vec3 const& point) const
	{
		for (glm::vec4 const& plane : m_Planes)
		{
			if (glm::dot(glm::vec3(plane), point) + plane.w < 0.0f)
			{
				return false;
			}
		}
		return true;
	}
}

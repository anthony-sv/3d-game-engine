#pragma once

#include <glm/glm.hpp>

#include <limits>

namespace Strada
{
	// Axis-aligned bounding box. A default-constructed box is empty (Min > Max) and grows with Expand.
	struct AABB
	{
		glm::vec3 Min = glm::vec3(std::numeric_limits<float>::max());
		glm::vec3 Max = glm::vec3(std::numeric_limits<float>::lowest());

		AABB() = default;
		AABB(glm::vec3 const& min, glm::vec3 const& max)
			: Min(min),
			  Max(max)
		{
		}

		bool IsValid() const { return Min.x <= Max.x && Min.y <= Max.y && Min.z <= Max.z; }
		glm::vec3 GetCenter() const { return (Min + Max) * 0.5f; }
		glm::vec3 GetSize() const { return Max - Min; }
		glm::vec3 GetExtents() const { return (Max - Min) * 0.5f; }

		void Expand(glm::vec3 const& point)
		{
			Min = glm::min(Min, point);
			Max = glm::max(Max, point);
		}

		void Expand(AABB const& other)
		{
			if (other.IsValid())
			{
				Min = glm::min(Min, other.Min);
				Max = glm::max(Max, other.Max);
			}
		}

		bool Contains(glm::vec3 const& point) const
		{
			return glm::all(glm::greaterThanEqual(point, Min)) && glm::all(glm::lessThanEqual(point, Max));
		}

		// Smallest box containing this box after an affine transform. Empty boxes stay empty.
		AABB Transform(glm::mat4 const& transform) const;

		bool operator==(AABB const& other) const = default;
	};
}

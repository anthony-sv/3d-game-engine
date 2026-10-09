#pragma once

// Internal to the Physics module: Jolt types stay out of every other header.

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystem.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Strada::Jolt
{
	// The job system of PhysicsSystem (valid while it is initialized).
	JPH::JobSystem& GetJobSystem();

	inline JPH::Vec3 ToJolt(glm::vec3 const& value)
	{
		return JPH::Vec3(value.x, value.y, value.z);
	}

	inline JPH::Quat ToJolt(glm::quat const& value)
	{
		return JPH::Quat(value.x, value.y, value.z, value.w);
	}

	inline glm::vec3 ToGlm(JPH::Vec3 const& value)
	{
		return glm::vec3(value.GetX(), value.GetY(), value.GetZ());
	}

	inline glm::quat ToGlm(JPH::Quat const& value)
	{
		return glm::quat(value.GetW(), value.GetX(), value.GetY(), value.GetZ());
	}
}

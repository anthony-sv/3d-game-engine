#include "stpch.h"
#include "Strada/Renderer/InstanceMath.h"

namespace Strada::Instancing
{
	std::array<glm::vec4, 3> ComputeNormalBasis(glm::mat4 const& model)
	{
		glm::vec3 const x(model[0]);
		glm::vec3 const y(model[1]);
		glm::vec3 const z(model[2]);
		// The inverse of a matrix with columns x, y, z has the rows (y x z, z x x, x x y) / det, so its transpose has them as
		// columns.
		glm::vec3 const cofactorX = glm::cross(y, z);
		glm::vec3 const cofactorY = glm::cross(z, x);
		glm::vec3 const cofactorZ = glm::cross(x, y);
		float const sign = glm::dot(x, cofactorX) < 0.0f ? -1.0f : 1.0f;
		return {glm::vec4(cofactorX * sign, 0.0f), glm::vec4(cofactorY * sign, 0.0f), glm::vec4(cofactorZ * sign, 0.0f)};
	}

	ShaderInterop::InstanceData MakeInstanceData(glm::mat4 const& model, glm::mat4 const& previousModel, uint32_t entityId)
	{
		ShaderInterop::InstanceData data{};
		data.Model = model;
		std::array<glm::vec4, 3> const basis = ComputeNormalBasis(model);
		data.NormalBasis[0] = basis[0];
		data.NormalBasis[1] = basis[1];
		data.NormalBasis[2] = basis[2];
		data.EntityId = entityId;
		data.PreviousModel = previousModel;
		return data;
	}
}

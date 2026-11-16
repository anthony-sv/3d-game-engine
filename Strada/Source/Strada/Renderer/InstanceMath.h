#pragma once

#include "RendererInterop.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

// The per-instance data of the scene renderer (ShaderInterop::InstanceData, Docs/Architecture.md §6.2, "Instancing"),
// computed on the CPU once per instance for every pass that draws it.
namespace Strada::Instancing
{
	// The columns of the cofactor matrix of model's upper 3x3 times the sign of its determinant: the inverse transpose
	// scaled by the determinant's absolute value, which transforms normals to the directions the inverse transpose gives
	// once they are normalized. Needs no division, so it is finite for every finite matrix, a singular one included (a
	// zero determinant counts as positive). W is 0.
	std::array<glm::vec4, 3> ComputeNormalBasis(glm::mat4 const& model);

	// An instance's data: its matrices, its normal basis and its entity ID (the picking ID with
	// ShaderInterop::EntityIdSelectedBit, 0 when not pickable).
	ShaderInterop::InstanceData MakeInstanceData(glm::mat4 const& model, glm::mat4 const& previousModel, uint32_t entityId);
}

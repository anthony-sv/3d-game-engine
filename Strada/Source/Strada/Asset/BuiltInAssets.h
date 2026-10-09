#pragma once

#include "Strada/Asset/AssetHandle.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Strada
{
	// Assets available in every project, referenced as "builtin://<Name>" or by their fixed handles (all below
	// AssetHandle::ReservedCount, so they never collide with generated handles).
	enum class BuiltInAsset : uint64_t
	{
		CubeMesh = 1,
		SphereMesh = 2,
		PlaneMesh = 3,
		CylinderMesh = 4,
		CapsuleMesh = 5,
		ConeMesh = 6,
		QuadMesh = 7,
		DefaultMaterial = 100,
		WhiteTexture = 200,
		BlackTexture = 201,
		// (0.5, 0.5, 1.0): a tangent-space normal map without perturbation.
		FlatNormalTexture = 202
	};

	constexpr AssetHandle GetBuiltInHandle(BuiltInAsset asset)
	{
		return AssetHandle(UUID(static_cast<uint64_t>(asset)));
	}

	// The <Name> part of "builtin://<Name>" (e.g. "Cube", "DefaultMaterial", "White").
	std::string_view GetBuiltInAssetName(BuiltInAsset asset);
	std::span<BuiltInAsset const> GetDefaultBuiltInAssets();
}

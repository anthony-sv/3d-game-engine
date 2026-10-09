#include "stpch.h"
#include "Strada/Asset/BuiltInAssets.h"

#include <array>

namespace Strada
{
	namespace
	{
		constexpr std::array<BuiltInAsset, 13> s_DefaultBuiltInAssets = {
			BuiltInAsset::CubeMesh,     BuiltInAsset::SphereMesh,   BuiltInAsset::PlaneMesh,         BuiltInAsset::CylinderMesh,
			BuiltInAsset::CapsuleMesh,  BuiltInAsset::ConeMesh,     BuiltInAsset::QuadMesh,          BuiltInAsset::DefaultMaterial,
			BuiltInAsset::WhiteTexture, BuiltInAsset::BlackTexture, BuiltInAsset::FlatNormalTexture, BuiltInAsset::DefaultFont,
			BuiltInAsset::DefaultSky,
		};
	}

	std::string_view GetBuiltInAssetName(BuiltInAsset asset)
	{
		switch (asset)
		{
			case BuiltInAsset::CubeMesh:
				return "Cube";
			case BuiltInAsset::SphereMesh:
				return "Sphere";
			case BuiltInAsset::PlaneMesh:
				return "Plane";
			case BuiltInAsset::CylinderMesh:
				return "Cylinder";
			case BuiltInAsset::CapsuleMesh:
				return "Capsule";
			case BuiltInAsset::ConeMesh:
				return "Cone";
			case BuiltInAsset::QuadMesh:
				return "Quad";
			case BuiltInAsset::DefaultMaterial:
				return "DefaultMaterial";
			case BuiltInAsset::WhiteTexture:
				return "White";
			case BuiltInAsset::BlackTexture:
				return "Black";
			case BuiltInAsset::FlatNormalTexture:
				return "FlatNormal";
			case BuiltInAsset::DefaultFont:
				return "DefaultFont";
			case BuiltInAsset::DefaultSky:
				return "DefaultSky";
		}
		return "Unknown";
	}

	std::span<BuiltInAsset const> GetDefaultBuiltInAssets()
	{
		return s_DefaultBuiltInAssets;
	}
}

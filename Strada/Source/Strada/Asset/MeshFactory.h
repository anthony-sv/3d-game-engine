#pragma once

#include "Strada/Asset/MeshSource.h"

namespace Strada::MeshFactory
{
	// Primitive meshes with one submesh and one material slot, centered at the origin, counter-clockwise front faces,
	// UV origin at the top-left. Sizes match the default collider dimensions.

	// 1 x 1 x 1 box with per-face UVs.
	Ref<MeshSource> CreateCube(AssetHandle material);
	// Radius 0.5 UV sphere.
	Ref<MeshSource> CreateSphere(AssetHandle material, uint32_t segments = 48, uint32_t rings = 24);
	// 1 x 1 plane in XZ facing +Y.
	Ref<MeshSource> CreatePlane(AssetHandle material);
	// Radius 0.5, height 1 cylinder along Y with caps.
	Ref<MeshSource> CreateCylinder(AssetHandle material, uint32_t segments = 48);
	// Radius 0.5 capsule along Y with a cylinder half-height of 0.5 (total height 2).
	Ref<MeshSource> CreateCapsule(AssetHandle material, uint32_t segments = 48, uint32_t hemisphereRings = 12);
	// Radius 0.5, height 1 cone along Y (base at y = -0.5) with a base cap.
	Ref<MeshSource> CreateCone(AssetHandle material, uint32_t segments = 48);
	// 1 x 1 quad in XY facing +Z (sprites, billboards).
	Ref<MeshSource> CreateQuad(AssetHandle material);
}

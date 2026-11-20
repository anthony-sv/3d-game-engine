#pragma once

#include "Strada/Asset/AssetHandle.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

// The physics material (Docs/Architecture.md §8, "Physics materials"): how a surface slides, bounces and rolls, the name
// scripts tell it by, and what an impact on it sounds and looks like. The data of a .sphysmat asset
// (Strada/Asset/PhysicsMaterialAsset.h has its field table and registration); colliders reference one. Plain data, any
// thread.
namespace Strada
{
	// How the two sides of a contact combine a value (friction or restitution). When the sides name different modes, the
	// one later in this list wins, so the order is part of the format: Multiply on either side multiplies unless the other
	// says Maximum.
	enum class PhysicsMaterialCombine : uint8_t
	{
		// The square root of the product: friction's rule for colliders without a material.
		GeometricMean = 0,
		// Half the sum.
		Average,
		// The smaller value.
		Minimum,
		// The product.
		Multiply,
		// The larger value: restitution's rule for colliders without a material.
		Maximum
	};

	struct PhysicsMaterial
	{
		// The friction coefficient (0 slides freely; a collider's default is 0.6).
		float Friction = 0.6f;
		// Bounciness: 0 does not bounce, 1 keeps all of the speed along the normal.
		float Restitution = 0.0f;
		PhysicsMaterialCombine FrictionCombine = PhysicsMaterialCombine::GeometricMean;
		PhysicsMaterialCombine RestitutionCombine = PhysicsMaterialCombine::Maximum;
		// The rolling resistance coefficient: the force that opposes a wheel rolling on the surface, per newton of load
		// (about 0.01 for a tyre on asphalt). A vehicle's wheels slow by it under their load (Docs/Architecture.md §8,
		// "Vehicles"), and scripts and graphs read it; rigid-body contacts ignore it.
		float RollingResistance = 0.0f;
		// What a vehicle's tyres multiply their friction curves by on the surface (at least 0; 1 keeps the tyres as they are).
		// Read by vehicles alone: rigid-body contacts take Friction, so a surface can hold blocks and tyres differently (a
		// gravel trap with a high Friction and a low TyreGrip).
		float TyreGrip = 1.0f;
		// The surface's name ("Asphalt", "Gravel"), which scripts and graphs compare; empty for none.
		std::string Surface;
		// An AudioClip for impacts on the surface, and a Prefab (usually a ParticleSystem) spawned where they happen. A
		// collision that begins plays each side's ImpactSound at its strongest point when the project's mixer names a bus for
		// impact sounds (Mixer::Impacts, Docs/Architecture.md §9.8; none by default); scripts and graphs read both, and
		// spawn the particles: contacts spawn nothing themselves.
		AssetHandle ImpactSound;
		AssetHandle ImpactParticles;

		bool operator==(PhysicsMaterial const&) const = default;
	};

	// The value a contact uses for two sides' values and modes: the mode later in PhysicsMaterialCombine's order wins.
	// Symmetric in the two sides. With GeometricMean it is exactly std::sqrt(first * second) and with Maximum exactly
	// std::max(first, second), the expressions the contact listener used before materials existed, so colliders without a
	// material simulate bit for bit as they did. Values are finite and not negative. Any thread.
	inline float CombinePhysicsMaterialValues(PhysicsMaterialCombine firstMode, float first, PhysicsMaterialCombine secondMode,
	                                          float second)
	{
		switch (std::max(firstMode, secondMode))
		{
			case PhysicsMaterialCombine::GeometricMean:
				return std::sqrt(first * second);
			case PhysicsMaterialCombine::Average:
				return (first + second) * 0.5f;
			case PhysicsMaterialCombine::Minimum:
				return std::min(first, second);
			case PhysicsMaterialCombine::Multiply:
				return first * second;
			case PhysicsMaterialCombine::Maximum:
				return std::max(first, second);
		}
		return std::max(first, second);
	}
}

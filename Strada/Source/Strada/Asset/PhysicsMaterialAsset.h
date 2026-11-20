#pragma once

#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructDocument.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <string_view>
#include <tuple>
#include <utility>

// The PhysicsMaterial data asset type (.sphysmat, Docs/Architecture.md §8 and §7.3): the engine's PhysicsMaterial
// (Strada/Asset/PhysicsMaterial.h) through the data asset registration (§6.4). Every reader of physics material assets
// includes this header; code that only combines values includes PhysicsMaterial.h.
namespace Strada
{
	template<>
	struct EnumTraits<PhysicsMaterialCombine>
	{
		static constexpr std::array<std::pair<PhysicsMaterialCombine, std::string_view>, 5> Values = {{
			{PhysicsMaterialCombine::GeometricMean, "GeometricMean"},
			{PhysicsMaterialCombine::Average, "Average"},
			{PhysicsMaterialCombine::Minimum, "Minimum"},
			{PhysicsMaterialCombine::Multiply, "Multiply"},
			{PhysicsMaterialCombine::Maximum, "Maximum"},
		}};
	};

	template<>
	struct StructTraits<PhysicsMaterial>
	{
		static constexpr std::string_view Name = "PhysicsMaterial";
		static constexpr auto Fields = std::make_tuple(
			Field("Friction", &PhysicsMaterial::Friction)
				.AtLeast(0.0)
				.Doc("Resists sliding along other colliders: 0 slides freely, 1 holds about as well as rubber on concrete."),
			Field("Restitution", &PhysicsMaterial::Restitution)
				.Range(0.0, 1.0)
				.Doc("Bounciness: 0 does not bounce, 1 keeps all of the speed along the contact's normal."),
			Field("FrictionCombine", &PhysicsMaterial::FrictionCombine)
				.Doc("How a contact combines the frictions of its two sides: GeometricMean (a collider without a material's rule), "
		             "Average, Minimum, Multiply or Maximum. When the sides differ, the one later in that list wins."),
			Field("RestitutionCombine", &PhysicsMaterial::RestitutionCombine)
				.Doc("How a contact combines the restitutions of its two sides: GeometricMean, Average, Minimum, Multiply or Maximum "
		             "(a collider without a material's rule). When the sides differ, the one later in that list wins."),
			Field("RollingResistance", &PhysicsMaterial::RollingResistance)
				.AtLeast(0.0)
				.Doc("The force opposing a wheel that rolls on the surface, per newton of load (about 0.01 for a tyre on asphalt). "
		             "A vehicle's wheels slow by it under their load, and scripts and graphs read it; rigid bodies roll without it."),
			Field("TyreGrip", &PhysicsMaterial::TyreGrip)
				.AtLeast(0.0)
				.Doc("What a vehicle's tyres multiply their friction by on the surface: 1 keeps the tyres as they are, lower slides "
		             "sooner (gravel, grass). Read by vehicles alone; rigid bodies take Friction."),
			Field("Surface", &PhysicsMaterial::Surface)
				.Doc("The surface's name (Asphalt, Gravel, Ice), which scripts and graphs compare to tell surfaces apart; empty for "
		             "none."),
			Field("ImpactSound", &PhysicsMaterial::ImpactSound)
				.References("AudioClip")
				.Doc("A sound for impacts on the surface: a collision that begins plays it where it begins when the project's mixer "
		             "names a bus for impact sounds (its Impacts), louder for a stronger impact; scripts and graphs can play it too."),
			Field("ImpactParticles", &PhysicsMaterial::ImpactParticles)
				.References("Prefab")
				.Doc("A prefab (usually a particle system) for impacts on the surface, which scripts and graphs spawn; contacts spawn "
		             "nothing by themselves."));
	};

	inline constexpr std::array<DocumentMigration, 0> PhysicsMaterialMigrations = {};

	inline constexpr DataAssetTypeSpecification<PhysicsMaterial> PhysicsMaterialAssetSpecification = {
		.Name = "PhysicsMaterial",
		.Extension = ".sphysmat",
		.FormatVersion = 1,
		.Description = "How a surface slides, bounces and rolls, the name scripts tell it by, and what impacts on it sound and look like.",
		.Migrations = PhysicsMaterialMigrations,
		.Validate = nullptr,
	};
}

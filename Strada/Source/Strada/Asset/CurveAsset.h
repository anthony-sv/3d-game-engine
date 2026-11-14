#pragma once

#include "Strada/Asset/DataAsset.h"
#include "Strada/Math/Curve.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructDocument.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <string_view>
#include <tuple>
#include <utility>

// The Curve data asset type (.scurve, Docs/Architecture.md §6.5 and §7.3): the engine's Curve (Strada/Math/Curve.h) through
// the data asset registration (§6.4). Every user of curve assets and of curves embedded in other data includes this header.
namespace Strada
{
	template<>
	struct EnumTraits<CurveInterpolation>
	{
		static constexpr std::array<std::pair<CurveInterpolation, std::string_view>, 3> Values = {{
			{CurveInterpolation::Cubic, "Cubic"},
			{CurveInterpolation::Linear, "Linear"},
			{CurveInterpolation::Constant, "Constant"},
		}};
	};

	template<>
	struct EnumTraits<CurveExtrapolation>
	{
		static constexpr std::array<std::pair<CurveExtrapolation, std::string_view>, 4> Values = {{
			{CurveExtrapolation::Hold, "Hold"},
			{CurveExtrapolation::Loop, "Loop"},
			{CurveExtrapolation::PingPong, "PingPong"},
			{CurveExtrapolation::Linear, "Linear"},
		}};
	};

	template<>
	struct StructTraits<CurveKey>
	{
		static constexpr std::string_view Name = "CurveKey";
		static constexpr auto Fields = std::make_tuple(
			Field("Time", &CurveKey::Time)
				.Doc("Where on the curve's input the key lies (seconds, a speed, a distance); keys are in time order, and several "
		             "keys at one time make a jump."),
			Field("Value", &CurveKey::Value).Doc("The curve's value at the key."),
			Field("Interpolation", &CurveKey::Interpolation)
				.Doc("How the segment to the next key interpolates: Cubic (smooth, with the slopes), Linear or Constant (a step)."),
			Field("HasIn", &CurveKey::HasIn).Doc("Whether In is set; otherwise the slope into the key is automatic."),
			Field("In", &CurveKey::In)
				.EnabledBy("HasIn")
				.Doc("The slope the curve enters the key with, in value units per input unit (a Cubic segment's end)."),
			Field("HasOut", &CurveKey::HasOut).Doc("Whether Out is set; otherwise the slope out of the key is automatic."),
			Field("Out", &CurveKey::Out)
				.EnabledBy("HasOut")
				.Doc("The slope the curve leaves the key with, in value units per input unit (a Cubic segment's start)."));
	};

	template<>
	struct StructTraits<Curve>
	{
		static constexpr std::string_view Name = "Curve";
		static constexpr auto Fields = std::make_tuple(
			Field("PreExtrapolation", &Curve::PreExtrapolation)
				.Doc("What the curve does before its first key: Hold its value, Loop or PingPong the keys, or go on Linear along its "
		             "slope."),
			Field("PostExtrapolation", &Curve::PostExtrapolation)
				.Doc("What the curve does after its last key: Hold its value, Loop or PingPong the keys, or go on Linear along its "
		             "slope."),
			Field("Keys", &Curve::Keys).MaxCount(MaxCurveKeys).Doc("The keys, in time order. Without keys the curve is 0."));
	};

	inline constexpr std::array<DocumentMigration, 0> CurveMigrations = {};

	inline constexpr DataAssetTypeSpecification<Curve> CurveAssetSpecification = {
		.Name = "Curve",
		.Extension = ".scurve",
		.FormatVersion = 1,
		.Description = "A curve of keys with their interpolation and slopes, and what it does before and after them.",
		.Migrations = CurveMigrations,
		.Validate = &ValidateCurve,
	};
}

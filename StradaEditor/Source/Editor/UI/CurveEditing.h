#pragma once

#include "Editor/UI/CurveView.h"

#include "Strada/Core/Result.h"
#include "Strada/Math/Curve.h"

#include <glm/glm.hpp>

#include <cfloat>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The curve editor's edits of the engine's scalar Curve (Strada/Math/Curve.h, the data of a .scurve asset), and the Curve as a
// document of the shared curve view (CurveView.h): one curve track named "Curve" over an unbounded domain (a key's time is any
// finite number), with no snapping step of its own. Each edit checks before it changes and leaves the curve unchanged when it
// fails; keys stay in time order, finite and at most MaxCurveKeys, so what an edit makes passes ValidateCurve. Keys are named
// by CurveKeyRef with track 0. A slope that is not set keeps the number it holds (HasIn false, In kept), so edits that do not
// touch a key leave every field of it as it was. Pure functions of values: any thread.
namespace Strada
{
	// The name of a Curve's one track in the curve editor's descriptions ("Move Key of Curve on Torque").
	inline constexpr std::string_view CurveEditorTrackName = "Curve";

	// Moves the selected keys by one total offset, as a block: the time offset is limited so that no selected key passes an
	// unselected neighbour or leaves the finite floats, so the selection keeps its spacing and keys stay sorted (and keep their
	// indices); valueOffset is added to their values. Callers pass the curve at the start of the interaction and the total
	// offset so far, never the previous frame's result.
	[[nodiscard]] Result<void> MoveCurveKeys(Curve& curve, std::span<CurveKeyRef const> keys, float timeOffset, float valueOffset);
	// Adds a key at a finite time (after the keys at that time) with a finite value and automatic slopes; returns its index.
	[[nodiscard]] Result<uint32_t> AddCurveKey(Curve& curve, float time, float value);
	[[nodiscard]] Result<void> RemoveCurveKeys(Curve& curve, std::span<CurveKeyRef const> keys);
	[[nodiscard]] Result<void> SetCurveKeyValue(Curve& curve, uint32_t key, float value);
	[[nodiscard]] Result<void> SetCurveKeyInterpolation(Curve& curve, std::span<CurveKeyRef const> keys, CurveInterpolation interpolation);
	// Sets a key's slopes (nothing: automatic, the number held kept).
	[[nodiscard]] Result<void> SetCurveKeyTangents(Curve& curve, uint32_t key, std::optional<float> in, std::optional<float> out);
	// Makes the slopes of the keys flat (set to 0) or automatic.
	[[nodiscard]] Result<void> FlattenCurveTangents(Curve& curve, std::span<CurveKeyRef const> keys);
	[[nodiscard]] Result<void> ResetCurveTangents(Curve& curve, std::span<CurveKeyRef const> keys);

	template<>
	struct CurveDocumentTraits<Curve>
	{
		struct Intent
		{
			CurveIntentKind Kind = CurveIntentKind::Commit;
			// Edit and Commit: the curve to set; CancelEdit: the curve at the drag's start.
			Curve Document;
			std::string Action;
		};

		// Snapping and nudges follow the view: a hundredth of the visible span.
		static constexpr float TimeStep = 0.0f;
		// A curve's input may be seconds, a speed or revolutions per minute: the time axis has the value axis's bounds.
		static constexpr double MinTimeSpan = 1e-4;
		static constexpr double MaxTimeSpan = 1.0e39;
		static constexpr double MaxTimeMagnitude = 1.0e39;
		static constexpr char const* TimeFormat = "%.4g";

		static Curve& GetDocument(Intent& intent) { return intent.Document; }

		static CurveDomain GetDomain(Curve const&) { return {-static_cast<double>(FLT_MAX), static_cast<double>(FLT_MAX), false}; }
		static uint32_t GetTrackCount(Curve const&) { return 1; }
		static std::string_view GetTrackName(Curve const&, uint32_t) { return CurveEditorTrackName; }
		static uint32_t GetComponents(Curve const&, uint32_t) { return 1; }
		static uint32_t GetKeyCount(Curve const& curve, uint32_t) { return static_cast<uint32_t>(curve.Keys.size()); }
		static float GetKeyTime(Curve const& curve, uint32_t, uint32_t key) { return curve.Keys[key].Time; }
		static float GetKeyValue(Curve const& curve, uint32_t, uint32_t key, uint32_t) { return curve.Keys[key].Value; }
		static CurveInterpolation GetKeyInterpolation(Curve const& curve, uint32_t, uint32_t key) { return curve.Keys[key].Interpolation; }
		static std::optional<glm::vec4> GetKeyIn(Curve const& curve, uint32_t, uint32_t key)
		{
			CurveKey const& stored = curve.Keys[key];
			return stored.HasIn ? std::optional<glm::vec4>(glm::vec4(stored.In, 0.0f, 0.0f, 0.0f)) : std::nullopt;
		}
		static std::optional<glm::vec4> GetKeyOut(Curve const& curve, uint32_t, uint32_t key)
		{
			CurveKey const& stored = curve.Keys[key];
			return stored.HasOut ? std::optional<glm::vec4>(glm::vec4(stored.Out, 0.0f, 0.0f, 0.0f)) : std::nullopt;
		}
		static std::pair<glm::vec4, glm::vec4> GetKeySlopes(Curve const& curve, uint32_t, uint32_t key)
		{
			std::pair<float, float> const slopes = curve.GetKeySlopes(key);
			return {glm::vec4(slopes.first, 0.0f, 0.0f, 0.0f), glm::vec4(slopes.second, 0.0f, 0.0f, 0.0f)};
		}
		// The curve's value with its extrapolations: before and after its keys the editor draws what the curve does there.
		static glm::vec4 Evaluate(Curve const& curve, uint32_t, double time) { return glm::vec4(curve.Evaluate(time), 0.0f, 0.0f, 0.0f); }

		[[nodiscard]] static Result<void> MoveKeys(Curve& curve, std::span<CurveKeyRef const> keys, float timeOffset, glm::vec4 valueOffset)
		{
			return MoveCurveKeys(curve, keys, timeOffset, valueOffset.x);
		}
		[[nodiscard]] static Result<uint32_t> AddKey(Curve& curve, uint32_t track, float time, glm::vec4 value);
		[[nodiscard]] static Result<void> RemoveKeys(Curve& curve, std::span<CurveKeyRef const> keys)
		{
			return RemoveCurveKeys(curve, keys);
		}
		[[nodiscard]] static Result<void> SetKeyValue(Curve& curve, CurveKeyRef key, glm::vec4 value);
		[[nodiscard]] static Result<void> SetKeyInterpolation(Curve& curve, std::span<CurveKeyRef const> keys,
		                                                      CurveInterpolation interpolation)
		{
			return SetCurveKeyInterpolation(curve, keys, interpolation);
		}
		[[nodiscard]] static Result<void> SetKeyTangents(Curve& curve, CurveKeyRef key, std::optional<glm::vec4> in,
		                                                 std::optional<glm::vec4> out);
		[[nodiscard]] static Result<void> FlattenTangents(Curve& curve, std::span<CurveKeyRef const> keys)
		{
			return FlattenCurveTangents(curve, keys);
		}
		[[nodiscard]] static Result<void> ResetTangents(Curve& curve, std::span<CurveKeyRef const> keys)
		{
			return ResetCurveTangents(curve, keys);
		}
	};

	// Instantiated once, in CurveEditing.cpp.
	extern template class CurveView<Curve>;
}

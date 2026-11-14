#pragma once

#include "Editor/UI/CurveView.h"
#include "Editor/VisualScripting/VisualScriptTimeline.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The model of the Timeline drawer's curve view (spec-slice3 §9.5): the shared curve view (Editor/UI/CurveView.h) over a
// Flow.Timeline's timeline. The view transform, hit testing and the gestures of §9.5's table turned into intents (timelines
// to commit, made from the timeline at the start of the interaction and the total change so far) are the shared view's; this
// header gives it the timeline through CurveDocumentTraits, whose reads and edits are the timeline's own functions
// (VisualScriptTimeline.h), so the drawer edits exactly as Details and graph.edit's SetTimeline read. Pure (no ImGui): the
// curve editor (VisualScriptCurveEditor) feeds it the mouse each frame, draws from its queries and commits its intents with
// SetTimeline. Main thread only.
namespace Strada
{
	using VisualScriptCurveHitKind = CurveHitKind;
	using VisualScriptCurveHit = CurveHit;
	using VisualScriptCurveTrackView = CurveTrackView;
	using VisualScriptCurveViewInput = CurveViewInput;
	using VisualScriptCurveIntentKind = CurveIntentKind;
	using VisualScriptCurveGesture = CurveGesture;

	struct VisualScriptCurveIntent
	{
		VisualScriptCurveIntentKind Kind = VisualScriptCurveIntentKind::Commit;
		// Edit and Commit: the timeline to set; CancelEdit: the timeline at the drag's start.
		VisualScriptTimeline Timeline;
		// Edit and Commit: the step's description, without the script ("Move Key of Lift on Timeline").
		std::string Action;
	};

	// The timeline's tracks are the curve view's: curve tracks of 1 to 4 components and event tracks (0) on lanes, over the
	// bounded domain [0, Length], with key times snapped to and nudged by 0.01 s.
	template<>
	struct CurveDocumentTraits<VisualScriptTimeline>
	{
		using Intent = VisualScriptCurveIntent;

		static constexpr float TimeStep = 0.01f;
		static constexpr double MinTimeSpan = 0.001;
		static constexpr double MaxTimeSpan = 7200.0;
		static constexpr double MaxTimeMagnitude = 1.0e5;
		static constexpr char const* TimeFormat = "%.3f s";

		static VisualScriptTimeline& GetDocument(Intent& intent) { return intent.Timeline; }

		static CurveDomain GetDomain(VisualScriptTimeline const& timeline) { return {0.0, static_cast<double>(timeline.Length), true}; }
		static uint32_t GetTrackCount(VisualScriptTimeline const& timeline) { return static_cast<uint32_t>(timeline.Tracks.size()); }
		static std::string_view GetTrackName(VisualScriptTimeline const& timeline, uint32_t track) { return timeline.Tracks[track].Name; }
		static uint32_t GetComponents(VisualScriptTimeline const& timeline, uint32_t track) { return timeline.Tracks[track].Components; }
		static uint32_t GetKeyCount(VisualScriptTimeline const& timeline, uint32_t track)
		{
			return static_cast<uint32_t>(timeline.Tracks[track].Keys.size());
		}
		static float GetKeyTime(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key)
		{
			return timeline.Tracks[track].Keys[key].Time;
		}
		static float GetKeyValue(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key, uint32_t component)
		{
			return timeline.Tracks[track].Keys[key].Value[static_cast<glm::length_t>(component)];
		}
		static CurveInterpolation GetKeyInterpolation(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key)
		{
			return timeline.Tracks[track].Keys[key].Interpolation;
		}
		static std::optional<glm::vec4> GetKeyIn(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key)
		{
			return timeline.Tracks[track].Keys[key].In;
		}
		static std::optional<glm::vec4> GetKeyOut(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key)
		{
			return timeline.Tracks[track].Keys[key].Out;
		}
		static std::pair<glm::vec4, glm::vec4> GetKeySlopes(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key)
		{
			return GetVisualScriptTimelineKeyTangents(timeline.Tracks[track], key);
		}
		static glm::vec4 Evaluate(VisualScriptTimeline const& timeline, uint32_t track, double time)
		{
			return EvaluateVisualScriptTimelineTrack(timeline.Tracks[track], time);
		}

		[[nodiscard]] static Result<void> MoveKeys(VisualScriptTimeline& timeline, std::span<CurveKeyRef const> keys, float timeOffset,
		                                           glm::vec4 valueOffset)
		{
			return MoveVisualScriptTimelineKeys(timeline, keys, timeOffset, valueOffset);
		}
		[[nodiscard]] static Result<uint32_t> AddKey(VisualScriptTimeline& timeline, uint32_t track, float time, glm::vec4 value)
		{
			return AddVisualScriptTimelineKey(timeline, track, time, value);
		}
		[[nodiscard]] static Result<void> RemoveKeys(VisualScriptTimeline& timeline, std::span<CurveKeyRef const> keys)
		{
			return RemoveVisualScriptTimelineKeys(timeline, keys);
		}
		[[nodiscard]] static Result<void> SetKeyValue(VisualScriptTimeline& timeline, CurveKeyRef key, glm::vec4 value)
		{
			return SetVisualScriptTimelineKeyValue(timeline, key, value);
		}
		[[nodiscard]] static Result<void> SetKeyInterpolation(VisualScriptTimeline& timeline, std::span<CurveKeyRef const> keys,
		                                                      CurveInterpolation interpolation)
		{
			return SetVisualScriptTimelineKeyInterpolation(timeline, keys, interpolation);
		}
		[[nodiscard]] static Result<void> SetKeyTangents(VisualScriptTimeline& timeline, CurveKeyRef key, std::optional<glm::vec4> in,
		                                                 std::optional<glm::vec4> out)
		{
			return SetVisualScriptTimelineKeyTangents(timeline, key, in, out);
		}
		[[nodiscard]] static Result<void> FlattenTangents(VisualScriptTimeline& timeline, std::span<CurveKeyRef const> keys)
		{
			return FlattenVisualScriptTimelineTangents(timeline, keys);
		}
		[[nodiscard]] static Result<void> ResetTangents(VisualScriptTimeline& timeline, std::span<CurveKeyRef const> keys)
		{
			return ResetVisualScriptTimelineTangents(timeline, keys);
		}
	};

	// Instantiated once, in VisualScriptCurveView.cpp.
	extern template class CurveView<VisualScriptTimeline>;

	using VisualScriptCurveView = CurveView<VisualScriptTimeline>;
}

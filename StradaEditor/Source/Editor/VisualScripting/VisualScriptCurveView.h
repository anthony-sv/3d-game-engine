#pragma once

#include "Editor/VisualScripting/VisualScriptTimeline.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The model of the Timeline drawer's curve view (spec-slice3 §9.5): the view transform (time and value ranges to a rectangle
// in pixels; zoom about a point, pan, frame), hit testing (keys, tangent handles, event lanes, curves, empty space, in that
// order) and the gestures of §9.5's table turned into intents: timelines to commit, made from the timeline at the start of
// the interaction and the total change so far. Pure (no ImGui): the curve editor (VisualScriptCurveEditor) feeds it the
// mouse each frame, draws from its queries and commits its intents with SetTimeline. Main thread only.
namespace Strada
{
	enum class VisualScriptCurveHitKind : uint8_t
	{
		// Outside the view.
		None = 0,
		// A key's diamond (Component: the curve's component it stands for; 0 on an event lane).
		Key,
		// A selected cubic key's tangent handles.
		InTangent,
		OutTangent,
		// An event track's lane, off its diamonds.
		EventLane,
		// Near a curve, off its keys.
		Curve,
		// Anywhere else in the view.
		Empty
	};

	struct VisualScriptCurveHit
	{
		VisualScriptCurveHitKind Kind = VisualScriptCurveHitKind::None;
		uint32_t Track = 0;
		uint32_t Key = 0;
		uint32_t Component = 0;
		// The time and the value under the point.
		float Time = 0.0f;
		float Value = 0.0f;

		bool operator==(VisualScriptCurveHit const& other) const = default;
	};

	// What the view shows of a track: the eye, and per component (X Y Z W, R G B A) its toggle.
	struct VisualScriptCurveTrackView
	{
		bool Visible = true;
		std::array<bool, 4> Components{true, true, true, true};

		bool operator==(VisualScriptCurveTrackView const& other) const = default;
	};

	// The mouse over the view in a frame, in screen pixels.
	struct VisualScriptCurveViewInput
	{
		glm::vec2 Mouse{0.0f};
		// The mouse is over the view and nothing drawn on top of it.
		bool Hovered = false;
		// Per button (left, right, middle): pressed this frame, held, released this frame.
		std::array<bool, 3> Pressed{};
		std::array<bool, 3> Down{};
		std::array<bool, 3> Released{};
		// The left press of this frame is a double-click.
		bool DoubleClicked = false;
		bool Shift = false;
		bool Ctrl = false;
		bool Alt = false;
		// Wheel notches (up is positive), while hovered.
		float Wheel = 0.0f;
	};

	enum class VisualScriptCurveIntentKind : uint8_t
	{
		// A frame of a drag: the timeline the drag has made so far (from its start); one undo step until EndEdit.
		Edit = 0,
		// The drag ended: its last value commits.
		EndEdit,
		// A discrete edit, one step of its own.
		Commit,
		// Open the menu of the selected keys (Interpolation, Tangents, Delete).
		OpenKeyMenu
	};

	struct VisualScriptCurveIntent
	{
		VisualScriptCurveIntentKind Kind = VisualScriptCurveIntentKind::Commit;
		// Edit and Commit: the timeline to set.
		VisualScriptTimeline Timeline;
		// Edit and Commit: the step's description, without the script ("Move Key of Lift on Timeline").
		std::string Action;
	};

	// The gesture in progress.
	enum class VisualScriptCurveGesture : uint8_t
	{
		Idle = 0,
		// A left press that has not moved past the threshold yet.
		Press,
		MoveKeys,
		Tangent,
		Marquee,
		// A right or middle press (a right release without moving opens the key menu).
		PanPress,
		Pan
	};

	class VisualScriptCurveView
	{
	public:
		// Key times snap to this step while dragged and are nudged by it.
		static constexpr float TimeStep = 0.01f;
		// Sizes in font sizes.
		static constexpr float RulerHeight = 1.4f;
		static constexpr float LaneHeight = 1.25f;
		static constexpr float KeyRadius = 0.35f;
		static constexpr float HitRadius = 0.5f;
		static constexpr float HandleLength = 2.5f;
		static constexpr float CurveHitRadius = 0.4f;
		static constexpr float DragThreshold = 0.25f;
		// A wheel notch zooms by this factor.
		static constexpr float ZoomStep = 1.2f;
		// The bounds of the visible spans.
		static constexpr float MinTimeSpan = 0.001f;
		static constexpr float MaxTimeSpan = 7200.0f;
		static constexpr float MinValueSpan = 1e-4f;
		static constexpr float MaxValueSpan = 1e9f;

		// The view's rectangle in screen pixels and the font size its parts scale with.
		void SetRect(glm::vec2 min, glm::vec2 max, float fontSize);
		glm::vec2 GetMin() const { return m_Min; }
		glm::vec2 GetMax() const { return m_Max; }
		float GetFontSize() const { return m_FontSize; }

		// Takes the timeline shown this frame: keeps the selection's keys that still exist, the eye and component toggles
		// of the tracks by name, and frames everything the first time.
		void Sync(VisualScriptTimeline const& timeline);

		// The view transform.
		glm::vec2 GetTimeRange() const { return {m_TimeStart, m_TimeEnd}; }
		glm::vec2 GetValueRange() const { return {m_ValueStart, m_ValueEnd}; }
		void SetTimeRange(float start, float end);
		void SetValueRange(float start, float end);
		float ToScreenX(double time) const;
		float ToScreenY(double value) const;
		double ToTime(float x) const;
		double ToValue(float y) const;
		// The lanes of the visible event tracks under the ruler, then the curves' area.
		float GetLanesTop() const;
		float GetCurvesTop() const;
		// The lane of an event track (top and bottom); nothing for a hidden or curve track.
		std::optional<std::pair<float, float>> GetLane(uint32_t track) const;
		// The screen position of a key's diamond for a component (event keys: on their lane).
		std::optional<glm::vec2> GetKeyPosition(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key,
		                                        uint32_t component) const;
		// Where a selected cubic key's tangent handles are for a component; nothing for the side it has none of.
		std::optional<glm::vec2> GetTangentHandle(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key, uint32_t component,
		                                          bool out) const;
		// Whether a key shows tangent handles: a selected curve key whose segment is cubic.
		bool HasTangentHandles(VisualScriptTimeline const& timeline, uint32_t track, uint32_t key) const;

		void ZoomTime(float factor, float aboutX);
		void ZoomValues(float factor, float aboutY);
		void Pan(glm::vec2 pixels);
		// Frames the selected keys, or every visible key and curve without a selection (and the whole length).
		void Frame(VisualScriptTimeline const& timeline);

		VisualScriptCurveHit HitTest(VisualScriptTimeline const& timeline, glm::vec2 point) const;

		// The eye and component toggles: hidden tracks and components are neither hit nor framed nor selected by Ctrl+A.
		VisualScriptCurveTrackView GetTrackView(std::string_view track) const;
		void SetTrackVisible(std::string_view track, bool visible);
		void SetComponentVisible(std::string_view track, uint32_t component, bool visible);
		bool IsVisible(VisualScriptTimeline const& timeline, uint32_t track, uint32_t component) const;

		std::span<VisualScriptTimelineKeyRef const> GetSelection() const { return m_Selection; }
		bool IsSelected(uint32_t track, uint32_t key) const;
		void Select(std::vector<VisualScriptTimelineKeyRef> keys);
		void ClearSelection() { m_Selection.clear(); }

		VisualScriptCurveGesture GetGesture() const { return m_Gesture; }
		// The marquee's rectangle while one is drawn.
		std::optional<std::pair<glm::vec2, glm::vec2>> GetMarquee() const;

		// A frame of the mouse over the view, with the timeline shown and the node's title (which descriptions end with).
		std::vector<VisualScriptCurveIntent> Update(VisualScriptCurveViewInput const& input, VisualScriptTimeline const& timeline,
		                                            std::string_view title);

		// The keys of §9.5's table. Ctrl+A selects every key of the visible tracks.
		void SelectAll(VisualScriptTimeline const& timeline);
		// Delete: removes the selected keys.
		std::optional<VisualScriptCurveIntent> DeleteSelection(VisualScriptTimeline const& timeline, std::string_view title);
		// The arrows: moves the selected keys by steps of TimeStep (Shift: 10 steps) as a block.
		std::optional<VisualScriptCurveIntent> Nudge(VisualScriptTimeline const& timeline, int steps, std::string_view title);
		// The key menu's items and the header row's buttons, on the selected keys.
		std::optional<VisualScriptCurveIntent> SetInterpolation(VisualScriptTimeline const& timeline,
		                                                        VisualScriptKeyInterpolation interpolation, std::string_view title);
		std::optional<VisualScriptCurveIntent> FlattenTangents(VisualScriptTimeline const& timeline, std::string_view title);
		std::optional<VisualScriptCurveIntent> AutoTangents(VisualScriptTimeline const& timeline, std::string_view title);
		// Adds a key to a track at a time (snapped unless free) with the curve's value there, and selects it.
		std::optional<VisualScriptCurveIntent> AddKey(VisualScriptTimeline const& timeline, uint32_t track, double time, bool free,
		                                              std::string_view title);
		// Ends the gesture in progress: a drag's last value commits (EndEdit).
		std::vector<VisualScriptCurveIntent> CancelGesture();

		// "Move Key of Lift on Timeline", "Move 3 Keys on Timeline": what an edit of the selected keys is called.
		static std::string DescribeKeys(VisualScriptTimeline const& timeline, std::span<VisualScriptTimelineKeyRef const> keys,
		                                std::string_view verb, std::string_view title, std::string_view suffix = {});

	private:
		struct NamedTrackView
		{
			std::string Name;
			VisualScriptCurveTrackView View;
		};

		void BeginPress(VisualScriptCurveViewInput const& input, VisualScriptTimeline const& timeline, std::string_view title,
		                std::vector<VisualScriptCurveIntent>& intents);
		void UpdateDrag(VisualScriptCurveViewInput const& input, std::string_view title, std::vector<VisualScriptCurveIntent>& intents);
		void UpdateMarquee(VisualScriptTimeline const& timeline);
		std::vector<VisualScriptTimelineKeyRef> FindKeysIn(VisualScriptTimeline const& timeline, glm::vec2 min, glm::vec2 max) const;
		float GetPixelsPerSecond() const;
		float GetPixelsPerValue() const;
		double Snap(double time) const;

		glm::vec2 m_Min{0.0f};
		glm::vec2 m_Max{1.0f};
		float m_FontSize = 13.0f;
		float m_TimeStart = 0.0f;
		float m_TimeEnd = 1.0f;
		float m_ValueStart = -1.0f;
		float m_ValueEnd = 1.0f;
		bool m_Framed = false;
		// The visible event tracks, top to bottom (their lanes).
		std::vector<uint32_t> m_Lanes;
		std::vector<NamedTrackView> m_TrackViews;
		std::vector<VisualScriptTimelineKeyRef> m_Selection;

		VisualScriptCurveGesture m_Gesture = VisualScriptCurveGesture::Idle;
		int m_Button = 0;
		glm::vec2 m_PressMouse{0.0f};
		glm::vec2 m_LastMouse{0.0f};
		VisualScriptCurveHit m_PressHit;
		bool m_PressShift = false;
		bool m_PressCtrl = false;
		// The interaction's start: the timeline, the selection, and what the drag was last at.
		VisualScriptTimeline m_Start;
		std::vector<VisualScriptTimelineKeyRef> m_StartSelection;
		std::optional<VisualScriptTimeline> m_LastEdit;
	};
}

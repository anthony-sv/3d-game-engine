#pragma once

#include "Strada/Math/Curve.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The model of the curve editor (Docs/Architecture.md §6.5 and §11.1), shared by its hosts: the Timeline drawer of the visual
// script editor (over a Flow.Timeline's VisualScriptTimeline) and the inspector's editor of .scurve assets (over a Curve).
// It holds the view transform (time and value ranges to a rectangle in pixels; zoom about a point, pan, frame), hit testing
// (keys, tangent handles, event lanes, curves, empty space, in that order) and the gestures turned into intents: documents to
// commit, made from the document at the start of the interaction and the total change so far. Pure (no ImGui): the curve
// editor widget (CurveEditor.h) feeds it the mouse each frame, draws from its queries and hands its intents to its host.
//
// A host's document type reaches the view through CurveDocumentTraits (below), which the host specializes with its own reads
// and edits, so the document keeps its own format and its own edit functions. The view's member definitions are in
// CurveViewImpl.h, which only the translation unit that instantiates the view for a document type includes (one explicit
// instantiation per type). Main thread only.
namespace Strada
{
	// A key of a curve document: (track index, key index).
	using CurveKeyRef = std::pair<uint32_t, uint32_t>;

	enum class CurveHitKind : uint8_t
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

	struct CurveHit
	{
		CurveHitKind Kind = CurveHitKind::None;
		uint32_t Track = 0;
		uint32_t Key = 0;
		uint32_t Component = 0;
		// The time and the value under the point.
		float Time = 0.0f;
		float Value = 0.0f;

		bool operator==(CurveHit const& other) const = default;
	};

	// What the view shows of a track: the eye, and per component (X Y Z W, R G B A) its toggle.
	struct CurveTrackView
	{
		bool Visible = true;
		std::array<bool, 4> Components{true, true, true, true};

		bool operator==(CurveTrackView const& other) const = default;
	};

	// The mouse over the view in a frame, in screen pixels.
	struct CurveViewInput
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

	enum class CurveIntentKind : uint8_t
	{
		// A frame of a drag: the document the drag has made so far (from its start); one undo step until EndEdit.
		Edit = 0,
		// The drag ended: its last value commits.
		EndEdit,
		// A discrete edit, one step of its own.
		Commit,
		// Open the menu of the selected keys (Interpolation, Tangents, Delete).
		OpenKeyMenu,
		// The drag was cancelled (Escape): what it committed goes back to the document at its start, in the drag's step, which
		// then nets to nothing.
		CancelEdit
	};

	// The gesture in progress.
	enum class CurveGesture : uint8_t
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

	// The input axis of a document: where its keys may lie.
	struct CurveDomain
	{
		// Keys stay within [Start, End] (finite).
		double Start = 0.0;
		double End = 1.0;
		// A bounded domain is the document's own extent (a timeline's [0, Length]): the view frames it without a selection, and
		// the editor shades what lies outside it and draws curves only within it. An unbounded one (a curve's input, any finite
		// number) is framed from its keys, and its curves are drawn across the whole view, extrapolations included.
		bool Bounded = true;
	};

	// How the view reads and edits a document type: each host specializes it for its own document (VisualScriptCurveView.h for
	// the timeline, CurveEditing.h for the scalar Curve). A specialization gives, with track and key indices the document has
	// (callers check them against GetTrackCount and GetKeyCount first):
	//   using Intent = ...;        an aggregate { CurveIntentKind Kind; TDocument <document>; std::string Action; }
	//   static TDocument& GetDocument(Intent& intent);
	//   static constexpr float TimeStep;            key times snap to it while dragged and are nudged by it; 0: a hundredth of
	//                                               the visible time span, rounded to 1, 2 or 5 times a power of ten
	//   static constexpr double MinTimeSpan, MaxTimeSpan, MaxTimeMagnitude;   the bounds of the visible time span and of its
	//                                               middle's distance from 0
	//   static constexpr char const* TimeFormat;    the printf format of the header's time field ("%.3f s")
	//   static CurveDomain GetDomain(TDocument const&);
	//   static uint32_t GetTrackCount(TDocument const&);
	//   static std::string_view GetTrackName(TDocument const&, uint32_t track);
	//   static uint32_t GetComponents(TDocument const&, uint32_t track);   0 for an event track (keys on a lane), else 1 to 4
	//   static uint32_t GetKeyCount(TDocument const&, uint32_t track);
	//   static float GetKeyTime(TDocument const&, uint32_t track, uint32_t key);
	//   static float GetKeyValue(TDocument const&, uint32_t track, uint32_t key, uint32_t component);
	//   static CurveInterpolation GetKeyInterpolation(TDocument const&, uint32_t track, uint32_t key);
	//   static std::optional<glm::vec4> GetKeyIn(TDocument const&, uint32_t track, uint32_t key);    the set slopes, per component
	//   static std::optional<glm::vec4> GetKeyOut(TDocument const&, uint32_t track, uint32_t key);   (nothing: automatic)
	//   static std::pair<glm::vec4, glm::vec4> GetKeySlopes(TDocument const&, uint32_t track, uint32_t key);   resolved (In, Out)
	//   static glm::vec4 Evaluate(TDocument const&, uint32_t track, double time);   the track's value, beyond its keys too
	// and the edits, each leaving the document unchanged when it fails and keeping keys sorted and within the domain:
	//   static Result<void> MoveKeys(TDocument&, std::span<CurveKeyRef const>, float timeOffset, glm::vec4 valueOffset);
	//   static Result<uint32_t> AddKey(TDocument&, uint32_t track, float time, glm::vec4 value);
	//   static Result<void> RemoveKeys(TDocument&, std::span<CurveKeyRef const>);
	//   static Result<void> SetKeyValue(TDocument&, CurveKeyRef, glm::vec4 value);
	//   static Result<void> SetKeyInterpolation(TDocument&, std::span<CurveKeyRef const>, CurveInterpolation);
	//   static Result<void> SetKeyTangents(TDocument&, CurveKeyRef, std::optional<glm::vec4> in, std::optional<glm::vec4> out);
	//   static Result<void> FlattenTangents(TDocument&, std::span<CurveKeyRef const>);
	//   static Result<void> ResetTangents(TDocument&, std::span<CurveKeyRef const>);
	// TDocument is a value type with operator==.
	template<typename TDocument>
	struct CurveDocumentTraits;

	template<typename TDocument>
	class CurveView
	{
	public:
		using Traits = CurveDocumentTraits<TDocument>;
		using Intent = typename Traits::Intent;

		// Key times snap to this step while dragged and are nudged by it (0: a step of the visible span, GetTimeStep).
		static constexpr float TimeStep = Traits::TimeStep;
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
		// The bounds of the visible spans, and how far from 0 their middles may go. Values are floats, so the value span also
		// stays at least RelativeMinSpan of its middle's magnitude (finer steps than a float has there show nothing), and its
		// bounds reach past the largest floats (about 3.4e38) so that every value can be framed. The time bounds are the
		// document's.
		static constexpr double MinTimeSpan = Traits::MinTimeSpan;
		static constexpr double MaxTimeSpan = Traits::MaxTimeSpan;
		static constexpr double MaxTimeMagnitude = Traits::MaxTimeMagnitude;
		static constexpr double MinValueSpan = 1e-4;
		static constexpr double MaxValueSpan = 1.0e39;
		static constexpr double MaxValueMagnitude = 1.0e39;
		static constexpr double RelativeMinSpan = 1e-6;

		// The view's rectangle in screen pixels and the font size its parts scale with.
		void SetRect(glm::vec2 min, glm::vec2 max, float fontSize);
		glm::vec2 GetMin() const { return m_Min; }
		glm::vec2 GetMax() const { return m_Max; }
		float GetFontSize() const { return m_FontSize; }

		// Takes the document shown this frame and frames everything the first time. The selection, the eye and the component
		// toggles follow the tracks when the track list changed since the previous Sync (an edit, an undo or an agent's):
		// tracks are followed by name, and a track whose name changed in its place by position (a rename, whose toggles are
		// renamed with it); the keys of a removed track and keys that no longer exist leave the selection.
		void Sync(TDocument const& document);
		// Where a track of the document the previous Sync took is in the one the last Sync took, followed as Sync follows it;
		// nothing for a track that was removed.
		std::optional<uint32_t> FollowTrack(uint32_t track) const;

		// The view transform. The ranges are kept in double, always finite and within the bounds above; screen positions
		// are clamped to a few screen sizes around the view, so values far outside it still draw and hit test as numbers.
		glm::dvec2 GetTimeRange() const { return {m_TimeStart, m_TimeEnd}; }
		glm::dvec2 GetValueRange() const { return {m_ValueStart, m_ValueEnd}; }
		void SetTimeRange(double start, double end);
		void SetValueRange(double start, double end);
		float ToScreenX(double time) const;
		float ToScreenY(double value) const;
		double ToTime(float x) const;
		double ToValue(float y) const;
		// The step key times snap to and are nudged by: TimeStep, or without one a hundredth of the visible time span rounded to
		// 1, 2 or 5 times a power of ten.
		double GetTimeStep() const;
		// The lanes of the visible event tracks under the ruler, then the curves' area.
		float GetLanesTop() const;
		float GetCurvesTop() const;
		// The lane of an event track (top and bottom); nothing for a hidden or curve track.
		std::optional<std::pair<float, float>> GetLane(uint32_t track) const;
		// The screen position of a key's diamond for a component (event keys: on their lane).
		std::optional<glm::vec2> GetKeyPosition(TDocument const& document, uint32_t track, uint32_t key, uint32_t component) const;
		// Where a selected cubic key's tangent handles are for a component; nothing for the side it has none of.
		std::optional<glm::vec2> GetTangentHandle(TDocument const& document, uint32_t track, uint32_t key, uint32_t component,
		                                          bool out) const;
		// Whether a key shows tangent handles: a selected curve key whose segment is cubic.
		bool HasTangentHandles(TDocument const& document, uint32_t track, uint32_t key) const;

		void ZoomTime(float factor, float aboutX);
		void ZoomValues(float factor, float aboutY);
		void Pan(glm::vec2 pixels);
		// Frames the selected keys (those the document has), or without a selection every visible key and curve: over a
		// bounded domain the whole domain, over an unbounded one the keys' span.
		void Frame(TDocument const& document);

		CurveHit HitTest(TDocument const& document, glm::vec2 point) const;

		// The eye and component toggles: hidden tracks and components are neither hit nor framed nor selected by Ctrl+A.
		CurveTrackView GetTrackView(std::string_view track) const;
		void SetTrackVisible(std::string_view track, bool visible);
		void SetComponentVisible(std::string_view track, uint32_t component, bool visible);
		bool IsVisible(TDocument const& document, uint32_t track, uint32_t component) const;

		std::span<CurveKeyRef const> GetSelection() const { return m_Selection; }
		bool IsSelected(uint32_t track, uint32_t key) const;
		void Select(std::vector<CurveKeyRef> keys);
		void ClearSelection() { m_Selection.clear(); }

		CurveGesture GetGesture() const { return m_Gesture; }
		// The marquee's rectangle while one is drawn.
		std::optional<std::pair<glm::vec2, glm::vec2>> GetMarquee() const;

		// A frame of the mouse over the view, with the document shown and its title (which descriptions end with).
		std::vector<Intent> Update(CurveViewInput const& input, TDocument const& document, std::string_view title);

		// Ctrl+A selects every key of the visible tracks.
		void SelectAll(TDocument const& document);
		// Delete: removes the selected keys.
		std::optional<Intent> DeleteSelection(TDocument const& document, std::string_view title);
		// The arrows: moves the selected keys by steps of GetTimeStep (Shift: 10 steps) as a block.
		std::optional<Intent> Nudge(TDocument const& document, int steps, std::string_view title);
		// The key menu's items and the header row's buttons, on the selected keys.
		std::optional<Intent> SetInterpolation(TDocument const& document, CurveInterpolation interpolation, std::string_view title);
		std::optional<Intent> FlattenTangents(TDocument const& document, std::string_view title);
		std::optional<Intent> AutoTangents(TDocument const& document, std::string_view title);
		// Adds a key to a track at a time (snapped unless free, within the domain) with the curve's value there, and selects it.
		std::optional<Intent> AddKey(TDocument const& document, uint32_t track, double time, bool free, std::string_view title);
		// Cancels the gesture in progress (Escape), as the canvas does: a drag of keys or of a tangent goes back to where it
		// started (CancelEdit), and a marquee gives back the selection it started from.
		std::vector<Intent> CancelGesture();

		// "Move Key of Lift on Timeline", "Move 3 Keys on Timeline": what an edit of the selected keys is called.
		static std::string DescribeKeys(TDocument const& document, std::span<CurveKeyRef const> keys, std::string_view verb,
		                                std::string_view title, std::string_view suffix = {});

	private:
		struct NamedTrackView
		{
			std::string Name;
			CurveTrackView View;
		};

		void BeginPress(CurveViewInput const& input, TDocument const& document, std::string_view title, std::vector<Intent>& intents);
		void UpdateDrag(CurveViewInput const& input, std::string_view title, std::vector<Intent>& intents);
		void UpdateMarquee(TDocument const& document);
		std::vector<CurveKeyRef> FindKeysIn(TDocument const& document, glm::vec2 min, glm::vec2 max) const;
		// The selected keys of curve tracks (those interpolation and tangents apply to).
		std::vector<CurveKeyRef> GetSelectedCurveKeys(TDocument const& document) const;
		bool HasKey(TDocument const& document, CurveKeyRef key) const;
		double GetPixelsPerSecond() const;
		double GetPixelsPerValue() const;
		double Snap(double time) const;

		glm::vec2 m_Min{0.0f};
		glm::vec2 m_Max{1.0f};
		float m_FontSize = 13.0f;
		double m_TimeStart = 0.0;
		double m_TimeEnd = 1.0;
		double m_ValueStart = -1.0;
		double m_ValueEnd = 1.0;
		bool m_Framed = false;
		// The track names the last Sync took, and where each track of the Sync before it went.
		bool m_Synced = false;
		std::vector<std::string> m_TrackNames;
		std::vector<std::optional<uint32_t>> m_TrackMap;
		// The visible event tracks, top to bottom (their lanes).
		std::vector<uint32_t> m_Lanes;
		std::vector<NamedTrackView> m_TrackViews;
		std::vector<CurveKeyRef> m_Selection;

		CurveGesture m_Gesture = CurveGesture::Idle;
		int m_Button = 0;
		glm::vec2 m_PressMouse{0.0f};
		glm::vec2 m_LastMouse{0.0f};
		CurveHit m_PressHit;
		bool m_PressShift = false;
		bool m_PressCtrl = false;
		// The interaction's start: the document, the selection, and what the drag was last at.
		TDocument m_Start{};
		std::vector<CurveKeyRef> m_StartSelection;
		std::optional<TDocument> m_LastEdit;
	};

	// A step of 1, 2 or 5 times a power of ten that is at least minimum (1 for a minimum that is not a positive number).
	[[nodiscard]] double GetNiceCurveStep(double minimum);
}

#pragma once

#include "Editor/UI/CurveView.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ImDrawList;

// The curve editor widget (Docs/Architecture.md §6.5), shared by its hosts: the Timeline drawer of the visual script editor
// (VisualScriptCurveEditor) and the inspector's editor of .scurve assets (CurveAssetEditor). It draws a document through the
// shared curve view (CurveView.h): the header row (the selected key's time and value, interpolation, tangents, Frame), the
// ruler, the value lines, the event lanes, the curves with their keys and the tangent handles of the selected cubic keys, the
// marquee and a playhead, and the menu of the selected keys. It feeds the view the mouse and turns what the user did into
// changes (CurveEditorChange) that the host commits to its document as undo steps; it never edits a document itself. What a
// host has of its own (a track list, colours, the value field of its value types, how it commits) it gives through
// CurveEditorFrame. The member definitions are in CurveEditorImpl.h, which only the translation unit that instantiates the
// widget for a document type includes. Main thread only, inside an ImGui frame (but the getters).
namespace Strada
{
	// What the widget drew in its last frame, for tests and automation.
	struct CurveEditorDrawRecord
	{
		// Curves (one per visible component of a curve track) and diamonds (keys of curves, per visible component, and of events).
		uint32_t Curves = 0;
		uint32_t CurveKeys = 0;
		uint32_t EventKeys = 0;
		uint32_t TangentHandles = 0;
		bool Marquee = false;
		// The host showed its empty state instead of curves (CurveEditor::SetEmpty).
		bool Empty = false;
		// The playhead was drawn, at this screen x.
		bool Playhead = false;
		float PlayheadX = 0.0f;
	};

	// The keys a host routes to the widget while it has the keyboard, done at the next ApplyActions.
	enum class CurveEditorAction : uint8_t
	{
		// Delete: removes the selected keys.
		Delete = 0,
		// Ctrl+A: selects the keys of the visible tracks.
		SelectAll,
		// The arrows: nudge the selected keys by the view's time step, Shift by 10 steps.
		NudgeLeft,
		NudgeRight,
		NudgeLeftCoarse,
		NudgeRightCoarse,
		// F: frames the selection, or everything.
		Frame
	};

	enum class CurveEditorChangeKind : uint8_t
	{
		// A frame of a drag in the curve view: the document the drag has made from its start. Frames of one drag are one undo
		// step until EndDrag.
		Drag = 0,
		// The drag ended.
		EndDrag,
		// The drag was cancelled (Escape): Document is the document at its start, committed in the drag's step, which then nets
		// to nothing.
		CancelDrag,
		// A header field changed the document (Active: the field is still in use): one undo step while the field stays active.
		Change,
		// The header field was deactivated: its change is complete.
		End,
		// A discrete edit (a key added or removed, interpolation, tangents, a nudge), a step of its own, or merged with the
		// steps of the same non-zero MergeKey (a run of arrow-key nudges).
		Commit
	};

	template<typename TDocument>
	struct CurveEditorChange
	{
		CurveEditorChangeKind Kind = CurveEditorChangeKind::Commit;
		// Drag, CancelDrag, Change and Commit: the whole document to set.
		TDocument Document{};
		// The step's description, without the document's title ("Move Key of Lift on Timeline").
		std::string Action;
		bool Active = false;
		uint64_t MergeKey = 0;
	};

	// What one frame of the widget shows and how the host takes part in drawing it.
	template<typename TDocument>
	struct CurveEditorFrame
	{
		TDocument const& Document;
		// What descriptions end with ("Timeline", an asset's name).
		std::string_view Title;
		// Shown, never edited: every change is dropped.
		bool ReadOnly = false;
		// The clock that times runs of arrow-key nudges, in seconds.
		double Time = 0.0;
		// The colour (IM_COL32 layout) of a track's curve or diamonds for a component.
		std::function<uint32_t(uint32_t track, uint32_t component)> Color;
		// Draws the value field of the first selected key, a key of a curve track, inside a group the widget measures (its
		// rectangle is "Value"; it is active while an item of it is), and returns the new value when the user changed it.
		std::function<std::optional<glm::vec4>(uint32_t track, uint32_t key)> ValueField;
		// A track drawn with a thicker line (the host's highlighted row).
		std::optional<uint32_t> Emphasized;
		// The playhead's colour.
		uint32_t PlayheadColor = 0xFFFFFFFFu;
	};

	template<typename TDocument>
	class CurveEditor
	{
	public:
		using Change = CurveEditorChange<TDocument>;
		using View = CurveView<TDocument>;
		using Frame = CurveEditorFrame<TDocument>;

		// The curves are drawn with a point every this many pixels.
		static constexpr float CurveStep = 2.0f;
		// Arrow-key nudges less than this many seconds apart are one step (as the visual script canvas's nudges).
		static constexpr double NudgeRunSeconds = 0.5;

		// nudgeMergeKey is the base of the merge keys of nudge runs: each run's key is the base plus the run's counter, so give
		// each host's widgets a base of their own.
		explicit CurveEditor(uint64_t nudgeMergeKey);

		// Starts a frame: clears the rectangles and the record of the last one.
		void BeginFrame();
		// The header row: the first selected key's time (within the domain) and value (the host's field), the interpolation
		// buttons (Cubic, Linear, Constant), the tangent buttons (Auto, Flat) and Frame. It syncs the view with the document
		// first (CurveView::Sync), so a selection of keys the document no longer has is never read.
		void DrawHeader(Frame const& frame, std::vector<Change>& changes);
		// The curve view in the rest of the content region: an item over all of it takes every mouse button and the wheel,
		// the view is updated with the mouse, and the view is drawn.
		void DrawCurves(Frame const& frame, std::vector<Change>& changes);
		// The menu of the selected keys (Interpolation, Tangents, Delete), when the view opened it.
		void DrawKeyMenu(Frame const& frame, std::vector<Change>& changes);
		// Does the actions queued since the last call.
		void ApplyActions(Frame const& frame, std::vector<Change>& changes);

		void QueueAction(CurveEditorAction action) { m_Actions.push_back(action); }
		// Cancels the gesture in progress (Escape) at the next DrawCurves: a drag's keys or tangent go back where they were
		// (CancelDrag); a marquee gives back the selection it started from.
		void CancelGesture() { m_CancelGesture = true; }
		// Ends the interaction of the header field in progress, for a host whose document changed elsewhere meanwhile (an
		// undo, an agent's edit): the field lets go of the mouse at once and changes nothing more until it is pressed again,
		// so the other change stays. Nothing while no header field is active.
		void EndHeaderInteraction();
		// Forgets the view (ranges, selection, toggles, gesture), the queued actions and the header's interaction: for a host
		// that starts showing another document.
		void Reset();
		// The host drew its empty state instead of DrawCurves this frame.
		void SetEmpty() { m_Record.Empty = true; }

		// A time on the document drawn as a vertical line in the frame's playhead colour (a timeline's playing time); none draws
		// none. Kept until set again.
		void SetPlayhead(std::optional<double> time) { m_Playhead = time; }
		std::optional<double> GetPlayhead() const { return m_Playhead; }
		// Whether a gesture of the curve view is in progress: a press, a drag of keys or of a tangent, a marquee or a pan.
		bool IsGestureActive() const { return m_View.GetGesture() != CurveGesture::Idle; }
		// Whether the user is in the middle of an interaction this frame: a header field active, or a key or tangent drag.
		bool IsInteractionActive() const { return m_InteractionActive; }
		// Whether a header field (Time, the host's Value) was active in the last DrawHeader.
		bool IsHeaderActive() const { return m_HeaderField != 0; }

		View& GetView() { return m_View; }
		View const& GetView() const { return m_View; }
		CurveEditorDrawRecord const& GetDrawRecord() const { return m_Record; }
		// The screen rectangle (min, max) of a part in the last frame: "Curves" (the curve view), "Time", "Value", "Cubic",
		// "Linear", "Constant", "Auto", "Flat", "Frame".
		std::optional<std::pair<glm::vec2, glm::vec2>> FindRect(std::string_view part) const;

	private:
		void Add(Frame const& frame, std::vector<Change>& changes, typename View::Intent intent);
		// The merge key of an arrow-key nudge at a time.
		uint64_t GetNudgeKey(double time);
		void AddRect(std::string label);

		View m_View;
		uint64_t m_NudgeBase = 0;
		bool m_CancelGesture = false;
		bool m_OpenKeyMenu = false;
		bool m_InteractionActive = false;
		// The header row's Time drag: the document at the interaction's start.
		std::optional<TDocument> m_HeaderStart;
		// The ImGui item ID of the header field active in the last DrawHeader (the Value group's: its active item's); 0 for none.
		uint32_t m_HeaderField = 0;
		// The run of arrow-key nudges: its counter, its merge key and when its last press came.
		uint64_t m_NudgeRuns = 0;
		uint64_t m_NudgeKey = 0;
		double m_LastNudge = -1.0e30;
		std::vector<CurveEditorAction> m_Actions;
		CurveEditorDrawRecord m_Record;
		std::optional<double> m_Playhead;
		std::vector<std::pair<std::string, std::pair<glm::vec2, glm::vec2>>> m_Rects;
	};

	// A key's diamond, as the curve editor draws keys (shared with previews of curves drawn elsewhere).
	void DrawCurveKeyDiamond(ImDrawList* drawList, glm::vec2 center, float radius, uint32_t fill, uint32_t outline);
	// The lines of a ruler, the multiples of step within [low, high]: the index of the first (its multiple of step) and how
	// many, at most limit of them; none for a range or a step that is not finite.
	[[nodiscard]] std::pair<double, int> GetCurveRulerLines(double low, double high, double step, int limit);
}

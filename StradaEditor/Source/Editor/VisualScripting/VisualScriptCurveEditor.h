#pragma once

#include "Editor/VisualScripting/VisualScriptCurveView.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"
#include "Editor/VisualScripting/VisualScriptLanguageEditing.h"
#include "Editor/VisualScripting/VisualScriptLiteralEditor.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptTimeline.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Timeline drawer (spec-slice3 §9.5): a Flow.Timeline's tracks on the left (name, type colour, the eye and component
// toggles, and the track operations) and its curves on the right (VisualScriptCurveView's model: a ruler in seconds, event
// lanes above the curves, a header row with the selected key's time and value, interpolation and tangent buttons and Frame),
// every edit SetTimeline in one step (drags at the widget cadence), described as §9.6 says. The track edits are shared with
// Details' Timeline section. Main thread only.
namespace Strada
{
	// The merge key of the drawer's arrow-key nudges, "VSTN" plus the run's counter: a run of presses less than
	// VisualScriptNudgeRunSeconds apart is one undo step, as the canvas's nudges are (VisualScriptNudgeMergeKey).
	inline constexpr uint64_t VisualScriptTimelineNudgeMergeKey = 0x5653544E00000000ull;

	// A timeline edit: the document with the node's timeline set, and the step's description (without the script).
	struct VisualScriptTimelineEdit
	{
		VisualScriptDocument Document;
		std::string Action;
	};

	// Sets a node's timeline in a copy of the document (VisualScriptEditing::SetTimeline): action, with " (removes 1 link)"
	// when links went. Fails as SetTimeline fails.
	[[nodiscard]] Result<VisualScriptTimelineEdit>
	MakeVisualScriptTimelineEdit(VisualScriptDocument const& document, uint32_t node, VisualScriptTimeline const& timeline,
	                             std::string action, std::span<VisualScriptEditing::VisualScriptTrackRename const> renames = {});
	// The track operations of Details and the drawer, on the timeline shown (title: the node's title, "Timeline"):
	// "Add Track Curve to Timeline", "Rename Track Lift to Height on Timeline (and 2 links)", "Remove Track Lift from Timeline
	// (removes 1 link)", "Change Lift to Vector3 on Timeline (removes 1 link)", "Set Length on Timeline (moves 2 keys)" (from the
	// timeline at the interaction's start), "Loop Timeline" and "Stop Looping Timeline". A new track is named after its kind
	// ("Curve" for curves, "Event" for events; MakeVisualScriptTimelineTrackName).
	[[nodiscard]] Result<VisualScriptTimelineEdit> AddVisualScriptTimelineTrackEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                                VisualScriptTimeline const& timeline,
	                                                                                std::string_view type, std::string_view title);
	[[nodiscard]] Result<VisualScriptTimelineEdit> RenameVisualScriptTimelineTrackEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                                   VisualScriptTimeline const& timeline, uint32_t track,
	                                                                                   std::string const& name, std::string_view title);
	[[nodiscard]] Result<VisualScriptTimelineEdit> RemoveVisualScriptTimelineTrackEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                                   VisualScriptTimeline const& timeline, uint32_t track,
	                                                                                   std::string_view title);
	[[nodiscard]] Result<VisualScriptTimelineEdit>
	ChangeVisualScriptTimelineTrackTypeEdit(VisualScriptDocument const& document, uint32_t node, VisualScriptTimeline const& timeline,
	                                        uint32_t track, std::string_view type, std::string_view title);
	[[nodiscard]] Result<VisualScriptTimelineEdit> SetVisualScriptTimelineLengthEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                                 VisualScriptTimeline const& start, float length,
	                                                                                 std::string_view title);
	[[nodiscard]] Result<VisualScriptTimelineEdit> SetVisualScriptTimelineLoopEdit(VisualScriptDocument const& document, uint32_t node,
	                                                                               VisualScriptTimeline const& timeline, bool loop,
	                                                                               std::string_view title);
	// How many links of the document's graph start at a track's pin of the node (its split fields' included).
	[[nodiscard]] uint32_t CountVisualScriptTrackLinks(VisualScriptDocument const& document, uint32_t node, std::string_view track);
	// The colour a track's curve or diamonds are drawn in for a component (IM_COL32 layout): a float track's and an event
	// track's type colour, a vector's or a colour's per component (X Y Z W, R G B A).
	[[nodiscard]] uint32_t GetVisualScriptTrackColor(VisualScriptTimelineTrack const& track, uint32_t component,
	                                                 VisualScriptLibrary const* library);
	// The letter of a component of a track's type: X Y Z W, or R G B A for colours.
	[[nodiscard]] char GetVisualScriptTrackComponentLetter(VisualScriptTimelineTrack const& track, uint32_t component);

	// Draws a timeline track's curve (each component) or its event diamonds into a rectangle, over the timeline's length:
	// Details' preview of a track. Main thread only, inside an ImGui frame.
	void DrawVisualScriptTrackPreview(VisualScriptTimeline const& timeline, uint32_t track, VisualScriptLibrary const* library,
	                                  glm::vec2 min, glm::vec2 max);

	// What the curve editor drew in its last Draw, for tests and automation.
	struct VisualScriptCurveDrawRecord
	{
		// Curves (one per visible component of a curve track) and diamonds (keys of curves, per visible component, and of events).
		uint32_t Curves = 0;
		uint32_t CurveKeys = 0;
		uint32_t EventKeys = 0;
		uint32_t TangentHandles = 0;
		bool Marquee = false;
		// "No tracks yet" was shown instead of curves.
		bool Empty = false;
		// The playhead was drawn, at this screen x (spec-slice4 §15.7).
		bool Playhead = false;
		float PlayheadX = 0.0f;
	};

	class VisualScriptCurveEditor
	{
	public:
		// The track list's width, in font sizes.
		static constexpr float TrackListWidth = 15.0f;
		// The curves are drawn with a point every this many pixels.
		static constexpr float CurveStep = 2.0f;

		// Draws the timeline of a node of the shown sub-graph in the content region, committing through commits.
		void Draw(VisualScriptPaneContext& context, VisualScriptWidgetCommits& commits, VisualScriptPaneResult& result, uint32_t node);
		// A key the window routed to the drawer while it had the keyboard (Delete, Ctrl+A, the arrows, F): done at the next Draw.
		void QueueAction(VisualScriptEditorAction action) { m_Actions.push_back(action); }
		// Opens on a node, and on one of its tracks: its row is highlighted and the view frames its keys at the next Draw.
		void Open(uint32_t node, std::optional<uint32_t> track);
		// Cancels the gesture in progress (Escape) at the next Draw: a drag's keys or tangent go back where they were, in the
		// drag's step, which then nets to nothing; a marquee gives back the selection it started from.
		void CancelGesture();
		// While playing, the context instance's time on the timeline shown (spec-slice4 §15.7): the curves draw a vertical line
		// in the play colour there; none draws none. Kept until set again.
		void SetPlayhead(std::optional<double> time) { m_Playhead = time; }
		std::optional<double> GetPlayhead() const { return m_Playhead; }
		// Whether a gesture of the curve view is in progress.
		bool IsGestureActive() const { return m_View.GetGesture() != VisualScriptCurveGesture::Idle; }

		uint32_t GetNode() const { return m_Node; }
		VisualScriptCurveView& GetView() { return m_View; }
		VisualScriptCurveView const& GetView() const { return m_View; }
		VisualScriptCurveDrawRecord const& GetDrawRecord() const { return m_Record; }
		// The screen rectangle (min, max) of a part in the last Draw: "Curves" (the curve view), "Track <i>" (its name),
		// "Track <i> Eye", "Track <i> Component <c>", "Track <i> Name" (the rename field), "Add Track", "Add <Kind>" (the empty
		// view's buttons), "Time", "Value", "Cubic", "Linear", "Constant", "Auto", "Flat", "Frame".
		std::optional<std::pair<glm::vec2, glm::vec2>> FindRect(std::string_view part) const;
		// The track whose row is highlighted.
		std::optional<uint32_t> GetFocusedTrack() const { return m_FocusedTrack; }

	private:
		struct Frame;

		void DrawTracks(Frame& frame);
		void DrawTrackMenu(Frame& frame, uint32_t track);
		void DrawHeader(Frame& frame);
		void DrawCurves(Frame& frame);
		void DrawEmpty(Frame& frame);
		void DrawKeyMenu(Frame& frame);
		void ApplyActions(Frame& frame);
		void ApplyIntent(Frame& frame, VisualScriptCurveIntent intent);
		void Commit(Frame& frame, Result<VisualScriptTimelineEdit> edit, uint64_t mergeKey = 0);
		// The merge key of an arrow-key nudge at a time (VisualScriptPaneContext::Time).
		uint64_t GetNudgeKey(double time);
		void AddRect(std::string label);

		VisualScriptCurveView m_View;
		VisualScriptLiteralEditor m_Literals;
		VisualScriptNameField m_Names;
		uint32_t m_Node = 0;
		std::optional<uint32_t> m_FocusedTrack;
		bool m_FrameTrack = false;
		bool m_CancelGesture = false;
		// The track whose name is being typed, and whether its field takes the keyboard at the next Draw.
		std::optional<uint32_t> m_Renaming;
		bool m_RenameFocus = false;
		// The header row's Time drag: the timeline and the selected key at the interaction's start.
		std::optional<VisualScriptTimeline> m_HeaderStart;
		// The curve view's drag in progress: the node's Timeline setting before its first edit (none when the node had none)
		// and the step's description, which Escape commits back.
		struct DragStart
		{
			std::optional<Json> Setting;
			std::string Action;
		};
		std::optional<DragStart> m_DragStart;
		// The run of arrow-key nudges: its counter, its merge key and when its last press came.
		uint64_t m_NudgeRuns = 0;
		uint64_t m_NudgeKey = 0;
		double m_LastNudge = -1.0e30;
		bool m_OpenKeyMenu = false;
		std::vector<VisualScriptEditorAction> m_Actions;
		VisualScriptCurveDrawRecord m_Record;
		std::optional<double> m_Playhead;
		std::vector<std::pair<std::string, std::pair<glm::vec2, glm::vec2>>> m_Rects;
	};
}

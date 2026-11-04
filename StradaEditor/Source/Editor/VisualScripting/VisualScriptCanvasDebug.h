#pragma once

#include "Editor/VisualScripting/VisualScriptBreakpoints.h"
#include "Editor/VisualScripting/VisualScriptFlowTake.h"

#include "Strada/Script/VisualScriptDebugger.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What a graph window's canvas draws of debugging: one view per window and frame, which the window manager builds from the debug
// session (breakpoints, the selected stop and frame, the flow, the values) and the window's flow model, and which the canvas
// renderer only draws (VisualScriptCanvasDrawInput::Debug). Pure data, no ImGui; main thread only.
namespace Strada
{
	// How a breakpoint marker is drawn: a filled disc (Enabled), a ring (Disabled), a disc with a white inner dot (Conditional:
	// a condition or a hit count) or an amber disc with "!" (Invalid: its condition does not compile).
	enum class VisualScriptBreakpointMarkState : uint8_t
	{
		Enabled = 0,
		Disabled,
		Conditional,
		Invalid
	};

	struct VisualScriptBreakpointMark
	{
		uint32_t Node = 0;
		VisualScriptBreakpointMarkState State = VisualScriptBreakpointMarkState::Enabled;
		// The breakpoint stopped the selected stop: a yellow arrow over the marker.
		bool CausedStop = false;
		// The breakpoint does not stop now (spec-slice4 §2.6): drawn at half alpha.
		bool Inactive = false;

		bool operator==(VisualScriptBreakpointMark const& other) const = default;
	};

	// The selected stop frame's position, when it is in the shown sub-graph.
	struct VisualScriptInstructionMark
	{
		uint32_t Node = 0;
		VisualScriptStopPlace Place = VisualScriptStopPlace::Enter;
		// The entered exec input (Enter; empty for an event, which is entered at its header), the exit about to fire (Continue).
		std::string Pin;
		// "next: Then1", "next iteration: Index 3": the pill beside the exit at Continue.
		std::string Note;
		// A passed or an error stop: nothing resumes it, so its triangle is hollow.
		bool ReadOnly = false;

		bool operator==(VisualScriptInstructionMark const& other) const = default;
	};

	// A fired exec wire. Link: its index in the visual model. Intensity: from 0.05 to 1, on the flow clock (VisualScriptFlowModel).
	// Phase: seconds of editor time the wire's dots have travelled since they started; negative: they have not started (the
	// stagger of a chain) or are not drawn.
	struct VisualScriptFlowLinkMark
	{
		uint32_t Link = 0;
		float Intensity = 0.0f;
		float Phase = 0.0f;

		bool operator==(VisualScriptFlowLinkMark const& other) const = default;
	};

	// A node that ran: its glow's intensity (0 when it faded; then only its count is drawn). Count: the runs the window counted
	// since play started or its last Reset Run Counts (spec-slice4 §13.3).
	struct VisualScriptFlowNodeMark
	{
		uint32_t Node = 0;
		float Intensity = 0.0f;
		uint32_t Count = 0;

		bool operator==(VisualScriptFlowNodeMark const& other) const = default;
	};

	// A comment box glowing at Far and Overview with the highest intensity among its nodes (spec-slice4 §13.2).
	struct VisualScriptFlowBoxMark
	{
		uint32_t Comment = 0;
		float Intensity = 0.0f;

		bool operator==(VisualScriptFlowBoxMark const& other) const = default;
	};

	// A value a data wire carries: drawn as a pill at the wire's midpoint (VisualScriptDebugValues formats Text and Swatch).
	// Link: its index in the visual model. Earlier and Unknown values are drawn at half alpha; NotComputed ones not at all.
	struct VisualScriptValueMark
	{
		uint32_t Link = 0;
		std::string Text;
		// A colour value: its swatch, sRGB-encoded (IM_COL32 layout).
		std::optional<uint32_t> Swatch;
		VisualScriptValueCurrency Currency = VisualScriptValueCurrency::Current;

		bool operator==(VisualScriptValueMark const& other) const = default;
	};

	// U2 adds VisualScriptLiveMark (spec-slice4 §15.3).
	struct VisualScriptCanvasDebugView
	{
		// The shown sub-graph's breakpoints, by node ID ascending.
		std::span<VisualScriptBreakpointMark const> Breakpoints;
		// The selected stop frame, when it is in the shown sub-graph.
		std::optional<VisualScriptInstructionMark> Instruction;
		// The call or Send nodes of the stop's other frames in the shown sub-graph.
		std::span<uint32_t const> Callers;
		// The nodes the selected stop frame's execution computed (stamped current, spec-slice4 §5.2): the path that led to it.
		std::span<uint32_t const> Computed;
		// The flow of the shown sub-graph, most intense first (VisualScriptFlowModel::Build).
		std::span<VisualScriptFlowLinkMark const> FlowLinks;
		std::span<VisualScriptFlowNodeMark const> FlowNodes;
		std::span<VisualScriptFlowBoxMark const> FlowBoxes;
		// The flow clock holds (paused, stopped, between commands): no dots, intensities steady (spec-slice4 §13.3).
		bool FlowHeld = false;
		std::span<VisualScriptValueMark const> Values;
		// The key that toggles breakpoints, as FormatVisualScriptChord names it ("F9"), for the ghost marker's tooltip; empty
		// leaves the key out.
		std::string_view BreakpointChord;
	};

	// What a graph window shows of the debug session (graph.editors' debug.state): its graph holds the selected stop's frame
	// (Stopped; a passed breakpoint's Passed, an error's Error), a member of its chain that waits inside its Send or Broadcast
	// (Waiting), the wait of a step (Stepping), or the execution a step ended (StepEnded).
	enum class VisualScriptWindowDebugState : uint8_t
	{
		None = 0,
		Stopped,
		Waiting,
		Passed,
		Error,
		Stepping,
		StepEnded
	};

	// The buttons of a graph window's debug strip (spec-slice4 §13.6).
	enum class VisualScriptDebugStripAction : uint8_t
	{
		Continue = 0,
		StepOver,
		StepInto,
		StepOut,
		// Makes the stop's entity the graph's target and continues the graph's other stops (the flood remedy).
		OnlyInstance,
		// Opens the problems drawer on the first error (the last edit has errors, the stopped execution runs the last version).
		ShowFirstError,
		// Brings the frame the strip names into view (its window: a waiting member's receiver).
		Show,
		// Cancels a step waiting in a latent node.
		Break
	};

	struct VisualScriptDebugStripButton
	{
		VisualScriptDebugStripAction Action = VisualScriptDebugStripAction::Continue;
		std::string Label;
		// The key that does the same, as FormatVisualScriptChord names it ("F5"); empty for none.
		std::string Chord;
		// Show: the frame of the stop it brings into view (a waiting member's receiver).
		std::optional<uint32_t> Frame;
		// A second key that does the same, named in the tooltip (Step Into's Ctrl+F11, Cmd+F11 on macOS); empty for none.
		std::string SecondChord;

		bool operator==(VisualScriptDebugStripButton const& other) const = default;
	};

	// One line of the strip and its buttons, right of its text.
	struct VisualScriptDebugStripRow
	{
		std::string Text;
		std::vector<VisualScriptDebugStripButton> Buttons;

		bool operator==(VisualScriptDebugStripRow const& other) const = default;
	};

	// A value a pin of the shown sub-graph holds: the data wires that carry it show it (an output's wires, or the wire into an
	// input). Formatted (VisualScriptDebugValues): at most VisualScriptValuePillCodePoints code points, a colour as its swatch.
	struct VisualScriptDebugPinValue
	{
		uint32_t Node = 0;
		std::string Pin;
		bool Output = true;
		std::string Text;
		std::optional<uint32_t> Swatch;
		VisualScriptValueCurrency Currency = VisualScriptValueCurrency::Current;
		// The whole value, for the wire's hover pill.
		std::string Tooltip;

		bool operator==(VisualScriptDebugPinValue const& other) const = default;
	};

	// A value pill shows at most this many code points (spec-slice4 §13.4).
	inline constexpr size_t VisualScriptValuePillCodePoints = 24;
	// At most this many value pills are drawn in a frame (VisualScriptCanvasRenderer's cap, spec-slice4 §13.2).
	inline constexpr size_t VisualScriptValuePillLimit = 200;

	// A breakpoint of the window's document, with what the runtime reports of it (the breakpoint popup and the node menu).
	struct VisualScriptWindowBreakpoint
	{
		VisualScriptBreakpoint Breakpoint;
		VisualScriptBreakpointMarkState State = VisualScriptBreakpointMarkState::Enabled;
		// Why it does not stop now (spec-slice4 §2.6); empty while it is active or while the editor does not play.
		std::string InactiveReason;
		// The condition's error; empty when it has none.
		std::string Error;
		uint32_t Reached = 0;
		uint32_t Hits = 0;
		// It stopped the selected stop.
		bool CausedStop = false;

		bool operator==(VisualScriptWindowBreakpoint const& other) const = default;
	};

	// What the window manager tells a graph window of debugging every frame, read from the debug session; the window maps it
	// onto its canvas after its sync (VisualScriptCanvasDebugView). Pure data.
	struct VisualScriptWindowDebugInput
	{
		// Play runs with the debugger attached (never while simulating or in a test run).
		bool Attached = false;
		// Changes with every attachment: the window's flow and run counts start again.
		uint64_t Attachment = 0;
		VisualScriptWindowDebugState State = VisualScriptWindowDebugState::None;
		// The selected stop and its selected frame, when that frame is in the window's document; its sub-graph and position.
		std::optional<uint64_t> Stop;
		uint32_t Frame = 0;
		uint32_t Function = 0;
		std::optional<VisualScriptInstructionMark> Instruction;
		// The call or Send nodes of the stop's other frames in the window's document, by sub-graph: (function, node).
		std::vector<std::pair<uint32_t, uint32_t>> Callers;
		// The nodes the selected frame's execution computed (in Function's sub-graph).
		std::vector<uint32_t> Computed;
		// The values the window's wires show (in ValuesFunction's sub-graph): the selected frame's, or the target's live ones.
		std::vector<VisualScriptDebugPinValue> Values;
		uint32_t ValuesFunction = 0;
		// The document's breakpoints, by node ID.
		std::vector<VisualScriptWindowBreakpoint> Breakpoints;
		// The flow the session counted for the sub-graph the window showed when the manager took it, and the flow clock.
		VisualScriptFlowTake Flow;
		uint32_t FlowFunction = 0;
		double FlowTime = 0.0;
		// Flow is drawn: playing with Show Flow While Playing on.
		bool ShowFlow = false;
		// The debug strip under the toolbar (spec-slice4 §13.6), top to bottom; empty draws none.
		std::vector<VisualScriptDebugStripRow> Strip;
		// The STOPPED segment of the live pill: the window's graph holds the selected stop's frame (a waiting member included).
		bool StoppedHere = false;
		std::string StoppedTooltip;
		// The key that toggles breakpoints (F9), for the ghost marker's tooltip and the node menu.
		std::string BreakpointChord;
		// Wire values are drawn, and live ones while playing (Always).
		bool WireValues = true;
		bool LiveWireValues = false;
	};

	// The node menu's debugging items (spec-slice4 §13.5), listed in All for the glyph test.
	namespace VisualScriptDebugMenuTexts
	{
		inline constexpr std::string_view AddBreakpoint = "Add Breakpoint";
		inline constexpr std::string_view RemoveBreakpoint = "Remove Breakpoint";
		// Appended for several nodes ("Add Breakpoints").
		inline constexpr std::string_view Plural = "s";
		inline constexpr std::string_view EnableBreakpoint = "Enable Breakpoint";
		inline constexpr std::string_view DisableBreakpoint = "Disable Breakpoint";
		inline constexpr std::string_view NoBreakpoint = "The node has no breakpoint";
		inline constexpr std::string_view BreakpointSettings = "Breakpoint Settings...";
		inline constexpr std::string_view RunToHere = "Run to Here";
		inline constexpr std::string_view RunToHereNotPlaying = "Runs the game to the node while it plays";

		inline constexpr std::array<std::string_view, 9> All = {AddBreakpoint,      RemoveBreakpoint,  Plural,
		                                                        EnableBreakpoint,   DisableBreakpoint, NoBreakpoint,
		                                                        BreakpointSettings, RunToHere,         RunToHereNotPlaying};
	}

	// What a graph window asks of the debug session; the window manager hands each to it.
	struct VisualScriptDebugRequest
	{
		enum class Kind : uint8_t
		{
			// Nodes: F9 on them, a marker's click.
			ToggleBreakpoints = 0,
			// Nodes, Enabled: Ctrl+F9, the node menu, the popup's box.
			SetBreakpointsEnabled,
			// Breakpoint: the popup's condition and hit count.
			SetBreakpoint,
			// Nodes: the popup's Remove.
			RemoveBreakpoints,
			// Nodes (one): Run to Here.
			RunTo,
			// Action, Stop (none: every stop for Continue, the current one for steps): the strip's buttons.
			Resume,
			// Stop: the strip's [Only <entity>].
			OnlyInstance,
			// The strip's [Break]: cancels the waiting step.
			CancelStep,
			// Stop and Frame: the strip's [Show] (a waiting member's receiver).
			ShowFrame
		};

		Kind RequestKind = Kind::ToggleBreakpoints;
		std::vector<uint32_t> Nodes;
		bool Enabled = true;
		VisualScriptBreakpoint Breakpoint;
		VisualScriptDebugAction Action = VisualScriptDebugAction::Continue;
		std::optional<uint64_t> Stop;
		uint32_t Frame = 0;

		bool operator==(VisualScriptDebugRequest const& other) const = default;
	};
}

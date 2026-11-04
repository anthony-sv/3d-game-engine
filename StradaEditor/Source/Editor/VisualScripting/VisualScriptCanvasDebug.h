#pragma once

#include "Strada/Script/VisualScriptDebugger.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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
}

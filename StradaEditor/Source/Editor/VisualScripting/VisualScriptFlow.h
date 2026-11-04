#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasDebug.h"
#include "Editor/VisualScripting/VisualScriptFlowTake.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The flow a graph window draws while the game plays (spec-slice4 §13.3): the steps the debug session counted for the window's
// sub-graph, mapped onto its canvas, lit on the flow clock (the game's frames, never the wall clock) and counted since play
// started. Pure; main thread only.
namespace Strada
{
	class VisualScriptCanvasModel;

	// The multiplication sign run counts start with ("×37"); the editor's fonts have it (VisualScriptTextTests).
	inline constexpr std::string_view VisualScriptRunCountPrefix = "\xC3\x97";

	// A run count as the canvas shows it: "×37"; exact up to 9,999, then thousands ("×12k") and millions ("×1.2M", with one
	// decimal under 10), rounded down, in the invariant culture.
	[[nodiscard]] std::string FormatVisualScriptRunCount(uint32_t count);

	// What a flow model gives its window's canvas view for a frame, each list most intense first.
	struct VisualScriptFlowMarks
	{
		std::vector<VisualScriptFlowLinkMark> Links;
		// Nodes that are lit, or counted since play started (or the last reset).
		std::vector<VisualScriptFlowNodeMark> Nodes;
		std::vector<VisualScriptFlowBoxMark> Boxes;
		// The flow clock holds: the last take ran no frame.
		bool Held = false;

		void Clear();
	};

	// One window's flow: per step of its sub-graph (a node and the exit it fired, or none) the flow time it last fired at, the
	// runs counted and where its dots started. Steps are kept by node ID and exit name, and found again on the canvas when its
	// revision changes.
	class VisualScriptFlowModel
	{
	public:
		// The flow clock seconds in which a step's light halves, and the intensity under which it is not drawn (0.25 × log2(20),
		// about 1.08 seconds of game frames).
		static constexpr double HalfLife = 0.25;
		static constexpr float MinIntensity = 0.05f;
		// Dots move along a wire while its intensity is at least this and the flow clock runs.
		static constexpr float DotIntensity = 0.5f;
		// A chain's dots start one after another: a step's dots start min(rank, MaxStaggerRank) × StaggerSeconds of editor time
		// after its take, rank being its FirstOrder rank in the take.
		static constexpr double StaggerSeconds = 0.008;
		static constexpr uint32_t MaxStaggerRank = 30;

		// exp2(-(flowTime - lastFired) / HalfLife): 1 for a step that fired at flowTime (or later).
		[[nodiscard]] static float GetIntensity(double flowTime, double lastFired);

		// Adds a take of the window's sub-graph, taken at editorTime: each step's count and the flow time of the last frame that
		// ran it, mapped onto the canvas (the node through FindNode, the exit's wire through FindExecLink). A step whose wire was
		// not moving dots when the take came starts its dots after the take, staggered by its rank.
		void Apply(VisualScriptFlowTake const& take, VisualScriptCanvasModel const& canvas, uint64_t canvasRevision, double editorTime);
		// The marks at the flow clock flowTime and editor time editorTime; the steps are found again on the canvas when its
		// revision is not the one they were mapped on. reduceMotion: no dots, the light fades in place.
		void Build(VisualScriptCanvasModel const& canvas, uint64_t canvasRevision, double flowTime, double editorTime, bool reduceMotion,
		           VisualScriptFlowMarks& marks);
		// View > Reset Run Counts: counts start again from 0; what is lit stays lit.
		void ResetCounts();
		// Forgets every step (play started or stopped, another sub-graph shown).
		void Clear();

		// The last take ran no frame: the flow clock holds.
		bool IsHeld() const { return m_Held; }
		bool IsEmpty() const { return m_Steps.empty(); }
		// The runs counted for a node ID: the exits it fired (each output of a sequence, each iteration of a loop), or the times it
		// ended its chain without one when that is more, so a function call or a latent node that ran once (entered, then
		// returned or completed) counts once.
		uint32_t GetCount(uint32_t node) const;

	private:
		struct Step
		{
			uint32_t Node = 0;
			// Empty: the node ran without an exit.
			std::string Exit;
			uint32_t Count = 0;
			double LastFired = 0.0;
			// The editor time its dots started; none before it first fired with dots.
			std::optional<double> DotStart;
			// Where it is on the canvas of m_MappedRevision.
			std::optional<uint32_t> NodeIndex;
			std::optional<uint32_t> Link;
			// Merged into an equal step of the same take (Apply's own).
			bool Folded = false;
		};

		// The step of a node and exit among the first count steps (sorted); null when there is none.
		Step* Find(uint32_t node, std::string_view exit, size_t count);
		void Map(Step& step, VisualScriptCanvasModel const& canvas) const;
		void Remap(VisualScriptCanvasModel const& canvas, uint64_t canvasRevision);

		// By (Node, Exit).
		std::vector<Step> m_Steps;
		uint64_t m_MappedRevision = 0;
		bool m_Mapped = false;
		bool m_Held = false;
		// Per frame, reused: the lit nodes' indices in the canvas and their intensities.
		std::vector<std::pair<uint32_t, float>> m_LitNodes;
	};
}

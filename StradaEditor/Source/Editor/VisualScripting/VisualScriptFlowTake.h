#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Strada
{
	// A step of a watched sub-graph accumulated over the frames since the window last drained them: how often the flow left
	// Node through Exit (empty: it ran without one), the order in which the accumulation first saw it (steps of the first
	// frame keep their order, later frames' new steps come after them), and the flow clock after the last frame that ran it.
	struct VisualScriptFlowStep
	{
		uint32_t Node = 0;
		std::string Exit;
		uint32_t Count = 0;
		uint32_t FirstOrder = 0;
		double LastTime = 0.0;

		bool operator==(VisualScriptFlowStep const& other) const = default;
	};

	// What the debug session hands the window drawing a watched sub-graph each time it drains it (VisualScriptDebugSession::
	// TakeFlow): the steps since the previous take, in FirstOrder order.
	struct VisualScriptFlowTake
	{
		std::vector<VisualScriptFlowStep> Steps;
		// The flow clock when taken: the sum of the time steps of the scene frames run since play started.
		double Time = 0.0;
		// Scene frames merged into it.
		uint32_t Frames = 0;
		// No frame ran since the previous take: the flow clock holds (paused, stopped, between commands).
		bool Held = false;

		bool operator==(VisualScriptFlowTake const& other) const = default;
	};
}

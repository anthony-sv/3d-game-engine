#pragma once

#include "Editor/Automation/CommandRegistry.h"

#include "Strada/Core/Profiler.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <functional>

namespace Strada
{
	class EditorContext;
	class EditorProfiler;

	// Registers profiler.start, profiler.stop, profiler.status, profiler.capture and profiler.open (Docs/Automation.md),
	// none of them an undo step. showPanel brings the Profiler panel forward when profiler.open opened a capture; unset when
	// the editor has none (headless). The registry, the profiler and the context must outlive every registered command.
	[[nodiscard]] Result<void> RegisterProfilerCommands(CommandRegistry& registry, EditorProfiler& profiler, EditorContext& context,
	                                                    std::function<void()> showPanel);

	// Medians as the profiler commands report them: { frameMilliseconds, framePercentile99Milliseconds, subsystems, threads,
	// gpuPasses, zones, zonesOmitted }, the lists [{ name, milliseconds }] sorted by name but zones, [{ name,
	// millisecondsPerFrame, millisecondsPerCall, callsPerFrame }] sorted by their time per frame, the most first, at most
	// maxZones of them (zonesOmitted counts the rest).
	[[nodiscard]] Json DescribeProfilerMedians(ProfilerMedians const& medians, size_t maxZones);
}

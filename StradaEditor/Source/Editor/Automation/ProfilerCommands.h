#pragma once

#include "Editor/Automation/CommandRegistry.h"

#include "Strada/Core/Profiler.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace Strada
{
	class EditorContext;
	class EditorProfiler;

	// Registers profiler.start, profiler.stop, profiler.status, profiler.capture and profiler.open (Docs/Automation.md),
	// none of them an undo step. showPanel brings the Profiler panel forward when profiler.open opened a capture; unset when
	// the editor has none (headless). The registry, the profiler and the context must outlive every registered command.
	[[nodiscard]] Result<void> RegisterProfilerCommands(CommandRegistry& registry, EditorProfiler& profiler, EditorContext& context,
	                                                    std::function<void()> showPanel);

	// A file a command names by its parameter path, as profiler.capture and telemetry.export resolve theirs: absolute as given
	// (made lexically normal), else relative to the open project's directory and inside it (never the directory itself).
	// InvalidParams naming path for a path that is neither ("C:file", "\file", one that climbs out of the project);
	// InvalidOperation for a relative path while no project is open.
	[[nodiscard]] CommandValue<std::filesystem::path> ResolveCommandFilePath(std::string const& text, EditorContext const& context);

	// Medians as the profiler commands report them: { frameMilliseconds, framePercentile99Milliseconds, subsystems, threads,
	// gpuPasses, zones, zonesOmitted }, the lists [{ name, milliseconds }] sorted by name but zones, [{ name,
	// millisecondsPerFrame, millisecondsPerCall, callsPerFrame }] sorted by their time per frame, the most first, at most
	// maxZones of them (zonesOmitted counts the rest).
	[[nodiscard]] Json DescribeProfilerMedians(ProfilerMedians const& medians, size_t maxZones);
}

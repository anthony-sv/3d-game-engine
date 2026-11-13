// Compiles the profiling macros in the form Dist builds give them, in every configuration: defined before the first
// include (StradaTests has no precompiled header), ST_DISABLE_PROFILING selects that form.
#define ST_DISABLE_PROFILING

#include "Strada/Core/Profiler.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#ifdef ST_ENABLE_PROFILING
#error "ST_DISABLE_PROFILING must select the Dist form of the profiling macros"
#endif

using namespace Strada;

namespace
{
	int s_Evaluations = 0;

	std::string EvaluateName()
	{
		++s_Evaluations;
		return "Evaluated";
	}

	// Every macro, with locals that only the macros use: the Dist form references its arguments without evaluating
	// them, so this compiles without an unused-variable warning and runs nothing.
	void UseEveryMacro()
	{
		ST_PROFILE_THREAD("Dist.Thread");
		ST_PROFILE_FUNCTION();
		char const* const name = "Dist.Local";
		ST_PROFILE_SCOPE(name);
		ST_PROFILE_SCOPE_DYNAMIC(EvaluateName());
		ST_PROFILE_ZONE(zone, "Dist.Zone");
		uint64_t const steps = 3;
		ST_PROFILE_ZONE_VALUE(zone, "Steps", steps);
		ST_PROFILE_ZONE_DYNAMIC(dynamicZone, EvaluateName());
		ST_PROFILE_ZONE_VALUE(dynamicZone, "Steps", steps + 1);
	}
}

TEST_CASE("Profiler: the Dist form of the macros records and evaluates nothing")
{
	Profiler::Init();
	// Dist builds refuse to record; other builds record here, and must see no zone from this translation unit.
	bool const recording = Profiler::Start().IsOk();
	Profiler::BeginFrame();
	UseEveryMacro();
	Profiler::EndFrame();
	CHECK(s_Evaluations == 0);
	if (recording)
	{
		ProfilerCapture const& capture = Profiler::GetRecording();
		REQUIRE(capture.Frames.size() == 1);
		CHECK(capture.Frames[0].Zones.empty());
		CHECK(capture.Frames[0].DroppedZones == 0);
	}
	Profiler::Shutdown();
}

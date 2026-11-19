#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Core/Profiler.h"
#include "Strada/Core/Telemetry.h"

namespace Strada
{
	void Scene::SampleTelemetry(float step)
	{
		if (!Telemetry::IsRecording()) [[likely]]
		{
			return;
		}
		ST_PROFILE_SCOPE("Scene.Telemetry");
		// The engine's own channels of the step are recorded here, before the step's row ends: each subsystem that has
		// channels adds one call, in a fixed order, recording its entities in UUID order.
		Telemetry::EndStep({GetSerial(), m_FixedStepCount, step});
	}
}

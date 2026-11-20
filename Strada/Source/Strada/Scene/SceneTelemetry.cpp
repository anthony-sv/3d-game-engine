#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Core/Profiler.h"
#include "Strada/Core/Telemetry.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>

namespace Strada
{
	// The inputs, the engine, the gearbox, the speed and the lean, then four quantities a wheel (VehicleChannelQuantities).
	static_assert(VehicleChannelCount == 8 + 4 * VehicleWheelCount);

	void Scene::SampleTelemetry(float step)
	{
		if (!Telemetry::IsRecording()) [[likely]]
		{
			return;
		}
		ST_PROFILE_SCOPE("Scene.Telemetry");
		// The engine's own channels of the step are recorded here, before the step's row ends: each subsystem that has
		// channels adds one call, in a fixed order, recording its entities in UUID order.
		SampleVehicleTelemetry();
		Telemetry::EndStep({GetSerial(), m_FixedStepCount, step});
	}

	void Scene::SampleVehicleTelemetry()
	{
		if (!m_Physics)
		{
			return;
		}
		// Names built when each vehicle was made, and an order rebuilt only when a vehicle came or went: a step records
		// without formatting, sorting or allocating.
		for (entt::entity const handle : GetVehicleOrder())
		{
			VehicleSyncState const& vehicle = m_Vehicles.at(handle);
			std::optional<VehicleState> const state = vehicle.Made ? m_Physics->GetVehicleState(vehicle.Entity) : std::nullopt;
			if (!state)
			{
				continue;
			}
			std::array<float, VehicleChannelCount> values{};
			values[0] = state->AppliedInputs.Throttle;
			values[1] = state->AppliedInputs.FrontBrake;
			values[2] = state->AppliedInputs.RearBrake;
			values[3] = state->AppliedInputs.Lean;
			values[4] = state->EngineSpeed;
			values[5] = static_cast<float>(state->Gear);
			values[6] = state->ForwardSpeed;
			values[7] = glm::degrees(state->LeanAngle);
			for (size_t wheel = 0; wheel < VehicleWheelCount; wheel++)
			{
				VehicleWheelState const& reading = state->Wheels[wheel];
				size_t const first = 8 + 4 * wheel;
				values[first] = reading.LongitudinalSlip;
				values[first + 1] = glm::degrees(reading.LateralSlip);
				values[first + 2] = reading.SuspensionLength;
				values[first + 3] = reading.TyreGrip;
			}
			for (size_t index = 0; index < VehicleChannelCount; index++)
			{
				Telemetry::Record(vehicle.ChannelNames[index], values[index]);
			}
		}
	}
}

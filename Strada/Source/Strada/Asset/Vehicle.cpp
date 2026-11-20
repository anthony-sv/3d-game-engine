#include "stpch.h"
#include "Strada/Asset/Vehicle.h"

#include "Strada/Asset/VehicleAsset.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace Strada
{
	namespace
	{
		char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		// A direction of length 1, or empty for zero and values that are not finite: as the physics world normalizes the
		// definition's directions (PhysicsScene::AddVehicle), so that what validates is what the world takes.
		std::optional<glm::vec3> Normalize(glm::vec3 const& value)
		{
			if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
			{
				return std::nullopt;
			}
			float const largest = std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
			if (!(largest > 0.0f))
			{
				return std::nullopt;
			}
			glm::vec3 const scaled = value / largest;
			return scaled / glm::length(scaled);
		}

		// Up and Forward as NormalizeVehicleAxes (Strada/Physics/PhysicsTypes.h) takes them: neither zero, and not parallel
		// within a thousandth of a radian.
		bool AreUsableAxes(glm::vec3 const& up, glm::vec3 const& forward)
		{
			std::optional<glm::vec3> const unitUp = Normalize(up);
			std::optional<glm::vec3> const unitForward = Normalize(forward);
			return unitUp && unitForward && Normalize(*unitForward - *unitUp * glm::dot(*unitForward, *unitUp)) &&
			       glm::length(glm::cross(*unitForward, *unitUp)) >= 1e-3f;
		}

		Result<void> ValidateWheel(VehicleWheelDefinition const& wheel, size_t index)
		{
			if (wheel.Name.empty() || wheel.Name.size() > MaxVehicleWheelNameLength)
			{
				return MakeError("Wheels[{}].Name: a wheel's name has 1 to {} bytes, not {}", index, MaxVehicleWheelNameLength,
				                 wheel.Name.size());
			}
			if (wheel.SuspensionMaxLength < wheel.SuspensionMinLength)
			{
				return MakeError("Wheels[{}].SuspensionMaxLength: {} is below SuspensionMinLength {}", index, wheel.SuspensionMaxLength,
				                 wheel.SuspensionMinLength);
			}
			if (!Normalize(wheel.SuspensionDirection))
			{
				return MakeError("Wheels[{}].SuspensionDirection: the direction is zero", index);
			}
			if (!Normalize(wheel.SteeringAxis))
			{
				return MakeError("Wheels[{}].SteeringAxis: the direction is zero", index);
			}
			if (index == 0 && !(wheel.MaxSteerAngle > 0.0f))
			{
				return MakeError("Wheels[0].MaxSteerAngle: the front wheel steers, so its largest steering angle is above 0");
			}
			if (index == 1 && wheel.MaxSteerAngle != 0.0f)
			{
				return MakeError("Wheels[1].MaxSteerAngle: the rear wheel does not steer, so its largest steering angle is 0, not {}",
				                 wheel.MaxSteerAngle);
			}
			return {};
		}
	}

	bool IsSameVehicleWheelName(std::string_view first, std::string_view second)
	{
		return std::equal(first.begin(), first.end(), second.begin(), second.end(),
		                  [](char firstCharacter, char secondCharacter)
		                  {
							  return ToLowerAscii(firstCharacter) == ToLowerAscii(secondCharacter);
						  });
	}

	Result<void> ValidateVehicle(Vehicle const& vehicle)
	{
		if (vehicle.Wheels.size() != VehicleWheelCount)
		{
			return MakeError("Wheels: a motorcycle has exactly {} wheels, the front and then the rear, not {}", VehicleWheelCount,
			                 vehicle.Wheels.size());
		}
		for (size_t index = 0; index < vehicle.Wheels.size(); index++)
		{
			if (Result<void> valid = ValidateWheel(vehicle.Wheels[index], index); !valid)
			{
				return valid;
			}
			for (size_t earlier = 0; earlier < index; earlier++)
			{
				if (IsSameVehicleWheelName(vehicle.Wheels[earlier].Name, vehicle.Wheels[index].Name))
				{
					return MakeError("Wheels[{}].Name: '{}' names Wheels[{}] too (names are unique in any letter case)", index,
					                 vehicle.Wheels[index].Name, earlier);
				}
			}
		}
		if (!AreUsableAxes(vehicle.Up, vehicle.Forward))
		{
			return MakeError("Up, Forward: the chassis's up and front are neither zero nor parallel");
		}
		VehicleEngineDefinition const& engine = vehicle.Engine;
		if (!(engine.MinRPM < engine.MaxRPM))
		{
			return MakeError("Engine.MinRPM: the idle speed {} is not below MaxRPM {}", engine.MinRPM, engine.MaxRPM);
		}
		VehicleTransmissionDefinition const& transmission = vehicle.Transmission;
		if (transmission.GearRatios.empty())
		{
			return MakeError("Transmission.GearRatios: a gearbox has 1 to {} gears", MaxVehicleGears);
		}
		if (!(transmission.ShiftDownRPM < transmission.ShiftUpRPM))
		{
			return MakeError("Transmission.ShiftDownRPM: {} is not below ShiftUpRPM {}", transmission.ShiftDownRPM,
			                 transmission.ShiftUpRPM);
		}
		if (transmission.Mode == VehicleTransmissionMode::Automatic && !(transmission.ShiftUpRPM < engine.MaxRPM))
		{
			return MakeError("Transmission.ShiftUpRPM: an automatic gearbox shifts up below the engine's MaxRPM {}, not at {}",
			                 engine.MaxRPM, transmission.ShiftUpRPM);
		}
		return {};
	}
}

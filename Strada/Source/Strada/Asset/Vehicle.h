#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The vehicle definition (Docs/Architecture.md §8, "Vehicles"): a motorcycle's two wheels, its engine, its gearbox, its
// final drive and how it leans, as Jolt's motorcycle controller takes them. The data of a .svehicle asset
// (Strada/Asset/VehicleAsset.h has its field tables and registration); a Vehicle component names one. Lengths are meters,
// angles degrees, torques newton meters, engine speeds revolutions per minute; points and directions are in the chassis
// entity's local space without its scale (the chassis's scale moves no wheel). Plain data, any thread.
namespace Strada
{
	// A definition's wheels: exactly two, the front (Vehicle::Wheels[0], which steers) and the rear (Wheels[1], which the
	// engine drives).
	inline constexpr size_t VehicleWheelCount = 2;
	// The most forward gears a gearbox has (VehicleTransmissionDefinition::GearRatios).
	inline constexpr size_t MaxVehicleGears = 16;
	// The longest wheel name, in bytes (VehicleWheelDefinition::Name).
	inline constexpr size_t MaxVehicleWheelNameLength = 64;

	// How a vehicle's gearbox changes gear.
	enum class VehicleTransmissionMode : uint8_t
	{
		// By itself, at the shift points (Jolt's automatic gearbox).
		Automatic = 0,
		// When the rider asks (PhysicsScene::RequestVehicleShift), one gear per request, the clutch opened for SwitchTime and
		// closed again over ClutchReleaseTime.
		Manual
	};

	struct VehicleWheelDefinition
	{
		// The wheel's name, which a VehicleWheel component names to draw it: 1 to MaxVehicleWheelNameLength bytes, unique in
		// the definition ignoring letter case.
		std::string Name = "Front";
		// Where the suspension is attached.
		glm::vec3 Position = glm::vec3(0.0f, 0.66f, -0.55f);
		// The way the suspension extends from Position, pointing down (normalized when the vehicle is made; not zero). A
		// fork's leans forward by the rake.
		glm::vec3 SuspensionDirection = glm::vec3(0.0f, -0.866025f, -0.5f);
		// The wheel's center lies between these distances from Position along SuspensionDirection (0 <= min <= max).
		float SuspensionMinLength = 0.3f;
		float SuspensionMaxLength = 0.5f;
		// How far the spring is already compressed at SuspensionMaxLength (at least 0).
		float SuspensionPreload = 0.0f;
		// The spring's frequency (hertz, above 0) and damping ratio (at least 0). A new definition's 2.5 holds a 250 kg chassis
		// off its stops (the 1.5 of Jolt's motorcycle sample's front spring lets it sit on its shortest length).
		float SuspensionFrequency = 2.5f;
		float SuspensionDamping = 0.5f;
		// The axis the wheel steers around, pointing up (normalized; not zero): for a fork, against SuspensionDirection.
		glm::vec3 SteeringAxis = glm::vec3(0.0f, 0.866025f, 0.5f);
		// The largest steering angle, degrees (0 to 90): above 0 for the front wheel, 0 for the rear.
		float MaxSteerAngle = 30.0f;
		// The tyre's radius (above 0) and width (at least 0).
		float Radius = 0.31f;
		float Width = 0.05f;
		// The wheel's moment of inertia around its axle (kg m², above 0) and how fast its spin decays by itself (per second,
		// at least 0).
		float Inertia = 0.9f;
		float AngularDamping = 0.2f;
		// The torque the front brake applies at this wheel at full input, and the torque the rear brake applies at it (at
		// least 0 each); a wheel given both adds them.
		float FrontBrakeTorque = 500.0f;
		float RearBrakeTorque = 0.0f;
		// The tyre's friction coefficient over the longitudinal slip ratio, and over the slip angle in degrees: Curve assets
		// (.scurve). None, or a reference that is no loadable Curve, is Jolt's default tyre (WheeledVehicleController.cpp:44-56
		// of the pinned Jolt). The surface's tyre grip (PhysicsMaterial::TyreGrip) multiplies both.
		AssetHandle LongitudinalFriction;
		AssetHandle LateralFriction;

		bool operator==(VehicleWheelDefinition const&) const = default;
	};

	struct VehicleEngineDefinition
	{
		// The largest torque the engine gives (above 0), at full throttle.
		float MaxTorque = 150.0f;
		// The engine's speed range (0 <= min < max): it idles at the lowest and never turns faster than the highest.
		float MinRPM = 1000.0f;
		float MaxRPM = 10000.0f;
		// The torque over the engine's speed in revolutions per minute, as a fraction of MaxTorque: a Curve asset. None, or a
		// reference that is no loadable Curve, is Jolt's default (0.8, 1.0 and 0.8 at 0, 0.66 and 1 times MaxRPM).
		AssetHandle Torque;
		// The engine's moment of inertia (kg m², above 0) and how fast its speed decays by itself (per second, at least 0).
		float Inertia = 0.5f;
		float AngularDamping = 0.2f;
		// How much faster the engine's speed decays in neutral as the throttle closes (per second, at least 0;
		// Docs/Architecture.md §8, "The engine in neutral"): while the gearbox is in neutral the engine is damped by
		// AngularDamping plus this times how far the throttle is closed, so a revved engine falls back to idle with no gear to
		// hold it, while a full throttle revs it as without this and in gear AngularDamping alone acts. 0 is the engine as
		// built.
		float NeutralDamping = 0.0f;

		bool operator==(VehicleEngineDefinition const&) const = default;
	};

	struct VehicleTransmissionDefinition
	{
		VehicleTransmissionMode Mode = VehicleTransmissionMode::Automatic;
		// The forward gears' ratios between the engine and the gearbox's output, first gear first: 1 to MaxVehicleGears of
		// them, each above 0. There is no reverse gear.
		std::vector<float> GearRatios = {2.27f, 1.63f, 1.3f, 1.09f, 0.96f, 0.88f};
		// The ratio from the gearbox's output to the rear wheel (above 0): the primary and final drives together.
		float FinalDrive = 4.825f;
		// Seconds the clutch stays open when the gear changes, then seconds it takes to close (at least 0 each): both modes.
		float SwitchTime = 0.5f;
		float ClutchReleaseTime = 0.3f;
		// Automatic mode only: seconds after a shift before the next, and the engine speeds above which it shifts up and below
		// which it shifts down (0 < down < up, and up below the engine's MaxRPM).
		float SwitchLatency = 0.5f;
		float ShiftUpRPM = 8000.0f;
		float ShiftDownRPM = 2000.0f;
		// How hard the closed clutch couples the engine to the gearbox (above 0): its torque is this times the difference of
		// their speeds.
		float ClutchStrength = 2.0f;

		bool operator==(VehicleTransmissionDefinition const&) const = default;
	};

	struct VehicleLeanDefinition
	{
		// The farthest the bike leans in turns, degrees (0 to 90).
		float MaxLeanAngle = 45.0f;
		// The lean spring's constant and damping (at least 0 each), which turn the bike towards its target lean.
		float SpringConstant = 5000.0f;
		float SpringDamping = 1000.0f;
		// How much of its last target lean the next keeps (0 to 1: 0 none, 1 never changes), once per collision step.
		float SmoothingFactor = 0.8f;
		// Whether the steering is limited by the speed, so that a turn never asks for more lean than MaxLeanAngle.
		bool SteeringLimit = true;

		bool operator==(VehicleLeanDefinition const&) const = default;
	};

	// The wheels of a new definition: the proportions of Jolt's motorcycle sample (MotorcycleTest.cpp:34-50 of the pinned
	// Jolt: tyres of radius 0.31, a fork raked 30 degrees, a wheelbase of 1.5 m at rest), with the chassis's origin on the
	// ground between the tyres and its front along -Z.
	inline std::vector<VehicleWheelDefinition> MakeDefaultVehicleWheels()
	{
		VehicleWheelDefinition const front;
		VehicleWheelDefinition rear;
		rear.Name = "Rear";
		rear.Position = glm::vec3(0.0f, 0.71f, 0.75f);
		rear.SuspensionDirection = glm::vec3(0.0f, -1.0f, 0.0f);
		rear.SteeringAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		rear.MaxSteerAngle = 0.0f;
		rear.FrontBrakeTorque = 0.0f;
		rear.RearBrakeTorque = 250.0f;
		return {front, rear};
	}

	struct Vehicle
	{
		// Exactly VehicleWheelCount: the front, then the rear. Other data names a wheel by its Name, but the order makes each
		// the front or the rear, so editors keep it (FieldHints::FixedOrder).
		std::vector<VehicleWheelDefinition> Wheels = MakeDefaultVehicleWheels();
		// The chassis's up and front in its local space (normalized when the vehicle is made, the front made square to the up;
		// not zero, not parallel).
		glm::vec3 Up = glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 Forward = glm::vec3(0.0f, 0.0f, -1.0f);
		// The largest pitch and roll the chassis takes, degrees (0 to 180; 180 does not limit them).
		float MaxPitchRoll = 60.0f;
		VehicleEngineDefinition Engine;
		VehicleTransmissionDefinition Transmission;
		VehicleLeanDefinition Lean;

		bool operator==(Vehicle const&) const = default;
	};

	// Whether two wheel names are the same in any letter case (ASCII), as a VehicleWheel component and the definition's
	// uniqueness compare them. Pure; any thread.
	[[nodiscard]] bool IsSameVehicleWheelName(std::string_view first, std::string_view second);

	// What the field tables cannot state (the data asset's Validate): exactly two wheels; their names of 1 to
	// MaxVehicleWheelNameLength bytes and unique ignoring letter case; each suspension's longest length at least its shortest;
	// the front steering (MaxSteerAngle above 0) and the rear not (0); directions that are not zero, Up and Forward not
	// parallel (as the physics world takes them, NormalizeVehicleAxes); MinRPM below MaxRPM; at least one gear;
	// ShiftDownRPM below ShiftUpRPM, and in Automatic mode ShiftUpRPM below the engine's MaxRPM. So a definition that
	// validates is one PhysicsScene::AddVehicle takes. Refuses and never repairs; each error names its field
	// ("Wheels[1].MaxSteerAngle: the rear wheel does not steer, ..."). It reads the definition alone: a curve reference of
	// another type, or of no asset, is not refused (a vehicle made from the definition counts it as unset and says so once).
	// Pure; any thread.
	[[nodiscard]] Result<void> ValidateVehicle(Vehicle const& vehicle);
}

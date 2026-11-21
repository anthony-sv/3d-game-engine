#pragma once

#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructDocument.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <string_view>
#include <tuple>
#include <utility>

// The Vehicle data asset type (.svehicle, Docs/Architecture.md §8, "Vehicles", and §7.3): the engine's Vehicle
// (Strada/Asset/Vehicle.h) as field tables, and its registration's specification (DataAssetRegistry::AddEngineTypes). C++
// readers of a vehicle asset include this header. The physics world never includes it: its types include Vehicle.h for the
// definition's struct alone (MakeVehicleDesc), and PhysicsScene takes a VehicleDesc.
namespace Strada
{
	template<>
	struct EnumTraits<VehicleTransmissionMode>
	{
		static constexpr std::array<std::pair<VehicleTransmissionMode, std::string_view>, 2> Values = {{
			{VehicleTransmissionMode::Automatic, "Automatic"},
			{VehicleTransmissionMode::Manual, "Manual"},
		}};
	};

	template<>
	struct StructTraits<VehicleWheelDefinition>
	{
		static constexpr std::string_view Name = "VehicleWheelDefinition";
		static constexpr auto Fields = std::make_tuple(
			Field("Name", &VehicleWheelDefinition::Name)
				.Doc("The wheel's name, which a VehicleWheel component names to draw it; unique in the vehicle, ignoring letter case."),
			Field("Position", &VehicleWheelDefinition::Position)
				.Range(-1.0e3, 1.0e3)
				.Doc("Where the suspension is attached, in meters in the chassis's local space (the chassis's scale does not apply)."),
			Field("SuspensionDirection", &VehicleWheelDefinition::SuspensionDirection)
				.Range(-1.0, 1.0)
				.Doc("The way the suspension extends from Position, pointing down (a fork's leans forward by the rake); not zero."),
			Field("SuspensionMinLength", &VehicleWheelDefinition::SuspensionMinLength)
				.Range(0.0, 100.0)
				.Doc("Meters from Position to the wheel's center with the suspension fully compressed."),
			Field("SuspensionMaxLength", &VehicleWheelDefinition::SuspensionMaxLength)
				.Range(0.0, 100.0)
				.Doc("Meters from Position to the wheel's center with the suspension fully extended; at least SuspensionMinLength."),
			Field("SuspensionPreload", &VehicleWheelDefinition::SuspensionPreload)
				.Range(0.0, 100.0)
				.Doc("Meters the spring is already compressed at SuspensionMaxLength."),
			Field("SuspensionFrequency", &VehicleWheelDefinition::SuspensionFrequency)
				.Range(0.01, 100.0)
				.Doc("The suspension spring's frequency in hertz: higher is stiffer."),
			Field("SuspensionDamping", &VehicleWheelDefinition::SuspensionDamping)
				.Range(0.0, 100.0)
				.Doc("The suspension's damping ratio: 0 bounces freely, 1 settles without bouncing."),
			Field("SteeringAxis", &VehicleWheelDefinition::SteeringAxis)
				.Range(-1.0, 1.0)
				.Doc("The axis the wheel steers around, pointing up (for a fork, against SuspensionDirection); not zero."),
			Field("MaxSteerAngle", &VehicleWheelDefinition::MaxSteerAngle)
				.Range(0.0, 90.0)
				.AsAngle()
				.Doc("The largest steering angle in degrees: above 0 for the front wheel, 0 for the rear."),
			Field("Radius", &VehicleWheelDefinition::Radius).Range(0.01, 10.0).Doc("The tyre's radius in meters."),
			Field("Width", &VehicleWheelDefinition::Width).Range(0.0, 10.0).Doc("The tyre's width in meters."),
			Field("Inertia", &VehicleWheelDefinition::Inertia)
				.Range(0.001, 1.0e3)
				.Doc("The wheel's moment of inertia around its axle in kg m² (half its mass times the radius squared for a disc)."),
			Field("AngularDamping", &VehicleWheelDefinition::AngularDamping)
				.Range(0.0, 100.0)
				.Doc("How fast the wheel's spin decays by itself, per second."),
			Field("FrontBrakeTorque", &VehicleWheelDefinition::FrontBrakeTorque)
				.Range(0.0, 1.0e5)
				.Doc("The torque in N m the front brake applies at this wheel at full input (usually 0 at the rear)."),
			Field("RearBrakeTorque", &VehicleWheelDefinition::RearBrakeTorque)
				.Range(0.0, 1.0e5)
				.Doc("The torque in N m the rear brake applies at this wheel at full input (usually 0 at the front)."),
			Field("LongitudinalFriction", &VehicleWheelDefinition::LongitudinalFriction)
				.References("Curve")
				.Doc("A curve of the tyre's friction over the longitudinal slip ratio; none is the engine's default tyre. The "
		             "surface's TyreGrip multiplies it."),
			Field("LateralFriction", &VehicleWheelDefinition::LateralFriction)
				.References("Curve")
				.Doc("A curve of the tyre's friction over the slip angle in degrees; none is the engine's default tyre. The surface's "
		             "TyreGrip multiplies it."));
	};

	template<>
	struct StructTraits<VehicleEngineDefinition>
	{
		static constexpr std::string_view Name = "VehicleEngineDefinition";
		static constexpr auto Fields = std::make_tuple(
			Field("MaxTorque", &VehicleEngineDefinition::MaxTorque)
				.Range(0.01, 1.0e5)
				.Doc("The engine's largest torque in N m, at full throttle."),
			Field("MinRPM", &VehicleEngineDefinition::MinRPM)
				.Range(0.0, 1.0e5)
				.Doc("The engine's idle speed in revolutions per minute; below MaxRPM."),
			Field("MaxRPM", &VehicleEngineDefinition::MaxRPM)
				.Range(1.0, 1.0e5)
				.Doc("The engine's highest speed in revolutions per minute."),
			Field("Torque", &VehicleEngineDefinition::Torque)
				.References("Curve")
				.Doc("A curve of the engine's torque over its speed in revolutions per minute, as a fraction of MaxTorque; none "
		             "is the engine's default shape."),
			Field("Inertia", &VehicleEngineDefinition::Inertia)
				.Range(0.001, 1.0e3)
				.Doc("The engine's moment of inertia in kg m²: higher revs up more slowly."),
			Field("AngularDamping", &VehicleEngineDefinition::AngularDamping)
				.Range(0.0, 100.0)
				.Doc("How fast the engine's speed decays by itself, per second."),
			Field("NeutralDamping", &VehicleEngineDefinition::NeutralDamping)
				.Range(0.0, 100.0)
				.Doc("How much faster, per second, the engine's speed decays while it turns free of the wheels (in neutral, or with "
		             "a Manual gearbox's clutch held open below idle), times how far the throttle is closed: a revved engine falls "
		             "back to idle with no gear to hold it, while a full throttle and a closed clutch in gear are as without it. 0 "
		             "adds nothing."));
	};

	template<>
	struct StructTraits<VehicleTransmissionDefinition>
	{
		static constexpr std::string_view Name = "VehicleTransmissionDefinition";
		static constexpr auto Fields = std::make_tuple(
			Field("Mode", &VehicleTransmissionDefinition::Mode)
				.Doc("Automatic shifts by itself at the shift points; Manual shifts when the rider asks, one gear at a time."),
			Field("GearRatios", &VehicleTransmissionDefinition::GearRatios)
				.Range(0.01, 100.0)
				.MaxCount(MaxVehicleGears)
				.Doc("The forward gears' ratios between the engine and the gearbox's output, first gear first (1 to 16). There is "
		             "no reverse gear."),
			Field("FinalDrive", &VehicleTransmissionDefinition::FinalDrive)
				.Range(0.01, 1.0e3)
				.Doc("The ratio from the gearbox's output to the rear wheel: the primary and final drives together."),
			Field("SwitchTime", &VehicleTransmissionDefinition::SwitchTime)
				.Range(0.0, 10.0)
				.Doc("Seconds the clutch stays open when the gear changes."),
			Field("ClutchReleaseTime", &VehicleTransmissionDefinition::ClutchReleaseTime)
				.Range(0.0, 10.0)
				.Doc("Seconds the clutch then takes to close."),
			Field("SwitchLatency", &VehicleTransmissionDefinition::SwitchLatency)
				.Range(0.0, 10.0)
				.VisibleWhen("Mode", "Automatic")
				.Doc("Seconds after a shift before the automatic gearbox shifts again."),
			Field("ShiftUpRPM", &VehicleTransmissionDefinition::ShiftUpRPM)
				.Range(1.0, 1.0e5)
				.VisibleWhen("Mode", "Automatic")
				.Doc("The engine speed in revolutions per minute above which the automatic gearbox shifts up; below the engine's "
		             "MaxRPM."),
			Field("ShiftDownRPM", &VehicleTransmissionDefinition::ShiftDownRPM)
				.Range(1.0, 1.0e5)
				.VisibleWhen("Mode", "Automatic")
				.Doc("The engine speed in revolutions per minute below which the automatic gearbox shifts down; below ShiftUpRPM."),
			Field("ClutchStrength", &VehicleTransmissionDefinition::ClutchStrength)
				.Range(0.001, 1.0e4)
				.Doc("How hard the closed clutch couples the engine to the gearbox: its torque is this times the difference of "
		             "their speeds."));
	};

	template<>
	struct StructTraits<VehicleLeanDefinition>
	{
		static constexpr std::string_view Name = "VehicleLeanDefinition";
		static constexpr auto Fields =
			std::make_tuple(Field("MaxLeanAngle", &VehicleLeanDefinition::MaxLeanAngle)
		                        .Range(0.0, 90.0)
		                        .AsAngle()
		                        .Doc("The farthest the bike leans in turns, in degrees."),
		                    Field("SpringConstant", &VehicleLeanDefinition::SpringConstant)
		                        .Range(0.0, 1.0e6)
		                        .Doc("How hard the lean spring turns the bike towards its target lean."),
		                    Field("SpringDamping", &VehicleLeanDefinition::SpringDamping)
		                        .Range(0.0, 1.0e6)
		                        .Doc("How much the lean spring resists the bike's rate of leaning."),
		                    Field("SmoothingFactor", &VehicleLeanDefinition::SmoothingFactor)
		                        .Range(0.0, 1.0)
		                        .Doc("How much of its last target lean the bike keeps at each collision step: 0 none, 1 never changes."),
		                    Field("SteeringLimit", &VehicleLeanDefinition::SteeringLimit)
		                        .Doc("Limits the steering by the speed, so that a turn never asks for more lean than MaxLeanAngle."));
	};

	template<>
	struct StructTraits<Vehicle>
	{
		static constexpr std::string_view Name = "Vehicle";
		static constexpr auto Fields = std::make_tuple(
			Field("Wheels", &Vehicle::Wheels)
				.MaxCount(VehicleWheelCount)
				.FixedOrder()
				.Doc("Exactly two wheels: the front, which steers, then the rear, which the engine drives."),
			Field("Up", &Vehicle::Up).Range(-1.0, 1.0).Doc("The chassis's up in its local space; not zero."),
			Field("Forward", &Vehicle::Forward)
				.Range(-1.0, 1.0)
				.Doc("The chassis's front in its local space; not zero and not parallel to Up."),
			Field("MaxPitchRoll", &Vehicle::MaxPitchRoll)
				.Range(0.0, 180.0)
				.AsAngle()
				.Doc("The largest pitch and roll the chassis takes, in degrees; 180 does not limit them."),
			Field("Engine", &Vehicle::Engine).Doc("The engine."),
			Field("Transmission", &Vehicle::Transmission).Doc("The gearbox, its shifts and the drive to the rear wheel."),
			Field("Lean", &Vehicle::Lean).Doc("How the bike leans: Jolt's lean controller turns the lean input into a target lean."));
	};

	static_assert(Detail::IsDescribedFieldType<Vehicle>(), "A Vehicle asset must be editable by the generic editors");

	inline constexpr std::array<DocumentMigration, 0> VehicleMigrations = {};

	inline constexpr DataAssetTypeSpecification<Vehicle> VehicleAssetSpecification = {
		.Name = "Vehicle",
		.Extension = ".svehicle",
		.FormatVersion = 1,
		.Description = "A motorcycle: its two wheels, its engine, its gearbox, its final drive and how it leans.",
		.Migrations = VehicleMigrations,
		.Validate = &ValidateVehicle,
	};
}

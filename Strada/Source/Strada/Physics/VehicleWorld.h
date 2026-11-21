#pragma once

// Internal to the Physics module (Docs/Architecture.md §8, "Vehicles"): Jolt's vehicle types stay out of every other
// header. PhysicsScene's world owns one VehicleWorld, and its public vehicle functions forward to it.

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Hash.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsSurfaces.h"
#include "Strada/Physics/PhysicsTypes.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/PhysicsStepListener.h>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace JPH
{
	class Constraint;
	class PhysicsSystem;
	class StateRecorder;
	class VehicleConstraint;
	class WheelSettingsWV;
}

namespace Strada
{
	// Jolt's MotorcycleController with readers and writers of the protected values Strada keeps (its target lean, the
	// length of its last step, the lean spring's integral and its largest lean, which Jolt has no setter for), made by its
	// settings' ConstructController. Jolt is not patched: the subclass adds accessors only.
	class StradaMotorcycleController;
	// Jolt's cylinder cast of a wheel down its suspension (VehicleCollisionTesterCastCylinder, with half the wheel's width as
	// its convex radius), on the chassis's object layer and past the chassis's own body, that also passes through trigger
	// colliders, which Jolt's testers do not know (they skip sensor bodies only): a trigger never carries a wheel.
	class VehicleWheelTester;

	// What a wheel's test found under it in the collision step: the entity of the body it stands on (from the body's user
	// data), the physics material of the sub-shape and the values the step uses (a tyre grip of 1 and no rolling
	// resistance off a material, or without contact). Written by its vehicle's post-collide callback and read by its
	// friction combine function, both on the thread that steps the vehicles; read by the main thread between steps.
	struct VehicleWheelSurface
	{
		UUID ContactEntity = UUID::Invalid();
		AssetHandle Material;
		float TyreGrip = 1.0f;
		float RollingResistance = 0.0f;
	};

	// A Manual gearbox's state, which Jolt's manual mode leaves to its caller (VehicleTransmission::Set,
	// VehicleTransmission.h:51 of the pinned Jolt): the gear reached, and the seconds the clutch stays open and then takes to
	// close. All 0 in Automatic mode.
	struct VehicleShiftState
	{
		int32_t Gear = 0;
		float SwitchTimeLeft = 0.0f;
		float ClutchReleaseTimeLeft = 0.0f;
	};

	// What a vehicle made again on a new body or with new values keeps (PhysicsScene::AddVehicle), taken from the entity's
	// vehicle before it goes.
	struct VehicleCarriedState
	{
		VehicleInputs Inputs;
		float EngineSpeed = 0.0f;
		int32_t Gear = 0;
		std::array<float, VehicleWheelCount> WheelSpins{};
		std::array<float, VehicleWheelCount> WheelAngles{};
	};

	// One vehicle of the world. The main thread writes it between steps; during a step only the vehicle's own callbacks
	// touch it, on the thread that steps the vehicles. Its address is stable while it exists: the callbacks hold it.
	struct VehicleRecord
	{
		UUID Entity = UUID::Invalid();
		// The values it was made with or last given, its directions normalized and its Forward made square to its Up
		// (NormalizeVehicleAxes), and the body it is on.
		VehicleDesc Desc;
		JPH::BodyID Body;
		// The world's collider table, which the callbacks read for the surfaces under the wheels.
		std::vector<ColliderMaterial> const* Materials = nullptr;
		// Jolt's objects: the constraint, its controller (owned by the constraint), the wheels' settings (the record's own
		// objects, which Jolt reads at every step, so that values change in place) and the tester.
		JPH::Ref<JPH::VehicleConstraint> Constraint;
		StradaMotorcycleController* Controller = nullptr;
		std::array<JPH::Ref<JPH::WheelSettingsWV>, VehicleWheelCount> WheelSettings;
		JPH::Ref<VehicleWheelTester> Tester;
		// The latest inputs (clamped), which the next steps apply, and those the last step applied.
		VehicleInputs Inputs;
		VehicleInputs AppliedInputs;
		// The sum of the shift requests since the last step (RequestVehicleShift, Manual mode only), which the next step's
		// first collision step applies, and a Manual gearbox's state.
		int32_t PendingShift = 0;
		VehicleShiftState Shift;
		std::array<VehicleWheelSurface, VehicleWheelCount> Surfaces{};
	};

	// The world's vehicles and the one step listener that steps them all (Docs/Architecture.md §8, "Vehicles"). Main thread
	// only, but for OnStep and the callbacks it reaches, which run inside the step on the thread that runs Jolt's
	// step-listener job.
	class VehicleWorld final : public JPH::PhysicsStepListener
	{
	public:
		// The system and the world's collider table, which outlive it.
		VehicleWorld(JPH::PhysicsSystem& system, std::vector<ColliderMaterial> const& materials);
		// Removes the step listener and every vehicle's constraint from the system (before the bodies go).
		~VehicleWorld() override;

		VehicleWorld(VehicleWorld const&) = delete;
		VehicleWorld& operator=(VehicleWorld const&) = delete;

		// Checks a vehicle's values as PhysicsScene::AddVehicle refuses them, and normalizes its directions (Up, Forward and
		// each wheel's suspension direction and steering axis). Pure.
		[[nodiscard]] static Result<VehicleDesc> PrepareDesc(VehicleDesc const& desc);

		// PhysicsScene's vehicle functions, for the body that PhysicsScene found for the entity (its ID and object layer), with
		// a desc PrepareDesc returned. AddVehicle and RemoveVehicle leave the vehicles' constraints out of order in Jolt's
		// list: PhysicsScene then calls ReaddConstraints with its joints.
		[[nodiscard]] Result<void> AddVehicle(VehicleDesc const& desc, JPH::BodyID body, JPH::ObjectLayer objectLayer);
		void RemoveVehicle(UUID entity);
		bool HasVehicle(UUID entity) const;
		size_t GetVehicleCount() const;
		[[nodiscard]] Result<void> SetVehicleSettings(UUID entity, VehicleDesc const& desc);
		void SetVehicleInputs(UUID entity, VehicleInputs const& inputs);
		void RequestVehicleShift(UUID entity, int32_t gears);
		std::optional<VehicleState> GetVehicleState(UUID entity) const;
		std::optional<glm::mat4> GetVehicleWheelTransform(UUID entity, size_t wheel) const;
		VehicleDesc const* FindVehicleSettings(UUID entity) const;
		std::optional<float> GetVehicleRollInertia(UUID entity) const;
		// Appends the entity and the body of each vehicle one of whose wheels touched a surface of the material at its last step
		// (its wheel's slot), in the order of the entities' UUIDs: what an edit of the values only vehicles read wakes
		// (PhysicsScene::SetMaterialValues).
		void AppendVehiclesOnMaterial(AssetHandle material, std::vector<std::pair<UUID, JPH::BodyID>>& vehicles) const;
		// Takes the entity's vehicle out of the world, keeping its values in desc and what a vehicle made again keeps, before
		// its body is replaced; RestoreVehicle makes it again on the new body.
		std::optional<VehicleCarriedState> TakeVehicle(UUID entity, VehicleDesc& desc);
		[[nodiscard]] Result<void> RestoreVehicle(VehicleDesc const& desc, VehicleCarriedState const& state, JPH::BodyID body,
		                                          JPH::ObjectLayer objectLayer);
		// Makes the entity's vehicle again on the body it stands on, with the values it runs with and its tester's object layer,
		// carrying only its latest inputs (Docs/Architecture.md §8, "The reset"): what MakeVehicle gives a vehicle made there
		// from nothing, its record's Manual gearbox state and pending shift count included. Its body exists while the vehicle
		// does, so it is made again whenever it is found. Leaves its constraint out of order in Jolt's list, as AddVehicle
		// does; the chassis's velocities are PhysicsScene's. Fails, changing nothing, for an entity without a vehicle.
		[[nodiscard]] Result<void> ResetVehicle(UUID entity);

		// Takes every vehicle's constraint and the joints given out of Jolt's list and adds them again: the vehicles in the
		// order of their entities' UUIDs, then the joints in the order given (theirs). Registers the world's step listener while
		// there is a vehicle and removes it otherwise, so that a world without vehicles runs Jolt's update exactly as before.
		void ReaddConstraints(std::vector<JPH::Constraint*> const& joints);

		// Before Jolt's update, in the order of the entities' UUIDs: each vehicle's latest inputs to its controller, waking the
		// bodies of those whose inputs are not all 0. Main thread.
		void BeginStep();
		// The step listener, once per collision step: each vehicle, in the order of the entities' UUIDs, inside the zone
		// Physics.Vehicles: a Manual gearbox's gear and clutch over the collision step (the pending shift in the first), the
		// engine's damping for the collision step (EngineAngularDamping, plus EngineNeutralDamping times one less the applied
		// throttle while the engine runs free: Jolt's gearbox then in gear 0, or a Manual gearbox holding its clutch open
		// below idle outside a shift; Docs/Architecture.md §8, "The engine in neutral"), then Jolt's
		// VehicleConstraint::OnStep with the world's callbacks (its wheels' surfaces and rolling drag after the wheel tests,
		// its tyre grip in the friction combine function).
		void OnStep(JPH::PhysicsStepListenerContext const& context) override;
		// After Jolt's state was restored (PhysicsScene::RestoreState): each vehicle's gear back within its gears and its
		// engine's speed within its range, as SetVehicleSettings keeps them.
		void KeepWithinDefinitions();

		// After the system's own state, the characters and the contacts (PhysicsScene::SaveState): the number of vehicles, then
		// each vehicle's entity, latest and applied inputs, pending shift, gearbox mode and Manual gearbox, lean spring
		// integral and its wheels' steering angles, slips and surfaces, which Jolt does not save, in the order of the entities'
		// UUIDs. RestoreState reads what SaveState wrote for the same vehicles, all of it before any applies, and returns false,
		// changing nothing, when the stream fails or names other vehicles; a gearbox whose mode was edited in place since is
		// handed over as SetVehicleSettings hands it.
		void SaveState(JPH::StateRecorder& recorder) const;
		[[nodiscard]] bool RestoreState(JPH::StateRecorder& recorder);
		// Adds the vehicles' state to the world's hash in the order Docs/Architecture.md §8 ("Vehicles", "The state in the
		// hash") states: nothing at all when there are none.
		void AddToHash(HashBuilder& hash) const;

	private:
		[[nodiscard]] Result<void> MakeVehicle(VehicleDesc const& desc, VehicleCarriedState const* state, JPH::BodyID body,
		                                       JPH::ObjectLayer objectLayer);
		void DestroyVehicle(VehicleRecord& record);
		// The gear (Jolt's and a Manual gearbox's) within the record's gears, its Manual timers within its times and its
		// engine's speed within its range.
		static void KeepWithinDefinition(VehicleRecord& record);

		JPH::PhysicsSystem& m_System;
		std::vector<ColliderMaterial> const& m_Materials;
		// Ordered by entity: the listener steps them, and the world adds their constraints, in this order.
		std::map<UUID, VehicleRecord> m_Vehicles;
		bool m_IsListening = false;
	};
}

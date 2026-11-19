#include "stpch.h"
#include "Strada/Physics/VehicleWorld.h"

#include "Strada/Core/Assert.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Math/Curve.h"
#include "Strada/Physics/JoltContext.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorder.h>
#include <Jolt/Physics/Vehicle/MotorcycleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Strada
{
	class StradaMotorcycleController final : public JPH::MotorcycleController
	{
	public:
		StradaMotorcycleController(JPH::MotorcycleControllerSettings const& settings, JPH::VehicleConstraint& constraint)
			: JPH::MotorcycleController(settings, constraint)
		{
		}

		JPH::Vec3 GetTargetLean() const { return mTargetLean; }
		// The length of the controller's last collision step (0 before its first).
		float GetPreviousDeltaTime() const { return mPreviousDeltaTime; }
		float GetLeanSpringIntegral() const { return mLeanSpringIntegratedDeltaAngle; }
		void SetLeanSpringIntegral(float integral) { mLeanSpringIntegratedDeltaAngle = integral; }
		void SetMaxLeanAngle(float angle) { mMaxLeanAngle = angle; }
	};

	class VehicleWheelTester final : public JPH::VehicleCollisionTesterCastCylinder
	{
	public:
		VehicleWheelTester(JPH::ObjectLayer objectLayer, std::vector<ColliderMaterial> const& materials)
			: JPH::VehicleCollisionTesterCastCylinder(objectLayer, ConvexRadiusFraction),
			  m_Materials(materials)
		{
		}

		// Jolt's VehicleCollisionTesterCastCylinder::Collide (VehicleCollisionTester.cpp:241-329 of the pinned Jolt), with the
		// shape filter that refuses trigger colliders. Runs on the thread that steps the vehicles, with every body locked.
		bool Collide(JPH::PhysicsSystem& system, JPH::VehicleConstraint const& vehicle, JPH::uint wheelIndex, JPH::RVec3Arg origin,
		             JPH::Vec3Arg direction, JPH::BodyID const& vehicleBody, JPH::Body*& outBody, JPH::SubShapeID& outSubShapeID,
		             JPH::RVec3& outContactPosition, JPH::Vec3& outContactNormal, float& outSuspensionLength) const override;

	private:
		// Half the wheel's width (or its radius when smaller) rounds the cylinder, as Jolt's motorcycle sample sets it
		// (MotorcycleTest.cpp:129): the tyre rolls over edges.
		static constexpr float ConvexRadiusFraction = 1.0f;

		std::vector<ColliderMaterial> const& m_Materials;
	};

	namespace
	{
		// The settings whose controller is Strada's subclass. Jolt's VehicleConstraint takes ownership of the controller it
		// constructs and deletes it (VehicleConstraint.cpp:95, :109), so this is the one place a naked new is required.
		class StradaMotorcycleControllerSettings final : public JPH::MotorcycleControllerSettings
		{
		public:
			JPH::VehicleController* ConstructController(JPH::VehicleConstraint& constraint) const override
			{
				return new StradaMotorcycleController(*this, constraint);
			}
		};

		// The closest solid hit of a wheel's cast, skipping sensor bodies as Jolt's cylinder tester does.
		class WheelHitCollector final : public JPH::CastShapeCollector
		{
		public:
			WheelHitCollector(JPH::PhysicsSystem const& system, JPH::RShapeCast const& cast)
				: m_System(system),
				  m_Cast(cast)
			{
			}

			void AddHit(JPH::ShapeCastResult const& result) override
			{
				float const earlyOut = result.GetEarlyOutFraction();
				if (earlyOut >= GetEarlyOutFraction())
				{
					return;
				}
				// Every body is locked while the step runs: the body is read without a lock of its own.
				JPH::Body const* body = m_System.GetBodyLockInterfaceNoLock().TryGetBody(result.mBodyID2);
				if (body == nullptr || body->IsSensor())
				{
					return;
				}
				UpdateEarlyOutFraction(earlyOut);
				Body = body;
				SubShape = result.mSubShapeID2;
				ContactPosition = m_Cast.mCenterOfMassStart.GetTranslation() + result.mContactPointOn2;
				ContactNormal = -result.mPenetrationAxis.Normalized();
				Fraction = result.mFraction;
			}

			JPH::Body const* Body = nullptr;
			JPH::SubShapeID SubShape;
			JPH::RVec3 ContactPosition = JPH::RVec3::sZero();
			JPH::Vec3 ContactNormal = JPH::Vec3::sZero();
			float Fraction = 0.0f;

		private:
			JPH::PhysicsSystem const& m_System;
			JPH::RShapeCast const& m_Cast;
		};

		// The friction combine function: the tyre's curve values times the grip of the surface the wheel found in this
		// collision step (VehicleWheelSurface::TyreGrip, written by WheelSurfaceUpdate on the same thread before Jolt's
		// PostCollide calls this). It replaces Jolt's default, the geometric mean with the body's friction, so a surface's
		// Friction never reaches tyres. Jolt copies it into a std::function for each wheel at every collision step
		// (WheeledVehicleController.cpp:130): one pointer, trivially copyable, so no standard library allocates for it.
		struct TyreGripCombine
		{
			VehicleRecord const* Record = nullptr;

			void operator()(JPH::uint wheel, float& longitudinal, float& lateral, JPH::Body const&, JPH::SubShapeID const&) const
			{
				float const grip = wheel < VehicleWheelCount ? Record->Surfaces[wheel].TyreGrip : 1.0f;
				longitudinal *= grip;
				lateral *= grip;
			}
		};
		static_assert(std::is_trivially_copyable_v<TyreGripCombine> && sizeof(TyreGripCombine) <= 2 * sizeof(void*),
		              "The friction combine function must fit every standard library's small-object buffer of std::function");

		// The post-collide callback (VehicleConstraint.cpp:276, after the wheel tests): each wheel's surface and its rolling
		// drag. Reads the bodies the wheels touch (locked by the step) and the collider table (fixed for the step); writes the
		// vehicle's own wheels and record. Never logs or allocates.
		struct WheelSurfaceUpdate
		{
			VehicleRecord* Record = nullptr;

			void operator()(JPH::VehicleConstraint& vehicle, JPH::PhysicsStepListenerContext const& context) const
			{
				JPH::BodyLockInterfaceNoLock const& bodies = context.mPhysicsSystem->GetBodyLockInterfaceNoLock();
				// The suspension's impulse is the last collision step's: Jolt's own scale for impulses of the step before
				// (WheeledVehicleController.cpp:503), 0 before the first.
				float const previous = Record->Controller->GetPreviousDeltaTime();
				float const scale = previous > 0.0f ? context.mDeltaTime / previous : 0.0f;
				for (size_t index = 0; index < VehicleWheelCount; index++)
				{
					JPH::Wheel* wheel = vehicle.GetWheel(static_cast<JPH::uint>(index));
					VehicleWheelSurface& surface = Record->Surfaces[index];
					surface = VehicleWheelSurface();
					if (!wheel->HasContact())
					{
						continue;
					}
					JPH::Body const* body = bodies.TryGetBody(wheel->GetContactBodyID());
					if (body == nullptr)
					{
						continue;
					}
					surface.ContactEntity = UUID(body->GetUserData());
					SubShapeMaterial const material =
						FindSubShapeMaterial(*Record->Materials, *body->GetShape(), wheel->GetContactSubShapeID());
					if (material.Surface != nullptr)
					{
						surface.Material = material.Surface->Material;
						surface.TyreGrip = material.Surface->TyreGrip;
						surface.RollingResistance = material.Surface->RollingResistance;
					}
					if (surface.RollingResistance <= 0.0f)
					{
						continue;
					}
					// The torque RollingResistance x load x radius against the spin over this collision step, the load being
					// the suspension's impulse over the last one; never past a stop. The tyre then passes the deficit to the
					// chassis.
					JPH::WheelSettingsWV const& settings = *Record->WheelSettings[index];
					float const loss = surface.RollingResistance * std::max(wheel->GetSuspensionLambda(), 0.0f) * scale * settings.mRadius /
					                   settings.mInertia;
					float const spin = wheel->GetAngularVelocity();
					wheel->SetAngularVelocity(spin > 0.0f ? std::max(spin - loss, 0.0f) : std::min(spin + loss, 0.0f));
				}
			}
		};

		// A Manual gearbox over one collision step, before Jolt's OnStep reads it: the first collision step of a step applies
		// the pending shift requests (the clutch held open in that collision step, as Jolt's automatic gearbox holds it in
		// the update that shifts, VehicleTransmission.cpp:100-107), and every other one counts the switch time down with the
		// clutch open, then the release time with the clutch closing (:108-123).
		void UpdateManualGearbox(VehicleRecord& record, JPH::PhysicsStepListenerContext const& context)
		{
			if (record.Desc.TransmissionMode != VehicleTransmissionMode::Manual)
			{
				return;
			}
			VehicleShiftState& shift = record.Shift;
			bool shifted = false;
			if (context.mIsFirstStep && record.PendingShift != 0)
			{
				int32_t const gears = static_cast<int32_t>(record.Desc.GearRatios.size());
				int32_t const target = std::clamp(shift.Gear + record.PendingShift, 0, gears);
				record.PendingShift = 0;
				if (target != shift.Gear)
				{
					shift.SwitchTimeLeft = shift.Gear != 0 ? record.Desc.SwitchTime : 0.0f;
					shift.ClutchReleaseTimeLeft = record.Desc.ClutchReleaseTime;
					shift.Gear = target;
					shifted = true;
				}
			}
			float clutch = 1.0f;
			if (shifted)
			{
				clutch = 0.0f;
			}
			else if (shift.SwitchTimeLeft > 0.0f)
			{
				shift.SwitchTimeLeft = std::max(0.0f, shift.SwitchTimeLeft - context.mDeltaTime);
				clutch = 0.0f;
			}
			else if (shift.ClutchReleaseTimeLeft > 0.0f)
			{
				shift.ClutchReleaseTimeLeft = std::max(0.0f, shift.ClutchReleaseTimeLeft - context.mDeltaTime);
				float const release = record.Desc.ClutchReleaseTime;
				clutch = release > 0.0f ? std::clamp(1.0f - shift.ClutchReleaseTimeLeft / release, 0.0f, 1.0f) : 1.0f;
			}
			record.Controller->GetTransmission().Set(shift.Gear, clutch);
		}

		bool IsFinite(glm::vec3 const& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// A vector of length 1 in its direction, or empty for zero and values that are not finite (computed without
		// overflowing).
		std::optional<glm::vec3> Normalize(glm::vec3 const& value)
		{
			if (!IsFinite(value))
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

		// The checks of PrepareDesc, each naming its field.
		class DescChecker
		{
		public:
			void Range(std::string_view field, float value, float minimum, float maximum)
			{
				if (m_Error.empty() && !(value >= minimum && value <= maximum))
				{
					m_Error = std::string(field) + ": " + std::to_string(value) + " is not within " + std::to_string(minimum) + " to " +
					          std::to_string(maximum);
				}
			}

			void Fail(std::string error)
			{
				if (m_Error.empty())
				{
					m_Error = std::move(error);
				}
			}

			// Finite points, x not decreasing, at most MaxVehicleCurvePoints.
			void Curve(std::string_view field, VehicleCurve const& curve)
			{
				if (curve.size() > MaxVehicleCurvePoints)
				{
					Fail(std::string(field) + ": more than " + std::to_string(MaxVehicleCurvePoints) + " points");
					return;
				}
				for (size_t index = 0; index < curve.size(); index++)
				{
					glm::vec2 const point = curve[index];
					if (!std::isfinite(point.x) || !std::isfinite(point.y) || (index > 0 && point.x < curve[index - 1].x))
					{
						Fail(std::string(field) + ": its points are not finite with x in order");
						return;
					}
				}
			}

			// A direction that is not zero, made of length 1.
			void Direction(std::string_view field, glm::vec3& direction)
			{
				std::optional<glm::vec3> const unit = Normalize(direction);
				if (!unit)
				{
					Fail(std::string(field) + ": the direction is zero or not finite");
					return;
				}
				direction = *unit;
			}

			std::string const& GetError() const { return m_Error; }

		private:
			std::string m_Error;
		};

		// An angle within [0, limit] radians, with the rounding of a degree limit converted (glm::radians(90) may lie an ulp
		// above pi / 2, which Jolt asserts against) taken back to the limit.
		void CheckAngle(DescChecker& checker, std::string_view field, float& angle, float limit)
		{
			checker.Range(field, angle, 0.0f, limit * (1.0f + 1e-6f));
			angle = std::min(angle, limit);
		}

		JPH::LinearCurve MakeCurve(VehicleCurve const& points, JPH::LinearCurve const& fallback)
		{
			if (points.empty())
			{
				return fallback;
			}
			JPH::LinearCurve curve;
			curve.Reserve(static_cast<JPH::uint>(points.size()));
			for (glm::vec2 const& point : points)
			{
				curve.AddPoint(point.x, point.y);
			}
			return curve;
		}

		// Jolt's wheel from a prepared wheel desc: the record's own object, which Jolt reads at every step, so an edit changes
		// it in place.
		void ApplyWheel(JPH::WheelSettingsWV& settings, VehicleWheelDesc const& desc, JPH::Vec3Arg up, JPH::Vec3Arg forward)
		{
			JPH::WheelSettingsWV const defaults;
			settings.mPosition = Jolt::ToJolt(desc.Position);
			settings.mSuspensionDirection = Jolt::ToJolt(desc.SuspensionDirection);
			settings.mSteeringAxis = Jolt::ToJolt(desc.SteeringAxis);
			settings.mWheelUp = up;
			settings.mWheelForward = forward;
			settings.mSuspensionMinLength = desc.SuspensionMinLength;
			settings.mSuspensionMaxLength = desc.SuspensionMaxLength;
			settings.mSuspensionPreloadLength = desc.SuspensionPreload;
			settings.mSuspensionSpring.mMode = JPH::ESpringMode::FrequencyAndDamping;
			settings.mSuspensionSpring.mFrequency = desc.SuspensionFrequency;
			settings.mSuspensionSpring.mDamping = desc.SuspensionDamping;
			settings.mRadius = desc.Radius;
			settings.mWidth = desc.Width;
			settings.mInertia = desc.Inertia;
			settings.mAngularDamping = desc.AngularDamping;
			settings.mMaxSteerAngle = desc.MaxSteerAngle;
			// The front brake is Jolt's brake input, the rear brake its hand-brake input (WheeledVehicleController.cpp:537,
			// :629): each wheel takes the torque of each.
			settings.mMaxBrakeTorque = desc.FrontBrakeTorque;
			settings.mMaxHandBrakeTorque = desc.RearBrakeTorque;
			settings.mLongitudinalFriction = MakeCurve(desc.LongitudinalFriction, defaults.mLongitudinalFriction);
			settings.mLateralFriction = MakeCurve(desc.LateralFriction, defaults.mLateralFriction);
		}

		JPH::VehicleEngineSettings MakeEngine(VehicleDesc const& desc)
		{
			JPH::VehicleEngineSettings engine;
			engine.mMaxTorque = desc.MaxTorque;
			engine.mMinRPM = desc.MinRPM;
			engine.mMaxRPM = desc.MaxRPM;
			engine.mNormalizedTorque = MakeCurve(desc.NormalizedTorque, JPH::VehicleEngineSettings().mNormalizedTorque);
			engine.mInertia = desc.EngineInertia;
			engine.mAngularDamping = desc.EngineAngularDamping;
			return engine;
		}

		JPH::VehicleTransmissionSettings MakeTransmission(VehicleDesc const& desc)
		{
			JPH::VehicleTransmissionSettings transmission;
			transmission.mMode =
				desc.TransmissionMode == VehicleTransmissionMode::Manual ? JPH::ETransmissionMode::Manual : JPH::ETransmissionMode::Auto;
			transmission.mGearRatios.assign(desc.GearRatios.begin(), desc.GearRatios.end());
			// No reverse gear: the throttle is never negative, so the automatic gearbox never asks for one
			// (VehicleTransmission.cpp:58-62).
			transmission.mReverseGearRatios.clear();
			transmission.mSwitchTime = desc.SwitchTime;
			transmission.mClutchReleaseTime = desc.ClutchReleaseTime;
			transmission.mSwitchLatency = desc.SwitchLatency;
			transmission.mShiftUpRPM = desc.ShiftUpRPM;
			transmission.mShiftDownRPM = desc.ShiftDownRPM;
			transmission.mClutchStrength = desc.ClutchStrength;
			return transmission;
		}

		glm::mat4 ToGlm(JPH::Mat44 const& matrix)
		{
			glm::mat4 result(1.0f);
			for (int column = 0; column < 4; column++)
			{
				JPH::Vec4 const values = matrix.GetColumn4(column);
				result[column] = glm::vec4(values.GetX(), values.GetY(), values.GetZ(), values.GetW());
			}
			return result;
		}

		// The angle from `reference` to `direction` around `axis` (all of length 1), positive towards axis x reference: the
		// lean of a chassis's up from the world's up around its front, positive to its right. Both are taken square to the
		// axis first; 0 when either has no part square to it.
		float SignedAngleAround(glm::vec3 const& reference, glm::vec3 const& direction, glm::vec3 const& axis)
		{
			glm::vec3 const from = reference - axis * glm::dot(reference, axis);
			glm::vec3 const to = direction - axis * glm::dot(direction, axis);
			glm::vec3 const side = glm::cross(axis, from);
			float const along = glm::dot(to, from);
			float const across = glm::dot(to, side);
			return along == 0.0f && across == 0.0f ? 0.0f : std::atan2(across, along);
		}

		float ClampUnit(float value)
		{
			return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
		}

		// A state recorder over a buffer of its own, for the few bytes a part of Jolt's state takes (the gearbox's, which
		// the hash reads as Jolt saves it, since its members are private): no allocation. Writing past its end fails it.
		class SmallStateRecorder final : public JPH::StateRecorder
		{
		public:
			void WriteBytes(void const* data, size_t count) override
			{
				if (m_Failed || count > m_Bytes.size() - m_Size)
				{
					m_Failed = true;
					return;
				}
				std::copy_n(static_cast<uint8_t const*>(data), count, m_Bytes.begin() + static_cast<std::ptrdiff_t>(m_Size));
				m_Size += count;
			}

			void ReadBytes(void*, size_t) override { m_Failed = true; }
			bool IsEOF() const override { return true; }
			bool IsFailed() const override { return m_Failed; }

			std::span<uint8_t const> GetBytes() const { return std::span<uint8_t const>(m_Bytes.data(), m_Size); }

		private:
			// VehicleTransmission::SaveState writes 20 (VehicleTransmission.cpp:141-148 of the pinned Jolt).
			std::array<uint8_t, 64> m_Bytes{};
			size_t m_Size = 0;
			bool m_Failed = false;
		};

		// A state recorder is a stream both ways, each with its own IsFailed (the same state in Jolt's recorders).
		bool HasFailed(JPH::StateRecorder const& recorder)
		{
			return static_cast<JPH::StreamIn const&>(recorder).IsFailed();
		}

		void WriteInputs(JPH::StateRecorder& recorder, VehicleInputs const& inputs)
		{
			recorder.Write(inputs.Throttle);
			recorder.Write(inputs.FrontBrake);
			recorder.Write(inputs.RearBrake);
			recorder.Write(inputs.Lean);
		}

		void ReadInputs(JPH::StateRecorder& recorder, VehicleInputs& inputs)
		{
			recorder.Read(inputs.Throttle);
			recorder.Read(inputs.FrontBrake);
			recorder.Read(inputs.RearBrake);
			recorder.Read(inputs.Lean);
		}

		// What Strada's part of a snapshot holds for one vehicle (VehicleWorld::SaveState), read in full before any of it
		// applies.
		struct SavedVehicle
		{
			uint64_t Entity = 0;
			VehicleInputs Inputs;
			VehicleInputs AppliedInputs;
			int32_t PendingShift = 0;
			uint8_t Mode = 0;
			VehicleShiftState Shift;
			float LeanSpringIntegral = 0.0f;
			std::array<float, VehicleWheelCount> SteerAngles{};
			std::array<float, VehicleWheelCount> LongitudinalSlips{};
			std::array<float, VehicleWheelCount> LateralSlips{};
			std::array<uint64_t, VehicleWheelCount> ContactEntities{};
			std::array<uint64_t, VehicleWheelCount> Materials{};
			std::array<float, VehicleWheelCount> TyreGrips{};
			std::array<float, VehicleWheelCount> RollingResistances{};
		};
	}

	bool VehicleWheelTester::Collide(JPH::PhysicsSystem& system, JPH::VehicleConstraint const& vehicle, JPH::uint wheelIndex,
	                                 JPH::RVec3Arg origin, JPH::Vec3Arg direction, JPH::BodyID const& vehicleBody, JPH::Body*& outBody,
	                                 JPH::SubShapeID& outSubShapeID, JPH::RVec3& outContactPosition, JPH::Vec3& outContactNormal,
	                                 float& outSuspensionLength) const
	{
		JPH::DefaultBroadPhaseLayerFilter const broadPhaseFilter = system.GetDefaultBroadPhaseLayerFilter(mObjectLayer);
		JPH::DefaultObjectLayerFilter const layerFilter = system.GetDefaultLayerFilter(mObjectLayer);
		JPH::IgnoreSingleBodyFilter const bodyFilter(vehicleBody);
		SolidShapeFilter const shapeFilter(m_Materials);

		JPH::WheelSettings const* settings = vehicle.GetWheel(wheelIndex)->GetSettings();
		float const maxSuspensionLength = settings->mSuspensionMaxLength;
		// The wheel's transform for a cylinder along Y, at the suspension's attachment.
		JPH::RMat44 start = vehicle.GetWheelWorldTransform(wheelIndex, JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX());
		start.SetTranslation(origin);
		float const halfWidth = 0.5f * settings->mWidth;
		JPH::CylinderShape cylinder(halfWidth, settings->mRadius, std::min(halfWidth, settings->mRadius) * ConvexRadiusFraction);
		cylinder.SetEmbedded();
		JPH::RShapeCast const cast(&cylinder, JPH::Vec3::sOne(), start, direction * maxSuspensionLength);
		JPH::ShapeCastSettings castSettings;
		castSettings.mUseShrunkenShapeAndConvexRadius = true;
		castSettings.mReturnDeepestPoint = true;

		WheelHitCollector collector(system, cast);
		system.GetNarrowPhaseQueryNoLock().CastShape(cast, castSettings, cast.mCenterOfMassStart.GetTranslation(), collector,
		                                             broadPhaseFilter, layerFilter, bodyFilter, shapeFilter);
		if (collector.Body == nullptr)
		{
			return false;
		}
		outBody = const_cast<JPH::Body*>(collector.Body);
		outSubShapeID = collector.SubShape;
		outContactPosition = collector.ContactPosition;
		outContactNormal = collector.ContactNormal;
		outSuspensionLength = maxSuspensionLength * collector.Fraction;
		return true;
	}

	bool NormalizeVehicleAxes(glm::vec3& up, glm::vec3& forward)
	{
		std::optional<glm::vec3> const unitUp = Normalize(up);
		std::optional<glm::vec3> const unitForward = Normalize(forward);
		if (!unitUp || !unitForward)
		{
			return false;
		}
		// Square to the up; parallel (within a thousandth of a radian) is refused.
		std::optional<glm::vec3> const square = Normalize(*unitForward - *unitUp * glm::dot(*unitForward, *unitUp));
		if (!square || glm::length(glm::cross(*unitForward, *unitUp)) < 1e-3f)
		{
			return false;
		}
		up = *unitUp;
		forward = *square;
		return true;
	}

	glm::mat4 ComposeVehicleWheelTransform(VehicleWheelDesc const& wheel, glm::vec3 up, glm::vec3 forward, float suspensionLength,
	                                       float steerAngle, float rotationAngle)
	{
		// VehicleConstraint::GetWheelLocalBasis: the wheel's up and forward turned by the steering around its axis, made
		// square.
		glm::quat const steer = glm::angleAxis(steerAngle, wheel.SteeringAxis);
		glm::vec3 const wheelUp = steer * up;
		glm::vec3 const wheelForward = steer * forward;
		glm::vec3 const right = glm::normalize(glm::cross(wheelForward, wheelUp));
		glm::vec3 const front = glm::normalize(glm::cross(wheelUp, right));
		glm::vec3 const position = wheel.Position + wheel.SuspensionDirection * suspensionLength;
		glm::mat4 const rotationalToLocal(glm::vec4(right, 0.0f), glm::vec4(wheelUp, 0.0f), glm::vec4(front, 0.0f),
		                                  glm::vec4(position, 1.0f));
		// The model's X is the axle (right) and its Y its up, so its Z is up x right = -forward.
		glm::mat4 const modelToRotational(glm::vec4(1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
		                                  glm::vec4(0.0f, 0.0f, -1.0f, 0.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		return rotationalToLocal * glm::rotate(glm::mat4(1.0f), rotationAngle, glm::vec3(1.0f, 0.0f, 0.0f)) * modelToRotational;
	}

	VehicleDesc MakeVehicleDesc(UUID entity, Vehicle const& definition)
	{
		VehicleDesc desc;
		desc.Entity = entity;
		for (size_t index = 0; index < std::min(definition.Wheels.size(), VehicleWheelCount); index++)
		{
			VehicleWheelDefinition const& source = definition.Wheels[index];
			VehicleWheelDesc& wheel = desc.Wheels[index];
			wheel.Position = source.Position;
			wheel.SuspensionDirection = source.SuspensionDirection;
			wheel.SteeringAxis = source.SteeringAxis;
			wheel.SuspensionMinLength = source.SuspensionMinLength;
			wheel.SuspensionMaxLength = source.SuspensionMaxLength;
			wheel.SuspensionPreload = source.SuspensionPreload;
			wheel.SuspensionFrequency = source.SuspensionFrequency;
			wheel.SuspensionDamping = source.SuspensionDamping;
			wheel.MaxSteerAngle = glm::radians(source.MaxSteerAngle);
			wheel.Radius = source.Radius;
			wheel.Width = source.Width;
			wheel.Inertia = source.Inertia;
			wheel.AngularDamping = source.AngularDamping;
			wheel.FrontBrakeTorque = source.FrontBrakeTorque;
			wheel.RearBrakeTorque = source.RearBrakeTorque;
		}
		desc.Up = definition.Up;
		desc.Forward = definition.Forward;
		desc.MaxPitchRoll = glm::radians(definition.MaxPitchRoll);
		desc.MaxTorque = definition.Engine.MaxTorque;
		desc.MinRPM = definition.Engine.MinRPM;
		desc.MaxRPM = definition.Engine.MaxRPM;
		desc.EngineInertia = definition.Engine.Inertia;
		desc.EngineAngularDamping = definition.Engine.AngularDamping;
		desc.TransmissionMode = definition.Transmission.Mode;
		desc.GearRatios = definition.Transmission.GearRatios;
		desc.FinalDrive = definition.Transmission.FinalDrive;
		desc.SwitchTime = definition.Transmission.SwitchTime;
		desc.ClutchReleaseTime = definition.Transmission.ClutchReleaseTime;
		desc.SwitchLatency = definition.Transmission.SwitchLatency;
		desc.ShiftUpRPM = definition.Transmission.ShiftUpRPM;
		desc.ShiftDownRPM = definition.Transmission.ShiftDownRPM;
		desc.ClutchStrength = definition.Transmission.ClutchStrength;
		desc.MaxLeanAngle = glm::radians(definition.Lean.MaxLeanAngle);
		desc.LeanSpringConstant = definition.Lean.SpringConstant;
		desc.LeanSpringDamping = definition.Lean.SpringDamping;
		desc.LeanSmoothingFactor = definition.Lean.SmoothingFactor;
		desc.LeanSteeringLimit = definition.Lean.SteeringLimit;
		return desc;
	}

	VehicleCurve SampleVehicleCurve(Curve const& curve, float inputScale)
	{
		// Points between two keys of different times: a cubic segment is followed within a small part of its bend, and a
		// curve of MaxCurveKeys keys stays below MaxVehicleCurvePoints.
		constexpr int PointsBetweenKeys = 15;
		auto const point = [inputScale](double time, float value)
		{
			float const kept = std::isfinite(value) ? std::clamp(value, 0.0f, MaxVehicleCurveValue) : 0.0f;
			return glm::vec2(static_cast<float>(time * inputScale), kept);
		};
		VehicleCurve points;
		if (curve.Keys.empty())
		{
			points.push_back(point(0.0, curve.Evaluate(0.0)));
			return points;
		}
		for (size_t index = 0; index < curve.Keys.size() && points.size() < MaxVehicleCurvePoints; index++)
		{
			CurveKey const& key = curve.Keys[index];
			points.push_back(point(key.Time, key.Value));
			if (index + 1 == curve.Keys.size())
			{
				break;
			}
			double const start = key.Time;
			double const end = curve.Keys[index + 1].Time;
			if (!(end > start))
			{
				continue;
			}
			for (int between = 1; between <= PointsBetweenKeys && points.size() < MaxVehicleCurvePoints; between++)
			{
				double const time = start + (end - start) * between / (PointsBetweenKeys + 1);
				points.push_back(point(time, curve.Evaluate(time)));
			}
		}
		return points;
	}

	VehicleWorld::VehicleWorld(JPH::PhysicsSystem& system, std::vector<ColliderMaterial> const& materials)
		: m_System(system),
		  m_Materials(materials)
	{
	}

	VehicleWorld::~VehicleWorld()
	{
		if (m_IsListening)
		{
			m_System.RemoveStepListener(this);
		}
		for (auto& [entity, record] : m_Vehicles)
		{
			m_System.RemoveConstraint(record.Constraint);
		}
	}

	Result<VehicleDesc> VehicleWorld::PrepareDesc(VehicleDesc const& source)
	{
		// The ranges are the Vehicle definition's (Strada/Asset/VehicleAsset.h), in radians where it states degrees; what
		// Jolt asserts on is within them (Wheel.cpp:70-80, WheeledVehicleController.cpp:87-91 and :183-214).
		VehicleDesc desc = source;
		DescChecker checker;
		float const halfPi = glm::half_pi<float>();
		if (!NormalizeVehicleAxes(desc.Up, desc.Forward))
		{
			checker.Fail("Up, Forward: the directions are zero, not finite or parallel");
		}
		for (size_t index = 0; index < VehicleWheelCount; index++)
		{
			VehicleWheelDesc& wheel = desc.Wheels[index];
			std::string const name = "Wheels[" + std::to_string(index) + "].";
			for (int axis = 0; axis < 3; axis++)
			{
				checker.Range(name + "Position", wheel.Position[axis], -1.0e3f, 1.0e3f);
			}
			checker.Direction(name + "SuspensionDirection", wheel.SuspensionDirection);
			checker.Direction(name + "SteeringAxis", wheel.SteeringAxis);
			checker.Range(name + "SuspensionMinLength", wheel.SuspensionMinLength, 0.0f, 100.0f);
			checker.Range(name + "SuspensionMaxLength", wheel.SuspensionMaxLength, wheel.SuspensionMinLength, 100.0f);
			checker.Range(name + "SuspensionPreload", wheel.SuspensionPreload, 0.0f, 100.0f);
			checker.Range(name + "SuspensionFrequency", wheel.SuspensionFrequency, 0.01f, 100.0f);
			checker.Range(name + "SuspensionDamping", wheel.SuspensionDamping, 0.0f, 100.0f);
			CheckAngle(checker, name + "MaxSteerAngle", wheel.MaxSteerAngle, halfPi);
			checker.Range(name + "Radius", wheel.Radius, 0.01f, 10.0f);
			checker.Range(name + "Width", wheel.Width, 0.0f, 10.0f);
			checker.Range(name + "Inertia", wheel.Inertia, 0.001f, 1.0e3f);
			checker.Range(name + "AngularDamping", wheel.AngularDamping, 0.0f, 100.0f);
			checker.Range(name + "FrontBrakeTorque", wheel.FrontBrakeTorque, 0.0f, 1.0e5f);
			checker.Range(name + "RearBrakeTorque", wheel.RearBrakeTorque, 0.0f, 1.0e5f);
			checker.Curve(name + "LongitudinalFriction", wheel.LongitudinalFriction);
			checker.Curve(name + "LateralFriction", wheel.LateralFriction);
		}
		CheckAngle(checker, "MaxPitchRoll", desc.MaxPitchRoll, glm::pi<float>());
		checker.Range("MaxTorque", desc.MaxTorque, 0.01f, 1.0e5f);
		checker.Range("MinRPM", desc.MinRPM, 0.0f, 1.0e5f);
		checker.Range("MaxRPM", desc.MaxRPM, 1.0f, 1.0e5f);
		if (desc.MinRPM >= desc.MaxRPM)
		{
			checker.Fail("MinRPM: the engine's idle speed is not below its MaxRPM");
		}
		checker.Curve("NormalizedTorque", desc.NormalizedTorque);
		checker.Range("EngineInertia", desc.EngineInertia, 0.001f, 1.0e3f);
		checker.Range("EngineAngularDamping", desc.EngineAngularDamping, 0.0f, 100.0f);
		if (desc.TransmissionMode != VehicleTransmissionMode::Automatic && desc.TransmissionMode != VehicleTransmissionMode::Manual)
		{
			checker.Fail("TransmissionMode: not a mode");
		}
		if (desc.GearRatios.empty() || desc.GearRatios.size() > MaxVehicleGears)
		{
			checker.Fail("GearRatios: 1 to " + std::to_string(MaxVehicleGears) + " gears");
		}
		for (float const ratio : desc.GearRatios)
		{
			checker.Range("GearRatios", ratio, 0.01f, 100.0f);
		}
		checker.Range("FinalDrive", desc.FinalDrive, 0.01f, 1.0e3f);
		checker.Range("SwitchTime", desc.SwitchTime, 0.0f, 10.0f);
		checker.Range("ClutchReleaseTime", desc.ClutchReleaseTime, 0.0f, 10.0f);
		checker.Range("SwitchLatency", desc.SwitchLatency, 0.0f, 10.0f);
		checker.Range("ShiftUpRPM", desc.ShiftUpRPM, 1.0f, 1.0e5f);
		checker.Range("ShiftDownRPM", desc.ShiftDownRPM, 1.0f, 1.0e5f);
		if (desc.ShiftDownRPM >= desc.ShiftUpRPM)
		{
			checker.Fail("ShiftDownRPM: not below ShiftUpRPM");
		}
		if (desc.TransmissionMode == VehicleTransmissionMode::Automatic && desc.ShiftUpRPM >= desc.MaxRPM)
		{
			checker.Fail("ShiftUpRPM: an automatic gearbox shifts up below the engine's MaxRPM");
		}
		checker.Range("ClutchStrength", desc.ClutchStrength, 0.001f, 1.0e4f);
		CheckAngle(checker, "MaxLeanAngle", desc.MaxLeanAngle, halfPi);
		checker.Range("LeanSpringConstant", desc.LeanSpringConstant, 0.0f, 1.0e6f);
		checker.Range("LeanSpringDamping", desc.LeanSpringDamping, 0.0f, 1.0e6f);
		checker.Range("LeanSmoothingFactor", desc.LeanSmoothingFactor, 0.0f, 1.0f);
		if (!checker.GetError().empty())
		{
			return Error{checker.GetError()};
		}
		return desc;
	}

	Result<void> VehicleWorld::AddVehicle(VehicleDesc const& desc, JPH::BodyID body, JPH::ObjectLayer objectLayer)
	{
		VehicleDesc replaced;
		std::optional<VehicleCarriedState> const carried = TakeVehicle(desc.Entity, replaced);
		return MakeVehicle(desc, carried ? &*carried : nullptr, body, objectLayer);
	}

	Result<void> VehicleWorld::MakeVehicle(VehicleDesc const& desc, VehicleCarriedState const* state, JPH::BodyID body,
	                                       JPH::ObjectLayer objectLayer)
	{
		JPH::Vec3 const up = Jolt::ToJolt(desc.Up);
		JPH::Vec3 const forward = Jolt::ToJolt(desc.Forward);
		VehicleRecord record;
		record.Entity = desc.Entity;
		record.Desc = desc;
		record.Body = body;
		record.Materials = &m_Materials;

		JPH::VehicleConstraintSettings settings;
		settings.mUp = up;
		settings.mForward = forward;
		settings.mMaxPitchRollAngle = desc.MaxPitchRoll;
		for (size_t index = 0; index < VehicleWheelCount; index++)
		{
			record.WheelSettings[index] = new JPH::WheelSettingsWV();
			ApplyWheel(*record.WheelSettings[index], desc.Wheels[index], up, forward);
			settings.mWheels.push_back(record.WheelSettings[index].GetPtr());
		}
		JPH::Ref<StradaMotorcycleControllerSettings> controller = new StradaMotorcycleControllerSettings();
		controller->mEngine = MakeEngine(desc);
		controller->mTransmission = MakeTransmission(desc);
		// The final drive: the one differential Jolt needs to drive a wheel, with the rear wheel alone, which takes all of
		// its torque (WheeledVehicleController.cpp:404-408).
		JPH::VehicleDifferentialSettings finalDrive;
		finalDrive.mLeftWheel = -1;
		finalDrive.mRightWheel = 1;
		finalDrive.mDifferentialRatio = desc.FinalDrive;
		controller->mDifferentials.push_back(finalDrive);
		controller->mMaxLeanAngle = desc.MaxLeanAngle;
		controller->mLeanSpringConstant = desc.LeanSpringConstant;
		controller->mLeanSpringDamping = desc.LeanSpringDamping;
		// Kept at Jolt's 0 (Docs/Architecture.md §8, "Snapshots"): the definition does not offer it.
		controller->mLeanSpringIntegrationCoefficient = 0.0f;
		controller->mLeanSmoothingFactor = desc.LeanSmoothingFactor;
		settings.mController = controller;
		{
			JPH::BodyLockWrite const lock(m_System.GetBodyLockInterface(), body);
			if (!lock.Succeeded())
			{
				return MakeError("entity {} has no body for its vehicle", desc.Entity);
			}
			record.Constraint = new JPH::VehicleConstraint(lock.GetBody(), settings);
		}
		record.Controller = static_cast<StradaMotorcycleController*>(record.Constraint->GetController());
		record.Controller->EnableLeanSteeringLimit(desc.LeanSteeringLimit);
		record.Tester = new VehicleWheelTester(objectLayer, m_Materials);
		record.Constraint->SetVehicleCollisionTester(record.Tester);

		auto const inserted = m_Vehicles.insert_or_assign(desc.Entity, std::move(record)).first;
		VehicleRecord& stored = inserted->second;
		stored.Constraint->SetCombineFriction(TyreGripCombine{&stored});
		stored.Constraint->SetPostCollideCallback(WheelSurfaceUpdate{&stored});
		if (state != nullptr)
		{
			stored.Inputs = state->Inputs;
			stored.Controller->GetEngine().SetCurrentRPM(state->EngineSpeed);
			int32_t const gear = std::clamp(state->Gear, 0, static_cast<int32_t>(desc.GearRatios.size()));
			if (desc.TransmissionMode == VehicleTransmissionMode::Manual)
			{
				stored.Shift.Gear = gear;
			}
			stored.Controller->GetTransmission().Set(gear, 1.0f);
			for (size_t index = 0; index < VehicleWheelCount; index++)
			{
				JPH::Wheel* wheel = stored.Constraint->GetWheel(static_cast<JPH::uint>(index));
				wheel->SetAngularVelocity(state->WheelSpins[index]);
				wheel->SetRotationAngle(state->WheelAngles[index]);
			}
		}
		m_System.AddConstraint(stored.Constraint);
		return {};
	}

	void VehicleWorld::DestroyVehicle(VehicleRecord& record)
	{
		m_System.RemoveConstraint(record.Constraint);
	}

	void VehicleWorld::RemoveVehicle(UUID entity)
	{
		auto const it = m_Vehicles.find(entity);
		if (it == m_Vehicles.end())
		{
			return;
		}
		DestroyVehicle(it->second);
		m_Vehicles.erase(it);
	}

	bool VehicleWorld::HasVehicle(UUID entity) const
	{
		return m_Vehicles.contains(entity);
	}

	size_t VehicleWorld::GetVehicleCount() const
	{
		return m_Vehicles.size();
	}

	Result<void> VehicleWorld::SetVehicleSettings(UUID entity, VehicleDesc const& desc)
	{
		auto const it = m_Vehicles.find(entity);
		if (it == m_Vehicles.end())
		{
			return MakeError("entity {} has no vehicle", entity);
		}
		VehicleRecord& record = it->second;
		if (desc.Up != record.Desc.Up || desc.Forward != record.Desc.Forward)
		{
			return MakeError("the vehicle of entity {}: Up and Forward are taken when it is made; it must be made again", entity);
		}
		JPH::Vec3 const up = Jolt::ToJolt(desc.Up);
		JPH::Vec3 const forward = Jolt::ToJolt(desc.Forward);
		for (size_t index = 0; index < VehicleWheelCount; index++)
		{
			ApplyWheel(*record.WheelSettings[index], desc.Wheels[index], up, forward);
		}
		StradaMotorcycleController& controller = *record.Controller;
		// The settings parts of Jolt's engine and gearbox, which keep their running state (the engine's speed, the gear,
		// the clutch and the timers).
		static_cast<JPH::VehicleEngineSettings&>(controller.GetEngine()) = MakeEngine(desc);
		static_cast<JPH::VehicleTransmissionSettings&>(controller.GetTransmission()) = MakeTransmission(desc);
		controller.GetDifferentials().front().mDifferentialRatio = desc.FinalDrive;
		controller.SetMaxLeanAngle(desc.MaxLeanAngle);
		controller.SetLeanSpringConstant(desc.LeanSpringConstant);
		controller.SetLeanSpringDamping(desc.LeanSpringDamping);
		controller.SetLeanSmoothingFactor(desc.LeanSmoothingFactor);
		controller.EnableLeanSteeringLimit(desc.LeanSteeringLimit);
		record.Constraint->SetMaxPitchRollAngle(desc.MaxPitchRoll);
		if (desc.TransmissionMode != record.Desc.TransmissionMode)
		{
			// Manual takes the gear Jolt's gearbox is in; Automatic goes on from the gear Strada last gave it.
			record.Shift = VehicleShiftState();
			record.PendingShift = 0;
			if (desc.TransmissionMode == VehicleTransmissionMode::Manual)
			{
				record.Shift.Gear = std::max(controller.GetTransmission().GetCurrentGear(), 0);
			}
		}
		record.Desc = desc;
		KeepWithinDefinition(record);
		return {};
	}

	void VehicleWorld::SetVehicleInputs(UUID entity, VehicleInputs const& inputs)
	{
		auto const it = m_Vehicles.find(entity);
		if (it == m_Vehicles.end())
		{
			return;
		}
		VehicleInputs& latest = it->second.Inputs;
		latest.Throttle = ClampUnit(inputs.Throttle);
		latest.FrontBrake = ClampUnit(inputs.FrontBrake);
		latest.RearBrake = ClampUnit(inputs.RearBrake);
		latest.Lean = std::isfinite(inputs.Lean) ? std::clamp(inputs.Lean, -1.0f, 1.0f) : 0.0f;
	}

	void VehicleWorld::RequestVehicleShift(UUID entity, int32_t gears)
	{
		auto const it = m_Vehicles.find(entity);
		if (it == m_Vehicles.end() || it->second.Desc.TransmissionMode != VehicleTransmissionMode::Manual)
		{
			return;
		}
		// No sum moves the gear further than the gears there are, so the count stays within them (and never overflows).
		int32_t const limit = static_cast<int32_t>(MaxVehicleGears);
		int32_t& pending = it->second.PendingShift;
		pending = std::clamp(pending + std::clamp(gears, -limit, limit), -limit, limit);
	}

	void VehicleWorld::AppendVehiclesOnMaterial(AssetHandle material, std::vector<std::pair<UUID, JPH::BodyID>>& vehicles) const
	{
		if (!material.IsValid())
		{
			return;
		}
		for (auto const& [entity, record] : m_Vehicles)
		{
			bool const touches = std::any_of(record.Surfaces.begin(), record.Surfaces.end(),
			                                 [material](VehicleWheelSurface const& surface)
			                                 {
												 return surface.Material == material;
											 });
			if (touches)
			{
				vehicles.emplace_back(entity, record.Body);
			}
		}
	}

	std::optional<VehicleState> VehicleWorld::GetVehicleState(UUID entity) const
	{
		auto const it = m_Vehicles.find(entity);
		if (it == m_Vehicles.end())
		{
			return std::nullopt;
		}
		VehicleRecord const& record = it->second;
		StradaMotorcycleController const& controller = *record.Controller;
		JPH::VehicleTransmission const& transmission = controller.GetTransmission();
		VehicleState state;
		state.Entity = entity;
		state.Inputs = record.Inputs;
		state.AppliedInputs = record.AppliedInputs;
		state.EngineSpeed = controller.GetEngine().GetCurrentRPM();
		float const throttle = std::abs(controller.GetForwardInput()) *
		                       (transmission.mMode == JPH::ETransmissionMode::Auto ? transmission.GetClutchFriction() : 1.0f);
		state.EngineTorque = controller.GetEngine().GetTorque(throttle);
		state.Gear = transmission.GetCurrentGear();
		state.ClutchFriction = transmission.GetClutchFriction();
		state.IsShifting = record.Desc.TransmissionMode == VehicleTransmissionMode::Manual
		                       ? record.Shift.SwitchTimeLeft > 0.0f || record.Shift.ClutchReleaseTimeLeft > 0.0f
		                       : transmission.IsSwitchingGear() || transmission.GetClutchFriction() < 1.0f;
		{
			JPH::BodyLockRead const lock(m_System.GetBodyLockInterface(), record.Body);
			if (lock.Succeeded())
			{
				JPH::Body const& body = lock.GetBody();
				glm::quat const rotation = Jolt::ToGlm(body.GetRotation());
				glm::vec3 const forward = rotation * record.Desc.Forward;
				glm::vec3 const up = rotation * record.Desc.Up;
				std::optional<glm::vec3> const worldUp = Normalize(-Jolt::ToGlm(m_System.GetGravity()));
				glm::vec3 const reference = worldUp ? *worldUp : glm::vec3(0.0f, 1.0f, 0.0f);
				state.ForwardSpeed = glm::dot(Jolt::ToGlm(body.GetLinearVelocity()), forward);
				state.LeanAngle = SignedAngleAround(reference, up, forward);
				state.TargetLeanAngle = SignedAngleAround(reference, Jolt::ToGlm(controller.GetTargetLean()), forward);
			}
		}
		for (size_t index = 0; index < VehicleWheelCount; index++)
		{
			auto const* wheel = static_cast<JPH::WheelWV const*>(record.Constraint->GetWheel(static_cast<JPH::uint>(index)));
			VehicleWheelSurface const& surface = record.Surfaces[index];
			VehicleWheelState& reading = state.Wheels[index];
			reading.Surface = surface.Material;
			reading.TyreGrip = surface.TyreGrip;
			reading.RollingResistance = surface.RollingResistance;
			if (wheel->HasContact())
			{
				reading.ContactEntity = surface.ContactEntity;
				reading.ContactPoint = Jolt::ToGlm(JPH::Vec3(wheel->GetContactPosition()));
				reading.ContactNormal = Jolt::ToGlm(wheel->GetContactNormal());
			}
			reading.SuspensionLength = wheel->GetSuspensionLength();
			reading.AngularVelocity = wheel->GetAngularVelocity();
			reading.RotationAngle = wheel->GetRotationAngle();
			reading.SteerAngle = wheel->GetSteerAngle();
			reading.LongitudinalSlip = wheel->mLongitudinalSlip;
			reading.LateralSlip = wheel->mLateralSlip;
		}
		return state;
	}

	std::optional<glm::mat4> VehicleWorld::GetVehicleWheelTransform(UUID entity, size_t wheel) const
	{
		auto const it = m_Vehicles.find(entity);
		if (it == m_Vehicles.end() || wheel >= VehicleWheelCount)
		{
			return std::nullopt;
		}
		return ToGlm(
			it->second.Constraint->GetWheelLocalTransform(static_cast<JPH::uint>(wheel), JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY()));
	}

	std::optional<VehicleCarriedState> VehicleWorld::TakeVehicle(UUID entity, VehicleDesc& desc)
	{
		auto const it = m_Vehicles.find(entity);
		if (it == m_Vehicles.end())
		{
			return std::nullopt;
		}
		VehicleRecord& record = it->second;
		desc = record.Desc;
		VehicleCarriedState state;
		state.Inputs = record.Inputs;
		state.EngineSpeed = record.Controller->GetEngine().GetCurrentRPM();
		state.Gear = record.Desc.TransmissionMode == VehicleTransmissionMode::Manual
		                 ? record.Shift.Gear
		                 : record.Controller->GetTransmission().GetCurrentGear();
		for (size_t index = 0; index < VehicleWheelCount; index++)
		{
			JPH::Wheel const* wheel = record.Constraint->GetWheel(static_cast<JPH::uint>(index));
			state.WheelSpins[index] = wheel->GetAngularVelocity();
			state.WheelAngles[index] = wheel->GetRotationAngle();
		}
		DestroyVehicle(record);
		m_Vehicles.erase(it);
		return state;
	}

	Result<void> VehicleWorld::RestoreVehicle(VehicleDesc const& desc, VehicleCarriedState const& state, JPH::BodyID body,
	                                          JPH::ObjectLayer objectLayer)
	{
		return MakeVehicle(desc, &state, body, objectLayer);
	}

	void VehicleWorld::ReaddConstraints(std::vector<JPH::Constraint*> const& joints)
	{
		std::vector<JPH::Constraint*> constraints;
		constraints.reserve(m_Vehicles.size() + joints.size());
		for (auto const& [entity, record] : m_Vehicles)
		{
			constraints.push_back(record.Constraint.GetPtr());
		}
		constraints.insert(constraints.end(), joints.begin(), joints.end());
		if (!constraints.empty())
		{
			int const count = static_cast<int>(constraints.size());
			m_System.RemoveConstraints(constraints.data(), count);
			m_System.AddConstraints(constraints.data(), count);
		}
		bool const listen = !m_Vehicles.empty();
		if (listen && !m_IsListening)
		{
			m_System.AddStepListener(this);
		}
		else if (!listen && m_IsListening)
		{
			m_System.RemoveStepListener(this);
		}
		m_IsListening = listen;
	}

	void VehicleWorld::BeginStep()
	{
		JPH::BodyInterface& bodies = m_System.GetBodyInterface();
		for (auto& [entity, record] : m_Vehicles)
		{
			VehicleInputs const& inputs = record.Inputs;
			// Throttle as Jolt's forward input, the lean as its right input, the front brake as its brake and the rear brake
			// as its hand brake.
			record.Controller->SetDriverInput(inputs.Throttle, inputs.Lean, inputs.FrontBrake, inputs.RearBrake);
			record.AppliedInputs = inputs;
			// A sleeping chassis's constraint is not solved (Jolt's sample wakes it too, MotorcycleTest.cpp:196-198).
			if (inputs != VehicleInputs())
			{
				bodies.ActivateBody(record.Body);
			}
		}
	}

	void VehicleWorld::OnStep(JPH::PhysicsStepListenerContext const& context)
	{
		ST_PROFILE_SCOPE("Physics.Vehicles");
		for (auto& [entity, record] : m_Vehicles)
		{
			UpdateManualGearbox(record, context);
			// VehicleConstraint::OnStep is private to the constraint and public in the listener interface it implements.
			static_cast<JPH::PhysicsStepListener&>(*record.Constraint).OnStep(context);
		}
	}

	void VehicleWorld::KeepWithinDefinitions()
	{
		for (auto& [entity, record] : m_Vehicles)
		{
			KeepWithinDefinition(record);
		}
	}

	void VehicleWorld::KeepWithinDefinition(VehicleRecord& record)
	{
		StradaMotorcycleController& controller = *record.Controller;
		int32_t const gears = static_cast<int32_t>(record.Desc.GearRatios.size());
		JPH::VehicleTransmission& transmission = controller.GetTransmission();
		int32_t const gear = transmission.GetCurrentGear();
		if (gear < 0 || gear > gears)
		{
			transmission.Set(std::clamp(gear, 0, gears), transmission.GetClutchFriction());
		}
		VehicleShiftState& shift = record.Shift;
		shift.Gear = std::clamp(shift.Gear, 0, gears);
		shift.SwitchTimeLeft = std::clamp(shift.SwitchTimeLeft, 0.0f, record.Desc.SwitchTime);
		shift.ClutchReleaseTimeLeft = std::clamp(shift.ClutchReleaseTimeLeft, 0.0f, record.Desc.ClutchReleaseTime);
		controller.GetEngine().SetCurrentRPM(controller.GetEngine().GetCurrentRPM());
	}

	void VehicleWorld::SaveState(JPH::StateRecorder& recorder) const
	{
		recorder.Write(static_cast<uint64_t>(m_Vehicles.size()));
		for (auto const& [entity, record] : m_Vehicles)
		{
			recorder.Write(entity.GetValue());
			WriteInputs(recorder, record.Inputs);
			WriteInputs(recorder, record.AppliedInputs);
			recorder.Write(record.PendingShift);
			recorder.Write(static_cast<uint8_t>(record.Desc.TransmissionMode));
			recorder.Write(record.Shift.Gear);
			recorder.Write(record.Shift.SwitchTimeLeft);
			recorder.Write(record.Shift.ClutchReleaseTimeLeft);
			recorder.Write(record.Controller->GetLeanSpringIntegral());
			for (size_t index = 0; index < VehicleWheelCount; index++)
			{
				auto const* wheel = static_cast<JPH::WheelWV const*>(record.Constraint->GetWheel(static_cast<JPH::uint>(index)));
				VehicleWheelSurface const& surface = record.Surfaces[index];
				recorder.Write(wheel->GetSteerAngle());
				recorder.Write(wheel->mLongitudinalSlip);
				recorder.Write(wheel->mLateralSlip);
				recorder.Write(surface.ContactEntity.GetValue());
				recorder.Write(surface.Material.GetUUID().GetValue());
				recorder.Write(surface.TyreGrip);
				recorder.Write(surface.RollingResistance);
			}
		}
	}

	bool VehicleWorld::RestoreState(JPH::StateRecorder& recorder)
	{
		// Read in full first: a stream that fails, or holds other vehicles, changes nothing.
		uint64_t count = 0;
		recorder.Read(count);
		if (HasFailed(recorder) || count != m_Vehicles.size())
		{
			return false;
		}
		std::vector<SavedVehicle> saved(m_Vehicles.size());
		auto vehicle = m_Vehicles.begin();
		for (SavedVehicle& values : saved)
		{
			recorder.Read(values.Entity);
			ReadInputs(recorder, values.Inputs);
			ReadInputs(recorder, values.AppliedInputs);
			recorder.Read(values.PendingShift);
			recorder.Read(values.Mode);
			recorder.Read(values.Shift.Gear);
			recorder.Read(values.Shift.SwitchTimeLeft);
			recorder.Read(values.Shift.ClutchReleaseTimeLeft);
			recorder.Read(values.LeanSpringIntegral);
			for (size_t index = 0; index < VehicleWheelCount; index++)
			{
				recorder.Read(values.SteerAngles[index]);
				recorder.Read(values.LongitudinalSlips[index]);
				recorder.Read(values.LateralSlips[index]);
				recorder.Read(values.ContactEntities[index]);
				recorder.Read(values.Materials[index]);
				recorder.Read(values.TyreGrips[index]);
				recorder.Read(values.RollingResistances[index]);
			}
			if (HasFailed(recorder) || values.Entity != vehicle->first.GetValue())
			{
				return false;
			}
			++vehicle;
		}
		vehicle = m_Vehicles.begin();
		for (SavedVehicle const& values : saved)
		{
			VehicleRecord& record = vehicle->second;
			++vehicle;
			record.Inputs = values.Inputs;
			record.AppliedInputs = values.AppliedInputs;
			record.Controller->SetLeanSpringIntegral(values.LeanSpringIntegral);
			if (values.Mode == static_cast<uint8_t>(record.Desc.TransmissionMode))
			{
				record.PendingShift = values.PendingShift;
				record.Shift = values.Shift;
			}
			else
			{
				// The mode was edited in place since the snapshot: the gearbox is handed over as SetVehicleSettings hands it
				// (Manual takes the gear Jolt's state was restored to; Automatic goes on from it), its requests dropped.
				record.PendingShift = 0;
				record.Shift = VehicleShiftState();
				if (record.Desc.TransmissionMode == VehicleTransmissionMode::Manual)
				{
					record.Shift.Gear = std::max(record.Controller->GetTransmission().GetCurrentGear(), 0);
				}
			}
			for (size_t index = 0; index < VehicleWheelCount; index++)
			{
				auto* wheel = static_cast<JPH::WheelWV*>(record.Constraint->GetWheel(static_cast<JPH::uint>(index)));
				wheel->SetSteerAngle(values.SteerAngles[index]);
				wheel->mLongitudinalSlip = values.LongitudinalSlips[index];
				wheel->mLateralSlip = values.LateralSlips[index];
				VehicleWheelSurface& surface = record.Surfaces[index];
				surface.ContactEntity = UUID(values.ContactEntities[index]);
				surface.Material = AssetHandle(UUID(values.Materials[index]));
				surface.TyreGrip = values.TyreGrips[index];
				surface.RollingResistance = values.RollingResistances[index];
			}
		}
		return true;
	}

	void VehicleWorld::AddToHash(HashBuilder& hash) const
	{
		// A world without vehicles hashes exactly as it did before vehicles existed.
		if (m_Vehicles.empty())
		{
			return;
		}
		JPH::BodyLockInterfaceLocking const& bodies = m_System.GetBodyLockInterface();
		hash.AddUInt64(m_Vehicles.size());
		for (auto const& [entity, record] : m_Vehicles)
		{
			StradaMotorcycleController const& controller = *record.Controller;
			hash.AddUInt64(entity.GetValue());
			// The inputs as the last step applied them, in Jolt's order.
			hash.AddFloat(controller.GetForwardInput());
			hash.AddFloat(controller.GetRightInput());
			hash.AddFloat(controller.GetBrakeInput());
			hash.AddFloat(controller.GetHandBrakeInput());
			hash.AddFloat(controller.GetEngine().GetCurrentRPM());
			// The gear, the clutch and the three timers, as Jolt saves them.
			SmallStateRecorder transmission;
			controller.GetTransmission().SaveState(transmission);
			ST_CORE_ASSERT(!transmission.IsFailed(), "A gearbox's state outgrew its recorder");
			hash.AddBytes(transmission.GetBytes());
			hash.AddFloat(controller.GetPreviousDeltaTime());
			JPH::Vec3 const lean = controller.GetTargetLean();
			hash.AddFloat(lean.GetX());
			hash.AddFloat(lean.GetY());
			hash.AddFloat(lean.GetZ());
			hash.AddInt32(record.Shift.Gear);
			hash.AddFloat(record.Shift.SwitchTimeLeft);
			hash.AddFloat(record.Shift.ClutchReleaseTimeLeft);
			for (size_t index = 0; index < VehicleWheelCount; index++)
			{
				JPH::Wheel const* wheel = record.Constraint->GetWheel(static_cast<JPH::uint>(index));
				hash.AddFloat(wheel->GetAngularVelocity());
				hash.AddFloat(wheel->GetRotationAngle());
				hash.AddFloat(wheel->GetSteerAngle());
				hash.AddFloat(wheel->GetSuspensionLength());
				// The entity under it, read from its body as a character's ground is; 0 without contact.
				UUID ground = UUID::Invalid();
				if (wheel->HasContact())
				{
					JPH::BodyLockRead const lock(bodies, wheel->GetContactBodyID());
					ground = lock.Succeeded() ? UUID(lock.GetBody().GetUserData()) : UUID::Invalid();
				}
				hash.AddUInt64(ground.GetValue());
			}
		}
	}
}

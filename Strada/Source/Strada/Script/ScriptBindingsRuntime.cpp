#include "stpch.h"
#include "Strada/Script/ScriptGlue.h"

#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/Input.h"
#include "Strada/Physics/PhysicsScene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace Strada::ScriptGlue
{
	namespace
	{
		// The layout of Strada.Interop.NativeRaycastHit (no implicit padding): what a ray hits, and what a shape cast touches
		// (ShapeCastHit has the same values).
		struct RaycastHit
		{
			uint64_t Entity;
			Vector3 Point;
			Vector3 Normal;
			float Distance;
			uint32_t Reserved;
			// The physics material of the surface hit (RaycastHit::Material); 0 for none.
			uint64_t Material;
		};

		static_assert(sizeof(RaycastHit) == 48 && offsetof(RaycastHit, Distance) == 32 && offsetof(RaycastHit, Material) == 40);

		// The layout of Strada.Interop.NativeQueryShape (no implicit padding): a PhysicsQueryShape as scripts give it, Type a
		// PhysicsQueryShapeType.
		struct QueryShapeValues
		{
			int32_t Type;
			float Radius;
			float HalfHeight;
			Vector3 HalfExtents;
		};

		static_assert(sizeof(QueryShapeValues) == 24 && offsetof(QueryShapeValues, HalfHeight) == 8 &&
		              offsetof(QueryShapeValues, HalfExtents) == 12);
		// Strada.Interop.NativeQueryShape names the shapes by these values (InteropTests pins them).
		static_assert(static_cast<int32_t>(PhysicsQueryShapeType::Sphere) == 0 && static_cast<int32_t>(PhysicsQueryShapeType::Box) == 1 &&
		              static_cast<int32_t>(PhysicsQueryShapeType::Capsule) == 2);

		// The layout of Strada.Interop.NativeCollision (no implicit padding).
		struct CollisionValues
		{
			uint64_t Entity;
			uint64_t Other;
			uint64_t Step;
			uint32_t PointCount;
			uint32_t Reserved;
		};

		static_assert(sizeof(CollisionValues) == 32 && offsetof(CollisionValues, Step) == 16 &&
		              offsetof(CollisionValues, PointCount) == 24);

		// The layout of Strada.Interop.NativeContactPoint (no implicit padding).
		struct ContactPointValues
		{
			Vector3 Point;
			Vector3 Normal;
			Vector3 RelativeVelocity;
			float Impulse;
			// The physics materials' handles (ContactPoint::Material, OtherMaterial); 0 for none.
			uint64_t Material;
			uint64_t OtherMaterial;
		};

		static_assert(sizeof(ContactPointValues) == 56 && offsetof(ContactPointValues, Impulse) == 36 &&
		              offsetof(ContactPointValues, Material) == 40 && offsetof(ContactPointValues, OtherMaterial) == 48);
		// The managed side sizes the buffer Collision_Read fills with its own constant (NativeContactPoint.MaxCount, which
		// InteropTests pins): both change together.
		static_assert(MaxContactPoints == 4, "Strada.Interop.NativeContactPoint.MaxCount must equal MaxContactPoints");

		// --- Time ---

		double Time_GetElapsed()
		{
			Scene const* scene = ScriptEngine::GetSceneContext();
			return scene != nullptr ? scene->GetRuntimeTime() : 0.0;
		}

		float Time_GetFixedDeltaTime()
		{
			Scene const* scene = ScriptEngine::GetSceneContext();
			return scene != nullptr ? scene->GetFixedTimestep() : 0.0f;
		}

		uint64_t Time_GetFrameCount()
		{
			Scene const* scene = ScriptEngine::GetSceneContext();
			return scene != nullptr ? scene->GetRuntimeFrame() : 0;
		}

		float Time_GetTimeScale()
		{
			Scene const* scene = ScriptEngine::GetSceneContext();
			return scene != nullptr ? scene->GetTimeScale() : 1.0f;
		}

		void Time_SetTimeScale(float scale)
		{
			if (!std::isfinite(scale) || scale < 0.0f)
			{
				Log::GetScriptLogger().error("Time.TimeScale: {} is not a valid scale (it must be 0 or more)", scale);
			}
			else if (Scene* scene = GetScene("Time.TimeScale"))
			{
				scene->SetTimeScale(scale);
			}
		}

		// --- Input (out-of-range codes read as released) ---

		bool IsKey(int32_t key)
		{
			return key > 0 && key < static_cast<int32_t>(KeyCodeCount);
		}

		bool IsMouseButton(int32_t button)
		{
			return button >= 0 && button < static_cast<int32_t>(MouseButtonCount);
		}

		uint8_t Input_IsKeyDown(int32_t key)
		{
			return IsKey(key) && Input::IsKeyDown(static_cast<KeyCode>(key)) ? 1 : 0;
		}

		uint8_t Input_IsKeyPressed(int32_t key)
		{
			return IsKey(key) && Input::IsKeyPressed(static_cast<KeyCode>(key)) ? 1 : 0;
		}

		uint8_t Input_IsKeyReleased(int32_t key)
		{
			return IsKey(key) && Input::IsKeyReleased(static_cast<KeyCode>(key)) ? 1 : 0;
		}

		uint8_t Input_IsMouseButtonDown(int32_t button)
		{
			return IsMouseButton(button) && Input::IsMouseButtonDown(static_cast<MouseButton>(button)) ? 1 : 0;
		}

		uint8_t Input_IsMouseButtonPressed(int32_t button)
		{
			return IsMouseButton(button) && Input::IsMouseButtonPressed(static_cast<MouseButton>(button)) ? 1 : 0;
		}

		uint8_t Input_IsMouseButtonReleased(int32_t button)
		{
			return IsMouseButton(button) && Input::IsMouseButtonReleased(static_cast<MouseButton>(button)) ? 1 : 0;
		}

		void Input_GetMousePosition(Vector2* value)
		{
			*value = ToScript(Input::GetMousePosition());
		}

		void Input_GetMouseDelta(Vector2* value)
		{
			*value = ToScript(Input::GetMouseDelta());
		}

		void Input_GetMouseScrollDelta(Vector2* value)
		{
			*value = ToScript(Input::GetScrollDelta());
		}

		int32_t Input_GetCursorMode()
		{
			return static_cast<int32_t>(Input::GetCursorMode());
		}

		void Input_SetCursorMode(int32_t mode)
		{
			if (mode < static_cast<int32_t>(CursorMode::Normal) || mode > static_cast<int32_t>(CursorMode::Locked))
			{
				Log::GetScriptLogger().error("Input.CursorMode: {} is not a CursorMode", mode);
				return;
			}
			Input::SetCursorMode(static_cast<CursorMode>(mode));
		}

		uint8_t Input_IsGamepadConnected(uint32_t gamepad)
		{
			return Input::IsGamepadConnected(gamepad) ? 1 : 0;
		}

		float Input_GetGamepadAxis(uint32_t gamepad, int32_t axis)
		{
			if (axis < 0 || axis >= static_cast<int32_t>(GamepadAxisCount))
			{
				return 0.0f;
			}
			return Input::GetGamepadAxis(gamepad, static_cast<GamepadAxis>(axis));
		}

		bool IsGamepadButton(int32_t button)
		{
			return button >= 0 && button < static_cast<int32_t>(GamepadButtonCount);
		}

		uint8_t Input_IsGamepadButtonDown(uint32_t gamepad, int32_t button)
		{
			return IsGamepadButton(button) && Input::IsGamepadButtonDown(gamepad, static_cast<GamepadButton>(button)) ? 1 : 0;
		}

		uint8_t Input_IsGamepadButtonPressed(uint32_t gamepad, int32_t button)
		{
			return IsGamepadButton(button) && Input::IsGamepadButtonPressed(gamepad, static_cast<GamepadButton>(button)) ? 1 : 0;
		}

		uint8_t Input_IsGamepadButtonReleased(uint32_t gamepad, int32_t button)
		{
			return IsGamepadButton(button) && Input::IsGamepadButtonReleased(gamepad, static_cast<GamepadButton>(button)) ? 1 : 0;
		}

		// --- Physics ---

		// The live gravity while simulating, the scene setting otherwise.
		void Physics_GetGravity(Vector3* value)
		{
			glm::vec3 gravity(0.0f);
			if (Scene* scene = GetScene("Physics.Gravity"))
			{
				gravity =
					scene->GetPhysicsScene() != nullptr ? scene->GetPhysicsScene()->GetGravity() : scene->GetSettings().Physics.Gravity;
			}
			*value = ToScript(gravity);
		}

		void Physics_SetGravity(Vector3 const* value)
		{
			if (!CheckFinite("Physics.Gravity", *value))
			{
				return;
			}
			if (Scene* scene = GetScene("Physics.Gravity"))
			{
				scene->GetSettings().Physics.Gravity = FromScript(*value);
				if (PhysicsScene* physics = scene->GetPhysicsScene())
				{
					physics->SetGravity(FromScript(*value));
				}
			}
		}

		RaycastHit ToScriptHit(Strada::RaycastHit const& hit)
		{
			return {hit.Entity.GetValue(), ToScript(hit.Point), ToScript(hit.Normal), hit.Distance, 0u, hit.Material.GetUUID().GetValue()};
		}

		RaycastHit ToScriptHit(ShapeCastHit const& hit)
		{
			return {hit.Entity.GetValue(), ToScript(hit.Point), ToScript(hit.Normal), hit.Distance, 0u, hit.Material.GetUUID().GetValue()};
		}

		uint8_t Physics_Raycast(Vector3 const* origin, Vector3 const* direction, float maxDistance, uint32_t layerMask, RaycastHit* hit)
		{
			*hit = {};
			if (!CheckFinite("Physics.Raycast", *origin, *direction, maxDistance))
			{
				return 0;
			}
			Scene* const scene = GetScene("Physics.Raycast");
			if (scene == nullptr || scene->GetPhysicsScene() == nullptr)
			{
				return 0;
			}
			scene->ApplyPhysicsChanges();
			std::optional<Strada::RaycastHit> const result =
				scene->GetPhysicsScene()->Raycast(FromScript(*origin), FromScript(*direction), maxDistance, layerMask);
			if (!result)
			{
				return 0;
			}
			*hit = ToScriptHit(*result);
			return 1;
		}

		// Every entity along a ray, each at its nearest hit, ordered by distance then entity UUID (Scene::RaycastAll); copies the
		// first capacity of them into hits and returns how many it copied.
		int32_t Physics_RaycastAll(Vector3 const* origin, Vector3 const* direction, float maxDistance, uint32_t layerMask,
		                           uint8_t includeTriggers, RaycastHit* hits, int32_t capacity)
		{
			if (!CheckFinite("Physics.RaycastAll", *origin, *direction, maxDistance))
			{
				return 0;
			}
			Scene* const scene = GetScene("Physics.RaycastAll");
			if (scene == nullptr || capacity <= 0)
			{
				return 0;
			}
			std::vector<Strada::RaycastHit> found;
			scene->RaycastAll(FromScript(*origin), FromScript(*direction), maxDistance, found, {layerMask, includeTriggers != 0});
			// The order puts the nearest first, so a buffer smaller than the hits keeps the nearest.
			size_t const count = std::min(found.size(), static_cast<size_t>(capacity));
			for (size_t index = 0; index < count; index++)
			{
				hits[index] = ToScriptHit(found[index]);
			}
			return static_cast<int32_t>(count);
		}

		// The query shape a script gave, with the API member's name for messages; nothing (and a script error) for a size that is
		// not finite, negative or beyond its largest (MaxPhysicsQueryRadius, MaxPhysicsQueryExtent).
		std::optional<PhysicsQueryShape> ReadQueryShape(QueryShapeValues const& values, bool cast, std::string_view& function)
		{
			auto const checkSize = [&function](std::string_view size, float value, float largest)
			{
				if (std::isfinite(value) && value >= 0.0f && value <= largest)
				{
					return true;
				}
				Log::GetScriptLogger().error("{}: a {} of {} is not valid (it must be 0 to {:.0f})", function, size, value, largest);
				return false;
			};
			if (values.Type < static_cast<int32_t>(PhysicsQueryShapeType::Sphere) ||
			    values.Type > static_cast<int32_t>(PhysicsQueryShapeType::Capsule))
			{
				function = cast ? "Physics.ShapeCast" : "Physics.Overlap";
				Log::GetScriptLogger().error("{}: {} is not a query shape", function, values.Type);
				return std::nullopt;
			}
			switch (static_cast<PhysicsQueryShapeType>(values.Type))
			{
				case PhysicsQueryShapeType::Sphere:
					function = cast ? "Physics.SphereCast" : "Physics.OverlapSphere";
					if (!checkSize("radius", values.Radius, MaxPhysicsQueryRadius))
					{
						return std::nullopt;
					}
					return PhysicsQueryShape::MakeSphere(values.Radius);
				case PhysicsQueryShapeType::Box:
					function = cast ? "Physics.BoxCast" : "Physics.OverlapBox";
					if (!checkSize("half extent", values.HalfExtents.X, MaxPhysicsQueryExtent) ||
					    !checkSize("half extent", values.HalfExtents.Y, MaxPhysicsQueryExtent) ||
					    !checkSize("half extent", values.HalfExtents.Z, MaxPhysicsQueryExtent))
					{
						return std::nullopt;
					}
					return PhysicsQueryShape::MakeBox(FromScript(values.HalfExtents));
				case PhysicsQueryShapeType::Capsule:
					function = cast ? "Physics.CapsuleCast" : "Physics.OverlapCapsule";
					if (!checkSize("radius", values.Radius, MaxPhysicsQueryRadius) ||
					    !checkSize("half height", values.HalfHeight, MaxPhysicsQueryExtent))
					{
						return std::nullopt;
					}
					return PhysicsQueryShape::MakeCapsule(values.Radius, values.HalfHeight);
			}
			return std::nullopt;
		}

		// The first entity a sphere, box or capsule touches as it moves along a direction (Scene::ShapeCast).
		uint8_t Physics_ShapeCast(QueryShapeValues const* shape, Vector3 const* origin, Quaternion const* rotation,
		                          Vector3 const* direction, float maxDistance, uint32_t layerMask, uint8_t includeTriggers, RaycastHit* hit)
		{
			*hit = {};
			std::string_view function;
			std::optional<PhysicsQueryShape> const query = ReadQueryShape(*shape, true, function);
			if (!query || !CheckFinite(function, *origin, *rotation, *direction, maxDistance))
			{
				return 0;
			}
			Scene* const scene = GetScene(function);
			if (scene == nullptr)
			{
				return 0;
			}
			std::optional<ShapeCastHit> const result = scene->ShapeCast(
				*query, FromScript(*origin), ToRotation(*rotation), FromScript(*direction), maxDistance, {layerMask, includeTriggers != 0});
			if (!result)
			{
				return 0;
			}
			*hit = ToScriptHit(*result);
			return 1;
		}

		// Every entity a sphere, box or capsule overlaps, ordered by UUID (Scene::Overlap); copies the first capacity of their
		// IDs into entities and returns how many it copied.
		int32_t Physics_Overlap(QueryShapeValues const* shape, Vector3 const* center, Quaternion const* rotation, uint32_t layerMask,
		                        uint8_t includeTriggers, uint64_t* entities, int32_t capacity)
		{
			std::string_view function;
			std::optional<PhysicsQueryShape> const query = ReadQueryShape(*shape, false, function);
			if (!query || !CheckFinite(function, *center, *rotation))
			{
				return 0;
			}
			Scene* const scene = GetScene(function);
			if (scene == nullptr || capacity <= 0)
			{
				return 0;
			}
			std::vector<UUID> found;
			scene->Overlap(*query, FromScript(*center), ToRotation(*rotation), found, {layerMask, includeTriggers != 0});
			// The order is by UUID, so a buffer smaller than the overlaps keeps the smallest UUIDs.
			size_t const count = std::min(found.size(), static_cast<size_t>(capacity));
			for (size_t index = 0; index < count; index++)
			{
				entities[index] = found[index].GetValue();
			}
			return static_cast<int32_t>(count);
		}

		// --- Audio (the game's values of the mix, Docs/Architecture.md section 9.5; without an audio engine reads are
		// neutral and writes do nothing, silently, as a game without sound runs) ---

		float Audio_GetMasterVolume()
		{
			return AudioEngine::IsInitialized() ? AudioEngine::GetMasterVolume() : 1.0f;
		}

		void Audio_SetMasterVolume(float volume)
		{
			if (CheckFinite("Audio.MasterVolume", volume))
			{
				AudioEngine::SetMasterVolume(volume);
			}
		}

		float Audio_GetBusVolume(char const* bus, int32_t length)
		{
			return AudioEngine::GetBusVolume(ToStringView(bus, length));
		}

		void Audio_SetBusVolume(char const* bus, int32_t length, float decibels)
		{
			if (CheckFinite("Audio.SetBusVolume", decibels))
			{
				AudioEngine::SetBusVolume(ToStringView(bus, length), decibels);
			}
		}

		uint8_t Audio_IsBusMuted(char const* bus, int32_t length)
		{
			return AudioEngine::IsBusMuted(ToStringView(bus, length)) ? 1 : 0;
		}

		void Audio_SetBusMuted(char const* bus, int32_t length, uint8_t muted)
		{
			AudioEngine::SetBusMuted(ToStringView(bus, length), muted != 0);
		}

		uint8_t Audio_PlayClipAtPoint(uint64_t clip, Vector3 const* position, float volume, char const* bus, int32_t length)
		{
			if (!CheckFinite("Audio.PlayClipAtPoint", *position, volume))
			{
				return 0;
			}
			Scene* const scene = GetScene("Audio.PlayClipAtPoint");
			return scene != nullptr &&
			               scene->PlayAudioClipAtPoint(AssetHandle(UUID(clip)), FromScript(*position), volume, ToStringView(bus, length))
			           ? 1
			           : 0;
		}

		int32_t Audio_GetActiveVoiceCount()
		{
			return static_cast<int32_t>(AudioEngine::GetActiveVoiceCount());
		}

		// --- Collision ---

		// The contact event whose handlers run (ScriptEngine::GetHandledContactEvent), seen from the handling entity; 0 and
		// zeros outside one. points, when given, has room for MaxContactPoints.
		uint8_t Collision_Read(CollisionValues* collision, ContactPointValues* points)
		{
			*collision = {};
			HandledContactEvent const* const handled = ScriptEngine::GetHandledContactEvent();
			if (handled == nullptr)
			{
				return 0;
			}
			uint32_t const count = std::min(handled->Event->PointCount, MaxContactPoints);
			*collision = {handled->GetEntity().GetValue(), handled->GetOther().GetValue(), handled->Event->Step, count, 0u};
			if (points != nullptr)
			{
				for (uint32_t index = 0; index < count; index++)
				{
					ContactPoint const point = handled->GetPoint(index);
					points[index] = {ToScript(point.Point),
					                 ToScript(point.Normal),
					                 ToScript(point.RelativeVelocity),
					                 point.Impulse,
					                 point.Material.GetUUID().GetValue(),
					                 point.OtherMaterial.GetUUID().GetValue()};
				}
			}
			return 1;
		}
	}

	void RegisterRuntimeBindings(BindingTable& table)
	{
		table.Add("Time_GetElapsed", &Time_GetElapsed);
		table.Add("Time_GetFixedDeltaTime", &Time_GetFixedDeltaTime);
		table.Add("Time_GetFrameCount", &Time_GetFrameCount);
		table.Add("Time_GetTimeScale", &Time_GetTimeScale);
		table.Add("Time_SetTimeScale", &Time_SetTimeScale);

		table.Add("Input_IsKeyDown", &Input_IsKeyDown);
		table.Add("Input_IsKeyPressed", &Input_IsKeyPressed);
		table.Add("Input_IsKeyReleased", &Input_IsKeyReleased);
		table.Add("Input_IsMouseButtonDown", &Input_IsMouseButtonDown);
		table.Add("Input_IsMouseButtonPressed", &Input_IsMouseButtonPressed);
		table.Add("Input_IsMouseButtonReleased", &Input_IsMouseButtonReleased);
		table.Add("Input_GetMousePosition", &Input_GetMousePosition);
		table.Add("Input_GetMouseDelta", &Input_GetMouseDelta);
		table.Add("Input_GetMouseScrollDelta", &Input_GetMouseScrollDelta);
		table.Add("Input_GetCursorMode", &Input_GetCursorMode);
		table.Add("Input_SetCursorMode", &Input_SetCursorMode);
		table.Add("Input_IsGamepadConnected", &Input_IsGamepadConnected);
		table.Add("Input_GetGamepadAxis", &Input_GetGamepadAxis);
		table.Add("Input_IsGamepadButtonDown", &Input_IsGamepadButtonDown);
		table.Add("Input_IsGamepadButtonPressed", &Input_IsGamepadButtonPressed);
		table.Add("Input_IsGamepadButtonReleased", &Input_IsGamepadButtonReleased);

		table.Add("Physics_GetGravity", &Physics_GetGravity);
		table.Add("Physics_SetGravity", &Physics_SetGravity);
		table.Add("Physics_Raycast", &Physics_Raycast);
		table.Add("Physics_RaycastAll", &Physics_RaycastAll);
		table.Add("Physics_ShapeCast", &Physics_ShapeCast);
		table.Add("Physics_Overlap", &Physics_Overlap);

		table.Add("Collision_Read", &Collision_Read);

		table.Add("Audio_GetMasterVolume", &Audio_GetMasterVolume);
		table.Add("Audio_SetMasterVolume", &Audio_SetMasterVolume);
		table.Add("Audio_GetBusVolume", &Audio_GetBusVolume);
		table.Add("Audio_SetBusVolume", &Audio_SetBusVolume);
		table.Add("Audio_IsBusMuted", &Audio_IsBusMuted);
		table.Add("Audio_SetBusMuted", &Audio_SetBusMuted);
		table.Add("Audio_PlayClipAtPoint", &Audio_PlayClipAtPoint);
		table.Add("Audio_GetActiveVoiceCount", &Audio_GetActiveVoiceCount);
	}
}

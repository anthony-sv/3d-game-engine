#include "stpch.h"
#include "Strada/Script/ScriptGlue.h"

#include "Strada/Core/Input.h"
#include "Strada/Physics/PhysicsScene.h"

#include <cmath>

namespace Strada::ScriptGlue
{
	namespace
	{
		// The layout of Strada.Interop.NativeRaycastHit.
		struct RaycastHit
		{
			uint64_t Entity;
			Vector3 Point;
			Vector3 Normal;
			float Distance;
		};

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
			*hit = {result->Entity.GetValue(), ToScript(result->Point), ToScript(result->Normal), result->Distance};
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
	}
}

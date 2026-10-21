using System;
using Strada.Interop;

namespace Strada.VisualScripting;

// The environment of the engine's runtime (spec section 5.5): the script log, the running scene's entities and
// components through the entity bindings, Time.DeltaTime and Input. Entities that do not exist are answered here, so the
// bindings never log errors about them on the VM's behalf.
internal sealed unsafe class EngineEnvironment : IVisualScriptEnvironment
{
	public void Log(VisualScriptLogLevel level, string message)
	{
		Strada.Log.Write(level switch
		{
			VisualScriptLogLevel.Trace => Strada.Log.TraceLevel,
			VisualScriptLogLevel.Info => Strada.Log.InfoLevel,
			VisualScriptLogLevel.Warn => Strada.Log.WarnLevel,
			_ => Strada.Log.ErrorLevel,
		}, message);
	}

	public Entity? GetEntity(ulong id) => id != 0 ? ScriptRegistry.GetEntity(id) : null;

	public string GetEntityName(ulong id)
	{
		if (!IsEntityValid(id))
		{
			return string.Empty;
		}
		int length = 0;
		byte* name = InternalCalls.Entity_GetName(id, &length);
		return NativeString.FromUtf8(name, length);
	}

	public bool IsEntityValid(ulong id) => id != 0 && InternalCalls.Entity_IsValid(id) != 0;

	public bool HasComponent(ulong id, Type componentType) => IsEntityValid(id) && Component.IsOn(id, componentType);

	public Component? GetComponent(ulong id, Type componentType) => IsEntityValid(id) ? Component.FetchFrom(id, componentType) : null;

	public void SetDeltaTime(float timeStep)
	{
		Time.DeltaTime = timeStep;
	}

	public bool IsKeyPressed(KeyCode key) => Input.IsKeyPressed(key);

	public bool IsKeyReleased(KeyCode key) => Input.IsKeyReleased(key);

	public bool IsMouseButtonPressed(MouseButton button) => Input.IsMouseButtonPressed(button);

	public bool IsMouseButtonReleased(MouseButton button) => Input.IsMouseButtonReleased(button);

	public bool IsGamepadButtonPressed(GamepadButton button, int gamepad) => Input.IsGamepadButtonPressed(button, gamepad);

	public bool IsGamepadButtonReleased(GamepadButton button, int gamepad) => Input.IsGamepadButtonReleased(button, gamepad);
}

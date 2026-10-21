using System;

namespace Strada.VisualScripting;

// The levels of the engine's script log.
internal enum VisualScriptLogLevel
{
	Trace,
	Info,
	Warn,
	Error,
}

// What the VM needs from the engine (spec section 5.5): the log, entities, components, the frame time and input. The VM
// reaches the engine only through this seam, so tests run graphs against an environment of their own; calls of reflected
// API members are not seam calls. Main thread only, like the runtime.
internal interface IVisualScriptEnvironment
{
	void Log(VisualScriptLogLevel level, string message);

	// The entity's C# script instance when it has one, else a plain Entity; null for 0.
	Entity? GetEntity(ulong id);

	// "" when the entity is unknown.
	string GetEntityName(ulong id);

	// Whether the entity exists in the running scene.
	bool IsEntityValid(ulong id);

	bool HasComponent(ulong id, Type componentType);

	// The entity's component of the type (a component view), or null when the entity has none.
	Component? GetComponent(ulong id, Type componentType);

	// Sets Time.DeltaTime for the Update dispatch, as C# dispatch does.
	void SetDeltaTime(float timeStep);

	bool IsKeyPressed(KeyCode key);

	bool IsKeyReleased(KeyCode key);

	bool IsMouseButtonPressed(MouseButton button);

	bool IsMouseButtonReleased(MouseButton button);

	bool IsGamepadButtonPressed(GamepadButton button, int gamepad);

	bool IsGamepadButtonReleased(GamepadButton button, int gamepad);
}

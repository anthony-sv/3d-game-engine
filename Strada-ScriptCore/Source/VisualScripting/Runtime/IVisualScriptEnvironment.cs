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

	// The strongest point of the contact event being handled (Strada.Collision.Points[0]: seen from the entity that handles
	// it) and the Surface of its other side's physics material ("" for none); false, a default point and "" outside a
	// contact handler or for a contact without points.
	bool TryGetContactPoint(out ContactPoint point, out string surface);

	// A copy of the contact event being handled (Strada.Collision), or null outside a contact handler: a stopped execution
	// continues with the contact it read, as with Time.DeltaTime (spec-slice4 section 4.12).
	HandledContact? SaveContact();

	// Makes Strada.Collision read the contact while a stopped execution continues (null: the engine's again); answers the
	// one it replaces, which the caller puts back.
	HandledContact? RestoreContact(HandledContact? contact);

	// Sets Time.DeltaTime for the Update dispatch, as C# dispatch does.
	void SetDeltaTime(float timeStep);

	// Time.DeltaTime as scripts read it now: a stopped execution continues with the value it read (spec-slice4 section 4.12).
	float GetDeltaTime();

	bool IsKeyPressed(KeyCode key);

	bool IsKeyReleased(KeyCode key);

	bool IsMouseButtonPressed(MouseButton button);

	bool IsMouseButtonReleased(MouseButton button);

	bool IsGamepadButtonPressed(GamepadButton button, int gamepad);

	bool IsGamepadButtonReleased(GamepadButton button, int gamepad);

	// The name of the input action whose edge the engine dispatches now (Event.ActionPressed and Event.ActionReleased); ""
	// outside one.
	string GetHandledAction();

	// The number of the fixed step the running scene's input actions are in, 0 outside every step: what an execution that
	// starts records, so that a debugger resume reads that step's action edges (Docs/Architecture.md section 7.4).
	ulong GetActionStep();
}

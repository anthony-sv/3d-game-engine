using Strada.Interop;

namespace Strada;

/// <summary>Runs a script class on the entity.</summary>
[NativeComponent("Script")]
public sealed unsafe class ScriptComponent : Component
{
	private ScriptComponent()
	{
	}

	/// <summary>The full name of the script class, e.g. Game.PlayerController. Changing it replaces the instance.</summary>
	public string ClassName
	{
		get => NativeField.GetString(InternalCalls.ScriptComponent_GetClassName, Entity.ID);
		set => NativeField.SetString(InternalCalls.ScriptComponent_SetClassName, Entity.ID, value);
	}

	/// <summary>The entity's script instance, or null when it has none (unknown class, or the scene is not running).</summary>
	public Script? Instance => ScriptRegistry.GetInstance(Entity.ID);
}

using Strada.Interop;

namespace Strada;

/// <summary>The ears of the scene: the first active listener hears it.</summary>
[NativeComponent("AudioListener")]
public sealed unsafe class AudioListenerComponent : Component
{
	private AudioListenerComponent()
	{
	}

	/// <summary>Whether this listener can hear the scene.</summary>
	public bool Active
	{
		get => NativeField.GetBool(InternalCalls.AudioListenerComponent_GetActive, Entity.ID);
		set => NativeField.SetBool(InternalCalls.AudioListenerComponent_SetActive, Entity.ID, value);
	}
}

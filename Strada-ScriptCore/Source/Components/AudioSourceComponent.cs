using Strada.Interop;

namespace Strada;

/// <summary>Plays an audio clip, optionally positioned in 3D.</summary>
[NativeComponent("AudioSource")]
public sealed unsafe class AudioSourceComponent : Component
{
	private AudioSourceComponent()
	{
	}

	/// <summary>The clip played, or null.</summary>
	public AudioClip? Clip
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.AudioSourceComponent_GetClip, Entity.ID);
			return handle.IsValid ? new AudioClip(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.AudioSourceComponent_SetClip, Entity.ID, value);
	}

	/// <summary>Linear gain; 1 plays the clip unchanged.</summary>
	public float Volume
	{
		get => NativeField.Get<float>(InternalCalls.AudioSourceComponent_GetVolume, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.AudioSourceComponent_SetVolume, Entity.ID, value);
	}

	/// <summary>Playback speed multiplier, which also shifts the pitch.</summary>
	public float Pitch
	{
		get => NativeField.Get<float>(InternalCalls.AudioSourceComponent_GetPitch, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.AudioSourceComponent_SetPitch, Entity.ID, value);
	}

	/// <summary>Whether the clip repeats.</summary>
	public bool Loop
	{
		get => NativeField.GetBool(InternalCalls.AudioSourceComponent_GetLoop, Entity.ID);
		set => NativeField.SetBool(InternalCalls.AudioSourceComponent_SetLoop, Entity.ID, value);
	}

	/// <summary>Whether the sound starts with the scene (or when the component is added while it runs).</summary>
	public bool PlayOnStart
	{
		get => NativeField.GetBool(InternalCalls.AudioSourceComponent_GetPlayOnStart, Entity.ID);
		set => NativeField.SetBool(InternalCalls.AudioSourceComponent_SetPlayOnStart, Entity.ID, value);
	}

	/// <summary>Whether the sound is positioned in 3D (otherwise it plays at a constant level).</summary>
	public bool Spatial
	{
		get => NativeField.GetBool(InternalCalls.AudioSourceComponent_GetSpatial, Entity.ID);
		set => NativeField.SetBool(InternalCalls.AudioSourceComponent_SetSpatial, Entity.ID, value);
	}

	/// <summary>Distance in meters within which the sound plays at full volume.</summary>
	public float MinDistance
	{
		get => NativeField.Get<float>(InternalCalls.AudioSourceComponent_GetMinDistance, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.AudioSourceComponent_SetMinDistance, Entity.ID, value);
	}

	/// <summary>Distance in meters beyond which the sound gets no quieter.</summary>
	public float MaxDistance
	{
		get => NativeField.Get<float>(InternalCalls.AudioSourceComponent_GetMaxDistance, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.AudioSourceComponent_SetMaxDistance, Entity.ID, value);
	}

	/// <summary>Whether the sound is playing (held sounds of a paused scene count as playing).</summary>
	public bool IsPlaying => InternalCalls.AudioSourceComponent_IsPlaying(Entity.ID) != 0;

	/// <summary>Plays the clip from the start, or from where <see cref="Pause"/> left it.</summary>
	public void Play()
	{
		InternalCalls.AudioSourceComponent_Play(Entity.ID);
	}

	/// <summary>Pauses the sound, keeping its position.</summary>
	public void Pause()
	{
		InternalCalls.AudioSourceComponent_Pause(Entity.ID);
	}

	/// <summary>Stops the sound and rewinds it.</summary>
	public void Stop()
	{
		InternalCalls.AudioSourceComponent_Stop(Entity.ID);
	}
}

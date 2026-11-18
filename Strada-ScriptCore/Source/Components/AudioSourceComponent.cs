using System;
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

	/// <summary>
	/// Whether the clip is read from its file while it plays instead of being loaded whole first: only a little of it is
	/// in memory at a time, which suits music and long ambience. Changing it, like changing <see cref="Clip"/>, makes a
	/// new sound that is stopped until <see cref="Play"/>.
	/// </summary>
	public bool Stream
	{
		get => NativeField.GetBool(InternalCalls.AudioSourceComponent_GetStream, Entity.ID);
		set => NativeField.SetBool(InternalCalls.AudioSourceComponent_SetStream, Entity.ID, value);
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

	/// <summary>The bus of the project's mixer the source's sounds play on, by name in any letter case: empty for Master, and
	/// a name the mixer lacks plays on Master (logged once). Changing it moves the sounds playing, without a click.</summary>
	public string Bus
	{
		get => NativeField.GetString(InternalCalls.AudioSourceComponent_GetBus, Entity.ID);
		set
		{
			ArgumentNullException.ThrowIfNull(value);
			NativeField.SetString(InternalCalls.AudioSourceComponent_SetBus, Entity.ID, value);
		}
	}

	/// <summary>From 0 to 255 (higher values play as 255), 128 by default: when more sounds play than the engine mixes at
	/// once, those of lower priority go silent first. A one-shot playing takes a new priority too.</summary>
	public uint Priority
	{
		get => NativeField.Get<uint>(InternalCalls.AudioSourceComponent_GetPriority, Entity.ID);
		set => NativeField.Set<uint>(InternalCalls.AudioSourceComponent_SetPriority, Entity.ID, value);
	}

	/// <summary>How many sounds of the source play at once, its clip and its one-shots together: from 1 to 64 (values beyond
	/// play as the nearest), 4 by default. A one-shot past it stops the source's oldest one-shot.</summary>
	public uint MaxVoices
	{
		get => NativeField.Get<uint>(InternalCalls.AudioSourceComponent_GetMaxVoices, Entity.ID);
		set => NativeField.Set<uint>(InternalCalls.AudioSourceComponent_SetMaxVoices, Entity.ID, value);
	}

	/// <summary>How much of the doppler effect the source's sounds take when it is spatial: their pitch rises as source and
	/// listener close in and falls as they part, by the factor c / (c - v * DopplerLevel), c being the mixer's speed of sound
	/// and v how fast they close in. 0 turns it off, 1 (the default) is physical, up to 5 exaggerates it (values beyond play
	/// as the nearest). Sources that are not spatial take none.</summary>
	public float DopplerLevel
	{
		get => NativeField.Get<float>(InternalCalls.AudioSourceComponent_GetDopplerLevel, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.AudioSourceComponent_SetDopplerLevel, Entity.ID, value);
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

	/// <summary>Plays the source's clip once more on a voice of its own, over whatever the source plays already (a shot, a
	/// footstep): it never loops, follows the source as it moves and changes, and fades out with <see cref="Stop"/>. When the
	/// source already plays <see cref="MaxVoices"/> sounds, its oldest one-shot stops; with <see cref="MaxVoices"/> 1 and the
	/// clip playing, nothing more plays. A source whose clip streams loads it for its one-shots. Returns whether it
	/// started; false when the source has no clip that plays (or none at all: give it the clip to play) or nothing runs
	/// with sound.</summary>
	/// <param name="volume">Its linear gain, times the source's <see cref="Volume"/>.</param>
	public bool PlayOneShot(float volume = 1.0f)
	{
		return InternalCalls.AudioSourceComponent_PlayOneShot(Entity.ID, 0, volume) != 0;
	}

	/// <summary>Plays another clip once on a voice of the source, over whatever the source plays already, as
	/// <see cref="PlayOneShot(float)"/> plays the source's own: it never loops, follows the source and fades out with
	/// <see cref="Stop"/>. A source with no clip of its own plays the clips it is given this way (footsteps, impacts).
	/// Returns whether it started; false when the source's own clip does not play, when nothing runs with sound, and for
	/// a clip that does not load (logged).</summary>
	/// <param name="clip">The clip to play.</param>
	/// <param name="volume">Its linear gain, times the source's <see cref="Volume"/>.</param>
	public bool PlayOneShot(AudioClip clip, float volume = 1.0f)
	{
		ArgumentNullException.ThrowIfNull(clip);
		return InternalCalls.AudioSourceComponent_PlayOneShot(Entity.ID, clip.Handle.ID, volume) != 0;
	}
}

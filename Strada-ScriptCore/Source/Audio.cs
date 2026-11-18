using System;
using System.Text;
using Strada.Interop;
using Strada.VisualScripting;

namespace Strada;

/// <summary>
/// The game's side of the mix: the master volume, the game's volume and mute of each bus of the project's mixer, blends to
/// the mixer's snapshots, each bus's level as a meter reads it, sounds played at a point, and how many voices play. The
/// values set here are the game's, a gain stage after the mixer's own (the <see cref="Mixer"/> asset's), so that a settings
/// menu and the mix never overwrite each other; they are never written to the asset, and every game starts with them at
/// their defaults (a master volume of 1, every bus at 0 dB and not muted, the mixer's own values without a snapshot). Bus
/// and snapshot names match ignoring letter case; the empty bus name is Master's. Without sound (no audio engine) reads
/// give the defaults and writes do nothing.
/// </summary>
public static unsafe class Audio
{
	// Bus names longer than this many UTF-8 bytes are refused by the mixer anyway; shorter ones cross without allocating.
	private const int StackNameBytes = 256;

	/// <summary>The gain applied to the whole mix, after Master's effects and volume: 1 leaves it unchanged, 0 is silence
	/// (values below 0 are 0), above 1 louder. It moves without a click.</summary>
	public static float MasterVolume
	{
		get => InternalCalls.Audio_GetMasterVolume();
		set => InternalCalls.Audio_SetMasterVolume(value);
	}

	/// <summary>The voices mixed now across every running scene: those playing, in scenes that are not paused, within the
	/// engine's limit of voices mixed at once (beyond it, the lowest priority, then the quietest, then the oldest go
	/// silent). It counts a voice from the call that starts it, and a voice that ended from the next frame.</summary>
	public static int ActiveVoiceCount => InternalCalls.Audio_GetActiveVoiceCount();

	/// <summary>The game's volume of a bus, in decibels (0 leaves the bus as the mixer sets it); 0 for a bus the project's
	/// mixer lacks (logged once).</summary>
	/// <param name="bus">The bus's name, in any letter case; empty for Master.</param>
	[PureNode]
	public static float GetBusVolume(string bus)
	{
		ArgumentNullException.ThrowIfNull(bus);
		Span<byte> buffer = stackalloc byte[StackNameBytes];
		ReadOnlySpan<byte> name = Encode(bus, buffer);
		fixed (byte* bytes = name)
		{
			return InternalCalls.Audio_GetBusVolume(bytes, name.Length);
		}
	}

	/// <summary>Sets the game's volume of a bus, in decibels: added to the mixer's own volume of the bus, from -80 (silence)
	/// to +24 (values beyond are brought within). It moves without a click. A bus the project's mixer lacks is logged once,
	/// and nothing changes.</summary>
	/// <param name="bus">The bus's name, in any letter case; empty for Master.</param>
	/// <param name="decibels">The volume; 0 leaves the bus as the mixer sets it.</param>
	public static void SetBusVolume(string bus, float decibels)
	{
		ArgumentNullException.ThrowIfNull(bus);
		Span<byte> buffer = stackalloc byte[StackNameBytes];
		ReadOnlySpan<byte> name = Encode(bus, buffer);
		fixed (byte* bytes = name)
		{
			InternalCalls.Audio_SetBusVolume(bytes, name.Length, decibels);
		}
	}

	/// <summary>Whether the game has muted a bus (the mixer's own mute is the <see cref="Mixer"/> asset's); false for a bus
	/// the project's mixer lacks (logged once).</summary>
	/// <param name="bus">The bus's name, in any letter case; empty for Master.</param>
	[PureNode]
	public static bool IsBusMuted(string bus)
	{
		ArgumentNullException.ThrowIfNull(bus);
		Span<byte> buffer = stackalloc byte[StackNameBytes];
		ReadOnlySpan<byte> name = Encode(bus, buffer);
		fixed (byte* bytes = name)
		{
			return InternalCalls.Audio_IsBusMuted(bytes, name.Length) != 0;
		}
	}

	/// <summary>Mutes or unmutes a bus for the game: a muted bus, and every bus below it, is silent whatever the mixer sets.
	/// It moves without a click. A bus the project's mixer lacks is logged once, and nothing changes.</summary>
	/// <param name="bus">The bus's name, in any letter case; empty for Master.</param>
	/// <param name="muted">Whether the bus is silent.</param>
	public static void SetBusMuted(string bus, bool muted)
	{
		ArgumentNullException.ThrowIfNull(bus);
		Span<byte> buffer = stackalloc byte[StackNameBytes];
		ReadOnlySpan<byte> name = Encode(bus, buffer);
		fixed (byte* bytes = name)
		{
			InternalCalls.Audio_SetBusMuted(bytes, name.Length, muted ? (byte)1 : (byte)0);
		}
	}

	/// <summary>Plays a clip once at a point of the world, on a voice of the running scene rather than of an entity: it
	/// outlives whatever played it (a break, a pickup taken), is held while the scene is paused and ends with the scene. It
	/// is spatial, full within 1 m and falling with distance to 100 m, never loops, and at the engine's limit of voices it
	/// may be the one refused. Returns whether it started; false without a running scene with sound, or for a clip that does
	/// not load (logged).</summary>
	/// <param name="clip">The clip to play.</param>
	/// <param name="position">Where it sounds, in world space.</param>
	/// <param name="volume">Its linear gain; 1 plays the clip unchanged.</param>
	/// <param name="bus">The bus it plays on, by name in any letter case; empty for Master (a name the mixer lacks plays on
	/// Master, logged once).</param>
	public static bool PlayClipAtPoint(AudioClip clip, Vector3 position, float volume = 1.0f, string bus = "")
	{
		ArgumentNullException.ThrowIfNull(clip);
		ArgumentNullException.ThrowIfNull(bus);
		Span<byte> buffer = stackalloc byte[StackNameBytes];
		ReadOnlySpan<byte> name = Encode(bus, buffer);
		fixed (byte* bytes = name)
		{
			return InternalCalls.Audio_PlayClipAtPoint(clip.Handle.ID, &position, volume, bytes, name.Length) != 0;
		}
	}

	/// <summary>The snapshot of the project's mixer the mix blends to or holds, as the mixer names it; empty for the mixer's
	/// own values, which every game starts and ends with (and without sound).</summary>
	public static string CurrentSnapshot
	{
		get
		{
			int length;
			byte* text = InternalCalls.Audio_GetCurrentSnapshot(&length);
			return NativeString.FromUtf8(text, length);
		}
	}

	/// <summary>Blends the mix to a snapshot of the project's mixer: every value the snapshot sets (bus volumes and mutes,
	/// effect parameters) moves from where it is to the snapshot's over the time given, in steps that sound even (volumes in
	/// decibels, frequencies by octave); the values it does not set keep the mixer's own. A transition that starts while
	/// another runs carries on from where that one is. The blend runs on mixed audio, so a scene paused does not hold it.
	/// Returns whether the mix blends to it; false for a snapshot the mixer lacks (logged once per name), which changes
	/// nothing, and without sound.</summary>
	/// <param name="snapshot">The snapshot's name, in any letter case; empty for the mixer's own values.</param>
	/// <param name="seconds">How long the blend takes, at most 300 s; 0 (and below) is as quick as a click-free change
	/// allows.</param>
	public static bool TransitionTo(string snapshot, float seconds = 0.5f)
	{
		ArgumentNullException.ThrowIfNull(snapshot);
		Span<byte> buffer = stackalloc byte[StackNameBytes];
		ReadOnlySpan<byte> name = Encode(snapshot, buffer);
		fixed (byte* bytes = name)
		{
			return InternalCalls.Audio_TransitionTo(bytes, name.Length, seconds) != 0;
		}
	}

	/// <summary>The peak of a bus's output over the last block the engine mixed (about 5 ms), after its effects and every
	/// gain: a linear level, 0 for silence and 1 for full scale (it can pass 1 before a limiter). It shows how loud a bus
	/// plays now, as a meter does; 0 for a bus the project's mixer lacks (logged once) and without sound.</summary>
	/// <param name="bus">The bus's name, in any letter case; empty for Master.</param>
	[PureNode]
	public static float GetBusPeak(string bus)
	{
		ArgumentNullException.ThrowIfNull(bus);
		Span<byte> buffer = stackalloc byte[StackNameBytes];
		ReadOnlySpan<byte> name = Encode(bus, buffer);
		fixed (byte* bytes = name)
		{
			return InternalCalls.Audio_GetBusPeak(bytes, name.Length);
		}
	}

	// The name's UTF-8 bytes: in the buffer when they fit, otherwise in an array of their own.
	private static ReadOnlySpan<byte> Encode(string text, Span<byte> buffer)
	{
		if (Encoding.UTF8.GetMaxByteCount(text.Length) <= buffer.Length)
		{
			return buffer[..Encoding.UTF8.GetBytes(text, buffer)];
		}
		return NativeString.ToUtf8(text);
	}
}

using System;
using System.Buffers;
using System.Collections.Generic;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;

namespace Strada;

/// <summary>The graphics quality of the game's views: presets made of the scene's own settings, each only lowering
/// them.</summary>
public enum QualityPreset
{
	/// <summary>Shadow maps of at most a quarter of the scene's size over half its distance, with at most two cascades and
	/// hard edges, and no ambient occlusion, bloom or anti-aliasing.</summary>
	Low = 0,

	/// <summary>Shadow maps of at most half the scene's size over three quarters of its distance, with at most three
	/// cascades.</summary>
	Medium,

	/// <summary>The scenes' settings as they are.</summary>
	High,
}

/// <summary>How the game's window is shown.</summary>
public enum WindowMode
{
	/// <summary>A window of <see cref="Settings.WindowWidth"/> by <see cref="Settings.WindowHeight"/>.</summary>
	Windowed = 0,

	/// <summary>Borderless, covering the monitor the window is on.</summary>
	Fullscreen,
}

/// <summary>
/// The player's settings of the game: the window, the graphics quality, the master volume, each bus's volume and mute, and
/// named values the game owns (its assists, its options), kept for the player in the game's own data folder, which this
/// class never names. The game starts with them as the player last saved them (or the game's own window settings and High
/// quality); every setter applies at once (the window once the frame's update has returned; the player's command line wins
/// over the window, and the editor's play mode keeps the window without applying it) and reaches the file only at
/// <see cref="Save"/>. The volumes are the player's, a stage of the mix after <see cref="Audio"/>'s, so a settings screen and
/// the game's own mixing never overwrite each other. Main thread.
/// </summary>
public static unsafe class Settings
{
	// Names whose value Get found of another type, warned about once each while the game's scripts are loaded.
	private static readonly HashSet<string> s_Mismatched = new(StringComparer.Ordinal);

	/// <summary>The graphics quality of the game's views, applied from the next frame.</summary>
	public static QualityPreset Quality
	{
		get => (QualityPreset)InternalCalls.Settings_GetQuality();
		set => InternalCalls.Settings_SetQuality((int)value);
	}

	/// <summary>Whether the game's window is a window or covers its monitor.</summary>
	public static WindowMode WindowMode
	{
		get => (WindowMode)GetWindow().Mode;
		set
		{
			NativePlayerWindow window = GetWindow();
			window.Mode = (int)value;
			SetWindow(window);
		}
	}

	/// <summary>The width of the game's window when it is a window, in screen coordinates.</summary>
	public static int WindowWidth => GetWindow().Width;

	/// <summary>The height of the game's window when it is a window, in screen coordinates.</summary>
	public static int WindowHeight => GetWindow().Height;

	/// <summary>Whether frames are shown in step with the display's refresh.</summary>
	public static bool VSync
	{
		get => GetWindow().VSync != 0;
		set
		{
			NativePlayerWindow window = GetWindow();
			window.VSync = value ? 1 : 0;
			SetWindow(window);
		}
	}

	/// <summary>The player's master volume, from 0 (silence) to 1 (the mix as the game makes it); values beyond are brought
	/// within. It applies at once, without a click.</summary>
	public static float MasterVolume
	{
		get => InternalCalls.Settings_GetMasterVolume();
		set => InternalCalls.Settings_SetMasterVolume(value);
	}

	/// <summary>Sets the size of the game's window when it is a window, in screen coordinates (64 to 16,384; values beyond are
	/// brought within).</summary>
	/// <param name="width">The width.</param>
	/// <param name="height">The height.</param>
	public static void SetWindowSize(int width, int height)
	{
		NativePlayerWindow window = GetWindow();
		window.Width = width;
		window.Height = height;
		SetWindow(window);
	}

	/// <summary>The player's volume of a bus of the game's mixer, in decibels (0 leaves the bus as the game sets it).</summary>
	/// <param name="bus">The bus's name, in any letter case.</param>
	[PureNode]
	public static float GetBusVolume(string bus)
	{
		ArgumentNullException.ThrowIfNull(bus);
		byte[] name = NativeString.ToUtf8(bus);
		fixed (byte* bytes = name)
		{
			return InternalCalls.Settings_GetBusVolume(bytes, name.Length);
		}
	}

	/// <summary>Sets the player's volume of a bus, in decibels: added to the game's own, from -80 (silence) to +24 (values
	/// beyond are brought within). It applies at once, without a click; a bus the game's mixer lacks is kept by name.</summary>
	/// <param name="bus">The bus's name, in any letter case: 1 to 64 bytes (the master volume is <see cref="MasterVolume"/>).</param>
	/// <param name="decibels">The volume; 0 leaves the bus as the game sets it.</param>
	public static void SetBusVolume(string bus, float decibels)
	{
		ArgumentNullException.ThrowIfNull(bus);
		byte[] name = NativeString.ToUtf8(bus);
		fixed (byte* bytes = name)
		{
			InternalCalls.Settings_SetBusVolume(bytes, name.Length, decibels);
		}
	}

	/// <summary>Whether the player muted a bus of the game's mixer.</summary>
	/// <param name="bus">The bus's name, in any letter case.</param>
	[PureNode]
	public static bool IsBusMuted(string bus)
	{
		ArgumentNullException.ThrowIfNull(bus);
		byte[] name = NativeString.ToUtf8(bus);
		fixed (byte* bytes = name)
		{
			return InternalCalls.Settings_IsBusMuted(bytes, name.Length) != 0;
		}
	}

	/// <summary>Mutes or unmutes a bus for the player; it applies at once.</summary>
	/// <param name="bus">The bus's name, in any letter case: 1 to 64 bytes.</param>
	/// <param name="muted">Whether the bus is silent.</param>
	public static void SetBusMuted(string bus, bool muted)
	{
		ArgumentNullException.ThrowIfNull(bus);
		byte[] name = NativeString.ToUtf8(bus);
		fixed (byte* bytes = name)
		{
			InternalCalls.Settings_SetBusMuted(bytes, name.Length, muted ? (byte)1 : (byte)0);
		}
	}

	/// <summary>Whether the game keeps a value of the name in the settings, of any type.</summary>
	/// <param name="name">The value's name; names differ in letter case.</param>
	[PureNode]
	public static bool Has(string name) => Find(name) != null;

	/// <summary>The game's value of the name, when the settings hold one of <typeparamref name="T"/>'s type (as
	/// <see cref="SaveSlot.Get{T}"/> says); otherwise the fallback, and a value of another type is warned about once per
	/// name.</summary>
	/// <param name="name">The value's name.</param>
	/// <param name="fallback">What settings without such a value answer.</param>
	[PureNode]
	public static T Get<T>(string name, T fallback)
	{
		if (Find(name) is not { } saved)
		{
			return fallback;
		}
		if (SavedValueCodec.TryFromSaved(saved, out T value, out string? problem))
		{
			return value;
		}
		if (s_Mismatched.Add(name))
		{
			Log.Warn($"Settings.Get: value '{name}': {problem}: the fallback answers");
		}
		return fallback;
	}

	/// <summary>Sets a value of the game's (an assist, an option), with a save slot's rules for names and values (as
	/// <see cref="SaveSlot.Set{T}"/> says). Keep many values in a slot instead: each changed value measures the settings'
	/// file.</summary>
	/// <param name="name">1 to 128 bytes of UTF-8 without control characters.</param>
	/// <param name="value">The value.</param>
	/// <exception cref="ArgumentException">The name, the type or the value is not one the settings hold, they hold 4,096
	/// values already, or the value would make the settings' file larger than 1 MiB.</exception>
	public static void Set<T>(string name, T value)
	{
		SavedValueCodec.RequireName("Settings.Set", name);
		SavedValue saved = SavedValueCodec.ToSaved("Settings.Set", value);
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer))
		{
			writer.WriteStartObject();
			writer.WritePropertyName(name);
			SavedValueCodec.Write(writer, saved);
			writer.WriteEndObject();
		}
		int length;
		byte* refused;
		fixed (byte* bytes = buffer.WrittenSpan)
		{
			refused = InternalCalls.Settings_SetValue(bytes, buffer.WrittenCount, &length);
		}
		if (refused != null)
		{
			throw new ArgumentException($"Settings.Set: {NativeString.FromUtf8(refused, length)}", nameof(value));
		}
		s_Mismatched.Remove(name);
	}

	/// <summary>Removes a value of the game's; <see cref="Save"/> writes the settings without it.</summary>
	/// <param name="name">The value's name.</param>
	/// <returns>Whether the settings held a value of the name.</returns>
	public static bool Remove(string name)
	{
		ArgumentNullException.ThrowIfNull(name);
		s_Mismatched.Remove(name);
		byte[] utf8 = NativeString.ToUtf8(name);
		fixed (byte* bytes = utf8)
		{
			return InternalCalls.Settings_RemoveValue(bytes, utf8.Length) != 0;
		}
	}

	/// <summary>Writes the settings to the game's data folder and returns once the file is on disk, whole (the old file stays
	/// whole until then). False, logged, while no game runs and when the file cannot be written.</summary>
	/// <returns>Whether the settings are on disk.</returns>
	public static bool Save() => InternalCalls.Settings_Save() != 0;

	// The game's scripts are unloaded: the next game warns again.
	internal static void ForgetWarnings() => s_Mismatched.Clear();

	private static NativePlayerWindow GetWindow()
	{
		NativePlayerWindow window;
		InternalCalls.Settings_GetWindow(&window);
		return window;
	}

	private static void SetWindow(NativePlayerWindow window) => InternalCalls.Settings_SetWindow(&window);

	// The game's value of the name; null when the settings hold none.
	private static SavedValue? Find(string name)
	{
		ArgumentNullException.ThrowIfNull(name);
		byte[] utf8 = NativeString.ToUtf8(name);
		int length;
		byte* text;
		fixed (byte* bytes = utf8)
		{
			text = InternalCalls.Settings_GetValue(bytes, utf8.Length, &length);
		}
		if (text == null)
		{
			return null;
		}
		Utf8JsonReader reader = new(new ReadOnlySpan<byte>(text, length));
		using JsonDocument document = JsonDocument.ParseValue(ref reader);
		return SavedValueCodec.Read(document.RootElement, out _);
	}
}

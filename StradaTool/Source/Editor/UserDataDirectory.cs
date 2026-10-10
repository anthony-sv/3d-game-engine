using System;
using System.IO;

namespace Strada.Tool.Editor;

/// <summary>The engine's per-user data directory, as FileSystem::GetUserDataDirectory computes it: editors write their
/// automation instance files to Editor/Instances inside it.</summary>
internal static class UserDataDirectory
{
	/// <summary>The directory for this process's environment, or null when the environment names none.</summary>
	public static string? Get() => Get(Environment.GetEnvironmentVariable);

	/// <summary>The directory for the environment that <paramref name="readVariable"/> describes (the environment of an
	/// editor started with other variables).</summary>
	public static string? Get(Func<string, string?> readVariable)
	{
		if (OperatingSystem.IsWindows())
		{
			if (readVariable("APPDATA") is { Length: > 0 } appData)
			{
				return Path.Combine(appData, "Strada");
			}
			return readVariable("USERPROFILE") is { Length: > 0 } profile ? Path.Combine(profile, "AppData", "Roaming", "Strada") : null;
		}
		string? home = readVariable("HOME");
		if (OperatingSystem.IsMacOS())
		{
			return home is { Length: > 0 } ? Path.Combine(home, "Library", "Application Support", "Strada") : null;
		}
		if (readVariable("XDG_DATA_HOME") is { Length: > 0 } dataHome)
		{
			return Path.Combine(dataHome, "strada");
		}
		return home is { Length: > 0 } ? Path.Combine(home, ".local", "share", "strada") : null;
	}

	/// <summary>Where editors write their instance files, or null when there is no user data directory.</summary>
	public static string? GetInstanceDirectory(Func<string, string?> readVariable) =>
		Get(readVariable) is { } directory ? Path.Combine(directory, "Editor", "Instances") : null;

	/// <summary>The instance directory for this process's environment.</summary>
	public static string? GetInstanceDirectory() => GetInstanceDirectory(Environment.GetEnvironmentVariable);
}

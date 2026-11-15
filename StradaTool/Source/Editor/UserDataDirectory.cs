using System;
using System.IO;

namespace Strada.Tool.Editor;

/// <summary>The engine's per-user data directory, as FileSystem::GetUserDataDirectory computes it below a per-user root
/// (UserDataLocations::FindRoot): editors write their automation instance files to Editor/Instances inside it. A variable
/// that is unset, empty or not an absolute path counts as unset, as it does for the engine.</summary>
internal static class UserDataDirectory
{
	/// <summary>The directory for this process's environment, or null when the environment names none.</summary>
	public static string? Get() => Get(Environment.GetEnvironmentVariable);

	/// <summary>The directory for the environment that <paramref name="readVariable"/> describes (the environment of an
	/// editor started with other variables), or null when it names none. The engine then uses a folder beside its own
	/// executable, which no automation client looks in.</summary>
	public static string? Get(Func<string, string?> readVariable)
	{
		string? Read(string name) => readVariable(name) is { } value && IsAbsolute(value) ? value : null;

		if (OperatingSystem.IsWindows())
		{
			if (Read("APPDATA") is { } appData)
			{
				return Path.Combine(appData, "Strada");
			}
			return Read("USERPROFILE") is { } profile ? Path.Combine(profile, "AppData", "Roaming", "Strada") : null;
		}
		string? home = Read("HOME");
		if (OperatingSystem.IsMacOS())
		{
			return home is not null ? Path.Combine(home, "Library", "Application Support", "Strada") : null;
		}
		if (Read("XDG_DATA_HOME") is { } dataHome)
		{
			return Path.Combine(dataHome, "strada");
		}
		return home is not null ? Path.Combine(home, ".local", "share", "strada") : null;
	}

	/// <summary>Whether a variable's value is an absolute path by this platform's rules, as the engine reads it: a drive and
	/// a separator ("C:/", "C:\") or a share ("\\server", "//server") on Windows, a leading '/' elsewhere.</summary>
	internal static bool IsAbsolute(string path)
	{
		if (!OperatingSystem.IsWindows())
		{
			return path.StartsWith('/');
		}
		static bool IsSeparator(char character) => character is '/' or '\\';
		bool drive = path.Length >= 3 && char.IsAsciiLetter(path[0]) && path[1] == ':' && IsSeparator(path[2]);
		bool share = path.Length >= 3 && IsSeparator(path[0]) && IsSeparator(path[1]) && !IsSeparator(path[2]);
		return drive || share;
	}

	/// <summary>Where editors write their instance files, or null when there is no user data directory.</summary>
	public static string? GetInstanceDirectory(Func<string, string?> readVariable) =>
		Get(readVariable) is { } directory ? Path.Combine(directory, "Editor", "Instances") : null;

	/// <summary>The instance directory for this process's environment.</summary>
	public static string? GetInstanceDirectory() => GetInstanceDirectory(Environment.GetEnvironmentVariable);
}

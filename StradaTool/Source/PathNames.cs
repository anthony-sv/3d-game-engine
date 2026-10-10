using System;
using System.IO;

namespace Strada.Tool;

/// <summary>Compares paths as the platform's usual file system does: case-insensitively on Windows and macOS.</summary>
internal static class PathNames
{
	public static StringComparison Comparison { get; } =
		OperatingSystem.IsWindows() || OperatingSystem.IsMacOS() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

	/// <summary>Whether two paths (absolute, or relative to the working directory) name the same file.</summary>
	public static bool AreSame(string first, string second) => string.Equals(Normalize(first), Normalize(second), Comparison);

	private static string Normalize(string path) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
}

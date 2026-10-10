using System;
using System.IO;
using System.Linq;

namespace Strada.Tool.Editor;

/// <summary>Finds the StradaEditor executable that strada starts.</summary>
internal static class EditorLocator
{
	public const string EnvironmentVariable = "STRADA_EDITOR";

	public static string ExecutableName { get; } = OperatingSystem.IsWindows() ? "StradaEditor.exe" : "StradaEditor";

	/// <summary>The editor to start: <paramref name="path"/> (--editor) when given; else the STRADA_EDITOR environment
	/// variable; else the editor next to strada (the engine's build output, where CMake puts both); else the most recently
	/// built editor in build/*/bin below <paramref name="workingDirectory"/> (an engine checkout, whose strada `dotnet run`
	/// builds elsewhere). Throws <see cref="EditorUnavailableException"/> when there is none.</summary>
	public static string Find(string? path, Func<string, string?> readVariable, string toolDirectory, string workingDirectory)
	{
		if (!string.IsNullOrEmpty(path))
		{
			return File.Exists(path) ? Path.GetFullPath(path) : throw new EditorUnavailableException($"--editor: '{path}' does not exist");
		}
		if (readVariable(EnvironmentVariable) is { Length: > 0 } variable)
		{
			return File.Exists(variable)
				? Path.GetFullPath(variable)
				: throw new EditorUnavailableException($"{EnvironmentVariable} names '{variable}', which does not exist");
		}
		string sibling = Path.Combine(toolDirectory, ExecutableName);
		if (File.Exists(sibling))
		{
			return sibling;
		}
		string? built = FindNewestBuild(Path.Combine(workingDirectory, "build"));
		return built ?? throw new EditorUnavailableException(
			$"no {ExecutableName} found: pass --editor <path>, set {EnvironmentVariable}, or build the engine (python Tools/build.py)");
	}

	private static string? FindNewestBuild(string buildDirectory)
	{
		if (!Directory.Exists(buildDirectory))
		{
			return null;
		}
		try
		{
			return Directory.EnumerateDirectories(buildDirectory)
				.Select(preset => Path.Combine(preset, "bin", ExecutableName))
				.Where(File.Exists)
				.OrderByDescending(File.GetLastWriteTimeUtc)
				.FirstOrDefault();
		}
		catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
		{
			return null;
		}
	}
}

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace Strada.Tool.Editor;

/// <summary>A running editor's automation endpoint, read from its instance file
/// (<c>&lt;user data&gt;/Editor/Instances/&lt;pid&gt;.json</c>, Docs/Automation.md).</summary>
internal sealed class EditorInstance
{
	private const string FileType = "AutomationInstance";
	private const int FormatVersion = 1;

	public required int ProcessId { get; init; }
	public required int Port { get; init; }
	public required string Token { get; init; }
	/// <summary>The project file the editor has open; empty when none is.</summary>
	public required string Project { get; init; }
	public required string Version { get; init; }

	public static string GetPath(string directory, int processId) => Path.Combine(directory, $"{processId}.json");

	/// <summary>Reads an instance file's contents; throws <see cref="InvalidDataException"/> when they are not one.</summary>
	public static EditorInstance Parse(string json)
	{
		try
		{
			using JsonDocument document = JsonDocument.Parse(json);
			JsonElement root = document.RootElement;
			JsonElement header = root.GetProperty("Strada");
			string? type = header.GetProperty("Type").GetString();
			int version = header.GetProperty("Version").GetInt32();
			if (type != FileType)
			{
				throw new InvalidDataException($"not an automation instance file (type '{type}')");
			}
			if (version < 1 || version > FormatVersion)
			{
				throw new InvalidDataException($"instance file version {version} is not supported (this strada reads version {FormatVersion})");
			}
			int processId = checked((int)root.GetProperty("ProcessID").GetUInt32());
			int port = root.GetProperty("Port").GetUInt16();
			if (processId == 0 || port == 0)
			{
				throw new InvalidDataException("the instance file has no process ID or port");
			}
			return new EditorInstance
			{
				ProcessId = processId,
				Port = port,
				Token = root.GetProperty("Token").GetString() ?? "",
				Project = root.GetProperty("Project").GetString() ?? "",
				Version = root.GetProperty("Version").GetString() ?? "",
			};
		}
		catch (Exception exception) when (exception is JsonException or KeyNotFoundException or InvalidOperationException or FormatException or
										  OverflowException)
		{
			throw new InvalidDataException($"invalid instance file: {exception.Message}", exception);
		}
	}

	/// <summary>The editors whose instance files are in the directory and whose processes run, newest file first (the
	/// latest started editor, or the latest that opened a project). Files of processes that no longer run are deleted, as
	/// editors do; files that cannot be read are skipped.</summary>
	public static List<EditorInstance> FindRunning(string directory)
	{
		List<(EditorInstance Instance, DateTime Written)> running = [];
		IEnumerable<string> files;
		try
		{
			files = Directory.EnumerateFiles(directory, "*.json").ToList();
		}
		catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
		{
			return [];
		}
		foreach (string file in files)
		{
			EditorInstance instance;
			DateTime written;
			try
			{
				instance = Parse(File.ReadAllText(file));
				written = File.GetLastWriteTimeUtc(file);
			}
			catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidDataException)
			{
				continue;
			}
			if (IsProcessRunning(instance.ProcessId))
			{
				running.Add((instance, written));
				continue;
			}
			try
			{
				File.Delete(file);
			}
			catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
			{
				// Another scanner or the editor itself removed it, or it is not ours to remove.
			}
		}
		return running.OrderByDescending(entry => entry.Written).ThenByDescending(entry => entry.Instance.ProcessId)
			.Select(entry => entry.Instance).ToList();
	}

	/// <summary>Whether the process exists (processes of other users count as running).</summary>
	public static bool IsProcessRunning(int processId)
	{
		if (processId <= 0)
		{
			return false;
		}
		try
		{
			using Process process = Process.GetProcessById(processId);
			return !process.HasExited;
		}
		catch (ArgumentException)
		{
			return false;
		}
		catch (InvalidOperationException)
		{
			return false;
		}
		catch (Win32Exception)
		{
			// It exists, but its state cannot be read.
			return true;
		}
	}

	/// <summary>Whether the editor has this project file open.</summary>
	public bool HasProject(string projectFile) => Project.Length > 0 && PathNames.AreSame(Project, projectFile);
}

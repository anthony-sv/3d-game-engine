using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using Strada.Tool.Editor;
using Xunit;

namespace Strada.Tool.Tests;

public sealed class EditorInstanceTests
{
	// Larger than any process ID the systems hand out (Windows uses multiples of 4 below 2^32, Linux at most 2^22, macOS 99999).
	private const int ExitedProcessId = 2147483644;

	private static string InstanceJson(int processId, int port = 51234, string project = "", int version = 1, string type = "AutomationInstance") =>
		new JsonObject
		{
			["Strada"] = new JsonObject { ["Version"] = version, ["Type"] = type },
			["ProcessID"] = processId,
			["Port"] = port,
			["Token"] = "secret",
			["Project"] = project,
			["Version"] = "0.1.0",
		}.ToJsonString();

	[Fact]
	public void InstanceFilesAreParsed()
	{
		EditorInstance instance = EditorInstance.Parse(InstanceJson(1234, 4321, "C:/Games/Game.sproj"));
		Assert.Equal(1234, instance.ProcessId);
		Assert.Equal(4321, instance.Port);
		Assert.Equal("secret", instance.Token);
		Assert.Equal("C:/Games/Game.sproj", instance.Project);
		Assert.Equal("0.1.0", instance.Version);
	}

	[Theory]
	[InlineData("not json")]
	[InlineData("{}")]
	[InlineData("{\"Strada\":{\"Version\":1,\"Type\":\"AutomationInstance\"},\"ProcessID\":1,\"Port\":2}")]
	[InlineData("{\"Strada\":{\"Version\":1,\"Type\":\"AutomationInstance\"},\"ProcessID\":-1,\"Port\":2,\"Token\":\"\",\"Project\":\"\",\"Version\":\"\"}")]
	[InlineData("{\"Strada\":{\"Version\":1,\"Type\":\"AutomationInstance\"},\"ProcessID\":1,\"Port\":70000,\"Token\":\"\",\"Project\":\"\",\"Version\":\"\"}")]
	[InlineData("{\"Strada\":{\"Version\":1,\"Type\":\"AutomationInstance\"},\"ProcessID\":0,\"Port\":2,\"Token\":\"\",\"Project\":\"\",\"Version\":\"\"}")]
	public void MalformedInstanceFilesAreRejected(string json)
	{
		Assert.Throws<InvalidDataException>(() => EditorInstance.Parse(json));
	}

	[Fact]
	public void OtherFilesAndNewerVersionsAreRejected()
	{
		Assert.Throws<InvalidDataException>(() => EditorInstance.Parse(InstanceJson(1, type: "Project")));
		InvalidDataException newer = Assert.Throws<InvalidDataException>(() => EditorInstance.Parse(InstanceJson(1, version: 2)));
		Assert.Contains("version 2", newer.Message);
	}

	[Fact]
	public void RunningEditorsAreFoundNewestFirstAndStaleFilesRemoved()
	{
		using TemporaryDirectory directory = new();
		string older = Path.Combine(directory.Path, "older.json");
		string newer = Path.Combine(directory.Path, "newer.json");
		string stale = EditorInstance.GetPath(directory.Path, ExitedProcessId);
		string corrupt = Path.Combine(directory.Path, "corrupt.json");
		// Both name this process, which runs; the ports tell them apart.
		File.WriteAllText(older, InstanceJson(Environment.ProcessId, 1001));
		File.WriteAllText(newer, InstanceJson(Environment.ProcessId, 1002));
		File.SetLastWriteTimeUtc(older, DateTime.UtcNow.AddMinutes(-5));
		File.WriteAllText(stale, InstanceJson(ExitedProcessId));
		File.WriteAllText(corrupt, "{");
		File.WriteAllText(Path.Combine(directory.Path, "notes.txt"), "not an instance");

		List<EditorInstance> running = EditorInstance.FindRunning(directory.Path);
		Assert.Equal([1002, 1001], running.Select(instance => instance.Port));
		Assert.False(File.Exists(stale));
		// Files that cannot be read are left alone: they may be written right now.
		Assert.True(File.Exists(corrupt));
		Assert.Empty(EditorInstance.FindRunning(Path.Combine(directory.Path, "missing")));
	}

	[Fact]
	public void ProcessesAreCheckedByID()
	{
		Assert.True(EditorInstance.IsProcessRunning(Environment.ProcessId));
		Assert.False(EditorInstance.IsProcessRunning(ExitedProcessId));
		Assert.False(EditorInstance.IsProcessRunning(0));
	}

	[Fact]
	public void ProjectsAreComparedAsFiles()
	{
		using TemporaryDirectory directory = new();
		string project = Path.Combine(directory.Path, "Game", "Game.sproj");
		EditorInstance instance = EditorInstance.Parse(InstanceJson(1, project: project));
		Assert.True(instance.HasProject(project));
		Assert.True(instance.HasProject(Path.Combine(directory.Path, "Game", "..", "Game", "Game.sproj")));
		Assert.False(instance.HasProject(Path.Combine(directory.Path, "Other", "Game.sproj")));
		Assert.Equal(OperatingSystem.IsWindows() || OperatingSystem.IsMacOS(), instance.HasProject(project.ToUpperInvariant()));
		Assert.False(EditorInstance.Parse(InstanceJson(1)).HasProject(project));
	}
}

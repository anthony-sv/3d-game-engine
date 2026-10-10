using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Strada.Tool.Editor;
using Xunit;

namespace Strada.Tool.Tests;

/// <summary>strada with the real editor, started headless with a user data directory of the test's own (so that the
/// user's editors and recent projects are left alone). STRADA_EDITOR names the editor; ctest sets it.</summary>
public sealed class EditorIntegrationTests
{
	private static string[] InstanceFiles(string directory) => Directory.Exists(directory) ? Directory.GetFiles(directory) : [];

	private static async Task<bool> WaitUntilAsync(Func<bool> condition, double seconds = 30.0)
	{
		Stopwatch elapsed = Stopwatch.StartNew();
		while (!condition())
		{
			if (elapsed.Elapsed.TotalSeconds > seconds)
			{
				return false;
			}
			await Task.Delay(50);
		}
		return true;
	}

	[Fact]
	public async Task McpSessionsDriveAHeadlessEditor()
	{
		string editor = TestEditor.Require();
		using TemporaryDirectory directory = new();
		EditorSessionOptions options = TestEditor.IsolatedSession(editor, Path.Combine(directory.Path, "UserData"));
		string instances = options.InstanceDirectory!;
		McpTestClient client = new(options);
		int processId;
		try
		{
			await client.ResultAsync("initialize", new JsonObject { ["protocolVersion"] = "2025-06-18" });
			JsonArray tools = (await client.ResultAsync("tools/list"))["tools"]!.AsArray();
			Assert.Contains(tools, tool => tool!["name"]!.GetValue<string>() == "entity_create");
			Assert.Contains(tools, tool => tool!["name"]!.GetValue<string>() == "asset_create_folder");
			// The session keeps the headless editor it started for the list.
			EditorInstance instance = Assert.Single(EditorInstance.FindRunning(instances));
			processId = instance.ProcessId;
			Assert.Equal("", instance.Project);

			JsonObject created = await client.CallToolAsync("entity_create", new JsonObject { ["name"] = "Probe" });
			Assert.False(created["isError"]!.GetValue<bool>(), McpTestClient.Text(created));
			string id = McpTestClient.ParseText(created)["id"]!.GetValue<string>();
			JsonObject entity = await client.CallToolAsync("entity_get", new JsonObject { ["entity"] = id });
			Assert.Equal("Probe", McpTestClient.ParseText(entity)["name"]!.GetValue<string>());

			JsonObject missing = await client.CallToolAsync("entity_get", new JsonObject { ["entity"] = "123" });
			Assert.True(missing["isError"]!.GetValue<bool>());
			Assert.StartsWith("EntityNotFound (1001)", McpTestClient.Text(missing));
			JsonObject screenshot = await client.CallToolAsync("viewport_screenshot");
			Assert.True(screenshot["isError"]!.GetValue<bool>());
			Assert.StartsWith("Unavailable (1005)", McpTestClient.Text(screenshot));

			// The instance file names the project the editor has open, so that strada finds editors by project.
			string projectDirectory = Path.Combine(directory.Path, "Game");
			JsonObject project = await client.CallToolAsync("project_create",
				new JsonObject { ["directory"] = projectDirectory, ["name"] = "Game", ["discardChanges"] = true });
			Assert.False(project["isError"]!.GetValue<bool>(), McpTestClient.Text(project));
			string projectFile = Path.Combine(projectDirectory, "Game.sproj");
			Assert.True(await WaitUntilAsync(() => EditorInstance.FindRunning(instances) is [{ } open] && open.HasProject(projectFile)));
			JsonObject closed = await client.CallToolAsync("project_close", new JsonObject { ["discardChanges"] = true });
			Assert.False(closed["isError"]!.GetValue<bool>(), McpTestClient.Text(closed));
			Assert.True(await WaitUntilAsync(() => EditorInstance.FindRunning(instances) is [{ Project.Length: 0 }]));
		}
		finally
		{
			await client.DisposeAsync();
		}

		// Editors a session starts headless close with it, and remove their instance files as they exit.
		Assert.False(EditorInstance.IsProcessRunning(processId));
		Assert.Empty(InstanceFiles(instances));
	}

	[Fact]
	public async Task EditorsCloseOnceTheProcessThatStartedThemExits()
	{
		string editor = TestEditor.Require();
		using TemporaryDirectory directory = new();
		Dictionary<string, string> environment = IsolatedUserData.Environment(Path.Combine(directory.Path, "UserData"));
		string instances = IsolatedUserData.InstanceDirectory(environment);

		ProcessStartInfo sleeper = OperatingSystem.IsWindows() ? new ProcessStartInfo("ping") : new ProcessStartInfo("sleep");
		string[] sleep = OperatingSystem.IsWindows() ? ["-n", "600", "127.0.0.1"] : ["600"];
		foreach (string argument in sleep)
		{
			sleeper.ArgumentList.Add(argument);
		}
		sleeper.UseShellExecute = false;
		sleeper.RedirectStandardOutput = true;
		sleeper.CreateNoWindow = true;
		using Process parent = Process.Start(sleeper)!;
		try
		{
			using LaunchedEditor launched = await EditorLauncher.LaunchAsync(new EditorLaunchOptions
			{
				Executable = editor,
				InstanceDirectory = instances,
				Headless = true,
				ParentProcessId = parent.Id,
				Environment = environment,
			}, CancellationToken.None);
			Assert.False(launched.HasExited);

			parent.Kill();
			await parent.WaitForExitAsync(TestContext.Current.CancellationToken);
			Assert.True(await WaitUntilAsync(() => launched.HasExited), "the editor kept running after its parent process exited");
			// It closed normally, removing its instance file.
			Assert.Empty(InstanceFiles(instances));
		}
		finally
		{
			// An editor still running then closes by itself.
			EditorLauncher.Kill(parent);
		}
	}

	[Fact]
	public async Task AProjectTheStartedEditorCannotOpenIsReported()
	{
		string editor = TestEditor.Require();
		using TemporaryDirectory directory = new();
		string project = Path.Combine(directory.Path, "Missing", "Missing.sproj");
		EditorSessionOptions options = TestEditor.IsolatedSession(editor, Path.Combine(directory.Path, "UserData"), project);
		string instances = options.InstanceDirectory!;
		await using McpTestClient client = new(options);

		JsonObject response = await client.RequestAsync("tools/list");
		string message = response["error"]!["message"]!.GetValue<string>();
		Assert.Contains($"the editor could not open the project '{project}'", message);
		// The editor, of no use without the project, was closed.
		Assert.True(await WaitUntilAsync(() => InstanceFiles(instances).Length == 0));
		Assert.Empty(EditorInstance.FindRunning(instances));
	}
}

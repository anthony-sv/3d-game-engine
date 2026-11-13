using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using Strada.Tool.Editor;
using Xunit;

namespace Strada.Tool.Tests;

/// <summary>The command lines strada starts editors with: strada mcp's sessions start them without the start screen,
/// strada launch with it; and the streams an editor strada launched holds.</summary>
public sealed class EditorLaunchTests
{
	[Fact]
	public void TheOptionsBecomeTheEditorsCommandLine()
	{
		Assert.Empty(EditorLauncher.GetArguments(new EditorLaunchOptions { Executable = "StradaEditor", InstanceDirectory = "Instances" }));
		List<string> arguments = EditorLauncher.GetArguments(new EditorLaunchOptions
		{
			Executable = "StradaEditor",
			InstanceDirectory = "Instances",
			Headless = true,
			NoStartScreen = true,
			Project = "/Games/Orb Run/Orb Run.sproj",
			ParentProcessId = 42,
		});
		Assert.Equal(["--headless", "--no-start-screen", "--project", "/Games/Orb Run/Orb Run.sproj", "--parent-process", "42"], arguments);
	}

	[Fact]
	public async Task McpSessionsStartEditorsWithoutTheStartScreen()
	{
		using TemporaryDirectory directory = new();
		string editor = Path.Combine(directory.Path, "StradaEditor");
		await File.WriteAllTextAsync(editor, "", TestContext.Current.CancellationToken);
		foreach (bool headless in new[] { false, true })
		{
			await using EditorSession session = new(new EditorSessionOptions
			{
				EditorPath = editor,
				Headless = headless,
				InstanceDirectory = Path.Combine(directory.Path, "Instances"),
			});
			List<string> arguments = EditorLauncher.GetArguments(session.CreateLaunchOptions(headless, null));
			Assert.Contains("--no-start-screen", arguments);
			Assert.Equal(headless, arguments.Contains("--headless"));
		}
	}

	/// <summary>A program that reads strada launch's output through a pipe reaches its end once strada exits, while the editor
	/// strada started keeps running: the editor holds none of strada's standard streams. STRADA_EDITOR names the editor;
	/// ctest sets it.</summary>
	[Fact]
	public async Task TheOutputOfStradaLaunchEndsWhenStradaExitsWhileTheEditorRuns()
	{
		string editor = TestEditor.Require();
		string strada = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(editor))!, OperatingSystem.IsWindows() ? "strada.exe" : "strada");
		Assert.True(File.Exists(strada), $"{strada} does not exist");
		using TemporaryDirectory directory = new();
		Dictionary<string, string> environment = IsolatedUserData.Environment(Path.Combine(directory.Path, "UserData"));
		string instances = IsolatedUserData.InstanceDirectory(environment);

		ProcessStartInfo startInfo = new(strada)
		{
			UseShellExecute = false,
			RedirectStandardInput = true,
			RedirectStandardOutput = true,
			RedirectStandardError = true,
			CreateNoWindow = true,
		};
		foreach (string argument in new[] { "launch", "--headless", "--editor", editor })
		{
			startInfo.ArgumentList.Add(argument);
		}
		foreach ((string name, string value) in environment)
		{
			startInfo.Environment[name] = value;
		}
		using Process launcher = Process.Start(startInfo)!;
		launcher.StandardInput.Close();
		Task<string> output = launcher.StandardOutput.ReadToEndAsync(TestContext.Current.CancellationToken);
		Task<string> errors = launcher.StandardError.ReadToEndAsync(TestContext.Current.CancellationToken);
		try
		{
			// WaitAsync bounds the reads where cancelling a read of the process's pipe does not interrupt it. A read that never
			// ends is the failure: the editor holds the pipe's writing end open.
			string text = await output.WaitAsync(TimeSpan.FromSeconds(90), TestContext.Current.CancellationToken);
			string message = await errors.WaitAsync(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken);
			await launcher.WaitForExitAsync(TestContext.Current.CancellationToken);
			Assert.True(launcher.ExitCode == 0, message);
			int processId = JsonNode.Parse(text)!["processId"]!.GetValue<int>();
			// The editor outlives strada.
			Assert.True(EditorInstance.FindRunning(instances) is [{ } running] && running.ProcessId == processId);
		}
		finally
		{
			if (!launcher.HasExited)
			{
				launcher.Kill(entireProcessTree: true);
			}
			foreach (EditorInstance running in EditorInstance.FindRunning(instances))
			{
				try
				{
					using Process started = Process.GetProcessById(running.ProcessId);
					started.Kill(entireProcessTree: true);
					await started.WaitForExitAsync(TestContext.Current.CancellationToken);
				}
				catch (ArgumentException)
				{
					// It exited in the meantime.
				}
			}
		}
	}

	[Fact]
	public void StradaLaunchStartsEditorsWithTheStartScreen()
	{
		foreach (string[] command in new[] { new[] { "launch" }, new[] { "launch", "--headless" }, new[] { "launch", "--project", "Game.sproj" } })
		{
			ToolArguments arguments = ToolArguments.Parse(command);
			EditorLaunchOptions options = Program.CreateLaunchOptions(arguments, "StradaEditor", "Instances",
				arguments.Project is null ? null : Path.GetFullPath(arguments.Project));
			Assert.False(options.NoStartScreen);
			Assert.DoesNotContain("--no-start-screen", EditorLauncher.GetArguments(options));
			Assert.Equal(arguments.Headless, options.Headless);
		}
	}
}

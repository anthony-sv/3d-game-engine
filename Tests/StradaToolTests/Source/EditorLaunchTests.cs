using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using Strada.Tool.Editor;
using Xunit;

namespace Strada.Tool.Tests;

/// <summary>The command lines strada starts editors with: strada mcp's sessions start them without the start screen,
/// strada launch with it.</summary>
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

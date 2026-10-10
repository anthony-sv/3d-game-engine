using System;
using System.Diagnostics;
using System.IO;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using Xunit;

namespace Strada.Tool.Tests;

/// <summary>What an agent does to make a game, through strada mcp and the real editor (headless): a project, a script it
/// writes and builds, a scene that uses it, a test run that checks it in play mode, and an export whose player passes
/// the same check without the editor. STRADA_EDITOR names the editor; ctest sets it.</summary>
public sealed class AgentWorkflowTests
{
	// The namespace is replaced by the project's.
	private const string SpinScript = """
		using Strada;
		using Strada.Testing;

		namespace ProjectNamespace;

		// Turns its entity about the vertical axis; after half a second it checks that it turned at its speed.
		public class Spin : Script
		{
			public float DegreesPerSecond = 90.0f;

			private float m_Angle;

			protected override void OnUpdate(float deltaTime)
			{
				m_Angle += DegreesPerSecond * deltaTime;
				Rotation = Quaternion.AngleAxis(m_Angle, Vector3.Up);
				if (Time.Elapsed < 0.5)
				{
					return;
				}
				TestReporter.Run("the scene's speed arrives", () => Assert.AreEqual(180.0f, DegreesPerSecond));
				TestReporter.Run("the cube turns at its speed", () =>
				{
					Quaternion expected = Quaternion.AngleAxis(DegreesPerSecond * (float)Time.Elapsed, Vector3.Up);
					Assert.AreApproximatelyEqual(1.0f, Mathf.Abs(Quaternion.Dot(expected, Rotation)), 1e-4f);
				});
				TestReporter.Finish();
				Application.Quit();
			}
		}
		""";

	private static async Task<JsonNode> ToolAsync(McpTestClient client, string tool, JsonObject? arguments = null)
	{
		JsonObject result = await client.CallToolAsync(tool, arguments);
		Assert.False(result["isError"]!.GetValue<bool>(), $"{tool}: {McpTestClient.Text(result)}");
		return McpTestClient.ParseText(result);
	}

	private static async Task<(int ExitCode, string Output)> RunAsync(string executable, string argument)
	{
		// macOS games are application bundles: the program is inside.
		if (executable.EndsWith(".app", StringComparison.Ordinal) && Directory.Exists(executable))
		{
			executable = Path.Combine(executable, "Contents", "MacOS", Path.GetFileNameWithoutExtension(executable));
		}
		ProcessStartInfo startInfo = new(executable)
		{
			UseShellExecute = false,
			RedirectStandardOutput = true,
			RedirectStandardError = true,
			CreateNoWindow = true,
		};
		startInfo.ArgumentList.Add(argument);
		using Process process = Process.Start(startInfo)!;
		Task<string> output = process.StandardOutput.ReadToEndAsync(TestContext.Current.CancellationToken);
		Task<string> error = process.StandardError.ReadToEndAsync(TestContext.Current.CancellationToken);
		await process.WaitForExitAsync(TestContext.Current.CancellationToken);
		return (process.ExitCode, await output + await error);
	}

	[Fact]
	public async Task AnAgentMakesTestsAndShipsAGame()
	{
		string editor = TestEditor.Require();
		using TemporaryDirectory directory = new();
		await using McpTestClient client = new(TestEditor.IsolatedSession(editor, Path.Combine(directory.Path, "UserData")));
		await client.ResultAsync("initialize", new JsonObject { ["protocolVersion"] = "2025-06-18" });

		string projectDirectory = Path.Combine(directory.Path, "Spinner");
		await ToolAsync(client, "project_create", new JsonObject { ["directory"] = projectDirectory, ["name"] = "Spinner" });
		JsonNode script = await ToolAsync(client, "script_create", new JsonObject { ["className"] = "Spin" });
		string className = script["class"]!.GetValue<string>();
		string projectNamespace = className[..className.LastIndexOf('.')];
		File.WriteAllText(Path.Combine(projectDirectory, script["path"]!.GetValue<string>()),
			SpinScript.Replace("namespace ProjectNamespace;", $"namespace {projectNamespace};"));
		JsonNode build = await ToolAsync(client, "script_build");
		Assert.True(build["succeeded"]!.GetValue<bool>(), build.ToJsonString());

		await ToolAsync(client, "entity_create", new JsonObject
		{
			["name"] = "Spinning Cube",
			["components"] = new JsonObject
			{
				["Mesh"] = new JsonObject { ["Mesh"] = "builtin://Cube" },
				["Script"] = new JsonObject
				{
					["ClassName"] = className,
					["Fields"] = new JsonObject { ["DegreesPerSecond"] = new JsonObject { ["Type"] = "Float", ["Value"] = 180.0 } },
				},
			},
		});
		await ToolAsync(client, "scene_save");

		// Play mode runs the scene's checks.
		JsonNode run = await ToolAsync(client, "test_run", new JsonObject { ["timeout"] = 10 });
		Assert.True(run["finished"]!.GetValue<bool>(), run.ToJsonString());
		Assert.Equal(2, run["passed"]!.GetValue<int>());
		Assert.Equal(0, run["failed"]!.GetValue<int>());

		// The exported game passes them without the editor.
		JsonNode export = await ToolAsync(client, "project_export", new JsonObject { ["directory"] = Path.Combine(directory.Path, "Build") });
		Assert.True(export["succeeded"]!.GetValue<bool>(), export.ToJsonString());
		(int exitCode, string output) = await RunAsync(export["executable"]!.GetValue<string>(), "--test");
		Assert.True(exitCode == 0, output);
		Assert.Contains("Tests passed: 2 checks", output);
	}
}

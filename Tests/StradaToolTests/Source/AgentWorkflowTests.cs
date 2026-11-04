using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using Xunit;

namespace Strada.Tool.Tests;

/// <summary>What an agent does to make a game, through strada mcp and the real editor (headless): a project, a script it
/// writes and builds, a scene that uses it, a test run that checks it in play mode, and an export whose player passes
/// the same check without the editor; and the documented debugging session, which finds and fixes a graph's bug while the
/// game plays, then ships the fix. STRADA_EDITOR names the editor; ctest sets it.</summary>
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

	private static async Task<(int ExitCode, string Output)> RunAsync(string executable, params string[] arguments)
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
		foreach (string argument in arguments)
		{
			startInfo.ArgumentList.Add(argument);
		}
		using Process process = Process.Start(startInfo)!;
		Task<string> output = process.StandardOutput.ReadToEndAsync(TestContext.Current.CancellationToken);
		Task<string> error = process.StandardError.ReadToEndAsync(TestContext.Current.CancellationToken);
		await process.WaitForExitAsync(TestContext.Current.CancellationToken);
		return (process.ExitCode, await output + await error);
	}

	// Where actual departs from expect, as the documented session's own test reads its expectations: objects member by
	// member, arrays whose elements match elements of actual in order, numbers within 1e-4, anything else equal. The path of
	// the first mismatch, or null.
	private static string? FindMismatch(JsonNode? expect, JsonNode? actual, string path)
	{
		if (expect is JsonObject expected)
		{
			if (actual is not JsonObject members)
			{
				return $"{path}: expected an object, got {actual?.ToJsonString() ?? "null"}";
			}
			foreach ((string key, JsonNode? value) in expected)
			{
				if (!members.TryGetPropertyValue(key, out JsonNode? member))
				{
					return $"{path}.{key}: missing";
				}
				if (FindMismatch(value, member, $"{path}.{key}") is string mismatch)
				{
					return mismatch;
				}
			}
			return null;
		}
		if (expect is JsonArray elements)
		{
			if (actual is not JsonArray candidates)
			{
				return $"{path}: expected an array, got {actual?.ToJsonString() ?? "null"}";
			}
			int next = 0;
			for (int index = 0; index < elements.Count; index++)
			{
				while (next < candidates.Count && FindMismatch(elements[index], candidates[next], path) is not null)
				{
					next++;
				}
				if (next == candidates.Count)
				{
					return $"{path}[{index}]: no later element matches {elements[index]?.ToJsonString()} in {candidates.ToJsonString()}";
				}
				next++;
			}
			return null;
		}
		if (expect is JsonValue number && number.GetValueKind() == JsonValueKind.Number && actual is JsonValue other &&
			other.GetValueKind() == JsonValueKind.Number)
		{
			return Math.Abs(number.GetValue<double>() - other.GetValue<double>()) <= 1e-4
				? null
				: $"{path}: expected {number.ToJsonString()}, got {other.ToJsonString()}";
		}
		return JsonNode.DeepEquals(expect, actual) ? null : $"{path}: expected {expect?.ToJsonString()}, got {actual?.ToJsonString()}";
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

	[Fact]
	public async Task AnAgentDebugsAGraphWhileTheGamePlaysAndShipsTheFix()
	{
		string editor = TestEditor.Require();
		using TemporaryDirectory directory = new();
		await using McpTestClient client = new(TestEditor.IsolatedSession(editor, Path.Combine(directory.Path, "UserData")));
		await client.ResultAsync("initialize", new JsonObject { ["protocolVersion"] = "2025-06-18" });
		JsonNode created = await ToolAsync(client, "project_create", new JsonObject { ["directory"] = Path.Combine(directory.Path, "Spinner"), ["name"] = "Spinner" });
		string createdScene = created["scene"]!["path"]!.GetValue<string>();

		// Tests/Data/VisualScripting/Agents/Debugging.json, the session Docs/Scripting.md, Docs/Automation.md and the
		// strada-make-game skill show: the Spinner with a mistake (% 36 for % 360), stopped at a conditional breakpoint,
		// fixed while stopped, traced, then undone and redone after Stop. Each step is the tool of its command.
		JsonArray session = JsonNode.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Agents", "Debugging.json")))!.AsArray();
		Assert.Equal(22, session.Count);
		for (int index = 0; index < session.Count; index++)
		{
			JsonNode step = session[index]!;
			string tool = step["command"]!.GetValue<string>().Replace('.', '_').Replace('-', '_');
			JsonNode result = await ToolAsync(client, tool, step["arguments"]!.DeepClone().AsObject());
			string? mismatch = FindMismatch(step["expect"], result, "result");
			Assert.True(mismatch is null, $"step {index + 1} {tool}: {mismatch}\nresult: {result.ToJsonString()}");
		}

		// The fix ships: the exported game's graph holds the formula fixed while the game played, and its player runs the
		// graph without a problem.
		// The scene the steps built is the one project_create made the project's start scene, which the export runs.
		await ToolAsync(client, "scene_save");
		JsonNode status = await ToolAsync(client, "editor_status");
		Assert.Equal(createdScene, status["scene"]!["path"]!.GetValue<string>());
		JsonNode info = await ToolAsync(client, "project_info");
		JsonNode startScene = await ToolAsync(client, "asset_get", new JsonObject { ["asset"] = info["settings"]!["StartScene"]!.GetValue<string>() });
		Assert.Equal("Scene", startScene["type"]!.GetValue<string>());
		string assetDirectory = info["project"]!["assetDirectory"]!.GetValue<string>();
		Assert.Equal(Path.GetFullPath(createdScene), Path.GetFullPath(Path.Combine(assetDirectory, startScene["path"]!.GetValue<string>())));
		string sceneName = status["scene"]!["name"]!.GetValue<string>();
		JsonNode export = await ToolAsync(client, "project_export", new JsonObject { ["directory"] = Path.Combine(directory.Path, "Build") });
		Assert.True(export["succeeded"]!.GetValue<bool>(), export.ToJsonString());
		string executable = export["executable"]!.GetValue<string>();
		// macOS games are application bundles, with the game's files in Contents/Resources.
		string assets = executable.EndsWith(".app", StringComparison.Ordinal) && Directory.Exists(executable)
			? Path.Combine(executable, "Contents", "Resources", "Assets")
			: Path.Combine(export["directory"]!.GetValue<string>(), "Assets");
		JsonNode exported = JsonNode.Parse(File.ReadAllText(Path.Combine(assets, "Scripts", "Spinner.sgraph")))!;
		JsonNode? fixedNode = exported["VisualScript"]!["Graph"]!["Nodes"]!.AsArray().SingleOrDefault(node => node!["ID"]!.GetValue<int>() == 7);
		Assert.NotNull(fixedNode);
		Assert.Equal("(angle + speed * dt) % 360", fixedNode["Settings"]!["Formula"]!.GetValue<string>());
		(int exitCode, string output) = await RunAsync(executable, "--headless", "--frames", "120");
		Assert.True(exitCode == 0, output);
		// The player started the Spinner's scene and ran its graph without a problem: a graph that does not load, an entity
		// left without its graph and an error while running all name the graph.
		Assert.Contains($"Started scene '{sceneName}'", output);
		Assert.DoesNotContain(".sgraph", output);
		Assert.DoesNotContain("Visual script", output);
		Assert.DoesNotContain("runs no visual script", output);
	}
}

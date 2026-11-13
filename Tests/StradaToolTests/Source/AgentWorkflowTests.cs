using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Strada.Tool.Editor;
using Xunit;

namespace Strada.Tool.Tests;

/// <summary>What an agent does to make a game, through strada mcp and the real editor (headless): a project, a script it
/// writes and builds, a scene that uses it, a test run that checks it in play mode, and an export whose player passes
/// the same check without the editor; the documented debugging session, which finds and fixes a graph's bug while the
/// game plays, then ships the fix; and a project set up for coding agents, whose .mcp.json starts a strada that reaches the
/// editor with the project. STRADA_EDITOR names the editor; ctest sets it.</summary>
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

	// The next JSON-RPC response with this ID from a strada mcp process (notifications in between are skipped), within the
	// time an editor needs to answer.
	private static async Task<JsonObject> ReadResponseAsync(Process server, int id)
	{
		using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
		timeout.CancelAfter(TimeSpan.FromSeconds(120));
		while (true)
		{
			string? line = await server.StandardOutput.ReadLineAsync(timeout.Token);
			Assert.True(line is not null, "strada mcp ended before it answered");
			if (JsonNode.Parse(line) is JsonObject message && message["id"] is JsonValue value && value.GetValue<int>() == id)
			{
				return message;
			}
		}
	}

	// The directory's absolute path with its symbolic links resolved, as a process's working directory reads it (macOS's
	// temporary directory lies below /var, a link to /private/var).
	private static string ResolveLinks(string path)
	{
		string full = Path.GetFullPath(path);
		string root = Path.GetPathRoot(full)!;
		string resolved = root;
		foreach (string part in full[root.Length..].Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries))
		{
			string next = Path.Combine(resolved, part);
			resolved = new DirectoryInfo(next).ResolveLinkTarget(returnFinalTarget: true)?.FullName ?? next;
		}
		return resolved;
	}

	private static async Task<JsonObject> RequestAsync(Process server, int id, string method, JsonObject? parameters = null)
	{
		await server.StandardInput.WriteLineAsync(McpTestClient.Request(id, method, parameters).ToJsonString());
		await server.StandardInput.FlushAsync(TestContext.Current.CancellationToken);
		JsonObject response = await ReadResponseAsync(server, id);
		Assert.True(response["error"] is null, response.ToJsonString());
		return response["result"]!.AsObject();
	}

	[Fact]
	public async Task AProjectSetUpForAgentsReachesItsEditorThroughTheStradaEntryOfItsMcpJson()
	{
		string editor = TestEditor.Require();
		using TemporaryDirectory directory = new();
		string userData = Path.Combine(directory.Path, "UserData");
		EditorSessionOptions options = TestEditor.IsolatedSession(editor, userData);
		string instances = options.InstanceDirectory!;
		await using McpTestClient client = new(options);
		await client.ResultAsync("initialize", new JsonObject { ["protocolVersion"] = "2025-06-18" });
		// Spelled as the working directory of a process started in it reads it, which strada resolves --project against.
		string projectDirectory = Path.Combine(ResolveLinks(directory.Path), "Orb Run");
		JsonNode created = await ToolAsync(client, "project_create", new JsonObject { ["directory"] = projectDirectory, ["name"] = "Orb Run" });
		string projectFile = created["project"]!["file"]!.GetValue<string>();

		// The agent asks the user first: without their consent nothing is written.
		JsonObject refused = await client.CallToolAsync("project_setup_agents");
		Assert.True(refused["isError"]!.GetValue<bool>());
		Assert.StartsWith("ConsentRequired (1010)", McpTestClient.Text(refused));
		Assert.False(File.Exists(Path.Combine(projectDirectory, ".mcp.json")));
		JsonNode setup = await ToolAsync(client, "project_setup_agents", new JsonObject { ["confirmed"] = true });
		Assert.DoesNotContain(setup["warnings"]!.AsArray(), warning => warning!.GetValue<string>().StartsWith("strada is not beside the editor", StringComparison.Ordinal));

		// The strada entry names the project file relative to the project's directory, and the strada beside the editor: by
		// its path, or as strada when that is the one on PATH.
		JsonNode entry = JsonNode.Parse(File.ReadAllText(Path.Combine(projectDirectory, ".mcp.json")))!["mcpServers"]!["strada"]!;
		string command = entry["command"]!.GetValue<string>();
		string[] arguments = entry["args"]!.AsArray().Select(argument => argument!.GetValue<string>()).ToArray();
		Assert.Equal(["mcp", "--project", Path.GetFileName(projectFile)], arguments);
		if (command != "strada")
		{
			Assert.True(Path.IsPathRooted(command), command);
			Assert.True(File.Exists(command), $"{command} does not exist");
			Assert.True(PathNames.AreSame(Path.GetDirectoryName(Path.GetFullPath(editor))!, Path.GetDirectoryName(command)!), command);
		}
		// The relative project file names the project the editor has open, seen from the project's directory.
		string resolved = Path.GetFullPath(Path.Combine(projectDirectory, arguments[2]));
		Assert.True(PathNames.AreSame(projectFile, resolved), resolved);
		bool announced = false;
		for (int attempt = 0; attempt < 600 && !announced; attempt++)
		{
			announced = EditorInstance.FindRunning(instances) is [{ } open] && open.HasProject(projectFile);
			if (!announced)
			{
				await Task.Delay(50, TestContext.Current.CancellationToken);
			}
		}
		Assert.True(announced, "the editor's instance file does not name the project");

		// Run as an MCP client runs it: the command and its arguments in the project's directory, with the test's user data.
		ProcessStartInfo startInfo = new(command)
		{
			WorkingDirectory = projectDirectory,
			UseShellExecute = false,
			RedirectStandardInput = true,
			RedirectStandardOutput = true,
			RedirectStandardError = true,
			CreateNoWindow = true,
		};
		foreach (string argument in arguments)
		{
			startInfo.ArgumentList.Add(argument);
		}
		foreach ((string name, string value) in IsolatedUserData.Environment(userData))
		{
			startInfo.Environment[name] = value;
		}
		using Process server = Process.Start(startInfo)!;
		Task<string> errors = server.StandardError.ReadToEndAsync(TestContext.Current.CancellationToken);
		try
		{
			await RequestAsync(server, 1, "initialize", new JsonObject
			{
				["protocolVersion"] = "2025-06-18",
				["capabilities"] = new JsonObject(),
				["clientInfo"] = new JsonObject { ["name"] = "tests", ["version"] = "1" },
			});
			await server.StandardInput.WriteLineAsync("""{"jsonrpc":"2.0","method":"notifications/initialized"}""");
			JsonArray tools = (await RequestAsync(server, 2, "tools/list"))["tools"]!.AsArray();
			Assert.Contains(tools, tool => tool!["name"]!.GetValue<string>() == "entity_create");
			Assert.Contains(tools, tool => tool!["name"]!.GetValue<string>() == "project_setup_agents");

			// Its tools act on the editor that has the project open.
			JsonObject status = await RequestAsync(server, 3, "tools/call", new JsonObject { ["name"] = "editor_status", ["arguments"] = new JsonObject() });
			Assert.False(status["isError"]!.GetValue<bool>(), McpTestClient.Text(status));
			string statusProject = McpTestClient.ParseText(status)["project"]!["file"]!.GetValue<string>();
			Assert.True(PathNames.AreSame(projectFile, statusProject), statusProject);
		}
		finally
		{
			server.StandardInput.Close();
			if (!server.WaitForExit(TimeSpan.FromSeconds(60)))
			{
				server.Kill(entireProcessTree: true);
			}
		}
		Assert.True(server.ExitCode == 0, await errors);
		// It used the editor that was running, and started none.
		Assert.True(EditorInstance.FindRunning(instances) is [{ } only] && only.HasProject(projectFile));
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

		// Tests/Data/VisualScripting/Agents/Debugging.json, the session Docs/VisualScripting.md, Docs/Automation.md and the
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

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Strada.Tool.Editor;
using Strada.Tool.Mcp;

namespace Strada.Tool;

/// <summary>strada's entry point: runs one command (see <see cref="Usage"/>).</summary>
internal static class Program
{
	public const int Success = 0;
	public const int Failure = 1;
	public const int UsageError = 2;

	private const string Usage = """
		Usage: strada <command> [options]

		Commands:
		  mcp                      Serve the editor's commands as tools over the Model Context Protocol (stdio)
		  call <command> [params]  Run a command in a running editor; params is a JSON object
		  commands                 List the editor's commands
		  launch                   Start an editor and print its process ID and port
		  instances                List the running editors

		Options:
		  --editor <path>   The StradaEditor to start (default: $STRADA_EDITOR, the editor next to strada,
		                    or the most recently built build/*/bin/StradaEditor below the working directory)
		  --project <file>  Use the editor that has this project open; editors strada starts open it
		  --pid <id>        Use the running editor with this process ID (call, commands)
		  --headless        Start editors without a window (mcp, launch); strada mcp closes them when it ends
		  --new             Do not use editors strada did not start (mcp)
		  --json            Print JSON (commands, instances)
		  --help            Show this help
		  --version         Show strada's version

		Exit codes: 0 success, 1 failure, 2 wrong usage. See Docs/Automation.md.

		""";

	private static readonly UTF8Encoding s_Utf8 = new(encoderShouldEmitUTF8Identifier: false);

	public static async Task<int> Main(string[] args)
	{
		// Ctrl+C and SIGTERM end the command cleanly: headless editors that strada started are closed.
		using CancellationTokenSource stop = new();
		Console.CancelKeyPress += (_, cancel) =>
		{
			cancel.Cancel = true;
			stop.Cancel();
		};
		// Windows ends console programs without a signal they can handle; headless editors then close by themselves.
		using PosixSignalRegistration? terminate = OperatingSystem.IsWindows()
			? null
			: PosixSignalRegistration.Create(PosixSignal.SIGTERM, signal =>
			{
				signal.Cancel = true;
				stop.Cancel();
			});
		return await RunAsync(args, Console.Out, Console.Error, stop.Token);
	}

	/// <summary>Runs the command line; what it prints goes to <paramref name="output"/> and messages for people to
	/// <paramref name="error"/>. Returns the exit code. (strada mcp serves the process's standard streams.)</summary>
	public static async Task<int> RunAsync(IReadOnlyList<string> args, TextWriter output, TextWriter error, CancellationToken cancellationToken)
	{
		ToolArguments arguments;
		try
		{
			arguments = ToolArguments.Parse(args);
		}
		catch (UsageException exception)
		{
			return ReportUsageError(exception, error);
		}
		if (arguments.Version)
		{
			await output.WriteLineAsync($"strada {ToolInfo.Version}");
			return Success;
		}
		if (arguments.Help || arguments.Command is null)
		{
			await (arguments.Help ? output : error).WriteAsync(Usage);
			return arguments.Help ? Success : UsageError;
		}

		try
		{
			return arguments.Command switch
			{
				"mcp" => await RunMcpAsync(arguments, error, cancellationToken),
				"call" => await CallAsync(arguments, output, cancellationToken),
				"commands" => await ListCommandsAsync(arguments, output, cancellationToken),
				"launch" => await LaunchAsync(arguments, output, error, cancellationToken),
				"instances" => await ListInstancesAsync(arguments, output),
				_ => throw new UsageException($"unknown command '{arguments.Command}'"),
			};
		}
		catch (UsageException exception)
		{
			return ReportUsageError(exception, error);
		}
		catch (EditorUnavailableException exception)
		{
			await error.WriteLineAsync($"strada: {exception.Message}");
			return Failure;
		}
		catch (EditorCommandException exception)
		{
			await error.WriteLineAsync($"strada: {exception.Describe()}");
			return Failure;
		}
		catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
		{
			await error.WriteLineAsync("strada: stopped");
			return Failure;
		}
	}

	private static int ReportUsageError(UsageException exception, TextWriter error)
	{
		error.WriteLine($"strada: {exception.Message}");
		error.WriteLine("Run strada --help for the commands and options.");
		return UsageError;
	}

	private static async Task<int> RunMcpAsync(ToolArguments arguments, TextWriter error, CancellationToken cancellationToken)
	{
		arguments.Require(new HashSet<string> { "--editor", "--project", "--headless", "--new" }, 0, 0);
		await using EditorSession session = new(new EditorSessionOptions
		{
			EditorPath = arguments.Editor,
			Project = arguments.Project,
			Headless = arguments.Headless,
			StartNew = arguments.StartNew,
		});
		McpServer server = new(session, error);
		// The protocol owns standard output: nothing else may be written to it.
		using StreamReader input = new(Console.OpenStandardInput(), s_Utf8, detectEncodingFromByteOrderMarks: false);
		await using StreamWriter protocol = new(Console.OpenStandardOutput(), s_Utf8);
		await server.RunAsync(input, protocol, cancellationToken);
		return Success;
	}

	private static async Task<int> CallAsync(ToolArguments arguments, TextWriter output, CancellationToken cancellationToken)
	{
		arguments.Require(new HashSet<string> { "--project", "--pid" }, 1, 2);
		string command = arguments.Operands[0];
		JsonObject? parameters = arguments.Operands.Count == 2 ? ParseParameters(arguments.Operands[1]) : null;

		await using EditorSession session = new(new EditorSessionOptions { Project = arguments.Project, ProcessId = arguments.ProcessId });
		EditorConnection connection = await session.TryAttachAsync(cancellationToken) ??
			throw new EditorUnavailableException(arguments.Project is null
				? "no editor is running: start one with strada launch"
				: $"no running editor has '{arguments.Project}' open: start one with strada launch --project <file>");
		JsonNode? result = await connection.CallAsync(command, parameters, cancellationToken);
		await output.WriteLineAsync(result?.ToJsonString(JsonText.Indented) ?? "null");
		return Success;
	}

	private static JsonObject ParseParameters(string text)
	{
		try
		{
			return JsonNode.Parse(text) as JsonObject ?? throw new UsageException("the parameters are a JSON object, such as {\"name\": \"Crate\"}");
		}
		catch (JsonException exception)
		{
			throw new UsageException($"the parameters are not valid JSON: {exception.Message}");
		}
	}

	private static async Task<int> ListCommandsAsync(ToolArguments arguments, TextWriter output, CancellationToken cancellationToken)
	{
		arguments.Require(new HashSet<string> { "--editor", "--project", "--pid", "--json" }, 0, 0);
		await using EditorSession session = new(new EditorSessionOptions
		{
			EditorPath = arguments.Editor,
			Project = arguments.Project,
			ProcessId = arguments.ProcessId,
		});
		JsonArray commands = await session.GetCommandsAsync(cancellationToken);
		if (arguments.Json)
		{
			await output.WriteLineAsync(commands.ToJsonString(JsonText.Indented));
			return Success;
		}
		foreach (JsonNode? command in commands)
		{
			string name = command?["name"] is JsonValue nameValue && nameValue.TryGetValue(out string? text) ? text : "";
			string description = command?["description"] is JsonValue descriptionValue && descriptionValue.TryGetValue(out string? about) ? about : "";
			await output.WriteLineAsync($"{name,-24} {description}");
		}
		return Success;
	}

	private static async Task<int> LaunchAsync(ToolArguments arguments, TextWriter output, TextWriter error, CancellationToken cancellationToken)
	{
		arguments.Require(new HashSet<string> { "--editor", "--project", "--headless" }, 0, 0);
		string instanceDirectory = UserDataDirectory.GetInstanceDirectory() ??
			throw new EditorUnavailableException("there is no user data directory for editors (HOME, or APPDATA on Windows, is not set)");
		string? project = arguments.Project is null ? null : Path.GetFullPath(arguments.Project);
		using LaunchedEditor editor = await EditorLauncher.LaunchAsync(new EditorLaunchOptions
		{
			Executable = EditorLocator.Find(arguments.Editor, Environment.GetEnvironmentVariable, AppContext.BaseDirectory, Environment.CurrentDirectory),
			InstanceDirectory = instanceDirectory,
			Headless = arguments.Headless,
			Project = project,
		}, cancellationToken);
		await output.WriteLineAsync(Describe(editor.Instance).ToJsonString(JsonText.Indented));
		if (project is not null && !editor.Instance.HasProject(project))
		{
			await error.WriteLineAsync($"strada: the editor started without the project '{project}' (its log says why)");
			return Failure;
		}
		return Success;
	}

	private static async Task<int> ListInstancesAsync(ToolArguments arguments, TextWriter output)
	{
		arguments.Require(new HashSet<string> { "--json" }, 0, 0);
		string? directory = UserDataDirectory.GetInstanceDirectory();
		List<EditorInstance> instances = directory is null ? [] : EditorInstance.FindRunning(directory);
		if (arguments.Json)
		{
			JsonArray list = [];
			foreach (EditorInstance instance in instances)
			{
				list.Add(Describe(instance));
			}
			await output.WriteLineAsync(list.ToJsonString(JsonText.Indented));
			return Success;
		}
		if (instances.Count == 0)
		{
			await output.WriteLineAsync("No editor is running.");
			return Success;
		}
		foreach (EditorInstance instance in instances)
		{
			string project = instance.Project.Length > 0 ? instance.Project : "(no project)";
			await output.WriteLineAsync($"{instance.ProcessId,-8} port {instance.Port,-6} {instance.Version,-8} {project}");
		}
		return Success;
	}

	private static JsonObject Describe(EditorInstance instance) => new()
	{
		["processId"] = instance.ProcessId,
		["port"] = instance.Port,
		["project"] = instance.Project,
		["version"] = instance.Version,
	};
}

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Strada.Tool.Editor;

namespace Strada.Tool.Mcp;

/// <summary>A Model Context Protocol server on a stream of JSON-RPC 2.0 messages, one per line (the stdio transport). Its
/// tools are the editor's automation commands (<see cref="ToolCatalog"/>), run on the editor of an
/// <see cref="EditorSession"/>. Requests run concurrently, so a long export does not hold up a ping; each response is
/// written as one line. Requests the client cancels get no response, as the protocol asks.</summary>
internal sealed class McpServer
{
	/// <summary>The protocol versions this server speaks, newest first.</summary>
	public static readonly IReadOnlyList<string> ProtocolVersions = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"];

	public const int ParseError = -32700;
	public const int InvalidRequest = -32600;
	public const int MethodNotFound = -32601;
	public const int InvalidParams = -32602;
	public const int InternalError = -32603;

	private const string Instructions =
		"These tools run the Strada game editor's automation commands (Docs/Automation.md in the engine's repository): projects, " +
		"scenes, entities and components, assets, materials, prefabs, C# scripts, play mode with input, tests and game export. " +
		"They act on the editor the user works in, through its undo history, or on an editor started for them. Entities are " +
		"UUIDs written as strings; component fields use the scene-file format (asset references \"asset://<path in Assets>\" or " +
		"\"builtin://<name>\", rotations as quaternions [x, y, z, w] or \"RotationEuler\" in degrees). editor_status shows the " +
		"open project and scene, component_types lists the components and their fields, and log_read reads the editor's log.";

	private readonly EditorSession m_Session;
	private readonly TextWriter m_Log;
	private readonly SemaphoreSlim m_WriteLock = new(1, 1);
	private readonly SemaphoreSlim m_CatalogLock = new(1, 1);
	private readonly ConcurrentDictionary<string, CancellationTokenSource> m_Running = new(StringComparer.Ordinal);
	private ToolCatalog? m_Catalog;
	private string m_ProtocolVersion = ProtocolVersions[0];
	private TextWriter m_Output = TextWriter.Null;

	/// <param name="session">The editor the tools run on.</param>
	/// <param name="log">Diagnostics for people (standard error): never the protocol stream.</param>
	public McpServer(EditorSession session, TextWriter log)
	{
		m_Session = session;
		m_Log = log;
	}

	/// <summary>Serves the messages of <paramref name="input"/> until it ends or <paramref name="cancellationToken"/> is
	/// cancelled; requests still running then are cancelled and get no response.</summary>
	public async Task RunAsync(TextReader input, TextWriter output, CancellationToken cancellationToken)
	{
		m_Output = output;
		using CancellationTokenSource stopping = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
		List<Task> handlers = [];
		try
		{
			while (true)
			{
				string? line;
				try
				{
					line = await input.ReadLineAsync(stopping.Token);
				}
				catch (OperationCanceledException)
				{
					break;
				}
				if (line is null)
				{
					break;
				}
				if (string.IsNullOrWhiteSpace(line))
				{
					continue;
				}
				handlers.RemoveAll(handler => handler.IsCompleted);
				handlers.Add(HandleLineAsync(line, stopping.Token));
			}
		}
		finally
		{
			// The client is gone: nobody waits for the answers.
			await stopping.CancelAsync();
			await Task.WhenAll(handlers);
		}
	}

	private async Task HandleLineAsync(string line, CancellationToken stopping)
	{
		JsonNode? message;
		try
		{
			message = JsonNode.Parse(line);
		}
		catch (JsonException exception)
		{
			await WriteAsync(ErrorResponse(null, ParseError, $"Parse error: {exception.Message}"));
			return;
		}

		if (message is JsonArray batch)
		{
			if (batch.Count == 0)
			{
				await WriteAsync(ErrorResponse(null, InvalidRequest, "Invalid request: the batch is empty"));
				return;
			}
			JsonObject?[] responses = await Task.WhenAll(batch.Select(item => HandleMessageAsync(item, stopping)));
			JsonArray answers = [];
			foreach (JsonObject? response in responses)
			{
				if (response is not null)
				{
					answers.Add(response);
				}
			}
			if (answers.Count > 0)
			{
				await WriteAsync(answers);
			}
			return;
		}
		if (await HandleMessageAsync(message, stopping) is { } answer)
		{
			await WriteAsync(answer);
		}
	}

	// The response to a message: null for notifications, responses to requests of this server and cancelled requests.
	private async Task<JsonObject?> HandleMessageAsync(JsonNode? message, CancellationToken stopping)
	{
		if (message is not JsonObject request)
		{
			return ErrorResponse(null, InvalidRequest, "Invalid request: a message is a JSON object");
		}
		JsonNode? id = request["id"];
		bool hasId = request.ContainsKey("id");
		if (hasId && !IsValidId(id))
		{
			return ErrorResponse(null, InvalidRequest, "Invalid request: an ID is a string or a number");
		}
		if (request["method"] is not JsonValue methodValue || !methodValue.TryGetValue(out string? method))
		{
			// Responses ("result" or "error") answer requests of this server, which sends none.
			return request.ContainsKey("result") || request.ContainsKey("error")
				? null
				: ErrorResponse(id, InvalidRequest, "Invalid request: the message has no method");
		}
		JsonNode? parameters = request["params"];
		if (!hasId)
		{
			HandleNotification(method, parameters);
			return null;
		}

		string key = id!.ToJsonString();
		using CancellationTokenSource cancellation = CancellationTokenSource.CreateLinkedTokenSource(stopping);
		if (!m_Running.TryAdd(key, cancellation))
		{
			return ErrorResponse(id, InvalidRequest, $"Invalid request: request {key} is still running");
		}
		try
		{
			return Response(id, await DispatchAsync(method, parameters, cancellation.Token));
		}
		catch (McpException exception)
		{
			return ErrorResponse(id, exception.Code, exception.Message);
		}
		catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
		{
			return null;
		}
		catch (Exception exception)
		{
			// A defect of this server, not of the request: reported to the client and logged.
			await m_Log.WriteLineAsync($"strada mcp: {method} failed: {exception}");
			return ErrorResponse(id, InternalError, $"Internal error: {exception.Message}");
		}
		finally
		{
			m_Running.TryRemove(key, out _);
		}
	}

	private void HandleNotification(string method, JsonNode? parameters)
	{
		if (method != "notifications/cancelled")
		{
			// notifications/initialized, and notifications this server has no use for.
			return;
		}
		if (parameters?["requestId"] is { } requestId && IsValidId(requestId) &&
			m_Running.TryGetValue(requestId.ToJsonString(), out CancellationTokenSource? cancellation))
		{
			try
			{
				cancellation.Cancel();
			}
			catch (ObjectDisposedException)
			{
				// The request finished in the meantime.
			}
		}
	}

	private async Task<JsonNode> DispatchAsync(string method, JsonNode? parameters, CancellationToken cancellationToken)
	{
		switch (method)
		{
			case "initialize":
				return Initialize(parameters);
			case "ping":
				return new JsonObject();
			case "tools/list":
				return new JsonObject { ["tools"] = (await GetCatalogAsync(cancellationToken)).Describe(m_ProtocolVersion) };
			case "tools/call":
				return await CallToolAsync(parameters, cancellationToken);
			default:
				throw new McpException(MethodNotFound, $"Method not found: {method}");
		}
	}

	private JsonObject Initialize(JsonNode? parameters)
	{
		string? requested = parameters?["protocolVersion"] is JsonValue value && value.TryGetValue(out string? version) ? version : null;
		// A version this server does not speak is answered with its newest one; the client decides whether it can use it.
		m_ProtocolVersion = requested is not null && ProtocolVersions.Contains(requested) ? requested : ProtocolVersions[0];
		JsonObject serverInfo = new() { ["name"] = "strada", ["version"] = ToolInfo.Version };
		if (string.CompareOrdinal(m_ProtocolVersion, "2025-06-18") >= 0)
		{
			serverInfo["title"] = "Strada Editor";
		}
		return new JsonObject
		{
			["protocolVersion"] = m_ProtocolVersion,
			["capabilities"] = new JsonObject { ["tools"] = new JsonObject { ["listChanged"] = false } },
			["serverInfo"] = serverInfo,
			["instructions"] = Instructions,
		};
	}

	private async Task<JsonObject> CallToolAsync(JsonNode? parameters, CancellationToken cancellationToken)
	{
		if (parameters is not JsonObject call || call["name"] is not JsonValue nameValue || !nameValue.TryGetValue(out string? name))
		{
			throw new McpException(InvalidParams, "Invalid params: tools/call needs the tool's name");
		}
		JsonNode? arguments = call["arguments"];
		if (arguments is not null and not JsonObject)
		{
			throw new McpException(InvalidParams, "Invalid params: a tool's arguments are an object");
		}
		ToolCatalog catalog = await GetCatalogAsync(cancellationToken);
		if (!catalog.TryGetTool(name, out McpTool tool))
		{
			throw new McpException(InvalidParams, $"Unknown tool: {name}");
		}

		try
		{
			EditorConnection connection = await m_Session.ConnectAsync(cancellationToken);
			return ToolCatalog.ToCallResult(await connection.CallAsync(tool.Command, arguments as JsonObject, cancellationToken));
		}
		catch (Exception exception) when (exception is EditorCommandException or EditorUnavailableException)
		{
			return ToolCatalog.ToErrorResult(exception);
		}
	}

	private async Task<ToolCatalog> GetCatalogAsync(CancellationToken cancellationToken)
	{
		await m_CatalogLock.WaitAsync(cancellationToken);
		try
		{
			if (m_Catalog is null)
			{
				try
				{
					m_Catalog = ToolCatalog.FromCommands(await m_Session.GetCommandsAsync(cancellationToken));
				}
				catch (Exception exception) when (exception is EditorCommandException or EditorUnavailableException)
				{
					throw new McpException(InternalError, $"The editor's commands cannot be listed: {exception.Message}");
				}
			}
			return m_Catalog;
		}
		finally
		{
			m_CatalogLock.Release();
		}
	}

	private async Task WriteAsync(JsonNode message)
	{
		string line = message.ToJsonString(JsonText.Compact);
		await m_WriteLock.WaitAsync();
		try
		{
			await m_Output.WriteAsync(line + "\n");
			await m_Output.FlushAsync();
		}
		catch (IOException)
		{
			// The client stopped reading; its input ends next.
		}
		finally
		{
			m_WriteLock.Release();
		}
	}

	private static bool IsValidId(JsonNode? id) => id is JsonValue value && value.GetValueKind() is JsonValueKind.String or JsonValueKind.Number;

	private static JsonObject Response(JsonNode? id, JsonNode result) =>
		new() { ["jsonrpc"] = "2.0", ["id"] = id?.DeepClone(), ["result"] = result };

	private static JsonObject ErrorResponse(JsonNode? id, int code, string message) => new()
	{
		["jsonrpc"] = "2.0",
		["id"] = id?.DeepClone(),
		["error"] = new JsonObject { ["code"] = code, ["message"] = message },
	};
}

/// <summary>A protocol error: answered with a JSON-RPC error object.</summary>
internal sealed class McpException : Exception
{
	public McpException(int code, string message)
		: base(message)
	{
		Code = code;
	}

	public int Code { get; }
}

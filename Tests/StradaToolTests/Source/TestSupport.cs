using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using Strada.Tool.Editor;
using Strada.Tool.Mcp;

namespace Strada.Tool.Tests;

/// <summary>A directory that exists for one test and is deleted afterwards.</summary>
internal sealed class TemporaryDirectory : IDisposable
{
	public TemporaryDirectory()
	{
		Path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"strada-tool-tests-{Guid.NewGuid():N}");
		Directory.CreateDirectory(Path);
	}

	public string Path { get; }

	public void Dispose()
	{
		try
		{
			Directory.Delete(Path, recursive: true);
		}
		catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
		{
			// A process that still holds a file (an editor that is ending): the system cleans temporary files up.
		}
	}
}

/// <summary>A reader of lines written to a channel: one side of an in-memory MCP stream.</summary>
internal sealed class ChannelLineReader : TextReader
{
	private readonly ChannelReader<string> m_Lines;

	public ChannelLineReader(ChannelReader<string> lines)
	{
		m_Lines = lines;
	}

	public override async ValueTask<string?> ReadLineAsync(CancellationToken cancellationToken)
	{
		try
		{
			return await m_Lines.ReadAsync(cancellationToken);
		}
		catch (ChannelClosedException)
		{
			return null;
		}
	}
}

/// <summary>A writer whose lines (ended by '\n') go to a channel: the other side of an in-memory MCP stream.</summary>
internal sealed class ChannelLineWriter : TextWriter
{
	private readonly ChannelWriter<string> m_Lines;
	private readonly StringBuilder m_Line = new();
	private readonly Lock m_Lock = new();

	public ChannelLineWriter(ChannelWriter<string> lines)
	{
		m_Lines = lines;
	}

	public override Encoding Encoding => Encoding.UTF8;

	public override void Write(char value)
	{
		lock (m_Lock)
		{
			if (value != '\n')
			{
				m_Line.Append(value);
				return;
			}
			m_Lines.TryWrite(m_Line.ToString());
			m_Line.Clear();
		}
	}

	public override void Write(string? value)
	{
		foreach (char character in value ?? "")
		{
			Write(character);
		}
	}

	public override Task WriteAsync(string? value)
	{
		Write(value);
		return Task.CompletedTask;
	}

	public override Task FlushAsync() => Task.CompletedTask;
}

/// <summary>An MCP client for tests: runs an <see cref="McpServer"/> on in-memory streams and exchanges messages with it.</summary>
internal sealed class McpTestClient : IAsyncDisposable
{
	private static readonly TimeSpan s_ResponseTimeout = TimeSpan.FromSeconds(120);

	private readonly Channel<string> m_Input = Channel.CreateUnbounded<string>();
	private readonly Channel<string> m_Output = Channel.CreateUnbounded<string>();
	private readonly Task m_Server;
	private int m_NextId;

	/// <param name="ownTools">strada's own tools for the session, such as <see cref="PolyHavenTools"/>.</param>
	public McpTestClient(EditorSessionOptions options, Func<EditorSession, IReadOnlyList<McpTool>>? ownTools = null)
	{
		Session = new EditorSession(options);
		McpServer server = new(Session, TextWriter.Null, ownTools?.Invoke(Session));
		ChannelLineReader input = new(m_Input.Reader);
		ChannelLineWriter output = new(m_Output.Writer);
		m_Server = Task.Run(() => server.RunAsync(input, output, CancellationToken.None));
	}

	public EditorSession Session { get; }

	public void Send(string line) => m_Input.Writer.TryWrite(line);

	public void Send(JsonNode message) => Send(message.ToJsonString());

	/// <summary>The next message the server writes.</summary>
	public async Task<JsonNode> ReceiveAsync()
	{
		using CancellationTokenSource timeout = new(s_ResponseTimeout);
		return JsonNode.Parse(await m_Output.Reader.ReadAsync(timeout.Token)) ?? throw new InvalidDataException("the server wrote null");
	}

	/// <summary>Sends a request and returns the response (the next message: requests are sent one at a time).</summary>
	public async Task<JsonObject> RequestAsync(string method, JsonObject? parameters = null)
	{
		int id = Interlocked.Increment(ref m_NextId);
		Send(Request(id, method, parameters));
		JsonObject response = (JsonObject)await ReceiveAsync();
		Xunit.Assert.Equal(id, response["id"]!.GetValue<int>());
		return response;
	}

	/// <summary>The result of a request that must succeed.</summary>
	public async Task<JsonObject> ResultAsync(string method, JsonObject? parameters = null)
	{
		JsonObject response = await RequestAsync(method, parameters);
		Xunit.Assert.Null(response["error"]);
		return response["result"]!.AsObject();
	}

	/// <summary>The tools/call result of the tool.</summary>
	public Task<JsonObject> CallToolAsync(string tool, JsonObject? arguments = null) =>
		ResultAsync("tools/call", new JsonObject { ["name"] = tool, ["arguments"] = arguments ?? new JsonObject() });

	public static JsonObject Request(JsonNode id, string method, JsonObject? parameters = null)
	{
		JsonObject request = new() { ["jsonrpc"] = "2.0", ["id"] = id, ["method"] = method };
		if (parameters is not null)
		{
			request["params"] = parameters;
		}
		return request;
	}

	/// <summary>The text of a tool result's only text content, parsed as JSON.</summary>
	public static JsonNode ParseText(JsonObject toolResult) => JsonNode.Parse(Text(toolResult))!;

	/// <summary>The text of a tool result's last content item.</summary>
	public static string Text(JsonObject toolResult)
	{
		JsonArray content = toolResult["content"]!.AsArray();
		return content[^1]!["text"]!.GetValue<string>();
	}

	/// <summary>The messages the server wrote that were not received yet (after <see cref="StopAsync"/>, all of them).</summary>
	public List<JsonNode> TakeRemainingMessages()
	{
		List<JsonNode> messages = [];
		while (m_Output.Reader.TryRead(out string? line))
		{
			messages.Add(JsonNode.Parse(line)!);
		}
		return messages;
	}

	/// <summary>Ends the input and waits for the server to finish.</summary>
	public async Task StopAsync()
	{
		m_Input.Writer.TryComplete();
		await m_Server;
	}

	public async ValueTask DisposeAsync()
	{
		await StopAsync();
		await Session.DisposeAsync();
	}
}

/// <summary>The user data directory of an editor started for a test, apart from the user's own editors.</summary>
internal static class IsolatedUserData
{
	/// <summary>The environment variables that move an editor's user data directory into <paramref name="root"/>.</summary>
	public static Dictionary<string, string> Environment(string root)
	{
		if (OperatingSystem.IsWindows())
		{
			return new Dictionary<string, string> { ["APPDATA"] = root };
		}
		return OperatingSystem.IsMacOS() ? new Dictionary<string, string> { ["HOME"] = root } : new Dictionary<string, string> { ["XDG_DATA_HOME"] = root };
	}

	/// <summary>Where an editor with that environment writes its instance file.</summary>
	public static string InstanceDirectory(IReadOnlyDictionary<string, string> environment) =>
		UserDataDirectory.GetInstanceDirectory(name => environment.TryGetValue(name, out string? value) ? value : System.Environment.GetEnvironmentVariable(name))!;
}

using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace Strada.Tool.Tests;

/// <summary>A stand-in for an editor's automation server: authentication first, then one JSON-RPC message per line,
/// with a few commands. Its instance file (in a directory of the test's) names this test process, which runs.</summary>
internal sealed class FakeEditor : IAsyncDisposable
{
	public const string Token = "0123456789abcdef";

	private static readonly UTF8Encoding s_Utf8 = new(encoderShouldEmitUTF8Identifier: false);

	private readonly TcpListener m_Listener;
	private readonly CancellationTokenSource m_Stopping = new();
	private readonly ConcurrentDictionary<TcpClient, Task> m_Connections = new();
	private readonly Task m_AcceptLoop;
	private readonly TaskCompletionSource m_SlowCommands = new(TaskCreationOptions.RunContinuationsAsynchronously);
	private int m_Connected;

	public FakeEditor(string instanceDirectory, string project = "")
	{
		m_Listener = new TcpListener(IPAddress.Loopback, 0);
		m_Listener.Start();
		Port = ((IPEndPoint)m_Listener.LocalEndpoint).Port;
		Directory.CreateDirectory(instanceDirectory);
		InstanceFile = Path.Combine(instanceDirectory, $"{Environment.ProcessId}.json");
		JsonObject instance = new()
		{
			["Strada"] = new JsonObject { ["Version"] = 1, ["Type"] = "AutomationInstance" },
			["ProcessID"] = Environment.ProcessId,
			["Port"] = Port,
			["Token"] = Token,
			["Project"] = project,
			["Version"] = "0.1.0",
		};
		File.WriteAllText(InstanceFile, instance.ToJsonString());
		m_AcceptLoop = AcceptLoopAsync();
	}

	public int Port { get; }
	public string InstanceFile { get; }
	/// <summary>How many connections authenticated.</summary>
	public int Connections => Volatile.Read(ref m_Connected);

	/// <summary>The commands editor.commands lists.</summary>
	public static JsonArray Commands => new(
		Describe("editor.status", "Shows the editor's state", null, readOnly: true),
		Describe("entity.create", "Creates an entity",
			new JsonObject
			{
				["type"] = "object",
				["properties"] = new JsonObject { ["name"] = new JsonObject { ["type"] = "string" } },
				["additionalProperties"] = false,
			}, readOnly: false),
		Describe("entity.get", "Describes an entity", new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }, readOnly: true),
		Describe("asset.create-folder", "Creates a folder", new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() },
			readOnly: false),
		Describe("viewport.screenshot", "Captures the editor", null, readOnly: true),
		Describe("test.slow", "Answers when the test lets it", null, readOnly: true));

	/// <summary>Lets test.slow requests answer: those that wait now, and from then on every one at once.</summary>
	public void ReleaseSlowCommands() => m_SlowCommands.TrySetResult();

	/// <summary>Closes every connection, as an editor that crashed would.</summary>
	public async Task DropConnectionsAsync()
	{
		foreach (TcpClient client in m_Connections.Keys)
		{
			client.Dispose();
		}
		await Task.WhenAll(m_Connections.Values);
	}

	public async ValueTask DisposeAsync()
	{
		await m_Stopping.CancelAsync();
		m_Listener.Stop();
		ReleaseSlowCommands();
		await DropConnectionsAsync();
		await m_AcceptLoop;
		File.Delete(InstanceFile);
		m_Stopping.Dispose();
	}

	private static JsonObject Describe(string name, string description, JsonObject? parameters, bool readOnly) => new()
	{
		["name"] = name,
		["description"] = description,
		["params"] = parameters,
		["readOnly"] = readOnly,
		["async"] = false,
	};

	private async Task AcceptLoopAsync()
	{
		while (!m_Stopping.IsCancellationRequested)
		{
			TcpClient client;
			try
			{
				client = await m_Listener.AcceptTcpClientAsync(m_Stopping.Token);
			}
			catch (Exception exception) when (exception is OperationCanceledException or SocketException or ObjectDisposedException)
			{
				return;
			}
			m_Connections[client] = ServeAsync(client);
		}
	}

	private async Task ServeAsync(TcpClient client)
	{
		try
		{
			NetworkStream stream = client.GetStream();
			using StreamReader reader = new(stream, s_Utf8);
			SemaphoreSlim writeLock = new(1, 1);
			bool authenticated = false;
			while (await reader.ReadLineAsync(m_Stopping.Token) is { } line)
			{
				JsonObject request = JsonNode.Parse(line)!.AsObject();
				JsonNode? id = request["id"]?.DeepClone();
				string method = request["method"]!.GetValue<string>();
				JsonObject? parameters = request["params"] as JsonObject;
				if (!authenticated)
				{
					if (method != "authenticate" || parameters?["token"]?.GetValue<string>() != Token)
					{
						await WriteAsync(stream, writeLock, Error(id, -32001, "invalid automation token"));
						return;
					}
					authenticated = true;
					Interlocked.Increment(ref m_Connected);
					await WriteAsync(stream, writeLock, Result(id, new JsonObject { ["authenticated"] = true, ["version"] = "0.1.0" }));
					continue;
				}
				// Each request is answered on its own, so a slow one does not hold up the others (as in the editor).
				_ = AnswerAsync(stream, writeLock, id, method, parameters);
			}
		}
		catch (Exception exception) when (exception is IOException or ObjectDisposedException or OperationCanceledException or SocketException)
		{
			// The connection ended.
		}
		finally
		{
			client.Dispose();
		}
	}

	private async Task AnswerAsync(NetworkStream stream, SemaphoreSlim writeLock, JsonNode? id, string method, JsonObject? parameters)
	{
		JsonObject response;
		switch (method)
		{
			case "editor.commands":
				response = Result(id, Commands);
				break;
			case "editor.status":
				response = Result(id, new JsonObject { ["version"] = "0.1.0" });
				break;
			case "entity.create":
				response = Result(id, new JsonObject { ["id"] = "42", ["name"] = parameters?["name"]?.DeepClone() });
				break;
			case "entity.get":
				response = Error(id, 1001, "entity 123 does not exist", new JsonObject { ["path"] = "entity" });
				break;
			case "viewport.screenshot":
				response = Result(id, new JsonObject { ["mimeType"] = "image/png", ["width"] = 2, ["height"] = 1, ["data"] = "iVBORw0KGgo=" });
				break;
			case "test.slow":
				await m_SlowCommands.Task;
				response = Result(id, new JsonObject { ["done"] = true });
				break;
			default:
				response = Error(id, -32601, $"unknown command '{method}'");
				break;
		}
		try
		{
			await WriteAsync(stream, writeLock, response);
		}
		catch (Exception exception) when (exception is IOException or ObjectDisposedException or SocketException)
		{
			// The client is gone.
		}
	}

	private static async Task WriteAsync(NetworkStream stream, SemaphoreSlim writeLock, JsonObject message)
	{
		byte[] line = s_Utf8.GetBytes(message.ToJsonString() + "\n");
		await writeLock.WaitAsync();
		try
		{
			await stream.WriteAsync(line);
		}
		finally
		{
			writeLock.Release();
		}
	}

	private static JsonObject Result(JsonNode? id, JsonNode result) => new() { ["jsonrpc"] = "2.0", ["id"] = id, ["result"] = result };

	private static JsonObject Error(JsonNode? id, int code, string message, JsonNode? data = null)
	{
		JsonObject error = new() { ["code"] = code, ["message"] = message };
		if (data is not null)
		{
			error["data"] = data;
		}
		return new JsonObject { ["jsonrpc"] = "2.0", ["id"] = id, ["error"] = error };
	}
}

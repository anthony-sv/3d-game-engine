using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace Strada.Tool.Editor;

/// <summary>A connection to an editor's automation server: JSON-RPC 2.0, one JSON document per line (Docs/Automation.md).
/// It authenticates with the instance's token, then sends requests; responses are matched by ID, so requests can run
/// concurrently. Thread-safe.</summary>
internal sealed class EditorConnection : IAsyncDisposable
{
	private static readonly UTF8Encoding s_Utf8 = new(encoderShouldEmitUTF8Identifier: false);

	private readonly TcpClient m_Client;
	private readonly NetworkStream m_Stream;
	private readonly SemaphoreSlim m_WriteLock = new(1, 1);
	private readonly ConcurrentDictionary<long, TaskCompletionSource<JsonNode?>> m_Pending = new();
	private readonly CancellationTokenSource m_Closing = new();
	private readonly Task m_ReadLoop;
	private long m_NextId;
	// Why the connection ended; null while it is open.
	private Exception? m_Failure;
	// An error the editor sent without a request ID (it refuses the connection), reported when the connection ends.
	private EditorCommandException? m_ConnectionError;

	private EditorConnection(EditorInstance instance, TcpClient client)
	{
		Instance = instance;
		m_Client = client;
		m_Stream = client.GetStream();
		m_ReadLoop = Task.Run(ReadLoopAsync);
	}

	public EditorInstance Instance { get; }

	public bool IsOpen => Volatile.Read(ref m_Failure) is null;

	/// <summary>Completes when the connection has ended.</summary>
	public Task Completion => m_ReadLoop;

	/// <summary>Connects to the editor and authenticates. Throws <see cref="EditorUnavailableException"/> when the editor
	/// cannot be reached and <see cref="EditorCommandException"/> when it refuses the token or the connection.</summary>
	public static async Task<EditorConnection> ConnectAsync(EditorInstance instance, CancellationToken cancellationToken)
	{
		TcpClient client = new() { NoDelay = true };
		try
		{
			await client.ConnectAsync(IPAddress.Loopback, instance.Port, cancellationToken);
		}
		catch (SocketException exception)
		{
			client.Dispose();
			throw new EditorUnavailableException(
				$"cannot connect to the editor (process {instance.ProcessId}, port {instance.Port}): {exception.Message}", exception);
		}
		catch
		{
			client.Dispose();
			throw;
		}

		EditorConnection connection = new(instance, client);
		try
		{
			await connection.CallAsync("authenticate", new JsonObject { ["token"] = instance.Token }, cancellationToken);
		}
		catch
		{
			await connection.DisposeAsync();
			throw;
		}
		return connection;
	}

	/// <summary>Runs an editor command and returns its result. Throws <see cref="EditorCommandException"/> when the
	/// command fails and <see cref="EditorUnavailableException"/> when the connection ends first. Cancelling stops waiting
	/// (the editor still finishes the command).</summary>
	public async Task<JsonNode?> CallAsync(string method, JsonObject? parameters, CancellationToken cancellationToken)
	{
		long id = Interlocked.Increment(ref m_NextId);
		TaskCompletionSource<JsonNode?> completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
		m_Pending[id] = completion;
		try
		{
			// Registered before this check: a connection that ends from now on completes the request.
			if (Volatile.Read(ref m_Failure) is { } failure)
			{
				throw Unavailable(failure);
			}

			JsonObject request = new() { ["jsonrpc"] = "2.0", ["id"] = id, ["method"] = method };
			if (parameters is not null)
			{
				request["params"] = parameters.DeepClone();
			}
			byte[] line = s_Utf8.GetBytes(request.ToJsonString(JsonText.Compact) + "\n");
			await m_WriteLock.WaitAsync(cancellationToken);
			try
			{
				await m_Stream.WriteAsync(line, cancellationToken);
			}
			catch (Exception exception) when (exception is IOException or ObjectDisposedException)
			{
				throw new EditorUnavailableException($"the connection to the editor broke: {exception.Message}", exception);
			}
			finally
			{
				m_WriteLock.Release();
			}

			using (cancellationToken.Register(() => completion.TrySetCanceled(cancellationToken)))
			{
				return await completion.Task;
			}
		}
		finally
		{
			m_Pending.TryRemove(id, out _);
		}
	}

	public async ValueTask DisposeAsync()
	{
		if (!m_Closing.IsCancellationRequested)
		{
			await m_Closing.CancelAsync();
		}
		m_Client.Dispose();
		// The loop catches everything: it records why it ended and fails the pending requests.
		await m_ReadLoop;
		m_Closing.Dispose();
	}

	private static EditorUnavailableException Unavailable(Exception failure) =>
		failure as EditorUnavailableException ?? new EditorUnavailableException(failure.Message, failure);

	private async Task ReadLoopAsync()
	{
		Exception failure;
		try
		{
			using StreamReader reader = new(m_Stream, s_Utf8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
			while (true)
			{
				string? line = await reader.ReadLineAsync(m_Closing.Token);
				if (line is null)
				{
					failure = new EditorUnavailableException("the editor closed the connection");
					break;
				}
				if (line.Length > 0)
				{
					Dispatch(line);
				}
			}
		}
		catch (OperationCanceledException)
		{
			failure = new EditorUnavailableException("the connection to the editor was closed");
		}
		catch (Exception exception)
		{
			// Whatever ended the loop ends the connection: no request may wait for a response that cannot come.
			failure = new EditorUnavailableException($"the connection to the editor broke: {exception.Message}", exception);
		}

		if (m_ConnectionError is { } refused)
		{
			failure = new EditorUnavailableException($"the editor refused the connection: {refused.Message}", refused);
		}
		Volatile.Write(ref m_Failure, failure);
		foreach (TaskCompletionSource<JsonNode?> pending in m_Pending.Values)
		{
			pending.TrySetException(Unavailable(failure));
		}
	}

	private void Dispatch(string line)
	{
		JsonNode? message;
		try
		{
			message = JsonNode.Parse(line);
		}
		catch (JsonException)
		{
			// Not a response this client can match to a request.
			return;
		}
		if (message is JsonArray batch)
		{
			foreach (JsonNode? response in batch)
			{
				DispatchResponse(response);
			}
			return;
		}
		DispatchResponse(message);
	}

	private void DispatchResponse(JsonNode? message)
	{
		if (message is not JsonObject response)
		{
			return;
		}
		if (response["id"] is not JsonValue idValue || !idValue.TryGetValue(out long id))
		{
			if (response["error"] is JsonObject connectionError)
			{
				m_ConnectionError = EditorCommandException.FromError(connectionError);
			}
			return;
		}
		if (!m_Pending.TryRemove(id, out TaskCompletionSource<JsonNode?>? completion))
		{
			// The caller stopped waiting.
			return;
		}
		if (response["error"] is JsonObject error)
		{
			completion.TrySetException(EditorCommandException.FromError(error));
			return;
		}
		JsonNode? result = response["result"];
		response.Remove("result");
		completion.TrySetResult(result);
	}
}

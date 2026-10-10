using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace Strada.Tool.Editor;

/// <summary>Which editor strada works with, and how it starts one.</summary>
internal sealed class EditorSessionOptions
{
	/// <summary>The executable to start (--editor); null looks for it (<see cref="EditorLocator"/>).</summary>
	public string? EditorPath { get; init; }
	/// <summary>A project file (--project): the editor that has it open, or a started editor opens it.</summary>
	public string? Project { get; init; }
	/// <summary>Only the running editor with this process ID (--pid); nothing is started.</summary>
	public int? ProcessId { get; init; }
	/// <summary>Start editors without a window (--headless).</summary>
	public bool Headless { get; init; }
	/// <summary>Never use an editor this session did not start (--new).</summary>
	public bool StartNew { get; init; }
	/// <summary>Where editors write their instance files; null uses this process's user data directory.</summary>
	public string? InstanceDirectory { get; init; }
	/// <summary>Environment variables of the editors this session starts.</summary>
	public IReadOnlyDictionary<string, string> EditorEnvironment { get; init; } = new Dictionary<string, string>();
	public TimeSpan LaunchTimeout { get; init; } = TimeSpan.FromSeconds(60);
	/// <summary>Where strada is: the editor is looked for next to it.</summary>
	public string ToolDirectory { get; init; } = AppContext.BaseDirectory;
	public string WorkingDirectory { get; init; } = Environment.CurrentDirectory;
}

/// <summary>The editor strada works with: a running editor that fits the options, or one it starts. A connection that
/// broke is replaced on the next use. Editors started headless close with the session (they also get --parent-process,
/// in case strada ends without closing them); windowed ones stay open for the user. Thread-safe.</summary>
internal sealed class EditorSession : IAsyncDisposable
{
	private static readonly TimeSpan s_QuitTimeout = TimeSpan.FromSeconds(15);

	private readonly EditorSessionOptions m_Options;
	private readonly SemaphoreSlim m_Lock = new(1, 1);
	private readonly List<LaunchedEditor> m_Launched = [];
	private EditorConnection? m_Connection;

	public EditorSession(EditorSessionOptions options)
	{
		m_Options = options;
	}

	/// <summary>The connection to the editor: attaches to a running editor that fits, else starts one (except with
	/// <see cref="EditorSessionOptions.ProcessId"/>). Throws <see cref="EditorUnavailableException"/> when neither works.</summary>
	public async Task<EditorConnection> ConnectAsync(CancellationToken cancellationToken)
	{
		await m_Lock.WaitAsync(cancellationToken);
		try
		{
			if (m_Connection is { IsOpen: true })
			{
				return m_Connection;
			}
			await CloseConnectionAsync();
			m_Connection = await AttachAsync(cancellationToken) ?? await LaunchAsync(cancellationToken);
			return m_Connection;
		}
		finally
		{
			m_Lock.Release();
		}
	}

	/// <summary>The connection to a running editor that fits, or null when there is none; nothing is started.</summary>
	public async Task<EditorConnection?> TryAttachAsync(CancellationToken cancellationToken)
	{
		await m_Lock.WaitAsync(cancellationToken);
		try
		{
			if (m_Connection is { IsOpen: true })
			{
				return m_Connection;
			}
			await CloseConnectionAsync();
			m_Connection = await AttachAsync(cancellationToken);
			return m_Connection;
		}
		finally
		{
			m_Lock.Release();
		}
	}

	/// <summary>The editor's command descriptions (editor.commands), read without changing anything the user sees: from
	/// the editor in use or a running one that fits, else from a headless editor started for this and closed again (or
	/// kept, when the session starts headless editors anyway).</summary>
	public async Task<JsonArray> GetCommandsAsync(CancellationToken cancellationToken)
	{
		EditorConnection? connection = m_Options.Headless ? await ConnectAsync(cancellationToken) : await TryAttachAsync(cancellationToken);
		if (connection is not null)
		{
			return ToCommandList(await connection.CallAsync("editor.commands", null, cancellationToken));
		}

		LaunchedEditor editor = await EditorLauncher.LaunchAsync(CreateLaunchOptions(headless: true, project: null), cancellationToken);
		try
		{
			EditorConnection temporary = await EditorConnection.ConnectAsync(editor.Instance, cancellationToken);
			await using (temporary)
			{
				JsonArray commands = ToCommandList(await temporary.CallAsync("editor.commands", null, cancellationToken));
				await QuitAsync(temporary);
				return commands;
			}
		}
		finally
		{
			await editor.WaitForExitAsync(s_QuitTimeout);
			editor.Dispose();
		}
	}

	public async ValueTask DisposeAsync()
	{
		await m_Lock.WaitAsync();
		try
		{
			await CloseConnectionAsync();
			foreach (LaunchedEditor editor in m_Launched)
			{
				if (m_Options.Headless && !editor.HasExited)
				{
					await StopAsync(editor);
				}
				editor.Dispose();
			}
			m_Launched.Clear();
		}
		finally
		{
			m_Lock.Release();
		}
	}

	private string InstanceDirectory => m_Options.InstanceDirectory ?? UserDataDirectory.GetInstanceDirectory() ??
		throw new EditorUnavailableException("there is no user data directory to find editors in (HOME, or APPDATA on Windows, is not set)");

	private async Task<EditorConnection?> AttachAsync(CancellationToken cancellationToken)
	{
		HashSet<int> launched = m_Launched.Where(editor => !editor.HasExited).Select(editor => editor.ProcessId).ToHashSet();
		// Editors this session started come first: a broken connection is replaced by one to the same editor.
		List<EditorInstance> candidates = EditorInstance.FindRunning(InstanceDirectory)
			.Where(instance => Fits(instance) && (!m_Options.StartNew || launched.Contains(instance.ProcessId)))
			.OrderBy(instance => launched.Contains(instance.ProcessId) ? 0 : 1)
			.ToList();

		Exception? failure = null;
		foreach (EditorInstance instance in candidates)
		{
			try
			{
				return await EditorConnection.ConnectAsync(instance, cancellationToken);
			}
			catch (Exception exception) when (exception is EditorUnavailableException or EditorCommandException)
			{
				// Its server is gone or refuses connections (busy, or the file is out of date): try the next one.
				failure = exception;
			}
		}
		if (m_Options.ProcessId is { } processId)
		{
			throw failure is null
				? new EditorUnavailableException($"no editor with process ID {processId} is running")
				: new EditorUnavailableException($"cannot use the editor with process ID {processId}: {failure.Message}", failure);
		}
		return null;
	}

	private bool Fits(EditorInstance instance)
	{
		if (m_Options.ProcessId is { } processId)
		{
			return instance.ProcessId == processId;
		}
		return m_Options.Project is not { } project || instance.HasProject(project);
	}

	private async Task<EditorConnection> LaunchAsync(CancellationToken cancellationToken)
	{
		LaunchedEditor editor =
			await EditorLauncher.LaunchAsync(CreateLaunchOptions(m_Options.Headless, m_Options.Project), cancellationToken);
		m_Launched.Add(editor);
		EditorConnection connection;
		try
		{
			connection = await EditorConnection.ConnectAsync(editor.Instance, cancellationToken);
		}
		catch (Exception exception) when (exception is EditorUnavailableException or EditorCommandException)
		{
			throw new EditorUnavailableException($"cannot connect to the editor that was started: {exception.Message}{editor.DescribeOutput()}",
												 exception);
		}

		if (m_Options.Project is { } project && !editor.Instance.HasProject(project))
		{
			// It runs without the project, which is of no use here: it reports why in its log.
			string reason = await ReadLastErrorAsync(connection, cancellationToken) ?? "see the editor's log";
			await QuitAsync(connection);
			await connection.DisposeAsync();
			await editor.WaitForExitAsync(s_QuitTimeout);
			m_Launched.Remove(editor);
			editor.Dispose();
			throw new EditorUnavailableException($"the editor could not open the project '{project}': {reason}");
		}
		return connection;
	}

	private EditorLaunchOptions CreateLaunchOptions(bool headless, string? project) => new()
	{
		Executable = EditorLocator.Find(m_Options.EditorPath, Environment.GetEnvironmentVariable, m_Options.ToolDirectory,
										m_Options.WorkingDirectory),
		InstanceDirectory = InstanceDirectory,
		Headless = headless,
		Project = project is null ? null : Path.GetFullPath(project),
		// Without a window nobody can close it: it ends with strada.
		ParentProcessId = headless ? Environment.ProcessId : null,
		Environment = m_Options.EditorEnvironment,
		Timeout = m_Options.LaunchTimeout,
	};

	private async Task CloseConnectionAsync()
	{
		if (m_Connection is not null)
		{
			await m_Connection.DisposeAsync();
			m_Connection = null;
		}
	}

	private static async Task StopAsync(LaunchedEditor editor)
	{
		try
		{
			EditorConnection connection = await EditorConnection.ConnectAsync(editor.Instance, CancellationToken.None);
			await using (connection)
			{
				await QuitAsync(connection);
			}
		}
		catch (Exception exception) when (exception is EditorUnavailableException or EditorCommandException)
		{
			// It is ended below.
		}
		await editor.WaitForExitAsync(s_QuitTimeout);
	}

	// Closes the editor without asking about unsaved changes. The editor answers before it exits, or the connection ends first.
	private static async Task QuitAsync(EditorConnection connection)
	{
		using CancellationTokenSource timeout = new(s_QuitTimeout);
		try
		{
			await connection.CallAsync("editor.quit", new JsonObject { ["discardChanges"] = true }, timeout.Token);
		}
		catch (Exception exception) when (exception is EditorUnavailableException or EditorCommandException or OperationCanceledException)
		{
			// Whoever waits for the process ends it when it does not exit.
		}
	}

	private static async Task<string?> ReadLastErrorAsync(EditorConnection connection, CancellationToken cancellationToken)
	{
		try
		{
			JsonNode? log = await connection.CallAsync("log.read", new JsonObject { ["minLevel"] = "error" }, cancellationToken);
			return (log?["entries"] as JsonArray)?.LastOrDefault()?["message"]?.GetValue<string>();
		}
		catch (Exception exception) when (exception is EditorUnavailableException or EditorCommandException or InvalidOperationException)
		{
			return null;
		}
	}

	private static JsonArray ToCommandList(JsonNode? result) =>
		result as JsonArray ?? throw new EditorUnavailableException("the editor's editor.commands answer is not a list of commands");
}

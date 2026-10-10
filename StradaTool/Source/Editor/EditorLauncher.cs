using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace Strada.Tool.Editor;

/// <summary>How to start an editor.</summary>
internal sealed class EditorLaunchOptions
{
	public required string Executable { get; init; }
	/// <summary>Where the editor writes its instance file: the instance directory of its environment.</summary>
	public required string InstanceDirectory { get; init; }
	public bool Headless { get; init; }
	/// <summary>A project file the editor opens at startup.</summary>
	public string? Project { get; init; }
	/// <summary>The editor closes once this process has exited (its --parent-process option).</summary>
	public int? ParentProcessId { get; init; }
	/// <summary>Environment variables the editor gets in addition to this process's.</summary>
	public IReadOnlyDictionary<string, string> Environment { get; init; } = new Dictionary<string, string>();
	/// <summary>How long the editor may take to start its automation server.</summary>
	public TimeSpan Timeout { get; init; } = TimeSpan.FromSeconds(60);
}

/// <summary>Starts editors and waits for their automation servers.</summary>
internal static class EditorLauncher
{
	private static readonly TimeSpan s_PollInterval = TimeSpan.FromMilliseconds(100);
	// Instance files written this long before the start are left over from an earlier process that had the same ID.
	private static readonly TimeSpan s_ClockTolerance = TimeSpan.FromSeconds(2);

	/// <summary>Starts the editor and returns once its automation server listens (its instance file exists). Throws
	/// <see cref="EditorUnavailableException"/> when it cannot start, exits first or takes longer than the timeout (then
	/// it is ended).</summary>
	public static async Task<LaunchedEditor> LaunchAsync(EditorLaunchOptions options, CancellationToken cancellationToken)
	{
		ProcessStartInfo startInfo = new(options.Executable)
		{
			UseShellExecute = false,
			// Pipes of its own: the editor must not hold on to strada's standard streams (the MCP channel). Editors ignore
			// writes to pipes that closed (SIGPIPE), so the ones that stay open outlive strada.
			RedirectStandardInput = true,
			RedirectStandardOutput = true,
			RedirectStandardError = true,
			CreateNoWindow = true,
		};
		if (options.Headless)
		{
			startInfo.ArgumentList.Add("--headless");
		}
		if (options.Project is not null)
		{
			startInfo.ArgumentList.Add("--project");
			startInfo.ArgumentList.Add(options.Project);
		}
		if (options.ParentProcessId is { } parent)
		{
			startInfo.ArgumentList.Add("--parent-process");
			startInfo.ArgumentList.Add(parent.ToString(CultureInfo.InvariantCulture));
		}
		foreach ((string name, string value) in options.Environment)
		{
			startInfo.Environment[name] = value;
		}

		EditorOutput output = new();
		DateTime started = DateTime.UtcNow;
		Process process = new() { StartInfo = startInfo };
		process.OutputDataReceived += (_, line) => output.Add(line.Data);
		process.ErrorDataReceived += (_, line) => output.Add(line.Data);
		try
		{
			process.Start();
		}
		catch (Win32Exception exception)
		{
			process.Dispose();
			throw new EditorUnavailableException($"cannot start '{options.Executable}': {exception.Message}", exception);
		}
		process.BeginOutputReadLine();
		process.BeginErrorReadLine();
		process.StandardInput.Close();

		LaunchedEditor? launched = null;
		try
		{
			launched = new LaunchedEditor(process, output, await WaitForInstanceAsync(process, output, options, started, cancellationToken));
			return launched;
		}
		finally
		{
			if (launched is null)
			{
				Kill(process);
				process.Dispose();
			}
		}
	}

	private static async Task<EditorInstance> WaitForInstanceAsync(Process process, EditorOutput output, EditorLaunchOptions options,
		DateTime started, CancellationToken cancellationToken)
	{
		string file = EditorInstance.GetPath(options.InstanceDirectory, process.Id);
		Stopwatch elapsed = Stopwatch.StartNew();
		while (true)
		{
			if (process.HasExited)
			{
				throw new EditorUnavailableException(
					$"the editor exited (code {process.ExitCode}) before its automation server started{output.Describe()}");
			}
			if (TryReadInstance(file, process.Id, started - s_ClockTolerance) is { } instance)
			{
				return instance;
			}
			if (elapsed.Elapsed > options.Timeout)
			{
				throw new EditorUnavailableException(
					$"the editor did not start its automation server within {options.Timeout.TotalSeconds:0} seconds{output.Describe()}");
			}
			await Task.Delay(s_PollInterval, cancellationToken);
		}
	}

	private static EditorInstance? TryReadInstance(string file, int processId, DateTime notBefore)
	{
		try
		{
			if (!File.Exists(file) || File.GetLastWriteTimeUtc(file) < notBefore)
			{
				return null;
			}
			// Editors write the file atomically: it is complete once it exists.
			EditorInstance instance = EditorInstance.Parse(File.ReadAllText(file));
			return instance.ProcessId == processId ? instance : null;
		}
		catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidDataException)
		{
			return null;
		}
	}

	internal static void Kill(Process process)
	{
		try
		{
			if (!process.HasExited)
			{
				process.Kill(entireProcessTree: true);
			}
		}
		catch (Exception exception) when (exception is InvalidOperationException or Win32Exception or NotSupportedException)
		{
			// It exited in the meantime, or cannot be ended by this user.
		}
	}
}

/// <summary>An editor that strada started: its process and the automation endpoint it published.</summary>
internal sealed class LaunchedEditor : IDisposable
{
	private readonly Process m_Process;
	private readonly EditorOutput m_Output;

	public LaunchedEditor(Process process, EditorOutput output, EditorInstance instance)
	{
		m_Process = process;
		m_Output = output;
		Instance = instance;
	}

	public EditorInstance Instance { get; }

	public int ProcessId => Instance.ProcessId;

	public bool HasExited
	{
		get
		{
			try
			{
				return m_Process.HasExited;
			}
			catch (InvalidOperationException)
			{
				return true;
			}
		}
	}

	/// <summary>The last lines the editor wrote to its console, for reports about it.</summary>
	public string DescribeOutput() => m_Output.Describe();

	/// <summary>Waits for the editor to exit, and ends it when it takes longer than <paramref name="timeout"/>.</summary>
	public async Task WaitForExitAsync(TimeSpan timeout)
	{
		using CancellationTokenSource timer = new(timeout);
		try
		{
			await m_Process.WaitForExitAsync(timer.Token);
		}
		catch (OperationCanceledException)
		{
			EditorLauncher.Kill(m_Process);
			await m_Process.WaitForExitAsync(CancellationToken.None);
		}
	}

	/// <summary>Releases strada's handle; the editor keeps running.</summary>
	public void Dispose() => m_Process.Dispose();
}

/// <summary>The last lines an editor wrote to its standard output and error, kept for reports when it fails.</summary>
internal sealed class EditorOutput
{
	private const int MaxLines = 40;

	private readonly Queue<string> m_Lines = new();
	private readonly Lock m_Lock = new();

	public void Add(string? line)
	{
		if (line is null)
		{
			return;
		}
		lock (m_Lock)
		{
			m_Lines.Enqueue(line);
			while (m_Lines.Count > MaxLines)
			{
				m_Lines.Dequeue();
			}
		}
	}

	/// <summary>"\nThe editor's last output:\n..." or empty when it wrote nothing.</summary>
	public string Describe()
	{
		lock (m_Lock)
		{
			return m_Lines.Count == 0 ? "" : $"\nThe editor's last output:\n{string.Join("\n", m_Lines)}";
		}
	}
}

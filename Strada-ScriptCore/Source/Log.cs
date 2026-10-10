using Strada.Interop;

namespace Strada;

/// <summary>Writes messages to the engine's script log: the editor console, the terminal and the log file.</summary>
public static class Log
{
	// The engine's LogLevel values.
	internal const int TraceLevel = 0;
	internal const int InfoLevel = 1;
	internal const int WarnLevel = 2;
	internal const int ErrorLevel = 3;

	/// <summary>Logs a detailed diagnostic message.</summary>
	public static void Trace(string message) => Write(TraceLevel, message);

	/// <summary>Logs an informational message.</summary>
	public static void Info(string message) => Write(InfoLevel, message);

	/// <summary>Logs a warning.</summary>
	public static void Warn(string message) => Write(WarnLevel, message);

	/// <summary>Logs an error.</summary>
	public static void Error(string message) => Write(ErrorLevel, message);

	internal static unsafe void Write(int level, string message)
	{
		byte[] text = NativeString.ToUtf8(message);
		fixed (byte* bytes = text)
		{
			InternalCalls.Log_Write(level, bytes, text.Length);
		}
	}
}

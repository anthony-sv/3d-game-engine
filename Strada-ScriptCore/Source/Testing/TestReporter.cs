using System;
using Strada.Interop;

namespace Strada.Testing;

/// <summary>Reports the results of checks to the application running the scene: the game player's test mode
/// (<c>--test</c>) exits with the number of failures, and the editor's test commands return them. Results are logged
/// too.</summary>
public static unsafe class TestReporter
{
	/// <summary>Records a passed check.</summary>
	public static void Pass(string name)
	{
		Report(name, true, string.Empty);
	}

	/// <summary>Records a failed check and why it failed.</summary>
	public static void Fail(string name, string message)
	{
		Report(name, false, message);
	}

	/// <summary>Runs a check, which passes unless it throws (assertion failures report their message, other exceptions
	/// their type, message and stack trace). Returns whether it passed.</summary>
	public static bool Run(string name, Action check)
	{
		ArgumentNullException.ThrowIfNull(check);
		try
		{
			check();
		}
		catch (AssertionException exception)
		{
			Fail(name, exception.Message);
			return false;
		}
		catch (Exception exception)
		{
			Fail(name, exception.ToString());
			return false;
		}
		Pass(name);
		return true;
	}

	/// <summary>Ends the test run: every check has reported.</summary>
	public static void Finish()
	{
		InternalCalls.TestReporter_Finish();
	}

	private static void Report(string name, bool passed, string message)
	{
		byte[] nameText = NativeString.ToUtf8(name);
		byte[] messageText = NativeString.ToUtf8(message);
		fixed (byte* nameBytes = nameText)
		fixed (byte* messageBytes = messageText)
		{
			InternalCalls.TestReporter_Report(nameBytes, nameText.Length, passed ? (byte)1 : (byte)0, messageBytes, messageText.Length);
		}
	}
}

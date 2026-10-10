using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;

namespace Strada.Testing;

/// <summary>Thrown by <see cref="Assert"/> when a check fails; <see cref="TestReporter.Run"/> reports its message.</summary>
public sealed class AssertionException : Exception
{
	internal AssertionException(string message)
		: base(message)
	{
	}
}

/// <summary>Checks for tests. Each throws an <see cref="AssertionException"/> describing the failure, followed by
/// <c>message</c> when one is given.</summary>
public static class Assert
{
	/// <summary>Fails unless <paramref name="condition"/> is true.</summary>
	public static void IsTrue([DoesNotReturnIf(false)] bool condition, string message = "")
	{
		if (!condition)
		{
			throw Failure("expected true", message);
		}
	}

	/// <summary>Fails unless <paramref name="condition"/> is false.</summary>
	public static void IsFalse([DoesNotReturnIf(true)] bool condition, string message = "")
	{
		if (condition)
		{
			throw Failure("expected false", message);
		}
	}

	/// <summary>Fails unless the values are equal (by <see cref="EqualityComparer{T}.Default"/>).</summary>
	public static void AreEqual<T>(T expected, T actual, string message = "")
	{
		if (!EqualityComparer<T>.Default.Equals(expected, actual))
		{
			throw Failure(Format($"expected {expected}, got {actual}"), message);
		}
	}

	/// <summary>Fails when the values are equal (by <see cref="EqualityComparer{T}.Default"/>).</summary>
	public static void AreNotEqual<T>(T notExpected, T actual, string message = "")
	{
		if (EqualityComparer<T>.Default.Equals(notExpected, actual))
		{
			throw Failure(Format($"expected anything but {notExpected}"), message);
		}
	}

	/// <summary>Fails unless the numbers differ by at most <paramref name="tolerance"/>.</summary>
	public static void AreApproximatelyEqual(float expected, float actual, float tolerance = 1e-5f, string message = "")
	{
		if (!(MathF.Abs(expected - actual) <= tolerance))
		{
			throw Failure(Format($"expected {expected} (within {tolerance}), got {actual}"), message);
		}
	}

	/// <summary>Fails unless the vectors are at most <paramref name="tolerance"/> apart.</summary>
	public static void AreApproximatelyEqual(Vector3 expected, Vector3 actual, float tolerance = 1e-5f, string message = "")
	{
		if (!(Vector3.Distance(expected, actual) <= tolerance))
		{
			throw Failure(Format($"expected {expected} (within {tolerance}), got {actual}"), message);
		}
	}

	/// <summary>Fails unless <paramref name="value"/> is null.</summary>
	public static void IsNull(object? value, string message = "")
	{
		if (value is not null)
		{
			throw Failure(Format($"expected null, got {value}"), message);
		}
	}

	/// <summary>Fails when <paramref name="value"/> is null.</summary>
	public static void IsNotNull([NotNull] object? value, string message = "")
	{
		if (value is null)
		{
			throw Failure("expected a value, got null", message);
		}
	}

	/// <summary>Fails unless <paramref name="action"/> throws a <typeparamref name="TException"/> (or a derived
	/// exception), which it returns.</summary>
	public static TException Throws<TException>(Action action, string message = "")
		where TException : Exception
	{
		ArgumentNullException.ThrowIfNull(action);
		try
		{
			action();
		}
		catch (TException exception)
		{
			return exception;
		}
		catch (Exception exception)
		{
			throw Failure($"expected {typeof(TException).Name}, got {exception.GetType().Name}: {exception.Message}", message);
		}
		throw Failure($"expected {typeof(TException).Name}, nothing was thrown", message);
	}

	/// <summary>Fails with <paramref name="message"/>.</summary>
	[DoesNotReturn]
	public static void Fail(string message)
	{
		throw new AssertionException(message);
	}

	private static string Format(FormattableString text) => text.ToString(CultureInfo.InvariantCulture);

	private static AssertionException Failure(string problem, string message) =>
		new(string.IsNullOrEmpty(message) ? problem : $"{problem}: {message}");
}

using System;
using System.Globalization;
using System.Threading;
using Strada.Testing;
using Xunit;
using Check = Strada.Testing.Assert;

namespace Strada.ScriptCore.Tests;

public sealed class TestingTests
{
	private static string FailureOf(Action check) => Xunit.Assert.Throws<AssertionException>(check).Message;

	[Fact]
	public void ConditionsFailWithTheirMessage()
	{
		Check.IsTrue(true);
		Check.IsFalse(false);
		Xunit.Assert.Equal("expected true: the door opens", FailureOf(() => Check.IsTrue(false, "the door opens")));
		Xunit.Assert.Equal("expected false", FailureOf(() => Check.IsFalse(true)));
		Xunit.Assert.Equal("gave up", FailureOf(() => Check.Fail("gave up")));
	}

	[Fact]
	public void EqualityChecksDescribeBothValues()
	{
		Check.AreEqual(new Vector3(1.0f, 2.0f, 3.0f), new Vector3(1.0f, 2.0f, 3.0f));
		Check.AreNotEqual("a", "b");
		Check.AreEqual<string?>(null, null);
		Xunit.Assert.Equal("expected 1, got 2: count", FailureOf(() => Check.AreEqual(1, 2, "count")));
		Xunit.Assert.Equal("expected anything but 4", FailureOf(() => Check.AreNotEqual(4, 4)));

		CultureInfo culture = Thread.CurrentThread.CurrentCulture;
		try
		{
			Thread.CurrentThread.CurrentCulture = new CultureInfo("de-DE");
			Xunit.Assert.Equal("expected 1.5, got 2.5", FailureOf(() => Check.AreEqual(1.5f, 2.5f)));
		}
		finally
		{
			Thread.CurrentThread.CurrentCulture = culture;
		}
	}

	[Fact]
	public void ApproximateChecksUseTheTolerance()
	{
		Check.AreApproximatelyEqual(1.0f, 1.000001f);
		Check.AreApproximatelyEqual(1.0f, 1.05f, 0.1f);
		Check.AreApproximatelyEqual(Vector3.One, new Vector3(1.0f, 1.0f, 1.000001f));
		Xunit.Assert.Equal("expected 1 (within 0.01), got 1.5", FailureOf(() => Check.AreApproximatelyEqual(1.0f, 1.5f, 0.01f)));
		Xunit.Assert.StartsWith("expected 1 (within", FailureOf(() => Check.AreApproximatelyEqual(1.0f, float.NaN)));
		Xunit.Assert.Equal("expected (0, 0, 0) (within 1E-05), got (0, 1, 0)",
			FailureOf(() => Check.AreApproximatelyEqual(Vector3.Zero, Vector3.Up)));
	}

	[Fact]
	public void NullChecks()
	{
		Check.IsNull(null);
		Check.IsNotNull(new object());
		Xunit.Assert.Equal("expected null, got Entity(3)", FailureOf(() => Check.IsNull(new Entity(3))));
		Xunit.Assert.Equal("expected a value, got null: the player", FailureOf(() => Check.IsNotNull(null, "the player")));
	}

	[Fact]
	public void ThrowsReturnsTheExpectedException()
	{
		InvalidOperationException thrown = Check.Throws<InvalidOperationException>(() => throw new InvalidOperationException("no"));
		Xunit.Assert.Equal("no", thrown.Message);
		Check.Throws<ArgumentException>(() => throw new ArgumentNullException("value"));
		Xunit.Assert.Equal("expected InvalidOperationException, nothing was thrown",
			FailureOf(() => Check.Throws<InvalidOperationException>(() => { })));
		Xunit.Assert.Equal("expected InvalidOperationException, got FormatException: bad",
			FailureOf(() => Check.Throws<InvalidOperationException>(() => throw new FormatException("bad"))));
	}
}

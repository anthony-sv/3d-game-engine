using System;
using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Strada.Testing itself: the assertions, and reporting results directly.</summary>
public sealed class TestingApiTests : FeatureTestScript
{
	protected override void Start()
	{
		Check("assertions that hold pass", () =>
		{
			Assert.IsTrue(true);
			Assert.IsFalse(false);
			Assert.AreEqual(4, 2 + 2);
			Assert.AreNotEqual("left", "right");
			Assert.AreApproximatelyEqual(1.0f, 1.000001f);
			Assert.AreApproximatelyEqual(Vector3.One, new Vector3(1.000001f));
			Assert.IsNull(null);
			Assert.IsNotNull(this);
		});
		Check("assertions that do not hold throw with their message", () =>
		{
			AssertionException failure = Assert.Throws<AssertionException>(() => Assert.Fail("failed on purpose"));
			Assert.IsTrue(failure.Message.Contains("failed on purpose", StringComparison.Ordinal), failure.Message);
			Assert.Throws<AssertionException>(() => Assert.IsTrue(false));
			Assert.Throws<AssertionException>(() => Assert.AreEqual(1, 2));
			Assert.Throws<AssertionException>(() => Assert.AreApproximatelyEqual(1.0f, 2.0f, 0.5f, "too far"));
			Assert.Throws<AssertionException>(() => Assert.IsNotNull(null, "nothing"));
			// Throws fails when nothing (or something else) is thrown.
			Assert.Throws<AssertionException>(() => Assert.Throws<InvalidOperationException>(() => { }));
		});

		// Results can be reported without Run; Fail reports what went wrong.
		const string checkName = "TestingApiTests: results are reported directly";
		if (Check("Run reports whether the check passed", () => { }))
		{
			TestReporter.Pass(checkName);
		}
		else
		{
			TestReporter.Fail(checkName, "TestReporter.Run reported a failure for a check that passed");
		}
	}
}

using Strada.Testing;

namespace Strada.Tests;

// Checks itself and finishes the test run in its first frame; with Fail set, one check fails.
public sealed class SelfTest : Script
{
	public bool Fail;

	protected override void OnUpdate(float deltaTime)
	{
		TestReporter.Run("frame runs", () => Assert.IsTrue(deltaTime > 0.0f));
		TestReporter.Run("fails on request", () => Assert.IsFalse(Fail, "Fail is set"));
		TestReporter.Finish();
	}
}

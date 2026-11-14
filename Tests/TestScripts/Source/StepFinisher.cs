using Strada.Testing;

namespace Strada.Tests;

// Finishes the test run once Steps fixed steps have run (in the fixed update after them), whatever the frame rate, so
// that runs at different time steps all reach the same step.
public sealed class StepFinisher : Script
{
	public int Steps = 60;

	private int m_FixedUpdates;
	private bool m_Finished;

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		m_FixedUpdates++;
		if (m_Finished || m_FixedUpdates <= Steps)
		{
			return;
		}
		m_Finished = true;
		TestReporter.Run("steps run", () => Assert.IsTrue(fixedDeltaTime > 0.0f));
		TestReporter.Finish();
	}
}

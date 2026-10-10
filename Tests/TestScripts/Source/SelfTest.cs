using Strada.Testing;

namespace Strada.Tests;

// Checks itself once, in its first frame, then finishes the test run, or instead loads NextScene (whose scripts go on) or
// quits when asked to. With Fail set one check fails; with an expected window size the view's size is checked too.
public sealed class SelfTest : Script
{
	public bool Fail;
	public bool Quit;
	public string NextScene = "";
	public int ExpectedWindowWidth;
	public int ExpectedWindowHeight;

	private bool m_Checked;

	protected override void OnUpdate(float deltaTime)
	{
		if (m_Checked)
		{
			return;
		}
		m_Checked = true;
		TestReporter.Run("frame runs", () => Assert.IsTrue(deltaTime > 0.0f));
		TestReporter.Run("fails on request", () => Assert.IsFalse(Fail, "Fail is set"));
		if (ExpectedWindowWidth > 0)
		{
			TestReporter.Run("window size", () =>
			{
				Assert.AreEqual(ExpectedWindowWidth, Application.WindowWidth);
				Assert.AreEqual(ExpectedWindowHeight, Application.WindowHeight);
			});
		}

		if (NextScene.Length > 0)
		{
			SceneManager.LoadScene(NextScene);
		}
		else if (Quit)
		{
			Application.Quit();
		}
		else
		{
			TestReporter.Finish();
		}
	}
}

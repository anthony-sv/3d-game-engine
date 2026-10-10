using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Loads the next scene once every feature test of this one is done; that scene finishes the test run.</summary>
public sealed class Coordinator : Script
{
	public string NextScene = "Scenes/Second.sscene";

	private bool m_Loading;

	protected override void OnUpdate(float deltaTime)
	{
		if (m_Loading || !Progress.AllDone)
		{
			return;
		}
		m_Loading = true;
		TestReporter.Run("Coordinator: the next scene loads", () => Assert.IsTrue(SceneManager.LoadScene(NextScene), NextScene));
	}
}

/// <summary>The second scene: SceneManager.LoadScene replaced the first one, and the test run ends here.</summary>
public sealed class SecondScene : Script
{
	protected override void OnCreate()
	{
		TestReporter.Run("SecondScene: the scene the scripts asked for runs",
			() => Assert.AreEqual("Second", SceneManager.CurrentSceneName));
		TestReporter.Finish();
		// The player stops after this frame; the editor stops playing.
		Application.Quit();
	}
}

using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Application, Time, Log, Debug and SceneManager.</summary>
public sealed class ApplicationTests : FeatureTestScript
{
	private double m_Elapsed;
	private ulong m_Frames;
	private float m_TimeScale;

	protected override void Start()
	{
		Check("the application describes the view and the scene", () =>
		{
			// The editor's game view or the player's window (its configured size when headless).
			Assert.IsTrue(Application.WindowWidth > 0 && Application.WindowHeight > 0,
				$"{Application.WindowWidth}x{Application.WindowHeight}");
			Assert.AreEqual("FeatureTest", SceneManager.CurrentSceneName);
			// The same scene runs in both: CI uses the player, people play it in the editor.
			Log.Info(Application.IsEditor ? "FeatureTest: playing in the editor" : "FeatureTest: running in the game player");
		});
		Check("unknown scenes are not loaded", () => Assert.IsFalse(SceneManager.LoadScene("Scenes/Missing.sscene")));
		Check("messages are logged at every level", () =>
		{
			Log.Trace("FeatureTest: a trace message");
			Log.Info("FeatureTest: an info message");
			Log.Warn("FeatureTest: a warning, on purpose");
			Log.Error("FeatureTest: an error message, on purpose");
		});
		Check("debug lines can be drawn", () =>
		{
			Debug.DrawLine(Vector3.Zero, Vector3.Up, Color.Green);
			Debug.DrawLine(Vector3.Zero, Vector3.Right, Color.Red, 0.5f);
		});
		Check("the fixed time step is the project's", () => Assert.AreApproximatelyEqual(1.0f / 60.0f, Time.FixedDeltaTime, 1e-6f));
		m_Elapsed = Time.Elapsed;
		m_Frames = Time.FrameCount;
		m_TimeScale = Time.TimeScale;
	}

	protected override bool Step(int frame, float deltaTime)
	{
		switch (frame)
		{
			case 1:
				Check("time advances every frame", () =>
				{
					Assert.AreEqual(deltaTime, Time.DeltaTime);
					Assert.IsTrue(deltaTime > 0.0f);
					Assert.IsTrue(Time.Elapsed > m_Elapsed, $"{Time.Elapsed} after {m_Elapsed}");
					Assert.IsTrue(Time.FrameCount > m_Frames, $"{Time.FrameCount} after {m_Frames}");
				});
				m_Elapsed = Time.Elapsed;
				Time.TimeScale = 0.5f;
				return false;
			case 2:
				// The engine's tests check the scaling itself: frame times vary when people play the scene.
				Check("the time scale is kept and elapsed time adds up the frame times", () =>
				{
					Assert.AreEqual(0.5f, Time.TimeScale);
					Assert.AreEqual(deltaTime, Time.DeltaTime);
					Assert.AreApproximatelyEqual((float)(m_Elapsed + deltaTime), (float)Time.Elapsed);
				});
				Time.TimeScale = m_TimeScale;
				return true;
			default:
				return true;
		}
	}
}

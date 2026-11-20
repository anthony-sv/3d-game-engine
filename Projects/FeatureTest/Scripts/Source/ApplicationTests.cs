using System;
using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Application, Time, Log, Debug, SceneManager, Telemetry, SaveSlot and Settings. Its fixed updates ride three laps for telemetry, as a
/// race does: FeatureTest.Distance (metres along a lap of LapLength) every step, marks Lap 1 to Lap 4 where each lap begins and
/// the last ends, then Lap end; the laps take 24, 18 and 30 steps, so the second is the best. The player records nothing,
/// and the editor's telemetry tests read what it rode.</summary>
public sealed class ApplicationTests : FeatureTestScript
{
	private const float LapLength = 120.0f;
	private static readonly int[] s_LapSteps = [24, 18, 30];

	private double m_Elapsed;
	private ulong m_Frames;
	private float m_TimeScale;
	private int m_FixedSteps;
	private int m_Lap;
	private int m_LapStart;

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
		Check("telemetry refuses null names, and records only in the editor", () =>
		{
			Assert.Throws<ArgumentNullException>(() => Telemetry.Record(null!, 1.0f));
			Assert.Throws<ArgumentNullException>(() => Telemetry.Mark(null!));
			Assert.Throws<ArgumentNullException>(() => Telemetry.MarkStep(null!, 1));
			// The game player never records; the editor records while its Telemetry panel is open or an agent's recording runs.
			Assert.IsTrue(Application.IsEditor || !Telemetry.IsRecording, "the game player records telemetry");
		});
		CheckSaveSlots();
		CheckSettings();
		m_Elapsed = Time.Elapsed;
		m_Frames = Time.FrameCount;
		m_TimeScale = Time.TimeScale;
	}

	// A slot of its own, deleted at the end, so that every run (a test run's temporary folder, play mode's play data) starts
	// from none. Without a game data folder (a scene run by the engine's tests alone) the slot is Unavailable and keeps nothing.
	private void CheckSaveSlots()
	{
		Check("a save slot keeps its values through a save and refuses what it cannot hold", () =>
		{
			SaveSlot slot = SaveSlot.Open("FeatureTest");
			Assert.AreEqual("featuretest", slot.Name);
			if (slot.Status == SaveSlotStatus.Unavailable)
			{
				slot.Set("Best", 1.0f);
				Assert.IsFalse(slot.Save());
				Assert.IsFalse(slot.Delete());
				Assert.AreEqual(0, SaveSlot.GetNames().Length);
				return;
			}
			Assert.IsTrue(slot.Delete());
			Assert.AreEqual(SaveSlotStatus.Missing, slot.Status);
			slot.Set("Best", 92.25f);
			slot.Set("Splits", new[] { 30.5f, 61.25f });
			slot.Set("Line", new[] { new Vector3(1.0f, 0.0f, 2.0f), new Vector3(3.0f, 0.0f, 4.0f) });
			slot.Set("Quality", QualityPreset.Medium);
			slot.Set("Gone", 1);
			Assert.IsTrue(slot.Remove("Gone"));
			Assert.Throws<ArgumentException>(() => slot.Set("Nan", float.NaN));
			Assert.IsTrue(slot.Save());
			Assert.AreEqual(SaveSlotStatus.Present, slot.Status);

			SaveSlot read = SaveSlot.Open("FEATURETEST");
			Assert.AreEqual(SaveSlotStatus.Present, read.Status);
			Assert.AreEqual(92.25f, read.Get("Best", 0.0f));
			Assert.AreEqual(61.25f, read.Get("Splits", Array.Empty<float>())[1]);
			Assert.AreEqual(new Vector3(3.0f, 0.0f, 4.0f), read.Get("Line", Array.Empty<Vector3>())[1]);
			Assert.AreEqual(QualityPreset.Medium, read.Get("Quality", QualityPreset.High));
			Assert.IsTrue(read.Has("Best"));
			Assert.IsFalse(read.Has("Gone"));
			Assert.IsTrue(Array.IndexOf(SaveSlot.GetNames(), "featuretest") >= 0, string.Join(", ", SaveSlot.GetNames()));
			Assert.IsTrue(read.Delete());
			Assert.AreEqual(SaveSlotStatus.Missing, SaveSlot.Open("FeatureTest").Status);
		});
	}

	// Every setting is changed and read back, then restored before the save, so that the settings file holds what the run
	// found and no frame draws at another quality; the window is set to what it is, which asks nothing of the application.
	private void CheckSettings()
	{
		Check("the player's settings apply at once, and are kept without a value of the run's", () =>
		{
			QualityPreset quality = Settings.Quality;
			float master = Settings.MasterVolume;
			float voices = Settings.GetBusVolume("Voices");
			bool muted = Settings.IsBusMuted("Voices");
			Settings.Quality = QualityPreset.Low;
			Assert.AreEqual(QualityPreset.Low, Settings.Quality);
			Settings.MasterVolume = 0.5f;
			Assert.AreEqual(0.5f, Settings.MasterVolume);
			Settings.SetBusVolume("Voices", -6.0f);
			Assert.AreEqual(-6.0f, Settings.GetBusVolume("voices"));
			Settings.SetBusMuted("Voices", !muted);
			Assert.AreEqual(!muted, Settings.IsBusMuted("Voices"));
			Settings.Quality = quality;
			Settings.MasterVolume = master;
			Settings.SetBusVolume("Voices", voices);
			Settings.SetBusMuted("Voices", muted);

			(WindowMode mode, int width, int height, bool vsync) = (Settings.WindowMode, Settings.WindowWidth, Settings.WindowHeight,
				Settings.VSync);
			Assert.IsTrue(width >= 64 && height >= 64, $"{width}x{height}");
			Settings.WindowMode = mode;
			Settings.SetWindowSize(width, height);
			Settings.VSync = vsync;
			Assert.AreEqual((mode, width, height, vsync), (Settings.WindowMode, Settings.WindowWidth, Settings.WindowHeight, Settings.VSync));

			Settings.Set("FeatureTest.LinkedBrakes", true);
			Assert.IsTrue(Settings.Has("FeatureTest.LinkedBrakes"));
			Assert.IsTrue(Settings.Get("FeatureTest.LinkedBrakes", false));
			Assert.IsTrue(Settings.Remove("FeatureTest.LinkedBrakes"));
			Assert.IsFalse(Settings.Has("FeatureTest.LinkedBrakes"));
			Assert.Throws<ArgumentException>(() => Settings.Set("", 1));
			Assert.AreEqual(SaveSlot.Open("FeatureTest").Status != SaveSlotStatus.Unavailable, Settings.Save());
		});
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		m_FixedSteps++;
		if (m_Lap > s_LapSteps.Length)
		{
			return;
		}
		if (m_Lap == 0 || m_FixedSteps - m_LapStart == s_LapSteps[m_Lap - 1])
		{
			// A lap boundary: the lap that begins here, or the finish, which closes the last lap.
			m_Lap++;
			m_LapStart = m_FixedSteps;
			Telemetry.Mark($"Lap {m_Lap}");
			if (m_Lap > s_LapSteps.Length)
			{
				Telemetry.Mark("Lap end");
				return;
			}
		}
		int lapSteps = s_LapSteps[m_Lap - 1];
		Telemetry.Record("FeatureTest.Distance", LapLength * (m_FixedSteps - m_LapStart) / lapSteps);
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

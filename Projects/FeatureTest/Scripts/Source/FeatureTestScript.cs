using System;
using System.Collections.Generic;
using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>A feature test: checks part of the engine through the scripting API. Start runs once, Step every frame until it
/// returns true; the coordinator moves on once every test of the scene is done.</summary>
public abstract class FeatureTestScript : Script
{
	private int m_Frame;
	private bool m_Done;

	protected sealed override void OnCreate()
	{
		Progress.Begin(this);
		Start();
	}

	protected sealed override void OnUpdate(float deltaTime)
	{
		if (m_Done)
		{
			return;
		}
		m_Frame++;
		if (Step(m_Frame, deltaTime))
		{
			m_Done = true;
			Progress.End(this);
		}
	}

	protected sealed override void OnDestroy()
	{
		// Also when the scene stops before the test finished.
		Progress.End(this);
	}

	/// <summary>Checks that need no frames of simulation.</summary>
	protected virtual void Start()
	{
	}

	/// <summary>Runs every frame (from 1 on) until it returns true.</summary>
	protected virtual bool Step(int frame, float deltaTime) => true;

	/// <summary>Reports a check named after the test.</summary>
	protected bool Check(string name, Action check) => TestReporter.Run($"{GetType().Name}: {name}", check);
}

/// <summary>The feature tests of the running scene that are not done yet.</summary>
public static class Progress
{
	private static readonly HashSet<FeatureTestScript> s_Running = [];

	public static bool AllDone => s_Running.Count == 0;

	public static void Begin(FeatureTestScript test) => s_Running.Add(test);

	public static void End(FeatureTestScript test) => s_Running.Remove(test);
}

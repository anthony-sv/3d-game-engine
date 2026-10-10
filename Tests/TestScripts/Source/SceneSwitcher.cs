using System;
using Strada.Testing;

namespace Strada.Tests;

// Reports its scene starting and stopping, and on key presses loads NextScene (L), quits (Q), throws (T) or finishes the
// test run (F).
public sealed class SceneSwitcher : Script
{
	public string NextScene = "";

	protected override void OnCreate()
	{
		TestReporter.Pass($"{SceneManager.CurrentSceneName} started");
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(KeyCode.L))
		{
			SceneManager.LoadScene(NextScene);
		}
		if (Input.IsKeyPressed(KeyCode.Q))
		{
			Application.Quit();
		}
		if (Input.IsKeyPressed(KeyCode.F))
		{
			TestReporter.Finish();
		}
		if (Input.IsKeyPressed(KeyCode.T))
		{
			throw new InvalidOperationException("thrown on purpose");
		}
	}

	protected override void OnDestroy()
	{
		TestReporter.Pass($"{SceneManager.CurrentSceneName} stopped");
	}
}

using Strada.Testing;

namespace Strada.Tests;

// Appends each lifecycle call, contact and click to the name of its Recorder entity (comma separated), so tests can see
// what ran and in which order, and logs its destruction. With Report set it also reports each call as a passed check
// (TestReporter), so a test's host sees each one as it happens.
public sealed class Lifecycle : Script
{
	public string Label = "";
	public Entity? Recorder;
	public bool RecordUpdates = true;
	public bool Report;

	protected override void OnCreate() => Record("Create");

	protected override void OnUpdate(float deltaTime)
	{
		if (RecordUpdates)
		{
			Record("Update");
		}
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		if (RecordUpdates)
		{
			Record("Fixed");
		}
	}

	protected override void OnDestroy()
	{
		Record("Destroy");
		Log.Info($"{Label} destroyed");
	}

	protected override void OnCollisionEnter(Entity other) => Record($"CollisionEnter({other.Name})");

	protected override void OnCollisionExit(Entity other) => Record($"CollisionExit({other.Name})");

	protected override void OnTriggerEnter(Entity other) => Record($"TriggerEnter({other.Name}{(other is Lifecycle ? " scripted" : "")})");

	protected override void OnTriggerExit(Entity other) => Record($"TriggerExit({other.Name})");

	protected override void OnClick(Entity button) => Record($"Click({button.Name})");

	private void Record(string call)
	{
		string entry = $"{Label}:{call}";
		if (Recorder != null && Recorder.IsValid)
		{
			Recorder.Name = Recorder.Name.Length == 0 ? entry : $"{Recorder.Name},{entry}";
		}
		if (Report)
		{
			TestReporter.Pass(entry);
		}
	}
}

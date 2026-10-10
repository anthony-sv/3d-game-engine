namespace Strada.Tests;

// Appends each lifecycle call and contact to the name of its Recorder entity (comma separated), so tests can see what
// ran and in which order, and logs its destruction.
public sealed class Lifecycle : Script
{
	public string Label = "";
	public Entity? Recorder;
	public bool RecordUpdates = true;

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

	private void Record(string call)
	{
		if (Recorder != null && Recorder.IsValid)
		{
			string entry = $"{Label}:{call}";
			Recorder.Name = Recorder.Name.Length == 0 ? entry : $"{Recorder.Name},{entry}";
		}
	}
}

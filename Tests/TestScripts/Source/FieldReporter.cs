namespace Strada.Tests;

// Counts its updates in a field and names its entity after its other fields: tests see the values the engine gives a
// running script, and read back the ones it changes itself.
public sealed class FieldReporter : Script
{
	public int Speed = 1;
	public bool Visible;
	public int Updates;

	protected override void OnUpdate(float deltaTime)
	{
		Updates++;
		Name = $"Speed {Speed} Visible {Visible}";
	}
}

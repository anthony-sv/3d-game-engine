using Xunit;

namespace Strada.ScriptCore.Tests;

// Tests that touch process-wide statics of the scripting API (Strada.Random's generator, Time.DeltaTime) run one at a
// time, never in parallel with each other.
[CollectionDefinition(Name, DisableParallelization = true)]
public sealed class ProcessStaticsCollection
{
	public const string Name = "ProcessStatics";
}

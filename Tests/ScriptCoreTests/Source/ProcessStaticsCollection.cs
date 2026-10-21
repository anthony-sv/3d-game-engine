using Xunit;

namespace Strada.ScriptCore.Tests;

// Tests that touch process-wide statics of the scripting API (Strada.Random's generator, Time.DeltaTime), and tests that
// measure time, run one at a time once the parallel tests are done: nothing runs beside them.
[CollectionDefinition(Name, DisableParallelization = true)]
public sealed class ProcessStaticsCollection
{
	public const string Name = "ProcessStatics";
}

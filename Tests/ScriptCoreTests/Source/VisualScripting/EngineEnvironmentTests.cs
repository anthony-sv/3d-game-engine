using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The engine's environment answers for no entity (ID 0) itself, so the bindings never see it; everything else reaches the
// engine, which the engine's own tests exercise (Tests/StradaTests).
[Collection(ProcessStaticsCollection.Name)]
public sealed class EngineEnvironmentTests
{
	[Fact]
	public void NoEntityNeverReachesTheEngine()
	{
		EngineEnvironment environment = new();
		Assert.Null(environment.GetEntity(0));
		Assert.False(environment.IsEntityValid(0));
		Assert.Equal("", environment.GetEntityName(0));
		Assert.False(environment.HasComponent(0, typeof(RigidBodyComponent)));
		Assert.Null(environment.GetComponent(0, typeof(RigidBodyComponent)));
	}

	[Fact]
	public void TheUpdateTimeStepIsTimeDeltaTime()
	{
		float previous = Time.DeltaTime;
		try
		{
			new EngineEnvironment().SetDeltaTime(0.25f);
			Assert.Equal(0.25f, Time.DeltaTime);
		}
		finally
		{
			Time.DeltaTime = previous;
		}
	}
}

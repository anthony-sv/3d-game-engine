using System;

namespace Strada.Tests;

// Throws from OnUpdate: the engine logs it and keeps running the scene.
public sealed class Thrower : Script
{
	protected override void OnUpdate(float deltaTime)
	{
		throw new InvalidOperationException("Thrower always fails");
	}
}

// Creates an entity when it starts.
public sealed class Spawner : Script
{
	public string SpawnName = "Spawned";

	protected override void OnCreate()
	{
		Entity spawned = Create(SpawnName);
		spawned.Translation = new Vector3(0.0f, 5.0f, 0.0f);
	}
}

// Serialized fields of base classes come first.
public class BaseBehaviour : Script
{
	[SerializeField]
	protected float BaseValue = 1.0f;
}

public sealed class DerivedBehaviour : BaseBehaviour
{
	public int Extra = 2;
}

// None of these are script classes.
public abstract class AbstractBehaviour : Script
{
}

public sealed class GenericBehaviour<T> : Script
{
	public T? Value;
}

public sealed class NoDefaultConstructor : Script
{
	public NoDefaultConstructor(int value)
	{
		Value = value;
	}

	public int Value;
}

public sealed class ThrowingConstructor : Script
{
	public ThrowingConstructor()
	{
		throw new InvalidOperationException("ThrowingConstructor cannot be created");
	}
}

public sealed class NotAScript
{
	public int Value = 1;
}

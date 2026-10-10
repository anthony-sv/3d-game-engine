namespace Strada;

/// <summary>Base class of entity behaviours. A Script component names a class deriving from Script; when the scene
/// runs, the engine creates one instance per such entity, sets its serialized fields (public fields and fields marked
/// <see cref="SerializeFieldAttribute"/>) and calls the overridden methods. Exceptions thrown by them are logged and
/// do not stop the scene.</summary>
public abstract class Script : Entity
{
	/// <summary>Called once when the scene starts (or the instance is created while it runs), after every script of the
	/// scene was created and before the first update.</summary>
	protected virtual void OnCreate()
	{
	}

	/// <summary>Called every frame with the seconds since the previous frame.</summary>
	protected virtual void OnUpdate(float deltaTime)
	{
	}

	/// <summary>Called before every physics step with the seconds per step.</summary>
	protected virtual void OnFixedUpdate(float fixedDeltaTime)
	{
	}

	/// <summary>Called when the entity is destroyed or the scene stops.</summary>
	protected virtual void OnDestroy()
	{
	}

	/// <summary>Called when one of the entity's colliders starts touching a collider of <paramref name="other"/>.</summary>
	protected virtual void OnCollisionEnter(Entity other)
	{
	}

	/// <summary>Called when the entity's colliders stop touching the colliders of <paramref name="other"/>.</summary>
	protected virtual void OnCollisionExit(Entity other)
	{
	}

	/// <summary>Called when a trigger collider of either entity starts overlapping a collider of the other.</summary>
	protected virtual void OnTriggerEnter(Entity other)
	{
	}

	/// <summary>Called when the overlap between a trigger collider and <paramref name="other"/> ends.</summary>
	protected virtual void OnTriggerExit(Entity other)
	{
	}

	internal void InvokeOnCreate() => OnCreate();

	internal void InvokeOnUpdate(float deltaTime) => OnUpdate(deltaTime);

	internal void InvokeOnFixedUpdate(float fixedDeltaTime) => OnFixedUpdate(fixedDeltaTime);

	internal void InvokeOnDestroy() => OnDestroy();

	internal void InvokeOnCollisionEnter(Entity other) => OnCollisionEnter(other);

	internal void InvokeOnCollisionExit(Entity other) => OnCollisionExit(other);

	internal void InvokeOnTriggerEnter(Entity other) => OnTriggerEnter(other);

	internal void InvokeOnTriggerExit(Entity other) => OnTriggerExit(other);
}

using Strada.Interop;

namespace Strada;

/// <summary>Renders the scene from the entity's position, looking down its -Z axis.</summary>
[NativeComponent("Camera")]
public sealed unsafe class CameraComponent : Component
{
	private CameraComponent()
	{
	}

	/// <summary>Perspective or orthographic projection.</summary>
	public ProjectionType Projection
	{
		get => (ProjectionType)NativeField.Get<int>(InternalCalls.CameraComponent_GetProjection, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.CameraComponent_SetProjection, Entity.ID, (int)value);
	}

	/// <summary>Vertical field of view of perspective projection, in degrees.</summary>
	public float PerspectiveFOV
	{
		get => NativeField.Get<float>(InternalCalls.CameraComponent_GetPerspectiveFOV, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CameraComponent_SetPerspectiveFOV, Entity.ID, value);
	}

	/// <summary>Near clipping distance of perspective projection.</summary>
	public float PerspectiveNear
	{
		get => NativeField.Get<float>(InternalCalls.CameraComponent_GetPerspectiveNear, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CameraComponent_SetPerspectiveNear, Entity.ID, value);
	}

	/// <summary>Far view distance of perspective projection (culling and shadows).</summary>
	public float PerspectiveFar
	{
		get => NativeField.Get<float>(InternalCalls.CameraComponent_GetPerspectiveFar, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CameraComponent_SetPerspectiveFar, Entity.ID, value);
	}

	/// <summary>Full view height of orthographic projection, in world units.</summary>
	public float OrthographicSize
	{
		get => NativeField.Get<float>(InternalCalls.CameraComponent_GetOrthographicSize, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CameraComponent_SetOrthographicSize, Entity.ID, value);
	}

	/// <summary>Near clipping distance of orthographic projection.</summary>
	public float OrthographicNear
	{
		get => NativeField.Get<float>(InternalCalls.CameraComponent_GetOrthographicNear, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CameraComponent_SetOrthographicNear, Entity.ID, value);
	}

	/// <summary>Far clipping distance of orthographic projection.</summary>
	public float OrthographicFar
	{
		get => NativeField.Get<float>(InternalCalls.CameraComponent_GetOrthographicFar, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CameraComponent_SetOrthographicFar, Entity.ID, value);
	}

	/// <summary>Whether the scene renders from this camera (the first primary camera in the hierarchy).</summary>
	public bool Primary
	{
		get => NativeField.GetBool(InternalCalls.CameraComponent_GetPrimary, Entity.ID);
		set => NativeField.SetBool(InternalCalls.CameraComponent_SetPrimary, Entity.ID, value);
	}

	/// <summary>Keeps <see cref="AspectRatio"/> instead of following the viewport.</summary>
	public bool FixedAspectRatio
	{
		get => NativeField.GetBool(InternalCalls.CameraComponent_GetFixedAspectRatio, Entity.ID);
		set => NativeField.SetBool(InternalCalls.CameraComponent_SetFixedAspectRatio, Entity.ID, value);
	}

	/// <summary>Width over height when <see cref="FixedAspectRatio"/> is set.</summary>
	public float AspectRatio
	{
		get => NativeField.Get<float>(InternalCalls.CameraComponent_GetAspectRatio, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CameraComponent_SetAspectRatio, Entity.ID, value);
	}

	/// <summary>The ray from the camera through a point of the scene's viewport, in pixels from its top-left corner.</summary>
	public Ray ScreenToWorldRay(Vector2 screenPosition)
	{
		Ray ray;
		InternalCalls.CameraComponent_ScreenToWorldRay(Entity.ID, &screenPosition, &ray);
		return ray;
	}
}

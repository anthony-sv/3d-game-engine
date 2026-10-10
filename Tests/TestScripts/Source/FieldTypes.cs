using System.Collections.Generic;
using System.Globalization;

namespace Strada.Tests;

// A field of every supported type, plus fields the engine must not serialize. OnCreate writes the values the instance
// received into its name.
public sealed class FieldTypes : Script
{
	public bool Flag = true;
	public int Count = -3;
	public uint Mask = 7;
	public long Big = -9000000000;
	public ulong Huge = 18000000000000000000;
	[Range(0.0f, 10.0f)]
	[Tooltip("Units per second")]
	public float Speed = 2.5f;
	public double Precise = 0.125;
	public string Title = "Hello";
	public Vector2 Size = new(1.0f, 2.0f);
	public Vector3 Offset = new(1.0f, 2.0f, 3.0f);
	public Vector4 Weights = new(1.0f, 2.0f, 3.0f, 4.0f);
	public Quaternion Turn = Quaternion.Identity;
	public Color Tint = new(0.5f, 0.25f, 1.0f, 1.0f);
	public Entity? Target;
	public AssetHandle Model;
	[HideInInspector]
	public int Hidden = 5;
	[SerializeField]
	private int m_Secret = 42;

	// Not serialized: private without [SerializeField], static, readonly and of an unsupported type.
	private int m_Private = 1;
	public static int Shared = 3;
	public readonly int Fixed = 4;
	public List<int> Unsupported = [];

	protected override void OnCreate()
	{
		Name = string.Create(CultureInfo.InvariantCulture,
			$"{Flag}|{Count}|{Mask}|{Big}|{Huge}|{Speed}|{Precise}|{Title}|{Size}|{Offset}|{Weights}|{Turn}|{Tint}|{Target?.ID ?? 0}|{Model}|{Hidden}|{m_Secret}|{m_Private}|{Shared}|{Fixed}|{Unsupported.Count}");
	}
}

using System;

namespace Strada.Tests;

// Moves its entity along Direction at Speed units per second. Graphs reach it through its [VisualNode] members (spec-slice3
// section 3.7): an instance method of a script class, an inferred generic method and a pure method returning an array.
public sealed class Mover : Script
{
	public float Speed = 1.0f;
	public Vector3 Direction = new(1.0f, 0.0f, 0.0f);

	// Moves the entity distance units along Direction at once.
	[VisualNode]
	public void Nudge(float distance)
	{
		Translation += Direction * distance;
	}

	[VisualNode]
	public static T Pick<T>(T first, T second, bool takeSecond) => takeSecond ? second : first;

	// count copies of value (none for a count of 0 or less).
	[VisualNode(Pure = true)]
	public static int[] Repeat(int value, int count)
	{
		int[] values = new int[Math.Max(count, 0)];
		Array.Fill(values, value);
		return values;
	}

	protected override void OnUpdate(float deltaTime)
	{
		Translation += Direction * (Speed * deltaTime);
	}
}

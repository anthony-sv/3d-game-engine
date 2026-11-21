using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Strada.Interop;

// What the last read-back of an instance's changes answered of one C# field or public graph variable (Docs/Architecture.md
// §11, "Long lists"): a copy of the running value, a list's elements copied in their own type, so that the next read-back
// compares the running value with it without converting it to the stored form and answers the value only when it changed.
// Values of plain types compare bit for bit (a float by its bit pattern, so that a NaN that stays is unchanged and a change
// between 0 and -0 is a change); references by what the stored form keeps of them (a string's characters, an entity's ID,
// an asset's handle). A value of another type or length differs. Main thread only, as the instances it copies.
internal abstract class ReadBackCopy
{
	// Whether the running value is the value last taken.
	internal abstract bool Matches(object? value);

	// Copies the running value (of the type the copy was made for), replacing what was taken before.
	internal abstract void Take(object? value);

	// A copy for values of a field's or variable's CLR type: T[] and List<T> are copied element by element as T[], other types
	// as one value. Nothing is taken yet, so it matches nothing.
	internal static ReadBackCopy Create(Type type)
	{
		Type? elementType = ScriptFieldCodec.GetListElementType(type);
		Type copyType = elementType != null ? typeof(ListCopy<>).MakeGenericType(elementType) : typeof(ValueCopy<>).MakeGenericType(type);
		return (ReadBackCopy)Activator.CreateInstance(copyType, nonPublic: true)!;
	}

	// The count of a list value (null, the empty list, counts 0); 0 for other values.
	internal static int CountOf(object? value) => value is ICollection list ? list.Count : 0;

	// Whether two runs of values are the same, as the class comment says.
	private protected static bool Same<T>(ReadOnlySpan<T> running, ReadOnlySpan<T> taken)
	{
		if (running.Length != taken.Length)
		{
			return false;
		}
		if (!RuntimeHelpers.IsReferenceOrContainsReferences<T>())
		{
			// Booleans, numbers, enums and the math structs, which have no padding: their bytes are their values.
			int length = running.Length * Unsafe.SizeOf<T>();
			ReadOnlySpan<byte> runningBytes = MemoryMarshal.CreateReadOnlySpan(ref Unsafe.As<T, byte>(ref MemoryMarshal.GetReference(running)), length);
			ReadOnlySpan<byte> takenBytes = MemoryMarshal.CreateReadOnlySpan(ref Unsafe.As<T, byte>(ref MemoryMarshal.GetReference(taken)), length);
			return runningBytes.SequenceEqual(takenBytes);
		}
		for (int index = 0; index < running.Length; index++)
		{
			if (!SameReference(running[index], taken[index]))
			{
				return false;
			}
		}
		return true;
	}

	// References compare by what their stored form holds (ScriptFieldCodec.WriteValue): an entity or an asset held as another
	// object of the same ID is the same value (an entity's ID is set once, as its instance is made).
	private static bool SameReference<T>(T running, T taken) => (object?)running switch
	{
		null => taken is null,
		string text => taken is string other && string.Equals(text, other, StringComparison.Ordinal),
		Entity entity => taken is Entity other && entity.ID == other.ID,
		Asset asset => taken is Asset other && asset.Handle == other.Handle,
		_ => EqualityComparer<T>.Default.Equals(running, taken),
	};

	// A copy of one value.
	private sealed class ValueCopy<T> : ReadBackCopy
	{
		private T m_Value = default!;
		private bool m_HasValue;

		internal override bool Matches(object? value) =>
			m_HasValue && TryRead(value, out T running) && Same(new ReadOnlySpan<T>(in running), new ReadOnlySpan<T>(in m_Value));

		internal override void Take(object? value)
		{
			m_HasValue = TryRead(value, out m_Value);
		}

		// A value of another type is none (it never matches); a null reference is a value of a reference type.
		private static bool TryRead(object? value, out T running)
		{
			if (value is T typed)
			{
				running = typed;
				return true;
			}
			running = default!;
			return value is null && !typeof(T).IsValueType;
		}
	}

	// A copy of a list's elements (an array or a List<T>; null is the empty list).
	private sealed class ListCopy<T> : ReadBackCopy
	{
		private T[] m_Elements = [];
		private int m_Count;
		private bool m_HasValue;

		internal override bool Matches(object? value) => m_HasValue && TryRead(value, out ReadOnlySpan<T> running) &&
			Same(running, new ReadOnlySpan<T>(m_Elements, 0, m_Count));

		internal override void Take(object? value)
		{
			if (!TryRead(value, out ReadOnlySpan<T> running))
			{
				m_HasValue = false;
				return;
			}
			if (m_Elements.Length < running.Length)
			{
				m_Elements = new T[running.Length];
			}
			else if (RuntimeHelpers.IsReferenceOrContainsReferences<T>() && running.Length < m_Count)
			{
				// Elements beyond the new count keep nothing alive.
				Array.Clear(m_Elements, running.Length, m_Count - running.Length);
			}
			running.CopyTo(m_Elements);
			m_Count = running.Length;
			m_HasValue = true;
		}

		private static bool TryRead(object? value, out ReadOnlySpan<T> running)
		{
			switch (value)
			{
				case T[] array:
					running = array;
					return true;
				case List<T> list:
					running = CollectionsMarshal.AsSpan(list);
					return true;
				case null:
					running = [];
					return true;
				default:
					running = [];
					return false;
			}
		}
	}
}

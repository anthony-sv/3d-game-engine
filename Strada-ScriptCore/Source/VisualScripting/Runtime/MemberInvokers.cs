using System;
using System.Collections.Generic;
using System.Linq.Expressions;
using System.Reflection;

namespace Strada.VisualScripting;

// Calls a member with its arguments in consecutive staging slots of a frame and writes its results to consecutive output
// slots: inputs is the slot of the first argument (Target for instance members), outputs the slot of the first result.
internal delegate void MemberInvoker(Slot[] slots, int inputs, int outputs);

// Builds the invokers of reflected members, operator and conversion methods, and of Make and Break (spec section 5.1):
// compiled expression thunks that read and write slots without boxing the values the slots hold in place. They are
// bindings, not graph code: one per member, built the first time a node needs it and cached by the runtime that built
// it, so a member of the game assembly is never referenced past the runtime's caches. An array a member gives is never
// null in a graph: null becomes the library's empty array of its type (spec-slice3 section 3.4.2).
internal static class MemberInvokers
{
	// Methods, constructors, getters and setters: Target and the parameters in, the out parameters and the result (a
	// getter's or constructor's value) out.
	internal static MemberInvoker ForMember(MemberInfo member, MemberNodeKind kind, NodeLibrary library)
	{
		Builder builder = new(library);
		List<Expression> body = [];
		List<ParameterExpression> locals = [];
		int input = 0;
		Expression? target = kind != MemberNodeKind.New && !IsStatic(member) ? builder.Read(input++, member.DeclaringType!) : null;
		switch (member)
		{
			case MethodBase method:
				// Out parameters become locals the call writes; their values and then the result are the outputs.
				ParameterInfo[] parameters = method.GetParameters();
				Expression[] arguments = new Expression[parameters.Length];
				List<ParameterExpression> outs = [];
				for (int index = 0; index < parameters.Length; index++)
				{
					Type type = parameters[index].ParameterType;
					if (type.IsByRef)
					{
						ParameterExpression value = Expression.Variable(type.GetElementType()!, parameters[index].Name);
						locals.Add(value);
						outs.Add(value);
						arguments[index] = value;
					}
					else
					{
						arguments[index] = builder.Read(input++, type);
					}
				}
				Expression call = method is ConstructorInfo constructor ? Expression.New(constructor, arguments)
					: Expression.Call(target, (MethodInfo)method, arguments);
				ParameterExpression? result = null;
				if (call.Type == typeof(void))
				{
					body.Add(call);
				}
				else
				{
					result = Expression.Variable(call.Type, "result");
					locals.Add(result);
					body.Add(Expression.Assign(result, call));
				}
				int output = 0;
				foreach (ParameterExpression value in outs)
				{
					body.Add(builder.Write(output++, value));
				}
				if (result != null)
				{
					body.Add(builder.Write(output, result));
				}
				break;
			case PropertyInfo property when kind == MemberNodeKind.Get:
				body.Add(builder.Write(0, Expression.Property(target, property)));
				break;
			case PropertyInfo property:
				body.Add(Expression.Assign(Expression.Property(target, property), builder.Read(input, property.PropertyType)));
				break;
			case FieldInfo { IsLiteral: true } constant:
				object raw = constant.GetRawConstantValue()!;
				body.Add(builder.Write(0, Expression.Constant(constant.FieldType.IsEnum ? Enum.ToObject(constant.FieldType, raw) : raw, constant.FieldType)));
				break;
			case FieldInfo field when kind == MemberNodeKind.Get:
				body.Add(builder.Write(0, Expression.Field(target, field)));
				break;
			case FieldInfo field:
				body.Add(Expression.Assign(Expression.Field(target, field), builder.Read(input, field.FieldType)));
				break;
			default:
				throw new ArgumentException($"{member} has no node form", nameof(member));
		}
		return builder.Compile(locals, body);
	}

	// Make (spec section 4.4): a struct from its fields, in declaration order.
	internal static MemberInvoker ForMake(VisualType type, NodeLibrary library)
	{
		Builder builder = new(library);
		List<MemberAssignment> bindings = [];
		for (int index = 0; index < type.Fields.Count; index++)
		{
			FieldInfo field = (FieldInfo)type.Fields[index].Member;
			bindings.Add(Expression.Bind(field, builder.Read(index, field.FieldType)));
		}
		Expression value = Expression.MemberInit(Expression.New(type.ClrType!), bindings);
		return builder.Compile([], [builder.Write(0, value)]);
	}

	// Break: the fields of a struct, or the get-only properties of a readonly struct, in declaration order.
	internal static MemberInvoker ForBreak(VisualType type, NodeLibrary library)
	{
		Builder builder = new(library);
		ParameterExpression value = Expression.Variable(type.ClrType!, "value");
		List<Expression> body = [Expression.Assign(value, builder.Read(0, type.ClrType!))];
		for (int index = 0; index < type.Fields.Count; index++)
		{
			MemberInfo member = type.Fields[index].Member;
			body.Add(builder.Write(index, member is FieldInfo field ? Expression.Field(value, field) : Expression.Property(value, (PropertyInfo)member)));
		}
		return builder.Compile([value], body);
	}

	private static bool IsStatic(MemberInfo member) => member switch
	{
		MethodBase method => method.IsStatic,
		PropertyInfo property => (property.GetMethod ?? property.SetMethod)!.IsStatic,
		FieldInfo field => field.IsStatic,
		_ => true,
	};

	// The reads and writes of slots: each type in the field its kind uses.
	private sealed class Builder(NodeLibrary library)
	{
		private readonly NodeLibrary m_Library = library;
		private readonly ParameterExpression m_Slots = Expression.Parameter(typeof(Slot[]), "slots");
		private readonly ParameterExpression m_Inputs = Expression.Parameter(typeof(int), "inputs");
		private readonly ParameterExpression m_Outputs = Expression.Parameter(typeof(int), "outputs");

		internal Expression Read(int offset, Type type)
		{
			Expression slot = Expression.ArrayIndex(m_Slots, Expression.Add(m_Inputs, Expression.Constant(offset)));
			SlotKind kind = SlotValues.KindOf(type);
			switch (kind)
			{
				case SlotKind.Enum:
					return Expression.Convert(Expression.Convert(Expression.Field(slot, SlotField(SlotKind.ULong)), Enum.GetUnderlyingType(type)), type);
				case SlotKind.Object:
					Expression value = Expression.Field(slot, SlotField(SlotKind.Object));
					return type == typeof(object) ? value : Expression.Convert(value, type);
				default:
					return Expression.Field(slot, SlotField(kind));
			}
		}

		internal Expression Write(int offset, Expression value)
		{
			Expression slot = Expression.ArrayAccess(m_Slots, Expression.Add(m_Outputs, Expression.Constant(offset)));
			Type type = value.Type;
			SlotKind kind = SlotValues.KindOf(type);
			Expression wrapped = kind switch
			{
				SlotKind.Enum => Expression.Call(Factory(nameof(Slot.FromULong)), EnumBits(value)),
				SlotKind.Object when type == typeof(string) => Expression.Call(Factory(nameof(Slot.FromString)), value),
				SlotKind.Object when type.IsSZArray => Expression.Call(Factory(nameof(Slot.FromObject)),
					Expression.Convert(Expression.Coalesce(value, Expression.Constant(EmptyArrayOf(type), type)), typeof(object))),
				SlotKind.Object => Expression.Call(Factory(nameof(Slot.FromObject)), Expression.Convert(value, typeof(object))),
				_ => Expression.Call(Factory("From" + kind), value),
			};
			return Expression.Assign(slot, wrapped);
		}

		// The library's empty array of an array type. The members nodes call have library types only; an invoker built for an
		// array type outside the library still never gives null, with an empty array of its own.
		private Array EmptyArrayOf(Type type) => m_Library.FindType(type)?.EmptyArray ?? Array.CreateInstance(type.GetElementType()!, 0);

		internal MemberInvoker Compile(IEnumerable<ParameterExpression> locals, IEnumerable<Expression> body)
		{
			BlockExpression block = Expression.Block(typeof(void), locals, body);
			return Expression.Lambda<MemberInvoker>(block, m_Slots, m_Inputs, m_Outputs).Compile();
		}

		// The bits of an enum value: its underlying value, sign-extended for signed types.
		private static Expression EnumBits(Expression value)
		{
			Type underlying = Enum.GetUnderlyingType(value.Type);
			Expression number = Expression.Convert(value, underlying);
			return SlotValues.IsSigned(underlying)
				? Expression.Convert(Expression.Convert(number, typeof(long)), typeof(ulong))
				: Expression.Convert(number, typeof(ulong));
		}

		private static FieldInfo SlotField(SlotKind kind) =>
			typeof(Slot).GetField(kind == SlotKind.Object ? nameof(Slot.Object) : kind.ToString(), BindingFlags.NonPublic | BindingFlags.Instance) ??
			throw new InvalidOperationException($"Slot has no field for {kind}");

		private static MethodInfo Factory(string name) =>
			typeof(Slot).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static) ?? throw new InvalidOperationException($"Slot has no {name}");
	}
}

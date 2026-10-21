using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;

namespace Strada.VisualScripting;

// The pins of a reflected member and how its parameters map to them.
internal sealed record MemberShape(IReadOnlyList<PinDescription> Pins, IReadOnlyList<MemberParameter> Parameters, TargetKind Target);

// Nodes from the public members of the scripting API (spec section 4.5): which members become nodes, their type strings,
// pins, purity, titles and categories.
internal static class MemberNodes
{
	internal const string ParameterSuffix = "Parameter";

	// The type whose methods graphs never call (they use Event.Send and the Variable nodes); its properties are nodes.
	private const string VisualScriptComponentName = "Strada.VisualScriptComponent";

	internal static void Create(IReadOnlyList<Type> apiTypes, Func<Type, VisualType?> resolve, VisualType entity, Documentation documentation,
		List<MemberNodeDefinition> nodes)
	{
		foreach (Type type in apiTypes)
		{
			if (!IsDeclaringType(type))
			{
				continue;
			}
			List<MemberInfo> members = [];
			const BindingFlags declared = BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;
			if (type.FullName != VisualScriptComponentName)
			{
				members.AddRange(type.GetMethods(declared));
			}
			members.AddRange(type.GetProperties(declared));
			members.AddRange(type.GetFields(declared));
			if (!type.IsAbstract)
			{
				members.AddRange(type.GetConstructors(BindingFlags.Public | BindingFlags.Instance));
			}
			// Reflection does not promise an order: metadata order is the declaration order, the same in every build.
			members.Sort(static (a, b) => a.MetadataToken.CompareTo(b.MetadataToken));
			foreach (MemberInfo member in members)
			{
				if (IsHidden(member))
				{
					continue;
				}
				switch (member)
				{
					case MethodInfo method:
						AddMethod(method, resolve, entity, documentation, nodes);
						break;
					case PropertyInfo property:
						AddProperty(property, resolve, entity, documentation, nodes);
						break;
					case FieldInfo field:
						AddField(field, resolve, entity, documentation, nodes);
						break;
					case ConstructorInfo constructor when HasPinParameters(constructor):
						Add(MemberNodeKind.New, constructor, constructor.DeclaringType!.IsValueType, resolve, entity, documentation, nodes);
						break;
				}
			}
		}
	}

	// Whether a member of the type can become a node: public classes and structs of the API other than enums, delegates,
	// attributes, exceptions and Strada.Component itself (its only member, Entity, is how graphs reach entities anyway).
	internal static bool IsDeclaringType(Type type) =>
		!type.IsEnum && !type.IsInterface && !type.IsGenericType && !typeof(Delegate).IsAssignableFrom(type) &&
		!typeof(Attribute).IsAssignableFrom(type) && !typeof(Exception).IsAssignableFrom(type) && type != typeof(Component) && !IsHidden(type);

	// The pins of a member: Execute and Then for impure members, Target for instance members, the parameters (out parameters
	// as outputs), Value for setters, Result for methods returning a value and Value for getters and constructors. A
	// parameter whose pin would take the name of another pin of its direction gets the suffix "Parameter". Null when a type
	// is not a library type (generic method parameters become wildcards) or two pins of a direction would share a name.
	internal static MemberShape? Shape(MemberNodeKind kind, MemberInfo member, bool pure, Func<Type, VisualType?> resolve, VisualType entity)
	{
		Type declaring = member.DeclaringType!;
		TargetKind target = kind == MemberNodeKind.New || IsStatic(member) ? TargetKind.None
			: declaring == typeof(Entity) ? TargetKind.Entity
			: typeof(Component).IsAssignableFrom(declaring) ? TargetKind.Component
			: TargetKind.Value;
		List<PinDescription> inputs = [];
		List<PinDescription> outputs = [];
		if (!pure)
		{
			inputs.Add(BuiltinNodes.ExecIn());
			outputs.Add(BuiltinNodes.ExecOut());
		}
		if (target is TargetKind.Entity or TargetKind.Component)
		{
			inputs.Add(BuiltinNodes.Target(entity));
		}
		else if (target == TargetKind.Value)
		{
			VisualType? declaringType = resolve(declaring);
			if (declaringType == null)
			{
				return null;
			}
			inputs.Add(new PinDescription("Target", PinDirection.In, declaringType));
		}

		List<MemberParameter> parameters = [];
		VisualType? memberType;
		switch (member)
		{
			case MethodBase method:
				HashSet<string> fixedInputs = Names(inputs);
				HashSet<string> fixedOutputs = Names(outputs);
				Type? returnType = method is MethodInfo info && info.ReturnType != typeof(void) ? info.ReturnType : null;
				if (kind == MemberNodeKind.New || returnType != null)
				{
					fixedOutputs.Add(kind == MemberNodeKind.New ? "Value" : "Result");
				}
				List<PinDescription> parameterInputs = [];
				List<PinDescription> parameterOutputs = [];
				foreach (ParameterInfo parameter in method.GetParameters())
				{
					bool isOut = parameter.ParameterType.IsByRef;
					VisualType? type = ResolveSignatureType(isOut ? parameter.ParameterType.GetElementType()! : parameter.ParameterType, resolve);
					if (type == null || string.IsNullOrEmpty(parameter.Name))
					{
						return null;
					}
					PinDirection direction = isOut ? PinDirection.Out : PinDirection.In;
					string name = PascalCase(parameter.Name);
					if ((isOut ? fixedOutputs : fixedInputs).Contains(name))
					{
						name += ParameterSuffix;
					}
					List<PinDescription> list = isOut ? parameterOutputs : parameterInputs;
					if ((isOut ? fixedOutputs : fixedInputs).Contains(name) || list.Exists(pin => pin.Name == name))
					{
						return null;
					}
					list.Add(ParameterPin(name, direction, type, parameter));
					parameters.Add(new MemberParameter(parameter, name, direction));
				}
				inputs.AddRange(parameterInputs);
				outputs.AddRange(parameterOutputs);
				if (kind == MemberNodeKind.New)
				{
					VisualType? constructed = resolve(declaring);
					if (constructed == null)
					{
						return null;
					}
					outputs.Add(new PinDescription("Value", PinDirection.Out, constructed));
				}
				else if (returnType != null)
				{
					VisualType? result = ResolveSignatureType(returnType, resolve);
					if (result == null)
					{
						return null;
					}
					outputs.Add(TypedPin("Result", PinDirection.Out, result));
				}
				break;
			case PropertyInfo property:
				memberType = resolve(property.PropertyType);
				if (memberType == null)
				{
					return null;
				}
				(kind == MemberNodeKind.Set ? inputs : outputs).Add(new PinDescription("Value", kind == MemberNodeKind.Set ? PinDirection.In
					: PinDirection.Out, memberType));
				break;
			case FieldInfo field:
				memberType = resolve(field.FieldType);
				if (memberType == null)
				{
					return null;
				}
				(kind == MemberNodeKind.Set ? inputs : outputs).Add(new PinDescription("Value", kind == MemberNodeKind.Set ? PinDirection.In
					: PinDirection.Out, memberType));
				break;
			default:
				return null;
		}
		inputs.AddRange(outputs);
		return new MemberShape(inputs, parameters, target);
	}

	// Purity (spec section 4.5): getters and struct constructors are pure, setters never; a method is pure when it is a
	// method of Mathf, Random or a math struct, or marked [PureNode], unless it returns void without out parameters.
	internal static bool IsPure(MethodInfo method)
	{
		if (method.ReturnType == typeof(void) && !Array.Exists(method.GetParameters(), static parameter => parameter.ParameterType.IsByRef))
		{
			return false;
		}
		Type type = method.DeclaringType!;
		return type == typeof(Mathf) || type == typeof(Random) || type == typeof(Vector2) || type == typeof(Vector3) || type == typeof(Vector4) ||
			type == typeof(Quaternion) || type == typeof(Matrix4) || type == typeof(Color) || method.IsDefined(typeof(PureNodeAttribute), false);
	}

	// Upper-cases the first character only ("xy" → "Xy").
	internal static string PascalCase(string name) => name.Length == 0 ? name : char.ToUpperInvariant(name[0]) + name[1..];

	// The category of a declaring type's members (spec section 4.5).
	internal static string CategoryOf(Type type)
	{
		if (type.Namespace == "Strada.Testing")
		{
			return "Testing";
		}
		if (type == typeof(Mathf) || type == typeof(Random))
		{
			return "Math";
		}
		if (type == typeof(Vector2) || type == typeof(Vector3) || type == typeof(Vector4) || type == typeof(Quaternion) || type == typeof(Matrix4) ||
			type == typeof(Color))
		{
			return "Math/" + Titles.OfType(type);
		}
		if (typeof(Entity).IsAssignableFrom(type))
		{
			return "Entity";
		}
		if (typeof(Component).IsAssignableFrom(type))
		{
			return "Components/" + Titles.OfType(type);
		}
		if (type == typeof(Input))
		{
			return "Input";
		}
		if (type == typeof(Physics) || type == typeof(Ray) || type == typeof(RaycastHit))
		{
			return "Physics";
		}
		if (type == typeof(Time))
		{
			return "Time";
		}
		if (type == typeof(Application) || type == typeof(SceneManager))
		{
			return "Application";
		}
		if (type == typeof(Assets) || type == typeof(Asset) || type == typeof(AssetHandle))
		{
			return "Assets";
		}
		if (typeof(Asset).IsAssignableFrom(type))
		{
			return "Assets/" + Titles.OfType(type);
		}
		return type == typeof(Debug) || type == typeof(Log) ? "Debug" : "Utilities";
	}

	private static void AddMethod(MethodInfo method, Func<Type, VisualType?> resolve, VisualType entity, Documentation documentation,
		List<MemberNodeDefinition> nodes)
	{
		if (method.IsSpecialName || method.Name is nameof(Equals) or nameof(GetHashCode) or nameof(ToString) || method.ReturnType.IsByRef)
		{
			return;
		}
		TypeConstraint domain = TypeConstraint.None;
		if ((method.IsGenericMethodDefinition && !TryGetGenericDomain(method, out domain)) || !HasPinParameters(method))
		{
			return;
		}
		Add(MemberNodeKind.Call, method, IsPure(method), resolve, entity, documentation, nodes, domain);
	}

	private static void AddProperty(PropertyInfo property, Func<Type, VisualType?> resolve, VisualType entity, Documentation documentation,
		List<MemberNodeDefinition> nodes)
	{
		if (property.GetIndexParameters().Length > 0 || property.PropertyType.IsByRef)
		{
			return;
		}
		if (property.GetMethod is { IsPublic: true })
		{
			Add(MemberNodeKind.Get, property, true, resolve, entity, documentation, nodes);
		}
		MethodInfo? setter = property.SetMethod;
		if (setter is { IsPublic: true } && !IsInitOnly(setter) && (setter.IsStatic || !property.DeclaringType!.IsValueType))
		{
			Add(MemberNodeKind.Set, property, false, resolve, entity, documentation, nodes);
		}
	}

	private static void AddField(FieldInfo field, Func<Type, VisualType?> resolve, VisualType entity, Documentation documentation,
		List<MemberNodeDefinition> nodes)
	{
		if (field.IsSpecialName)
		{
			return;
		}
		Add(MemberNodeKind.Get, field, true, resolve, entity, documentation, nodes);
		if (!field.IsInitOnly && !field.IsLiteral && (field.IsStatic || !field.DeclaringType!.IsValueType))
		{
			Add(MemberNodeKind.Set, field, false, resolve, entity, documentation, nodes);
		}
	}

	private static void Add(MemberNodeKind kind, MemberInfo member, bool pure, Func<Type, VisualType?> resolve, VisualType entity,
		Documentation documentation, List<MemberNodeDefinition> nodes, TypeConstraint domain = TypeConstraint.None)
	{
		MemberShape? shape = Shape(kind, member, pure, resolve, entity);
		if (shape == null)
		{
			return;
		}
		Type declaring = member.DeclaringType!;
		string declaringId = TypeIds.Of(declaring)!;
		string title = kind switch
		{
			MemberNodeKind.Call => Titles.Words(member.Name),
			MemberNodeKind.Get => "Get " + Titles.Words(member.Name),
			MemberNodeKind.Set => "Set " + Titles.Words(member.Name),
			_ => $"Make {Titles.OfType(declaring)} ({JoinInputNames(shape.Pins)})",
		};
		bool isStatic = kind == MemberNodeKind.New || IsStatic(member);
		string lookupKey = kind == MemberNodeKind.New ? MemberNodeDefinition.Prefix(kind) + declaringId
			: MemberNodeDefinition.Prefix(kind) + declaringId + "." + member.Name;
		List<SettingDescription> settings = [];
		if (domain != TypeConstraint.None)
		{
			settings.Add(new SettingDescription("TypeArguments", SettingKind.TypeList) { Required = true, Constraint = domain });
		}
		nodes.Add(new MemberNodeDefinition(kind, member, TypeString(kind, member, declaringId), title, CategoryOf(declaring), pure, shape.Pins)
		{
			Subtitle = isStatic ? Titles.OfType(declaring) : "Target is " + Titles.OfType(declaring),
			Description = documentation.Of(member),
			Target = shape.Target,
			Parameters = shape.Parameters,
			GenericDomain = domain,
			LookupKey = lookupKey,
			Settings = settings,
		});
	}

	// "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)",
	// "Call:Strada.Entity.GetComponent<T>()", "Get:Strada.Time.DeltaTime", "New:Strada.Vector3(float,float,float)".
	private static string TypeString(MemberNodeKind kind, MemberInfo member, string declaringId)
	{
		StringBuilder text = new(MemberNodeDefinition.Prefix(kind));
		text.Append(declaringId);
		if (kind is MemberNodeKind.Get or MemberNodeKind.Set)
		{
			return text.Append('.').Append(member.Name).ToString();
		}
		MethodBase method = (MethodBase)member;
		if (kind == MemberNodeKind.Call)
		{
			text.Append('.').Append(member.Name);
			if (method.IsGenericMethodDefinition)
			{
				text.Append('<');
				Type[] arguments = method.GetGenericArguments();
				for (int index = 0; index < arguments.Length; index++)
				{
					text.Append(index > 0 ? "," : "").Append(arguments[index].Name);
				}
				text.Append('>');
			}
		}
		text.Append('(');
		ParameterInfo[] parameters = method.GetParameters();
		for (int index = 0; index < parameters.Length; index++)
		{
			Type type = parameters[index].ParameterType;
			if (index > 0)
			{
				text.Append(',');
			}
			if (type.IsByRef)
			{
				text.Append("out ");
				type = type.GetElementType()!;
			}
			text.Append(SignatureTypeId(type));
		}
		return text.Append(')').ToString();
	}

	private static string SignatureTypeId(Type type)
	{
		if (type.IsGenericMethodParameter)
		{
			return type.Name;
		}
		if (type.IsSZArray && type.GetElementType()!.IsGenericMethodParameter)
		{
			return type.GetElementType()!.Name + TypeIds.ArraySuffix;
		}
		return TypeIds.Of(type)!;
	}

	// A library type for a parameter or return type; a generic method parameter is a wildcard the TypeArguments setting
	// decides.
	private static VisualType? ResolveSignatureType(Type type, Func<Type, VisualType?> resolve)
	{
		if (type.IsGenericMethodParameter)
		{
			return VisualType.Wildcard;
		}
		return type.ContainsGenericParameters ? null : resolve(type);
	}

	private static PinDescription TypedPin(string name, PinDirection direction, VisualType type) =>
		type.IsWildcard ? BuiltinNodes.Wildcard(name, direction, PinConstraint.Reference, WildcardSource.None) : new PinDescription(name, direction, type);

	// An optional parameter's default becomes the pin's default (enums by enumerator, strings never null).
	private static PinDescription ParameterPin(string name, PinDirection direction, VisualType type, ParameterInfo parameter)
	{
		if (type.IsWildcard || direction == PinDirection.Out || !parameter.HasDefaultValue || !LiteralCodec.HasLiteralForm(type.ClrType!))
		{
			return TypedPin(name, direction, type);
		}
		Type clrType = type.ClrType!;
		object? raw = parameter.DefaultValue;
		object? value = raw switch
		{
			null when clrType == typeof(string) => string.Empty,
			null => clrType.IsValueType ? LiteralCodec.DefaultOf(type) : null,
			_ when clrType.IsEnum => Enum.ToObject(clrType, raw),
			_ => raw,
		};
		return new PinDescription(name, direction, type) { HasDefault = true, Default = value };
	}

	// out parameters become outputs; ref, in and params parameters have no pin form.
	private static bool HasPinParameters(MethodBase method)
	{
		foreach (ParameterInfo parameter in method.GetParameters())
		{
			if ((parameter.ParameterType.IsByRef && !(parameter.IsOut && !parameter.IsIn)) || parameter.IsDefined(typeof(ParamArrayAttribute), false))
			{
				return false;
			}
		}
		return true;
	}

	private static bool TryGetGenericDomain(MethodInfo method, out TypeConstraint domain)
	{
		domain = TypeConstraint.None;
		foreach (Type parameter in method.GetGenericArguments())
		{
			Type[] constraints = parameter.GetGenericParameterConstraints();
			TypeConstraint parameterDomain = constraints.Length != 1 ? TypeConstraint.None
				: constraints[0] == typeof(Component) ? TypeConstraint.Component
				: constraints[0] == typeof(Asset) ? TypeConstraint.Asset
				: constraints[0] == typeof(Script) ? TypeConstraint.Script
				: TypeConstraint.None;
			// Unconstrained type parameters (Assert.AreEqual<T>) and other constraints have no domain to choose from.
			if (parameterDomain == TypeConstraint.None || (domain != TypeConstraint.None && domain != parameterDomain))
			{
				return false;
			}
			domain = parameterDomain;
		}
		return domain != TypeConstraint.None;
	}

	private static bool IsHidden(MemberInfo member) =>
		member.IsDefined(typeof(ObsoleteAttribute), false) ||
		member.GetCustomAttribute<EditorBrowsableAttribute>(false) is { State: EditorBrowsableState.Never };

	private static bool IsStatic(MemberInfo member) => member switch
	{
		MethodBase method => method.IsStatic,
		PropertyInfo property => (property.GetMethod ?? property.SetMethod)!.IsStatic,
		FieldInfo field => field.IsStatic,
		_ => true,
	};

	private static bool IsInitOnly(MethodInfo setter) =>
		Array.Exists(setter.ReturnParameter.GetRequiredCustomModifiers(), static modifier => modifier == typeof(IsExternalInit));

	private static HashSet<string> Names(List<PinDescription> pins)
	{
		HashSet<string> names = new(StringComparer.Ordinal);
		foreach (PinDescription pin in pins)
		{
			names.Add(pin.Name);
		}
		return names;
	}

	private static string JoinInputNames(IReadOnlyList<PinDescription> pins)
	{
		StringBuilder names = new();
		foreach (PinDescription pin in pins)
		{
			if (pin.Direction == PinDirection.In && !pin.IsExec)
			{
				names.Append(names.Length > 0 ? ", " : "").Append(pin.Name);
			}
		}
		return names.ToString();
	}
}

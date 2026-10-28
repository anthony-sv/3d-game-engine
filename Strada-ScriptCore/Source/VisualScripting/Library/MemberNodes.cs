using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;

namespace Strada.VisualScripting;

// The pins of a reflected member and how its parameters map to them.
internal sealed record MemberShape(IReadOnlyList<PinDescription> Pins, IReadOnlyList<MemberParameter> Parameters, TargetKind Target);

// How a member's node is named and listed: the words its title uses for the member (null for the member's name in words;
// a constructor's replace "Make <type>"), its category, description and keywords, and whether it is a game member's.
internal sealed record MemberNodeText(string? Name, string Category, string Description, IReadOnlyList<string> Keywords, bool Game);

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
	internal static MemberShape? Shape(MemberNodeKind kind, MemberInfo member, bool pure, Func<Type, VisualType?> resolve, VisualType entity) =>
		Shape(kind, member, pure, resolve, entity, out _);

	// problem says why a member has no shape: "<type> is not a type of graphs", "<type> is a ref struct: graphs cannot hold
	// its values", "two pins would be named <name>".
	internal static MemberShape? Shape(MemberNodeKind kind, MemberInfo member, bool pure, Func<Type, VisualType?> resolve, VisualType entity,
		out string? problem)
	{
		problem = null;
		Type declaring = member.DeclaringType!;
		TargetKind target = kind == MemberNodeKind.New || IsStatic(member) ? TargetKind.None
			: declaring == typeof(Entity) ? TargetKind.Entity
			: typeof(Component).IsAssignableFrom(declaring) ? TargetKind.Component
			: declaring != typeof(Script) && declaring.IsSubclassOf(typeof(Script)) ? TargetKind.Script
			: TargetKind.Value;
		List<PinDescription> inputs = [];
		List<PinDescription> outputs = [];
		if (!pure)
		{
			inputs.Add(BuiltinNodes.ExecIn());
			outputs.Add(BuiltinNodes.ExecOut());
		}
		if (target is TargetKind.Entity or TargetKind.Component or TargetKind.Script)
		{
			inputs.Add(BuiltinNodes.Target(entity));
		}
		else if (target == TargetKind.Value)
		{
			VisualType? declaringType = resolve(declaring);
			if (declaringType == null)
			{
				problem = NotATypeOfGraphs(declaring);
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
					Type parameterType = isOut ? parameter.ParameterType.GetElementType()! : parameter.ParameterType;
					VisualType? type = ResolveSignatureType(parameterType, resolve);
					if (type == null)
					{
						problem = NotATypeOfGraphs(parameterType);
						return null;
					}
					if (string.IsNullOrEmpty(parameter.Name))
					{
						problem = "a parameter has no name";
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
						problem = $"two pins would be named {name}";
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
						problem = NotATypeOfGraphs(declaring);
						return null;
					}
					outputs.Add(new PinDescription("Value", PinDirection.Out, constructed));
				}
				else if (returnType != null)
				{
					VisualType? result = ResolveSignatureType(returnType, resolve);
					if (result == null)
					{
						problem = NotATypeOfGraphs(returnType);
						return null;
					}
					outputs.Add(TypedPin("Result", PinDirection.Out, result));
				}
				break;
			case PropertyInfo property:
				memberType = resolve(property.PropertyType);
				if (memberType == null)
				{
					problem = NotATypeOfGraphs(property.PropertyType);
					return null;
				}
				(kind == MemberNodeKind.Set ? inputs : outputs).Add(new PinDescription("Value", kind == MemberNodeKind.Set ? PinDirection.In
					: PinDirection.Out, memberType));
				break;
			case FieldInfo field:
				memberType = resolve(field.FieldType);
				if (memberType == null)
				{
					problem = NotATypeOfGraphs(field.FieldType);
					return null;
				}
				(kind == MemberNodeKind.Set ? inputs : outputs).Add(new PinDescription("Value", kind == MemberNodeKind.Set ? PinDirection.In
					: PinDirection.Out, memberType));
				break;
			default:
				problem = "it has no node form";
				return null;
		}
		inputs.AddRange(outputs);
		return new MemberShape(inputs, parameters, target);
	}

	// Why a type a member's signature uses keeps it from being a node.
	internal static string NotATypeOfGraphs(Type type) => type.IsByRefLike
		? $"{DisplayName(type)} is a ref struct: graphs cannot hold its values"
		: $"{DisplayName(type)} is not a type of graphs";

	// A type as messages name it: its type ID when it has one, else its full name, with arrays, pointers and generic types
	// as C# writes them ("System.Threading.Tasks.Task<int>", "int*").
	internal static string DisplayName(Type type)
	{
		if (type.IsByRef)
		{
			return DisplayName(type.GetElementType()!);
		}
		if (type.IsArray || type.IsPointer)
		{
			string suffix = type.IsPointer ? "*" : type.IsSZArray ? "[]" : "[" + new string(',', type.GetArrayRank() - 1) + "]";
			return DisplayName(type.GetElementType()!) + suffix;
		}
		if (type.IsGenericParameter)
		{
			return type.Name;
		}
		if (TypeIds.Of(type) is { } id)
		{
			return id;
		}
		string name = (type.IsGenericType ? type.GetGenericTypeDefinition().FullName : type.FullName) ?? type.Name;
		int tick = name.IndexOf('`', StringComparison.Ordinal);
		if (tick < 0 || !type.IsGenericType)
		{
			return name;
		}
		StringBuilder text = new(name, 0, tick, name.Length + 16);
		text.Append('<');
		Type[] arguments = type.GetGenericArguments();
		for (int index = 0; index < arguments.Length; index++)
		{
			text.Append(index > 0 ? ", " : "").Append(DisplayName(arguments[index]));
		}
		return text.Append('>').ToString();
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
		bool inference = false;
		if ((method.IsGenericMethodDefinition && !TryGetGenericForm(method, out domain, out inference)) || !HasPinParameters(method))
		{
			return;
		}
		Add(MemberNodeKind.Call, method, IsPure(method), resolve, entity, documentation, nodes, domain, inference);
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
		Documentation documentation, List<MemberNodeDefinition> nodes, TypeConstraint domain = TypeConstraint.None, bool inference = false)
	{
		MemberNodeText text = new(null, CategoryOf(member.DeclaringType!), documentation.Of(member), [], false);
		if (CreateNode(kind, member, pure, resolve, entity, text, out _, domain, inference) is { } node)
		{
			nodes.Add(node);
		}
	}

	// The node of a member in one of its kinds; null (with the reason in problem) when it has no shape. domain and inference
	// describe a generic method's form (TryGetGenericForm).
	internal static MemberNodeDefinition? CreateNode(MemberNodeKind kind, MemberInfo member, bool pure, Func<Type, VisualType?> resolve,
		VisualType entity, MemberNodeText text, out string? problem, TypeConstraint domain = TypeConstraint.None, bool inference = false)
	{
		MemberShape? shape = Shape(kind, member, pure, resolve, entity, out problem);
		if (shape == null)
		{
			return null;
		}
		IReadOnlyList<GenericAnchor?> anchors = [];
		if (inference)
		{
			(shape, anchors) = Infer((MethodInfo)member, shape);
		}
		Type declaring = member.DeclaringType!;
		string declaringId = TypeIds.Of(declaring)!;
		string name = text.Name ?? Titles.Words(member.Name);
		string title = kind switch
		{
			MemberNodeKind.Call => name,
			MemberNodeKind.Get => "Get " + name,
			MemberNodeKind.Set => "Set " + name,
			_ => $"{text.Name ?? "Make " + Titles.OfType(declaring)} ({JoinInputNames(shape.Pins)})",
		};
		bool isStatic = kind == MemberNodeKind.New || IsStatic(member);
		string lookupKey = kind == MemberNodeKind.New ? MemberNodeDefinition.Prefix(kind) + declaringId
			: MemberNodeDefinition.Prefix(kind) + declaringId + "." + member.Name;
		List<SettingDescription> settings = [];
		if (domain != TypeConstraint.None)
		{
			settings.Add(new SettingDescription("TypeArguments", SettingKind.TypeList) { Required = true, Constraint = domain });
		}
		else if (inference)
		{
			// Optional: links decide the types, unless a type parameter appears in no input.
			settings.Add(new SettingDescription("TypeArguments", SettingKind.TypeList)
			{
				Required = Array.Exists([.. anchors], static anchor => anchor == null),
				Constraint = TypeConstraint.Any,
			});
		}
		return new MemberNodeDefinition(kind, member, TypeString(kind, member, declaringId), title, text.Category, pure, shape.Pins)
		{
			Subtitle = isStatic ? Titles.OfType(declaring) : "Target is " + Titles.OfType(declaring),
			Description = text.Description,
			Keywords = text.Keywords,
			Game = text.Game,
			Target = shape.Target,
			Parameters = shape.Parameters,
			GenericDomain = domain,
			GenericInference = inference,
			GenericAnchors = anchors,
			LookupKey = lookupKey,
			Settings = settings,
		};
	}

	// "Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)",
	// "Call:Strada.Entity.GetComponent<T>()", "Get:Strada.Time.DeltaTime", "New:Strada.Vector3(float,float,float)".
	internal static string TypeString(MemberNodeKind kind, MemberInfo member, string declaringId)
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

	// A library type for a parameter or return type; a generic method parameter, or an array of one, is a wildcard that links
	// or the TypeArguments setting decide.
	private static VisualType? ResolveSignatureType(Type type, Func<Type, VisualType?> resolve)
	{
		if (GenericParameterOf(type) != null)
		{
			return VisualType.Wildcard;
		}
		return type.ContainsGenericParameters ? null : resolve(type);
	}

	// The generic method parameter a signature type is (T), or is an array of (T[]); null for any other type.
	private static Type? GenericParameterOf(Type type) =>
		type.IsGenericMethodParameter ? type : type.IsSZArray && type.GetElementType()!.IsGenericMethodParameter ? type.GetElementType() : null;

	// The pins of an inferred generic method (spec-slice3 section 3.6), for each type parameter T in order: when an input has
	// type T[], the first is the anchor A, every pin of type T[] joins the group "T[]" and every pin of type T is an element
	// of A; else, when an input has type T, the first is the anchor E, every pin of type T joins the group "T" and every
	// output of type T[] is the array of E's type; else no input decides T (its anchor is null) and TypeArguments must.
	private static (MemberShape Shape, IReadOnlyList<GenericAnchor?> Anchors) Infer(MethodInfo method, MemberShape shape)
	{
		Dictionary<(string, PinDirection), Type> signature = [];
		foreach (MemberParameter parameter in shape.Parameters)
		{
			Type type = parameter.Parameter.ParameterType;
			signature[(parameter.Pin, parameter.Direction)] = type.IsByRef ? type.GetElementType()! : type;
		}
		if (method.ReturnType != typeof(void))
		{
			signature[("Result", PinDirection.Out)] = method.ReturnType;
		}
		Type[] parameters = method.GetGenericArguments();
		Dictionary<Type, GenericAnchor?> anchors = [];
		foreach (Type parameter in parameters)
		{
			PinDescription? array = null;
			PinDescription? value = null;
			foreach (PinDescription pin in shape.Pins)
			{
				if (pin.IsInput && signature.TryGetValue((pin.Name, pin.Direction), out Type? type) && GenericParameterOf(type) == parameter)
				{
					if (type.IsSZArray)
					{
						array ??= pin;
					}
					else
					{
						value ??= pin;
					}
				}
			}
			anchors[parameter] = array != null ? new GenericAnchor(array.Name, true) : value != null ? new GenericAnchor(value.Name, false) : null;
		}
		List<PinDescription> pins = [];
		foreach (PinDescription pin in shape.Pins)
		{
			if (!signature.TryGetValue((pin.Name, pin.Direction), out Type? type) || GenericParameterOf(type) is not { } parameter)
			{
				pins.Add(pin);
				continue;
			}
			pins.Add(InferredPin(pin, parameter.Name, type.IsSZArray, anchors[parameter]));
		}
		List<GenericAnchor?> ordered = [];
		foreach (Type parameter in parameters)
		{
			ordered.Add(anchors[parameter]);
		}
		return (shape with { Pins = pins }, ordered);
	}

	// A pin of type T (isArray: T[]) of an inferred generic method, by where T comes from.
	private static PinDescription InferredPin(PinDescription pin, string parameter, bool isArray, GenericAnchor? anchor) => anchor switch
	{
		null => BuiltinNodes.Wildcard(pin.Name, pin.Direction, isArray ? PinConstraint.Array : PinConstraint.Any, WildcardSource.None),
		{ IsArray: true } when isArray =>
			BuiltinNodes.Wildcard(pin.Name, pin.Direction, PinConstraint.Array, WildcardSource.Links, parameter + TypeIds.ArraySuffix),
		{ IsArray: true } => BuiltinNodes.Wildcard(pin.Name, pin.Direction, PinConstraint.Any, WildcardSource.ElementOf, elementOf: anchor.Pin),
		_ when isArray => BuiltinNodes.ArrayOf(pin.Name, anchor.Pin),
		_ => BuiltinNodes.Wildcard(pin.Name, pin.Direction, PinConstraint.Any, WildcardSource.Links, parameter),
	};

	private static PinDescription TypedPin(string name, PinDirection direction, VisualType type) =>
		type.IsWildcard ? BuiltinNodes.Wildcard(name, direction, PinConstraint.Reference, WildcardSource.None) : new PinDescription(name, direction, type);

	// An optional parameter's default becomes the pin's default (enums by enumerator, strings never null). A struct declared
	// "= default" defaults to default(T), all zeros, as in C#; a quaternion is the exception: its zero is no rotation, and
	// graphs never hold one (spec sections 2.1 and 2.2), so its pin keeps the type default, the identity.
	private static PinDescription ParameterPin(string name, PinDirection direction, VisualType type, ParameterInfo parameter)
	{
		if (type.IsWildcard || direction == PinDirection.Out || !parameter.HasDefaultValue || !LiteralCodec.HasLiteralForm(type.ClrType!))
		{
			return TypedPin(name, direction, type);
		}
		Type clrType = type.ClrType!;
		object? raw = parameter.DefaultValue;
		if (raw == null && clrType == typeof(Quaternion))
		{
			return TypedPin(name, direction, type);
		}
		object? value = raw switch
		{
			null when clrType == typeof(string) => string.Empty,
			null => clrType.IsValueType ? RuntimeHelpers.GetUninitializedObject(clrType) : null,
			_ when clrType.IsEnum => Enum.ToObject(clrType, raw),
			_ => raw,
		};
		return new PinDescription(name, direction, type) { HasDefault = true, Default = value };
	}

	// out parameters become outputs; ref, in and params parameters have no pin form.
	internal static bool HasPinParameters(MethodBase method)
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

	// A generic method is a node when its type parameters are all unconstrained (they are inferred), or all constrained to the
	// same one of Component, Asset or Script (its domain); a method mixing the two forms has none.
	internal static bool TryGetGenericForm(MethodInfo method, out TypeConstraint domain, out bool inference)
	{
		inference = Array.TrueForAll(method.GetGenericArguments(), static parameter =>
			parameter.GetGenericParameterConstraints().Length == 0 && parameter.GenericParameterAttributes == GenericParameterAttributes.None);
		domain = TypeConstraint.None;
		return inference || TryGetGenericDomain(method, out domain);
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
			// Unconstrained type parameters (inferred instead) and other constraints have no domain to choose from.
			if (parameterDomain == TypeConstraint.None || (domain != TypeConstraint.None && domain != parameterDomain))
			{
				return false;
			}
			domain = parameterDomain;
		}
		return domain != TypeConstraint.None;
	}

	internal static bool IsHidden(MemberInfo member) =>
		member.IsDefined(typeof(ObsoleteAttribute), false) ||
		member.GetCustomAttribute<EditorBrowsableAttribute>(false) is { State: EditorBrowsableState.Never };

	internal static bool IsStatic(MemberInfo member) => member switch
	{
		MethodBase method => method.IsStatic,
		PropertyInfo property => (property.GetMethod ?? property.SetMethod)!.IsStatic,
		FieldInfo field => field.IsStatic,
		_ => true,
	};

	internal static bool IsInitOnly(MethodInfo setter) =>
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

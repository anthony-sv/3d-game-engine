using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Xml.Linq;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Docs/ScriptingAPI.md is generated from the scripting API itself: its public types and members by reflection, their
// summaries from the XML documentation the API's build writes next to Strada.ScriptCore.dll. The reference cannot drift
// from the API: this test writes the document and fails when the committed one differs. STRADA_UPDATE_DOCS=1 writes it.
public sealed class ApiReferenceTests
{
	[Fact]
	public void TheApiReferenceDocumentsEveryPublicMember()
	{
		Assembly api = typeof(Script).Assembly;
		string documentation = Path.ChangeExtension(api.Location, ".xml");
		Assert.True(File.Exists(documentation), $"{documentation} is missing: the scripting API's build writes its XML documentation");
		string expected = ApiReference.Write(api, XDocument.Load(documentation));

		string path = Path.Combine(SourceDirectory(), "Docs", "ScriptingAPI.md");
		if (Environment.GetEnvironmentVariable("STRADA_UPDATE_DOCS") == "1")
		{
			File.WriteAllText(path, expected);
			return;
		}
		Assert.True(File.Exists(path), $"{path} is missing: run ScriptCoreTests with STRADA_UPDATE_DOCS=1");
		string actual = File.ReadAllText(path).Replace("\r", "", StringComparison.Ordinal);
		Assert.True(actual == expected,
			"Docs/ScriptingAPI.md does not match the scripting API: run ScriptCoreTests with STRADA_UPDATE_DOCS=1 and commit the result");
	}

	// The repository's root, which the project file records in the test assembly when it builds.
	private static string SourceDirectory()
	{
		string? directory = typeof(ApiReferenceTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
			.FirstOrDefault(attribute => attribute.Key == "StradaSourceDirectory")?.Value;
		Assert.True(directory is not null, "the test assembly does not record the repository's directory");
		return directory;
	}
}

// Writes the API reference: an overview of every public type, then each type with its declaration, its summary and a
// table of its members (constructors, constants and fields, properties, methods, operators, events) in source order.
internal static class ApiReference
{
	private const BindingFlags Declared =
		BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;

	private static readonly Dictionary<Type, string> s_Keywords = new()
	{
		[typeof(void)] = "void",
		[typeof(bool)] = "bool",
		[typeof(byte)] = "byte",
		[typeof(sbyte)] = "sbyte",
		[typeof(short)] = "short",
		[typeof(ushort)] = "ushort",
		[typeof(int)] = "int",
		[typeof(uint)] = "uint",
		[typeof(long)] = "long",
		[typeof(ulong)] = "ulong",
		[typeof(float)] = "float",
		[typeof(double)] = "double",
		[typeof(decimal)] = "decimal",
		[typeof(char)] = "char",
		[typeof(string)] = "string",
		[typeof(object)] = "object",
	};

	private static readonly Dictionary<string, string> s_Operators = new()
	{
		["op_Addition"] = "+",
		["op_Subtraction"] = "-",
		["op_Multiply"] = "*",
		["op_Division"] = "/",
		["op_Modulus"] = "%",
		["op_UnaryNegation"] = "-",
		["op_UnaryPlus"] = "+",
		["op_Equality"] = "==",
		["op_Inequality"] = "!=",
		["op_LessThan"] = "<",
		["op_GreaterThan"] = ">",
		["op_LessThanOrEqual"] = "<=",
		["op_GreaterThanOrEqual"] = ">=",
		["op_LogicalNot"] = "!",
		["op_BitwiseAnd"] = "&",
		["op_BitwiseOr"] = "|",
		["op_ExclusiveOr"] = "^",
		["op_OnesComplement"] = "~",
		["op_True"] = "true",
		["op_False"] = "false",
	};

	private static readonly NullabilityInfoContext s_Nullability = new();

	public static string Write(Assembly api, XDocument documentation)
	{
		Dictionary<string, XElement> summaries = new(StringComparer.Ordinal);
		foreach (XElement member in documentation.Root!.Element("members")!.Elements("member"))
		{
			summaries[(string)member.Attribute("name")!] = member;
		}

		List<Type> types = [.. api.GetExportedTypes().OrderBy(type => type.Namespace, StringComparer.Ordinal)
			.ThenBy(DisplayName, StringComparer.Ordinal)];
		int memberCount = types.Sum(type => Members(type).Count() + (type.IsEnum ? EnumValues(type).Count() : 0));

		StringBuilder document = new();
		document.Append("# Scripting API reference\n\n");
		document.Append("<!-- Generated from Strada.ScriptCore by ScriptCoreTests (Tests/ScriptCoreTests/Source/ApiReferenceTests.cs):\n");
		document.Append("     edit the API's XML documentation in Strada-ScriptCore, then run ScriptCoreTests with\n");
		document.Append("     STRADA_UPDATE_DOCS=1. -->\n\n");
		document.Append("Every public type and member of `Strada.ScriptCore`, the API that C# scripts and visual scripts use, with the\n");
		document.Append("summary C# editors show as you type. The [scripting guide](Scripting.md) shows the API in use and the\n");
		document.Append("[visual scripting guide](VisualScripting.md) how graphs call it: every member listed here is a node.\n");
		document.Append("The engine's tests generate this page from the API, so it lists exactly what the API has.\n\n");
		int namespaceCount = types.Select(type => type.Namespace).Distinct().Count();
		document.Append($"{types.Count} types in {namespaceCount} namespaces, with {memberCount} members and enum values in all.\n");

		foreach (IGrouping<string?, Type> space in types.GroupBy(type => type.Namespace))
		{
			document.Append($"\n## {space.Key}\n\n| Type | Kind | Summary |\n|---|---|---|\n");
			foreach (Type type in space)
			{
				document.Append($"| [{DisplayName(type)}](#{Anchor(type)}) | {Kind(type)} | {Summary(summaries, TypeId(type))} |\n");
			}
			foreach (Type type in space)
			{
				WriteType(document, type, summaries);
			}
		}
		return document.ToString();
	}

	private static void WriteType(StringBuilder document, Type type, Dictionary<string, XElement> summaries)
	{
		document.Append($"\n### {DisplayName(type)}\n\n```csharp\n{Declaration(type)}\n```\n\n");
		string summary = Summary(summaries, TypeId(type));
		if (summary.Length > 0)
		{
			document.Append(summary).Append("\n\n");
		}
		if (type.IsEnum)
		{
			List<FieldInfo> values = [.. EnumValues(type)];
			if (values.Any(value => Summary(summaries, "F:" + TypeId(type) + "." + value.Name).Length > 0))
			{
				document.Append("| Value | Summary |\n|---|---|\n");
				foreach (FieldInfo value in values)
				{
					document.Append($"| `{value.Name}` | {Summary(summaries, "F:" + TypeId(type) + "." + value.Name)} |\n");
				}
			}
			else
			{
				document.Append("Values: ").Append(string.Join(", ", values.Select(value => $"`{value.Name}`"))).Append(".\n");
			}
			return;
		}
		List<MemberInfo> members = [.. Members(type)];
		if (members.Count == 0)
		{
			return;
		}
		document.Append("| Member | Summary |\n|---|---|\n");
		foreach (MemberInfo member in members)
		{
			document.Append($"| `{Escape(Signature(member))}` | {Summary(summaries, MemberId(member))} |\n");
		}
	}

	// Members a script can use or override: public ones, and protected ones of types that can be derived from.
	private static IEnumerable<MemberInfo> Members(Type type)
	{
		if (type.IsEnum || typeof(Delegate).IsAssignableFrom(type))
		{
			return [];
		}
		bool derivable = !type.IsSealed;
		bool Visible(MethodBase? method) =>
			method is not null && (method.IsPublic || (derivable && (method.IsFamily || method.IsFamilyOrAssembly)));

		IEnumerable<MemberInfo> constructors =
			type.GetConstructors(Declared).Where(constructor => !constructor.IsStatic && Visible(constructor));
		IEnumerable<MemberInfo> fields = type.GetFields(Declared).Where(field =>
			!field.IsSpecialName && (field.IsPublic || (derivable && (field.IsFamily || field.IsFamilyOrAssembly))));
		IEnumerable<MemberInfo> properties =
			type.GetProperties(Declared).Where(property => Visible(property.GetMethod) || Visible(property.SetMethod));
		IEnumerable<MemberInfo> methods = type.GetMethods(Declared).Where(method =>
			Visible(method) && (!method.IsSpecialName || s_Operators.ContainsKey(method.Name) || IsConversion(method)) &&
			method.GetBaseDefinition().DeclaringType != typeof(object));
		IEnumerable<MemberInfo> events = type.GetEvents(Declared).Where(@event => Visible(@event.AddMethod));
		return constructors.OrderBy(member => member.MetadataToken)
			.Concat(fields.OrderBy(member => member.MetadataToken))
			.Concat(properties.OrderBy(member => member.MetadataToken))
			.Concat(methods.OrderBy(member => member.MetadataToken))
			.Concat(events.OrderBy(member => member.MetadataToken));
	}

	private static IEnumerable<FieldInfo> EnumValues(Type type) =>
		type.GetFields(BindingFlags.Public | BindingFlags.Static).OrderBy(field => field.MetadataToken);

	private static bool IsConversion(MethodInfo method) => method.Name is "op_Implicit" or "op_Explicit";

	private static string Kind(Type type)
	{
		if (type.IsEnum)
		{
			return "enum";
		}
		if (type.IsInterface)
		{
			return "interface";
		}
		if (type.IsValueType)
		{
			return "struct";
		}
		if (typeof(Delegate).IsAssignableFrom(type))
		{
			return "delegate";
		}
		if (type.IsAbstract && type.IsSealed)
		{
			return "static class";
		}
		if (typeof(Attribute).IsAssignableFrom(type))
		{
			return "attribute";
		}
		return type.IsAbstract ? "abstract class" : "class";
	}

	private static string Declaration(Type type)
	{
		string modifiers = type.IsEnum || type.IsInterface || type.IsValueType || typeof(Delegate).IsAssignableFrom(type)
			? ""
			: type.IsAbstract && type.IsSealed ? "static " : type.IsAbstract ? "abstract " : type.IsSealed ? "sealed " : "";
		string keyword = type.IsEnum ? "enum" : type.IsInterface ? "interface" : type.IsValueType ? "struct" :
			typeof(Delegate).IsAssignableFrom(type) ? "delegate" : "class";
		if (keyword == "delegate")
		{
			MethodInfo invoke = type.GetMethod("Invoke")!;
			string returns = TypeName(invoke.ReturnType, s_Nullability.Create(invoke.ReturnParameter));
			return $"public delegate {returns} {DisplayName(type)}({Parameters(invoke)})";
		}
		List<string> bases = [];
		if (type.IsEnum)
		{
			Type underlying = Enum.GetUnderlyingType(type);
			if (underlying != typeof(int))
			{
				bases.Add(TypeName(underlying, null));
			}
		}
		else
		{
			if (type.BaseType is { } baseType && baseType != typeof(object) && baseType != typeof(ValueType))
			{
				bases.Add(TypeName(baseType, null));
			}
			Type[] inherited = type.BaseType?.GetInterfaces() ?? [];
			bases.AddRange(type.GetInterfaces().Where(face => !inherited.Contains(face)).Select(face => TypeName(face, null))
				.OrderBy(name => name, StringComparer.Ordinal));
		}
		string flags = type.IsEnum && type.IsDefined(typeof(FlagsAttribute)) ? "[Flags]\n" : "";
		return $"{flags}public {modifiers}{keyword} {DisplayName(type)}{(bases.Count > 0 ? " : " + string.Join(", ", bases) : "")}";
	}

	private static string Signature(MemberInfo member)
	{
		switch (member)
		{
			case ConstructorInfo constructor:
				return $"{Access(constructor)}{DisplayName(constructor.DeclaringType!).Split('.')[^1]}({Parameters(constructor)})";
			case FieldInfo field:
				{
					string type = TypeName(field.FieldType, s_Nullability.Create(field));
					if (field.IsLiteral)
					{
						return $"const {type} {field.Name} = {Literal(field.GetRawConstantValue(), field.FieldType)}";
					}
					string modifiers = (field.IsFamily || field.IsFamilyOrAssembly ? "protected " : "") +
						(field.IsStatic ? "static " : "") + (field.IsInitOnly ? "readonly " : "");
					return $"{modifiers}{type} {field.Name}";
				}
			case PropertyInfo property:
				{
					MethodInfo accessor = (property.GetMethod ?? property.SetMethod)!;
					string type = TypeName(property.PropertyType, s_Nullability.Create(property));
					ParameterInfo[] index = property.GetIndexParameters();
					string name = index.Length > 0 ? $"this[{string.Join(", ", index.Select(Parameter))}]" : property.Name;
					List<string> accessors = [];
					if (IsVisibleAccessor(property.GetMethod, property))
					{
						accessors.Add("get;");
					}
					if (IsVisibleAccessor(property.SetMethod, property))
					{
						bool init = property.SetMethod!.ReturnParameter.GetRequiredCustomModifiers()
							.Any(modifier => modifier.FullName == "System.Runtime.CompilerServices.IsExternalInit");
						accessors.Add(init ? "init;" : "set;");
					}
					return $"{Access(accessor)}{Modifiers(accessor)}{type} {name} {{ {string.Join(" ", accessors)} }}";
				}
			case MethodInfo method:
				{
					string returns = TypeName(method.ReturnType, s_Nullability.Create(method.ReturnParameter));
					if (IsConversion(method))
					{
						string direction = method.Name == "op_Implicit" ? "implicit" : "explicit";
						return $"static {direction} operator {returns}({Parameters(method)})";
					}
					if (s_Operators.TryGetValue(method.Name, out string? symbol))
					{
						return $"static {returns} operator {symbol}({Parameters(method)})";
					}
					string generics = method.IsGenericMethodDefinition
						? $"<{string.Join(", ", method.GetGenericArguments().Select(argument => argument.Name))}>"
						: "";
					return $"{Access(method)}{Modifiers(method)}{returns} {method.Name}{generics}({Parameters(method)})";
				}
			case EventInfo @event:
				{
					string handler = TypeName(@event.EventHandlerType!, s_Nullability.Create(@event));
					string modifiers = Access(@event.AddMethod!) + (@event.AddMethod!.IsStatic ? "static " : "");
					return $"{modifiers}event {handler} {@event.Name}";
				}
			default:
				return member.Name;
		}
	}

	private static bool IsVisibleAccessor(MethodInfo? accessor, PropertyInfo property) =>
		accessor is not null &&
		(accessor.IsPublic || (!property.DeclaringType!.IsSealed && (accessor.IsFamily || accessor.IsFamilyOrAssembly)));

	private static string Access(MethodBase method) => method.IsPublic ? "" : "protected ";

	private static string Modifiers(MethodInfo method)
	{
		if (method.IsStatic)
		{
			return "static ";
		}
		if (method.IsAbstract && !method.DeclaringType!.IsInterface)
		{
			return "abstract ";
		}
		bool overrides = method.GetBaseDefinition().DeclaringType != method.DeclaringType;
		if (overrides)
		{
			return "override ";
		}
		return method.IsVirtual && !method.IsFinal && !method.DeclaringType!.IsInterface ? "virtual " : "";
	}

	private static string Parameters(MethodBase method) => string.Join(", ", method.GetParameters().Select(Parameter));

	private static string Parameter(ParameterInfo parameter)
	{
		Type type = parameter.ParameterType;
		string prefix = "";
		if (type.IsByRef)
		{
			prefix = parameter.IsOut ? "out " : parameter.IsIn ? "in " : "ref ";
		}
		else if (parameter.IsDefined(typeof(ParamArrayAttribute)))
		{
			prefix = "params ";
		}
		string text = $"{prefix}{TypeName(type, s_Nullability.Create(parameter))} {parameter.Name}";
		if (parameter.HasDefaultValue)
		{
			text += " = " + Literal(parameter.RawDefaultValue, type.IsByRef ? type.GetElementType()! : type);
		}
		return text;
	}

	private static string Literal(object? value, Type type)
	{
		Type underlying = Nullable.GetUnderlyingType(type) ?? type;
		return value switch
		{
			null => underlying.IsValueType ? "default" : "null",
			bool flag => flag ? "true" : "false",
			string text => $"\"{text}\"",
			char character => $"'{character}'",
			_ when underlying.IsEnum => $"{DisplayName(underlying)}.{Enum.ToObject(underlying, value)}",
			float number => FormatFloat(number.ToString("R", CultureInfo.InvariantCulture), "f"),
			double number => FormatFloat(number.ToString("R", CultureInfo.InvariantCulture), ""),
			IFormattable number => number.ToString(null, CultureInfo.InvariantCulture),
			_ => value.ToString() ?? "",
		};
	}

	private static string FormatFloat(string text, string suffix) =>
		text.Contains('.', StringComparison.Ordinal) || text.Contains('E', StringComparison.Ordinal) ? text + suffix : text;

	// A type as C# source names it: keywords, generic arguments, arrays, and ? where the API says it may be null.
	private static string TypeName(Type type, NullabilityInfo? nullability)
	{
		if (type.IsByRef)
		{
			return TypeName(type.GetElementType()!, nullability);
		}
		if (Nullable.GetUnderlyingType(type) is { } underlying)
		{
			return TypeName(underlying, null) + "?";
		}
		string name;
		if (type.IsArray)
		{
			name = TypeName(type.GetElementType()!, nullability?.ElementType) + "[]";
		}
		else if (type.IsGenericParameter)
		{
			name = type.Name;
		}
		else if (s_Keywords.TryGetValue(type, out string? keyword))
		{
			name = keyword;
		}
		else if (type.IsGenericType)
		{
			Type[] arguments = type.GetGenericArguments();
			NullabilityInfo[] argumentNullability = nullability?.GenericTypeArguments ?? [];
			IEnumerable<string> argumentNames = arguments.Select((argument, index) =>
				TypeName(argument, index < argumentNullability.Length ? argumentNullability[index] : null));
			string baseName = type.Name[..type.Name.IndexOf('`', StringComparison.Ordinal)];
			name = $"{baseName}<{string.Join(", ", argumentNames)}>";
		}
		else
		{
			name = DisplayName(type);
		}
		bool nullable = !type.IsValueType && nullability?.ReadState == NullabilityState.Nullable;
		return nullable ? name + "?" : name;
	}

	private static string DisplayName(Type type) =>
		type.IsNested ? $"{DisplayName(type.DeclaringType!)}.{type.Name}" : type.Name;

	private static string Anchor(Type type) => DisplayName(type).ToLowerInvariant().Replace(".", "", StringComparison.Ordinal);

	// XML documentation IDs (ECMA-334 annex D): T:, F:, P:, M: and E: with full names, generic arities and parameter types.
	private static string TypeId(Type type) => "T:" + TypeKey(type);

	private static string TypeKey(Type type) => type.FullName!.Replace('+', '.');

	private static string MemberId(MemberInfo member)
	{
		string owner = TypeKey(member.DeclaringType!);
		switch (member)
		{
			case ConstructorInfo constructor:
				return $"M:{owner}.#ctor{ParameterList(constructor.GetParameters())}";
			case MethodInfo method:
				{
					string arity = method.IsGenericMethodDefinition ? $"``{method.GetGenericArguments().Length}" : "";
					string conversion = IsConversion(method) ? "~" + ParameterKey(method.ReturnType) : "";
					return $"M:{owner}.{method.Name}{arity}{ParameterList(method.GetParameters())}{conversion}";
				}
			case PropertyInfo property:
				return $"P:{owner}.{property.Name}{ParameterList(property.GetIndexParameters())}";
			case FieldInfo field:
				return $"F:{owner}.{field.Name}";
			case EventInfo @event:
				return $"E:{owner}.{@event.Name}";
			default:
				return member.Name;
		}
	}

	private static string ParameterList(ParameterInfo[] parameters) =>
		parameters.Length == 0 ? "" : $"({string.Join(",", parameters.Select(parameter => ParameterKey(parameter.ParameterType)))})";

	private static string ParameterKey(Type type)
	{
		if (type.IsByRef)
		{
			return ParameterKey(type.GetElementType()!) + "@";
		}
		if (type.IsPointer)
		{
			return ParameterKey(type.GetElementType()!) + "*";
		}
		if (type.IsArray)
		{
			return ParameterKey(type.GetElementType()!) + "[]";
		}
		if (type.IsGenericMethodParameter)
		{
			return "``" + type.GenericParameterPosition.ToString(CultureInfo.InvariantCulture);
		}
		if (type.IsGenericTypeParameter)
		{
			return "`" + type.GenericParameterPosition.ToString(CultureInfo.InvariantCulture);
		}
		if (type.IsConstructedGenericType)
		{
			string definition = TypeKey(type.GetGenericTypeDefinition());
			definition = definition[..definition.LastIndexOf('`')];
			return $"{definition}{{{string.Join(",", type.GetGenericArguments().Select(ParameterKey))}}}";
		}
		return TypeKey(type);
	}

	// A member's summary as one line of Markdown: references and code in backquotes, paragraphs joined.
	private static string Summary(Dictionary<string, XElement> summaries, string id)
	{
		if (!summaries.TryGetValue(id, out XElement? member) || member.Element("summary") is not { } summary)
		{
			return "";
		}
		StringBuilder text = new();
		AppendNodes(text, summary.Nodes());
		string line = string.Join(" ", text.ToString().Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
		return Escape(line);
	}

	private static void AppendNodes(StringBuilder text, IEnumerable<XNode> nodes)
	{
		foreach (XNode node in nodes)
		{
			if (node is XText content)
			{
				text.Append(content.Value);
				continue;
			}
			if (node is not XElement element)
			{
				continue;
			}
			switch (element.Name.LocalName)
			{
				case "see":
				case "seealso":
					if (element.Attribute("langword") is { } langword)
					{
						text.Append('`').Append(langword.Value).Append('`');
					}
					else if (element.Attribute("cref") is { } cref)
					{
						text.Append('`').Append(element.IsEmpty ? ReferenceName(cref.Value) : element.Value).Append('`');
					}
					else
					{
						AppendNodes(text, element.Nodes());
					}
					break;
				case "paramref":
				case "typeparamref":
					text.Append('`').Append((string?)element.Attribute("name")).Append('`');
					break;
				case "c":
					text.Append('`').Append(element.Value).Append('`');
					break;
				case "para":
					text.Append(' ');
					AppendNodes(text, element.Nodes());
					text.Append(' ');
					break;
				default:
					AppendNodes(text, element.Nodes());
					break;
			}
		}
	}

	// "M:Strada.Physics.Raycast(Strada.Vector3,...)" reads as "Physics.Raycast".
	private static string ReferenceName(string cref)
	{
		string name = cref.Length > 2 && cref[1] == ':' ? cref[2..] : cref;
		int parameters = name.IndexOf('(', StringComparison.Ordinal);
		if (parameters >= 0)
		{
			name = name[..parameters];
		}
		name = name.Replace("``", "`", StringComparison.Ordinal);
		int arity = name.IndexOf('`', StringComparison.Ordinal);
		if (arity >= 0)
		{
			name = name[..arity];
		}
		foreach (string space in new[] { "Strada.Testing.", "Strada.", "System." })
		{
			if (name.StartsWith(space, StringComparison.Ordinal))
			{
				return name[space.Length..];
			}
		}
		return name;
	}

	// A table cell ends at a bar.
	private static string Escape(string text) => text.Replace("|", "\\|", StringComparison.Ordinal);
}

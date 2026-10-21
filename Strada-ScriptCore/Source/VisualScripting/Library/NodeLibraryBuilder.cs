using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using Strada.Interop;

namespace Strada.VisualScripting;

// What a node library is built from.
internal sealed class NodeLibrarySources
{
	// The types whose public members become nodes and which are library types themselves (unless static): the exported
	// types of Strada.ScriptCore in the namespaces Strada and Strada.Testing.
	internal required IReadOnlyList<Type> ApiTypes { get; init; }

	// The game assembly's script classes: library types (Kind Entity) and the domain of Script type arguments.
	internal IReadOnlyList<Type> ScriptClasses { get; init; } = [];

	internal IReadOnlyList<NodeRedirect> Redirects { get; init; } = NodeRedirects.Current;

	// The XML documentation of the API; null for no descriptions.
	internal string? DocumentationPath { get; init; }

	// The scripting API with the game's script classes: what the engine's library is built from.
	internal static NodeLibrarySources ForScriptCore(IReadOnlyList<Type> scriptClasses) => new()
	{
		ApiTypes = GetApiTypes(),
		ScriptClasses = scriptClasses,
		DocumentationPath = GetDocumentationPath(),
	};

	internal static IReadOnlyList<Type> GetApiTypes()
	{
		List<Type> types = [];
		foreach (Type type in typeof(Entity).Assembly.GetExportedTypes())
		{
			if (type.Namespace is "Strada" or "Strada.Testing")
			{
				types.Add(type);
			}
		}
		types.Sort(static (a, b) => string.CompareOrdinal(a.FullName, b.FullName));
		return types;
	}

	// Strada.ScriptCore.xml next to the assembly, when it is there.
	internal static string? GetDocumentationPath()
	{
		string location = typeof(Entity).Assembly.Location;
		if (location.Length == 0)
		{
			return null;
		}
		string path = Path.ChangeExtension(location, ".xml");
		return File.Exists(path) ? path : null;
	}
}

// Builds node libraries: registers the types, makes the built-in and reflected nodes and derives the facts of every type
// (spec sections 3, 4 and 6.1). Deterministic: the same sources give the same library, whatever the culture.
internal static class NodeLibraryBuilder
{
	internal static NodeLibrary Build(NodeLibrarySources sources)
	{
		Registry registry = new();
		foreach (Type type in sources.ApiTypes)
		{
			if (IsTypeOfValues(type))
			{
				registry.Register(type, KindOf(type));
			}
		}
		// The types built-in nodes use, whatever the sources hold.
		foreach (Type type in new[] { typeof(Entity), typeof(KeyCode), typeof(MouseButton), typeof(GamepadButton) })
		{
			registry.Register(type, KindOf(type));
		}
		List<Type> scriptClasses = [.. sources.ScriptClasses];
		scriptClasses.Sort(static (a, b) => string.CompareOrdinal(a.FullName, b.FullName));
		foreach (Type type in scriptClasses)
		{
			if (type.IsSubclassOf(typeof(Script)) && !type.IsGenericType && TypeIds.Of(type) is { } id && registry.FindId(id) == null)
			{
				registry.Register(type, TypeKind.Entity);
			}
		}

		BuiltinTypes builtinTypes = new(registry.Get(typeof(bool))!, registry.Get(typeof(int))!, registry.Get(typeof(float))!,
			registry.Get(typeof(string))!, registry.Get(typeof(Entity))!, registry.Get(typeof(KeyCode))!, registry.Get(typeof(MouseButton))!,
			registry.Get(typeof(GamepadButton))!);
		List<NodeDefinition> nodes = [.. BuiltinNodes.Create(builtinTypes)];
		List<MemberNodeDefinition> members = [];
		MemberNodes.Create(sources.ApiTypes, registry.Get, builtinTypes.Entity, Documentation.Load(sources.DocumentationPath), members);
		HashSet<string> typeStrings = new(StringComparer.Ordinal);
		foreach (NodeDefinition node in nodes)
		{
			typeStrings.Add(node.Type);
		}
		foreach (MemberNodeDefinition member in members)
		{
			if (typeStrings.Add(member.Type))
			{
				nodes.Add(member);
			}
		}
		nodes.Sort(static (a, b) =>
		{
			int order = string.CompareOrdinal(a.Category, b.Category);
			order = order != 0 ? order : string.CompareOrdinal(a.Title, b.Title);
			return order != 0 ? order : string.CompareOrdinal(a.Type, b.Type);
		});

		List<VisualType> types = registry.All();
		foreach (VisualType type in types)
		{
			DeriveFacts(type, registry);
		}
		foreach (VisualType type in types)
		{
			type.ImplicitTo = GetImplicitTargets(type, registry);
		}
		types.Add(VisualType.Exec);
		types.Sort(static (a, b) => string.CompareOrdinal(a.Id, b.Id));

		List<VisualType> components = [];
		List<VisualType> assets = [];
		List<VisualType> scripts = [];
		foreach (VisualType type in types)
		{
			Type? clrType = type.ClrType;
			if (clrType == null || clrType.IsAbstract)
			{
				continue;
			}
			if (type.Kind == TypeKind.Component && clrType.IsDefined(typeof(NativeComponentAttribute), false))
			{
				components.Add(type);
			}
			else if (type.Kind == TypeKind.Asset)
			{
				assets.Add(type);
			}
			else if (type.IsScriptClass && scriptClasses.Contains(clrType))
			{
				scripts.Add(type);
			}
		}
		return new NodeLibrary(types, nodes, sources.Redirects, builtinTypes, new LibraryDomains(components, assets, scripts));
	}

	// API types with values: not static classes, interfaces, delegates, attributes, exceptions or generic types.
	private static bool IsTypeOfValues(Type type) =>
		!(type.IsAbstract && type.IsSealed) && !type.IsInterface && !type.IsGenericType && !type.IsPointer &&
		!typeof(Delegate).IsAssignableFrom(type) && !typeof(Attribute).IsAssignableFrom(type) && !typeof(Exception).IsAssignableFrom(type);

	private static TypeKind KindOf(Type type)
	{
		if (type.IsEnum)
		{
			return TypeKind.Enum;
		}
		if (type.IsValueType)
		{
			return type == typeof(Vector2) || type == typeof(Vector3) || type == typeof(Vector4) ? TypeKind.Vector
				: type == typeof(Quaternion) ? TypeKind.Quaternion
				: type == typeof(Color) ? TypeKind.Color
				: type == typeof(Matrix4) ? TypeKind.Matrix
				: TypeKind.Struct;
		}
		return typeof(Entity).IsAssignableFrom(type) ? TypeKind.Entity
			: typeof(Component).IsAssignableFrom(type) ? TypeKind.Component
			: typeof(Asset).IsAssignableFrom(type) ? TypeKind.Asset
			: TypeKind.Class;
	}

	private static void DeriveFacts(VisualType type, Registry registry)
	{
		Type clrType = type.ClrType!;
		type.FieldType = type.IsScriptClass ? ScriptFieldType.None : ScriptFieldCodec.GetFieldType(clrType);
		type.AssetType = type.Kind == TypeKind.Asset && !clrType.IsAbstract ? AssetTypes.GetNativeType(clrType) : NativeAssetType.None;
		type.HasLiteral = LiteralCodec.HasLiteralForm(clrType);
		if (clrType.IsEnum)
		{
			List<EnumeratorInfo> enumerators = [];
			foreach (FieldInfo field in SortedByToken(clrType.GetFields(BindingFlags.Public | BindingFlags.Static)))
			{
				enumerators.Add(new EnumeratorInfo(field.Name, field.GetValue(null)!));
			}
			type.Enumerators = enumerators;
			type.IsFlags = clrType.IsDefined(typeof(FlagsAttribute), false);
		}
		else if (clrType.IsValueType && !clrType.IsPrimitive)
		{
			DeriveStructMembers(type, clrType, registry);
		}
		else if (clrType.IsClass && clrType != typeof(object) && clrType != typeof(string) && !clrType.IsArray)
		{
			for (Type? baseType = clrType.BaseType; baseType != null && baseType != typeof(object); baseType = baseType.BaseType)
			{
				if (registry.Find(baseType) is { } library)
				{
					type.Base = library;
					break;
				}
			}
		}
	}

	// Make sets the public instance fields of structs whose fields are all writable; Break reads them, or the public get-only
	// properties of readonly structs without public fields (spec section 4.4).
	private static void DeriveStructMembers(VisualType type, Type clrType, Registry registry)
	{
		List<FieldInfo> fields = SortedByToken(clrType.GetFields(BindingFlags.Public | BindingFlags.Instance));
		List<StructMember> members = [];
		if (fields.Count > 0)
		{
			foreach (FieldInfo field in fields)
			{
				if (field.IsInitOnly || registry.Find(field.FieldType) is not { } fieldType)
				{
					return;
				}
				members.Add(new StructMember(field.Name, fieldType, field));
			}
			type.Makeable = true;
			type.Breakable = true;
			type.Fields = members;
			return;
		}
		if (!clrType.IsDefined(typeof(IsReadOnlyAttribute), false))
		{
			return;
		}
		foreach (PropertyInfo property in SortedByToken(clrType.GetProperties(BindingFlags.Public | BindingFlags.Instance)))
		{
			if (property.GetIndexParameters().Length > 0 || property.GetMethod is not { IsPublic: true } || property.SetMethod is { IsPublic: true })
			{
				continue;
			}
			if (registry.Find(property.PropertyType) is not { } propertyType)
			{
				return;
			}
			members.Add(new StructMember(property.Name, propertyType, property));
		}
		if (members.Count > 0)
		{
			type.Breakable = true;
			type.Fields = members;
		}
	}

	// Spec section 3.4 rules 4, 5 and 6, in a fixed order: the widened types narrowest first, the base classes nearest first
	// (script classes up to Strada.Entity, typed assets up to Strada.Asset), a component's entity, a typed asset's handle,
	// System.Object.
	private static List<VisualType> GetImplicitTargets(VisualType type, Registry registry)
	{
		List<VisualType> targets = [];
		Type clrType = type.ClrType!;
		Type[] widened = clrType == typeof(int) ? [typeof(long), typeof(float), typeof(double)]
			: clrType == typeof(uint) ? [typeof(long), typeof(ulong), typeof(float), typeof(double)]
			: clrType == typeof(long) || clrType == typeof(ulong) ? [typeof(float), typeof(double)]
			: clrType == typeof(float) ? [typeof(double)]
			: [];
		foreach (Type target in widened)
		{
			targets.Add(registry.Find(target)!);
		}
		if (type.IsScriptClass || type.IsTypedAsset)
		{
			for (VisualType? baseType = type.Base; baseType != null; baseType = baseType.Base)
			{
				targets.Add(baseType);
			}
		}
		if (type.Kind == TypeKind.Component && registry.Find(typeof(Entity)) is { } entity)
		{
			targets.Add(entity);
		}
		if (type.IsTypedAsset && registry.Find(typeof(AssetHandle)) is { } handle)
		{
			targets.Add(handle);
		}
		if (clrType != typeof(object))
		{
			targets.Add(registry.Find(typeof(object))!);
		}
		return targets;
	}

	private static List<T> SortedByToken<T>(T[] members)
		where T : MemberInfo
	{
		List<T> sorted = [.. members];
		sorted.Sort(static (a, b) => a.MetadataToken.CompareTo(b.MetadataToken));
		return sorted;
	}

	// The types of the library being built, by CLR type and ID. Array types are registered when a member signature uses
	// them.
	private sealed class Registry
	{
		private readonly Dictionary<Type, VisualType> m_ByClr = [];
		private readonly Dictionary<string, VisualType> m_ById = new(StringComparer.Ordinal);

		public Registry()
		{
			foreach (Type type in new[] { typeof(bool), typeof(int), typeof(uint), typeof(long), typeof(ulong), typeof(float), typeof(double),
				typeof(string), typeof(object) })
			{
				Register(type, type == typeof(bool) ? TypeKind.Bool
					: type == typeof(string) ? TypeKind.String
					: type == typeof(object) ? TypeKind.Object
					: type == typeof(float) || type == typeof(double) ? TypeKind.Float
					: TypeKind.Integer);
			}
		}

		public void Register(Type type, TypeKind kind)
		{
			if (m_ByClr.ContainsKey(type) || TypeIds.Of(type) is not { } id || m_ById.ContainsKey(id))
			{
				return;
			}
			VisualType visualType = new(id, Titles.OfType(type), kind, type);
			m_ByClr[type] = visualType;
			m_ById[id] = visualType;
		}

		public VisualType? FindId(string id) => m_ById.TryGetValue(id, out VisualType? type) ? type : null;

		// A registered type; never registers one.
		public VisualType? Find(Type type) => m_ByClr.TryGetValue(type, out VisualType? visualType) ? visualType : null;

		public VisualType? Get(Type type)
		{
			if (m_ByClr.TryGetValue(type, out VisualType? visualType))
			{
				return visualType;
			}
			if (!type.IsSZArray || type.GetElementType() is not { IsArray: false } elementType || Get(elementType) is not { } element)
			{
				return null;
			}
			Register(type, TypeKind.Array);
			if (m_ByClr.TryGetValue(type, out VisualType? array))
			{
				array.Element = element;
				return array;
			}
			return null;
		}

		public List<VisualType> All() => [.. m_ByClr.Values];
	}
}

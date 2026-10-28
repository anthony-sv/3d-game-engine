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

	// The game assembly's public types: what [VisualNode] marks becomes nodes and types of graphs (spec-slice3 section 3.7).
	internal IReadOnlyList<Type> GameTypes { get; init; } = [];

	internal IReadOnlyList<NodeRedirect> Redirects { get; init; } = NodeRedirects.Current;

	// The XML documentation of the API; null for no descriptions.
	internal string? DocumentationPath { get; init; }

	// The scripting API with the game's script classes and public types: what the engine's library is built from.
	internal static NodeLibrarySources ForScriptCore(IReadOnlyList<Type> scriptClasses, IReadOnlyList<Type> gameTypes) => new()
	{
		ApiTypes = GetApiTypes(),
		ScriptClasses = scriptClasses,
		GameTypes = gameTypes,
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
// (spec sections 3, 4 and 6.1), with the game's [VisualNode] members and types (spec-slice3 section 3.7.4).
// Deterministic: the same sources give the same library, whatever the culture. It never logs: the notes of the game's
// members are the library's (EngineLibrary logs them).
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
		foreach (Type type in new[] { typeof(Entity), typeof(KeyCode), typeof(MouseButton), typeof(GamepadButton), typeof(TimelineDirection) })
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
		List<LibraryNote> notes = [];
		List<GameType> gameTypes = RegisterGameTypes(sources, registry, notes);

		BuiltinTypes builtinTypes = new(registry.Get(typeof(bool))!, registry.Get(typeof(int))!, registry.Get(typeof(float))!,
			registry.Get(typeof(string))!, registry.Get(typeof(Entity))!, registry.Get(typeof(KeyCode))!, registry.Get(typeof(MouseButton))!,
			registry.Get(typeof(GamepadButton))!, registry.Get(typeof(string[]))!, registry.Get(typeof(TimelineDirection))!);
		List<NodeDefinition> nodes = [.. BuiltinNodes.Create(builtinTypes)];
		List<MemberNodeDefinition> members = [];
		MemberNodes.Create(sources.ApiTypes, registry.Get, builtinTypes.Entity, Documentation.Load(sources.DocumentationPath), members);
		List<GameRename> renames = [];
		foreach (GameType game in gameTypes)
		{
			ReadGameType(game, notes, () => GameNodes.Create(game.Type, game.Attribute, registry.Get, builtinTypes.Entity, members, notes, renames));
		}
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

		// Every type with values has an array type, whether or not a member uses it (spec-slice3 section 3.4.1).
		registry.RegisterArrays();
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
			if (type.Kind == TypeKind.Component && IsDefined(clrType, typeof(NativeComponentAttribute)))
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
		List<NodeRedirect> redirects = [.. sources.Redirects, .. GameNodes.CreateRedirects(renames, nodes, sources.Redirects, notes)];
		return new NodeLibrary(types, nodes, redirects, builtinTypes, new LibraryDomains(components, assets, scripts), GameNodes.Order(notes));
	}

	// The game types whose members may be nodes (every public type but those named like an API type, by full name, and those
	// whose attributes cannot be read), with their [VisualNode]s, after registering the types of graphs among them
	// (spec-slice3 section 3.7.4): the types [VisualNode] marks, then the game types the marked members' signatures use. A
	// type's attributes are read here once: a type whose attributes cannot be read (an attribute of a dependency of the game
	// that is missing or broken) is left out with a note, so nothing reads them later.
	private static List<GameType> RegisterGameTypes(NodeLibrarySources sources, Registry registry, List<LibraryNote> notes)
	{
		List<GameType> gameTypes = [];
		Dictionary<Type, VisualNodeAttribute?> attributes = [];
		HashSet<Type> seen = [];
		foreach (Type type in sources.GameTypes)
		{
			if (!seen.Add(type))
			{
				continue;
			}
			if (!GameNodes.TryReadAttribute(type, out VisualNodeAttribute? attribute, out string problem))
			{
				NoteUnreadable(type, GameNodes.DefaultCategoryOf(type), problem, notes);
				continue;
			}
			if (registry.Find(type) == null && TypeIds.Of(type) is { } id && registry.FindId(id) != null)
			{
				notes.Add(new LibraryNote(id, GameNodes.CategoryOf(type, attribute),
					$"{id}: the game type has the name of a scripting API type and is not a type of graphs"));
				continue;
			}
			attributes.Add(type, attribute);
			gameTypes.Add(new GameType(type, attribute));
		}
		gameTypes.Sort(static (a, b) => string.CompareOrdinal(a.Type.FullName, b.Type.FullName));
		foreach (GameType game in gameTypes)
		{
			if (game.Attribute != null)
			{
				RegisterGameType(game.Type, attributes, registry, notes);
			}
		}
		foreach (GameType game in gameTypes)
		{
			ReadGameType(game, notes, () =>
			{
				foreach (Type used in GameNodes.FindSignatureTypes(game.Type, game.Attribute, attributes.ContainsKey))
				{
					if (!used.IsByRefLike)
					{
						RegisterGameType(used, attributes, registry, notes);
					}
				}
			});
		}
		return gameTypes;
	}

	// Reads the members of a game type: when reflection cannot load what they use (a dependency of the game that is
	// missing or broken), the type gives the members it gave until then and a note.
	private static void ReadGameType(GameType game, List<LibraryNote> notes, Action read)
	{
		try
		{
			read();
		}
		catch (Exception exception) when (GameNodes.IsReadFailure(exception))
		{
			NoteUnreadable(game.Type, GameNodes.CategoryOf(game.Type, game.Attribute), exception.Message, notes);
		}
	}

	private static void NoteUnreadable(Type type, string category, string problem, List<LibraryNote> notes)
	{
		string id = MemberNodes.DisplayName(type);
		notes.Add(new LibraryNote(id, category, $"{id}: its members cannot be read: {problem}"));
	}

	// A game type of graphs: registered with the kind of its values, unless it has none (a static class, a generic type).
	// A ref struct never is one: graphs cannot hold its values.
	private static void RegisterGameType(Type type, Dictionary<Type, VisualNodeAttribute?> attributes, Registry registry, List<LibraryNote> notes)
	{
		if (type.IsByRefLike)
		{
			string id = MemberNodes.DisplayName(type);
			notes.Add(new LibraryNote(id, GameNodes.CategoryOf(type, attributes.GetValueOrDefault(type)), $"{id} is a ref struct: graphs cannot hold its values"));
			return;
		}
		if (IsTypeOfValues(type))
		{
			registry.Register(type, KindOf(type));
		}
	}

	// API types with values: not static classes, interfaces, delegates, attributes, exceptions, generic types or ref structs
	// (their values cannot be boxed into slots).
	private static bool IsTypeOfValues(Type type) =>
		!(type.IsAbstract && type.IsSealed) && !type.IsInterface && !type.IsGenericType && !type.IsPointer && !type.IsByRefLike &&
		!typeof(Delegate).IsAssignableFrom(type) && !typeof(Attribute).IsAssignableFrom(type) && !typeof(Exception).IsAssignableFrom(type);

	// Whether a type of graphs has an attribute. Registered game types had their attributes read already; a type whose
	// attributes cannot be read has none, as for the game types RegisterGameTypes leaves out.
	private static bool IsDefined(Type type, Type attribute)
	{
		try
		{
			return type.IsDefined(attribute, false);
		}
		catch (Exception exception) when (GameNodes.IsReadFailure(exception))
		{
			return false;
		}
	}

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
		if (type.Kind == TypeKind.Array)
		{
			// Arrays are not exposable (public array variables are a later change), and have literals when their elements do.
			type.HasLiteral = LiteralCodec.HasLiteralForm(clrType);
			return;
		}
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
			type.IsFlags = IsDefined(clrType, typeof(FlagsAttribute));
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
		if (!IsDefined(clrType, typeof(IsReadOnlyAttribute)))
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
	// them, and then for every other type with values.
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
			return RegisterArray(element);
		}

		// The array type of every registered type that can have one.
		public void RegisterArrays()
		{
			foreach (VisualType element in All())
			{
				if (element.Kind != TypeKind.Array && !element.ClrType!.IsByRefLike)
				{
					RegisterArray(element);
				}
			}
		}

		public List<VisualType> All() => [.. m_ByClr.Values];

		private VisualType? RegisterArray(VisualType element)
		{
			if (element.ArrayType != null)
			{
				return element.ArrayType;
			}
			Type elementType = element.ClrType!;
			Type arrayType = elementType.MakeArrayType();
			Register(arrayType, TypeKind.Array);
			if (!m_ByClr.TryGetValue(arrayType, out VisualType? array) || array.Kind != TypeKind.Array)
			{
				return null;
			}
			array.Element = element;
			array.EmptyArray = Array.CreateInstance(elementType, 0);
			element.ArrayType = array;
			return array;
		}
	}
}

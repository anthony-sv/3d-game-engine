using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Strada.VisualScripting;

// A game member's node and the former names its [VisualNode] gives it, with what its notes need.
internal sealed record GameRename(MemberNodeDefinition Node, string Member, string Category, IReadOnlyList<string> FormerNames);

// Nodes of the game assembly's members that [VisualNode] marks (spec-slice3 section 3.7): which members become nodes, with
// the titles, categories, descriptions, keywords and purity their attributes give; the notes of marked members that have
// no node and of attribute values outside their limits; and the redirects of their former names. Members are visited in
// metadata order and types in the order given (by full name), so the same assembly gives the same nodes and notes.
internal static class GameNodes
{
	internal const int MaxTitleLength = 128;
	internal const int MaxCategorySegments = 8;
	internal const int MaxSegmentLength = 64;
	internal const int MaxDescriptionLength = 4096;
	internal const int MaxKeywordLength = 64;
	internal const string CategoryRoot = "Game/";

	private const BindingFlags Declared = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static |
		BindingFlags.DeclaredOnly;

	internal static VisualNodeAttribute? AttributeOf(MemberInfo member) => member.GetCustomAttribute<VisualNodeAttribute>(false);

	// Where the palette lists a game type's nodes: its [VisualNode]'s Category when it is valid, else "Game/<type title>".
	internal static string CategoryOf(Type type) =>
		AttributeOf(type) is { Category: { Length: > 0 } category } && IsCategory(category) ? category : CategoryRoot + Titles.OfType(type);

	// The types of graphs a game type's marked members need, other than those registered anyway: the declaring types of
	// instance members and constructors, and the types of parameters, results, properties and fields (an array's element
	// for arrays). isGameType says which types are the game's.
	internal static List<Type> FindSignatureTypes(Type type, Func<Type, bool> isGameType)
	{
		List<Type> found = [];
		if (type.IsEnum)
		{
			return found;
		}
		VisualNodeAttribute? typeAttribute = AttributeOf(type);
		foreach (MemberInfo member in MembersOf(type))
		{
			if (!IsMarked(member, typeAttribute))
			{
				continue;
			}
			switch (member)
			{
				case MethodBase method:
					if (method is ConstructorInfo || !method.IsStatic)
					{
						AddGameType(found, type, isGameType);
					}
					if (method is MethodInfo info)
					{
						AddGameType(found, info.ReturnType, isGameType);
					}
					foreach (ParameterInfo parameter in method.GetParameters())
					{
						AddGameType(found, parameter.ParameterType, isGameType);
					}
					break;
				case PropertyInfo property:
					if (!MemberNodes.IsStatic(property))
					{
						AddGameType(found, type, isGameType);
					}
					AddGameType(found, property.PropertyType, isGameType);
					break;
				case FieldInfo field:
					if (!field.IsStatic)
					{
						AddGameType(found, type, isGameType);
					}
					AddGameType(found, field.FieldType, isGameType);
					break;
			}
		}
		return found;
	}

	// The nodes of a game type's marked members, the notes of those without one, and the former names of those with one.
	internal static void Create(Type type, Func<Type, VisualType?> resolve, VisualType entity, List<MemberNodeDefinition> nodes,
		List<LibraryNote> notes, List<GameRename> renames)
	{
		string typeId = TypeIds.Of(type) ?? MemberNodes.DisplayName(type);
		VisualNodeAttribute? typeAttribute = AttributeOf(type);
		string defaultCategory = CategoryRoot + Titles.OfType(type);
		if (typeAttribute != null)
		{
			defaultCategory = ReadCategory(typeAttribute, typeId, defaultCategory, defaultCategory, notes);
		}
		if (type.IsEnum)
		{
			return;
		}
		foreach (MemberInfo member in MembersOf(type))
		{
			if (!IsMarked(member, typeAttribute))
			{
				continue;
			}
			VisualNodeAttribute? attribute = AttributeOf(member);
			string name = member is ConstructorInfo ? typeId : typeId + "." + member.Name;
			string category = attribute != null ? ReadCategory(attribute, name, defaultCategory, defaultCategory, notes) : defaultCategory;
			GameMember game = new(member, name, category, attribute, notes);
			if (!IsPublic(member))
			{
				game.Note("[VisualNode] members must be public");
				continue;
			}
			if (type.IsInterface || type.IsGenericType)
			{
				game.Note(type.IsInterface ? "members of interfaces have no node form" : "members of generic types have no node form");
				continue;
			}
			MemberNodeText text = ReadText(game);
			int count = nodes.Count;
			switch (member)
			{
				case MethodInfo method:
					AddMethod(game, method, text, resolve, entity, nodes);
					break;
				case PropertyInfo property:
					AddProperty(game, property, text, resolve, entity, nodes);
					break;
				case FieldInfo field:
					AddField(game, field, text, resolve, entity, nodes);
					break;
				case ConstructorInfo constructor:
					AddConstructor(game, constructor, text, resolve, entity, nodes);
					break;
			}
			if (attribute != null && nodes.Count > count && ReadFormerNames(attribute) is { Count: > 0 } formerNames)
			{
				for (int index = count; index < nodes.Count; index++)
				{
					renames.Add(new GameRename(nodes[index], name, category, formerNames));
				}
			}
		}
	}

	// The redirects of the game members' former names (spec-slice3 section 3.7.5): from each former type string to the
	// member's current one. A former name that is no member name, or whose type string is a node's type or another
	// redirect's, is ignored with a note.
	internal static List<NodeRedirect> CreateRedirects(IReadOnlyList<GameRename> renames, IReadOnlyList<NodeDefinition> nodes,
		IReadOnlyList<NodeRedirect> redirects, List<LibraryNote> notes)
	{
		HashSet<string> taken = new(StringComparer.Ordinal);
		foreach (NodeDefinition node in nodes)
		{
			taken.Add(node.Type);
		}
		foreach (NodeRedirect redirect in redirects)
		{
			taken.Add(redirect.OldType);
		}
		List<NodeRedirect> created = [];
		foreach (GameRename rename in renames)
		{
			foreach (string formerName in rename.FormerNames)
			{
				if (!IsMemberName(formerName))
				{
					notes.Add(new LibraryNote(rename.Member, rename.Category,
						$"{rename.Member}: the former name {JsonText.Quote(JsonText.Shorten(formerName))} is not a name of a member or type: it is ignored"));
					continue;
				}
				string oldType = FormerType(rename.Node, formerName);
				if (!taken.Add(oldType))
				{
					notes.Add(new LibraryNote(rename.Member, rename.Category,
						$"{rename.Member}: the former name {formerName} is the type of another node: it is ignored"));
					continue;
				}
				created.Add(new NodeRedirect(oldType, rename.Node.Type, []));
			}
		}
		return created;
	}

	// The type string a node had under a former name: a name without dots is a member of the same type, one with dots names
	// the type too; a constructor's former name is its type's former full name.
	internal static string FormerType(MemberNodeDefinition node, string formerName)
	{
		string prefix = MemberNodeDefinition.Prefix(node.MemberKind);
		string declaringId = TypeIds.Of(node.DeclaringType)!;
		if (node.MemberKind == MemberNodeKind.New)
		{
			return prefix + formerName + node.Type[(prefix.Length + declaringId.Length)..];
		}
		string rest = node.Type[(prefix.Length + declaringId.Length + 1 + node.Member.Name.Length)..];
		string qualified = formerName.Contains('.', StringComparison.Ordinal) ? formerName : declaringId + "." + formerName;
		return prefix + qualified + rest;
	}

	// The game's notes once, ordered by member, category and message.
	internal static List<LibraryNote> Order(List<LibraryNote> notes)
	{
		HashSet<LibraryNote> seen = [];
		List<LibraryNote> ordered = [];
		foreach (LibraryNote note in notes)
		{
			if (seen.Add(note))
			{
				ordered.Add(note);
			}
		}
		ordered.Sort(static (a, b) =>
		{
			int order = string.CompareOrdinal(a.Member, b.Member);
			order = order != 0 ? order : string.CompareOrdinal(a.Category, b.Category);
			return order != 0 ? order : string.CompareOrdinal(a.Message, b.Message);
		});
		return ordered;
	}

	private static void AddMethod(GameMember game, MethodInfo method, MemberNodeText text, Func<Type, VisualType?> resolve, VisualType entity,
		List<MemberNodeDefinition> nodes)
	{
		if (method.IsSpecialName)
		{
			game.Note("operators and accessors have no node of their own: graphs use the operator nodes and the property's nodes");
			return;
		}
		if (method.Name is nameof(Equals) or nameof(GetHashCode) or nameof(ToString) || method.ReturnType.IsByRef)
		{
			game.Note(method.ReturnType.IsByRef ? "ref results have no pins" : "[VisualNode] member has no node form");
			return;
		}
		if (ParameterProblem(method) is { } problem)
		{
			game.Note("[VisualNode] member has no node form: " + problem);
			return;
		}
		TypeConstraint domain = TypeConstraint.None;
		bool inference = false;
		if (method.IsGenericMethodDefinition && !MemberNodes.TryGetGenericForm(method, out domain, out inference))
		{
			game.Note("[VisualNode] member has no node form: a generic method's type parameters are all unconstrained, or all constrained to Component, Asset or Script");
			return;
		}
		bool pure = game.Attribute?.Pure == true;
		if (pure && method.ReturnType == typeof(void) && !Array.Exists(method.GetParameters(), static parameter => parameter.ParameterType.IsByRef))
		{
			game.Note("Pure needs a result or out parameters: the node is impure");
			pure = false;
		}
		game.Add(MemberNodes.CreateNode(MemberNodeKind.Call, method, pure, resolve, entity, text, out string? shapeProblem, domain, inference),
			shapeProblem, nodes);
	}

	private static void AddProperty(GameMember game, PropertyInfo property, MemberNodeText text, Func<Type, VisualType?> resolve, VisualType entity,
		List<MemberNodeDefinition> nodes)
	{
		if (property.GetIndexParameters().Length > 0 || property.PropertyType.IsByRef)
		{
			game.Note(property.PropertyType.IsByRef ? "ref properties have no pins" : "indexers have no node form");
			return;
		}
		if (property.GetMethod is { IsPublic: true })
		{
			game.Add(MemberNodes.CreateNode(MemberNodeKind.Get, property, true, resolve, entity, text, out string? problem), problem, nodes);
		}
		MethodInfo? setter = property.SetMethod;
		if (setter is { IsPublic: true } && !MemberNodes.IsInitOnly(setter) && (setter.IsStatic || !property.DeclaringType!.IsValueType))
		{
			game.Add(MemberNodes.CreateNode(MemberNodeKind.Set, property, false, resolve, entity, text, out string? problem), problem, nodes);
		}
	}

	private static void AddField(GameMember game, FieldInfo field, MemberNodeText text, Func<Type, VisualType?> resolve, VisualType entity,
		List<MemberNodeDefinition> nodes)
	{
		if (field.IsSpecialName)
		{
			return;
		}
		game.Add(MemberNodes.CreateNode(MemberNodeKind.Get, field, true, resolve, entity, text, out string? problem), problem, nodes);
		if (!field.IsInitOnly && !field.IsLiteral && (field.IsStatic || !field.DeclaringType!.IsValueType))
		{
			game.Add(MemberNodes.CreateNode(MemberNodeKind.Set, field, false, resolve, entity, text, out string? setProblem), setProblem, nodes);
		}
	}

	private static void AddConstructor(GameMember game, ConstructorInfo constructor, MemberNodeText text, Func<Type, VisualType?> resolve,
		VisualType entity, List<MemberNodeDefinition> nodes)
	{
		Type type = constructor.DeclaringType!;
		if (type.IsSubclassOf(typeof(Script)))
		{
			game.Note("script classes are created by components, not by New");
			return;
		}
		if (type.IsAbstract)
		{
			game.Note("abstract types have no constructor nodes");
			return;
		}
		if (ParameterProblem(constructor) is { } problem)
		{
			game.Note("[VisualNode] member has no node form: " + problem);
			return;
		}
		game.Add(MemberNodes.CreateNode(MemberNodeKind.New, constructor, type.IsValueType, resolve, entity, text, out string? shapeProblem),
			shapeProblem, nodes);
	}

	// Why a method's parameters have no pins: ref, in and params parameters (out parameters are outputs).
	private static string? ParameterProblem(MethodBase method)
	{
		foreach (ParameterInfo parameter in method.GetParameters())
		{
			if (parameter.ParameterType.IsByRef && !(parameter.IsOut && !parameter.IsIn))
			{
				return parameter.IsIn ? "in parameters have no pins" : "ref parameters have no pins";
			}
			if (parameter.IsDefined(typeof(ParamArrayAttribute), false))
			{
				return "params parameters have no pins";
			}
		}
		return null;
	}

	// The title words, description and keywords a member's [VisualNode] gives its nodes; values outside their limits are
	// noted and not used.
	private static MemberNodeText ReadText(GameMember game)
	{
		VisualNodeAttribute? attribute = game.Attribute;
		if (attribute == null)
		{
			return new MemberNodeText(null, game.Category, string.Empty, [], true);
		}
		string? title = null;
		string requested = attribute.Title ?? string.Empty;
		if (requested.Length > MaxTitleLength || JsonText.HasControlCharacter(requested))
		{
			game.Note(FormattableString.Invariant(
				$"the Title must have 1 to {MaxTitleLength} characters without control characters: the node is titled by the member's name"));
		}
		else if (requested.Length > 0)
		{
			title = requested;
		}
		string description = attribute.Description ?? string.Empty;
		if (description.Length > MaxDescriptionLength)
		{
			game.Note(FormattableString.Invariant($"the Description has more than {MaxDescriptionLength} characters: it is left out"));
			description = string.Empty;
		}
		List<string> keywords = [];
		foreach (string? keyword in attribute.Keywords ?? [])
		{
			if (keyword is not { Length: > 0 and <= MaxKeywordLength } || JsonText.HasControlCharacter(keyword))
			{
				game.Note(FormattableString.Invariant(
					$"the keyword {JsonText.Quote(JsonText.Shorten(keyword ?? string.Empty))} must have 1 to {MaxKeywordLength} characters without control characters: it is left out"));
				continue;
			}
			keywords.Add(keyword);
		}
		return new MemberNodeText(title, game.Category, description, keywords, true);
	}

	// The category an attribute gives (fallback when it gives none); one outside the limits is noted as listed in
	// listedIn and gives fallback.
	private static string ReadCategory(VisualNodeAttribute attribute, string member, string fallback, string listedIn, List<LibraryNote> notes)
	{
		string category = attribute.Category ?? string.Empty;
		if (category.Length == 0)
		{
			return fallback;
		}
		if (IsCategory(category))
		{
			return category;
		}
		notes.Add(new LibraryNote(member, listedIn, FormattableString.Invariant(
			$"{member}: the Category {JsonText.Quote(JsonText.Shorten(category))} is not 1 to {MaxCategorySegments} names of 1 to {MaxSegmentLength} characters separated by '/': the nodes are listed in {listedIn}")));
		return fallback;
	}

	private static bool IsCategory(string category)
	{
		string[] segments = category.Split('/');
		if (segments.Length > MaxCategorySegments)
		{
			return false;
		}
		foreach (string segment in segments)
		{
			if (segment.Length is 0 or > MaxSegmentLength || JsonText.HasControlCharacter(segment))
			{
				return false;
			}
		}
		return true;
	}

	private static IReadOnlyList<string> ReadFormerNames(VisualNodeAttribute attribute)
	{
		List<string> names = [];
		foreach (string? name in attribute.FormerNames ?? [])
		{
			string formerName = name ?? string.Empty;
			if (!names.Contains(formerName))
			{
				names.Add(formerName);
			}
		}
		return names;
	}

	// A dotted name of identifiers ("Heal", "Game.Hero.Heal", "Game.Outer+Inner").
	private static bool IsMemberName(string name)
	{
		if (name.Length is 0 or > 1024)
		{
			return false;
		}
		foreach (string segment in name.Split('.'))
		{
			if (segment.Length == 0 || char.IsAsciiDigit(segment[0]))
			{
				return false;
			}
			foreach (char character in segment)
			{
				if (!char.IsLetterOrDigit(character) && character is not ('_' or '+'))
				{
					return false;
				}
			}
		}
		return true;
	}

	// Members are marked by their own [VisualNode], or by their type's when they are public, shown and not generated by the
	// compiler (a record's Clone and Deconstruct are not the game's members).
	private static bool IsMarked(MemberInfo member, VisualNodeAttribute? typeAttribute)
	{
		if (AttributeOf(member) != null)
		{
			return true;
		}
		return typeAttribute != null && IsPublic(member) && !MemberNodes.IsHidden(member) && !member.IsDefined(typeof(CompilerGeneratedAttribute), false) &&
			!(member is MethodInfo method && (method.IsSpecialName || method.Name is nameof(Equals) or nameof(GetHashCode) or nameof(ToString)));
	}

	private static bool IsPublic(MemberInfo member) => member switch
	{
		MethodBase method => method.IsPublic,
		FieldInfo field => field.IsPublic,
		PropertyInfo property => property.GetMethod is { IsPublic: true } || property.SetMethod is { IsPublic: true },
		_ => false,
	};

	// Methods, properties, fields and constructors declared on the type, public or not, in metadata order.
	private static List<MemberInfo> MembersOf(Type type)
	{
		List<MemberInfo> members = [.. type.GetMethods(Declared), .. type.GetProperties(Declared), .. type.GetFields(Declared)];
		members.AddRange(type.GetConstructors(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance));
		members.Sort(static (a, b) => a.MetadataToken.CompareTo(b.MetadataToken));
		return members;
	}

	private static void AddGameType(List<Type> found, Type type, Func<Type, bool> isGameType)
	{
		while (type.IsByRef || type.IsSZArray)
		{
			type = type.GetElementType()!;
		}
		if (isGameType(type) && !found.Contains(type))
		{
			found.Add(type);
		}
	}

	// A marked member being made into nodes: its notes say why it has none, for marked members only.
	private sealed class GameMember
	{
		private readonly List<LibraryNote> m_Notes;

		internal GameMember(MemberInfo member, string name, string category, VisualNodeAttribute? attribute, List<LibraryNote> notes)
		{
			Member = member;
			Name = name;
			Category = category;
			Attribute = attribute;
			m_Notes = notes;
		}

		internal MemberInfo Member { get; }

		internal string Name { get; }

		internal string Category { get; }

		// The member's own attribute; null when only its type's marks it (its nodes then take the type's category and the
		// member's name, and nothing is noted about it).
		internal VisualNodeAttribute? Attribute { get; }

		internal void Note(string message)
		{
			if (Attribute != null)
			{
				m_Notes.Add(new LibraryNote(Name, Category, $"{Name}: {message}"));
			}
		}

		internal void Add(MemberNodeDefinition? node, string? problem, List<MemberNodeDefinition> nodes)
		{
			if (node != null)
			{
				nodes.Add(node);
			}
			else
			{
				Note(problem ?? "[VisualNode] member has no node form");
			}
		}
	}
}

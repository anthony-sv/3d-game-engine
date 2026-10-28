using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// The node library (spec sections 4 and 6.1): the types of the type system and every node kind, built-in and reflected.
// Immutable once built, so every runtime and thread may share it.
internal sealed class NodeLibrary
{
	private readonly Dictionary<string, VisualType> m_TypesById = new(StringComparer.Ordinal);
	private readonly Dictionary<Type, VisualType> m_TypesByClr = [];
	private readonly Dictionary<string, NodeDefinition> m_NodesByType = new(StringComparer.Ordinal);
	private readonly Dictionary<string, List<MemberNodeDefinition>> m_MembersByKey = new(StringComparer.Ordinal);
	private readonly Dictionary<string, NodeRedirect> m_Redirects = new(StringComparer.Ordinal);
	private readonly Dictionary<BuiltinNode, BuiltinNodeDefinition> m_Builtins = [];

	internal NodeLibrary(IReadOnlyList<VisualType> types, IReadOnlyList<NodeDefinition> nodes, IReadOnlyList<NodeRedirect> redirects,
		BuiltinTypes builtinTypes, LibraryDomains domains, IReadOnlyList<LibraryNote> notes)
	{
		Types = types;
		Nodes = nodes;
		BuiltinTypes = builtinTypes;
		Domains = domains;
		Notes = notes;
		foreach (VisualType type in types)
		{
			m_TypesById[type.Id] = type;
			if (type.ClrType != null)
			{
				m_TypesByClr[type.ClrType] = type;
			}
		}
		List<string> nodeTypes = new(nodes.Count);
		foreach (NodeDefinition node in nodes)
		{
			m_NodesByType[node.Type] = node;
			nodeTypes.Add(node.Type);
			switch (node)
			{
				case BuiltinNodeDefinition builtin:
					m_Builtins[builtin.Node] = builtin;
					break;
				case MemberNodeDefinition member:
					if (!m_MembersByKey.TryGetValue(member.LookupKey, out List<MemberNodeDefinition>? overloads))
					{
						overloads = [];
						m_MembersByKey[member.LookupKey] = overloads;
					}
					overloads.Add(member);
					break;
			}
		}
		nodeTypes.Sort(StringComparer.Ordinal);
		NodeTypes = nodeTypes;
		foreach (NodeRedirect redirect in redirects)
		{
			m_Redirects.TryAdd(redirect.OldType, redirect);
		}
		List<string> typeIds = new(types.Count);
		foreach (VisualType type in types)
		{
			typeIds.Add(type.Id);
		}
		TypeIds = typeIds;
		Entity = builtinTypes.Entity;
		Bool = builtinTypes.Bool;
		Long = Require(typeof(long));
		String = builtinTypes.String;
	}

	// Ordered by ID (ordinal).
	internal IReadOnlyList<VisualType> Types { get; }

	// Ordered by category, title and type string (ordinal).
	internal IReadOnlyList<NodeDefinition> Nodes { get; }

	// Every node type string and type ID, ordered (the candidates of suggestions).
	internal IReadOnlyList<string> NodeTypes { get; }

	internal IReadOnlyList<string> TypeIds { get; }

	internal BuiltinTypes BuiltinTypes { get; }

	internal LibraryDomains Domains { get; }

	// The game members [VisualNode] marks that have no node, and why (spec-slice3 section 3.7.2), ordered by member,
	// category and message (ordinal).
	internal IReadOnlyList<LibraryNote> Notes { get; }

	internal VisualType Entity { get; }

	internal VisualType Bool { get; }

	internal VisualType Long { get; }

	internal VisualType String { get; }

	// The type of a type ID, including exec; null for unknown IDs and the wildcard.
	internal VisualType? FindType(string id) => m_TypesById.TryGetValue(id, out VisualType? type) ? type : null;

	internal VisualType? FindType(Type type) => m_TypesByClr.TryGetValue(type, out VisualType? visualType) ? visualType : null;

	internal NodeDefinition? FindNode(string type) => m_NodesByType.TryGetValue(type, out NodeDefinition? node) ? node : null;

	internal BuiltinNodeDefinition GetBuiltin(BuiltinNode node) => m_Builtins[node];

	// The reflected nodes of one member name: the overloads of a method, a getter or setter, the constructors of a type.
	internal IReadOnlyList<MemberNodeDefinition> FindMembers(string lookupKey) =>
		m_MembersByKey.TryGetValue(lookupKey, out List<MemberNodeDefinition>? members) ? members : [];

	internal NodeRedirect? FindRedirect(string oldType) => m_Redirects.TryGetValue(oldType, out NodeRedirect? redirect) ? redirect : null;

	// The types a TypeList or Type setting with a domain constraint accepts.
	internal IReadOnlyList<VisualType> DomainOf(TypeConstraint constraint) => constraint switch
	{
		TypeConstraint.Component => Domains.Components,
		TypeConstraint.Asset => Domains.Assets,
		TypeConstraint.Script => Domains.Scripts,
		_ => [],
	};

	private VisualType Require(Type type) => m_TypesByClr.TryGetValue(type, out VisualType? visualType) ? visualType
		: throw new InvalidOperationException($"the node library has no type {type.FullName}");
}

// Why a member of the game that [VisualNode] marks has no node, or what of its attribute is not used (spec-slice3 section
// 3.7.2): Member is the member's declaring type and name (a type alone for notes about a type), Category where the palette
// would list it, Message the reason, starting with the member.
internal sealed record LibraryNote(string Member, string Category, string Message);

// The type arguments generic API methods accept (spec section 4.5), each ordered by ID: the registered component classes,
// the concrete asset classes and the game's script classes.
internal sealed record LibraryDomains(IReadOnlyList<VisualType> Components, IReadOnlyList<VisualType> Assets, IReadOnlyList<VisualType> Scripts);

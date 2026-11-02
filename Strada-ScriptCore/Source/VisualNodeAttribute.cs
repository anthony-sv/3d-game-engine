using System;

namespace Strada;

/// <summary>Makes a member of the game's scripts a node of visual scripts, or every public member of a type: graphs
/// call your code as they call the scripting API.</summary>
/// <remarks>
/// <para>On a public method, property, field or constructor of a public type of the game assembly, the member becomes a
/// node. On a public class or struct, every public member declared on it that has a node form becomes one, and the type
/// becomes a type of graphs. On a public enum, the enum becomes a type of graphs (variables, switches, literals). Types a
/// node's signature uses become types of graphs too.</para>
/// <para>A member has a node form when its parameters and result are types of graphs (numbers, bool, string, vectors,
/// colours, quaternions, entities, components, assets, enums, structs and classes, arrays of them) and it takes no
/// <c>ref</c>, <c>in</c> or <c>params</c> parameter, pointer or delegate; <c>out</c> parameters become outputs. Generic
/// methods are allowed when their type parameters are unconstrained, or constrained to Component, Asset or Script. Methods
/// run to their end: a member cannot make a graph wait.</para>
/// <para>An instance member of a script class takes the entity whose script it runs on (the graph's own entity when its
/// Target is not linked); the call fails when that entity has no script of the class. Constructors of script classes have
/// no node.</para>
/// <para>Methods are impure (they have flow pins) unless <see cref="Pure"/> is set; getters are pure, setters never.</para>
/// <para>A member the attribute marks that has no node form is listed, with the reason, among the notes of the node
/// library, which the editor's node palette shows and the log reports.</para>
/// </remarks>
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct | AttributeTargets.Enum | AttributeTargets.Method |
	AttributeTargets.Property | AttributeTargets.Field | AttributeTargets.Constructor, AllowMultiple = false, Inherited = false)]
public sealed class VisualNodeAttribute : Attribute
{
	/// <summary>The node's title; empty for the member's name in words ("AddHealth" gives "Add Health"). A property's or
	/// field's nodes are titled "Get" and "Set" followed by it.</summary>
	public string Title { get; set; } = string.Empty;

	/// <summary>Where the node palette lists the node, as a path of categories ("Game/Combat"); empty for "Game/" and the
	/// declaring type's name in words. On a type, the category of the nodes of its members that name none.</summary>
	public string Category { get; set; } = string.Empty;

	/// <summary>What the node does, shown by the palette and the node's tooltip.</summary>
	public string Description { get; set; } = string.Empty;

	/// <summary>More words the palette finds the node by.</summary>
	public string[] Keywords { get; set; } = [];

	/// <summary>Makes a method a pure node: no flow pins, and it runs each time a node reads its outputs. Only for methods
	/// that return a value or have <c>out</c> parameters and change nothing.</summary>
	public bool Pure { get; set; }

	/// <summary>Names the member had before: "Heal" for a member of the same type, "Game.Hero.Heal" for one that moved to
	/// another type (for a constructor, the type's former full name). Graphs that use a former name keep working and store
	/// the current one when next saved.</summary>
	public string[] FormerNames { get; set; } = [];
}

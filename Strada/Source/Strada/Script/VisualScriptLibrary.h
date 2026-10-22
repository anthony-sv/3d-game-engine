#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptVariables.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// What the scripting runtime says about visual scripts, read from its JSON: the node library (the types and node kinds
// graphs can use) and the analysis of a document (its nodes with their resolved pins, and the problems found). The engine
// never derives pins or types itself: VisualScriptEngine asks the runtime and reads its answers with these. Plain data and
// pure functions, usable on any thread.
namespace Strada
{
	// The type of flow pins, and the type of wildcard pins whose type is not decided yet.
	inline constexpr std::string_view VisualScriptExecType = "exec";
	inline constexpr std::string_view VisualScriptWildcardType = "*";

	// What kind of values a type has.
	enum class VisualScriptTypeKind : uint8_t
	{
		Exec = 0,
		Bool,
		Integer,
		Float,
		String,
		Vector,
		Quaternion,
		Color,
		Matrix,
		// Strada.Entity and script classes.
		Entity,
		Component,
		Asset,
		Enum,
		Struct,
		Class,
		Array,
		// System.Object.
		Object
	};

	enum class VisualScriptNodeKind : uint8_t
	{
		Event = 0,
		Flow,
		Call,
		Pure,
		Latent,
		Variable,
		Function,
		Operator,
		Data,
		Reroute,
		// Analyses only: a node whose type the library does not know.
		Unknown
	};

	enum class VisualScriptPinDirection : uint8_t
	{
		In = 0,
		Out
	};

	// What an exec output of a latent node is for.
	enum class VisualScriptPinRole : uint8_t
	{
		None = 0,
		// Where the flow continues when the node is done.
		Continuation,
		// Fired while the node is pending (Tween, Interval).
		Tick
	};

	// What a wildcard pin accepts.
	enum class VisualScriptPinConstraint : uint8_t
	{
		// Not a wildcard, or the inferred pins of an unknown node, which accept anything like Any.
		None = 0,
		// Any type with values (not exec).
		Any,
		// Integer and float types.
		Numeric,
		// Entities, components, assets, classes, arrays and System.Object.
		Reference,
		// Array types.
		Array
	};

	struct VisualScriptPin
	{
		std::string Name;
		VisualScriptPinDirection Direction = VisualScriptPinDirection::In;
		// A type ID, VisualScriptExecType, or VisualScriptWildcardType while a wildcard is undecided.
		std::string Type;
		// Shown instead of the name; empty when none.
		std::string Label;
		// The literal an unconnected input takes, in the document encoding; null when none.
		Json Default;
		// An unconnected input that means the entity running the graph (a Target).
		bool DefaultsToSelf = false;
		VisualScriptPinRole Role = VisualScriptPinRole::None;
		VisualScriptPinConstraint Constraint = VisualScriptPinConstraint::None;
		// Wildcards of one node with the same group share one type; empty when none.
		std::string Group;
		// A wildcard whose type is the element type of the array pin of this name; empty when none.
		std::string ElementOf;
		// A wildcard of a Reroute node: it passes the flow as well as values, so it connects to exec pins too.
		bool CarriesExec = false;
	};

	// A setting of a node kind, which documents store in the node's Settings.
	struct VisualScriptSetting
	{
		std::string Name;
		// Bool, Int, String, Type, TypeList, Variable, Function, Parameters, Cases, Value, Enum or Choice.
		std::string Kind;
		// What a missing setting means; null when the setting has no default.
		Json Default;
		bool Required = false;
		std::optional<int64_t> Min;
		std::optional<int64_t> Max;
		// Type and TypeList settings: which types they take (Literal, Exposable, Struct, ...); empty when any.
		std::string Constraint;
		// Enum settings: the enum type; empty when none.
		std::string Type;
		// Choice settings: the values they take.
		std::vector<std::string> Choices;
	};

	struct VisualScriptStructField
	{
		std::string Name;
		std::string Type;
	};

	// A type of the library: what graphs store, pass and convert.
	struct VisualScriptTypeInfo
	{
		std::string ID;
		std::string Title;
		VisualScriptTypeKind Kind = VisualScriptTypeKind::Object;
		// Types public variables can have: what stores their values on components; None for the others.
		ScriptFieldType FieldType = ScriptFieldType::None;
		// Asset classes: the type of asset their values reference; None for the others.
		AssetType AcceptedAssetType = AssetType::None;
		// Whether documents can hold values of the type as literals.
		bool HasLiteral = false;
		// Enums: the enumerators in declaration order, as values of FieldType (Int32, UInt32, Int64 or UInt64).
		std::vector<ScriptEnumerator> Enumerators;
		// A [Flags] enum: values combine enumerators bitwise.
		bool IsFlags = false;
		// Structs that Make builds and Break takes apart, from their fields.
		bool Makeable = false;
		bool Breakable = false;
		std::vector<VisualScriptStructField> Fields;
		// Classes: the base class; empty for none.
		std::string Base;
		// Arrays: the element type; empty for other types.
		std::string Element;
		// The types values convert to implicitly (numbers that widen, base classes, a component's entity, an asset's handle,
		// System.Object). Every type with values also converts to string, which is not listed.
		std::vector<std::string> ImplicitTo;
		// The type's entry of the runtime's JSON, as automation reports it.
		Json Source;
	};

	// A node kind of the library, with the pins of its default settings.
	struct VisualScriptNodeTemplate
	{
		// "Flow.Branch", "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", ...
		std::string Type;
		std::string Title;
		// Empty when none.
		std::string Subtitle;
		// Slash-separated: "Components/Rigid Body".
		std::string Category;
		// Empty when none.
		std::string Description;
		// More words searches find the node by.
		std::vector<std::string> Keywords;
		VisualScriptNodeKind Kind = VisualScriptNodeKind::Call;
		// Pure nodes have no flow pins and run whenever an output is read.
		bool Pure = false;
		// Latent nodes keep the flow waiting (Delay, ...).
		bool Latent = false;
		std::vector<VisualScriptPin> Pins;
		std::vector<VisualScriptSetting> Settings;
		// The node's entry of the runtime's JSON, as automation reports it.
		Json Source;

		// Null when the node has no such pin.
		VisualScriptPin const* FindPin(std::string_view name, VisualScriptPinDirection direction) const;
	};

	// A category of the library's nodes, with the categories under it.
	struct VisualScriptCategory
	{
		// The last part of the path ("Rigid Body"); empty for the root.
		std::string Name;
		// The whole path ("Components/Rigid Body"); empty for the root.
		std::string Path;
		// Sorted by name (ordinal).
		std::vector<VisualScriptCategory> Children;
		// The nodes directly in this category, in library order.
		std::vector<VisualScriptNodeTemplate const*> Nodes;
	};

	// The node library of the scripting runtime: every type and node kind graphs can use, read from the runtime's JSON. It
	// never changes once read; what the lookups return points into it and lives as long as it does.
	class VisualScriptLibrary
	{
	public:
		VisualScriptLibrary() = default;
		VisualScriptLibrary(VisualScriptLibrary const&) = delete;
		VisualScriptLibrary& operator=(VisualScriptLibrary const&) = delete;

		// Reads the library JSON: { "Types": [...], "Nodes": [...] }. Fails when it is not of that form (an engine and a
		// scripting runtime that do not match), naming the first member that is not.
		[[nodiscard]] static Result<Scope<VisualScriptLibrary>> Parse(std::string_view json);

		// Sorted by ID (ordinal).
		std::span<VisualScriptTypeInfo const> GetTypes() const { return m_Types; }
		// In library order: by category, then title, then type.
		std::span<VisualScriptNodeTemplate const> GetNodes() const { return m_Nodes; }
		// The root of the category tree; its Path is empty and it holds the nodes without a category.
		VisualScriptCategory const& GetCategories() const { return m_Categories; }

		// Null when the library has no such type.
		VisualScriptTypeInfo const* FindType(std::string_view id) const;
		// Null when the library has no such node kind.
		VisualScriptNodeTemplate const* FindNode(std::string_view type) const;
		// The nodes whose title, a keyword, category or type contains the text, ignoring the case of ASCII letters, in
		// library order; every node for an empty text.
		std::vector<VisualScriptNodeTemplate const*> Search(std::string_view text) const;
		// Whether a link from an output pin to an input pin is allowed: the same type; exec only to exec (or to a Reroute's
		// wildcards); a wildcard input takes what its constraint accepts and a wildcard output goes to an input of a type
		// its constraint accepts, or to another wildcard; then the implicit conversions of the output's type, and any value
		// to string. The pins are a library node's or an analyzed node's (whose wildcards may be resolved).
		bool CanConnect(VisualScriptPin const& output, VisualScriptPin const& input) const;
		// Whether a type satisfies a wildcard's constraint. Exec and wildcards satisfy none but Any and None.
		bool Satisfies(std::string_view type, VisualScriptPinConstraint constraint) const;
		// The enum type of a type ID, as the public variables of visual scripts use it (VisualScriptEnumLookup); nothing for
		// the types that are not enums.
		std::optional<VisualScriptEnumInfo> FindEnum(std::string_view typeId) const;

	private:
		std::vector<VisualScriptTypeInfo> m_Types;
		std::vector<VisualScriptNodeTemplate> m_Nodes;
		// Views of the IDs and types the vectors hold, which never move once read.
		std::unordered_map<std::string_view, size_t> m_TypeIndices;
		std::unordered_map<std::string_view, size_t> m_NodeIndices;
		VisualScriptCategory m_Categories;
	};

	// How analysis resolved a node's stored type.
	enum class VisualScriptResolution : uint8_t
	{
		// The library has the type.
		Exact = 0,
		// Through the runtime's redirects: an API member that was renamed or changed its signature.
		Redirect,
		// The only overload of its member that has every pin the node uses.
		Unique,
		// Not resolved: the node cannot run.
		Unknown
	};

	// A pin a node resolved inexactly calls differently now.
	struct VisualScriptPinRename
	{
		VisualScriptPinDirection Direction = VisualScriptPinDirection::In;
		std::string From;
		std::string To;
	};

	// A node of an analyzed document.
	struct VisualScriptAnalyzedNode
	{
		uint32_t ID = 0;
		// The function whose graph has the node; 0 for the event graph.
		uint32_t Function = 0;
		// As stored.
		std::string Type;
		VisualScriptResolution Resolved = VisualScriptResolution::Exact;
		// Redirect and Unique: the type the node should be stored as, and the pins that are renamed with it.
		std::string CanonicalType;
		std::vector<VisualScriptPinRename> PinRenames;
		std::string Title;
		// Empty when none.
		std::string Subtitle;
		VisualScriptNodeKind Kind = VisualScriptNodeKind::Unknown;
		bool Pure = false;
		bool Latent = false;
		// With the types analysis resolved wildcards to (VisualScriptWildcardType while undecided). An unknown node has the
		// pins its links and inputs name.
		std::vector<VisualScriptPin> Pins;
		// A disabled node: the exec output its flow continues at; empty when the chain ends there, or for a node that is not
		// disabled. Where the flow leaves a disabled node is for the scripting runtime to say: the editor draws it from here.
		std::string PassThrough;

		// Null when the node has no such pin.
		VisualScriptPin const* FindPin(std::string_view name, VisualScriptPinDirection direction) const;
	};

	enum class VisualScriptSeverity : uint8_t
	{
		// The graph cannot run.
		Error = 0,
		Warning
	};

	// A problem analysis found in a document.
	struct VisualScriptDiagnostic
	{
		VisualScriptSeverity Severity = VisualScriptSeverity::Error;
		// "TypeMismatch", "UnknownNodeType", ...: stable codes (Docs/Automation.md lists them).
		std::string Code;
		// The function whose graph it concerns; 0 for the event graph and the document.
		uint32_t Function = 0;
		std::optional<uint32_t> Node;
		// The pin it concerns, with its direction; empty when none.
		std::string Pin;
		std::optional<VisualScriptPinDirection> Direction;
		std::optional<VisualScriptLink> Link;
		// A variable or local.
		std::optional<uint32_t> Variable;
		std::string Message;
		// The known names closest to an unknown one, or the candidates of an ambiguous node.
		std::vector<std::string> Suggestions;
	};

	// The scripting runtime's analysis of a document.
	struct VisualScriptAnalysis
	{
		// Every node of the document: those of the functions' graphs in document order, then those of the event graph.
		std::vector<VisualScriptAnalyzedNode> Nodes;
		std::vector<VisualScriptDiagnostic> Diagnostics;
		// The runtime's JSON, { "Nodes": [...], "Diagnostics": [...] }, as automation reports it.
		Json Source;

		// Node IDs are unique in a document. Null when it has no such node.
		VisualScriptAnalyzedNode const* FindNode(uint32_t id) const;

		// Reads the analysis JSON. Fails when it is not of its form, naming the first member that is not.
		[[nodiscard]] static Result<VisualScriptAnalysis> Parse(std::string_view json);
	};

	// Reads a diagnostics array: what the runtime answers when it loads a document.
	[[nodiscard]] Result<std::vector<VisualScriptDiagnostic>> ParseVisualScriptDiagnostics(std::string_view json);
	size_t CountVisualScriptErrors(std::span<VisualScriptDiagnostic const> diagnostics);
	// "1 error; first: <message> (node <id>)" or "<n> errors; first: ...", with "(variable <id>)" for a variable and no
	// place for the document; empty when there is no error.
	std::string DescribeVisualScriptErrors(std::span<VisualScriptDiagnostic const> diagnostics);
}

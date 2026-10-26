#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Base.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// The document of a visual script (.sgraph): variables, functions and the event graph. The engine keeps it as data and
	// checks its structure only (VisualScriptSerializer); the scripting runtime gives node types, settings, inputs and
	// values their meaning. Variables, locals, functions, nodes and comments share one space of IDs.

	struct VisualScriptVariable
	{
		uint32_t ID = 0;
		std::string Name;
		// A type of the scripting runtime: "float", "Strada.Vector3", "Strada.ForceMode", ...
		std::string Type;
		// The initial value as a literal of its type; null means none (the type's default).
		Json Value;
		// Public variables are shown, and can be set, on every VisualScriptComponent that runs the script.
		bool Public = false;
		std::string Tooltip;
		// Slider range of a public number: minimum, maximum.
		std::optional<glm::vec2> Range;
		// Earlier names: values set on components under one of them still apply after a rename.
		std::vector<std::string> FormerNames;

		bool operator==(VisualScriptVariable const& other) const = default;
	};

	// Storage of a function's call, initialized on every call.
	struct VisualScriptLocal
	{
		uint32_t ID = 0;
		std::string Name;
		std::string Type;
		// The initial value as a literal of its type; null means none (the type's default).
		Json Value;

		bool operator==(VisualScriptLocal const& other) const = default;
	};

	// An input or output of a function.
	struct VisualScriptParameter
	{
		std::string Name;
		std::string Type;

		bool operator==(VisualScriptParameter const& other) const = default;
	};

	// Whether a node takes part when its graph runs.
	enum class VisualScriptNodeEnabled : uint8_t
	{
		Enabled = 0,
		// The node passes the flow through to its next node instead of running.
		Disabled,
		// Enabled, except in exported games, where it runs as Disabled.
		DevelopmentOnly
	};

	// "Enabled", "Disabled" or "DevelopmentOnly"; empty for a value that is none of the enumerators.
	[[nodiscard]] std::string_view VisualScriptNodeEnabledToString(VisualScriptNodeEnabled enabled);
	[[nodiscard]] std::optional<VisualScriptNodeEnabled> VisualScriptNodeEnabledFromString(std::string_view text);

	// The struct pins of a node shown as one pin per field, by direction. An entry is a pin path: the name of a pin ("Hit"),
	// or a field pin of an entry listed earlier in the same list ("Hit.Point"). Whether a path names a pin whose type can
	// split is for the scripting runtime to say; the format only keeps parents ahead of their fields.
	struct VisualScriptSplitPins
	{
		std::vector<std::string> Inputs;
		std::vector<std::string> Outputs;

		bool IsEmpty() const { return Inputs.empty() && Outputs.empty(); }

		bool operator==(VisualScriptSplitPins const& other) const = default;
	};

	struct VisualScriptNode
	{
		uint32_t ID = 0;
		// What the node does: "Flow.Branch", "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", ...
		std::string Type;
		// Where the node sits on the canvas; none for unplaced nodes (PlaceUnplacedNodes places them).
		std::optional<glm::vec2> Position;
		// The node's settings and the literals of its unconnected inputs, by name, stored as written.
		Json Settings = Json::object();
		Json Inputs = Json::object();
		// Version 2 of the format added the members below.
		VisualScriptSplitPins Split;
		// A note shown with the node.
		std::string Comment;
		// Only the node's header is shown.
		bool Collapsed = false;
		VisualScriptNodeEnabled Enabled = VisualScriptNodeEnabled::Enabled;

		bool operator==(VisualScriptNode const& other) const = default;
	};

	// A pin of a node: pins are identified by node, direction and name, so an input and an output may share a name.
	struct VisualScriptPinReference
	{
		uint32_t Node = 0;
		std::string Pin;

		bool operator==(VisualScriptPinReference const& other) const = default;
	};

	// Connects an output (From) to an input (To) of nodes of the same graph.
	struct VisualScriptLink
	{
		VisualScriptPinReference From;
		VisualScriptPinReference To;

		bool operator==(VisualScriptLink const& other) const = default;
	};

	// A note on the canvas, framing the nodes under it.
	struct VisualScriptComment
	{
		uint32_t ID = 0;
		std::string Text;
		glm::vec2 Position{0.0f, 0.0f};
		glm::vec2 Size{400.0f, 200.0f};
		// Linear RGBA.
		glm::vec4 Color{0.5f, 0.5f, 0.5f, 0.25f};

		bool operator==(VisualScriptComment const& other) const = default;
	};

	struct VisualScriptGraph
	{
		std::vector<VisualScriptNode> Nodes;
		std::vector<VisualScriptLink> Links;
		std::vector<VisualScriptComment> Comments;

		bool operator==(VisualScriptGraph const& other) const = default;
	};

	struct VisualScriptFunction
	{
		uint32_t ID = 0;
		std::string Name;
		// Pure functions have no flow pins and run whenever an output of their call is read.
		bool Pure = false;
		std::vector<VisualScriptParameter> Inputs;
		std::vector<VisualScriptParameter> Outputs;
		std::vector<VisualScriptLocal> Locals;
		VisualScriptGraph Graph;

		bool operator==(VisualScriptFunction const& other) const = default;
	};

	struct VisualScriptDocument
	{
		std::string Description;
		std::vector<VisualScriptVariable> Variables;
		std::vector<VisualScriptFunction> Functions;
		// The event graph.
		VisualScriptGraph Graph;

		bool operator==(VisualScriptDocument const& other) const = default;
	};

	// Whether two documents give the same program: alike except for what the scripting runtime never reads (the document's
	// Description; variables' Tooltip and Range; nodes' Position, Comment and Collapsed; the graphs' Comments). Everything
	// else is compared, order included. Values (of variables and locals, and nodes' Settings and Inputs) are compared as the
	// runtime reads them, from the document's text: an integer and a float are never alike, since an int literal or an ID
	// must be an integer (operator== takes 1 and 1.0 as equal), integers are alike by value whether stored signed or
	// unsigned, and floats bit for bit. Pure; any thread.
	[[nodiscard]] bool HaveSameVisualScriptProgram(VisualScriptDocument const& a, VisualScriptDocument const& b);
	// The same comparison for one node (its ID, Type, Settings, Inputs, Split and Enabled): with operator==, whether a node is
	// stored identically, representation included. Pure; any thread.
	[[nodiscard]] bool HaveSameVisualScriptProgram(VisualScriptNode const& a, VisualScriptNode const& b);

	// A visual script (.sgraph file). Its document is an immutable shared object: a change replaces it with another, so
	// whoever keeps the previous one (an undo step, an editor) keeps it unchanged and without a copy.
	class VisualScriptAsset final : public Asset
	{
	public:
		VisualScriptAsset() = default;
		explicit VisualScriptAsset(VisualScriptDocument document);

		static AssetType GetStaticType() { return AssetType::VisualScript; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		// Valid until the next SetDocument, which replaces the document object rather than its contents.
		VisualScriptDocument const& GetDocument() const { return *m_Document; }
		// The document as the shared object it is; never null.
		Ref<VisualScriptDocument const> const& GetSharedDocument() const { return m_Document; }
		void SetDocument(VisualScriptDocument document);
		// Takes the object itself (document must not be null).
		void SetDocument(Ref<VisualScriptDocument const> document);
		// Incremented by every change; users of the document compare it to notice edits.
		uint64_t GetVersion() const { return m_Version; }

	private:
		Ref<VisualScriptDocument const> m_Document = CreateRef<VisualScriptDocument const>();
		uint64_t m_Version = 0;
	};

	// What a new visual script starts with.
	enum class VisualScriptTemplate : uint8_t
	{
		// An Event.Create node and an Event.Update node below it.
		Events = 0,
		// Nothing.
		Empty
	};

	[[nodiscard]] VisualScriptDocument MakeVisualScriptTemplate(VisualScriptTemplate kind);
}

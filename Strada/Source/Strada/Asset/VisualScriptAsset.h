#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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

	// A visual script (.sgraph file).
	class VisualScriptAsset final : public Asset
	{
	public:
		VisualScriptAsset() = default;
		explicit VisualScriptAsset(VisualScriptDocument document)
			: m_Document(std::move(document))
		{
		}

		static AssetType GetStaticType() { return AssetType::VisualScript; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		VisualScriptDocument const& GetDocument() const { return m_Document; }
		void SetDocument(VisualScriptDocument document);
		// Incremented by every change; users of the document compare it to notice edits.
		uint64_t GetVersion() const { return m_Version; }

	private:
		VisualScriptDocument m_Document;
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

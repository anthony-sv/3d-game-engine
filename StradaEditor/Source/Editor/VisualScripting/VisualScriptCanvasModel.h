#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Script/VisualScriptLiterals.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The canvas's visual model: the drawable form of one sub-graph of a visual script (the event graph or a function's graph),
// laid out once per change in canvas units (the units of positions in documents), so the renderer, hit testing and the
// spatial index only read it. Pure (no ImGui); main thread only.
namespace Strada
{
	// Drawn after the visible part of an ellipsized text: U+2026, which the editor's UI font has.
	inline constexpr std::string_view VisualScriptEllipsis = "\xE2\x80\xA6";

	// A text of the visual model, kept in the model's arena (VisualScriptCanvasModel::GetText).
	struct VisualScriptTextRef
	{
		uint32_t Offset = 0;
		uint16_t Length = 0;
		// The bytes drawn: fewer than Length when the text is ellipsized, and then VisualScriptEllipsis is drawn after them.
		uint16_t VisibleLength = 0;
		// What is drawn (the ellipsis included), in canvas units at the element's text size.
		float Width = 0.0f;

		bool IsEllipsized() const { return VisibleLength < Length; }

		bool operator==(VisualScriptTextRef const& other) const = default;
	};

	enum class VisualScriptNodeShape : uint8_t
	{
		// A header with the title, and rows of pins under it.
		Standard = 0,
		// Variable.Get and Self: a pill with the name and its one output on the right.
		Pill,
		// A binary operator: a box with the operator's symbol and unlabelled pins.
		Operator,
		// A dot that wires pass through.
		Reroute
	};

	enum class VisualScriptPinShape : uint8_t
	{
		// A pentagon pointing the way the flow goes.
		Exec = 0,
		Data,
		// A grid of cells in the element type's colour.
		Array,
		// A diamond: a wildcard whose type is not decided ("*"), and the pins inferred without a library.
		Wildcard
	};

	// What a pin's value chip shows, and how.
	enum class VisualScriptChipKind : uint8_t
	{
		None = 0,
		// bool: a check box; the text is "true" or "false".
		Check,
		// Integers and floating-point numbers.
		Number,
		// string.
		Text,
		// Vectors and quaternions (in Euler degrees): one frame per component, the text one line per component.
		Cells,
		// Strada.Color: a swatch of ChipColor; no text.
		Color,
		// Enums and [Flags] enums: the enumerator's name or "A | B", and an arrow box.
		Choice,
		// Asset references: the asset's name.
		Asset,
		// Strada.Matrix4: the text "Matrix" (Details edits matrices).
		Matrix,
		// No frame, the text in TextDisabled: "Self" for an input that means the entity running the graph, "None" for entities
		// and the types without literals.
		Placeholder,
		// A literal as stored, in compact JSON: one the pin's type cannot read (ChipInvalid, in the error colour), or one of a
		// pin whose type is not known.
		Raw
	};

	// VisualScriptPinVisual::Flags.
	namespace VisualScriptPinFlags
	{
		// A link ends at the pin.
		inline constexpr uint16_t Linked = 1u << 0;
		// A struct the pin can split into one pin per field: an input of a Makeable type or an output of a Breakable one, with
		// two fields or more.
		inline constexpr uint16_t Splittable = 1u << 1;
		// A field pin of a split pin ("Force.X").
		inline constexpr uint16_t SplitField = 1u << 2;
		// The first of the rows of one split pin's fields (nested splits included): its bracket spans them in BracketColor.
		inline constexpr uint16_t SplitGroupStart = 1u << 3;
		// A pin the analysis does not have but a link or a stored literal names (a removed Switch case): a row after the
		// regular ones, in the error colour.
		inline constexpr uint16_t Orphan = 1u << 4;
		// The analysis reports a problem on the pin.
		inline constexpr uint16_t HasError = 1u << 5;
		inline constexpr uint16_t HasWarning = 1u << 6;
		// An input that, unlinked, means the entity running the graph.
		inline constexpr uint16_t DefaultsToSelf = 1u << 7;
		// Nothing is stored: the chip shows the pin's default, else the type's.
		inline constexpr uint16_t ChipIsDefault = 1u << 8;
		// The stored literal does not read as the pin's type.
		inline constexpr uint16_t ChipInvalid = 1u << 9;
		// The node's only exec input named Execute, or its only exec output named Then: no label is drawn or measured.
		inline constexpr uint16_t LabelHidden = 1u << 10;
	}

	// A pin as the canvas draws it. Rectangles and the anchor are relative to the node's position.
	struct VisualScriptPinVisual
	{
		// The pin's name as the canvas knows it ("Then", "Force.X"): a pin the analysis renamed has its new name here, while the
		// document keeps the old one until the next commit (VisualScriptLinkVisual::FromName and ToName).
		VisualScriptTextRef Name;
		// The text drawn ("Force X"); empty with LabelHidden.
		VisualScriptTextRef Label;
		// Where wires end on the node drawn expanded: the glyph's centre. A collapsed node's wires end at its header's edges
		// instead (VisualScriptGeometry::GetPinAnchor).
		glm::vec2 Anchor{0.0f};
		// The pin's row, from the node's edge to the end of its label.
		VisualScriptRect HitRect;
		// The value chip; empty when none is drawn.
		VisualScriptRect ChipRect;
		// The value, formatted: what the chip shows. Pins without a drawn chip may have one too (the inputs of a compact
		// operator), for their tooltip.
		VisualScriptTextRef ChipText;
		// VisualScriptExecType, a type ID, or VisualScriptWildcardType.
		std::string Type;
		// The type's colour (VisualScriptTheme); the error colour for orphans.
		uint32_t Color = 0;
		// ChipKind Color: the colour, sRGB-encoded.
		uint32_t ChipColor = 0;
		// SplitGroupStart: the colour of the split pin's type, which its bracket is drawn in.
		uint32_t BracketColor = 0;
		VisualScriptTypeKind TypeKind = VisualScriptTypeKind::Object;
		VisualScriptPinShape Shape = VisualScriptPinShape::Data;
		VisualScriptPinDirection Direction = VisualScriptPinDirection::In;
		VisualScriptChipKind ChipKind = VisualScriptChipKind::None;
		// VisualScriptPinFlags.
		uint16_t Flags = 0;

		bool HasFlag(uint16_t flag) const { return (Flags & flag) != 0; }
	};

	// VisualScriptNodeVisual::Flags.
	namespace VisualScriptNodeFlags
	{
		inline constexpr uint16_t Pure = 1u << 0;
		inline constexpr uint16_t Latent = 1u << 1;
		inline constexpr uint16_t Collapsed = 1u << 2;
		inline constexpr uint16_t Disabled = 1u << 3;
		inline constexpr uint16_t DevelopmentOnly = 1u << 4;
		// Where the pins, title and kind come from (exactly one is set): the analysis; the library's template of the node's
		// type, for a node changed since the analysis; or the document alone (no library, or a type it does not know).
		inline constexpr uint16_t Analyzed = 1u << 5;
		inline constexpr uint16_t Template = 1u << 6;
		inline constexpr uint16_t Inferred = 1u << 7;
		// Function.Entry, Function.Return and Function.Call: the "ƒ" glyph left of the title.
		inline constexpr uint16_t FunctionNode = 1u << 8;
		// A disabled node whose flow continues at PassThroughPin: the bypass line from its exec input to that exit.
		inline constexpr uint16_t PassThroughExit = 1u << 9;
		// A type the library does not know (the analysis says Unknown): the Unknown header and a hatched body.
		inline constexpr uint16_t UnknownType = 1u << 10;
	}

	// A node as the canvas draws it.
	struct VisualScriptNodeVisual
	{
		uint32_t ID = 0;
		// The top-left corner.
		glm::vec2 Position{0.0f};
		// As drawn: a collapsed node is its header, with its strip when disabled or development-only.
		glm::vec2 Size{0.0f};
		// With every pin row: what a collapsed node takes when drawn expanded under a dragged wire; Size when not collapsed.
		glm::vec2 ExpandedSize{0.0f};
		// Absolute: everything drawn for the node (body, shadow, badges at their size at 100 %, comment bubble), for culling
		// and the spatial index.
		VisualScriptRect VisualBounds;
		// Relative to Position: the node comment's bubble above the node, its tail included; empty without a comment.
		VisualScriptRect BubbleRect;
		uint32_t FirstPin = 0;
		uint16_t PinCount = 0;
		VisualScriptNodeShape Shape = VisualScriptNodeShape::Standard;
		VisualScriptNodeKind Kind = VisualScriptNodeKind::Unknown;
		// What the node shows as its title: the title (ellipsized to MaxNodeWidth), a pill's name, an operator's symbol.
		VisualScriptTextRef Title;
		VisualScriptTextRef Subtitle;
		// The node comment wrapped at NodeCommentWrapWidth, its lines separated by '\n' (at most NodeCommentMaxLines); its Width
		// is the widest line's.
		VisualScriptTextRef Comment;
		// The header's gradient, and the whole node at Overview and Far (VisualScriptNodeColors).
		uint32_t HeaderColor = 0;
		uint32_t HeaderEndColor = 0;
		uint32_t BoxColor = 0;
		// The analysis' problems located on the node.
		uint16_t ErrorCount = 0;
		uint16_t WarningCount = 0;
		// VisualScriptNodeFlags.
		uint16_t Flags = 0;
		// PassThroughExit: the exit's index among the node's pins.
		uint8_t PassThroughPin = 0;

		bool HasFlag(uint16_t flag) const { return (Flags & flag) != 0; }
	};

	// VisualScriptLinkVisual::Flags.
	namespace VisualScriptLinkFlags
	{
		// The wire carries the flow.
		inline constexpr uint8_t Exec = 1u << 0;
		// The analyzed types of its ends differ (an implicit conversion): the wire fades from FromColor to ToColor.
		inline constexpr uint8_t Converted = 1u << 1;
		// The analysis reports an error on the link.
		inline constexpr uint8_t Error = 1u << 2;
		// An end is an orphan pin.
		inline constexpr uint8_t Orphan = 1u << 3;
		// An end's node is disabled.
		inline constexpr uint8_t Disabled = 1u << 4;
	}

	// A link as the canvas draws it (a wire).
	struct VisualScriptLinkVisual
	{
		// Indices into GetNodes() and GetPins().
		uint32_t FromNode = 0;
		uint32_t ToNode = 0;
		uint32_t FromPin = 0;
		uint32_t ToPin = 0;
		// Equal unless Converted.
		uint32_t FromColor = 0;
		uint32_t ToColor = 0;
		// The link's index in the shown graph's Links.
		uint32_t Source = 0;
		// The pin names the document stores for the link: the names of FromPin and ToPin, except where the analysis renamed a
		// pin and the document still has the old name.
		VisualScriptTextRef FromName;
		VisualScriptTextRef ToName;
		// VisualScriptLinkFlags.
		uint8_t Flags = 0;

		bool HasFlag(uint8_t flag) const { return (Flags & flag) != 0; }
	};

	// A comment box as the canvas draws it.
	struct VisualScriptCommentVisual
	{
		uint32_t ID = 0;
		VisualScriptRect Rect;
		// The first line of its text, its width at CommentTitleTextSize, ellipsized to the box's width at that size (at lower
		// zooms the title is drawn larger than that, and the renderer shortens it further).
		VisualScriptTextRef Title;
		// The further lines, wrapped at CommentBodyInset inside the box, lines separated by '\n'.
		VisualScriptTextRef Body;
		// VisualScriptCommentColors.
		uint32_t BodyColor = 0;
		uint32_t BarColor = 0;
		uint32_t BarTextColor = 0;
		uint32_t BorderColor = 0;
	};

	// The name of an asset as value chips show it; given an invalid handle, never called.
	using VisualScriptAssetNameLookup = std::function<std::string(AssetHandle handle)>;

	// What the visual model is made from. The pointers must stay valid during VisualScriptCanvasModel::Sync only.
	struct VisualScriptCanvasSource
	{
		// The document the canvas shows: the asset's, with its unplaced nodes placed.
		VisualScriptDocument const* Display = nullptr;
		// The sub-graph shown: 0 for the event graph, else a function's ID (an unknown ID shows nothing).
		uint32_t Function = 0;
		// May be null (no scripting runtime, or not analyzed yet).
		VisualScriptAnalysis const* Analysis = nullptr;
		// The document Analysis was made from: a node takes its analysis' pins only while it equals its counterpart there.
		VisualScriptDocument const* AnalyzedDocument = nullptr;
		// May be null (no scripting runtime): pins are then inferred from the document, or come from the analysis kept.
		VisualScriptLibrary const* Library = nullptr;
		// The enumerators of enum types, for chip text; may be empty.
		VisualScriptEnumLookup EnumLookup;
		// False keeps an analysis' pins without its problems: the analysis of a scripting runtime that stopped.
		bool ShowDiagnostics = true;
		// The names of assets, for chip text; empty shows handles in decimal.
		VisualScriptAssetNameLookup AssetNames;
		// Changes when assets' names may have changed (AssetManager::GetVersion): the nodes whose chips show an asset's name are
		// laid out again.
		uint64_t AssetVersion = 0;
	};

	// A literal as a value chip (and a pin's tooltip) shows it.
	struct VisualScriptChipValue
	{
		VisualScriptChipKind Kind = VisualScriptChipKind::None;
		// Integers in decimal, floats as printf's %g, vectors and quaternions one line per component (quaternions in Euler
		// degrees), enums by enumerator name ("A | B" for [Flags] enums), assets by name, strings as stored, and literals that
		// do not read as the type as compact JSON.
		std::string Text;
		// Kind Color: the colour, sRGB-encoded (IM_COL32 layout).
		uint32_t Color = 0;
		// The literal does not read as the type.
		bool Invalid = false;
	};

	// How a value of a pin's type is shown: the literal, or with none (null) the type's default. type is the library's entry
	// of typeId; null for "*" and types the library does not have, whose literals show as compact JSON. Kind Placeholder
	// ("None") for the types without literals and entities, and None for exec.
	[[nodiscard]] VisualScriptChipValue FormatVisualScriptLiteral(Json const& literal, std::string_view typeId,
	                                                              VisualScriptTypeInfo const* type,
	                                                              VisualScriptEnumLookup const& enumLookup,
	                                                              VisualScriptAssetNameLookup const& assetNames);

	// The symbol a binary operator's compact shape shows ("+", "≤", "AND"); empty for other node types.
	[[nodiscard]] std::string_view GetVisualScriptOperatorSymbol(std::string_view nodeType);

	// Whether a node still is what its analysis entry describes, so its pins, title and kind come from the analysis: its type
	// (or the type the analysis wants it stored as), settings, input keys and splits equal its counterpart's in the analyzed
	// document. Literal values, positions, comments and states do not change pins.
	[[nodiscard]] bool MatchesVisualScriptAnalysis(VisualScriptNode const& node, VisualScriptNode const& counterpart,
	                                               VisualScriptAnalyzedNode const& entry);
	// The name a pin the document stores has on an analyzed node: a pin the analysis renamed, and the fields of one, take the
	// new name until the next commit rewrites the document (an exact name wins over a field reading, then the longest renamed
	// pin). Returns stored itself, a rename's name, or a name built in buffer.
	[[nodiscard]] std::string_view MapVisualScriptPinName(VisualScriptAnalyzedNode const* entry, std::string_view stored,
	                                                      VisualScriptPinDirection direction, std::string& buffer);

	// The drawable form of one sub-graph of a visual script's display document. Sync lays out what changed: a node keeps its
	// layout as long as what it is drawn from is unchanged and only takes its new position; links and comments are rebuilt.
	// Main thread only.
	class VisualScriptCanvasModel
	{
	public:
		VisualScriptCanvasModel();
		~VisualScriptCanvasModel();
		VisualScriptCanvasModel(VisualScriptCanvasModel&&) noexcept;
		VisualScriptCanvasModel& operator=(VisualScriptCanvasModel&&) noexcept;
		VisualScriptCanvasModel(VisualScriptCanvasModel const&) = delete;
		VisualScriptCanvasModel& operator=(VisualScriptCanvasModel const&) = delete;

		// Lays out what changed since the last Sync: every element when the library, the theme anchors or the measurer's
		// Signature changed; otherwise each node whose type, settings, inputs, splits, comment, collapsed and enabled state,
		// pins (from the analysis, the template or the document), links' pin names and problems changed, and the nodes whose
		// chips show assets' names when the source's AssetVersion changed. Never analyzes and never writes the document.
		// Returns true when what the spatial index holds changed (node and comment rectangles, wire ends).
		bool Sync(VisualScriptCanvasSource const& source, VisualScriptTextMeasurer const& measure, VisualScriptThemeAnchors const& theme);

		// In the shown graph's order.
		std::span<VisualScriptNodeVisual const> GetNodes() const { return m_Nodes; }
		std::span<VisualScriptPinVisual const> GetPins() const { return m_Pins; }
		// The pins of a node: inputs, then outputs, each with their orphans last.
		std::span<VisualScriptPinVisual const> GetPins(VisualScriptNodeVisual const& node) const;
		// In the shown graph's order; links whose nodes are not in the graph are left out.
		std::span<VisualScriptLinkVisual const> GetLinks() const { return m_Links; }
		// Largest area first, as they are drawn: inner comments above the outer ones.
		std::span<VisualScriptCommentVisual const> GetComments() const { return m_Comments; }
		// The indices of the links that end at a node, in link order.
		std::span<uint32_t const> GetIncidentLinks(uint32_t nodeIndex) const;
		// The first link (in the graph's order) that leaves a node: the target a reroute faces.
		std::optional<uint32_t> GetFirstOutgoingLink(uint32_t nodeIndex) const;

		// Node and comment indices by document ID.
		std::optional<uint32_t> FindNode(uint32_t id) const;
		std::optional<uint32_t> FindComment(uint32_t id) const;
		// A pin's index in GetPins(), by its name as the canvas knows it.
		std::optional<uint32_t> FindPin(uint32_t nodeIndex, std::string_view name, VisualScriptPinDirection direction) const;
		// The link leaving the node's output of that name.
		std::optional<uint32_t> FindExecLink(uint32_t nodeIndex, std::string_view outputPin) const;

		// The whole text; and the bytes drawn before an ellipsis.
		std::string_view GetText(VisualScriptTextRef ref) const;
		std::string_view GetVisibleText(VisualScriptTextRef ref) const;
		// The link as the document stores it.
		VisualScriptLink GetDocumentLink(uint32_t linkIndex) const;

		// The size of a node as drawn (VisualScriptNodeExtents); (0, 0) for an ID that is no node of the shown graph.
		glm::vec2 GetExtents(uint32_t id) const;
		// The bounds of the given nodes and comments (IDs of the shown graph; others are ignored), by their rectangles;
		// nothing when none of them is there.
		std::optional<VisualScriptRect> GetBounds(std::span<uint32_t const> elements) const;
		// The bounds of every node and comment; nothing for an empty graph.
		std::optional<VisualScriptRect> GetBounds() const;

		// The sub-graph shown, as the last Sync was given it.
		uint32_t GetFunction() const { return m_Function; }
		// Bytes its vectors and text arena have reserved (capacity, not size), including the spare ones each Sync builds into:
		// a Sync of an unchanged document does not grow it.
		size_t GetCapacityBytes() const;
		// How many nodes the last Sync laid out anew (the others kept their layout).
		uint32_t GetMeasuredNodeCount() const { return m_MeasuredNodeCount; }
		// Whether some node's chip shows an asset's name: only then does a new AssetVersion change anything, so a caller need
		// not Sync for one otherwise.
		bool ShowsAssetNames() const { return m_ShowsAssetNames; }

	private:
		// What a node's or a comment's layout was made from, and where its texts are.
		struct LayoutRecord
		{
			uint64_t Hash = 0;
			// The element's texts (its pins' included) are one block of the arena.
			uint32_t TextOffset = 0;
			uint32_t TextLength = 0;
			// Of where its pins' wires end and are hit.
			uint64_t AnchorHash = 0;
			// A chip shows an asset's name: laid out again when assets' names may have changed.
			bool ShowsAssetNames = false;
		};
		using NodeRecord = LayoutRecord;
		using CommentRecord = LayoutRecord;

		// Sync's work (in the source file), and the buffers it reuses from one Sync to the next.
		class Builder;
		struct Scratch;

		std::vector<VisualScriptNodeVisual> m_Nodes;
		std::vector<NodeRecord> m_NodeRecords;
		std::vector<VisualScriptPinVisual> m_Pins;
		std::vector<VisualScriptLinkVisual> m_Links;
		std::vector<VisualScriptCommentVisual> m_Comments;
		std::vector<CommentRecord> m_CommentRecords;
		std::string m_Text;
		// Sorted by ID: (ID, index).
		std::vector<std::pair<uint32_t, uint32_t>> m_NodeIndices;
		std::vector<std::pair<uint32_t, uint32_t>> m_CommentIndices;
		// Links ending at each node (CSR): m_Incident[m_IncidentStarts[node]...m_IncidentStarts[node + 1]).
		std::vector<uint32_t> m_IncidentStarts;
		std::vector<uint32_t> m_Incident;
		// Per node: its first outgoing link, or NoLink.
		std::vector<uint32_t> m_FirstOutgoing;

		// What the last Sync was given, to tell what changed.
		VisualScriptLibrary const* m_Library = nullptr;
		VisualScriptThemeAnchors m_Theme;
		uint64_t m_MeasureSignature = 0;
		uint64_t m_AssetVersion = 0;
		bool m_HasSynced = false;
		uint32_t m_Function = 0;
		uint64_t m_GeometryHash = 0;
		uint32_t m_MeasuredNodeCount = 0;
		bool m_ShowsAssetNames = false;

		// The previous Sync's results, which the next one copies kept layouts from; their capacity is kept equal to the
		// results', so building into them never grows them.
		std::vector<VisualScriptNodeVisual> m_SpareNodes;
		std::vector<NodeRecord> m_SpareNodeRecords;
		std::vector<VisualScriptPinVisual> m_SparePins;
		std::vector<VisualScriptCommentVisual> m_SpareComments;
		std::vector<CommentRecord> m_SpareCommentRecords;
		std::string m_SpareText;
		std::vector<std::pair<uint32_t, uint32_t>> m_SpareNodeIndices;
		std::vector<std::pair<uint32_t, uint32_t>> m_SpareCommentIndices;

		Scope<Scratch> m_Scratch;
	};
}

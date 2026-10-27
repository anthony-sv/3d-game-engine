#pragma once

#include "Editor/VisualScripting/VisualScriptEditorTypes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"
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

// The edits of the visual script editor as pure functions of documents: what the canvas, the panes and the quick fixes do to
// a copy of the document they show before they commit it (VisualScriptCommit.h). No ImGui, no state; main thread or any
// other.
//
// Every operation checks its arguments before it changes anything: when it fails, the document is unchanged and the error
// names what is wrong ("node 12 has no output Then"). What the operations write keeps the document loadable as the format
// requires (names, IDs, limits, values), and never addresses a field of a struct pin that is not split. New elements take
// the document's largest ID plus one, in the order they are created. A graph is named by `function`: 0 for the event
// graph, else a function's ID. Nodes, comments, variables, locals and functions share one space of IDs, so the operations
// that take a node, a comment or a variable alone find it in whichever graph or function holds it.
namespace Strada
{
	// A pin of a node as the canvas knows it (from the scripting runtime's analysis, else the node library's template of the
	// node's type, else what the document's links and literals name), and where its wires end.
	struct VisualScriptNodePin
	{
		// The name the document stores for the pin, which its links and Inputs keys use ("Then", "Force.X"): for a node the
		// analysis renamed a pin of (a redirect), the stored name, not the name shown until the next commit rewrites it.
		std::string Name;
		VisualScriptPinDirection Direction = VisualScriptPinDirection::In;
		// VisualScriptExecType, a type ID, or VisualScriptWildcardType.
		std::string Type;
		// Where the pin's wires end, relative to the node's position (its top-left corner), in canvas units.
		glm::vec2 Anchor{0.0f};
		// An input's default: the literal it takes while it has none of its own and nothing is linked to it (the node
		// library's VisualScriptPin::Default, in the document encoding); null when none, so the type's default.
		Json Default = nullptr;
		// An input that means the entity running the graph while nothing is linked to it (VisualScriptPin::DefaultsToSelf):
		// no literal stands for that.
		bool DefaultsToSelf = false;

		bool operator==(VisualScriptNodePin const& other) const = default;
	};

	// The pins of a node in the canvas's order (inputs and outputs each in pin order, a split pin's fields in its place, no
	// orphan rows), with the names the document stores (VisualScriptEditing::ToStoredVisualScriptPin); empty for a node the
	// canvas does not show. The operations that need pins take this rather than a
	// VisualScriptPinTypeLookup, which answers for one pin at a time: they also need a node's whole row of pins (whether a
	// node has a single exec input and a single exec output, its first exec output, the fields of a split pin) and where
	// pins sit (straightening).
	using VisualScriptNodePins = std::function<std::vector<VisualScriptNodePin>(uint32_t node)>;

	// What VisualScriptEditing::RemoveElements removed.
	struct VisualScriptRemoval
	{
		uint32_t Nodes = 0;
		uint32_t Comments = 0;
		// What the editor tells the user, such as why a function's entry stays.
		std::vector<std::string> Notes;
	};

	// The edge or centre line VisualScriptEditing::AlignElements aligns.
	enum class VisualScriptAlignment : uint8_t
	{
		Left = 0,
		Right,
		Top,
		Bottom,
		// Horizontal centres: the elements end up in a column.
		Centers,
		// Vertical centres: the elements end up in a row.
		Middles
	};

	// The axis VisualScriptEditing::DistributeElements spaces along.
	enum class VisualScriptDistribution : uint8_t
	{
		Horizontal = 0,
		Vertical
	};

	namespace VisualScriptEditing
	{
		// Names (variables, locals, functions, parameters, custom events). A name is valid when
		// VisualScriptSerializer::IsValidName accepts it; uniqueness depends on what is named, as the Is...NameTaken
		// functions below say. To rename an element, check the new name against these when it differs from the current one.

		// A valid name made from base that taken does not hold: base without the characters names cannot have (control
		// characters, '.' and bytes that are not UTF-8), without leading and trailing spaces and cut to 64 bytes at a
		// character boundary ("Value" when nothing is left), else that name with the lowest number from 1 appended
		// ("Score1"), cut so that the whole stays within 64 bytes. taken must hold finitely many names; an empty taken holds
		// none.
		[[nodiscard]] std::string MakeUniqueVisualScriptName(std::string_view base, std::function<bool(std::string_view)> const& taken);

		// Whether a new variable cannot have the name: a variable has it, as its name or one of its former names, or a local
		// of any function does.
		[[nodiscard]] bool IsVariableNameTaken(VisualScriptDocument const& document, std::string_view name);
		// Whether a new local of the function cannot have the name: one of the function's locals or a variable has it.
		[[nodiscard]] bool IsLocalNameTaken(VisualScriptDocument const& document, uint32_t function, std::string_view name);
		// Whether a new parameter of a list (a function's inputs or outputs, a custom event's or a sender's parameters) cannot
		// have the name: the list has it, or it is a reserved pin name.
		[[nodiscard]] bool IsParameterNameTaken(std::span<VisualScriptParameter const> parameters, std::string_view name);
		// Whether a new function cannot have the name: a function has it.
		[[nodiscard]] bool IsFunctionNameTaken(VisualScriptDocument const& document, std::string_view name);
		// Whether a new custom event cannot have the name: an Event.Custom node of the event graph has it.
		[[nodiscard]] bool IsCustomEventNameTaken(VisualScriptDocument const& document, std::string_view name);
		// The pin names nodes have whatever their settings: Execute, Then, Target, Self and Default. Parameters cannot have them.
		[[nodiscard]] bool IsReservedPinName(std::string_view name);

		// What the format accepts as a type, a node type, a pin name or the name of a Settings or Inputs member: 1 to maxBytes
		// of UTF-8 without control characters (VisualScriptSerializer's MaxTypeBytes, MaxNodeTypeBytes, MaxPinNameBytes).
		[[nodiscard]] bool IsValidVisualScriptText(std::string_view text, size_t maxBytes);
		// What the format accepts as a pin path (an entry of Split, or a pin a link or a literal names): a valid pin name
		// whose '.'-separated parts are not empty.
		[[nodiscard]] bool IsValidVisualScriptPinPath(std::string_view path);

		// Pin names. The operations take pins, links and Inputs keys by the names the document stores. A node the analysis
		// resolved through a redirect shows some pins under new names until the next commit rewrites it, and a canvas names
		// its pins (and the fields of split pins) that way: translate those names with this before calling an operation.

		// The name the document stores for a pin the analysis of its node names: the pin a rename gave that name, else, for a
		// field of a renamed pin ("Value.X" of a pin stored as "Amount"), that pin's stored name with the field ("Amount.X"),
		// the longest renamed pin winning; else the name itself. With no analysis (null), the name itself.
		[[nodiscard]] std::string ToStoredVisualScriptPin(VisualScriptAnalyzedNode const* analyzed, VisualScriptPinDirection direction,
		                                                  std::string_view name);

		// IDs and the shapes the operations share.

		// The largest ID of the document's variables, locals, functions, nodes and comments; 0 when it has none. New elements
		// take the IDs after it.
		[[nodiscard]] uint32_t GetLargestVisualScriptID(VisualScriptDocument const& document);

		// What a comment box that frames elements adds around their bounds: on every side, and above for its title.
		inline constexpr float CommentFrameMargin = 32.0f;
		inline constexpr float CommentFrameTitleSpace = 40.0f;

		// Nodes and links.

		// Adds a node of the type with the settings (an object; null for none) at the position (kept within the format's
		// coordinates). Returns its ID.
		[[nodiscard]] Result<uint32_t> AddNode(VisualScriptDocument& document, uint32_t function, std::string type, Json settings,
		                                       glm::vec2 position);
		// Puts a node of the graph into a link: A → B becomes A → node.inputPin and node.outputPin → B, each connected as
		// Connect connects (so the node's input and output keep at most the links their kinds allow). The link and the pins
		// are named as the document stores them.
		[[nodiscard]] Result<void> SpliceNode(VisualScriptDocument& document, uint32_t function, uint32_t node,
		                                      VisualScriptLink const& link, std::string const& inputPin, std::string const& outputPin,
		                                      VisualScriptNodePins const& pins);
		// Removes nodes and comments of the graph (a comment's contents stay) and every link of a removed node. With bridge,
		// the flow that went through removed nodes is kept: a removed Reroute's sources are linked to its targets, and with
		// pins, a removed node with exactly one exec input and exactly one exec output (a Set, a call, a Delay) has the
		// links into its exec input re-pointed to the targets of its exec output. Runs of removed nodes bridge end to end, to
		// the first nodes kept. A function's entry (the first Function.Entry of its graph) is never removed: it stays, with a
		// note. Without pins (no scripting runtime) only reroutes bridge.
		[[nodiscard]] Result<VisualScriptRemoval> RemoveElements(VisualScriptDocument& document, uint32_t function,
		                                                         std::span<uint32_t const> elements, VisualScriptNodePins const* pins,
		                                                         bool bridge);
		// Moves nodes and comments of the graph by the offset; positions stay within the format's coordinates.
		[[nodiscard]] Result<void> MoveElements(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> elements,
		                                        glm::vec2 offset);
		// Sets the positions of nodes and comments of the graph (kept within the format's coordinates).
		[[nodiscard]] Result<void> SetPositions(VisualScriptDocument& document, uint32_t function,
		                                        std::span<std::pair<uint32_t, glm::vec2> const> positions);
		// Adds a link between pins the nodes have, in the graph, named as the document stores them. A link of exec pins
		// (either end exec) leaves its output with no other link, a link of data pins its input: each keeps at most one.
		// Returns how many links it replaced. A link the graph has already changes nothing; a link from a node to itself is
		// refused.
		[[nodiscard]] Result<uint32_t> Connect(VisualScriptDocument& document, uint32_t function, VisualScriptLink const& link,
		                                       VisualScriptNodePins const& pins);
		// Removes links of the graph (named as the document stores them).
		[[nodiscard]] Result<void> Disconnect(VisualScriptDocument& document, uint32_t function, std::span<VisualScriptLink const> links);
		// Removes links of the graph, then connects others as Connect does (links moved by a drag), all named as the document
		// stores them. Returns how many links the connections replaced.
		[[nodiscard]] Result<uint32_t> Relink(VisualScriptDocument& document, uint32_t function, std::span<VisualScriptLink const> remove,
		                                      std::span<VisualScriptLink const> add, VisualScriptNodePins const& pins);
		// Adds a Reroute into a link (named as the document stores it): A → B becomes A → Reroute.In and Reroute.Out → B.
		// Returns its ID.
		[[nodiscard]] Result<uint32_t> InsertReroute(VisualScriptDocument& document, uint32_t function, VisualScriptLink const& link,
		                                             glm::vec2 position);

		// Values and settings.

		// Stores the literal of a node's input (named as the document stores it); null removes it. A field of a struct input
		// ("Force.X") takes literals only while the struct is split, and a split struct takes none of its own.
		[[nodiscard]] Result<void> SetInput(VisualScriptDocument& document, uint32_t node, std::string pin, Json literal);
		// Stores a node's setting; null removes it. The settings that rename or remove pins have operations of their own,
		// which also update the links, and are refused here: a Sequence's Count (SetSequenceOutputs), a Switch's Cases
		// (SetSwitchCases), the Parameters of Event.Custom and Event.Send (SetEventParameters) and an Event.Custom's Name
		// (RenameCustomEvent, which renames its senders too).
		[[nodiscard]] Result<void> SetSetting(VisualScriptDocument& document, uint32_t node, std::string name, Json value);
		// Gives a Sequence count outputs (2 to 64) and renumbers its links. Without insertAt or removeAt the outputs from
		// count on go; with insertAt, count minus the current count outputs are inserted at that index (the later links move
		// up: Then1 becomes Then2); with removeAt, the current count minus count outputs are removed from that index (their
		// links go, the later ones move down). Links of pins that were no outputs stay as they are. Returns how many links it
		// removed.
		[[nodiscard]] Result<uint32_t> SetSequenceOutputs(VisualScriptDocument& document, uint32_t node, int count,
		                                                  std::optional<int> insertAt, std::optional<int> removeAt);
		// Sets a Switch's Cases (an array, or null to remove the setting) and updates the links of its case outputs
		// ("Case:" and the case: an integer in decimal, a string as it is). A case kept keeps its links; when the list keeps
		// its length, the cases that changed are renamed in order (the k-th case that went is renamed to the k-th that
		// came), so are their links; the links of every other case that went are removed. Without Cases a Switch on int or
		// string has no cases; one on an enum has every enumerator, whose links stay. Returns how many links it removed.
		[[nodiscard]] Result<uint32_t> SetSwitchCases(VisualScriptDocument& document, uint32_t node, Json cases);
		// Sets the Parameters of an Event.Custom (outputs) or an Event.Send (inputs) and updates their links, and a sender's
		// literals and split pins: a parameter kept keeps them; when the list keeps its length, the parameters whose names
		// changed are renamed in order, with them; those of every other parameter that went are removed. Returns how many
		// links it removed.
		[[nodiscard]] Result<uint32_t> SetEventParameters(VisualScriptDocument& document, uint32_t node,
		                                                  std::vector<VisualScriptParameter> parameters);
		// Renames an Event.Custom node, and every Event.Send of the document that sent the old name. Returns how many senders
		// it renamed.
		[[nodiscard]] Result<uint32_t> RenameCustomEvent(VisualScriptDocument& document, uint32_t node, std::string const& name);
		// The note shown with each node; empty removes it.
		[[nodiscard]] Result<void> SetNodeComment(VisualScriptDocument& document, std::span<uint32_t const> nodes,
		                                          std::string const& comment);
		[[nodiscard]] Result<void> SetNodeCollapsed(VisualScriptDocument& document, std::span<uint32_t const> nodes, bool collapsed);
		[[nodiscard]] Result<void> SetNodeEnabled(VisualScriptDocument& document, std::span<uint32_t const> nodes,
		                                          VisualScriptNodeEnabled enabled);

		// Split struct pins, named by the paths the document stores.

		// Shows a struct pin of a node as one pin per field: an input whose type the library's Make builds, an output whose
		// type Break takes apart, with two fields or more (a field pin of a split pin splits again). The path joins the
		// node's Split list; an input's literal becomes one literal per field (each component as stored; a literal that is
		// not of the struct's form goes), and the pin's links go. Returns how many links it removed.
		[[nodiscard]] Result<uint32_t> SplitPin(VisualScriptDocument& document, uint32_t node, VisualScriptPinDirection direction,
		                                        std::string const& path, VisualScriptNodePins const& pins,
		                                        VisualScriptLibrary const& library);
		// Shows a split pin as one pin again: the path and the paths under it leave the node's Split list, with the links and
		// literals of its fields. When every field of an input had a literal and none was linked, their literals become the
		// pin's literal again (for structs with literals: vectors, quaternions, colours, matrices; the struct is the type the
		// node's library template gives the pin, else the one the library has with exactly the fields shown). Returns how
		// many links it removed.
		[[nodiscard]] Result<uint32_t> RecombinePin(VisualScriptDocument& document, uint32_t node, VisualScriptPinDirection direction,
		                                            std::string const& path, VisualScriptNodePins const& pins,
		                                            VisualScriptLibrary const& library);

		// Comment boxes.

		// Adds a comment box with the text and colour (linear RGBA, each kept within 0 to 1). With elements (nodes and
		// comments of the graph) it frames their bounds, nodes measured by extents, with 32 to spare on each side and 40 more
		// above for its title; without, it is 400 × 200 at the position. Returns its ID.
		[[nodiscard]] Result<uint32_t> AddComment(VisualScriptDocument& document, uint32_t function, std::string text,
		                                          std::span<uint32_t const> elements, VisualScriptNodeExtents const& extents,
		                                          glm::vec2 position, glm::vec4 color);
		[[nodiscard]] Result<void> SetCommentText(VisualScriptDocument& document, uint32_t comment, std::string text);
		// Kept within the format's coordinates and sizes (16 to 1e6).
		[[nodiscard]] Result<void> SetCommentGeometry(VisualScriptDocument& document, uint32_t comment, glm::vec2 position, glm::vec2 size);
		// Linear RGBA, each kept within 0 to 1.
		[[nodiscard]] Result<void> SetCommentColor(VisualScriptDocument& document, uint32_t comment, glm::vec4 color);

		// The document.

		[[nodiscard]] Result<void> SetDescription(VisualScriptDocument& document, std::string description);

		// Variables. Nodes name variables and locals by ID, so renames change no node; the commit records a renamed
		// variable's former name (PrepareVisualScriptCommit).

		// Adds a private variable without a value. Returns its ID.
		[[nodiscard]] Result<uint32_t> AddVariable(VisualScriptDocument& document, std::string name, std::string type);
		// Replaces a variable (its ID stays). A variable whose type changes loses a Value that does not decode as the new type
		// (DecodeVisualScriptLiteral, with enums for enum types). A variable made public, or public with a new type, must have
		// a type components can store. Returns whether that cleared the value.
		[[nodiscard]] Result<bool> SetVariable(VisualScriptDocument& document, uint32_t id, VisualScriptVariable variable,
		                                       VisualScriptEnumLookup const& enums = {});
		// Removes a variable; with removeNodes, also the Variable.Get and Variable.Set nodes of every graph that name it, with
		// their links (no flow is bridged). Returns how many nodes it removed.
		[[nodiscard]] Result<uint32_t> RemoveVariable(VisualScriptDocument& document, uint32_t id, bool removeNodes);
		// Puts the variables in the order of their IDs in order, which lists each variable once.
		[[nodiscard]] Result<void> ReorderVariables(VisualScriptDocument& document, std::span<uint32_t const> order);
		// Adds a copy of a variable after it, with a name of its own and no former names. Returns its ID.
		[[nodiscard]] Result<uint32_t> DuplicateVariable(VisualScriptDocument& document, uint32_t id);

		// Locals: storage of a function's call, used only in that function's graph.

		[[nodiscard]] Result<uint32_t> AddLocal(VisualScriptDocument& document, uint32_t function, std::string name, std::string type);
		// Replaces a local of the function (its ID stays); a type change clears a Value as SetVariable does. Returns whether
		// that cleared the value.
		[[nodiscard]] Result<bool> SetLocal(VisualScriptDocument& document, uint32_t function, uint32_t id, VisualScriptLocal local,
		                                    VisualScriptEnumLookup const& enums = {});
		// Removes a local of the function; with removeNodes, also the Variable.Get and Variable.Set nodes of the function's
		// graph that name it, with their links (no flow is bridged). Returns how many nodes it removed.
		[[nodiscard]] Result<uint32_t> RemoveLocal(VisualScriptDocument& document, uint32_t function, uint32_t id, bool removeNodes);

		// Functions.

		// Adds an impure function without parameters: a Function.Entry at (0, 0) linked from Then to the Execute of a
		// Function.Return at (480, 0). Returns its ID (the entry and the return take the next two).
		[[nodiscard]] Result<uint32_t> AddFunction(VisualScriptDocument& document, std::string name);
		[[nodiscard]] Result<void> RenameFunction(VisualScriptDocument& document, uint32_t function, std::string name);
		// Sets a function's inputs and outputs and updates the pins named after them: the outputs of its Function.Entry and
		// the inputs of its Function.Return nodes, and the inputs and outputs of its Function.Call nodes in every graph, with
		// their links, literals and split pins. A parameter kept keeps them (reordering changes no link); when a list keeps
		// its length, the parameters whose names changed are renamed in order, with them; those of every other parameter
		// that went are removed. Returns how many links it removed.
		[[nodiscard]] Result<uint32_t> SetFunctionSignature(VisualScriptDocument& document, uint32_t function,
		                                                    std::vector<VisualScriptParameter> inputs,
		                                                    std::vector<VisualScriptParameter> outputs);
		// Making a function pure removes the flow links (Execute, Then) of its calls, its entry and its returns, which pure
		// functions do not have. Returns how many links it removed.
		[[nodiscard]] Result<uint32_t> SetFunctionPure(VisualScriptDocument& document, uint32_t function, bool pure);
		// Removes a function with its graph; with removeCalls, also the Function.Call nodes of the other graphs that call it,
		// with their links (no flow is bridged). Returns how many calls it removed.
		[[nodiscard]] Result<uint32_t> RemoveFunction(VisualScriptDocument& document, uint32_t function, bool removeCalls);
		// Adds a copy of a function after it, with a name of its own and new IDs for it, its locals, nodes and comments; its
		// nodes name the copy's locals, and its calls of itself call the copy. Returns its ID.
		[[nodiscard]] Result<uint32_t> DuplicateFunction(VisualScriptDocument& document, uint32_t function);

		// Promotions, of a pin named as the document stores it. An input's value stays what the program reads: its literal,
		// else its default (VisualScriptNodePin::Default), else its type's default. An unlinked input that means the entity
		// running the graph (DefaultsToSelf) is refused, since no value stands for that.

		// Adds a private variable of a data pin's type, named after the pin (made a valid, free name: "Force.X" gives
		// "ForceX"), and a node that uses it, 240 left of an input's anchor or 80 right of an output's (its top 16 above the
		// anchor): for an input, a Variable.Get linked into it (the input's literal moves to the variable's value, else its
		// default is the value); for an output, a Variable.Set fed by it, put after the node's first exec output (linked from
		// it, or into its link). The new node takes the ID after the variable's. Returns the variable's ID.
		[[nodiscard]] Result<uint32_t> PromoteToVariable(VisualScriptDocument& document, uint32_t function,
		                                                 VisualScriptPinReference const& pin, VisualScriptPinDirection direction,
		                                                 VisualScriptNodePins const& pins);
		// Makes an unlinked data input of a node in a function's graph a new input of the function, named after the pin (a
		// free name among the function's inputs), linked from the function's entry. The input's literal moves to the
		// function's calls, else its default becomes their literal, so they pass what it had.
		[[nodiscard]] Result<void> PromoteToFunctionInput(VisualScriptDocument& document, uint32_t function,
		                                                  VisualScriptPinReference const& pin, VisualScriptNodePins const& pins);

		// Align, distribute and straighten. Positions are rounded to whole canvas units (alignment wins over the grid).

		// Moves two or more nodes and comments of the graph so that the edge or centre line is the one the alignment takes
		// from all of them: the leftmost left edge, the rightmost right edge, the top, the bottom, or the centre of their
		// bounds. Nodes are measured by extents, comments by their size.
		[[nodiscard]] Result<void> AlignElements(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> elements,
		                                         VisualScriptAlignment alignment, VisualScriptNodeExtents const& extents);
		// Spaces three or more nodes and comments of the graph along the axis so that the gaps between neighbours are equal:
		// sorted by their left (top) edges, then their IDs, the first and the last stay.
		[[nodiscard]] Result<void> DistributeElements(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> elements,
		                                              VisualScriptDistribution distribution, VisualScriptNodeExtents const& extents);
		// Moves two or more nodes of the graph vertically, in the order of the exec links between them, so that each node
		// whose exec input is linked from another of them has that input level with the output (the first such link in the
		// graph's order). Returns how many nodes moved.
		[[nodiscard]] Result<uint32_t> StraightenExec(VisualScriptDocument& document, uint32_t function, std::span<uint32_t const> nodes,
		                                              VisualScriptNodePins const& pins);
	}
}

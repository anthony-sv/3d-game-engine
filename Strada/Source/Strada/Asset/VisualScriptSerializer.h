#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// A repair the loader made. Path locates it in the content object ("Graph.Nodes[3].ID", "Functions[0].Locals[1].Name",
	// "Variables[2].Range"), with the indices of the document as given; unknown keys outside the content object (file form
	// only) are named from the file's root ("Strada.Comment", "Comment").
	struct VisualScriptIssue
	{
		std::string Path;
		std::string Message;

		bool operator==(VisualScriptIssue const& other) const = default;
	};

	// Reads and writes visual scripts. The file form is
	//   { "Strada": { "Version": 3, "Type": "VisualScript" }, "VisualScript": { <content> } }
	// and the content form is the "VisualScript" object alone (automation documents, the scripting runtime's input).
	//
	// Files of versions 1 and 2 load too. Version 2 added the node keys Split, Comment, Collapsed and Enabled; version 3 the
	// document's Kind and parameters' FormerNames. In a file of an earlier version the keys of later ones are unknown keys
	// (no earlier writer wrote them), and what they would set keeps its default. The content form has no version and has
	// every key of the current one. Saving always writes the current version.
	//
	// Loading checks structure only: never what node types, pins, settings, inputs or values mean (the scripting runtime
	// reports those). A document is refused when its header or its content object is unusable, and when it has an unknown
	// key while the context's UnknownFields policy is Error. Everything else that is damaged is repaired: the element is
	// dropped, or the value is cleared, clamped or given its default. Each repair is reported as an issue and as the
	// context warning "<Path>: <Message>"; what loads always saves and loads again without issues. Callers that must not
	// accept repaired input (automation) load with UnknownFieldPolicy::Warn and refuse any issue.
	//
	// Saving writes the canonical form: keys in a fixed order (Kind first, when it is not Script), optional keys omitted at
	// their defaults, Settings, Inputs and values as stored. Unplaced nodes stay unplaced.
	//
	// The functions keep no state and may be called from any thread.
	class VisualScriptSerializer
	{
	public:
		static constexpr int FormatVersion = 3;

		// IDs are 1 to MaxID, unique across the document.
		static constexpr uint32_t MaxID = 2147483647;
		// Names of variables, locals, functions, parameters and former names: UTF-8 without control characters or '.', no
		// leading or trailing space.
		static constexpr size_t MaxNameBytes = 64;
		// The other strings of the format are UTF-8 without control characters too: the types of variables, locals and
		// parameters, the types of nodes, pin names (of links), member names (of Settings and Inputs) and the pin paths of
		// Split (pin and field names separated by '.', none of them empty).
		static constexpr size_t MaxTypeBytes = 256;
		static constexpr size_t MaxNodeTypeBytes = 1024;
		static constexpr size_t MaxPinNameBytes = 128;
		// Description, tooltips, the texts of comments and nodes, and every string inside a value: UTF-8, lines allowed.
		static constexpr size_t MaxTextBytes = 65536;
		// Values (of variables and locals, and the members of Settings and Inputs) nest at most this deep; a value that is
		// not an array or object is one level.
		static constexpr size_t MaxValueDepth = 16;
		static constexpr size_t MaxSettingsMembers = 256;
		static constexpr size_t MaxNodesPerGraph = 10000;
		static constexpr size_t MaxLinksPerGraph = 40000;
		static constexpr size_t MaxCommentsPerGraph = 2000;
		static constexpr size_t MaxVariables = 1024;
		static constexpr size_t MaxFunctions = 1024;
		static constexpr size_t MaxParameters = 64;
		static constexpr size_t MaxLocals = 256;
		static constexpr size_t MaxFormerNames = 16;
		// Entries of each list (Inputs, Outputs) of a node's Split.
		static constexpr size_t MaxSplitPins = 64;
		// Positions of nodes and comments lie within +-MaxCoordinate; comment sizes within [MinCommentSize, MaxCommentSize].
		static constexpr double MaxCoordinate = 1.0e7;
		static constexpr double MinCommentSize = 16.0;
		static constexpr double MaxCommentSize = 1.0e6;
		// Floats (ranges, and the literals of float and of the components of vectors, quaternions and colors) are finite
		// numbers of at most this magnitude: float's largest value as written with the fewest digits that read back as it
		// (.NET writes float.MaxValue so), a little above its exact value.
		static constexpr double MaxFloatMagnitude = 3.4028235e38;

		// Whether a name has the shape the format requires of the names of variables, locals, functions, parameters and former
		// names: 1 to MaxNameBytes of UTF-8 without control characters or '.', and no leading or trailing space. The loader
		// drops what is named otherwise; it also requires names to be unique in their scope, which this does not check.
		[[nodiscard]] static bool IsValidName(std::string_view name);
		// Whether a text has the shape the format requires of types, node types, pin names and the names of Settings and Inputs
		// members: 1 to maxBytes (MaxTypeBytes, MaxNodeTypeBytes, MaxPinNameBytes) of UTF-8 without control characters.
		// Parsed JSON always holds UTF-8; documents built in memory may not.
		[[nodiscard]] static bool IsValidText(std::string_view text, size_t maxBytes);
		// Whether a text is a pin path as Split lists and links and literals name pins: a pin name ("Hit") or the path of a
		// field pin ("Hit.Point"), IsValidText within MaxPinNameBytes, of '.'-separated parts none of which is empty.
		[[nodiscard]] static bool IsValidPinPath(std::string_view path);
		// Whether a value of a variable, a setting or an input literal saves and loads back as it is: nested at most
		// MaxValueDepth levels deep (the value itself is one), with finite numbers, and strings and member names of at most
		// MaxTextBytes of UTF-8. Parsed JSON always has UTF-8 and finite numbers; values built in memory may not (a NaN or an
		// infinity would be written as null).
		[[nodiscard]] static bool IsValidValue(Json const& value);
		// Whether a text has the shape the format requires of descriptions, tooltips, the texts of comments and nodes, and the
		// strings and member names inside values: at most MaxTextBytes of UTF-8, lines and other control characters allowed.
		[[nodiscard]] static bool IsValidLongText(std::string_view text);
		// Whether an ID is in the format's range, 1 to MaxID. The loader also requires IDs to be unique across the document,
		// which this does not check.
		[[nodiscard]] static bool IsValidID(uint64_t id);
		// Whether a node's or a comment's position is finite and within +-MaxCoordinate in both coordinates.
		[[nodiscard]] static bool IsValidPosition(glm::vec2 position);
		// Whether a comment's size is within [MinCommentSize, MaxCommentSize] in both coordinates.
		[[nodiscard]] static bool IsValidCommentSize(glm::vec2 size);
		// Whether a comment's colour has every component within [0, 1].
		[[nodiscard]] static bool IsValidColor(glm::vec4 color);
		// Whether a variable's slider range is two finite floats, the minimum first.
		[[nodiscard]] static bool IsValidRange(glm::vec2 range);
		// Whether a text is a path Split may list: the rule of IsValidPinPath, which it forwards to. The loader also requires a
		// list to hold each path once, a field's parent before it and at most MaxSplitPins paths, which this does not check.
		[[nodiscard]] static bool IsValidSplitPath(std::string_view path);

		// A float as documents store it: a finite number of magnitude at most MaxFloatMagnitude, as the nearest float.
		// Nothing for anything else.
		[[nodiscard]] static std::optional<float> ReadFloat(Json const& value);

		// The first thing that keeps this document from loading back exactly as it is: an issue whose path has the loader's
		// form ("Graph.Nodes[3].ID") and whose message says what is wrong; nothing when WriteText's text loads back strictly as
		// this document (with no repair, and equal in every value as HaveSameVisualScriptProgram compares them). It applies the
		// loader's rules, in the loader's order, to the model and builds no Json: when the loader would repair the document,
		// the issue is one the loader reports. The commit pipeline's strict check. Any thread.
		[[nodiscard]] static std::optional<VisualScriptIssue> Validate(VisualScriptDocument const& document);

		static Json SerializeDocument(VisualScriptDocument const& document);
		// Repairs are appended to issues (when given) and always reported through the context's warnings.
		[[nodiscard]] static Result<VisualScriptDocument> DeserializeDocument(Json const& content, DeserializationContext const& context,
		                                                                      std::vector<VisualScriptIssue>* issues = nullptr);

		static Json Serialize(VisualScriptDocument const& document);
		[[nodiscard]] static Result<VisualScriptDocument> Deserialize(Json const& file, DeserializationContext const& context,
		                                                              std::vector<VisualScriptIssue>* issues = nullptr);

		// The file form's text: equal, byte for byte, to DumpJson(Serialize(document)) for every document, written without
		// building the Json.
		[[nodiscard]] static std::string WriteText(VisualScriptDocument const& document);
		// The content form, compact: equal to SerializeDocument(document).dump(-1, ' ', false, Json::error_handler_t::replace)
		// for every document, written without building the Json.
		[[nodiscard]] static std::string WriteContentText(VisualScriptDocument const& document);

		[[nodiscard]] static Result<void> SaveToFile(VisualScriptDocument const& document, std::filesystem::path const& path);
		[[nodiscard]] static Result<VisualScriptDocument> LoadFromFile(std::filesystem::path const& path,
		                                                               DeserializationContext const& context,
		                                                               std::vector<VisualScriptIssue>* issues = nullptr);
	};
}

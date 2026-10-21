#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
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
	//   { "Strada": { "Version": 1, "Type": "VisualScript" }, "VisualScript": { <content> } }
	// and the content form is the "VisualScript" object alone (automation documents, the scripting runtime's input).
	//
	// Loading checks structure only: never what node types, pins, settings, inputs or values mean (the scripting runtime
	// reports those). A document is refused when its header or its content object is unusable, and when it has an unknown
	// key while the context's UnknownFields policy is Error. Everything else that is damaged is repaired: the element is
	// dropped, or the value is cleared, clamped or given its default. Each repair is reported as an issue and as the
	// context warning "<Path>: <Message>"; what loads always saves and loads again without issues. Callers that must not
	// accept repaired input (automation) load with UnknownFieldPolicy::Warn and refuse any issue.
	//
	// Saving writes the canonical form: keys in a fixed order, optional keys omitted at their defaults, Settings, Inputs and
	// values as stored. Unplaced nodes stay unplaced.
	//
	// The functions keep no state and may be called from any thread.
	class VisualScriptSerializer
	{
	public:
		static constexpr int FormatVersion = 1;

		// IDs are 1 to MaxID, unique across the document.
		static constexpr uint32_t MaxID = 2147483647;
		// Names of variables, locals, functions, parameters and former names: UTF-8 without control characters or '.', no
		// leading or trailing space.
		static constexpr size_t MaxNameBytes = 64;
		// The other strings of the format are UTF-8 without control characters too: the types of variables, locals and
		// parameters, the types of nodes, and pin names (of links) and member names (of Settings and Inputs).
		static constexpr size_t MaxTypeBytes = 256;
		static constexpr size_t MaxNodeTypeBytes = 1024;
		static constexpr size_t MaxPinNameBytes = 128;
		// Description, tooltips, comment texts and every string inside a value.
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
		// Positions of nodes and comments lie within +-MaxCoordinate; comment sizes within [MinCommentSize, MaxCommentSize].
		static constexpr double MaxCoordinate = 1.0e7;
		static constexpr double MinCommentSize = 16.0;
		static constexpr double MaxCommentSize = 1.0e6;

		static Json SerializeDocument(VisualScriptDocument const& document);
		// Repairs are appended to issues (when given) and always reported through the context's warnings.
		[[nodiscard]] static Result<VisualScriptDocument> DeserializeDocument(Json const& content, DeserializationContext const& context,
		                                                                      std::vector<VisualScriptIssue>* issues = nullptr);

		static Json Serialize(VisualScriptDocument const& document);
		[[nodiscard]] static Result<VisualScriptDocument> Deserialize(Json const& file, DeserializationContext const& context,
		                                                              std::vector<VisualScriptIssue>* issues = nullptr);

		[[nodiscard]] static Result<void> SaveToFile(VisualScriptDocument const& document, std::filesystem::path const& path);
		[[nodiscard]] static Result<VisualScriptDocument> LoadFromFile(std::filesystem::path const& path,
		                                                               DeserializationContext const& context,
		                                                               std::vector<VisualScriptIssue>* issues = nullptr);
	};
}

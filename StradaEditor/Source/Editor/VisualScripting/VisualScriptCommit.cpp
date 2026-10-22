#include "Editor/VisualScripting/VisualScriptCommit.h"

#include "Editor/VisualScripting/VisualScriptNormalization.h"

#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <iterator>
#include <utility>

namespace Strada
{
	namespace
	{
		// Step 4: what is written must load again as it is, since the loaded asset keeps the document itself and a later load
		// of the file must give the same program.
		Result<void> CheckWritable(VisualScriptDocument const& document)
		{
			DeserializationContext context;
			context.UnknownFields = UnknownFieldPolicy::Warn;
			std::vector<VisualScriptIssue> issues;
			Result<VisualScriptDocument> const read =
				VisualScriptSerializer::DeserializeDocument(VisualScriptSerializer::SerializeDocument(document), context, &issues);
			if (!read)
			{
				return MakeError("the edit would write an invalid document: {}", read.GetError());
			}
			if (!issues.empty())
			{
				return MakeError("the edit would write an invalid document: {}: {}", issues.front().Path, issues.front().Message);
			}
			if (!IsSameVisualScriptDocument(read.GetValue(), document))
			{
				return Error{"the edit would write an invalid document: it does not load back as written"};
			}
			return {};
		}
	}

	Result<VisualScriptCommit> PrepareVisualScriptCommit(VisualScriptDocument const& stored, VisualScriptDocument edited,
	                                                     VisualScriptAnalysis const* analysis)
	{
		VisualScriptCommit commit;
		commit.Warnings = RecordFormerNames(edited, stored);
		commit.RenameCount = commit.Warnings.size();
		if (analysis != nullptr)
		{
			std::vector<std::string> updated = ApplyCanonicalNodeTypes(edited, *analysis);
			commit.Warnings.insert(commit.Warnings.end(), std::make_move_iterator(updated.begin()), std::make_move_iterator(updated.end()));
		}
		PlaceUnplacedNodes(edited);
		if (Result<void> checked = CheckWritable(edited); !checked)
		{
			return Error{checked.GetError()};
		}
		commit.Document = std::move(edited);
		return commit;
	}

	Result<VisualScriptCommitRequest> MakeVisualScriptCommitRequest(VisualScriptDocument const& stored, VisualScriptDocument edited,
	                                                                VisualScriptAnalysis const* analysis, std::string_view action,
	                                                                std::string_view scriptName, uint64_t mergeKey)
	{
		Result<VisualScriptCommit> prepared = PrepareVisualScriptCommit(stored, std::move(edited), analysis);
		if (!prepared)
		{
			return Error{prepared.GetError()};
		}
		VisualScriptCommit commit = prepared.TakeValue();
		VisualScriptCommitRequest request;
		request.Document = CreateRef<VisualScriptDocument const>(std::move(commit.Document));
		request.Description = scriptName.empty() ? std::string(action) : fmt::format("{} in {}", action, scriptName);
		request.MergeKey = mergeKey;
		request.Notes.assign(std::make_move_iterator(commit.Warnings.begin() + static_cast<std::ptrdiff_t>(commit.RenameCount)),
		                     std::make_move_iterator(commit.Warnings.end()));
		return request;
	}

	bool IsSameVisualScriptDocument(VisualScriptDocument const& a, VisualScriptDocument const& b)
	{
		return &a == &b || (a == b && HaveSameVisualScriptProgram(a, b));
	}
}

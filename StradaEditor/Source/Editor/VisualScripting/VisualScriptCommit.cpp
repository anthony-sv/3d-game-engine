#include "Editor/VisualScripting/VisualScriptCommit.h"

#include "Editor/VisualScripting/VisualScriptNormalization.h"

#include "Strada/Asset/VisualScriptLayout.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Timer.h"

#include <iterator>
#include <optional>
#include <utility>

namespace Strada
{
	namespace
	{
		// Step 4: what is written must load again as it is, since the loaded asset keeps the document itself and a later load
		// of the file must give the same program. Validate applies the loader's rules to the document itself, so no Json is
		// built and read back; the text of the file is then written directly.
		Result<std::string> MakeWritableText(VisualScriptDocument const& document, VisualScriptCommitTimings& timings)
		{
			Timer phase;
			std::optional<VisualScriptIssue> const issue = VisualScriptSerializer::Validate(document);
			timings.Check = phase.GetElapsedMilliseconds();
			if (issue)
			{
				return MakeError("the edit would write an invalid document: {}: {}", issue->Path, issue->Message);
			}
			phase.Reset();
			std::string written = VisualScriptSerializer::WriteText(document);
			timings.Text = phase.GetElapsedMilliseconds();
			return written;
		}
	}

	Result<VisualScriptCommit> PrepareVisualScriptCommit(VisualScriptDocument const& stored, VisualScriptDocument edited,
	                                                     VisualScriptAnalysis const* analysis)
	{
		Timer const normalize;
		VisualScriptCommit commit;
		commit.Warnings = RecordFormerNames(edited, stored);
		commit.RenameCount = commit.Warnings.size();
		if (analysis != nullptr)
		{
			std::vector<std::string> updated = ApplyCanonicalNodeTypes(edited, *analysis, &stored);
			commit.Warnings.insert(commit.Warnings.end(), std::make_move_iterator(updated.begin()), std::make_move_iterator(updated.end()));
		}
		PlaceUnplacedNodes(edited);
		commit.Timings.Normalize = normalize.GetElapsedMilliseconds();
		Result<std::string> text = MakeWritableText(edited, commit.Timings);
		if (!text)
		{
			return Error{text.GetError()};
		}
		commit.Text = text.TakeValue();
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
		request.Text = std::move(commit.Text);
		request.Description = scriptName.empty() ? std::string(action) : fmt::format("{} in {}", action, scriptName);
		request.MergeKey = mergeKey;
		request.Notes.assign(std::make_move_iterator(commit.Warnings.begin() + static_cast<std::ptrdiff_t>(commit.RenameCount)),
		                     std::make_move_iterator(commit.Warnings.end()));
		request.Timings = commit.Timings;
		return request;
	}

	bool IsSameVisualScriptDocument(VisualScriptDocument const& a, VisualScriptDocument const& b)
	{
		return &a == &b || (a == b && HaveSameVisualScriptProgram(a, b));
	}
}

#include "Fuzzing.h"

#include "Strada/Asset/VisualScriptSerializer.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	// One round of a visual-script fuzz case: loads a damaged file (file form) the way the asset manager does, with every
	// asset reference accepted. Nothing when the file is refused; otherwise the document, once it is known to save and
	// load again unchanged and without issues. Every case of this file loads its rounds here.
	std::optional<VisualScriptDocument> LoadDamagedFile(Json const& file)
	{
		std::vector<std::string> warnings;
		DeserializationContext const context = Testing::MakeTolerantContext(warnings);
		Result<VisualScriptDocument> document = VisualScriptSerializer::Deserialize(file, context);
		if (!document)
		{
			return std::nullopt;
		}

		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> reloaded =
			VisualScriptSerializer::Deserialize(VisualScriptSerializer::Serialize(document.GetValue()), context, &issues);
		REQUIRE_MESSAGE(reloaded.IsOk(), reloaded.GetError());
		CHECK_MESSAGE(issues.empty(), (issues.empty() ? std::string() : issues.front().Path + ": " + issues.front().Message));
		CHECK(reloaded.GetValue() == document.GetValue());
		return document.TakeValue();
	}
}

TEST_CASE("VisualScript: damaged files are refused or load and save again")
{
	Json const original = Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph");
	std::vector<std::string> warnings;
	std::vector<VisualScriptIssue> issues;
	REQUIRE(VisualScriptSerializer::Deserialize(original, Testing::MakeTolerantContext(warnings), &issues).IsOk());
	REQUIRE(issues.empty());

	Testing::DocumentMutator mutator(10000);
	int loaded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		if (LoadDamagedFile(mutator.Damage(original, round)))
		{
			loaded++;
		}
		else
		{
			refused++;
		}
	}
	CHECK(loaded > 0);
	CHECK(refused > 0);
}

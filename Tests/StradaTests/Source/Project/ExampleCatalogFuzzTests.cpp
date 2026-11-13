#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Project/ExampleCatalog.h"
#include "Strada/Project/ProjectCopy.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

using namespace Strada;

TEST_CASE("ExampleCatalog: damaged catalogs are refused or lose their damaged entries, and what is available copies")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const examples = temporary.GetPath() / "Examples";
	REQUIRE(FileSystem::CopyDirectory(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR "/FrontDoor/Content/Examples"), examples).IsOk());
	Result<std::string> text = FileSystem::ReadTextFile(examples / "catalog.json");
	REQUIRE(text.IsOk());
	Result<Json> parsed = ParseJson(text.GetValue());
	REQUIRE(parsed.IsOk());
	Json original = parsed.TakeValue();
	// Members a later version might add, which damage can reach without breaking the catalog.
	original["Featured"] = Json::object({{"Id", "tiny"}, {"Until", "2027-01-01"}});
	original["Examples"][0]["Video"] = Json::object({{"Path", "Videos/tiny.mp4"}});

	Testing::DocumentMutator mutator(6200);
	Testing::FuzzRandom random(6201);
	int loaded = 0;
	int refused = 0;
	int copied = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(200); round < rounds; round++)
	{
		CAPTURE(round);
		Json const document = mutator.Damage(original, round);
		std::string damaged = DumpJson(document);
		if (round % 7 == 0)
		{
			damaged.resize(random.Pick(damaged.size() + 1));
		}
		else if (round % 7 == 1)
		{
			damaged[random.Pick(damaged.size())] = static_cast<char>(random.Pick(256));
		}
		REQUIRE(FileSystem::WriteTextFile(examples / "catalog.json", damaged).IsOk());

		Result<ExampleCatalog> catalog = ExampleCatalog::Load(examples / "catalog.json");
		if (!catalog)
		{
			CHECK_FALSE(catalog.GetError().empty());
			refused++;
			continue;
		}
		loaded++;
		// Every entry of a catalog damaged as a document is an example or a warning.
		if (round % 7 > 1)
		{
			CHECK(catalog.GetValue().GetExamples().size() + catalog.GetValue().GetWarnings().size() == document["Examples"].size());
		}
		for (ExampleEntry const& example : catalog.GetValue().GetExamples())
		{
			CAPTURE(example.Id);
			CHECK(FileSystem::IsInside(catalog.GetValue().GetDirectory(example), examples));
			CHECK(FileSystem::IsInside(catalog.GetValue().GetProjectFile(example), examples));
			std::filesystem::path const thumbnail = catalog.GetValue().GetThumbnailPath(example);
			CHECK((thumbnail.empty() || FileSystem::IsInside(thumbnail, examples)));
			if (!catalog.GetValue().IsAvailable(example))
			{
				continue;
			}
			// What is available copies into a new directory, inside it.
			std::filesystem::path const target = temporary.GetPath() / "Copies" / std::to_string(round);
			Result<ProjectCopyResult> copy = ProjectCopy::Copy(catalog.GetValue().GetDirectory(example), target);
			REQUIRE_MESSAGE(copy.IsOk(), (copy ? std::string() : copy.GetError()));
			CHECK(FileSystem::IsRegularFile(target / FileSystem::PathFromUtf8(example.ProjectFile)));
			REQUIRE(FileSystem::RemoveAll(target).IsOk());
			copied++;
		}
	}
	CHECK(loaded > 0);
	CHECK(refused > 0);
	CHECK(copied > 0);
}

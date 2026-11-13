#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <string>

using namespace Strada;

TEST_CASE("EngineLayout: damaged markers are ignored or name a checkout, and the layout is always usable")
{
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const bin = temporary.GetPath() / "bin";
	std::filesystem::path const checkout = temporary.GetPath() / "Checkout";
	REQUIRE(FileSystem::CreateDirectories(bin).IsOk());
	REQUIRE(FileSystem::CreateDirectories(checkout).IsOk());
	std::filesystem::path const marker = bin / EngineLayout::ManifestFileName;

	// The marker CMake writes, with members a later version might add, which damage can reach without breaking it.
	Json original = Json::object();
	original["Strada"] = MakeFileHeader(EngineLayout::ManifestType, EngineLayout::ManifestVersion);
	original["Content"] = FileSystem::PathToUtf8(checkout);
	original["Generator"] = Json::object({{"Name", "CMake"}, {"Options", Json::array({"Debug", 1, true})}});

	constexpr std::array<EngineContent, 5> AllContent = {EngineContent::Templates, EngineContent::Examples, EngineContent::Docs,
	                                                     EngineContent::AgentKit, EngineContent::MakeGameSkill};
	Testing::DocumentMutator mutator(5200);
	Testing::FuzzRandom random(5201);
	int sourceTrees = 0;
	int ignored = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		std::string text = DumpJson(mutator.Damage(original, round));
		// Some markers are damaged as text: cut short, or with a byte changed.
		if (round % 5 == 0)
		{
			text.resize(random.Pick(text.size() + 1));
		}
		else if (round % 5 == 1)
		{
			text[random.Pick(text.size())] = static_cast<char>(random.Pick(256));
		}
		REQUIRE(FileSystem::WriteTextFile(marker, text).IsOk());

		Result<EngineLayout> resolved = EngineLayout::Resolve(bin / "StradaEditor");
		REQUIRE(resolved.IsOk());
		EngineLayout const& layout = resolved.GetValue();
		CHECK(layout.GetWarnings().size() <= 1);
		CHECK(layout.GetResourcesDirectory().lexically_normal() == bin.lexically_normal());
		std::filesystem::path const root = layout.IsSourceTree() ? checkout : bin;
		if (layout.IsSourceTree())
		{
			CHECK(layout.GetWarnings().empty());
			CHECK(FileSystem::IsDirectory(layout.GetSourceDirectory()));
			sourceTrees++;
		}
		else
		{
			ignored++;
		}
		for (EngineContent const content : AllContent)
		{
			CAPTURE(static_cast<int>(content));
			CHECK(FileSystem::IsInside(layout.GetContentPath(content), root));
		}
		CHECK(FileSystem::IsInside(layout.GetExampleCatalogPath(), root));
		CHECK(FileSystem::IsInside(layout.GetToolPath(), bin));
	}
	CHECK(sourceTrees > 0);
	CHECK(ignored > 0);
}

#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectTemplate.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

using namespace Strada;

TEST_CASE("ProjectTemplate: damaged template files are refused, and what loads instantiates a project that opens")
{
	Testing::AssetManagerScope assets;
	Testing::TemporaryDirectory temporary;
	std::filesystem::path const fixtures = FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR "/FrontDoor/Content/Templates");
	// Both kinds of template, each in a folder of its id, with members a later version might add.
	std::filesystem::path const projectTemplate = temporary.GetPath() / "Templates" / "tiny-project";
	std::filesystem::path const sceneTemplate = temporary.GetPath() / "Templates" / "empty-3d";
	REQUIRE(FileSystem::CopyDirectory(fixtures / "tiny-project", projectTemplate).IsOk());
	REQUIRE(FileSystem::CopyDirectory(fixtures / "empty-3d", sceneTemplate).IsOk());
	auto const readOriginal = [](std::filesystem::path const& folder)
	{
		Result<std::string> text = FileSystem::ReadTextFile(folder / "template.json");
		REQUIRE(text.IsOk());
		Result<Json> document = ParseJson(text.GetValue());
		REQUIRE(document.IsOk());
		Json original = document.TakeValue();
		original["Template"]["Later"] = Json::object({{"Kind", "Arcade"}, {"Players", Json::array({1, 2})}});
		return original;
	};
	Json const originals[2] = {readOriginal(projectTemplate), readOriginal(sceneTemplate)};
	std::filesystem::path const folders[2] = {projectTemplate, sceneTemplate};

	Scene startScene("Main");
	startScene.CreateEntity("Player");
	Testing::DocumentMutator mutator(6100);
	Testing::FuzzRandom random(6101);
	int loadedCount = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(200); round < rounds; round++)
	{
		CAPTURE(round);
		size_t const kind = static_cast<size_t>(round % 2);
		std::string text = DumpJson(mutator.Damage(originals[kind], round));
		if (round % 7 == 0)
		{
			text.resize(random.Pick(text.size() + 1));
		}
		else if (round % 7 == 1)
		{
			text[random.Pick(text.size())] = static_cast<char>(random.Pick(256));
		}
		REQUIRE(FileSystem::WriteTextFile(folders[kind] / "template.json", text).IsOk());

		Result<ProjectTemplate> loaded = ProjectTemplate::Load(folders[kind]);
		if (!loaded)
		{
			CHECK_FALSE(loaded.GetError().empty());
			refused++;
			continue;
		}
		loadedCount++;
		ProjectTemplate const& value = loaded.GetValue();
		CHECK(value.Id == (kind == 0 ? "tiny-project" : "empty-3d"));
		(void)value.GetThumbnailPath();

		// What loads instantiates, and the project opens with its start scene.
		std::filesystem::path const target = temporary.GetPath() / "Projects" / std::to_string(round);
		Result<std::filesystem::path> created = ProjectTemplate::Instantiate(value, target, "Fuzz Game", TemplateOptions{}, &startScene);
		REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
		CHECK(FileSystem::IsInside(created.GetValue(), target));
		CHECK_FALSE(FileSystem::Exists(target / "template.json"));
		Result<Ref<Project>> opened = Project::Open(created.GetValue());
		REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
		CHECK(opened.GetValue()->GetSettings().Name == "Fuzz Game");
		CHECK(opened.GetValue()->GetSettings().StartScene.IsValid());
		AssetManager::CloseAssetDirectory();
		REQUIRE(FileSystem::RemoveAll(target).IsOk());
	}
	CHECK(loadedCount > 0);
	CHECK(refused > 0);

	// Listing the damaged folder never fails: a damaged template is left out with a warning.
	std::vector<std::string> warnings;
	std::vector<ProjectTemplate> const listed = ProjectTemplate::List(temporary.GetPath() / "Templates", warnings);
	CHECK(listed.size() + warnings.size() == 2);
}

#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <set>
#include <string>
#include <vector>

using namespace Strada;

// Projects/FeatureTest exercises every component, and its scripts the whole scripting API (ScriptCoreTests checks that
// they use all of it, StradaRuntimeTests runs them). New components must appear in its scenes.
TEST_CASE("Project: the feature-test project opens and its scenes use every component")
{
	// A copy: opening a project saves its asset registry.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const source = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR);
	REQUIRE(FileSystem::CopyDirectory(source / "Assets", directory.GetPath() / "Assets").IsOk());
	REQUIRE(FileSystem::Copy(source / "FeatureTest.sproj", directory.GetPath() / "FeatureTest.sproj", false).IsOk());

	Testing::AssetManagerScope assets;
	std::vector<std::string> warnings;
	Result<Ref<Project>> project = Project::Open(directory.GetPath() / "FeatureTest.sproj", &warnings);
	REQUIRE_MESSAGE(project.IsOk(), (project ? std::string() : project.GetError()));
	CHECK(warnings.empty());
	CHECK(AssetManager::GetAssetType(project.GetValue()->GetSettings().StartScene) == AssetType::Scene);

	std::set<std::string> used;
	std::vector<AssetMetadata> const scenes = AssetManager::GetAssets(AssetType::Scene);
	CHECK(scenes.size() == 2);
	for (AssetMetadata const& scene : scenes)
	{
		CAPTURE(scene.Path);
		// Fields this version does not know would mean that the scenes and the engine drifted apart.
		Result<Ref<Scene>> loaded = SceneSerializer::LoadFromFile(AssetManager::GetAbsolutePath(scene.Handle),
		                                                          AssetManager::CreateDeserializationContext(UnknownFieldPolicy::Error));
		REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
		Json const document = SceneSerializer::Serialize(*loaded.GetValue());
		for (Json const& entity : document.at("Entities"))
		{
			for (auto const& component : entity.at("Components").items())
			{
				used.insert(component.key());
			}
		}
	}
	for (ComponentInfo const& component : ComponentRegistry::GetComponents())
	{
		if (!component.IsInternal())
		{
			CAPTURE(component.Name);
			CHECK(used.contains(std::string(component.Name)));
		}
	}
}

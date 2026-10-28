#include "Fuzzing.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>
#include <utility>
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
	// The FeatureTest format sample (every key of a script) and the library fixture (Kind, and parameters' former names).
	Result<std::string> const libraryText =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Library.sgraph");
	REQUIRE(libraryText.IsOk());
	Result<Json> const library = ParseJson(libraryText.GetValue());
	REQUIRE(library.IsOk());
	std::array<Json, 2> const seeds = {Testing::LoadFeatureTestDocument("Assets/Scripts/FormatSample.sgraph"), library.GetValue()};
	std::vector<std::string> warnings;
	for (Json const& seed : seeds)
	{
		std::vector<VisualScriptIssue> issues;
		REQUIRE(VisualScriptSerializer::Deserialize(seed, Testing::MakeTolerantContext(warnings), &issues).IsOk());
		REQUIRE(issues.empty());
	}

	// Each seed is damaged as a file of version 3, 2 and 1 in turn, where the keys of later versions it holds are unknown
	// keys: the sample's node keys of version 2 in a file of version 1, the library's Kind and former names in both.
	auto const withVersion = [](Json file, int version)
	{
		file["Strada"]["Version"] = version;
		return file;
	};
	for (auto const& [seed, version] : {std::pair<size_t, int>(0, 1), std::pair<size_t, int>(1, 2), std::pair<size_t, int>(1, 1)})
	{
		std::vector<VisualScriptIssue> issues;
		REQUIRE(
			VisualScriptSerializer::Deserialize(withVersion(seeds[seed], version), Testing::MakeTolerantContext(warnings), &issues).IsOk());
		CHECK_FALSE(issues.empty());
	}

	Testing::DocumentMutator mutator(10000);
	int loaded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		int const version = VisualScriptSerializer::FormatVersion - (round / 2) % 3;
		if (LoadDamagedFile(mutator.Damage(withVersion(seeds[static_cast<size_t>(round % 2)], version), round)))
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

TEST_CASE("VisualScript: damaged graphs that load also run")
{
	Json const features = Testing::LoadFeatureTestDocument("Assets/Scripts/Features.sgraph");
	// The editor's fixture document in a file of version 3: disabled, development-only and split nodes.
	Result<std::string> const text =
		FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Documents" / "Editor.json");
	REQUIRE(text.IsOk());
	Result<Json> const content = ParseJson(text.GetValue());
	REQUIRE(content.IsOk());
	std::vector<std::string> warnings;
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> editor =
		VisualScriptSerializer::DeserializeDocument(content.GetValue(), Testing::MakeTolerantContext(warnings), &issues);
	REQUIRE_MESSAGE(editor.IsOk(), (editor ? std::string() : editor.GetError()));
	REQUIRE(issues.empty());
	Json const seeds[] = {features, VisualScriptSerializer::Serialize(editor.GetValue())};
	REQUIRE(seeds[1]["Strada"]["Version"] == 3);
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;

	// Each document that loads runs on an entity of a scene that starts, updates and stops: whatever the runtime makes of
	// it, including failures, nothing crashes or hangs. The seeds take turns.
	Testing::DocumentMutator mutator(10100);
	std::array<int, 2> ran{};
	for (int round = 0, rounds = Testing::GetFuzzRounds(100); round < rounds; round++)
	{
		CAPTURE(round);
		size_t const seed = static_cast<size_t>(round % 2);
		std::optional<VisualScriptDocument> document = LoadDamagedFile(mutator.Damage(seeds[seed], round));
		if (!document)
		{
			continue;
		}
		AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(*document)), "Damaged");
		{
			Scene scene("Fuzz");
			Entity entity = scene.CreateEntity("Graph");
			entity.AddComponent<VisualScriptComponent>().Graph = graph;
			scene.OnRuntimeStart();
			if (VisualScriptEngine::HasInstance(entity.GetUUID()))
			{
				ran[seed]++;
			}
			for (int frame = 0; frame < 5; frame++)
			{
				scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
			}
			scene.OnRuntimeStop();
		}
		AssetManager::RemoveMemoryAsset(graph);
	}
	CHECK(ran[0] > 0);
	CHECK(ran[1] > 0);
}
